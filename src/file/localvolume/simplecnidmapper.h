#pragma once

#include "cnidmapper.h"
#include <set>
#include <string>
#include <unordered_map>

namespace Executor
{

class SimpleCNIDMapper : public CNIDMapper
{
    CNID nextCNID_ = 3;

    struct StoredMapping
    {
        CNID parID;
        CNID cnid;
        fs::path path;
        mac_string macname;
    };
    std::unordered_map<CNID, StoredMapping> mappings_;

    // Directory contents: parID -> (host filename -> cnid). Doubles as the
    // (parID, posixFilename) -> cnid lookup index and as the diff index for
    // re-enumeration (stored children not seen in a host scan are gone).
    using ChildIndex = std::unordered_map<std::string, CNID>;
    std::unordered_map<CNID, ChildIndex> children_;

    std::set<mac_string> namesInUse(CNID dirID);
    mac_string chooseMacName(const fs::path& filename, std::set<mac_string>& usedNames);

public:
    explicit SimpleCNIDMapper(fs::path root, mac_string volumeName);

    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<DirEntry> realPaths) override;
    virtual std::optional<Mapping> lookupCNID(CNID cnid) override;
    virtual std::optional<Mapping> lookupChild(CNID parID, const fs::path& filename) override;
    virtual Mapping getOrCreateChild(CNID parID, const fs::path& path, const fs::path& filename,
        const fs::path& nameSource) override;
    virtual void deleteCNID(CNID cnid) override;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop) override;
};

}
