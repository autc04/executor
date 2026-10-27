#include "itemcache.h"
#include "item.h"
#include "cnidmapper.h"
#include "stats.h"

#include <OSUtil.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <set>

using namespace Executor;

namespace
{
mac_string foldMacName(mac_string_view name)
{
    mac_string folded { name };
    ROMlib_UprString(folded.data(), false, folded.size());
    return folded;
}

std::pair<std::uint64_t, std::uint64_t> inodeKey(const FileIdentity& id)
{
    return { id.device, id.inode };
}

}

ItemCache::ItemCache(fs::path root,
                mac_string_view volumeName,
                std::unique_ptr<CNIDMapper> cnidMapper,
                ItemFactory *itemFactory
                )
    : cnidMapper_(std::move(cnidMapper))
    , itemFactory_(itemFactory)
    , rootPath_(std::move(root))
{
    // The volume root (CNID 2) is just another cached directory: nothing pins it,
    // so it can be evicted and rebuilt like any other. Warm it here so the first
    // access finds it already realized.
    DirectoryItemPtr rootItem = std::make_shared<DirectoryItem>(*this, 1, 2, rootPath_, mac_string(volumeName));
    items_[2] = rootItem;
    cacheItem(rootItem);
}

ItemCache::~ItemCache() = default;

void ItemCache::cacheItem(const ItemPtr& item)
{
    size_t weight = item->cacheWeight();

    auto it = cachedIndex_.find(item.get());
    if(it != cachedIndex_.end())
    {
        // splice moves the node without invalidating its iterator, so the index
        // stays correct.
        auto listIt = it->second;
        cachedWeight_ -= listIt->weight;
        cachedWeight_ += weight;
        listIt->weight = weight;
        cachedItems_.splice(cachedItems_.end(), cachedItems_, listIt);
    }
    else
    {
        auto listIt = cachedItems_.insert(cachedItems_.end(), { item, weight });
        cachedWeight_ += weight;
        cachedIndex_.emplace(item.get(), listIt);
    }

    // Bound the queue's weight, least recently used first. The item just went to
    // the back, so eviction (which takes from the front) cannot select it.
    // Eviction never clears a payload: an item no one else holds dies and takes
    // its payload with it, while one a consumer still holds keeps it -- that is
    // the consumer's memory, not the cache's.
    const size_t maxCachedWeight = localVolumeCacheBudget();
    while(cachedWeight_ > maxCachedWeight && !cachedItems_.empty())
    {
        auto listIt = cachedItems_.begin();
        cachedWeight_ -= listIt->weight;
        cachedIndex_.erase(listIt->item.get());
        cachedItems_.erase(listIt);
    }
}

void ItemCache::flushDirectoryCache(CNID dirID)
{
    // Invalidation by CNID: resolve the live directory and clear its cached
    // payload (clearCache re-caches, so the queue's accounting follows).
    auto it = items_.find(dirID);
    if(it != items_.end())
    {
        if(auto item = it->second.lock())
        {
            if(auto dir = std::dynamic_pointer_cast<DirectoryItem>(item))
                dir->clearCache();
        }
    }
}

void ItemCache::noteItemFreed(CNID cnid)
{
    items_.erase(cnid);
}

std::optional<CNIDRecord> ItemCache::lookupCNID(CNID cnid)
{
    return cnidMapper_->get(cnid);
}

CNID ItemCache::getOrCreateChild(CNID parID, const fs::path& posixName,
    const FileIdentity& identity, bool isDir)
{
    CNID cnid = 0;
    cnidMapper_->withTransaction([&] {
        if(auto existing = cnidMapper_->getChild(parID, posixName))
        {
            // The same host file still holds the name, so reuse its CNID. A
            // different kind, or a different file (the inode changed), is a new
            // item.
            if(existing->isDir == isDir
               && sameIdentity(existing->identity, identity))
            {
                if(!existing->identity.valid && identity.valid)
                {
                    existing->identity = identity;
                    cnidMapper_->put(*existing);
                }
                cnid = existing->cnid;
                return;
            }

            deleteCNIDLocked(existing->cnid);
        }
        // A child index without a record is harmless: putChild below overwrites
        // it.

        cnid = cnidMapper_->allocate();
        CNIDRecord record { parID, cnid, posixName, std::nullopt, identity, isDir };
        cnidMapper_->put(record);
        cnidMapper_->putChild(parID, posixName, cnid);
    });
    return cnid;
}

