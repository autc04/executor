#include "appledouble.h"
#include "plain.h"

#include <algorithm>

using namespace Executor;

static fs::path makeADPath(AppleDoubleScheme scheme, const fs::path& p)
{
    switch(scheme)
    {
        case AppleDoubleScheme::percent:
            return p.parent_path() / ("%" + p.filename().string());
        case AppleDoubleScheme::dotunderscore:
            return p.parent_path() / ("._" + p.filename().string());
        default:
            std::abort();
    }
}

bool AppleDoubleItemFactory::isHidden(const fs::directory_entry& e)
{
    if(e.path().filename().string().substr(0,1) == "%")
        return true;
    if(e.path().filename().string().substr(0,2) == "._")
        return true;
    return false;
}

ItemPtr AppleDoubleItemFactory::createItemForDirEntry(ItemCache& itemcache, CNID parID, CNID cnid,
    const fs::directory_entry& e, mac_string_view macname)
{
    if(fs::is_regular_file(e.path()))
    {
        for(auto scheme : { AppleDoubleScheme::percent, AppleDoubleScheme::dotunderscore })
        {
            if(fs::is_regular_file(makeADPath(scheme, e.path())))
                return std::make_shared<AppleDoubleFileItem>(scheme, itemcache, parID, cnid, e.path(), macname);
        }
    }
    return nullptr;
}

void AppleDoubleItemFactory::createFile(const fs::path& path)
{
    fs::path adpath = makeADPath(AppleDoubleScheme::percent, path);

    PlainDataFork data(path, PlainDataFork::create);
    AppleSingleDoubleFile rsrc(std::make_unique<PlainDataFork>(adpath, PlainDataFork::create),
                                AppleSingleDoubleFile::create_double);
}

AppleDoubleFileItem::AppleDoubleFileItem(AppleDoubleScheme scheme, ItemCache& itemcache, CNID parID, CNID cnid, fs::path p, mac_string_view name)
    : FileItem(itemcache, parID, cnid, p, name), scheme(scheme)
{
}

std::shared_ptr<AppleSingleDoubleFile> AppleDoubleFileItem::access(int8_t permission)
{
    std::shared_ptr<AppleSingleDoubleFile> p;
    if((p = openedFile.lock()))
        return p;
    else
    {
        fs::path adpath = makeADPath(scheme, path());
        p = std::make_shared<AppleSingleDoubleFile>(std::make_unique<PlainDataFork>(adpath, permission));
        openedFile = p;
        return p;
    }
}
std::unique_ptr<OpenFile> AppleDoubleFileItem::open(int8_t permission)
{
    return std::make_unique<PlainDataFork>(path_, permission);
}
std::unique_ptr<OpenFile> AppleDoubleFileItem::openRF(int8_t permission)
{
    return std::make_unique<AppleSingleDoubleFork>(access(permission), 2);
}

ItemInfo AppleDoubleFileItem::getInfo()
{
    ItemInfo info = FileItem::getInfo();
    access(fsRdPerm)->read(9, 0, &info, sizeof(info.file));

    return info;
}

void AppleDoubleFileItem::setInfo(ItemInfo info)
{
    FileItem::setInfo(info);
    access(fsRdWrPerm)->write(9, 0, &info, sizeof(info.file));
}

void AppleDoubleFileItem::deleteItem()
{
    fs::remove(path());
    fs::path adpath = makeADPath(scheme, path());
    boost::system::error_code ec;
    fs::remove(adpath, ec);
}

void AppleDoubleFileItem::moveItem(const fs::path& newPath, mac_string_view newName)
{
    fs::path adpathOld = makeADPath(scheme, path());
    fs::path adpathNew = makeADPath(scheme, newPath);

    fs::rename(path(), newPath);

    boost::system::error_code ec;
    fs::rename(adpathOld, adpathNew, ec);

    path_ = newPath;
    if(!newName.empty())
        name_ = newName;
}

