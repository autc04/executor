#pragma once

#include "cnidmapper.h"
#include <map>
#include <string>
#include <unordered_map>

namespace Executor
{

// The in-memory reference backend. It stores records, the parent-keyed posix
// name index and the parent-keyed Mac name index; all policy lives in
// ItemCache.
class SimpleCNIDMapper : public CNIDMapperBackend
{
    CNID nextCNID_ = 3;

    std::unordered_map<CNID, Mapping> mappings_;

    // parID -> (host name -> child CNID): a directory's child set and the
    // (parID, posixName) -> cnid lookup.
    std::unordered_map<CNID, std::map<fs::path, CNID>> posixNames_;

    // parID -> (case-folded Mac name -> cnid), populated at NameResolve.
    std::unordered_map<CNID, std::map<mac_string, CNID>> names_;

public:
    explicit SimpleCNIDMapper(mac_string volumeName);

    virtual std::optional<Mapping> get(CNID cnid) override;
    virtual std::optional<Mapping> getChild(CNID parID, const fs::path& posixName) override;
    virtual std::vector<std::pair<fs::path, CNID>> getChildren(CNID parID) override;
    virtual std::optional<CNID> getName(CNID parID, mac_string_view foldedMacName) override;

    virtual void put(const Mapping& record) override;
    virtual void erase(CNID cnid) override;
    virtual void putChild(CNID parID, const fs::path& posixName, CNID cnid) override;
    virtual void eraseChild(CNID parID, const fs::path& posixName) override;
    virtual void putName(CNID parID, mac_string_view foldedMacName, CNID cnid) override;
    virtual void eraseName(CNID parID, mac_string_view foldedMacName) override;
    virtual CNID allocate() override;
};

} // namespace Executor
