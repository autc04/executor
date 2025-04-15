#include "localvolume.h"
#include <base/common.h>
#include <FileMgr.h>
#include <MemoryMgr.h>
#include <file/file.h>
#include <hfs/hfs.h>
#include "item.h"

#include "plain.h"
#include "basilisk.h"
#include "mac.h"
#include "appledouble.h"

#include "simplecnidmapper.h"
#include "lmdbcnidmapper.h"
#include "itemcache.h"

#include <algorithm>
#include <map>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX 1
#include <windows.h>
#endif

using namespace Executor;


ItemPtr ExtensionItemFactory::createItemForDirEntry(ItemCache& itemcache, CNID parID, CNID cnid,
    const fs::directory_entry& e, mac_string_view macname)
{
    if(fs::is_regular_file(e.path()))
    {
        return std::make_shared<PlainFileItem>(itemcache, parID, cnid, e.path(), macname);
    }
    return nullptr;
}

void ExtensionItemFactory::createFile(const fs::path& newPath)
{
    PlainDataFork data(newPath, PlainDataFork::create);
}

LocalVolume::LocalVolume(VCB& vcb, fs::path root)
    : Volume(vcb), root(root)
{
    itemCache = std::make_unique<ItemCache>(
            root,
            getVolumeName(),
            std::make_unique<SimpleCNIDMapper>(root, getVolumeName()),
            //std::make_unique<LMDBCNIDMapper>(root, getVolumeName()),
            static_cast<ItemFactory*>(this)
        );

    itemFactories.push_back(std::make_unique<DirectoryItemFactory>());
    itemFactories.push_back(std::make_unique<AppleSingleItemFactory>());
    itemFactories.push_back(std::make_unique<AppleDoubleItemFactory>());
    upgradedItemFactory = itemFactories.back().get();
    itemFactories.push_back(std::make_unique<BasiliskItemFactory>());
#ifdef __APPLE__
    itemFactories.push_back(std::make_unique<MacItemFactory>());
    upgradedItemFactory = itemFactories.back().get();
#endif
    itemFactories.push_back(std::make_unique<ExtensionItemFactory>());
    defaultItemFactory = itemFactories.back().get();
}

bool LocalVolume::isHidden(const fs::directory_entry& e)
{
    for(auto& itemFactory : itemFactories)
    {
        if(itemFactory->isHidden(e))
            return true;
    }
    return false;
}

ItemPtr LocalVolume::createItemForDirEntry(ItemCache& itemcache, CNID parID, CNID cnid,
    const fs::directory_entry& e, mac_string_view macname)
{
    for(auto& itemFactory : itemFactories)
    {
        if(ItemPtr item = itemFactory->createItemForDirEntry(itemcache, parID, cnid, e, macname))
            return item;
    }
    return {};
}

std::shared_ptr<DirectoryItem> LocalVolume::resolveDir(long dirID)
{
    if(auto dirItem = std::dynamic_pointer_cast<DirectoryItem>(itemCache->tryResolve(dirID)))
        return dirItem;
    else
        throw OSErrorException(dirNFErr); // not a directory
}

std::shared_ptr<DirectoryItem> LocalVolume::resolveDir(short vRef, long dirID)
{
    if(dirID)
        return resolveDir(dirID);
    else if(ISWDNUM(vRef))
    {
        auto wdp = WDNUMTOWDP(vRef);
        return resolveDir(wdp->dirid);
    }
    else if(vRef == 0)
    {
        return resolveDir(DefDirID);
    }
    else
    {
        // "poor man's search path": not implemented
        return resolveDir(2);
    }
}

