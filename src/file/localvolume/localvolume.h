#pragma once

#include <file/volume.h>
#include <FileMgr.h>
#include <MemoryMgr.h>
#include <util/macstrings.h>
#include "item.h"

#include <rsys/filesystem.h>

#include <map>
#include <unordered_map>
#include <vector>
#include <optional>

namespace Executor
{


class OpenFile;
class ItemFactory;



class LocalVolume : public Volume, private ItemFactory
{
    fs::path root;

    std::unique_ptr<ItemCache> itemCache;
    std::vector<std::unique_ptr<ItemFactory>> itemFactories;
    ItemFactory *defaultItemFactory;
    ItemFactory *upgradedItemFactory;


    //ItemPtr resolve(long cnid);
    std::shared_ptr<DirectoryItem> resolveDir(long dirID);
    std::shared_ptr<DirectoryItem> resolveDir(short vRef, long dirID);
    ItemPtr resolve(mac_string_view name, short vRef, long dirID);
    ItemPtr resolveForInfo(mac_string_view name, short vRef, long dirID, short index, bool includeDirectories);
    ItemPtr resolveRelative(const std::shared_ptr<DirectoryItem>& base, mac_string_view name);

    struct FCBExtension;
    std::vector<FCBExtension> fcbExtensions;

    FCBExtension& getFCBX(short refNum);
    FCBExtension& openFCBX();

    struct NonexistentFile
    {
        std::shared_ptr<DirectoryItem> parent;
        mac_string_view name;
    };

    NonexistentFile resolveForCreate(mac_string_view name, short vRefNum, CNID dirID);

    void createCommon(NonexistentFile file);

    enum class Fork
    {
        data,
        resource
    };
    void openCommon(GUEST<short>& refNum, ItemPtr item, Fork fork, int8_t permission);
    void deleteCommon(ItemPtr item);
    void renameCommon(ItemPtr item, mac_string_view newName);
    void setFPosCommon(ParmBlkPtr pb, bool checkEOF);

    enum class InfoKind
    {
        FInfo,
        HFInfo,
        CatInfo
    };

    void setInfoCommon(const ItemPtr& item, CInfoPBPtr pb, InfoKind infoKind);
    void getInfoCommon(CInfoPBPtr pb, InfoKind infoKind);

    virtual bool isHidden(const fs::directory_entry& e) override;
    virtual FileRepPtr createRepForFile(ItemCache& itemcache, FileItem& item,
        const fs::directory_entry& e) override;

    FileItemPtr upgradeItem(FileItemPtr item, ItemFactory* betterFactory);

public:
    std::optional<FSSpec> nativePathToFSSpec(const fs::path& p);

    mac_string getVolumeName() const;

    LocalVolume(VCB& vcb, fs::path root);

    virtual void PBGetCatInfo(CInfoPBPtr pb) override;
    virtual void PBGetFInfo(ParmBlkPtr pb) override;
    virtual void PBHGetFInfo(HParmBlkPtr pb) override;

    virtual void PBSetCatInfo(CInfoPBPtr pb) override;
    virtual void PBSetFInfo(ParmBlkPtr pb) override;
    virtual void PBHSetFInfo(HParmBlkPtr pb) override;
    virtual void PBSetVInfo(HParmBlkPtr pb) override;

    virtual void PBDirCreate(HParmBlkPtr pb) override;
    virtual void PBCreate(ParmBlkPtr pb) override;
    virtual void PBHCreate(HParmBlkPtr pb) override;
    virtual void PBDelete(ParmBlkPtr pb) override;
    virtual void PBHDelete(HParmBlkPtr pb) override;
    virtual void PBRename(ParmBlkPtr pb) override;
    virtual void PBHRename(HParmBlkPtr pb) override;
    virtual void PBCatMove(CMovePBPtr pb) override;

    virtual void PBOpenWD(WDPBPtr pb) override;
    virtual void PBOpenDF(ParmBlkPtr pb) override;
    virtual void PBOpenRF(ParmBlkPtr pb) override;
    virtual void PBHOpenDF(HParmBlkPtr pb) override;
    virtual void PBHOpenRF(HParmBlkPtr pb) override;

    virtual void PBSetFLock(ParmBlkPtr pb) override;
    virtual void PBHSetFLock(HParmBlkPtr pb) override;
    virtual void PBRstFLock(ParmBlkPtr pb) override;
    virtual void PBHRstFLock(HParmBlkPtr pb) override;

    virtual void PBClose(ParmBlkPtr pb) override;
    virtual void PBRead(ParmBlkPtr pb) override;
    virtual void PBWrite(ParmBlkPtr pb) override;
    virtual void PBGetFPos(ParmBlkPtr pb) override;
    virtual void PBSetFPos(ParmBlkPtr pb) override;
    virtual void PBGetEOF(ParmBlkPtr pb) override;
    virtual void PBFlushFile(ParmBlkPtr pb) override;

    virtual void PBFlushVol(ParmBlkPtr pb) override;
    virtual void PBUnmountVol(ParmBlkPtr pb) override;
    virtual void PBEject(ParmBlkPtr pb) override;
    virtual void PBOffLine(ParmBlkPtr pb) override;

    virtual void PBAllocate(ParmBlkPtr pb) override;
    virtual void PBAllocContig(ParmBlkPtr pb) override;
    virtual void PBSetEOF(ParmBlkPtr pb) override;
    virtual void PBLockRange(ParmBlkPtr pb) override;
    virtual void PBUnlockRange(ParmBlkPtr pb) override;

    virtual void PBSetFVers(ParmBlkPtr pb) override;
};


class ExtensionItemFactory : public ItemFactory
{
public:
    virtual FileRepPtr createRepForFile(ItemCache& itemcache, FileItem& item,
        const fs::directory_entry& e) override;
    virtual void createFile(const fs::path& newPath) override;
};




} /* namespace Executor */
