#include "filestamp.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using namespace Executor;

bool Executor::operator==(const FileStamp& a, const FileStamp& b)
{
    return a.valid && b.valid && a.mtime == b.mtime && a.size == b.size;
}

PathStat Executor::statPath(const fs::path& path)
{
    PathStat s;

#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if(GetFileAttributesExW(path.wstring().c_str(), GetFileExInfoStandard, &fad))
    {
        ULARGE_INTEGER t;
        t.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        // FILETIME counts 100 ns ticks since 1601-01-01; rebase to Unix ns.
        const std::uint64_t ticks1601To1970 = 116444736000000000ull;
        if(t.QuadPart >= ticks1601To1970)
            s.stamp.mtime = (t.QuadPart - ticks1601To1970) * 100ull;
        s.stamp.size = ((std::uint64_t)fad.nFileSizeHigh << 32) | (std::uint64_t)fad.nFileSizeLow;
        s.stamp.valid = true;
        s.exists = true;
        s.isDirectory = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        s.isRegular = !s.isDirectory;
        // GetFileAttributesEx exposes no inode; a handle-based variant
        // (GetFileInformationByHandle) is needed for identity on Windows.
    }
#else
    struct stat st;
    if(::stat(path.string().c_str(), &st) == 0)
    {
#if defined(__APPLE__)
        const struct timespec& ts = st.st_mtimespec;
#else
        const struct timespec& ts = st.st_mtim;
#endif
        s.stamp.mtime = (std::uint64_t)ts.tv_sec * 1000000000ull + (std::uint64_t)ts.tv_nsec;
        s.stamp.size = (std::uint64_t)st.st_size;
        s.stamp.valid = true;
        s.exists = true;
        s.isDirectory = S_ISDIR(st.st_mode);
        s.isRegular = S_ISREG(st.st_mode);
        s.identity.device = (std::uint64_t)st.st_dev;
        s.identity.inode = (std::uint64_t)st.st_ino;
        s.identity.nlink = (std::uint64_t)st.st_nlink;
        // Some filesystems report inode 0; treat that as "no usable inode".
        s.identity.valid = st.st_ino != 0;
    }
#endif

    return s;
}

FileStamp Executor::statFile(const fs::path& path)
{
    return statPath(path).stamp;
}