ItemPtr LocalVolume::resolve(mac_string_view name, short vRef, long dirID)
{
    if(name.empty())
        return resolveDir(vRef, dirID);

    size_t colon = name.find(':');

        // special case: directory ID 1 is a funny thing...
    if(dirID == 1 && (colon == 0 || colon == mac_string_view::npos))
    {
        std::shared_ptr<DirectoryItem> dir = resolveDir(vRef, 2);

        size_t afterColon = colon == 0 ? 1 : 0;
        size_t rest = colon == 0 ? name.find(':', 1) : mac_string_view::npos;

        if(name.substr(afterColon, rest-afterColon) != getVolumeName())
        {
            if(rest != mac_string_view::npos)
                throw OSErrorException(dirNFErr);
            else
                throw OSErrorException(fnfErr);
        }

        if(rest == mac_string_view::npos)
            return dir;
        else
            return resolveRelative(dir, name.substr(rest));
    }
    else if(colon != mac_string_view::npos)
    {
        std::shared_ptr<DirectoryItem> dir = resolveDir(vRef, colon == 0 ? dirID : 2);
        return resolveRelative(dir, name.substr(colon));
    }
    else
    {
        std::shared_ptr<DirectoryItem> dir = resolveDir(vRef, dirID);
        itemCache->cacheDirectory(dir);
        if(auto file = dir->tryResolve(name))
            return file;
        else
            throw OSErrorException(fnfErr);
    }
}

ItemPtr LocalVolume::resolveRelative(const std::shared_ptr<DirectoryItem>& base, mac_string_view name)
{
    auto p = name.begin();

    if(p == name.end())
        return base;

    ++p;

    if(p == name.end())
        return base;

    auto colon = std::find(p, name.end(), ':');

    ItemPtr item;
    
    if(colon == p)
    {
        item = resolveDir(base->parID());
    }
    else
    {
        itemCache->cacheDirectory(base);
        item = base->tryResolve(mac_string_view(p, colon));
        if(!item)
        {
            if(colon == name.end())
                throw OSErrorException(fnfErr);
            else
                throw OSErrorException(dirNFErr);
        }
    }

    if(colon == name.end())
        return item;
    else if(auto dir = std::dynamic_pointer_cast<DirectoryItem>(item))
    {
        return resolveRelative(dir, mac_string_view(colon, name.end()));
    }
    else
        throw OSErrorException(fnfErr);
}

ItemPtr LocalVolume::resolveForInfo(mac_string_view name, short vRef, long dirID, short index, bool includeDirectories)
{
    if(index > 0)
    {
        auto dir = resolveDir(vRef, dirID);
        itemCache->cacheDirectory(dir);
        return dir->resolve(index, includeDirectories);
    }
    else if(index == 0 && !name.empty())
        return resolve(name, vRef, dirID);
    else
        return resolveDir(vRef, dirID);
}

mac_string LocalVolume::getVolumeName() const
{
    return mac_string(mac_string_view(vcb.vcbVN));
}


struct LocalVolume::FCBExtension
{
    filecontrolblock *fcb;
    short refNum;
    std::unique_ptr<OpenFile> access;
    FileItemPtr file;

    FCBExtension() = default;
    FCBExtension(filecontrolblock *fcb, short refNum)
        : fcb(fcb), refNum(refNum)
    {}
};

LocalVolume::FCBExtension& LocalVolume::getFCBX(short refNum)
{
    int index = (refNum - 2) / 94;
    if(refNum < 0 || refNum % 94 != 2 || index >= fcbExtensions.size())
        throw OSErrorException(paramErr);
    return fcbExtensions[index];
}

LocalVolume::FCBExtension& LocalVolume::openFCBX()
{
    filecontrolblock* fcb = ROMlib_getfreefcbp();
    short refNum = (char *)fcb - (char *)LM(FCBSPtr);
    int index = (refNum - 2) / 94;

    memset(fcb, 0, sizeof(*fcb));
    fcb->fcbVPtr = &vcb;

    if(fcbExtensions.size() < index + 1)
        fcbExtensions.resize(index + 1);
    fcbExtensions[index] = FCBExtension(fcb, refNum);

    return fcbExtensions[index];
}

