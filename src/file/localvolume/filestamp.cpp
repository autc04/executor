#include "filestamp.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <sys/stat.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#else
#include <sys/stat.h>
#endif

#include <cerrno>
#include <unordered_map>

using namespace Executor;

bool Executor::dontUseInodes = false;

#if !defined(_WIN32)
namespace
{
// Whether the filesystem mounted at `path` exposes stable (dev, ino) values.
bool pathUsesStableIdentity(const fs::path& path)
{
#if defined(__linux__)
    struct statfs st;
    if(::statfs(path.string().c_str(), &st) != 0)
        return false;   // unknown: fall back to name+kind

    switch(static_cast<std::uint64_t>(st.f_type))
    {
    case EXT2_SUPER_MAGIC:   // ext2/ext3/ext4 share this magic
    case XFS_SUPER_MAGIC:
    case BTRFS_SUPER_MAGIC:
    case F2FS_SUPER_MAGIC:
    case REISERFS_SUPER_MAGIC:
    case NILFS_SUPER_MAGIC:
    case OCFS2_SUPER_MAGIC:
#ifdef BCACHEFS_SUPER_MAGIC
    case BCACHEFS_SUPER_MAGIC:
#endif
        return true;

    default:
        // tmpfs, overlayfs, NFS, FUSE, and unknown types: (dev, ino) is
        // per-mount or synthesized, so it is not a stable identity.
        return false;
    }
#else
    // No platform-specific classification yet; keep the previous behavior.
    (void)path;
    return true;
#endif
}

// The policy is per filesystem instance, not per volume: one LocalVolume may
// span several st_dev values (nested and bind mounts). Classify each device
// once, using a path on it, and cache the verdict for this thread.
bool deviceUsesStableIdentity(std::uint64_t device, const fs::path& path)
{
    // could use a regular static here, but just in case someone decides to do multithreading after all...
    thread_local std::unordered_map<std::uint64_t, bool> cache;

    auto it = cache.find(device);
    if(it != cache.end())
        return it->second;

    const bool stable = pathUsesStableIdentity(path);
    cache.emplace(device, stable);
    return stable;
}
}
#endif

bool Executor::operator==(const FileStamp& a, const FileStamp& b)
{
    return a.valid && b.valid && a.mtime == b.mtime && a.size == b.size;
}

#ifdef _WIN32
namespace
{
// Fill a PathStat from the fields both the attribute query and a directory-index
// lookup return.
void fillStat(PathStat& s, const FILETIME& lastWrite, DWORD sizeHigh, DWORD sizeLow,
    DWORD attributes)
{
    ULARGE_INTEGER t;
    t.LowPart = lastWrite.dwLowDateTime;
    t.HighPart = lastWrite.dwHighDateTime;
    // FILETIME counts 100 ns ticks since 1601-01-01; rebase to Unix ns.
    const std::uint64_t ticks1601To1970 = 116444736000000000ull;
    if(t.QuadPart >= ticks1601To1970)
        s.stamp.mtime = (t.QuadPart - ticks1601To1970) * 100ull;
    s.stamp.size = ((std::uint64_t)sizeHigh << 32) | (std::uint64_t)sizeLow;
    s.stamp.valid = true;
    s.exists = true;
    s.isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    s.isRegular = !s.isDirectory;
    // Neither query exposes an inode; a handle-based variant
    // (GetFileInformationByHandle) is needed for identity on Windows.
}
}
#endif

PathStat Executor::statPath(const fs::path& path)
{
    PathStat s;

#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if(GetFileAttributesExW(path.wstring().c_str(), GetFileExInfoStandard, &fad))
    {
        fillStat(s, fad.ftLastWriteTime, fad.nFileSizeHigh, fad.nFileSizeLow,
            fad.dwFileAttributes);
    }
    else
    {
        const DWORD error = GetLastError();
        if(error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED)
        {
            // A file that someone holds without sharing (pagefile.sys,
            // DumpStack.log.tmp, ...) cannot be opened for an attribute query, but
            // it is still in the directory index: FindFirstFile reads that index
            // and needs no handle, so one such file does not make its whole
            // directory unlistable. Not for patterns, where it would report an
            // unrelated match.
            WIN32_FIND_DATAW fd;
            HANDLE found = INVALID_HANDLE_VALUE;
            if(path.filename().wstring().find_first_of(L"*?") == std::wstring::npos)
            {
                found = FindFirstFileW(path.wstring().c_str(), &fd);
                if(found != INVALID_HANDLE_VALUE)
                    FindClose(found);
            }

            if(found != INVALID_HANDLE_VALUE)
                fillStat(s, fd.ftLastWriteTime, fd.nFileSizeHigh, fd.nFileSizeLow,
                    fd.dwFileAttributes);
            else
                s.error = std::error_code((int)error, std::system_category());
        }
        else if(error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            s.error = std::error_code((int)error, std::system_category());
    }
#else
    struct stat st;
    int statResult;
    do
    {
        statResult = ::stat(path.string().c_str(), &st);
    } while(statResult != 0 && errno == EINTR);

    if(statResult == 0)
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
        // Some filesystems report inode 0; treat that as "no usable inode".
        // --dont-use-inodes, or a filesystem without stable identities, forces
        // the same state for every file.
        s.identity.valid = !dontUseInodes && st.st_ino != 0
            && deviceUsesStableIdentity(s.identity.device, path);
    }
    else if(errno != ENOENT && errno != ENOTDIR)
        s.error = std::error_code(errno, std::generic_category());
#endif

    return s;
}

FileStamp Executor::statFile(const fs::path& path)
{
    return statPath(path).stamp;
}
