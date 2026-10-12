#pragma once

#include <util/macstrings.h>
#include <rsys/filesystem.h>
#include <FileMgr.h>
#include "filestamp.h"

#include <memory>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>
#include <optional>

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

    // The filename the displayed Mac name is derived from. Reps that rename on
    // materialization (MacBinary drops a trailing ".bin") override this.
    virtual fs::path nameSource(const fs::path& filename) const;

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
    mac_string name_;             // decided at NameResolve; empty until then
    bool nameDecided_ = false;

    friend class ItemCache;

public:
    Item(ItemCache& itemcache, CNID parID, CNID cnid, fs::path p, std::optional<mac_string> name = {});
    virtual ~Item();

    const fs::path& path() const { return path_; }

    CNID parID() const { return parID_; }
    CNID cnid() const { return cnid_; }

    // The Mac name. Deciding it is NameResolve, so reading it may list the
    // parent directory's `macNamePrefix` group on first use; every name read goes
    // through here, so a caller never observes an undecided name.
    const mac_string& name();

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

// A listed directory entry: the host name, its kind, the grouping key it sorts
// and resolves by, and its identity. It is not an Item — children are realized
// from descriptors on demand and dropped again when nothing holds them.
struct Descriptor
{
    fs::path posixName;
    bool isDir = false;
    mac_string prefix;   // case-folded group/sort key (Mac name without disambiguator)
    CNID cnid = 0;
};

class DirectoryItem : public Item
{
    std::vector<Descriptor> descriptors_;            // sorted by (prefix, posixName)
    bool    listed_ = false;
    FileStamp stamp_;
    std::optional<int> count_;   // Counted: a tally, without the descriptors
    FileStamp countStamp_;       // the stamp that tally is valid for

    friend class ItemCache;
    void clearCache();
    void storeDescriptors(std::vector<Descriptor> descriptors, FileStamp stamp);
    bool isListed() const { return listed_; }

    // Select a descriptor from the (valid) listing. Only ItemCache::child() may
    // call these, so the "listed and valid" precondition cannot be violated
    // elsewhere.
    const Descriptor* childByIndex(int index, bool includeDirectories) const;
public:
    using Item::Item;

    long dirID() const { return cnid(); }

    int countItems() const { return (int)descriptors_.size(); }

    const std::vector<Descriptor>& descriptors() const { return descriptors_; }

    // The [begin, end) range of descriptors whose (folded) prefix equals `prefix`.
    std::pair<size_t, size_t> groupRange(mac_string_view prefix) const;

    virtual void deleteItem();
};

class FileItem : public Item
{
    friend class Executor::LocalVolume;
    short dataWriteAccessRefNum = -1;
    short resWriteAccessRefNum = -1;
    FileRepPtr rep_;
    FileStamp repStamp_;   // (mtime, size) the rep was built from

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