FileItemPtr LocalVolume::upgradeItem(FileItemPtr item, ItemFactory *betterFactory)
{
    CNID cnid = item->cnid();

    for(int i = 0; i < NFCB; i++)
    {
        if(ROMlib_fcblocks[i].fdfnum == cnid
           && ROMlib_fcblocks[i].fcvptr == &vcb)
        {
            FCBExtension& fcbx = fcbExtensions[i];
            assert(fcbx.refNum == i * 94 + 2);
            fcbx.file = {};
            fcbx.access = {};
        }
    }

    fs::path path = item->path();
    fs::path tempPath = path.parent_path() / fs::unique_path();

    itemCache->flushItem(item);
    item->moveItem(tempPath, mac_string_view());
    betterFactory->createFile(path);
    auto newItem = std::dynamic_pointer_cast<FileItem>(itemCache->tryResolve(cnid));

    assert(newItem->cnid() == cnid);

    auto copydata = [](const std::unique_ptr<OpenFile>& from, const std::unique_ptr<OpenFile>& to) {
        size_t sz = from->getEOF();
        to->setEOF(sz);

        const size_t bufSize = 1024 * 1024;
        std::unique_ptr<uint8_t[]> buffer(new uint8_t[bufSize]);
        for(size_t offset = 0; offset < sz; offset += bufSize)
        {
            size_t sz1 = std::min(bufSize, sz - offset);
            from->read(offset, buffer.get(), sz1);
            to->write(offset, buffer.get(), sz1);
        }
    };

    try
    {
        copydata(item->open(fsRdPerm), newItem->open(fsRdWrPerm));
        copydata(item->openRF(fsRdPerm), newItem->openRF(fsRdWrPerm));
    }
    catch(const std::exception& e)
    {
        std::cout << e.what() << std::endl;
    }
    newItem->setInfo(item->getInfo());
    item->deleteItem();


    newItem->resWriteAccessRefNum = item->resWriteAccessRefNum;
    newItem->dataWriteAccessRefNum = item->dataWriteAccessRefNum;

    for(int i = 0; i < NFCB; i++)
    {
        if(ROMlib_fcblocks[i].fdfnum == cnid
           && ROMlib_fcblocks[i].fcvptr == &vcb)
        {
            FCBExtension& fcbx = fcbExtensions[i];
            assert(fcbx.refNum == i * 94 + 2);
            fcbx.file = newItem;

            int8_t perm = (fcbx.fcb->fcbMdRByt & WRITEBIT) ? fsRdWrPerm : fsRdPerm;

            if(fcbx.fcb->fcbMdRByt & RESOURCEBIT)
                fcbx.access = newItem->openRF(perm);
            else
                fcbx.access = newItem->open(perm);
        }
    }

    return newItem;
}

void LocalVolume::openCommon(GUEST<short>& refNum, ItemPtr item, Fork fork, int8_t permission)
{
    if(auto fileItem = std::dynamic_pointer_cast<FileItem>(item))
    {
        short& writeAccessRef = fork == Fork::resource ? fileItem->resWriteAccessRefNum : fileItem->dataWriteAccessRefNum;

        if(permission != fsRdPerm)
        {
            if(writeAccessRef > 0)
            {
                refNum = writeAccessRef;
                throw OSErrorException(opWrErr);
            }
        }

        auto access = fork == Fork::resource ? fileItem->openRF(permission) : fileItem->open(permission);
        FCBExtension& fcbx = openFCBX();
        fcbx.access = std::move(access);
        fcbx.fcb->fcbFlNum = fileItem->cnid();
        fcbx.fcb->fcbMdRByt = 0;
        if(fork == Fork::resource)
            fcbx.fcb->fcbMdRByt |= RESOURCEBIT;
        if(fcbx.access->canWrite())
            fcbx.fcb->fcbMdRByt |= WRITEBIT;
        fcbx.file = fileItem;
        refNum = fcbx.refNum;
        fcbx.fcb->fcbDirID = item->parID();

        if(permission != fsRdPerm)
        {
            writeAccessRef = fcbx.refNum;
        }
    }
    else
        throw OSErrorException(paramErr);    // TODO: what's the correct error?
}

void LocalVolume::PBHOpenDF(HParmBlkPtr pb)
{
    openCommon(pb->ioParam.ioRefNum, resolve(pb->ioParam.ioNamePtr, pb->ioParam.ioVRefNum, pb->fileParam.ioDirID), Fork::data, pb->ioParam.ioPermssn);
}
void LocalVolume::PBHOpenRF(HParmBlkPtr pb)
{
    openCommon(pb->ioParam.ioRefNum, resolve(pb->ioParam.ioNamePtr, pb->ioParam.ioVRefNum, pb->fileParam.ioDirID), Fork::resource, pb->ioParam.ioPermssn);
}

