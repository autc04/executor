#pragma once

#include "filestamp.h"
#include <rsys/filesystem.h>
#include <util/macstrings.h>
#include <optional>
#include <vector>

namespace Executor
{

using CNID = int32_t;

// The identity and naming store. It maps a CNID to the host entry it denotes
// (and back, by host inode), holds each directory's child set, and remembers
// decided Mac names. It is a deliberately dumb value store: resolving and
// staleness *policy* lives in ItemCache. See *Component changes / `CNIDMapper`*
// in docs/ai/2026-10-08-localvolume-staged-materialization-plan.md.
class CNIDMapper
{
public:
    virtual ~CNIDMapper() = default;

    // One entry, as seen by ItemCache. The authoritative record lives in the
    // mapper; this is a copy. `macname` is empty until NameResolve. No host path
    // is carried: it is derived from the `parID` chain plus `posixName`.
    struct Mapping
    {
        CNID parID;
        CNID cnid;
        fs::path posixName;
        std::optional<mac_string> macname;
        FileIdentity identity;
        bool isDir;
    };

    // A host entry offered to the per-directory diff. Only the name is kept: the
    // entry's full path is derived from the parent's chain when needed.
    struct DirEntry
    {
        fs::path posixName;
        FileIdentity identity;
        bool isDir;
    };

    // One member of a name group: the entry to name, the binding the nameSource
    // was derived from (so a name request can be dropped if another instance
    // renamed that binding after the listing), and the filename its Mac name is
    // derived from (MacBinary drops a trailing ".bin").
    struct NameRequest
    {
        CNID cnid;
        fs::path posixName;
        fs::path nameSource;
    };

    // Reconcile a fresh scan of `dirID` against its stored children, per
    // *Reconciliation* in the plan: a child whose name and kind are unchanged
    // keeps its CNID and Mac name; a child with exactly one occurrence at a new
    // name was renamed (keep the CNID, adopt the name, drop the Mac name); a
    // child with no occurrence is gone; a child with several unmatched
    // occurrences is dropped and each occurrence becomes a new entry. A
    // same-inode record in another directory is never a match. No names are
    // decided. Returns entries in scan order (kept/renamed first).
    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<DirEntry> entries) = 0;

    // The record for a CNID, or nothing. No host validation is done here; the
    // caller validates (see ItemCache). 
    virtual std::optional<Mapping> lookupCNID(CNID cnid) = 0;

    // The record for the child of `parID` stored under `posixName`, or nothing.
    // A pure lookup: the mapper neither validates nor creates.
    virtual std::optional<Mapping> lookupPosixName(CNID parID, const fs::path& posixName) = 0;

    // Bind `posixName` under `parID` to a fresh CNID. Replacing a child means
    // delete then create, which the caller does; the mapper only assigns.
    virtual CNID createRecord(CNID parID, const fs::path& posixName,
        const FileIdentity& identity, bool isDir) = 0;

    // CNIDResolve as one atomic read-modify-write: the CNID for the child of
    // `parID` stored under `posixName`, creating a record when there is none.
    // The same identity and kind reuse the CNID; any other file at that name is
    // a different item, so the old record is dropped and a fresh CNID assigned.
    // No name is decided. This is one transaction so two instances cannot mint
    // two CNIDs for one binding.
    virtual CNID getOrCreateChild(CNID parID, const fs::path& posixName,
        const FileIdentity& identity, bool isDir) = 0;

    // The child of `parID` whose decided Mac name is `macName` (case-folded), or
    // nothing.
    virtual std::optional<CNID> cnidForName(CNID parID, mac_string_view macName) = 0;

    // Decide names for one `macNamePrefix` group at once: assign each request a
    // Mac name that does not collide with any name already decided in `parID`.
    // Already-decided names are left untouched.
    virtual void nameGroup(CNID parID, const std::vector<NameRequest>& group) = 0;

    virtual void deleteCNID(CNID cnid) = 0;
    // Re-home a binding. The caller has already performed the host rename and
    // passes the resulting host name; this is a pure mapper update, so no host
    // I/O runs inside the transaction.
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName,
        const fs::path& newPosixName) = 0;
};

}