std::optional<CNID> ItemCache::cnidForName(CNID parID, mac_string_view macName)
{
    return cnidMapper_->getName(parID, foldMacName(macName));
}

std::vector<CNIDRecord> ItemCache::mapDirectoryContents(CNID dirID,
    std::vector<DirEntry> entries)
{
    std::vector<CNIDRecord> result;
    cnidMapper_->withTransaction([&] {
        // The stored child set, in posix-name order. An index entry whose record
        // is gone is an orphan; drop it.
        std::vector<CNIDRecord> stored;
        for(auto& child : cnidMapper_->getChildren(dirID))
        {
            if(auto rec = cnidMapper_->get(child.second))
                stored.push_back(std::move(*rec));
            else
                cnidMapper_->eraseChild(dirID, child.first);
        }

        std::unordered_map<CNID, std::size_t> storedIndex;
        std::map<fs::path, std::size_t> storedByName;
        for(std::size_t i = 0; i < stored.size(); ++i)
        {
            storedIndex[stored[i].cnid] = i;
            storedByName[stored[i].posixName] = i;
        }

        const std::size_t n = entries.size();
        std::vector<bool> consumed(n, false);
        std::set<CNID> surviving;

        // Pass 1: a child whose name, kind and identity still match a scanned
        // entry is unchanged. A child's name is unique, so each entry matches at
        // most one child.
        for(std::size_t i = 0; i < n; ++i)
        {
            const DirEntry& e = entries[i];

            auto it = storedByName.find(e.posixName);
            if(it == storedByName.end())
                continue;

            CNIDRecord& rec = stored[it->second];
            if(rec.isDir != e.isDir || !sameIdentity(rec.identity, e.identity))
                continue;

            // After a --dont-use-inodes session the record has no usable
            // identity; re-adopt one now that the host exposes it. A usable
            // identity is never replaced by an unusable one.
            if(!rec.identity.valid && e.identity.valid)
            {
                rec.identity = e.identity;
                cnidMapper_->put(rec);
            }

            consumed[i] = true;
            surviving.insert(rec.cnid);
        }

        // The inode pairing for rename recovery is scratch, built from the scan.
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::size_t>> byInode;
        for(std::size_t i = 0; i < n; ++i)
            if(entries[i].identity.valid && !consumed[i])
                byInode[inodeKey(entries[i].identity)].push_back(i);

        // Pass 2: a child that was not kept was renamed, split, or is gone.
        // Process in CNID order so the result does not depend on scan order.
        std::vector<CNID> remaining;
        for(auto& rec : stored)
            if(!surviving.count(rec.cnid))
                remaining.push_back(rec.cnid);
        std::sort(remaining.begin(), remaining.end());

        for(CNID cnid : remaining)
        {
            CNIDRecord& rec = stored[storedIndex.at(cnid)];

            std::vector<std::size_t> occurrences;
            if(rec.identity.valid)
                if(auto bit = byInode.find(inodeKey(rec.identity)); bit != byInode.end())
                    for(std::size_t i : bit->second)
                        if(!consumed[i])
                            occurrences.push_back(i);

            if(occurrences.size() == 1 && entries[occurrences[0]].isDir == rec.isDir)
            {
                // Exactly one occurrence at a new name: the entry was renamed.
                std::size_t i = occurrences[0];
                consumed[i] = true;

                forgetRecordName(rec);
                forgetRecordChild(rec);
                rec.macname.reset();   // the Mac name is re-derived
                rec.posixName = entries[i].posixName;
                cnidMapper_->put(rec);
                cnidMapper_->putChild(dirID, rec.posixName, rec.cnid);
                surviving.insert(rec.cnid);
            }
            else
            {
                // No occurrence: gone. Several: the old CNID is dropped and each
                // occurrence becomes a new entry.
                deleteCNIDLocked(cnid);
            }
        }

        std::vector<CNIDRecord> mapped;
        mapped.reserve(n);

        for(auto& rec : stored)
            if(surviving.count(rec.cnid))
                mapped.push_back(rec);

        for(std::size_t i = 0; i < n; ++i)
            if(!consumed[i])
            {
                CNID cnid = cnidMapper_->allocate();
                CNIDRecord record { dirID, cnid, entries[i].posixName, std::nullopt,
                    entries[i].identity, entries[i].isDir };
                cnidMapper_->put(record);
                cnidMapper_->putChild(dirID, record.posixName, cnid);
                mapped.push_back(std::move(record));
            }

        result = std::move(mapped);
    });
    return result;
}