void LocalVolume::PBOpenDF(ParmBlkPtr pb)
{
    openCommon(pb->ioParam.ioRefNum, resolve(pb->ioParam.ioNamePtr, pb->ioParam.ioVRefNum, 0), Fork::data, pb->ioParam.ioPermssn);
}
void LocalVolume::PBOpenRF(ParmBlkPtr pb)
{
    openCommon(pb->ioParam.ioRefNum, resolve(pb->ioParam.ioNamePtr, pb->ioParam.ioVRefNum, 0), Fork::resource, pb->ioParam.ioPermssn);
}

void LocalVolume::getInfoCommon(CInfoPBPtr pb, InfoKind infoKind)
{
    ItemPtr item = resolveForInfo(pb->hFileInfo.ioNamePtr,
        pb->hFileInfo.ioVRefNum, infoKind == InfoKind::FInfo ? 0 : pb->hFileInfo.ioDirID.get(), pb->hFileInfo.ioFDirIndex,
        infoKind == InfoKind::CatInfo);

    if(pb->hFileInfo.ioFDirIndex != 0)
    {
        if(StringPtr outputName = pb->hFileInfo.ioNamePtr)
        {
            const mac_string name = item->name();
            size_t n = std::min(name.size(), (size_t)255);
            memcpy(outputName+1, name.data(), n);
            outputName[0] = n;
        }
    }

    pb->hFileInfo.ioDirID = item->cnid();   // a.k.a. pb->dirInfo.ioDrDirID

    if(infoKind == InfoKind::CatInfo)
    {
        pb->hFileInfo.ioFlParID = item->parID();    // a.k.a. pb->dirInfo.ioDrParID
    }

    if(DirectoryItemPtr dirItem = std::dynamic_pointer_cast<DirectoryItem>(item))
    {
        if(infoKind != InfoKind::CatInfo)
            throw OSErrorException(fnfErr);

        pb->dirInfo.ioFlAttrib = ATTRIB_ISADIR;
        
        itemCache->cacheDirectory(dirItem);
        pb->dirInfo.ioDrNmFls = dirItem->countItems();

        // TODO:
        pb->dirInfo.ioACUser = 0;
        pb->dirInfo.ioDrUsrWds = {};
        pb->dirInfo.ioDrCrDat = 0;
        pb->dirInfo.ioDrMdDat = 0;
        pb->dirInfo.ioDrBkDat = 0;
        pb->dirInfo.ioDrFndrInfo = {};


    }
    else if(FileItem *fileItem = dynamic_cast<FileItem*>(item.get()))
    {
        pb->hFileInfo.ioFlAttrib = open_attrib_bits(item->cnid(), &vcb, &pb->hFileInfo.ioFRefNum);
        auto info = fileItem->getInfo();
        pb->hFileInfo.ioFlFndrInfo = info.file.info;
        
        size_t dsize = fileItem->open(fsRdPerm)->getEOF();
        size_t rsize = fileItem->openRF(fsRdPerm)->getEOF();
        
        pb->hFileInfo.ioFlLgLen = dsize;
        pb->hFileInfo.ioFlPyLen = dsize;
        pb->hFileInfo.ioFlRLgLen = rsize;
        pb->hFileInfo.ioFlRLgLen = rsize;

        pb->hFileInfo.ioFlMdDat = info.modTime;
        pb->hFileInfo.ioFlCrDat = info.creationTime;

        if(infoKind == InfoKind::CatInfo)
        {
            // ioFlBkDat
            pb->hFileInfo.ioFlXFndrInfo = info.file.xinfo;

            // ioFlClpSiz
        }
    }
    else
        std::abort();
}

void LocalVolume::PBGetCatInfo(CInfoPBPtr pb)
{
    getInfoCommon(pb, InfoKind::CatInfo);
}
void LocalVolume::PBHGetFInfo(HParmBlkPtr pb)
{
    getInfoCommon(reinterpret_cast<CInfoPBPtr>(pb), InfoKind::HFInfo);
}
void LocalVolume::PBGetFInfo(ParmBlkPtr pb)
{
    getInfoCommon(reinterpret_cast<CInfoPBPtr>(pb), InfoKind::FInfo);
}

