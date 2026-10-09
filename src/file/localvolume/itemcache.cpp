#include "itemcache.h"
#include "item.h"
#include "cnidmapper.h"
#include "stats.h"
#include <OSUtil.h>
#include <set>
#include <iostream>
#include <algorithm>

using namespace Executor;

ItemCache::ItemCache(fs::path root,
                mac_string_view volumeName,
                std::unique_ptr<CNIDMapper> cnidMapper,
                ItemFactory *itemFactory
                )
    : cnidMapper_(std::move(cnidMapper))
    , itemFactory_(itemFactory)
{
    items_[2] = rootDirItem_ = std::make_shared<DirectoryItem>(*this, 1, 2, root, volumeName);
}

ItemCache::~ItemCache()
{
}

void ItemCache::cleanDirectoryCache()
{
    using namespace std::chrono_literals;
    const auto cacheExpiryTime = 1s;
    const size_t maxCachedDirectories = 20;
    const auto now = std::chrono::steady_clock::now();
    
    while(!cachedDirectories_.empty()
        && (cachedDirectories_.front().timestamp + cacheExpiryTime < now
            || cachedDirectories_.size() > maxCachedDirectories))
    {
        cachedDirectories_.front().directory->clearCache();
        cachedDirectories_.pop_front();
    }
}

void ItemCache::cacheDirectory(DirectoryItemPtr dir)
{
    cleanDirectoryCache();
    if(dir->isCached())
        return;
    cachedDirectories_.push_back({dir, std::chrono::steady_clock::now()});
    localVolumeStats().directoryIterations++;

    std::vector<fs::directory_entry> entries;
    try
    {
        for(const auto& e : fs::directory_iterator(dir->path()))
        {
            if(!itemFactory_->isHidden(e))
                entries.push_back(e);
        }
    }
    catch(const boost::filesystem::filesystem_error& exc)
    {
        std::cerr << exc.what() << std::endl;
        //throw OSErrorException(ioErr);
        // TODO: report this error the right way.
        // throwing here makes listing the *parent* directory fail,
        // which is not what we want.
        // ignoring this error makes inaccessible directories look empty,
        // which is not perfect, either.
    }
    
    localVolumeStats().entriesSeen += entries.size();

    std::vector<CNIDMapper::Mapping> mappings =
        cnidMapper_->mapDirectoryContents(dir->cnid(), std::move(entries));

    std::vector<ItemPtr> items;
    items.reserve(mappings.size());

    for(auto& m : mappings)
    {
        ItemPtr item;

        auto itemIt = items_.find(m.cnid);
        if(itemIt != items_.end())
            item = itemIt->second.lock();

        assert(m.macname.size());
        if(!item)
        {
            item = itemFactory_->createItemForDirEntry(*this, m.parID, m.cnid, m.entry, m.macname);
            if(item)
                localVolumeStats().itemsConstructed++;
        }

        if(!item)
            continue;
        
        items.push_back(item);
        items_.emplace(m.cnid, item);
    }

    dir->populateCache(std::move(items));
}

void ItemCache::flushDirectoryCache(DirectoryItemPtr item)
{
    if(item->isCached())
    {
        item->clearCache();
        cachedDirectories_.remove_if([&](const auto& p) { return p.directory == item; });
    }
}

void ItemCache::flushDirectoryCache(CNID dirID)
{
    auto it = items_.find(dirID);
    if(it != items_.end())
    {
        if(auto item = it->second.lock())
        {
            if(auto dir = std::dynamic_pointer_cast<DirectoryItem>(item))
                flushDirectoryCache(dir);
        }
    }
}

void ItemCache::flushItem(ItemPtr item)
{
    flushDirectoryCache(item->parID());
    if(auto dir = std::dynamic_pointer_cast<DirectoryItem>(item))
        flushDirectoryCache(dir);
    
    auto it = items_.find(item->cnid());
    if(it != items_.end())
    {
        if(auto item2 = it->second.lock())
        {
            if(item2 == item)
            {
                items_.erase(it);
            }
        }
    }
    item->noteItemFlushed();
}

void ItemCache::noteItemFreed(CNID cnid)
{
    items_.erase(cnid);
}

ItemPtr ItemCache::tryResolve(CNID cnid)
{
    {
        auto it = items_.find(cnid);
        if(it != items_.end())
            if(ItemPtr item = it->second.lock())
                return item;
    }

    auto optionalMapping = cnidMapper_->lookupCNID(cnid);
    if(!optionalMapping)
        return {};
    auto& m = *optionalMapping;
    
    if(ItemPtr item = itemFactory_->createItemForDirEntry(*this, m.parID, m.cnid, m.entry, m.macname))
    {
        localVolumeStats().itemsConstructed++;
        items_.emplace(item->cnid(), item);
        return item;
    }
    else
    {
        return {};
    }
}

ItemPtr ItemCache::tryResolve(fs::path inPath)
{
    const auto& rootPath = rootDirItem_->path();
    auto [rootIter, pathIter] = std::mismatch(rootPath.begin(), rootPath.end(), inPath.begin(), inPath.end());

    if(rootIter != rootPath.end())
        return{};

    ItemPtr item = rootDirItem_;
    for(; pathIter != inPath.end(); ++pathIter)
    {
        const auto& elem = *pathIter;

        if(auto dir = std::dynamic_pointer_cast<DirectoryItem>(item))
        {
            cacheDirectory(dir);
            item = dir->tryResolve(elem);
            if(!item)
                return {};
        }
        else
            return {};

    }
    return item;
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

void ItemCache::moveItem(ItemPtr item, DirectoryItemPtr newParent)
{
    flushDirectoryCache(item->parID());
    flushDirectoryCache(newParent->cnid());
    fs::path newPath = newParent->path() / item->path().filename();
    cnidMapper_->moveCNID(item->cnid(), newParent->cnid(), mac_string_view(), 
        [&] {
            item->moveItem(newPath, mac_string_view());
            return item->path();
        });
}
