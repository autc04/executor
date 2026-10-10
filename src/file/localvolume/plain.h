#pragma once

#include "localvolume.h"
#include "item.h"
#include "openfile.h"

namespace Executor
{
class PlainRep : public FileRep
{
public:
    explicit PlainRep(FileItem& item) : FileRep(item) {}

    virtual std::unique_ptr<OpenFile> open(int8_t permission) override;
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission) override;
    virtual void getInfo(ItemInfo& info) override;
    virtual void setInfo(const ItemInfo& info) override;
};

class PlainDataFork : public OpenFile
{
protected:
    int fd;

public:
    struct create_t {};
    static constexpr create_t create = {};

    PlainDataFork(fs::path path, int8_t permission);
    PlainDataFork(fs::path path, create_t);
    ~PlainDataFork();

    virtual size_t getEOF() override;
    virtual void setEOF(size_t sz) override;
    virtual size_t read(size_t offset, void *p, size_t n) override;
    virtual size_t write(size_t offset, void *p, size_t n) override;
};
} // namespace Executor
