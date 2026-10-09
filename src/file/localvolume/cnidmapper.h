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

    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<fs::directory_entry> realPaths) = 0;
    virtual std::optional<Mapping> lookupCNID(CNID cnid) = 0;

    // Resolve an existing child by (parent CNID, host filename) without
    // enumerating the parent directory.
    virtual std::optional<Mapping> lookupChild(CNID parID, const fs::path& filename) = 0;
    // Resolve or create the identity for (parent CNID, filename), assigning a
    // CNID and a (provisional) Mac name when new. `path` is the full host path.
    // Name-free identity assignment is the eventual goal; for now a name is
    // still chosen here so that a later full enumeration reuses it.
    virtual Mapping getOrCreateChild(CNID parID, const fs::path& path, const fs::path& filename) = 0;

    virtual void deleteCNID(CNID cnid) = 0;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop) = 0;
};

}
