#pragma once

#include "cnidmapper.h"
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
class FileItem;
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
    };
    std::list<CachedDirectory> cachedDirectories_;
    size_t cachedItemCount_ = 0; // total items retained across enumerated dirs

    void evictCachedDirectories(const DirectoryItemPtr& protect);
    void touchCachedDirectory(const DirectoryItemPtr& dir);
    void clearCachedDirectory(const DirectoryItemPtr& dir);
    ItemPtr itemForMapping(const CNIDMapper::Mapping& mapping);

    // Construct an identity Item (DirectoryItem or bare FileItem) from a host
    // entry, without running the format gauntlet. Returns null for entries that
    // are neither a regular file nor a directory.
    ItemPtr makeItem(CNID parID, CNID cnid, const fs::directory_entry& e, mac_string_view name);

    // The filename a child's Mac name is derived from (differs from the host
    // filename only for MacBinary, whose ".bin" is dropped).
    fs::path nameSourceFor(const fs::path& path);

public:
    // Build and install the FileRep for a FileItem (the format gauntlet).
    // Idempotent; called lazily from FileItem on first access.
    void materialize(FileItem& item);

    ItemCache(fs::path root, mac_string_view volumeName,
                std::unique_ptr<CNIDMapper> cnidMapper,
                ItemFactory *itemFactory
                );
    ~ItemCache();

    void noteItemFreed(CNID cnid);

    ItemPtr tryResolve(CNID cnid);
    ItemPtr tryResolve(fs::path path);


    // Ensure `dir` is enumerated and valid; re-enumerates if the host changed.
    // Returns false if a fresh scan failed (the directory could not be read).
    bool ensureEnumerated(const DirectoryItemPtr& item);
    // Number of visible children, without enumerating/constructing them.
    int countChildren(const DirectoryItemPtr& item);
    void flushDirectoryCache(DirectoryItemPtr item);
    void flushDirectoryCache(CNID dirID);
    void flushItem(ItemPtr item);

    void deleteItem(ItemPtr item);
    void renameItem(ItemPtr item, mac_string_view newName);
    void moveItem(ItemPtr item, DirectoryItemPtr newParent, mac_string_view newName = {});
};

}
