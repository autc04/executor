#pragma once

#include "cnidmapper.h"

#include <memory>
#include <lmdb++.h>


namespace Executor
{

class LMDBCNIDMapper : public CNIDMapper
{
    lmdb::env env_;
    lmdb::dbi config_;
    lmdb::dbi mappings_;
    lmdb::dbi directories_;

    struct StoredMapping
    {
        CNID parID;
        CNID cnid;
        fs::path path;
        mac_string macname;
    };

    class BinMapping;
    StoredMapping decodeMapping(CNID cnid, lmdb::val val);

    std::optional<StoredMapping> getMapping(lmdb::txn& txn, CNID cnid);
    void setMapping(lmdb::txn& txn, StoredMapping m);
    void deleteMapping(lmdb::txn& txn, CNID cnid);
    std::vector<CNID> getDirectory(lmdb::txn& txn, CNID cnid);
    void setDirectory(lmdb::txn& txn, CNID cnid, const std::vector<CNID>& contents);
    
    void deleteCNID(lmdb::txn& txn, CNID cnid);

    std::vector<Mapping> updateDirectoryContents(CNID dirID, std::vector<fs::directory_entry> realPaths);
public:
    LMDBCNIDMapper(fs::path root, mac_string volumeName);
    ~LMDBCNIDMapper();

    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<fs::directory_entry> realPaths) override;
    virtual std::optional<Mapping> lookupCNID(CNID cnid) override;
    // Not implemented yet; the LMDB backend is disabled and will be reworked in
    // Phase 7 of docs/ai/2026-10-08-localvolume-staged-materialization-plan.md.
    virtual std::optional<Mapping> lookupChild(CNID parID, const fs::path& filename) override;
    virtual Mapping getOrCreateChild(CNID parID, const fs::path& path, const fs::path& filename) override;
    virtual void deleteCNID(CNID cnid) override;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop) override;
};

}