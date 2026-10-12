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
    retainDirectory(rootDirItem_);
}

ItemCache::~ItemCache()
{
}

size_t ItemCache::chargeOf(const DirectoryItemPtr& dir) const
{
    return dir->isListed() ? std::max<size_t>(1, (size_t)dir->countItems()) : 1;
}

void ItemCache::clearCachedDirectory(const DirectoryItemPtr& dir)
{
    dir->clearCache();
}

void ItemCache::retainDirectory(const DirectoryItemPtr& dir)
{
    cachedDirectories_.remove_if([&](const CachedDirectory& p) {
        if(p.directory == dir)
        {
            cachedItemCount_ -= p.charge;
            return true;
        }
        return false;
    });
    size_t charge = chargeOf(dir);
    cachedDirectories_.push_back({ dir, charge });
    cachedItemCount_ += charge;
}

void ItemCache::evictCachedDirectories(const DirectoryItemPtr& protect)
{
    // Memory-only policy: bound the retained charge. The directory in use is
    // never evicted; if it alone exceeds the budget, we simply keep it.
    const size_t maxCachedItems = localVolumeItemBudget();
    while(cachedItemCount_ > maxCachedItems)
    {
        auto it = std::find_if(cachedDirectories_.begin(), cachedDirectories_.end(),
            [&](const auto& p) { return p.directory != protect; });
        if(it == cachedDirectories_.end())
            break;

        clearCachedDirectory(it->directory);
        cachedItemCount_ -= it->charge;
        cachedDirectories_.erase(it);
    }
}

// The grouping/sort key for a Mac name: case-folded, without any trailing
// disambiguator.  (Phase 7 derives it from the decided name; Phase 9 will derive
// it lexically from the host name, before naming.)
static mac_string namePrefix(mac_string_view name)
{
    mac_string s { name };
    size_t hash = s.rfind((unsigned char)'#');
    if(hash != mac_string::npos && hash + 1 < s.size())
    {
        bool digits = true;
        for(size_t i = hash + 1; i < s.size(); i++)
            if(s[i] < (unsigned char)'0' || s[i] > (unsigned char)'9')
            {
                digits = false;
                break;
            }
        if(digits)
            s.resize(hash);
    }
    ROMlib_UprString(s.data(), false, s.size());
    return s;
}

bool ItemCache::ensureListed(const DirectoryItemPtr& dir)
{
    FileStamp stamp = statFile(dir->path());
    if(dir->isListed() && dir->stamp_ == stamp)
        return true;   // L0: the stamp is unchanged, so everything is still valid

    // L1: re-scan. Re-stat after the readdir and only accept a *consistent*
    // (before == after) stamp, so a change racing our own scan is caught.
    bool ok = true;
    for(int attempt = 0; attempt < 3; attempt++)
    {
        std::vector<CNIDMapper::DirEntry> entries;
        try
        {
            for(const auto& e : fs::directory_iterator(dir->path()))
            {
                if(itemFactory_->isHidden(e))
                    continue;

                PathStat st = statPath(e.path());
                if(!st.exists || (!st.isDirectory && !st.isRegular))
                    continue;   // not something the File Manager can address

                entries.push_back({ e.path(), nameSourceFor(e.path()), st.identity,
                                    st.stamp.mtime, st.isDirectory });
            }
        }
        catch(const boost::filesystem::filesystem_error& exc)
        {
            std::cerr << exc.what() << std::endl;
            ok = false;
            // Fill the listing with what we saw (nothing) and report the failure
            // through the return value; child() turns it into ioErr, since the
            // directory being listed there is the one being queried.
        }

        FileStamp after = statFile(dir->path());
        if(after == stamp || attempt == 2)
        {
            localVolumeStats().directoryIterations++;
            localVolumeStats().entriesSeen += entries.size();

            std::vector<CNIDMapper::Mapping> mappings =
                cnidMapper_->mapDirectoryContents(dir->cnid(), std::move(entries));

            std::vector<Descriptor> descriptors;
            descriptors.reserve(mappings.size());

            for(auto& m : mappings)
            {
                assert(m.macname.size());
                Descriptor d;
                d.posixName = m.path.filename();
                d.isDir = m.isDir;
                d.cnid = m.cnid;
                d.macName = m.macname;
                d.prefix = namePrefix(m.macname);
                descriptors.push_back(std::move(d));
            }

            std::sort(descriptors.begin(), descriptors.end(),
                [](const Descriptor& a, const Descriptor& b) {
                    if(a.prefix != b.prefix)
                        return a.prefix < b.prefix;
                    return a.posixName.native() < b.posixName.native();
                });

            dir->storeDescriptors(std::move(descriptors), after);
            retainDirectory(dir);
            evictCachedDirectories(dir);
            return ok;
        }

        stamp = after;
    }

    return false; // unreachable
}

