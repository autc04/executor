#include "plain.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include <rsys/macros.h>
#include <rsys/unixio.h>

using namespace Executor;

#if 1
PlainDataFork::PlainDataFork(fs::path path, int8_t permission)
{
    fd = -1;
    if (permission == fsRdWrPerm || permission == fsWrPerm || permission == fsCurPerm
        || permission == fsRdWrShPerm || permission == fsWrDenyPerm)
    {
        fd = open(path.string().c_str(), O_RDWR | O_BINARY, 0644);
        writable_ = fd >= 0;
    }

    if (fd == -1 &&
        (permission == fsCurPerm || permission == fsRdPerm
        || permission == fsRdDenyPerm))
    {
        fd = open(path.string().c_str(), O_RDONLY | O_BINARY, 0644);
    }

    if(fd < 0)
        throw hostError(errno);

    //std::cout << "ACCESSING FILE: " << fd << " = " << path << std::endl;
}

PlainDataFork::PlainDataFork(fs::path path, create_t)
{
    fd = open(path.string().c_str(), O_RDWR | O_CREAT | O_BINARY, 0644);
    if(fd < 0)
        throw hostError(errno);
    writable_ = true;
    //std::cout << "CREATING FILE: " << fd << " = " << path << std::endl;
}

PlainDataFork::~PlainDataFork()
{
    if(fd >= 0)
        close(fd);
    //std::cout << "CLOSING FILE: " << fd << std::endl;
}

size_t PlainDataFork::getEOF()
{
    if(fd < 0)
        return -1;
    return lseek(fd, 0, SEEK_END);
}
void PlainDataFork::setEOF(size_t sz)
{
#if defined(_WIN32)
    chsize(fd, sz);
#else
    ftruncate(fd, sz);
#endif
}

size_t PlainDataFork::read(size_t offset, void *p, size_t n)
{
    lseek(fd, offset, SEEK_SET);
    auto done = ::read(fd, p, n);

    if(done < 0)
        throw hostError(errno);

    return size_t(done);
}
size_t PlainDataFork::write(size_t offset, void *p, size_t n)
{
    lseek(fd, offset, SEEK_SET);
    auto done = ::write(fd, p, n);

    if(done < 0)
        throw hostError(errno);

    if((size_t)done != n)
    {
        std::cout << "short write: " << done << " of " << n << " ; errno = " << errno << std::endl;
    }
    return done;
}
#else
PlainDataFork::PlainDataFork(fs::path path, int8_t permission)
    : stream(path, std::ios::binary)
{
    std::cout << "ACCESSING FILE: " << path << std::endl;
}
PlainDataFork::~PlainDataFork()
{
}

size_t PlainDataFork::getEOF()
{
    stream.seekg(0, std::ios::end);
    return stream.tellg();
}
void PlainDataFork::setEOF(size_t sz)
{
    
}
size_t PlainDataFork::read(size_t offset, void *p, size_t n)
{
    stream.seekg(offset);
    stream.read((char*)p, n);
    stream.clear(); // ?
    return stream.gcount();
}
size_t PlainDataFork::write(size_t offset, void *p, size_t n)
{
    stream.seekp(offset);
    stream.write((char*)p, n);
    return n;
}
#endif


std::unique_ptr<OpenFile> PlainRep::open(int8_t permission)
{
    return std::make_unique<PlainDataFork>(item_.path(), permission);
}

std::unique_ptr<OpenFile> PlainRep::openRF(int8_t permission)
{
    return std::make_unique<EmptyFork>();
}

void PlainRep::getInfo(ItemInfo& info)
{
    info.file.info = {
        "TEXT"_4,
        "ttxt"_4,
        0, // fdFlags
        { 0, 0 }, // fdLocation
        0 // fdFldr
    };
}

void PlainRep::setInfo(const ItemInfo& info)
{
    if(info.file.info.fdType != "TEXT"_4)
        throw UpgradeRequiredException();
}
