#pragma once

#include "cnidmapper.h"
#include <rsys/filesystem.h>
#include <util/macstrings.h>
#include <memory>
#include <list>
#include <optional>
#include <unordered_map>
#include <stdint.h>

namespace Executor
{
class Item;
class FileItem;
class ItemFactory;
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
    // Item -> its slot in cachedItems_, so a touch does not scan the queue. Kept
    // in step with cachedItems_ by cacheItem only.
    std::unordered_map<Item*, std::list<CachedItem>::iterator> cachedIndex_;
    size_t cachedWeight_ = 0;   // total weight the queue holds

    // The realized Item for `cnid` if its kind still matches the description, else
    // nothing (and the stale registry entry is dropped so the Item can be rebuilt).
    // Re-points the item at `path` and marks it used, so a recovered rename is
    // picked up and the LRU's recency reflects this access.
    ItemPtr findAndRefreshRealizedItem(CNID cnid, const fs::path& path, bool isDir);

    // The host path a CNID denotes: its own posix name and each ancestor's,
    // joined onto the volume root. Empty if the chain is broken. Derived from
    // records only: a live Item's path can lag its record (a listing of an
    // ancestor can recover a rename the live Item has not been relocated for),
    // so it is never a source here.
    fs::path pathOf(CNID cnid);

    // Realize a CNID once, validating its binding: null if the binding is stale.
    // The recovering entry is realizeByCNID.
    ItemPtr tryRealize(CNID cnid);

    // CNIDResolve: the CNID for the child of `parID` named `posixName`, creating a
    // record when there is none. The same identity and kind reuse the CNID; any
    // other file at that name is a different item, so the old record is dropped and
    // a fresh CNID is assigned. No name is decided.
    CNID resolveChildCNID(CNID parID, const fs::path& posixName, const PathStat& st);

    // Construct an identity Item from known host file data, without running the
    // format gauntlet, and register it.
    ItemPtr makeItem(CNID parID, CNID cnid, const fs::path& path,
        std::optional<mac_string> name, bool isDir);

    // Invalidation by CNID: resolve the live directory and clear its cached
    // payload. Callers that already hold the DirectoryItem clear it directly.
    void flushDirectoryCache(CNID dirID);

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

    // Realize the Item a CNID denotes, validating its binding against the host.
    // If the binding is stale, re-list the parent (so the diff can recover a
    // same-directory rename or drop a gone entry) and retry, recursing up while an
    // ancestor's own binding is stale. The entry point for an access by CNID.
    ItemPtr realizeByCNID(CNID cnid);

    // Realize the Item at a host path, walking it from the volume root. Each
    // component is CNIDResolved stat-first; no ancestor is listed, and the walk
    // does not name (the target's Mac name is decided lazily, on read).
    ItemPtr realizeByHostPath(fs::path path);

    // The per-volume collaborators the Count/List/NameResolve machinery drives
    // directly: the identity/name store and the format/hidden-file policy.
    CNIDMapper& cnidMapper() { return *cnidMapper_; }
    ItemFactory& itemFactory() { return *itemFactory_; }

    // For use by DirectoryItem, which owns the Count/List/NameResolve machinery
    // and drives these cache- and registry-side pieces; no other client calls them.

    // Mark `item` as in use: promote it to the most-recently-used end of the
    // queue, inserting it if it is not there yet, then bound the queue's weight by
    // dropping references, least recently used first. The queue holds every
    // realized Item -- files and directories alike -- so a probed FileRep or a
    // directory listing survives between accesses; "weight" is a relative cost
    // (Item::cacheWeight), not bytes. This is the only way into the queue.
    // Eviction never clears a payload: an item no one else holds dies with its
    // payload, while one a consumer still holds keeps it (the consumer's memory,
    // not the cache's).
    void cacheItem(const ItemPtr& item);

    // The realized Item for `cnid`, if any -- a plain registry read, nothing more.
    ItemPtr findRealizedItem(CNID cnid) const;

    // Realize the Item a descriptor denotes (from the registry, else freshly built).
    ItemPtr realizeDescriptor(const DirectoryItemPtr& dir, const Descriptor& descriptor);

    // Ensure `item`'s Mac name is decided, deciding its parent directory's group
    // if not. Called from Item::name().
    void ensureNamed(Item& item);

    void deleteItem(ItemPtr item);
    void renameItem(ItemPtr item, mac_string_view newName);
    void moveItem(ItemPtr item, DirectoryItemPtr newParent, mac_string_view newName = {});
};

}