int ItemCache::countChildren(const DirectoryItemPtr& dir)
{
    // List implies Count.
    if(dir->isListed())
        return dir->countItems();

    for(int attempt = 0; attempt < 3; attempt++)
    {
        FileStamp before = statFile(dir->path());
        if(dir->count_ && dir->countStamp_ == before)
        {
            retainDirectory(dir);
            evictCachedDirectories(dir);
            return *dir->count_;
        }

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
            // Same policy as ensureListed: report what we saw rather than
            // fail the caller.
        }

        FileStamp after = statFile(dir->path());
        if(after == before || attempt == 2)
        {
            dir->count_ = count;
            dir->countStamp_ = after;
            retainDirectory(dir);
            evictCachedDirectories(dir);
            return count;
        }
    }
    return 0; // unreachable
}

void ItemCache::flushDirectoryCache(DirectoryItemPtr item)
{
    clearCachedDirectory(item);
    cachedDirectories_.remove_if([&](const CachedDirectory& p) {
        if(p.directory == item)
        {
            cachedItemCount_ -= p.charge;
            return true;
        }
        return false;
    });
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

ItemPtr ItemCache::itemSnapshot(CNID cnid, bool isDir)
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
    return item;
}

ItemPtr ItemCache::realizeValidated(CNID cnid)
{
    if(cnid <= 0)
        return {};

    auto mapping = cnidMapper_->lookupCNID(cnid);
    if(!mapping)
        return {};

    PathStat st = statPath(mapping->path);
    // The binding is gone, the name now holds a different kind of file, or a
    // different file holds the name (a delete + create).
    if(!st.exists || (!st.isDirectory && !st.isRegular)
       || st.isDirectory != mapping->isDir
       || !sameIdentity(mapping->identity, st.identity))
    {
        cnidMapper_->deleteCNID(cnid);
        items_.erase(cnid);
        return {};
    }

    if(ItemPtr item = itemSnapshot(cnid, st.isDirectory))
        return item;

    return makeItem(mapping->parID, cnid, mapping->path, mapping->macname, st.isDirectory);
}

ItemPtr ItemCache::tryResolve(CNID cnid)
{
    return realizeValidated(cnid);
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

        // Stat-first: descend by host identity, never enumerating the parent.
        PathStat st = statPath(childPath);
        if(!st.exists || (!st.isDirectory && !st.isRegular))
            return {};

        CNID cnid = cnidMapper_->resolveChild(dir->cnid(), childPath, nameSourceFor(childPath),
            st.identity, st.isDirectory, st.stamp.mtime);
        item = realizeValidated(cnid);
        if(!item)
            return {};
    }
    return item;
}

ItemPtr ItemCache::realizeChild(const DirectoryItemPtr& dir, const Descriptor& d)
{
    if(ItemPtr item = itemSnapshot(d.cnid, d.isDir))
        return item;

    return makeItem(dir->cnid(), d.cnid, dir->path() / d.posixName, d.macName, d.isDir);
}

ItemPtr ItemCache::child(const DirectoryItemPtr& dir, mac_string_view name)
{
    if(!ensureListed(dir))
        throw OSErrorException(ioErr);
    const Descriptor* d = dir->childByName(name);
    return d ? realizeChild(dir, *d) : ItemPtr {};
}

ItemPtr ItemCache::child(const DirectoryItemPtr& dir, int index, bool includeDirectories)
{
    if(!ensureListed(dir))
        throw OSErrorException(ioErr);
    const Descriptor* d = dir->childByIndex(index, includeDirectories);
    if(!d)
        throw OSErrorException(fnfErr);
    return realizeChild(dir, *d);
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

ItemPtr ItemCache::makeItem(CNID parID, CNID cnid, const fs::path& path, mac_string_view name, bool isDir)
{
    ItemPtr item;

    if(isDir)
        item = std::make_shared<DirectoryItem>(*this, parID, cnid, path, name);
    else
        item = std::make_shared<FileItem>(*this, parID, cnid, path, name);

    localVolumeStats().itemsConstructed++;

    // Retain realized directories so a listing or tally survives between
    // accesses; files are realized transiently (retained from Phase 10).
    if(isDir)
    {
        auto dir = std::static_pointer_cast<DirectoryItem>(item);
        retainDirectory(dir);
        evictCachedDirectories(dir);
    }

    items_[cnid] = item;
    return item;
}

void ItemCache::materialize(FileItem& item)
{
    if(item.rep())
        return;

    item.setRep(itemFactory_->createRepForFile(*this, item, fs::directory_entry(item.path())));
}

void ItemCache::rebindItem(CNID cnid)
{
    auto mapping = cnidMapper_->lookupCNID(cnid);
    if(!mapping)
        return;

    PathStat st = statPath(mapping->path);
    cnidMapper_->setIdentity(cnid, st.identity);
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
