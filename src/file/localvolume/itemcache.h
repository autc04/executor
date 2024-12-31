#pragma once

#include <rsys/filesystem.h>
#include <util/macstrings.h>
#include <memory>
#include <chrono>
#include <list>
#include <unordered_map>
#include <stdint.h>

namespace Executor
{
class Item;
class ItemFactory;
class CNIDMapper;
class DirectoryItem;

using ItemPtr = std::shared_ptr<Item>;
using DirectoryItemPtr = std::shared_ptr<DirectoryItem>;

using CNID = int32_t;

class ItemCache
{
    std::unique_ptr<CNIDMapper> cnidMapper_;
    ItemFactory *itemFactory_;

    std::unordered_map<CNID, std::weak_ptr<Item>> items_; 
    
    DirectoryItemPtr rootDirItem_;

    struct CachedDirectory
    {
        DirectoryItemPtr directory;
        std::chrono::steady_clock::time_point timestamp;
    };
    std::list<CachedDirectory> cachedDirectories_;

    void cleanDirectoryCache();
public:
    ItemCache(fs::path root, mac_string_view volumeName,
                std::unique_ptr<CNIDMapper> cnidMapper,
                ItemFactory *itemFactory
                );
    ~ItemCache();

    void noteItemFreed(CNID cnid);

    ItemPtr tryResolve(CNID cnid);
    ItemPtr tryResolve(fs::path path);


    void cacheDirectory(DirectoryItemPtr item);
    void flushDirectoryCache(DirectoryItemPtr item);
    void flushDirectoryCache(CNID dirID);
    void flushItem(ItemPtr item);

    void deleteItem(ItemPtr item);
    void renameItem(ItemPtr item, mac_string_view newName);
    void moveItem(ItemPtr item, DirectoryItemPtr newParent);
};

}
