#pragma once

#include "filestamp.h"
#include <rsys/filesystem.h>
#include <util/macstrings.h>
#include <functional>
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
    // mapper; this is a copy. `macname` is empty until NameResolve.
    struct Mapping
    {
        CNID parID;
        CNID cnid;
        fs::path path;
        std::optional<mac_string> macname;
        FileIdentity identity;
        bool isDir;
    };

    // A host entry offered to the per-directory diff.
    struct DirEntry
    {
        fs::path path;
        FileIdentity identity;
        std::uint64_t mtime;
        bool isDir;
    };

    // One member of a name group: the entry to name and the filename its Mac
    // name is derived from (MacBinary drops a trailing ".bin").
    struct NameRequest
    {
        CNID cnid;
        fs::path nameSource;
    };

    // Reconcile a fresh scan of `dirID` against its stored children, keyed by
    // host inode: an inode seen again at the same name keeps its CNID and Mac
    // name; an inode seen under a new name is a rename (a new CNID, until
    // Phase 11); a scanned inode with no record is new; a stored child absent
    // from the scan is gone. No names are decided. Returns the surviving entries
    // in scan order.
    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<DirEntry> entries) = 0;

    // The record for a CNID, or nothing. No host validation is done here; the
    // caller validates (see ItemCache). 
    virtual std::optional<Mapping> lookupCNID(CNID cnid) = 0;

    // The CNID for the child of `parID` living at `path`, creating a record when
    // there is none. Identity is preferred over the path; a different inode at an
    // existing path is delete + create. No name is decided.
    virtual CNID resolveChild(CNID parID, const fs::path& path,
        const FileIdentity& identity, bool isDir, std::uint64_t mtime) = 0;

    // The child of `parID` whose decided Mac name is `macName` (case-folded), or
    // nothing.
    virtual std::optional<CNID> cnidForName(CNID parID, mac_string_view macName) = 0;

    // Decide names for one `macNamePrefix` group at once: assign each request a
    // Mac name that does not collide with any name already decided in `parID`.
    // Already-decided names are left untouched.
    virtual void nameGroup(CNID parID, const std::vector<NameRequest>& group) = 0;

    // Re-key a CNID to a (possibly different) host inode without changing the
    // CNID. Used when the host file behind a CNID is replaced in place but the
    // CNID must be preserved (an encoding upgrade).
    virtual void setIdentity(CNID cnid, const FileIdentity& identity) = 0;

    virtual void deleteCNID(CNID cnid) = 0;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop) = 0;
};

}
