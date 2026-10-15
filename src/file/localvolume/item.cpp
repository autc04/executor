#include "item.h"
#include "itemcache.h"
#include "cnidmapper.h"
#include "openfile.h"

#include <OSUtil.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <iostream>

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
}

mac_string Executor::macNamePrefix(mac_string_view macName)
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

mac_string Executor::hostGroupPrefix(const fs::path& posixName)
{
    return macNamePrefix(toMacRomanFilename(posixName, 0));
}

Item::Item(ItemCache& itemcache, CNID parID, CNID cnid, fs::path p, std::optional<mac_string> name)
    : itemcache_(itemcache), parID_(parID), cnid_(cnid), path_(std::move(p))
{
    if(name)
    {
        name_ = std::move(*name);
        nameDecided_ = true;
    }
}

Item::~Item()
{
    itemcache_.noteItemFreed(cnid_);
}

const mac_string& Item::name()
{
    itemcache_.ensureNamed(*this);
    return name_;
}

void Item::deleteItem()
{
    fs::remove(path());
}

void Item::moveItem(const fs::path& newPath, mac_string_view newName)
{
    fs::rename(path(), newPath);
    path_ = newPath;
    if(!newName.empty())
    {
        name_ = newName;
        nameDecided_ = true;
    }
}

void Item::relocate(const fs::path& newPath)
{
    if(path_ == newPath)
        return;

    if(path_.filename() != newPath.filename())
    {
        name_.clear();
        nameDecided_ = false;
    }

    path_ = newPath;
}

const int64_t macToUnixEpoch = 86400 * (365 * (1970-1904) + (1970-1904)/4);

ItemInfo Item::getInfo()
{
    ItemInfo info{};
    info.modTime = fs::last_write_time(path()) + macToUnixEpoch;

    return info;
}

void Item::setInfo(ItemInfo info)
{
    auto oldModTime = fs::last_write_time(path()) + macToUnixEpoch;
    if(oldModTime != (int64_t)info.modTime)
        fs::last_write_time(path(), info.modTime - macToUnixEpoch);
}

void FileRep::remove()
{
    fs::remove(item_.path());
}

fs::path FileRep::nameSource(const fs::path& filename) const
{
    return filename;
}

void FileRep::move(const fs::path& newPath)
{
    fs::rename(item_.path(), newPath);
}

void FileItem::ensureMaterialized()
{
    if(rep_)
    {
        // The rep is a function of the host file; rebuild it if that changed.
        if(statFile(path_) == repStamp_)
            return;
        rep_.reset();
    }
    itemcache_.materialize(*this);
    repStamp_ = statFile(path_);
    assert(rep_);
}

std::unique_ptr<OpenFile> FileItem::open(int8_t permission)
{
    ensureMaterialized();
    return rep_->open(permission);
}

std::unique_ptr<OpenFile> FileItem::openRF(int8_t permission)
{
    ensureMaterialized();
    return rep_->openRF(permission);
}

ItemInfo FileItem::getInfo()
{
    ensureMaterialized();
    ItemInfo info = Item::getInfo();
    rep_->getInfo(info);
    return info;
}

void FileItem::setInfo(ItemInfo info)
{
    ensureMaterialized();
    Item::setInfo(info);
    rep_->setInfo(info);
}

void FileItem::deleteItem()
{
    ensureMaterialized();
    rep_->remove();
}

void FileItem::moveItem(const fs::path& newPath, mac_string_view newName)
{
    ensureMaterialized();
    rep_->move(newPath);
    path_ = newPath;
    if(!newName.empty())
    {
        name_ = newName;
        nameDecided_ = true;
    }
}

DirectoryItemPtr DirectoryItem::self()
{
    return std::static_pointer_cast<DirectoryItem>(shared_from_this());
}

int DirectoryItem::countVisible()
{
    // List implies Count.
    if(listed_)
        return countItems();

    FileStamp stamp = statFile(path());
    if(count_ && countStamp_ == stamp)
    {
        itemcache_.cacheItem(self());
        return *count_;
    }

    count_ = itemcache_.countVisible(path());
    countStamp_ = stamp;   // as for a listing: keyed to the state we started
    itemcache_.cacheItem(self());
    return *count_;
}

