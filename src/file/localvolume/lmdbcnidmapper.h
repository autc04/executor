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

// Persistent storage backend: one LMDB environment per volume, keyed by a hash
// of the volume root. It stores records, the parent-keyed posix-name index and
// the parent-keyed Mac-name index. All reconciliation and naming policy lives
// in ItemCache; this class provides the primitives and the transaction seam.
class LMDBCNIDMapper : public CNIDMapperBackend
{
    lmdb::env env_;
    lmdb::dbi config_;
    lmdb::dbi mappings_;
    lmdb::dbi posixNames_;
    lmdb::dbi names_;

    // Set while withTransaction's callback runs, so every primitive uses the
    // same write transaction.
    lmdb::txn *activeTxn_ = nullptr;

    // Record and index primitives. All take the caller's transaction.
    std::optional<Mapping> getMapping(lmdb::txn& txn, CNID cnid);
    void putMapping(lmdb::txn& txn, const Mapping& rec);
    CNID allocateCNID(lmdb::txn& txn);

    std::optional<CNID> getPosixName(lmdb::txn& txn, CNID parID, const fs::path& name);
    void putPosixName(lmdb::txn& txn, CNID parID, const fs::path& name, CNID cnid);
    void delPosixName(lmdb::txn& txn, CNID parID, const fs::path& name);
    std::vector<std::pair<fs::path, CNID>> getChildren(lmdb::txn& txn, CNID parID);

    std::optional<CNID> getName(lmdb::txn& txn, CNID parID, mac_string_view folded);
    void putName(lmdb::txn& txn, CNID parID, mac_string_view folded, CNID cnid);
    void delName(lmdb::txn& txn, CNID parID, mac_string_view folded);

public:
    LMDBCNIDMapper(fs::path root, mac_string volumeName);
    ~LMDBCNIDMapper();

    // Readable, deterministic directory name for a volume root: "root" for
    // "/", "drive-C" for "C:/", and "path-..." for anything else. The config
    // DB stores the canonical root, so a name collision is detected rather than
    // silently mixing volumes.
    static fs::path volumeDirectoryName(const fs::path& root);

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

    virtual void withTransaction(const std::function<void()>& f) override;
};

} // namespace Executor