void LocalVolume::setInfoCommon(const ItemPtr& item, CInfoPBPtr pb, InfoKind infoKind)
{
    if(FileItemPtr fitem = std::dynamic_pointer_cast<FileItem>(item))
    {
        ItemInfo info;

        if(infoKind == InfoKind::CatInfo)
            info.file.xinfo = pb->hFileInfo.ioFlXFndrInfo;
        else
            info = fitem->getInfo();
        info.file.info = pb->hFileInfo.ioFlFndrInfo;
        info.modTime = pb->hFileInfo.ioFlMdDat;
        info.creationTime = pb->hFileInfo.ioFlCrDat;
        try
        {
            fitem->setInfo(info);
        }
        catch(const UpgradeRequiredException&)
        {
            fitem = upgradeItem(fitem, upgradedItemFactory);
            fitem->setInfo(info);
        }
        // TODO: if(infoKind == InfoKind::CatInfo)) ioFlBkDat
    }
    else
        throw OSErrorException(paramErr);   // TODO: item is a directory
}

void LocalVolume::PBSetCatInfo(CInfoPBPtr pb)
{
    setInfoCommon(resolve(pb->hFileInfo.ioNamePtr, pb->hFileInfo.ioVRefNum, pb->hFileInfo.ioDirID),
        pb, InfoKind::CatInfo);
}
void LocalVolume::PBSetFInfo(ParmBlkPtr pb)
{
    setInfoCommon(resolve(pb->fileParam.ioNamePtr, pb->fileParam.ioVRefNum, 0),
        reinterpret_cast<CInfoPBPtr>(pb), InfoKind::FInfo);
}
void LocalVolume::PBHSetFInfo(HParmBlkPtr pb)
{
    setInfoCommon(resolve(pb->fileParam.ioNamePtr, pb->fileParam.ioVRefNum, pb->fileParam.ioDirID),
        reinterpret_cast<CInfoPBPtr>(pb), InfoKind::HFInfo);
}


LocalVolume::NonexistentFile LocalVolume::resolveForCreate(mac_string_view name, short vRefNum, CNID dirID)
{
    std::shared_ptr<DirectoryItem> parent;
    auto colon = name.rfind(':');

    if(colon == mac_string_view::npos)
        parent = resolveDir(vRefNum, dirID);
    else
    {
        parent = std::dynamic_pointer_cast<DirectoryItem>(resolve(mac_string_view(name.data(), colon), vRefNum, dirID));
        if(!parent)
            throw OSErrorException(dirNFErr);
        name = mac_string_view(name.begin() + colon+1, name.end());
    }

    itemCache->cacheDirectory(parent);
    if(parent->tryResolve(name))
        throw OSErrorException(dupFNErr);
    
    return NonexistentFile { parent, name };
}

void LocalVolume::createCommon(NonexistentFile file)
{
    fs::path parentPath = file.parent->path();
    defaultItemFactory->createFile(parentPath / toUnicodeFilename(file.name));
    itemCache->flushDirectoryCache(file.parent);
}
void LocalVolume::PBCreate(ParmBlkPtr pb)
{
    createCommon(resolveForCreate(pb->ioParam.ioNamePtr, pb->fileParam.ioVRefNum, 0));
}
void LocalVolume::PBHCreate(HParmBlkPtr pb)
{
    createCommon(resolveForCreate(pb->ioParam.ioNamePtr, pb->fileParam.ioVRefNum, pb->fileParam.ioDirID));
}

void LocalVolume::PBDirCreate(HParmBlkPtr pb)
{
    auto [parent, name] = resolveForCreate(pb->ioParam.ioNamePtr, pb->fileParam.ioVRefNum, pb->fileParam.ioDirID);
    fs::path fn = toUnicodeFilename(name);
    fs::create_directory(parent->path() / fn);

    itemCache->flushDirectoryCache(parent);

    itemCache->cacheDirectory(parent);
    auto item = parent->tryResolve(name);
    if(!item)
        throw OSErrorException(ioErr);
    pb->fileParam.ioDirID = item->cnid();
}