ItemPtr AppleSingleItemFactory::createItemForDirEntry(ItemCache& itemcache, CNID parID, CNID cnid,
    const fs::directory_entry& e, mac_string_view macname)
{
    if(!fs::is_regular_file(e.path()))
        return nullptr;
    GUEST<uint64_t> magic = 0;
    fs::ifstream(e.path(), std::ios::binary).read((char*)&magic, 8);

    if(magic == 0x0005160000020000)
        return std::make_shared<AppleSingleFileItem>(itemcache, parID, cnid, e.path(), macname);
    else
        return nullptr;
}

void AppleSingleItemFactory::createFile(const fs::path& path)
{
    AppleSingleDoubleFile rsrc(std::make_unique<PlainDataFork>(path, PlainDataFork::create),
                                AppleSingleDoubleFile::create_single);
}


std::shared_ptr<AppleSingleDoubleFile> AppleSingleFileItem::access(int8_t permission)
{
    std::shared_ptr<AppleSingleDoubleFile> p;
    if((p = openedFile.lock()))
        return p;
    else
    {
        p = std::make_shared<AppleSingleDoubleFile>(std::make_unique<PlainDataFork>(path(), permission));
        openedFile = p;
        return p;
    }
}
std::unique_ptr<OpenFile> AppleSingleFileItem::open(int8_t permission)
{
    return std::make_unique<AppleSingleDoubleFork>(access(permission), 1);
}
std::unique_ptr<OpenFile> AppleSingleFileItem::openRF(int8_t permission)
{
    return std::make_unique<AppleSingleDoubleFork>(access(permission), 2);
}

ItemInfo AppleSingleFileItem::getInfo()
{
    ItemInfo info = FileItem::getInfo();
    access(fsRdPerm)->read(9, 0, &info, sizeof(info));

    return info;
}

void AppleSingleFileItem::setInfo(ItemInfo info)
{
    FileItem::setInfo(info);
    access(fsRdWrPerm)->write(9, 0, &info, sizeof(info.file));
}

AppleSingleDoubleFile::AppleSingleDoubleFile(std::unique_ptr<OpenFile> aFile)
    : file(std::move(aFile))
{
    //GUEST<uint64_t> magicAndVersion;
    //file->read(0, &magicAndVersion, 8);

    GUEST<uint16_t> n;
    file->read(24, &n, 2);
    descriptors.resize(n);
    file->read(26, descriptors.data(), sizeof(EntryDescriptor) * n);
    std::sort(descriptors.begin(), descriptors.end(), [](auto& a, auto& b) { return a.offset < b.offset; });
}

AppleSingleDoubleFile::AppleSingleDoubleFile(std::unique_ptr<OpenFile> aFile, create_single_t)
    : file(std::move(aFile))
{
    GUEST<uint64_t> magicAndVersion = 0x0005160000020000;
    file->write(0, &magicAndVersion, 8);
    uint16_t zero = 0;
    file->write(24, &zero, 2);
}

AppleSingleDoubleFile::AppleSingleDoubleFile(std::unique_ptr<OpenFile> aFile, create_double_t)
    : file(std::move(aFile))
{
    GUEST<uint64_t> magicAndVersion = 0x0005160700020000;
    file->write(0, &magicAndVersion, 8);
    uint16_t zero = 0;
    file->write(24, &zero, 2);
}

AppleSingleDoubleFile::~AppleSingleDoubleFile()
{

}

size_t AppleSingleDoubleFile::getEOF(uint32_t entryId)
{
    auto p = findEntry(entryId);
    if(p != descriptors.end())
        return p->length;
    return 0;
}

AppleSingleDoubleFile::EntryDescriptorIterator AppleSingleDoubleFile::findEntry(uint32_t entryId)
{
    return std::find_if(descriptors.begin(), descriptors.end(),
        [entryId](auto& desc) { return desc.entryId == entryId; });
}

