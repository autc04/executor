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
    fs::path rootPath_;
    CNID nextCNID_ = 3;

    struct StoredMapping
    {
        CNID parID;
        CNID cnid;
        fs::path posixName;   // the host name; the full path is derived
        std::optional<mac_string> macname;
        FileIdentity identity;
        bool isDir = false;
        std::uint64_t mtime = 0;
    };
    std::unordered_map<CNID, StoredMapping> mappings_;

    // Directory contents: parID -> set of child CNIDs.
    using ChildSet = std::set<CNID>;
    std::unordered_map<CNID, ChildSet> children_;

    // Identity index: (device, inode) -> the CNIDs bound to it. A regular file
    // has one binding; a hard-linked inode has one per name.
    std::map<std::pair<std::uint64_t, std::uint64_t>, ChildSet> bindings_;

    // Name index: parID -> (case-folded Mac name -> cnid), for decided names.
    std::unordered_map<CNID, std::map<mac_string, CNID>> names_;

    fs::path pathOf(CNID cnid) const;
    std::optional<CNID> childByName(CNID parID, const fs::path& posixName) const;
    CNID createRecord(CNID parID, const fs::path& posixName,
        const FileIdentity& identity, bool isDir, std::uint64_t mtime);
    void setRecordIdentity(StoredMapping& rec, const FileIdentity& identity);
    void addBinding(const StoredMapping& rec);
    void forgetRecordName(StoredMapping& rec);
    Mapping mappingFor(CNID cnid, const fs::path& dirPath) const;

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