void LocalVolume::deleteCommon(ItemPtr item)
{
    itemCache->deleteItem(item);
}

void LocalVolume::PBDelete(ParmBlkPtr pb)
{
    deleteCommon(resolve(pb->ioParam.ioNamePtr, pb->ioParam.ioVRefNum, 0));
}
void LocalVolume::PBHDelete(HParmBlkPtr pb)
{
    deleteCommon(resolve(pb->ioParam.ioNamePtr, pb->ioParam.ioVRefNum, pb->fileParam.ioDirID));
}

void LocalVolume::renameCommon(ItemPtr item, mac_string_view newName)
{
    if(newName.find(':') != mac_string_view::npos)
        throw OSErrorException(bdNamErr);

    auto parent = resolveDir(item->parID());
    itemCache->cacheDirectory(parent);
    if(parent->tryResolve(newName))
        throw OSErrorException(dupFNErr);
    
    itemCache->renameItem(item, newName);
}


void LocalVolume::PBRename(ParmBlkPtr pb)
{
    renameCommon(resolve(pb->ioParam.ioNamePtr, pb->ioParam.ioVRefNum, 0), 
                guest_cast<StringPtr>(pb->ioParam.ioMisc));
}

void LocalVolume::PBHRename(HParmBlkPtr pb)
{
    renameCommon(resolve(pb->ioParam.ioNamePtr, pb->ioParam.ioVRefNum, pb->fileParam.ioDirID), 
                guest_cast<StringPtr>(pb->ioParam.ioMisc));
}

void LocalVolume::PBOpenWD(WDPBPtr pb)
{
    ItemPtr item = resolve(pb->ioNamePtr, pb->ioVRefNum, pb->ioWDDirID);
    std::cout << "PBOpenWD: " << item->path() << std::endl;

    long dirID;
    if(DirectoryItem *dirItem = dynamic_cast<DirectoryItem*>(item.get()))
        dirID = dirItem->dirID();
    else
        dirID = item->parID();

    OSErr err =  ROMlib_mkwd(pb, &vcb, dirID, pb->ioWDProcID);
    if(err)
        throw OSErrorException(err);
}

void LocalVolume::PBRead(ParmBlkPtr pb)
{
    pb->ioParam.ioActCount = 0;
    PBSetFPos(pb);
    auto& fcbx = getFCBX(pb->ioParam.ioRefNum);
    size_t req = pb->ioParam.ioReqCount;
    size_t act = fcbx.access->read(fcbx.fcb->fcbCrPs, pb->ioParam.ioBuffer, req);
    pb->ioParam.ioActCount = act;
    pb->ioParam.ioPosOffset = fcbx.fcb->fcbCrPs =  fcbx.fcb->fcbCrPs + act ;

    if(act < req)
        throw OSErrorException(eofErr);
}
void LocalVolume::PBWrite(ParmBlkPtr pb)
{
    pb->ioParam.ioActCount = 0;
    setFPosCommon(pb, false);
    auto& fcbx = getFCBX(pb->ioParam.ioRefNum);
    
    
    size_t n;
    try
    {
        n = fcbx.access->write(fcbx.fcb->fcbCrPs, pb->ioParam.ioBuffer, pb->ioParam.ioReqCount);
    }
    catch(const UpgradeRequiredException&)
    {
        upgradeItem(fcbx.file, upgradedItemFactory);
        n = fcbx.access->write(fcbx.fcb->fcbCrPs, pb->ioParam.ioBuffer, pb->ioParam.ioReqCount);
    }
    
    pb->ioParam.ioActCount = n;
    pb->ioParam.ioPosOffset = fcbx.fcb->fcbCrPs =  fcbx.fcb->fcbCrPs + n ;
}
void LocalVolume::PBClose(ParmBlkPtr pb)
{
    short refNum = pb->ioParam.ioRefNum;
    auto& fcbx = getFCBX(refNum);
    fcbx.fcb->fcbFlNum = 0;

    if(fcbx.file->resWriteAccessRefNum == refNum)
        fcbx.file->resWriteAccessRefNum = -1;
    if(fcbx.file->dataWriteAccessRefNum == refNum)
        fcbx.file->dataWriteAccessRefNum = -1;

    fcbx = FCBExtension();
}