AppleSingleDoubleFile::EntryDescriptorIterator AppleSingleDoubleFile::setEOFCommon(uint32_t entryId, size_t sz, bool allowShrink)
{
    // figure out which entry we are talking about,
    // and insert a new one if necessary
    auto p = findEntry(entryId);

    if(p == descriptors.end())
    {
        if(sz == 0)
            return p;
        p = descriptors.insert(p, EntryDescriptor{entryId, 0, 0});
    }
    
    // keep a record of the previous layout
    auto oldDescriptors = descriptors;

    // update EOF for selected entry
    if(allowShrink || p->length < sz)
        p->length = sz;

    // update offsets for entries
    uint32_t offset = 24 + 2 + sizeof(EntryDescriptor) * descriptors.size();
    for(auto& desc : descriptors)
    {
        if(offset > desc.offset)
        {
            desc.offset = offset + 256;
        }
        else if(desc.offset - offset > 512)
        {
            desc.offset = offset + 256; 
        }
        offset = desc.offset + desc.length;
    }

    // shift data around.
    // 
    for(auto src = oldDescriptors.begin(), dst = descriptors.begin();
        dst != descriptors.end(); ++src, ++dst)
    {
        uint32_t dstOffset = dst->offset;
        uint32_t srcOffset = src->offset;        
        if(dstOffset < srcOffset)
        {
            uint32_t n = std::min(dst->length, src->length);

            while(n)
            {
                const uint32_t blockSize = 4096;
                uint32_t n1 = std::min(blockSize, n);
                uint8_t buf[blockSize];
                file->read(srcOffset, buf, n1);
                file->write(dstOffset, buf, n1);
                srcOffset += n1;
                dstOffset += n1;
                n -= n1;
            }
        }
    }

    for(auto src = oldDescriptors.rbegin(), dst = descriptors.rbegin();
        dst != descriptors.rend(); ++src, ++dst)
    {
        uint32_t dstOffset = dst->offset;
        uint32_t srcOffset = src->offset;        
        if(dstOffset > srcOffset)
        {
            uint32_t n = std::min(dst->length, src->length);
            dstOffset += n;
            srcOffset += n;
            while(n)
            {
                const uint32_t blockSize = 4096;
                uint32_t n1 = std::min(blockSize, n);
                uint8_t buf[blockSize];
                srcOffset -= n1;
                dstOffset -= n1;
                file->read(srcOffset, buf, n1);
                file->write(dstOffset, buf, n1);
                n -= n1;
            }
        }
    }

    // update headers
    {
        GUEST<uint16_t> n = descriptors.size();
        file->write(24, &n, 2);
        file->write(26, descriptors.data(), sizeof(EntryDescriptor) * n);
    }

    return p;
}

void AppleSingleDoubleFile::setEOF(uint32_t entryId, size_t sz)
{
    setEOFCommon(entryId, sz, true);
}

size_t AppleSingleDoubleFile::read(uint32_t entryId, size_t offset, void *p, size_t n)
{
    auto descIt = findEntry(entryId);
    if(descIt != descriptors.end())
    {
        if(offset >= descIt->length)
            return 0;
        if(offset + n > descIt->length)
            n = descIt->length - offset;
        return file->read(offset + descIt->offset, p, n);
    }
    return 0;
}

size_t AppleSingleDoubleFile::write(uint32_t entryId, size_t offset, void *p, size_t n)
{
    if(n == 0)
        return 0;
    auto descIt = setEOFCommon(entryId, offset + n, false);
    return file->write(descIt->offset + offset, p, n);
}


AppleSingleDoubleFork::AppleSingleDoubleFork(std::shared_ptr<AppleSingleDoubleFile> file, uint32_t entryId)
    : file(file), entryId(entryId)
{

}

AppleSingleDoubleFork::~AppleSingleDoubleFork()
{
}

size_t AppleSingleDoubleFork::getEOF()
{
    return file->getEOF(entryId);
}
void AppleSingleDoubleFork::setEOF(size_t sz)
{
    file->setEOF(entryId, sz);
}
size_t AppleSingleDoubleFork::read(size_t offset, void *p, size_t n)
{
    return file->read(entryId, offset, p, n);
}
size_t AppleSingleDoubleFork::write(size_t offset, void *p, size_t n)
{
    return file->write(entryId, offset, p, n);
}
