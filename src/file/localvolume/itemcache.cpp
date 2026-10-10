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
{
    items_[2] = rootDirItem_ = std::make_shared<DirectoryItem>(*this, 1, 2, root, volumeName);
}

ItemCache::~ItemCache()
{
}

void ItemCache::clearCachedDirectory(const DirectoryItemPtr& dir)
{
    if(dir->isCached())
        cachedItemCount_ -= (size_t)dir->countItems();
    dir->clearCache();
}

void ItemCache::evictCachedDirectories(const DirectoryItemPtr& protect)
{
    // Memory-only policy: bound the total number of enumerated items. The root
    // and the directory currently in use are never evicted; if they alone exceed
    // the budget, we simply keep them.
    const size_t maxCachedItems = localVolumeItemBudget();
    while(cachedItemCount_ > maxCachedItems)
    {
        auto it = std::find_if(cachedDirectories_.begin(), cachedDirectories_.end(),
            [&](const auto& p) {
                return p.directory != rootDirItem_ && p.directory != protect;
            });
        if(it == cachedDirectories_.end())
            break;

        clearCachedDirectory(it->directory);
        cachedDirectories_.erase(it);
    }
}

void ItemCache::touchCachedDirectory(const DirectoryItemPtr& dir)
{
    cachedDirectories_.remove_if([&](const auto& p) { return p.directory == dir; });
    cachedDirectories_.push_back({dir});
}

bool ItemCache::ensureEnumerated(const DirectoryItemPtr& dir)
{
    FileStamp stamp = statFile(dir->path());
    if(dir->isCached() && dir->stamp_ == stamp)
        return true;   // L0: the stamp is unchanged, so everything is still valid

    touchCachedDirectory(dir);

    // L1: re-scan. Re-stat after the readdir and only accept a *consistent*
    // (before == after) stamp, so a change racing our own scan is caught.
    bool ok = true;
    for(int attempt = 0; attempt < 3; attempt++)
    {
        std::vector<CNIDMapper::DirEntry> entries;
        try
        {
            for(const auto& e : fs::directory_iterator(dir->path()))
                if(!itemFactory_->isHidden(e))
                    entries.push_back({ e, nameSourceFor(e.path()) });
        }
        catch(const boost::filesystem::filesystem_error& exc)
        {
            std::cerr << exc.what() << std::endl;
            ok = false;
            // An unreadable directory looks empty rather than failing the
            // caller here; the caller decides whether this directory was the
            // one requested (see LocalVolume::resolveForInfo) or incidental.
        }

        FileStamp after = statFile(dir->path());
        if(after == stamp || attempt == 2)
        {
            localVolumeStats().directoryIterations++;
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
                {
                    item = itemIt->second.lock();

                    // A kind change (file <-> directory at the same name) keeps
                    // the mapping but must rebuild the Item as the new type.
                    if(item)
                    {
                        bool wantDir = fs::is_directory(m.entry.path());
                        bool isDir = std::dynamic_pointer_cast<DirectoryItem>(item) != nullptr;
                        if(wantDir != isDir)
                        {
                            items_.erase(itemIt);
                            item.reset();
                        }
                    }
                }

                assert(m.macname.size());
                if(!item)
                    item = makeItem(m.parID, m.cnid, m.entry, m.macname);

                if(!item)
                    continue;

                items.push_back(item);
                items_.emplace(m.cnid, item);
            }

            if(dir->isCached())
                cachedItemCount_ -= (size_t)dir->countItems();
            dir->populateCache(std::move(items), after);
            cachedItemCount_ += (size_t)dir->countItems();
            evictCachedDirectories(dir);
            return ok;
        }

        stamp = after;
    }

    return false; // unreachable
}

int ItemCache::countChildren(const DirectoryItemPtr& dir)
{
    // Enumerate implies Count.
    if(dir->isCached())
        return dir->countItems();

    for(int attempt = 0; attempt < 3; attempt++)
    {
        FileStamp before = statFile(dir->path());
        if(dir->count_ && dir->countStamp_ == before)
            return *dir->count_;

        int count = 0;
        localVolumeStats().countScans++;
        try
        {
            for(const auto& e : fs::directory_iterator(dir->path()))
                if(!itemFactory_->isHidden(e))
                    ++count;
        }
        catch(const boost::filesystem::filesystem_error&)
        {
            // Same policy as ensureEnumerated: report what we saw rather than
            // fail the caller.
        }

        FileStamp after = statFile(dir->path());
        if(after == before || attempt == 2)
        {
            dir->count_ = count;
            dir->countStamp_ = after;
            return count;
        }
    }
    return 0; // unreachable
}

void ItemCache::flushDirectoryCache(DirectoryItemPtr item)
{
    if(item->isCached())
    {
        clearCachedDirectory(item);
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
    
    if(ItemPtr item = makeItem(m.parID, m.cnid, m.entry, m.macname))
    {
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
        auto dir = std::dynamic_pointer_cast<DirectoryItem>(item);
        if(!dir)
            return {};

        const fs::path& elem = *pathIter;
        fs::path childPath = dir->path() / elem;

        if(!fs::exists(childPath))
            return {};

        // Resolve the child by (parent CNID, filename) instead of enumerating
        // the whole parent directory.
        auto mapping = cnidMapper_->lookupChild(dir->cnid(), elem);
        if(!mapping)
            mapping = cnidMapper_->getOrCreateChild(dir->cnid(), childPath, elem, nameSourceFor(childPath));

        item = itemForMapping(*mapping);
        if(!item)
            return {};
    }
    return item;
}

ItemPtr ItemCache::itemForMapping(const CNIDMapper::Mapping& mapping)
{
    auto it = items_.find(mapping.cnid);
    if(it != items_.end())
        if(ItemPtr item = it->second.lock())
            return item;

    ItemPtr item = makeItem(mapping.parID, mapping.cnid, mapping.entry, mapping.macname);
    if(item)
        items_.emplace(mapping.cnid, item);
    return item;
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
    // enumeration does not run the format gauntlet for every file.
    FileItem probe(*this, 0, 0, path);
    FileRepPtr rep = itemFactory_->createRepForFile(*this, probe, fs::directory_entry(path));
    return rep ? rep->nameSource(filename) : filename;
}

ItemPtr ItemCache::makeItem(CNID parID, CNID cnid, const fs::directory_entry& e, mac_string_view name)
{
    ItemPtr item;

    if(fs::is_directory(e.path()))
        item = std::make_shared<DirectoryItem>(*this, parID, cnid, e.path(), name);
    else if(fs::is_regular_file(e.path()))
        item = std::make_shared<FileItem>(*this, parID, cnid, e.path(), name);
    else
        return {};

    localVolumeStats().itemsConstructed++;
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
    flushDirectoryCache(newParent->cnid());

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