void LocalVolume::PBGetFPos(ParmBlkPtr pb)
{
    auto& fcbx = getFCBX(pb->ioParam.ioRefNum);
    pb->ioParam.ioReqCount = 0;
    pb->ioParam.ioActCount = 0;
    pb->ioParam.ioPosMode = 0;
    pb->ioParam.ioPosOffset = fcbx.fcb->fcbCrPs;
}

void LocalVolume::setFPosCommon(ParmBlkPtr pb, bool checkEOF)
{
    auto& fcbx = getFCBX(pb->ioParam.ioRefNum);
    int32_t eof = (int32_t)fcbx.access->getEOF();
    int32_t newPos = fcbx.fcb->fcbCrPs;
    
    switch(pb->ioParam.ioPosMode)
    {
        case fsAtMark:
            break;
        case fsFromStart:
            newPos = pb->ioParam.ioPosOffset;
            break;
        case fsFromLEOF:
            newPos = eof + pb->ioParam.ioPosOffset;
            break;
        case fsFromMark:
            newPos = fcbx.fcb->fcbCrPs + pb->ioParam.ioPosOffset;
            break;
    }

    OSErr err = noErr;
    if(newPos > eof && checkEOF)
    {
        newPos = eof;
        err = eofErr;
    }
    
    if(newPos < 0)
    {
        newPos = fcbx.fcb->fcbCrPs;     // MacOS 9 behavior.
                                            // System 6 actually sets fcbCrPs to a negative value.
        err = posErr;
    }

    pb->ioParam.ioPosOffset = fcbx.fcb->fcbCrPs = newPos;

    if(err)
        throw OSErrorException(err);
}
void LocalVolume::PBSetFPos(ParmBlkPtr pb)
{
    setFPosCommon(pb, true);
}
void LocalVolume::PBGetEOF(ParmBlkPtr pb)
{
    auto& fcbx = getFCBX(pb->ioParam.ioRefNum);
    size_t n = fcbx.access->getEOF();
    pb->ioParam.ioMisc = n;
}

void LocalVolume::PBSetEOF(ParmBlkPtr pb)
{
    auto& fcbx = getFCBX(pb->ioParam.ioRefNum);
    try
    {
        fcbx.access->setEOF(pb->ioParam.ioMisc);
    }
    catch(const UpgradeRequiredException&)
    {
        upgradeItem(fcbx.file, upgradedItemFactory);
        fcbx.access->setEOF(pb->ioParam.ioMisc);
    }
}

void LocalVolume::PBCatMove(CMovePBPtr pb)
{
    ItemPtr item = resolve(pb->ioNamePtr, pb->ioVRefNum, pb->ioDirID);
    auto newParent = std::dynamic_pointer_cast<DirectoryItem>(resolve(pb->ioNewName, pb->ioVRefNum, pb->ioNewDirID));

    if(!newParent)
        throw OSErrorException(dirNFErr);   // check this?

    auto name = item->name();

    itemCache->cacheDirectory(newParent);
    if(newParent->tryResolve(mac_string_view(name.data(), name.size())))
        throw OSErrorException(dupFNErr);

    itemCache->moveItem(item, newParent);
}

void LocalVolume::PBFlushFile(ParmBlkPtr pb)
{
}
void LocalVolume::PBFlushVol(ParmBlkPtr pb)
{
}

void LocalVolume::PBAllocate(ParmBlkPtr pb)
{
    // no-op... emulated programs will have a hard time noticing that we're not actually pre-allocating blocks
    // just pretend that everything worked.
    pb->ioParam.ioActCount = (pb->ioParam.ioReqCount + 511) & ~511;
}
void LocalVolume::PBAllocContig(ParmBlkPtr pb)
{
    // no-op, like PBALlocate above
    pb->ioParam.ioActCount = (pb->ioParam.ioReqCount + 511) & ~511;
}
void LocalVolume::PBLockRange(ParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}
void LocalVolume::PBUnlockRange(ParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}
void LocalVolume::PBSetFLock(ParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}
void LocalVolume::PBHSetFLock(HParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}
void LocalVolume::PBRstFLock(ParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}
void LocalVolume::PBHRstFLock(HParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}
void LocalVolume::PBSetFVers(ParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}

