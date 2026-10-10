#include "item.h"
#include "itemcache.h"
#include "openfile.h"

#include <OSUtil.h>
#include <cassert>
#include <iostream>

using namespace Executor;

Item::Item(ItemCache& itemcache, CNID parID, CNID cnid, fs::path p, mac_string_view name)
    : itemcache_(itemcache), parID_(parID), cnid_(cnid), path_(std::move(p)), name_(name)
{
}

Item::~Item()
{
    itemcache_.noteItemFreed(cnid_);
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
        name_ = newName;
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
        name_ = newName;
}

void DirectoryItem::clearCache()
{
    cache_valid_ = false;
    stamp_ = {};
    count_.reset();
    countStamp_ = {};
    contents_.clear();
    contents_by_name_.clear();
    files_.clear();
}

void DirectoryItem::populateCache(std::vector<ItemPtr> items, FileStamp stamp)
{
    contents_ = std::move(items);
    contents_by_name_.clear();
    files_.clear();

    for(const ItemPtr& item : contents_)
    {
        mac_string nameUpr = item->name();
        ROMlib_UprString(nameUpr.data(), false, nameUpr.size());

        assert(nameUpr.size());
        [[maybe_unused]] auto inserted = contents_by_name_.emplace(nameUpr, item).second;
        assert(inserted);

        if(!dynamic_cast<DirectoryItem*>(item.get()))
            files_.push_back(item);
    }

    cache_valid_ = true;
    stamp_ = stamp;
}

ItemPtr DirectoryItem::tryResolve(mac_string_view name)
{
    assert(cache_valid_);
    mac_string nameUpr { name };
    ROMlib_UprString(nameUpr.data(), false, nameUpr.size());
    auto it = contents_by_name_.find(nameUpr);
    if(it != contents_by_name_.end())
        return it->second;
    return {};
}

ItemPtr DirectoryItem::tryResolve(fs::path name)
{
    assert(cache_valid_);
    fs::path path = path_ / name;
    for(auto& item : contents_)
        if(item->path() == path)
            return item;
    return {};
}

ItemPtr DirectoryItem::resolve(int index, bool includeDirectories)
{
    assert(cache_valid_);
    const auto& array = includeDirectories ? contents_ : files_;
    if(index >= 1 && index <= array.size())
        return array[index-1];
    throw OSErrorException(fnfErr);
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
