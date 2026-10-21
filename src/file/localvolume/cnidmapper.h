#pragma once

#include "filestamp.h"
#include <rsys/filesystem.h>
#include <util/macstrings.h>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace Executor
{

using CNID = int32_t;

// The storage backend for CNID/name mappings. It is a dumb value store: the
// reconciliation, get-or-create and naming policy live in ItemCache. A backend
// provides primitive reads and writes, plus one transaction seam so ItemCache
// can run a compound read-modify-write atomically.
class CNIDMapperBackend
{
public:
    virtual ~CNIDMapperBackend() = default;

    // One record, as stored. The authoritative record lives in the backend;
    // this is a copy. `macname` is empty until NameResolve. No host path is
    // carried: it is derived from the `parID` chain plus `posixName`.
    struct Mapping
    {
        CNID parID;
        CNID cnid;
        fs::path posixName;
        std::optional<mac_string> macname;
        FileIdentity identity;
        bool isDir;
    };

    // --- read primitives ---

    // The record for a CNID, or nothing.
    virtual std::optional<Mapping> get(CNID cnid) = 0;

    // The record for the child of `parID` stored under `posixName`, or nothing.
    virtual std::optional<Mapping> getChild(CNID parID, const fs::path& posixName) = 0;

    // The directory's child set: one (posixName, cnid) pair per stored child.
    virtual std::vector<std::pair<fs::path, CNID>> getChildren(CNID parID) = 0;

    // The child of `parID` whose decided Mac name is `foldedMacName`, or nothing.
    virtual std::optional<CNID> getName(CNID parID, mac_string_view foldedMacName) = 0;

    // --- write primitives ---

    virtual void put(const Mapping& record) = 0;
    virtual void erase(CNID cnid) = 0;
    virtual void putChild(CNID parID, const fs::path& posixName, CNID cnid) = 0;
    virtual void eraseChild(CNID parID, const fs::path& posixName) = 0;
    virtual void putName(CNID parID, mac_string_view foldedMacName, CNID cnid) = 0;
    virtual void eraseName(CNID parID, mac_string_view foldedMacName) = 0;
    virtual CNID allocate() = 0;

    // Run `f` atomically. The default is suitable for an in-memory backend;
    // LMDB overrides it to open a write transaction and to retry on map growth.
    // The callback must be re-runnable: a retry starts from the state before it.
    virtual void withTransaction(const std::function<void()>& f) { f(); }
};

// A short alias for the record type used by callers that only need the value.
using CNIDRecord = CNIDMapperBackend::Mapping;

} // namespace Executor
