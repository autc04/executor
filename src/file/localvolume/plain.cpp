#include "plain.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <iostream>
#include <rsys/macros.h>
#include <rsys/unixio.h>

#ifdef _WIN32
#define NOMINMAX 1
#include <windows.h>
#include <io.h>
#include <cstdint>
#endif

using namespace Executor;

#ifdef _WIN32
namespace
{
// The errno a failed CreateFileW should report, so hostError() returns the same
// OSErr a failed open() would have produced.
int errnoForWin32Error(DWORD error)
{
    switch(error)
    {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
            return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return EACCES;
        case ERROR_TOO_MANY_OPEN_FILES:
            return EMFILE;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ENOSPC;
        case ERROR_WRITE_PROTECT:
            return EROFS;
        default:
            return EIO;
    }
}

// _open() lets CreateFile share only FILE_SHARE_READ|FILE_SHARE_WRITE, so while
// the guest holds a fork open the file cannot be renamed, moved or deleted --
// but classic Mac OS allows renaming and moving an open file (only deleting one
// is fBsyErr). Share delete rights too, so PBHRename and PBCatMove keep working
// on a file the guest has open.
int openSharingDelete(const fs::path& path, DWORD access, DWORD creation)
{
    HANDLE handle = ::CreateFileW(path.wstring().c_str(), access,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(handle == INVALID_HANDLE_VALUE)
    {
        errno = errnoForWin32Error(::GetLastError());
        return -1;
    }

    int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle),
                               (access & GENERIC_WRITE) ? _O_RDWR | _O_BINARY
                                                        : _O_RDONLY | _O_BINARY);
    if(fd < 0)
        ::CloseHandle(handle);   // _open_osfhandle has set errno

    return fd;
}
}
#endif

#if 1
PlainDataFork::PlainDataFork(fs::path path, int8_t permission)
{
    fd = -1;
    if (permission == fsRdWrPerm || permission == fsWrPerm || permission == fsCurPerm
        || permission == fsRdWrShPerm || permission == fsWrDenyPerm)
    {
#ifdef _WIN32
        fd = openSharingDelete(path, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
#else
        fd = open(path.string().c_str(), O_RDWR | O_BINARY, 0644);
#endif
        writable_ = fd >= 0;
    }

    if (fd == -1 &&
        (permission == fsCurPerm || permission == fsRdPerm
        || permission == fsRdDenyPerm))
    {
#ifdef _WIN32
        fd = openSharingDelete(path, GENERIC_READ, OPEN_EXISTING);
#else
        fd = open(path.string().c_str(), O_RDONLY | O_BINARY, 0644);
#endif
    }

    if(fd < 0)
        throw hostError(errno);

    //std::cout << "ACCESSING FILE: " << fd << " = " << path << std::endl;
}

PlainDataFork::PlainDataFork(fs::path path, create_t)
{
#ifdef _WIN32
    fd = openSharingDelete(path, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS);
#else
    fd = open(path.string().c_str(), O_RDWR | O_CREAT | O_BINARY, 0644);
#endif
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