void ItemCache::nameGroup(CNID parID, const std::vector<NameRequest>& group)
{
    cnidMapper_->withTransaction([&] {
        // Names can only collide within one prefix group (two names with
        // different folded prefixes cannot be equal), so the collision set is
        // exactly the group's already-decided members. Seeding only those keeps
        // naming linear.
        std::set<mac_string> used;
        for(const NameRequest& req : group)
            if(auto rec = cnidMapper_->get(req.cnid); rec && rec->macname)
                used.insert(foldMacName(*rec->macname));

        for(const NameRequest& req : group)
        {
            auto rec = cnidMapper_->get(req.cnid);
            if(!rec || rec->parID != parID)
                continue;
            if(rec->posixName != req.posixName)
                continue;   // the binding changed after the listing
            if(rec->macname)
                continue;   // already decided; names are sticky

            // Stop at the last index whose suffix fits the prefix bound. If every
            // index is taken, the entry cannot be named without minting a name
            // that breaks the bound, so it stays unnamed. That makes it invisible:
            // no name lookup can reach it, and indexed enumeration reads the name
            // of each entry it returns, so it stops (fnfErr) before this one.
            for(int index = 0; index <= maxDisambiguatorIndex; index++)
            {
                mac_string candidate = toMacRomanFilename(req.nameSource, index);
                mac_string folded = foldMacName(candidate);
                if(used.insert(folded).second)
                {
                    rec->macname = std::move(candidate);
                    cnidMapper_->put(*rec);
                    cnidMapper_->putName(parID, folded, req.cnid);
                    break;
                }
            }
        }
    });
}

void ItemCache::forgetRecordName(const CNIDRecord& record)
{
    if(!record.macname)
        return;

    mac_string folded = foldMacName(*record.macname);
    if(auto existing = cnidMapper_->getName(record.parID, folded);
       existing && *existing == record.cnid)
        cnidMapper_->eraseName(record.parID, folded);
}

void ItemCache::forgetRecordChild(const CNIDRecord& record)
{
    if(auto existing = cnidMapper_->getChild(record.parID, record.posixName);
       existing && existing->cnid == record.cnid)
        cnidMapper_->eraseChild(record.parID, record.posixName);
}

void ItemCache::deleteCNID(CNID cnid)
{
    cnidMapper_->withTransaction([&] {
        deleteCNIDLocked(cnid);
    });
}

void ItemCache::deleteCNIDLocked(CNID cnid)
{
    auto record = cnidMapper_->get(cnid);
    if(!record)
        return;

    forgetRecordName(*record);
    forgetRecordChild(*record);

    // Collect the children before mutating, so deleting from the index while
    // iterating it is not an issue.
    auto children = cnidMapper_->getChildren(cnid);
    cnidMapper_->erase(cnid);

    for(auto& child : children)
    {
        cnidMapper_->eraseChild(cnid, child.first);
        deleteCNIDLocked(child.second);
    }
}

void ItemCache::moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName,
    const fs::path& newPosixName)
{
    cnidMapper_->withTransaction([&] {
        auto rec = cnidMapper_->get(cnid);
        if(!rec)
            return;

        const CNID oldParent = rec->parID;
        const CNID targetParent = newParent == 0 ? oldParent : newParent;
        if(oldParent == targetParent && newMacName.empty()
           && newPosixName == rec->posixName)
            return;

        // Both name maps are keyed by parent, so a move re-homes the entries.
        forgetRecordName(*rec);
        forgetRecordChild(*rec);

        rec->parID = targetParent;
        rec->posixName = newPosixName;
        if(!newMacName.empty())
            rec->macname = mac_string(newMacName.data(), newMacName.size());

        cnidMapper_->put(*rec);
        cnidMapper_->putChild(targetParent, rec->posixName, cnid);
        if(rec->macname)
            cnidMapper_->putName(targetParent, foldMacName(*rec->macname), cnid);
    });
}

