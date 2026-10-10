#pragma once

#include "localvolume.h"
#include "item.h"

namespace Executor
{
class BasiliskItemFactory : public ItemFactory
{
public:
    virtual bool isHidden(const fs::directory_entry &e) override;
    virtual FileRepPtr createRepForFile(ItemCache& itemcache, FileItem& item,
        const fs::directory_entry& e) override;
    virtual void createFile(const fs::path& newPath) override;
};

class BasiliskRep : public FileRep
{
public:
    explicit BasiliskRep(FileItem& item) : FileRep(item) {}

    virtual void getInfo(ItemInfo& info) override;
    virtual void setInfo(const ItemInfo& info) override;
    virtual std::unique_ptr<OpenFile> open(int8_t permission) override;
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission) override;

    virtual void remove() override;
    virtual void move(const fs::path& newPath) override;
};
} // namespace Executor