#pragma once

#include "cnidmapper.h"
#include <set>
#include <map>
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
        FileIdentity identity;
        bool isDir = false;
        std::uint64_t mtime = 0;
    };
    std::unordered_map<CNID, StoredMapping> mappings_;

    // Directory contents: parID -> set of child CNIDs. The reconciliation diff
    // and the names-in-use set both read it.
    using ChildSet = std::set<CNID>;
    std::unordered_map<CNID, ChildSet> children_;

    // Identity index: (device, inode) -> cnid, for entries with a usable inode.
    std::map<std::pair<std::uint64_t, std::uint64_t>, CNID> inodeIndex_;

    std::set<mac_string> namesInUse(CNID dirID);
    mac_string chooseMacName(const fs::path& filename, std::set<mac_string>& usedNames);
    std::optional<CNID> childByPath(CNID parID, const fs::path& path) const;
    CNID createRecord(CNID parID, const fs::path& path, const fs::path& nameSource,
        const FileIdentity& identity, bool isDir, std::uint64_t mtime);

public:
    explicit SimpleCNIDMapper(fs::path root, mac_string volumeName);

    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<DirEntry> entries) override;
    virtual std::optional<Mapping> lookupCNID(CNID cnid) override;
    virtual CNID resolveChild(CNID parID, const fs::path& path, const fs::path& nameSource,
        const FileIdentity& identity, bool isDir, std::uint64_t mtime) override;
    virtual void setIdentity(CNID cnid, const FileIdentity& identity) override;
    virtual void deleteCNID(CNID cnid) override;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop) override;
};

}
