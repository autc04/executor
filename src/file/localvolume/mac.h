#pragma once

#include "localvolume.h"
#include "item.h"

namespace Executor
{
class MacRep : public FileRep
{
public:
    explicit MacRep(FileItem& item) : FileRep(item) {}

    virtual void getInfo(ItemInfo& info) override;
    virtual void setInfo(const ItemInfo& info) override;
    virtual std::unique_ptr<OpenFile> open(int8_t permission) override;
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission) override;
};


class MacItemFactory : public ItemFactory
{
    virtual FileRepPtr createRepForFile(ItemCache& itemcache, FileItem& item,
        const fs::directory_entry& e, ProbeTrace& trace) override;
    virtual void createFile(const fs::path& newPath) override;
};
}