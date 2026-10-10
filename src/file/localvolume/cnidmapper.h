#pragma once

#include <rsys/filesystem.h>
#include <util/macstrings.h>
#include <optional>

namespace Executor
{

using CNID = int32_t;

class CNIDMapper
{
public:
    virtual ~CNIDMapper() = default;

    struct Mapping
    {
        CNID parID;
        CNID cnid;
        fs::directory_entry entry;
        mac_string macname;
    };

    // A host entry plus the filename its Mac name is derived from. The two differ
    // only where a rep renames (MacBinary drops a trailing ".bin").
    struct DirEntry
    {
        fs::directory_entry entry;
        fs::path nameSource;
    };

    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<DirEntry> entries) = 0;
    virtual std::optional<Mapping> lookupCNID(CNID cnid) = 0;

    // Resolve an existing child by (parent CNID, host filename) without
    // enumerating the parent directory.
    virtual std::optional<Mapping> lookupChild(CNID parID, const fs::path& filename) = 0;
    // Resolve or create the identity for (parent CNID, filename), assigning a
    // CNID and a (provisional) Mac name when new. `path` is the full host path;
    // the Mac name is derived from `nameSource`.
    virtual Mapping getOrCreateChild(CNID parID, const fs::path& path, const fs::path& filename,
        const fs::path& nameSource) = 0;

    virtual void deleteCNID(CNID cnid) = 0;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop) = 0;
};

}
