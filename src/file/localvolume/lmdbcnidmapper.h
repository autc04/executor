#pragma once

#include "cnidmapper.h"

#include <lmdb++.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Executor
{

// Persistent CNID/name store: one LMDB environment per volume, keyed by a hash of
// the volume root. The record table and the two parent-keyed indexes mirror
// SimpleCNIDMapper. `identity` is a recorded observation used for validation and
// rename pairing, never a lookup key. Every read-modify-write (the per-directory
// diff, get-or-create, nameGroup, delete, move) runs in one write transaction, so
// instances sharing the environment converge. See Phase 11 in
// docs/ai/2026-10-08-localvolume-staged-materialization-plan.md.
class LMDBCNIDMapper : public CNIDMapper
{
public:
    // One record, as stored. The host path is derived from the parID chain.
    struct StoredMapping
    {
        CNID parID = 0;
        CNID cnid = 0;
        fs::path posixName;
        std::optional<mac_string> macname;
        FileIdentity identity;
        bool isDir = false;
    };

private:
    lmdb::env env_;
    lmdb::dbi config_;
    lmdb::dbi mappings_;
    lmdb::dbi posixNames_;
    lmdb::dbi names_;

    // Record and index primitives. All take the caller's transaction, so a
    // read-modify-write can be one write transaction.
    std::optional<StoredMapping> getMapping(lmdb::txn& txn, CNID cnid);
    void putMapping(lmdb::txn& txn, const StoredMapping& rec);
    CNID allocateCNID(lmdb::txn& txn);

    std::optional<CNID> getPosixName(lmdb::txn& txn, CNID parID, const fs::path& name);
    void putPosixName(lmdb::txn& txn, CNID parID, const fs::path& name, CNID cnid);
    void delPosixName(lmdb::txn& txn, CNID parID, const fs::path& name);
    std::vector<std::pair<fs::path, CNID>> getChildren(lmdb::txn& txn, CNID parID);

    std::optional<CNID> getName(lmdb::txn& txn, CNID parID, mac_string_view folded);
    void putName(lmdb::txn& txn, CNID parID, mac_string_view folded, CNID cnid);
    void delName(lmdb::txn& txn, CNID parID, mac_string_view folded);

    void forgetRecordName(lmdb::txn& txn, const StoredMapping& rec);

    CNID createRecord(lmdb::txn& txn, CNID parID, const fs::path& posixName,
        const FileIdentity& identity, bool isDir);
    void deleteCNID(lmdb::txn& txn, CNID cnid);

public:
    LMDBCNIDMapper(fs::path root, mac_string volumeName);
    ~LMDBCNIDMapper();

    // Readable, deterministic directory name for a volume root: "root" for
    // "/", "drive-C" for "C:/", and "path-..." for anything else. The config
    // DB stores the canonical root, so a name collision is detected rather than
    // silently mixing volumes.
    static fs::path volumeDirectoryName(const fs::path& root);

    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID,
        std::vector<DirEntry> entries) override;
    virtual std::optional<Mapping> lookupCNID(CNID cnid) override;
    virtual std::optional<Mapping> lookupPosixName(CNID parID,
        const fs::path& posixName) override;
    virtual CNID createRecord(CNID parID, const fs::path& posixName,
        const FileIdentity& identity, bool isDir) override;
    virtual CNID getOrCreateChild(CNID parID, const fs::path& posixName,
        const FileIdentity& identity, bool isDir) override;
    virtual std::optional<CNID> cnidForName(CNID parID, mac_string_view macName) override;
    virtual void nameGroup(CNID parID, const std::vector<NameRequest>& group) override;
    virtual void deleteCNID(CNID cnid) override;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName,
        const fs::path& newPosixName) override;
};

}
