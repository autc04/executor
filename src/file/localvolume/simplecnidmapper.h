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
        std::optional<mac_string> macname;
        FileIdentity identity;
        bool isDir = false;
        std::uint64_t mtime = 0;
    };
    std::unordered_map<CNID, StoredMapping> mappings_;

    // Directory contents: parID -> set of child CNIDs. The reconciliation diff
    // and the naming groups both read it.
    using ChildSet = std::set<CNID>;
    std::unordered_map<CNID, ChildSet> children_;

    // Identity index: (device, inode) -> cnid, for entries with a usable inode.
    std::map<std::pair<std::uint64_t, std::uint64_t>, CNID> inodeIndex_;

    // Name index: parID -> (case-folded Mac name -> cnid), for decided names.
    std::unordered_map<CNID, std::map<mac_string, CNID>> names_;

    std::optional<CNID> childByPath(CNID parID, const fs::path& path) const;
    CNID createRecord(CNID parID, const fs::path& path,
        const FileIdentity& identity, bool isDir, std::uint64_t mtime);
    void forgetRecordName(StoredMapping& rec);

public:
    explicit SimpleCNIDMapper(fs::path root, mac_string volumeName);

    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<DirEntry> entries) override;
    virtual std::optional<Mapping> lookupCNID(CNID cnid) override;
    virtual CNID resolveChild(CNID parID, const fs::path& path,
        const FileIdentity& identity, bool isDir, std::uint64_t mtime) override;
    virtual std::optional<CNID> cnidForName(CNID parID, mac_string_view macName) override;
    virtual void nameGroup(CNID parID, const std::vector<NameRequest>& group) override;
    virtual void deleteCNID(CNID cnid) override;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop) override;
};

}
