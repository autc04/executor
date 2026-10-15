#include "itemcache.h"
#include "item.h"
#include "cnidmapper.h"
#include "stats.h"
#include <algorithm>

using namespace Executor;

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

ItemPtr ItemCache::cachedItem(CNID cnid) const
{
    auto it = items_.find(cnid);
    if(it == items_.end())
        return {};
    return it->second.lock();
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

ItemPtr ItemCache::itemSnapshot(CNID cnid, const fs::path& path, bool isDir)
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
        auto rec = cnidMapper_->lookupCNID(c);
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

ItemPtr ItemCache::realizeOnce(CNID cnid)
{
    if(cnid <= 0)
        return {};

    auto mapping = cnidMapper_->lookupCNID(cnid);
    if(!mapping)
        return {};

    fs::path path = pathOf(cnid);
    if(path.empty())
        return {};

    PathStat st = statPath(path);
    // The binding is gone, the name now holds a different kind of file, or a
    // different file holds the name (a delete + create). Deleting is the diff's
    // job, not ours: see realizeValidated.
    if(!st.exists || (!st.isDirectory && !st.isRegular)
       || st.isDirectory != mapping->isDir
       || !sameIdentity(mapping->identity, st.identity))
        return {};

    if(ItemPtr item = itemSnapshot(cnid, path, st.isDirectory))
        return item;

    return makeItem(mapping->parID, cnid, path, mapping->macname, st.isDirectory);
}

ItemPtr ItemCache::realizeValidated(CNID cnid)
{
    return realizeValidated(cnid, 32);
}

ItemPtr ItemCache::realizeValidated(CNID cnid, int depth)
{
    if(ItemPtr item = realizeOnce(cnid))
        return item;

    auto mapping = cnidMapper_->lookupCNID(cnid);
    if(!mapping || depth <= 0)
        return {};

    // The binding is stale and the file may have moved: re-list the parent so the
    // diff can recover a same-directory rename (or drop a gone entry), then retry.
    auto parent = std::dynamic_pointer_cast<DirectoryItem>(realizeValidated(mapping->parID, depth - 1));
    if(!parent)
        return {};

    parent->clearCache();
    parent->ensureListed();

    return realizeOnce(cnid);
}

ItemPtr ItemCache::tryResolve(CNID cnid)
{
    return realizeValidated(cnid);
}

ItemPtr ItemCache::tryResolve(fs::path inPath)
{
    auto [rootIter, pathIter] = std::mismatch(rootPath_.begin(), rootPath_.end(), inPath.begin(), inPath.end());

    if(rootIter != rootPath_.end())
        return{};

    // The root is realized on demand like any other item; nothing pins it.
    ItemPtr item = realizeValidated(2);
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

        CNID cnid = resolveChild(dir->cnid(), elem, st);
        item = realizeValidated(cnid);
        if(!item)
            return {};
    }
    return item;
}

ItemPtr ItemCache::realizeChild(const DirectoryItemPtr& dir, const Descriptor& d)
{
    fs::path path = dir->path() / d.posixName;
    if(ItemPtr item = itemSnapshot(d.cnid, path, d.isDir))
        return item;

    return makeItem(dir->cnid(), d.cnid, path, std::nullopt, d.isDir);
}

CNID ItemCache::resolveChild(CNID parID, const fs::path& posixName, const PathStat& st)
{
    if(auto mapping = cnidMapper_->lookupPosixName(parID, posixName))
    {
        // The same host file still holds the name, so reuse its CNID. A different
        // kind, or a different file (the inode changed), is a new item.
        if(mapping->isDir == st.isDirectory && sameIdentity(mapping->identity, st.identity))
            return mapping->cnid;

        cnidMapper_->deleteCNID(mapping->cnid);
    }

    return cnidMapper_->createRecord(parID, posixName, st.identity, st.isDirectory);
}

void ItemCache::ensureNamed(Item& item)
{
    if(item.nameIsDecided())
        return;

    if(auto rec = cnidMapper_->lookupCNID(item.cnid()); rec && rec->macname)
    {
        item.decideName(*rec->macname);
        return;
    }

    auto dir = std::dynamic_pointer_cast<DirectoryItem>(realizeValidated(item.parID()));
    if(!dir)
        return;

    if(auto name = dir->resolveName(item))
        item.decideName(*name);
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

void ItemCache::materialize(FileItem& item)
{
    if(item.rep())
        return;

    item.setRep(itemFactory_->createRepForFile(*this, item, fs::directory_entry(item.path())));
}

void ItemCache::deleteItem(ItemPtr item)
{
    item->deleteItem();
    
    items_.erase(item->cnid());
    cnidMapper_->deleteCNID(item->cnid());
    flushDirectoryCache(item->parID());
}

void ItemCache::renameItem(ItemPtr item, mac_string_view newName)
{
    fs::path newPath = item->path().parent_path() / toUnicodeFilename(newName);
    flushDirectoryCache(item->parID());
    cnidMapper_->moveCNID(item->cnid(), 0, newName, 
        [&] {
            item->moveItem(newPath, newName);
            return item->path();
        });

}

void ItemCache::moveItem(ItemPtr item, DirectoryItemPtr newParent, mac_string_view newName)
{
    flushDirectoryCache(item->parID());
    newParent->clearCache();

    fs::path newPath = newParent->path();
    if(newName.empty())
        newPath /= item->path().filename();
    else
        newPath /= toUnicodeFilename(newName);

    cnidMapper_->moveCNID(item->cnid(), newParent->cnid(), newName, 
        [&] {
            item->moveItem(newPath, newName);
            return item->path();
        });
}
