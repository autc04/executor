#pragma once

#include "cnidmapper.h"
#include <rsys/filesystem.h>
#include <util/macstrings.h>
#include <memory>
#include <chrono>
#include <list>
#include <unordered_map>
#include <stdint.h>

namespace Executor
{
class Item;
class FileItem;
class ItemFactory;
class CNIDMapper;
class DirectoryItem;
struct Descriptor;

using ItemPtr = std::shared_ptr<Item>;
using DirectoryItemPtr = std::shared_ptr<DirectoryItem>;

using CNID = int32_t;

class ItemCache
{
    std::unique_ptr<CNIDMapper> cnidMapper_;
    ItemFactory *itemFactory_;
    const fs::path rootPath_;

    std::unordered_map<CNID, std::weak_ptr<Item>> items_; 

    struct CachedItem
    {
        ItemPtr item;
        size_t weight = 0;
    };
    std::list<CachedItem> cachedItems_;
    size_t cachedWeight_ = 0;   // total weight the queue holds

    // Promote `item` to the most-recently-used end of the queue, inserting it if
    // it is not there yet. The queue holds every realized Item -- files and
    // directories alike -- so a probed FileRep or a directory listing survives
    // between accesses. "Weight" is a relative cost (Item::cacheWeight), not
    // bytes.
    void touchItem(const ItemPtr& item);

    // Bound the queue's total weight by dropping references, least recently used
    // first. Eviction never clears a payload: an item no one else holds dies with
    // its payload, while one a consumer still holds keeps it (the consumer's
    // memory, not the cache's).
    void evictCachedItems();

    // A live, already-realized Item, if its kind still matches. Otherwise the
    // registry entry is dropped so the Item can be rebuilt. A moved binding
    // re-points the live Item in place.
    ItemPtr itemSnapshot(CNID cnid, const fs::path& path, bool isDir);

    // The host path a CNID denotes: its own posix name and each ancestor's,
    // joined onto the volume root. Empty if the chain is broken. Derived from
    // records only: a live Item's path can lag its record (a listing of an
    // ancestor can recover a rename the live Item has not been relocated for),
    // so it is never a source here.
    fs::path pathOf(CNID cnid);

    // Realize a CNID, validating its binding (see realizeValidated).
    ItemPtr realizeOnce(CNID cnid);

    // Validate a CNID's binding against the host and realize the Item. If the
    // binding is stale, flush the parent's listing (so the diff can recover a
    // same-directory rename or drop a gone entry) and retry, recursing up while
    // an ancestor's own binding is stale. `depth` bounds that recursion.
    ItemPtr realizeValidated(CNID cnid);
    ItemPtr realizeValidated(CNID cnid, int depth);

    // Realize a descriptor's Item (from the registry, else freshly built).
    ItemPtr realizeChild(const DirectoryItemPtr& dir, const Descriptor& descriptor);

    // The CNID for the child of `parID` named `posixName`, creating a record when
    // there is none. The same identity and kind reuse the CNID; any other file at
    // that name is a different item, so the old record is dropped and a fresh CNID
    // is assigned. No name is decided.
    CNID resolveChild(CNID parID, const fs::path& posixName, const PathStat& st);

    // Ensure `dir` is listed and valid; re-lists if the host changed. A scan that
    // fails is not committed: the listing is dropped and this returns false, so
    // the caller reports ioErr and the next access retries. Private: obtain
    // children through child(), which owns this precondition.
    bool ensureListed(const DirectoryItemPtr& item);

    // Construct an identity Item from known host file data, without running the
    // format gauntlet, and register it.
    ItemPtr makeItem(CNID parID, CNID cnid, const fs::path& path,
        std::optional<mac_string> name, bool isDir);

    // The filename a child's Mac name is derived from (differs from the host
    // filename only for MacBinary, whose ".bin" is dropped). Probes the file.
    fs::path nameSourceFor(const fs::path& path);

    // Decide the Mac names of the `macNamePrefix` group whose folded prefix is
    // `prefix` (the NameResolve batch). Probes the group's ".bin" entries only.
    void resolveNameGroup(const DirectoryItemPtr& dir, mac_string_view prefix);

public:
    // Build and install the FileRep for a FileItem (the format gauntlet).
    // Idempotent; called lazily from FileItem on first access.
    void materialize(FileItem& item);

    ItemCache(fs::path root, mac_string_view volumeName,
                std::unique_ptr<CNIDMapper> cnidMapper,
                ItemFactory *itemFactory
                );
    ~ItemCache();

    void noteItemFreed(CNID cnid);

    ItemPtr tryResolve(CNID cnid);
    ItemPtr tryResolve(fs::path path);

    // Obtain a child of a directory. This is the only way to query a
    // directory's contents: it first ensures `dir` is enumerated and valid, so
    // that precondition is an invariant of the lookup rather than a caller
    // obligation. Returns null if there is no such child; throws ioErr if `dir`
    // itself cannot be read.
    ItemPtr child(const DirectoryItemPtr& dir, mac_string_view name);
    ItemPtr child(const DirectoryItemPtr& dir, int index, bool includeDirectories);

    // Ensure `item`'s Mac name is decided, deciding its directory group if not.
    // Called from Item::name().
    void ensureNamed(Item& item);

    // Number of visible children, without enumerating/constructing them. Throws
    // ioErr if the directory cannot be read (a failed scan is never cached).
    int countChildren(const DirectoryItemPtr& item);
    void flushDirectoryCache(DirectoryItemPtr item);
    void flushDirectoryCache(CNID dirID);

    void deleteItem(ItemPtr item);
    void renameItem(ItemPtr item, mac_string_view newName);
    void moveItem(ItemPtr item, DirectoryItemPtr newParent, mac_string_view newName = {});
};

}
