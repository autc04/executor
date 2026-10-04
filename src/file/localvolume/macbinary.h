#pragma once

#include "localvolume.h"
#include "item.h"
#include "openfile.h"

#include <memory>

namespace Executor
{
// Returns true if p looks like a MacBinary file: the name ends in ".bin" and
// the header passes the validity checks described in
// docs/ai/2026-10-04-macbinary-localvolume-backend.md.
bool isMacBinaryFile(const fs::path& p);

// Manages a single MacBinary host file: the 128-byte header, the data fork
// (offset 128) and the resource fork (offset 128 plus the data fork padded to
// a 128-byte boundary).  Multi-byte header fields are big-endian.
class MacBinaryFile
{
public:
    enum class Fork
    {
        data,
        resource
    };

    explicit MacBinaryFile(std::unique_ptr<OpenFile> file);

    size_t getEOF(Fork fork);

    // Resize one fork, relocating the other fork and any trailing data
    // (Get Info comment and padding) as needed, then rewrite the header.
    void setEOF(Fork fork, size_t sz);

    size_t read(Fork fork, size_t offset, void *p, size_t n);
    size_t write(Fork fork, size_t offset, void *p, size_t n);

    void readFInfo(ItemInfo& info);
    void writeFInfo(const ItemInfo& info);

private:
    std::unique_ptr<OpenFile> file_;
    uint8_t header_[128];
    uint32_t dataLength_ = 0;
    uint32_t rsrcLength_ = 0;
    bool isV23_ = false;
    bool hasCRC_ = false;

    uint32_t forkLength(Fork fork) const;
    uint64_t rsrcOffset() const;
    uint64_t trailingOffset() const;
    uint64_t forkOffset(Fork fork) const;

    void writeHeader();
    void moveBlock(uint64_t from, uint64_t to, uint64_t size);
    void zeroRange(uint64_t offset, uint64_t size);
};

class MacBinaryFork : public OpenFile
{
    std::shared_ptr<MacBinaryFile> file;
    MacBinaryFile::Fork fork;

public:
    MacBinaryFork(std::shared_ptr<MacBinaryFile> file, MacBinaryFile::Fork fork);

    virtual size_t getEOF() override;
    virtual void setEOF(size_t sz) override;
    virtual size_t read(size_t offset, void *p, size_t n) override;
    virtual size_t write(size_t offset, void *p, size_t n) override;
};

class MacBinaryFileItem : public FileItem
{
    std::weak_ptr<MacBinaryFile> openedFile;

    std::shared_ptr<MacBinaryFile> access(int8_t permission);

public:
    using FileItem::FileItem;

    virtual ItemInfo getInfo() override;
    virtual void setInfo(ItemInfo info) override;
    virtual std::unique_ptr<OpenFile> open(int8_t permission) override;
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission) override;
};

class MacBinaryItemFactory : public ItemFactory
{
    virtual ItemPtr createItemForDirEntry(ItemCache& itemcache, CNID parID, CNID cnid,
        const fs::directory_entry& e, mac_string_view macname) override;
};

} // namespace Executor
