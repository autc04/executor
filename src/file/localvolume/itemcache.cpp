#include "itemcache.h"
#include "item.h"
#include "cnidmapper.h"
#include "stats.h"
#include <OSUtil.h>
#include <set>
#include <iostream>
#include <algorithm>
#include <cctype>

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

ItemCache::~ItemCache()
{
}

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

int ItemCache::countVisible(const fs::path& dir) const
{
    int count = 0;
    localVolumeStats().countScans++;
    try
    {
        for(const auto& e : fs::directory_iterator(dir))
            if(!itemFactory_->isHidden(e))
                ++count;
    }
    catch(const boost::filesystem::filesystem_error& exc)
    {
        // A failed scan is not a tally, so report it rather than caching a wrong
        // count as valid.
        std::cerr << exc.what() << std::endl;
        throw OSErrorException(ioErr);
    }
    return count;
}

std::optional<std::vector<Descriptor>> ItemCache::listDirectory(CNID dirID, const fs::path& dir)
{
    // Re-scan in two passes. The readdir runs to completion before any entry is
    // stat'ed, so the directory stream never spans the per-entry stats: on a
    // directory that needs more than one getdents fill, that is the difference
    // between a cursor that is live for the whole loop and one whose fills are
    // back to back. The caller keys the result to the stamp it read *before*
    // calling us, so a change arriving while we scan shows up as a mismatch on the
    // next access and re-lists, instead of this listing claiming to describe a
    // state it never observed. Concurrent modification is otherwise unguarded
    // (see *Future Ideas*).
    std::vector<CNIDMapper::DirEntry> entries;
    try
    {
        for(const auto& e : fs::directory_iterator(dir))
        {
            if(itemFactory_->isHidden(e))
                continue;

            // Identity and kind are filled in by the second pass.
            entries.push_back({ e.path().filename(), {}, false });
        }
    }
    catch(const boost::filesystem::filesystem_error& exc)
    {
        // A failed scan says nothing about the directory's contents, so it must
        // not be committed: the diff would read the (empty) result as every child
        // having gone and renumber the whole directory. Report it instead -- the
        // caller drops its listing and the next access retries from scratch.
        std::cerr << exc.what() << std::endl;
        return std::nullopt;
    }

    for(auto& entry : entries)
    {
        PathStat st = statPath(dir / entry.posixName);
        if(!st.exists || (!st.isDirectory && !st.isRegular))
        {
            entry.posixName.clear();   // not addressable; dropped below
            continue;
        }

        entry.identity = st.identity;
        entry.isDir = st.isDirectory;
    }

    // readdir never yields an empty name, so an empty one marks a dropped entry.
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [](const CNIDMapper::DirEntry& e) { return e.posixName.empty(); }),
        entries.end());

    localVolumeStats().directoryIterations++;
    localVolumeStats().entriesSeen += entries.size();

    std::vector<CNIDMapper::Mapping> mappings =
        cnidMapper_->mapDirectoryContents(dirID, std::move(entries));

    std::vector<Descriptor> descriptors;
    descriptors.reserve(mappings.size());

    for(auto& m : mappings)
    {
        // The diff may have recovered a rename (or dropped the entry), so re-point
        // any live Item at its current path before anything can use the stale one:
        // a live Item is otherwise only relocated when it is handed out, which
        // leaves the invariant unenforced between a listing and the next hand-out.
        // relocate() also clears a cached Mac name when the name changed, matching
        // the record's reset.
        if(auto itemIt = items_.find(m.cnid); itemIt != items_.end())
            if(ItemPtr item = itemIt->second.lock())
                item->relocate(dir / m.posixName);

        Descriptor d;
        d.posixName = m.posixName;
        d.isDir = m.isDir;
        d.cnid = m.cnid;
        d.prefix = hostGroupPrefix(d.posixName);
        descriptors.push_back(std::move(d));
    }

    std::sort(descriptors.begin(), descriptors.end(),
        [](const Descriptor& a, const Descriptor& b) {
            if(a.prefix != b.prefix)
                return a.prefix < b.prefix;
            return a.posixName.native() < b.posixName.native();
        });

    return descriptors;
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

std::optional<mac_string> ItemCache::decidedName(CNID cnid) const
{
    if(auto rec = cnidMapper_->lookupCNID(cnid); rec && rec->macname)
        return *rec->macname;
    return std::nullopt;
}

void ItemCache::decideNameGroup(CNID dirID, const std::vector<CNIDMapper::NameRequest>& group)
{
    cnidMapper_->nameGroup(dirID, group);
}

std::optional<CNID> ItemCache::childNamed(CNID dirID, mac_string_view macName) const
{
    return cnidMapper_->cnidForName(dirID, macName);
}

fs::path ItemCache::nameSourceFor(const fs::path& path)
{
    fs::path filename = path.filename();

    std::string ext = filename.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if(ext != ".bin")
        return filename;

    // Only ".bin" files can be MacBinary (and so rename); probe just those, so
    // naming does not run the format gauntlet for every file.
    localVolumeStats().nameProbes++;
    FileItem probe(*this, 0, 0, path);
    FileRepPtr rep = itemFactory_->createRepForFile(*this, probe, fs::directory_entry(path));
    return rep ? rep->nameSource(filename) : filename;
}

void ItemCache::ensureNamed(Item& item)
{
    if(item.nameIsDecided())
        return;

    if(auto name = decidedName(item.cnid()))
    {
        item.decideName(*name);
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
