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
        name_ = newName;
}

void DirectoryItem::clearCache()
{
    listed_ = false;
    stamp_ = {};
    count_.reset();
    countStamp_ = {};
    descriptors_.clear();
    nameIndex_.clear();
}

void DirectoryItem::storeDescriptors(std::vector<Descriptor> descriptors, FileStamp stamp)
{
    descriptors_ = std::move(descriptors);
    nameIndex_.clear();
    for(size_t i = 0; i < descriptors_.size(); i++)
    {
        mac_string folded = descriptors_[i].macName;
        ROMlib_UprString(folded.data(), false, folded.size());
        assert(folded.size());
        [[maybe_unused]] auto inserted = nameIndex_.emplace(std::move(folded), i).second;
        assert(inserted);
    }

    listed_ = true;
    stamp_ = stamp;
}

const Descriptor* DirectoryItem::childByName(mac_string_view name) const
{
    assert(listed_);
    mac_string nameUpr { name };
    ROMlib_UprString(nameUpr.data(), false, nameUpr.size());
    auto it = nameIndex_.find(nameUpr);
    if(it != nameIndex_.end())
        return &descriptors_[it->second];
    return nullptr;
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