ItemPtr ItemCache::findRealizedItem(CNID cnid) const
{
    auto it = items_.find(cnid);
    if(it == items_.end())
        return {};
    return it->second.lock();
}

ItemPtr ItemCache::findAndRefreshRealizedItem(CNID cnid, const fs::path& path, bool isDir)
{
    auto it = items_.find(cnid);
    if(it == items_.end())
        return {};

    ItemPtr item = it->second.lock();
    if(!item)
        return {};

    bool itemIsDir = std::dynamic_pointer_cast<DirectoryItem>(item) != nullptr;
    if(itemIsDir != isDir)
    {
        items_.erase(it);
        return {};
    }

    item->relocate(path);   // a recovered rename re-points the live Item

    // Handing an item out -- and re-adopting one that outlived eviction -- counts
    // as use, so the queue is ordered by recency of access, not of creation.
    cacheItem(item);
    return item;
}

fs::path ItemCache::pathOf(CNID cnid)
{
    std::vector<fs::path> names;

    for(CNID c = cnid; c != 2;)
    {
        auto rec = cnidMapper_->get(c);
        if(!rec)
            return {};

        names.push_back(rec->posixName);
        CNID parent = rec->parID;
        if(parent == c)   // broken chain; bail out rather than loop
            return {};
        c = parent;
    }

    fs::path path = rootPath_;
    for(auto n = names.rbegin(); n != names.rend(); ++n)
        path /= *n;
    return path;
}

ItemPtr ItemCache::tryRealize(CNID cnid)
{
    if(cnid <= 0)
        return {};

    auto mapping = lookupCNID(cnid);
    if(!mapping)
        return {};

    fs::path path = pathOf(cnid);
    if(path.empty())
        return {};

    PathStat st = statPath(path);
    // The binding is gone, the name now holds a different kind of file, or a
    // different file holds the name (a delete + create). Deleting is the diff's
    // job, not ours: see realizeByCNID.
    if(!st.exists || (!st.isDirectory && !st.isRegular)
       || st.isDirectory != mapping->isDir
       || !sameIdentity(mapping->identity, st.identity))
        return {};

    if(ItemPtr item = findAndRefreshRealizedItem(cnid, path, st.isDirectory))
        return item;

    return makeItem(mapping->parID, cnid, path, mapping->macname, st.isDirectory);
}

ItemPtr ItemCache::realizeByCNID(CNID cnid)
{
    if(ItemPtr item = tryRealize(cnid))
        return item;

    auto mapping = lookupCNID(cnid);
    if(!mapping)
        return {};

    // The binding is stale and the file may have moved: re-list the parent so the
    // diff can recover a same-directory rename (or drop a gone entry), then retry.
    // The parent's own binding may be stale too, so realize it first; the walk stops
    // as soon as an ancestor's binding holds, or its parID has no record. It
    // terminates because a parent is always CNIDResolved before its children, so
    // parID is nearer the root than cnid.
    auto parent = std::dynamic_pointer_cast<DirectoryItem>(realizeByCNID(mapping->parID));
    if(!parent)
        return {};

    parent->clearCache();
    parent->ensureListed();

    return tryRealize(cnid);
}

ItemPtr ItemCache::realizeByHostPath(fs::path inPath)
{
    auto [rootIter, pathIter] = std::mismatch(rootPath_.begin(), rootPath_.end(), inPath.begin(), inPath.end());

    if(rootIter != rootPath_.end())
        return{};

    // The root is realized on demand like any other item; nothing pins it.
    ItemPtr item = realizeByCNID(2);
    if(!item)
        return{};

    for(; pathIter != inPath.end(); ++pathIter)
    {
        auto dir = std::dynamic_pointer_cast<DirectoryItem>(item);
        if(!dir)
            return {};

        const fs::path& elem = *pathIter;
        fs::path childPath = dir->path() / elem;

        // Stat-first: descend by host identity, never enumerating the parent.
        PathStat st = statPath(childPath);
        if(!st.exists || (!st.isDirectory && !st.isRegular))
            return {};

        // Match the listing's visibility rule: a sidecar filtered out of the
        // listing is not addressable by host path either.
        if(itemFactory().isHidden(fs::directory_entry(childPath)))
            return {};

        CNID cnid = resolveChildCNID(dir->cnid(), elem, st);
        item = realizeByCNID(cnid);
        if(!item)
            return {};
    }
    return item;
}

