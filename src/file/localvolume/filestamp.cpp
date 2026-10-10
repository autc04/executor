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

FileStamp Executor::statFile(const fs::path& path)
{
    FileStamp s;

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
            s.mtime = (t.QuadPart - ticks1601To1970) * 100ull;
        s.size = ((std::uint64_t)fad.nFileSizeHigh << 32) | (std::uint64_t)fad.nFileSizeLow;
        s.valid = true;
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
        s.mtime = (std::uint64_t)ts.tv_sec * 1000000000ull + (std::uint64_t)ts.tv_nsec;
        s.size = (std::uint64_t)st.st_size;
        s.valid = true;
    }
#endif

    return s;
}
