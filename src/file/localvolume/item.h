#pragma once

#include <util/macstrings.h>
#include <rsys/filesystem.h>
#include <FileMgr.h>

#include <memory>
#include <map>

namespace Executor
{
class ItemCache;
class LocalVolume;
class OpenFile;

using CNID = int32_t;

class Item;
class DirectoryItem;
class FileItem;

using ItemPtr = std::shared_ptr<Item>;
using DirectoryItemPtr = std::shared_ptr<DirectoryItem>;
using FileItemPtr = std::shared_ptr<FileItem>;

struct ItemInfo
{
    union {
        struct {
            FInfo info;
            FXInfo xinfo;
        } file;
        struct {
            DInfo info;
            DXInfo xinfo;
        } dir;
    };
    unsigned long modTime;
    unsigned long creationTime;
};

// The file-format concern, split out of FileItem so an Item can exist without
// knowing its encoding. A FileRep knows where the forks/Finder info live, how to
// open them, and which host files a delete/move must touch. The delete/move entry
// points stay on FileItem (the Item API); the rep supplies the format-specific
// host-file mutation so it can be deferred until Materialize.
class FileRep
{
protected:
    FileItem& item_;

public:
    explicit FileRep(FileItem& item) : item_(item) {}
    virtual ~FileRep() = default;

    virtual std::unique_ptr<OpenFile> open(int8_t permission) = 0;
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission) = 0;
    virtual void getInfo(ItemInfo& info) = 0;
    virtual void setInfo(const ItemInfo& info) = 0;

    // Host-file mutation. The default handles a single host file; formats that
    // keep sidecars (AppleDouble, Basilisk) override.
    virtual void remove();
    virtual void move(const fs::path& newPath);
};

using FileRepPtr = std::shared_ptr<FileRep>;

class Item
{
protected:
    ItemCache& itemcache_;
    CNID parID_;
    CNID cnid_;
    fs::path path_;
    mac_string name_;

public:
    Item(ItemCache& itemcache, CNID parID, CNID cnid, fs::path p, mac_string_view name = {});
    virtual ~Item();

    const fs::path& path() const { return path_; }

    CNID parID() const { return parID_; }
    CNID cnid() const { return cnid_; }

    const mac_string& name() const { return name_; }

    virtual void deleteItem();
    virtual void moveItem(const fs::path& newPath, mac_string_view newName);

    virtual ItemInfo getInfo();
    virtual void setInfo(ItemInfo info);

        // for use by ItemCache only
    void noteItemFlushed() { cnid_ = -1; }
};


// The format gauntlet. A factory inspects a host file and, if it claims it,
// returns a FileRep for the (already-instantiated) FileItem. Directories are not
// handled here; ItemCache constructs them directly from the cheap "kind"
// decision. This runs at Materialize, not at Enumerate.
class ItemFactory
{
public:
    virtual ~ItemFactory() = default;

    virtual bool isHidden(const fs::directory_entry& e) { return false; }
    virtual FileRepPtr createRepForFile(ItemCache& itemcache, FileItem& item,
        const fs::directory_entry& e) { return {}; }
    virtual void createFile(const fs::path& newPath)
        { throw std::logic_error("createFile unimplemented"); }
};

class DirectoryItem : public Item
{
    std::vector<ItemPtr>    contents_;
    std::vector<ItemPtr>    files_;
    std::map<mac_string, ItemPtr> contents_by_name_;
    bool    cache_valid_ = false;

    friend class ItemCache;
    void clearCache();
    void populateCache(std::vector<ItemPtr> items);
    bool isCached() const { return cache_valid_; }
public:
    using Item::Item;

    ItemPtr tryResolve(mac_string_view name);
    ItemPtr tryResolve(fs::path name);
    ItemPtr resolve(int index, bool includeDirectories);

    long dirID() const { return cnid(); }

    int countItems() { return contents_.size(); }

    virtual void deleteItem();
};

class FileItem : public Item
{
    friend class Executor::LocalVolume;
    short dataWriteAccessRefNum = -1;
    short resWriteAccessRefNum = -1;
    FileRepPtr rep_;

public:
    using Item::Item;

    void setRep(FileRepPtr rep) { rep_ = std::move(rep); }
    const FileRepPtr& rep() const { return rep_; }

    // Build the rep on first use (the format gauntlet). Idempotent.
    void ensureMaterialized();

    virtual std::unique_ptr<OpenFile> open(int8_t permission);
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission);
    virtual ItemInfo getInfo() override;
    virtual void setInfo(ItemInfo info) override;

    virtual void deleteItem() override;
    virtual void moveItem(const fs::path& newPath, mac_string_view newName) override;
};

}