void LocalVolume::PBSetVInfo(HParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}
void LocalVolume::PBUnmountVol(ParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}
void LocalVolume::PBEject(ParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}
void LocalVolume::PBOffLine(ParmBlkPtr pb)
{
    throw OSErrorException(paramErr);
}

std::optional<FSSpec> LocalVolume::nativePathToFSSpec(const fs::path& inPath)
{
    boost::system::error_code ec;

    if(!fs::exists(inPath))
        return std::nullopt;

    auto path = fs::canonical(inPath, ec);
    if(ec)
        return {};
    ItemPtr item = itemCache->tryResolve(path);

    if(item)
    {
        FSSpec spec;
        const auto& name = item->name();
        spec.vRefNum = vcb.vcbVRefNum;
        spec.parID = item->parID();
        memcpy(&spec.name[1], name.c_str(), name.size());
        spec.name[0] = name.size();
        return spec;
    }
    else
        return std::nullopt;
}

static void MountLocalVolume(fs::path root)
{
    VCBExtra *vp;
    GUEST<THz> savezone;

    savezone = LM(TheZone);
    LM(TheZone) = LM(SysZone);
    vp = (VCBExtra *)NewPtr(sizeof(VCBExtra));
    LM(TheZone) = savezone;

    if(!vp)
        return;
    memset(vp, 0, sizeof(VCBExtra));
    vp->vcb.vcbDrvNum = 42;//pb->ioParam.ioVRefNum;

    --ROMlib_nextvrn;
    vp->vcb.vcbVRefNum = ROMlib_nextvrn;

    std::string rootName = root.root_name().string();
    if(rootName.empty())
        rootName = "vol";
    else if(rootName.back() == ':')
        rootName.pop_back();

    memcpy((char *)vp->vcb.vcbVN + 1, rootName.data(), rootName.size());
    vp->vcb.vcbVN[0] = rootName.size();

    fprintf(stderr, "Volume %s mounted as %s, vref %d\n", root.string().c_str(), rootName.c_str(), int(vp->vcb.vcbVRefNum));

    vp->vcb.vcbSigWord = 0x4244; /* IMIV-188 */
    vp->vcb.vcbFreeBks = 20480; /* arbitrary */
    vp->vcb.vcbCrDate = 0; /* I'm lying here */
    vp->vcb.vcbVolBkUp = 0;
    vp->vcb.vcbAtrb = VNONEJECTABLEBIT;
    vp->vcb.vcbNmFls = 100;
    vp->vcb.vcbNmAlBlks = 20480;
    vp->vcb.vcbAlBlkSiz = 10240;
    vp->vcb.vcbClpSiz = 1;
    vp->vcb.vcbAlBlSt = 10;
    vp->vcb.vcbNxtCNID = 1000;
    if(!LM(DefVCBPtr))
    {
        LM(DefVCBPtr) = (VCB *)vp;
        LM(DefVRefNum) = vp->vcb.vcbVRefNum;
        DefDirID = 2; /* top level */
    }
    Enqueue((QElemPtr)vp, &LM(VCBQHdr));

    vp->volume = new LocalVolume(vp->vcb, root);
}

void Executor::MountLocalVolumes()
{
#ifdef _WIN32
    uint32_t drives = ::GetLogicalDrives();
    for(int i = 0; i < 26; i++)
        if(drives & (1 << i))
        {
            boost::system::error_code ec;
            fs::path path = fs::canonical(std::string(1, 'A' + i) + ":/", ec);
            if(!ec)
                MountLocalVolume(path);
        }
#else
    MountLocalVolume("/");
#endif
}

std::optional<FSSpec> Executor::nativePathToFSSpec(const fs::path& p)
{
    VCBExtra *vcbp;

    for(vcbp = (VCBExtra *)LM(VCBQHdr).qHead; vcbp; vcbp = (VCBExtra *)vcbp->vcb.qLink)
    {
        if(auto volume = dynamic_cast<LocalVolume*>(vcbp->volume))
        {
            if(auto spec = volume->nativePathToFSSpec(p))
                return spec;
        }
    }

    return std::nullopt;
}
