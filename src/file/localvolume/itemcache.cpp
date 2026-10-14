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

namespace
{
// A generated disambiguator ("#123") is at most this many digits; a group's
// truncated prefix leaves room for one.
constexpr size_t maxDisambiguatorDigits = 3;

bool endsWithBin(const mac_string& s)
{
    if(s.size() < 4)
        return false;
    return s[s.size()-4] == (unsigned char)'.'
        && std::tolower(s[s.size()-3]) == 'b'
        && std::tolower(s[s.size()-2]) == 'i'
        && std::tolower(s[s.size()-1]) == 'n';
}

bool endsWithDisambiguator(const mac_string& s, size_t& hashPos)
{
    size_t hash = s.rfind((unsigned char)'#');
    if(hash == mac_string::npos || hash + 1 >= s.size())
        return false;

    for(size_t i = hash + 1; i < s.size(); i++)
        if(s[i] < (unsigned char)'0' || s[i] > (unsigned char)'9')
            return false;

    hashPos = hash;
    return true;
}

// The grouping key for a Mac name: its first 31 bytes, with a trailing ".bin"
// and/or "#<digits>" disambiguator stripped, truncated to leave room for a
// generated disambiguator, and case-folded. Two entries can collide iff their
// keys are equal, so over-grouping only costs laziness. See *Names* in the plan.
mac_string macNamePrefix(mac_string_view macName)
{
    mac_string s { macName };
    if(s.size() > 31)
        s.resize(31);

    for(;;)
    {
        if(endsWithBin(s))
        {
            s.resize(s.size() - 4);
            continue;
        }
        size_t hash;
        if(endsWithDisambiguator(s, hash))
        {
            s.resize(hash);
            continue;
        }
        break;
    }

    const size_t maxPrefix = 30 - maxDisambiguatorDigits;
    if(s.size() > maxPrefix)
        s.resize(maxPrefix);

    ROMlib_UprString(s.data(), false, s.size());
    return s;
}

// The grouping key of a host filename.
mac_string hostGroupPrefix(const fs::path& posixName)
{
    return macNamePrefix(toMacRomanFilename(posixName, 0));
}
}

ItemCache::ItemCache(fs::path root,
                mac_string_view volumeName,
                std::unique_ptr<CNIDMapper> cnidMapper,
                ItemFactory *itemFactory
                )
    : cnidMapper_(std::move(cnidMapper))
    , itemFactory_(itemFactory)
{
    items_[2] = rootDirItem_ = std::make_shared<DirectoryItem>(*this, 1, 2, root, mac_string(volumeName));
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

                entries.push_back({ e.path().filename(), st.identity, st.isDirectory });
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

    fs::path path = rootDirItem_->path();
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

    flushDirectoryCache(parent);
    ensureListed(parent);

    return realizeOnce(cnid);
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

ItemPtr ItemCache::child(const DirectoryItemPtr& dir, mac_string_view name)
{
    // Fast path: the name is already decided for this directory.
    if(auto cnid = cnidMapper_->cnidForName(dir->cnid(), name))
        if(ItemPtr item = realizeValidated(*cnid))
        {
            // Realizing may have recovered a rename, invalidating this name.
            if(cnidMapper_->cnidForName(dir->cnid(), name) == cnid)
                return item;
        }

    if(!ensureListed(dir))
        throw OSErrorException(ioErr);

    // Decide the group the name falls in, then look it up.
    resolveNameGroup(dir, macNamePrefix(name));

    if(auto cnid = cnidMapper_->cnidForName(dir->cnid(), name))
        if(ItemPtr item = realizeValidated(*cnid))
            return item;

    return {};
}

ItemPtr ItemCache::child(const DirectoryItemPtr& dir, int index, bool includeDirectories)
{
    if(!ensureListed(dir))
        throw OSErrorException(ioErr);
    const Descriptor* d = dir->childByIndex(index, includeDirectories);
    if(!d)
        throw OSErrorException(fnfErr);
    // The name is decided lazily, when the caller reads item->name().
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
    // naming does not run the format gauntlet for every file.
    localVolumeStats().nameProbes++;
    FileItem probe(*this, 0, 0, path);
    FileRepPtr rep = itemFactory_->createRepForFile(*this, probe, fs::directory_entry(path));
    return rep ? rep->nameSource(filename) : filename;
}

void ItemCache::resolveNameGroup(const DirectoryItemPtr& dir, mac_string_view prefix)
{
    if(!ensureListed(dir))
        return;   // unreadable: leave the names undecided

    auto [begin, end] = dir->groupRange(prefix);
    if(begin == end)
        return;

    const auto& descriptors = dir->descriptors();

    std::vector<CNIDMapper::NameRequest> group;
    group.reserve(end - begin);
    for(size_t i = begin; i < end; i++)
    {
        const Descriptor& d = descriptors[i];
        // The whole group is offered, so already-decided names seed the collision
        // set; only the undecided entries need a (probed) name source.
        if(auto rec = cnidMapper_->lookupCNID(d.cnid); rec && rec->macname)
            group.push_back({ d.cnid, {} });
        else
            group.push_back({ d.cnid, nameSourceFor(dir->path() / d.posixName) });
    }

    cnidMapper_->nameGroup(dir->cnid(), group);
}

void ItemCache::ensureNamed(Item& item)
{
    if(item.nameDecided_)
        return;

    if(auto rec = cnidMapper_->lookupCNID(item.cnid()); rec && rec->macname)
    {
        item.name_ = *rec->macname;
        item.nameDecided_ = true;
        return;
    }

    auto dir = std::dynamic_pointer_cast<DirectoryItem>(realizeValidated(item.parID_));
    if(!dir)
        return;

    resolveNameGroup(dir, hostGroupPrefix(item.path().filename()));

    if(auto rec = cnidMapper_->lookupCNID(item.cnid()); rec && rec->macname)
    {
        item.name_ = *rec->macname;
        item.nameDecided_ = true;
    }
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

    // Retain realized directories so a listing or tally survives between
    // accesses; files are realized transiently (an all-`Item` LRU is future
    // work).
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
