#pragma once

#include <rsys/filesystem.h>

#include <cstdint>

namespace Executor
{

// A cheap change/identity stamp for a host file or directory: its last-write
// time (native resolution) and size, captured in a single stat. Two stamps
// compare equal only when both are valid and agree on both fields; an invalid
// stamp (the stat failed) never compares equal, so validation fails safe.
//
// Deliberately not built from boost::filesystem::last_write_time(), whose
// std::time_t result is only second-resolution and would miss same-second
// changes. See docs/ai/2026-10-08-localvolume-staged-materialization-plan.md
// (Caching).
struct FileStamp
{
    std::uint64_t mtime = 0; // nanoseconds since the Unix epoch
    std::uint64_t size = 0;
    bool valid = false;
};

bool operator==(const FileStamp& a, const FileStamp& b);
inline bool operator!=(const FileStamp& a, const FileStamp& b) { return !(a == b); }

// The host file's identity, as opposed to its change stamp: the (device, inode)
// pair a CNID is keyed by, plus the link count. `valid` is false when the host
// does not expose a usable inode (or gives inode 0); callers then fall back to
// matching by path. nlink is recorded so the deferred hard-link key
// (dev, ino, posixName) can be added without another stat. See *Identity* in the
// plan.
struct FileIdentity
{
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t nlink = 1;
    bool valid = false;
};

// True when two identities denote the same host file. If either is unusable we
// cannot tell, and report "same" so the caller falls back to the path.
inline bool sameIdentity(const FileIdentity& a, const FileIdentity& b)
{
    if(!a.valid || !b.valid)
        return true;
    return a.device == b.device && a.inode == b.inode;
}

// Everything one raw stat yields that the item model needs: the change stamp,
// the identity, and the kind. A single stat serves all three, so callers need
// not stat twice.
struct PathStat
{
    FileStamp stamp;
    FileIdentity identity;
    bool isDirectory = false;
    bool isRegular = false;
    bool exists = false;
};

// stat the path (following symlinks); `stamp.valid` and `identity.valid` are
// false, and `exists` is false, if it could not be stat'd.
PathStat statPath(const fs::path& path);

// The change stamp alone (a convenience wrapper around statPath).
FileStamp statFile(const fs::path& path);

} // namespace Executor
