#pragma once

#include "cnidmapper.h"
#include <map>
#include <set>
#include <string>
#include <unordered_map>

namespace Executor
{

class SimpleCNIDMapper : public CNIDMapper
{
    CNID nextCNID_ = 3;

    // The record, exactly as ItemCache sees it. No host path is stored; the path
    // is derived from the parID chain (see ItemCache::pathOf).
    std::unordered_map<CNID, Mapping> mappings_;

    // parID -> (host name -> child CNID): a directory's child set *and* the
    // (parID, posixName) -> cnid lookup. There is no inode-keyed index: identity
    // lives in the record, and inode pairing is built from the scan while
    // re-listing a directory. See *Binding staleness* in the plan.
    std::unordered_map<CNID, std::map<fs::path, CNID>> posixNames_;

    // parID -> (case-folded Mac name -> cnid), populated at NameResolve.
    std::unordered_map<CNID, std::map<mac_string, CNID>> names_;

    void forgetRecordName(Mapping& rec);

public:
    explicit SimpleCNIDMapper(mac_string volumeName);

    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<DirEntry> entries) override;
    virtual std::optional<Mapping> lookupCNID(CNID cnid) override;
    virtual std::optional<Mapping> lookupPosixName(CNID parID, const fs::path& posixName) override;
    virtual CNID createRecord(CNID parID, const fs::path& posixName,
        const FileIdentity& identity, bool isDir) override;
    virtual std::optional<CNID> cnidForName(CNID parID, mac_string_view macName) override;
    virtual void nameGroup(CNID parID, const std::vector<NameRequest>& group) override;
    virtual void deleteCNID(CNID cnid) override;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop) override;
};

}
