#pragma once

#include "cnidmapper.h"
#include <rsys/filesystem.h>
#include <util/macstrings.h>
#include <memory>
#include <list>
#include <optional>
#include <unordered_map>
#include <stdint.h>
#include <vector>

namespace Executor
{
class Item;
class ItemFactory;
class DirectoryItem;
struct Descriptor;

using ItemPtr = std::shared_ptr<Item>;
using DirectoryItemPtr = std::shared_ptr<DirectoryItem>;

using CNID = int32_t;

// The owner of the local-volume object model and of the CNID store policy:
// reconciliation, get-or-create, naming and recursive deletion. The
// CNIDMapper behind it is a dumb value store (records and indexes).
class ItemCache
{
public:
    // A host entry offered to the per-directory diff. Only the name is kept: the
    // entry's full path is derived from the parent's chain when needed.
    struct DirEntry
    {
        fs::path posixName;
        FileIdentity identity;
        bool isDir;
    };

    // One member of a name group: the entry to name, the binding the nameSource
    // was derived from (so a name request can be dropped if another instance
    // renamed that binding after the listing), and the filename its Mac name is
    // derived from (MacBinary drops a trailing ".bin").
    struct NameRequest
    {
        CNID cnid;
        fs::path posixName;
        fs::path nameSource;
    };

private:
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

    // CNIDResolve's atomic body: get-or-create for one binding. Only
    // resolveChildCNID calls it.
    CNID getOrCreateChild(CNID parID, const fs::path& posixName,
        const FileIdentity& identity, bool isDir);

    // Recursive record delete and move; only ItemCache's delete/move entry
    // points call them. deleteCNIDLocked is recursive, so it stays a member;
    // the other policy bodies are inlined into their transaction lambdas.
    void deleteCNID(CNID cnid);
    void deleteCNIDLocked(CNID cnid);
    void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName,
        const fs::path& newPosixName);
    void forgetRecordName(const CNIDRecord& record);

    // Construct an identity Item from known host file data, without running the
    // format gauntlet, and register it.
    ItemPtr makeItem(CNID parID, CNID cnid, const fs::path& path,
        std::optional<mac_string> name, bool isDir);

    // Invalidation by CNID: resolve the live directory and clear its cached
    // payload. Callers that already hold the DirectoryItem clear it directly.
    void flushDirectoryCache(CNID dirID);

public:
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

    // The format/hidden-file policy.
    ItemFactory& itemFactory() { return *itemFactory_; }

    // --- CNID store policy driven by DirectoryItem (the backend is a dumb
    // value store) ---

    // Reconcile a fresh scan of `dirID` against its stored children: a child
    // whose name, kind and identity still match keeps its CNID and Mac name; a
    // child with exactly one occurrence at a new name was renamed (keep the
    // CNID, adopt the name, drop the Mac name); a child with no occurrence is
    // gone; a child with several unmatched occurrences is dropped and each
    // occurrence becomes a new entry. A same-inode record in another directory
    // is never a match. No names are decided. Returns entries in scan order
    // (kept/renamed first).
    std::vector<CNIDRecord> mapDirectoryContents(CNID dirID,
        std::vector<DirEntry> entries);

    // The record for a CNID, or nothing. No host validation is done here; the
    // caller validates (see tryRealize).
    std::optional<CNIDRecord> lookupCNID(CNID cnid);

    // The child of `parID` whose decided Mac name is `macName` (case-folded), or
    // nothing.
    std::optional<CNID> cnidForName(CNID parID, mac_string_view macName);

    // Decide names for one `macNamePrefix` group at once: assign each request a
    // Mac name that does not collide with any name already decided in `parID`.
    // Already-decided names are left untouched.
    void nameGroup(CNID parID, const std::vector<NameRequest>& group);

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
    // For use by DirectoryItem.
    ItemPtr realizeDescriptor(const DirectoryItemPtr& dir, const Descriptor& descriptor);

    // Ensure `item`'s Mac name is decided, deciding its parent directory's group
    // if not. Called from Item::name().
    void ensureNamed(Item& item);

    void deleteItem(ItemPtr item);
    void renameItem(ItemPtr item, mac_string_view newName);
    void moveItem(ItemPtr item, DirectoryItemPtr newParent, mac_string_view newName = {});
};

}
