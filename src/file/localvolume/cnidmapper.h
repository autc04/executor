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
// (and back, by host inode), and holds each directory's child set. It is a
// deliberately dumb value store: resolving and staleness *policy* lives in
// ItemCache. See *Component changes / `CNIDMapper`* in
// docs/ai/2026-10-08-localvolume-staged-materialization-plan.md.
class CNIDMapper
{
public:
    virtual ~CNIDMapper() = default;

    // One entry, as seen by ItemCache. The authoritative record lives in the
    // mapper; this is a copy.
    struct Mapping
    {
        CNID parID;
        CNID cnid;
        fs::path path;
        mac_string macname;
        FileIdentity identity;
        bool isDir;
    };

    // A host entry offered to the per-directory diff. `nameSource` is the
    // filename the Mac name is derived from (differs from the host name only
    // where a rep renames; MacBinary drops a trailing ".bin").
    struct DirEntry
    {
        fs::path path;
        fs::path nameSource;
        FileIdentity identity;
        std::uint64_t mtime;
        bool isDir;
    };

    // Reconcile a fresh scan of `dirID` against its stored children, keyed by
    // host inode: an inode seen again at the same name keeps its CNID and Mac
    // name; an inode seen under a new name is a rename (a new CNID, until
    // Phase 11); a scanned inode with no record is new; a stored child absent
    // from the scan is gone. Returns the surviving entries in scan order.
    virtual std::vector<Mapping> mapDirectoryContents(CNID dirID, std::vector<DirEntry> entries) = 0;

    // The record for a CNID, or nothing. No host validation is done here; the
    // caller validates (see ItemCache). 
    virtual std::optional<Mapping> lookupCNID(CNID cnid) = 0;

    // The CNID for the child of `parID` living at `path`, creating a record (and
    // picking a Mac name) when there is none. Identity is preferred over the
    // path; a different inode at an existing path is delete + create.
    virtual CNID resolveChild(CNID parID, const fs::path& path, const fs::path& nameSource,
        const FileIdentity& identity, bool isDir, std::uint64_t mtime) = 0;

    // Re-key a CNID to a (possibly different) host inode without changing the
    // CNID. Used when the host file behind a CNID is replaced in place but the
    // CNID must be preserved (an encoding upgrade).
    virtual void setIdentity(CNID cnid, const FileIdentity& identity) = 0;

    virtual void deleteCNID(CNID cnid) = 0;
    virtual void moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop) = 0;
};

}
