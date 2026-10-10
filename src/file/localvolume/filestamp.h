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

// stat the path; `valid` is false if it could not be stat'd.
FileStamp statFile(const fs::path& path);

} // namespace Executor
