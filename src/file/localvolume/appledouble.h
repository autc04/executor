#pragma once

#include "localvolume.h"
#include "item.h"
#include "openfile.h"

namespace Executor
{
enum class AppleDoubleScheme
{
    percent,
    dotunderscore
};

class AppleDoubleItemFactory : public ItemFactory
{
public:
    virtual bool isHidden(const fs::directory_entry &e) override;
    virtual ItemPtr createItemForDirEntry(ItemCache& itemcache, CNID parID, CNID cnid,
        const fs::directory_entry& e, mac_string_view macname) override;
    virtual void createFile(const fs::path& newPath) override;
};

class AppleSingleDoubleFile;

class AppleDoubleFileItem : public FileItem
{
    std::weak_ptr<AppleSingleDoubleFile> openedFile;
    AppleDoubleScheme scheme;

    std::shared_ptr<AppleSingleDoubleFile> access(int8_t permission);
public:
    AppleDoubleFileItem(AppleDoubleScheme scheme, ItemCache& itemcache, CNID parID, CNID cnid, fs::path p, mac_string_view name = {});

    virtual ItemInfo getInfo() override;
    virtual void setInfo(ItemInfo info) override;
    virtual std::unique_ptr<OpenFile> open(int8_t permission) override;
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission) override;

    virtual void deleteItem() override;
    virtual void moveItem(const fs::path& newPath, mac_string_view newName) override;
};

class AppleSingleItemFactory : public ItemFactory
{
public:
    virtual ItemPtr createItemForDirEntry(ItemCache& itemcache, CNID parID, CNID cnid,
        const fs::directory_entry& e, mac_string_view macname) override;
    virtual void createFile(const fs::path& newPath) override;
};

class AppleSingleFileItem : public FileItem
{
    std::weak_ptr<AppleSingleDoubleFile> openedFile;

    std::shared_ptr<AppleSingleDoubleFile> access(int8_t permission);
public:
    using FileItem::FileItem;

    virtual ItemInfo getInfo() override;
    virtual void setInfo(ItemInfo info) override;
    virtual std::unique_ptr<OpenFile> open(int8_t permission) override;
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission) override;
};

class AppleSingleDoubleFile
{
    struct EntryDescriptor
    {
        GUEST_STRUCT;
        GUEST<uint32_t> entryId;
        GUEST<uint32_t> offset;
        GUEST<uint32_t> length;
    };
    std::unique_ptr<OpenFile> file;
    std::vector<EntryDescriptor> descriptors;

    using EntryDescriptorIterator = std::vector<EntryDescriptor>::iterator;
    EntryDescriptorIterator findEntry(uint32_t entryId);
    EntryDescriptorIterator setEOFCommon(uint32_t entryId, size_t sz, bool allowShrink);

public:
    struct create_single_t {};
    struct create_double_t {};

    static constexpr create_single_t create_single = {};
    static constexpr create_double_t create_double = {};

    AppleSingleDoubleFile(std::unique_ptr<OpenFile> file);
    AppleSingleDoubleFile(std::unique_ptr<OpenFile> file, create_single_t);
    AppleSingleDoubleFile(std::unique_ptr<OpenFile> file, create_double_t);
    ~AppleSingleDoubleFile();

    size_t getEOF(uint32_t entryId);
    void setEOF(uint32_t entryId, size_t sz);
    size_t read(uint32_t entryId, size_t offset, void *p, size_t n);
    size_t write(uint32_t entryId, size_t offset, void *p, size_t n);
};

class AppleSingleDoubleFork : public OpenFile
{
    std::shared_ptr<AppleSingleDoubleFile> file;
    uint32_t entryId;
public:
    AppleSingleDoubleFork(std::shared_ptr<AppleSingleDoubleFile> file, uint32_t entryId);
    ~AppleSingleDoubleFork();

    virtual size_t getEOF() override;
    virtual void setEOF(size_t sz) override;
    virtual size_t read(size_t offset, void *p, size_t n) override;
    virtual size_t write(size_t offset, void *p, size_t n) override;
};
} // namespace Executor