ItemPtr ItemCache::realizeDescriptor(const DirectoryItemPtr& dir, const Descriptor& d)
{
    fs::path path = dir->path() / d.posixName;
    if(ItemPtr item = findAndRefreshRealizedItem(d.cnid, path, d.isDir))
        return item;

    return makeItem(dir->cnid(), d.cnid, path, std::nullopt, d.isDir);
}

CNID ItemCache::resolveChildCNID(CNID parID, const fs::path& posixName, const PathStat& st)
{
    // One atomic get-or-create in the mapper: a lookup followed by a create would
    // let two instances mint two CNIDs for the same binding.
    return getOrCreateChild(parID, posixName, st.identity, st.isDirectory);
}

void ItemCache::ensureNamed(Item& item)
{
    if(item.nameIsDecided())
        return;

    if(auto rec = lookupCNID(item.cnid()); rec && rec->macname)
    {
        item.decideName(*rec->macname);
        return;
    }

    auto dir = std::dynamic_pointer_cast<DirectoryItem>(realizeByCNID(item.parID()));
    if(!dir)
    {
        if(!lookupCNID(item.parID()))
            throw OSErrorException(fnfErr);
        throw OSErrorException(ioErr);
    }

    if(auto name = dir->resolveName(item))
        item.decideName(*name);
    else
        throw OSErrorException(fnfErr);
}

ItemPtr ItemCache::makeItem(CNID parID, CNID cnid, const fs::path& path,
    std::optional<mac_string> name, bool isDir)
{
    ItemPtr item;

    if(isDir)
        item = std::make_shared<DirectoryItem>(*this, parID, cnid, path, std::move(name));
    else
        item = std::make_shared<FileItem>(*this, parID, cnid, path, std::move(name));

    localVolumeStats().itemsConstructed++;

    // Every realized item enters the queue, so a file's probed representation
    // (its FileRep) and a directory's listing both survive between accesses.
    cacheItem(item);

    items_[cnid] = item;
    return item;
}

void ItemCache::deleteItem(ItemPtr item)
{
    item->deleteItem();

    // The binding is gone, so the LRU must not keep the deleted Item (and its
    // payload) alive until normal eviction; open holders still own it.
    auto cached = cachedIndex_.find(item.get());
    if(cached != cachedIndex_.end())
    {
        cachedWeight_ -= cached->second->weight;
        cachedItems_.erase(cached->second);
        cachedIndex_.erase(cached);
    }

    items_.erase(item->cnid());
    deleteCNID(item->cnid());
    flushDirectoryCache(item->parID());
}

void ItemCache::renameItem(ItemPtr item, mac_string_view newName)
{
    fs::path newPath = item->path().parent_path() / item->hostName(newName);
    flushDirectoryCache(item->parID());

    // Host I/O first, then the mapper: the two cannot be made atomic, and the
    // inode diff converges if they tear (see Phase 11).
    item->moveItem(newPath, item->parID(), newName);
    moveCNID(item->cnid(), 0, newName, newPath.filename());
}

void ItemCache::moveItem(ItemPtr item, DirectoryItemPtr newParent, mac_string_view newName)
{
    flushDirectoryCache(item->parID());
    newParent->clearCache();

    fs::path newPath = newParent->path();
    if(newName.empty())
        newPath /= item->path().filename();   // already encoded for its rep
    else
        newPath /= item->hostName(newName);

    // Host I/O first, then the mapper; a tear is reconciled by the inode diff.
    item->moveItem(newPath, newParent->cnid(), newName);
    moveCNID(item->cnid(), newParent->cnid(), newName, newPath.filename());
}