bool DirectoryItem::ensureListed()
{
    FileStamp stamp = statFile(path());
    if(listed_ && stamp_ == stamp)
        return true;   // L0: the stamp is unchanged, so everything is still valid

    auto descriptors = itemcache_.listDirectory(cnid(), path());
    if(!descriptors)
    {
        // A failed scan says nothing about the directory's contents, so it must
        // not be committed. Drop the listing instead -- the caller turns the
        // false into ioErr, and the next access retries from scratch.
        itemcache_.flushDirectoryCache(self());
        return false;
    }

    storeDescriptors(std::move(*descriptors), stamp);
    itemcache_.cacheItem(self());
    return true;
}

void DirectoryItem::resolveGroup(mac_string_view prefix)
{
    if(!ensureListed())
        return;   // unreadable: leave the names undecided

    auto [begin, end] = groupRange(prefix);
    if(begin == end)
        return;

    std::vector<CNIDMapper::NameRequest> group;
    group.reserve(end - begin);
    for(size_t i = begin; i < end; i++)
    {
        const Descriptor& d = descriptors_[i];
        // The whole group is offered, so already-decided names seed the collision
        // set; only the undecided entries need a (probed) name source.
        if(auto name = itemcache_.decidedName(d.cnid))
            group.push_back({ d.cnid, {} });
        else
            group.push_back({ d.cnid, itemcache_.nameSourceFor(path() / d.posixName) });
    }

    itemcache_.decideNameGroup(cnid(), group);
}

std::optional<mac_string> DirectoryItem::resolveName(const Item& item)
{
    if(!ensureListed())
        return {};

    resolveGroup(hostGroupPrefix(item.path().filename()));
    return itemcache_.decidedName(item.cnid());
}

ItemPtr DirectoryItem::child(mac_string_view name)
{
    // Fast path: the name is already decided for this directory.
    if(auto foundCnid = itemcache_.childNamed(cnid(), name))
        if(ItemPtr item = itemcache_.realizeValidated(*foundCnid))
        {
            // Realizing may have recovered a rename, invalidating this name.
            if(itemcache_.childNamed(cnid(), name) == foundCnid)
                return item;
        }

    if(!ensureListed())
        throw OSErrorException(ioErr);

    // Decide the group the name falls in, then look it up.
    resolveGroup(macNamePrefix(name));

    if(auto foundCnid = itemcache_.childNamed(cnid(), name))
        if(ItemPtr item = itemcache_.realizeValidated(*foundCnid))
            return item;

    return {};
}

ItemPtr DirectoryItem::child(int index, bool includeDirectories)
{
    if(!ensureListed())
        throw OSErrorException(ioErr);
    const Descriptor* d = childByIndex(index, includeDirectories);
    if(!d)
        throw OSErrorException(fnfErr);
    // The name is decided lazily, when the caller reads item->name().
    return itemcache_.realizeChild(self(), *d);
}

void DirectoryItem::clearCache()
{
    listed_ = false;
    stamp_ = {};
    count_.reset();
    countStamp_ = {};
    descriptors_.clear();
}

void DirectoryItem::storeDescriptors(std::vector<Descriptor> descriptors, FileStamp stamp)
{
    descriptors_ = std::move(descriptors);
    listed_ = true;
    stamp_ = stamp;
}

std::pair<size_t, size_t> DirectoryItem::groupRange(mac_string_view prefix) const
{
    assert(listed_);
    mac_string key { prefix };

    auto first = std::lower_bound(descriptors_.begin(), descriptors_.end(), key,
        [](const Descriptor& d, const mac_string& k) { return d.prefix < k; });
    auto last = std::upper_bound(first, descriptors_.end(), key,
        [](const mac_string& k, const Descriptor& d) { return k < d.prefix; });

    return { (size_t)(first - descriptors_.begin()), (size_t)(last - descriptors_.begin()) };
}

const Descriptor* DirectoryItem::childByIndex(int index, bool includeDirectories) const
{
    assert(listed_);
    if(index < 1)
        return nullptr;
    for(const Descriptor& d : descriptors_)
    {
        if(!includeDirectories && d.isDir)
            continue;
        if(--index == 0)
            return &d;
    }
    return nullptr;
}

void DirectoryItem::deleteItem()
{
    boost::system::error_code ec;
    fs::remove(path() / ".rsrc", ec);
    fs::remove(path() / ".finf", ec);       // TODO: individual ItemFactories should provide this info

    fs::remove(path(), ec);

    if(ec)
    {
        if(ec == boost::system::errc::directory_not_empty)
            throw OSErrorException(fBsyErr);
        else
            throw OSErrorException(paramErr);
    }
}
