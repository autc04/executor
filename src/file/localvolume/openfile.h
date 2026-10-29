#pragma once

#include <stdlib.h>
#include <cerrno>
#include <FileMgr.h>

namespace Executor
{
// Not an error the operation can act on: the current format cannot store what
// was asked of it and LocalVolume has to upgrade it. Public inheritance keeps an
// unhandled one from escaping the trap boundary as an uncaught exception.
class UpgradeRequiredException : public std::runtime_error
{
public:
    UpgradeRequiredException() : std::runtime_error("file metadata representation update required") {}
};

// Translate a host errno from a failed open/write into the File Manager's
// vocabulary. Reps use this instead of constructing an object backed by an
// invalid descriptor.
inline OSErrorException hostError(int err)
{
    switch(err)
    {
        case ENOSPC:
        case EFBIG:
#ifdef EDQUOT
        case EDQUOT:
#endif
            return OSErrorException(dskFulErr);
        case EROFS:
            return OSErrorException(wPrErr);
        case EACCES:
        case EPERM:
            return OSErrorException(permErr);
        case ENOENT:
            return OSErrorException(fnfErr);
        default:
            return OSErrorException(ioErr);
    }
}

class OpenFile
{
public:
    virtual ~OpenFile() = default;

    virtual size_t getEOF() = 0;
    virtual void setEOF(size_t sz) { throw UpgradeRequiredException(); }
    virtual size_t read(size_t offset, void *p, size_t n) = 0;
    virtual size_t write(size_t offset, void *p, size_t n) { throw UpgradeRequiredException(); }

    virtual bool canWrite() const { return true; }
};

class EmptyFork : public OpenFile
{
public:
    virtual size_t getEOF() override { return 0; }
    virtual size_t read(size_t offset, void *p, size_t n) override { return 0; }
};
}
