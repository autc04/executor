#pragma once

#include <util/macstrings.h>
#include <rsys/filesystem.h>
#include <FileMgr.h>
#include "filestamp.h"

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <optional>

namespace Executor
{
class ItemCache;
class LocalVolume;
class OpenFile;

using CNID = int32_t;

class Item;
class DirectoryItem;
class FileItem;

using ItemPtr = std::shared_ptr<Item>;
using DirectoryItemPtr = std::shared_ptr<DirectoryItem>;
using FileItemPtr = std::shared_ptr<FileItem>;

// A generated disambiguator is "#" plus at most this many digits. A group's
// truncated prefix must fit in the bytes a candidate keeps: 31 total, minus the
// ellipsis that marks the truncation, minus the suffix ("#" + digits).
// Otherwise two groups could generate the same candidate, and neither collision
// set would see the other.
inline constexpr size_t maxDisambiguatorDigits = 3;

// The largest index that still fits that bound: 0 is the plain name, 1..999
// give "#1".."#999". Entries that would need a larger index stay unnamed --
// and so invisible to the guest -- rather than minting a name that breaks the
// bound.
inline constexpr int maxDisambiguatorIndex = 999;

// The grouping key for a Mac name: its first 31 bytes, with a trailing ".bin"
// and/or "#<digits>" disambiguator stripped, truncated to leave room for a
// generated disambiguator, and case-folded. Two entries can collide iff their
// keys are equal, so over-grouping only costs laziness. See *Names* in the plan.
mac_string macNamePrefix(mac_string_view macName);

// The grouping key of a host filename.
mac_string hostGroupPrefix(const fs::path& posixName);

struct ItemInfo
{
    union {
        struct {
            FInfo info;
            FXInfo xinfo;
        } file;
        struct {
            DInfo info;
            DXInfo xinfo;
        } dir;
    };
    unsigned long modTime;
    unsigned long creationTime;
};

// The file-format concern, split out of FileItem so an Item can exist without
// knowing its encoding. A FileRep knows where the forks/Finder info live, how to
// open them, and which host files a delete/move must touch. The delete/move entry
// points stay on FileItem (the Item API); the rep supplies the format-specific
// host-file mutation so it can be deferred until Materialize.
class FileRep
{
protected:
    FileItem& item_;

    // Reject a move whose destination is occupied by a different host file. The
    // File Manager has already checked the guest namespace; this catches host
    // paths the guest name does not spell out (MacBinary's ".bin"). A
    // case-only rename on a case-insensitive host, or a rename onto a hard
    // link, denotes the same file and is allowed.
    void checkTargetFree(const fs::path& newPath) const;

public:
    explicit FileRep(FileItem& item) : item_(item) {}
    virtual ~FileRep() = default;

    virtual std::unique_ptr<OpenFile> open(int8_t permission) = 0;
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission) = 0;
    virtual void getInfo(ItemInfo& info) = 0;
    virtual void setInfo(const ItemInfo& info) = 0;

    // The filename the displayed Mac name is derived from. Reps that rename on
    // materialization (MacBinary drops a trailing ".bin") override this.
    // Note that DirectoryItem::nameSourceOf hard codes knowledge about which
    // reps may do that (as of this writing, MacBinary only).
    virtual fs::path nameSource(const fs::path& filename) const;

    // The inverse of nameSource: the host filename that stores the guest name
    // `macFilename`. MacBinary appends a trailing ".bin".
    virtual fs::path hostName(const fs::path& macFilename) const;

    // Host-file mutation. The default handles a single host file; formats that
    // keep sidecars (AppleDouble, Basilisk) override.
    virtual void remove();
    virtual void move(const fs::path& newPath);
};

using FileRepPtr = std::shared_ptr<FileRep>;

class Item : public std::enable_shared_from_this<Item>
{
protected:
    ItemCache& itemcache_;
    CNID parID_;
    CNID cnid_;
    fs::path path_;
    mac_string name_;             // decided at NameResolve; empty until then
    bool nameDecided_ = false;

    // The host-file half of moveItem: a plain rename by default; a file does it
    // through its rep, whose format decides which host files to touch.
    virtual void moveHost(const fs::path& newPath);

    // Called by relocate() after `path_` changed, so a subclass can drop state
    // derived from the old path (relocate() itself re-derives the Mac name).
    virtual void pathChanged() {}

public:
    Item(ItemCache& itemcache, CNID parID, CNID cnid, fs::path p, std::optional<mac_string> name = {});
    virtual ~Item();

    const fs::path& path() const { return path_; }

    // The host filename a guest name is stored under. This is the inverse of
    // the name rule a FileItem's rep supplies; a FileItem overrides it and the
    // default is the plain Mac-to-host conversion. `macName` is non-empty; the
    // "keep the current filename" move has no guest name to encode.
    virtual fs::path hostName(mac_string_view macName);

    CNID parID() const { return parID_; }
    CNID cnid() const { return cnid_; }

    // The Mac name. Deciding it is NameResolve, so reading it may list the
    // parent directory's `macNamePrefix` group on first use; every name read goes
    // through here, so a caller never observes an undecided name. Throws ioErr or
    // fnfErr if the name cannot be resolved.
    const mac_string& name();

    // The Mac name, set once NameResolve has decided it. `name()` is the reading
    // funnel; the naming machinery writes through decideName().
    void decideName(mac_string name) { name_ = std::move(name); nameDecided_ = true; }
    bool nameIsDecided() const { return nameDecided_; }

    virtual void deleteItem();

    // Move on the host (moveHost), then re-point at the new path and parent.
    // The parent is updated only after the host move succeeds.
    void moveItem(const fs::path& newPath, CNID newParent, mac_string_view newName);

    // Re-point a realized Item at a moved binding, keeping the object itself (so
    // an open FCB survives a host rename). The Mac name is re-derived only when
    // the entry's own name changed, not when an ancestor directory moved.
    void relocate(const fs::path& newPath);

    virtual ItemInfo getInfo();
    virtual void setInfo(ItemInfo info);

    // How much of the realization cache's budget this item weighs. A realized
    // item is one unit; a directory overrides this to also account for the
    // descriptors a listing retains, so one huge directory can outweigh many
    // small items. A relative measure for the LRU, not a byte count.
    virtual size_t cacheWeight() const { return 1; }
};


// The vocabulary a format probe records: the facts it relied on. These are pure
// data -- *how* each is re-obtained (or deliberately not) is policy, kept in
// FileItem::probeHolds, never in the observation itself. See *Rep staleness* in
// docs/ai/2026-10-08-localvolume-staged-materialization-plan.md.

// IsRegularFile(Path): the path is a regular file.
struct RegularFileObservation { fs::path path; };

// NoRegularFile(Path): the path is absent, or not a regular file. Recorded so a
// candidate that appears later (e.g. a sidecar) is noticed.
struct NoRegularFileObservation { fs::path path; };

// IsUnchanged(Path): the file still holds the content the probe read -- its
// (mtime, size) is the stamp taken when a content check decided the format. The
// policy ignores it as a whole (see FileItem::probeHolds): we trust the magic
// rather than re-read it, so a content change never invalidates a rep on its own.
struct UnchangedFileObservation { fs::path path; FileStamp stamp; };

// AttributeExists(Path, AttrName): the path carries the named attribute.
struct AttributeExistsObservation { fs::path path; const char* attrName; };

// AttributeDoesntExist(Path, AttrName): the path does not carry it.
struct AttributeDoesntExistObservation { fs::path path; const char* attrName; };

using ProbeObservation = std::variant<RegularFileObservation, NoRegularFileObservation,
    UnchangedFileObservation, AttributeExistsObservation, AttributeDoesntExistObservation>;

using ProbeTrace = std::vector<ProbeObservation>;

// The format gauntlet. A factory inspects a host file and, if it claims it,
// returns a FileRep for the (already-instantiated) FileItem. Directories are not
// handled here; ItemCache constructs them directly from the cheap "kind"
// decision. This runs at Materialize, not at Enumerate.
class ItemFactory
{
public:
    virtual ~ItemFactory() = default;

    virtual bool isHidden(const fs::directory_entry& e) { return false; }

    // `trace` collects the facts the probe relied on, so the choice can be
    // revalidated later (see the ProbeObservation vocabulary). The gauntlet
    // checks for a regular file once, up front, so a factory may assume it and
    // never records that fact itself.
    virtual FileRepPtr createRepForFile(ItemCache& itemcache, FileItem& item,
        const fs::directory_entry& e, ProbeTrace& trace) { return {}; }
    virtual void createFile(const fs::path& newPath)
        { throw std::logic_error("createFile unimplemented"); }
};

// A listed directory entry: the host name, its kind, the grouping key it sorts
// and resolves by, and its identity. It is not an Item — children are realized
// from descriptors on demand and dropped again when nothing holds them.
struct Descriptor
{
    fs::path posixName;
    bool isDir = false;
    mac_string prefix;   // case-folded group/sort key (Mac name without disambiguator)
    CNID cnid = 0;
};

class DirectoryItem : public Item
{
    std::vector<Descriptor> descriptors_;            // sorted by (prefix, posixName)
    bool    listed_ = false;
    FileStamp stamp_;
    std::optional<int> count_;   // Counted: a tally, without the descriptors
    FileStamp countStamp_;       // the stamp that tally is valid for

    void storeDescriptors(std::vector<Descriptor> descriptors, FileStamp stamp);
    bool isListed() const { return listed_; }

    // Select a descriptor from the (valid) listing, which this class owns the
    // "listed and valid" precondition of.
    const Descriptor* childByIndex(int index, bool includeDirectories) const;

    // Scan the host directory and reconcile it against the mapper (the diff),
    // re-pointing any live Item at its current path; sorted by (prefix, posixName).
    // Nothing is committed here: nullopt means the scan failed.
    std::optional<std::vector<Descriptor>> listDirectory();

    // The filename this entry's Mac name is derived from: the host name, unless
    // the entry is a ".bin" file whose rep renames it (MacBinary drops the
    // extension). The latter materializes the entry, keeping its rep for reuse.
    fs::path nameSourceOf(const Descriptor& d);

    DirectoryItemPtr self();

protected:
    virtual void moveHost(const fs::path& newPath) override;

public:
    using Item::Item;

    long dirID() const { return cnid(); }

    int countItems() const { return (int)descriptors_.size(); }

    size_t cacheWeight() const override
    {
        // A listing retains one descriptor per visible child, so it weighs as much
        // as that many items; an unlisted directory is one unit.
        return isListed() ? descriptors_.size() : 1;
    }

    const std::vector<Descriptor>& descriptors() const { return descriptors_; }

    // The [begin, end) range of descriptors whose (folded) prefix equals `prefix`.
    std::pair<size_t, size_t> groupRange(mac_string_view prefix) const;

    // --- Count / List / NameResolve: the directory-stage machinery ---

    // Count: the visible-child tally, without a descriptor list. List implies
    // Count. Throws ioErr if the directory cannot be read.
    int countVisible();

    // List: ensure the listing is present and valid, re-scanning the host if it
    // changed. False if the scan failed (the listing is dropped, so the next
    // access retries from scratch).
    bool ensureListed();

    // NameResolve: decide the Mac names of the `prefix` group of the (valid)
    // listing; and of the group holding `item`, returning its decided name.
    void resolveGroup(mac_string_view prefix);
    std::optional<mac_string> resolveName(const Item& item);

    // The lookup funnel (see the plan): the only way to query a directory's
    // contents. Ensures the listing, resolves the name's group, then returns the
    // child. Returns null if there is no such child; both throw ioErr if the
    // directory itself cannot be read.
    ItemPtr child(mac_string_view name);
    ItemPtr child(int index, bool includeDirectories);

    // Invalidation: drop the listing/tally (and re-cache, so the LRU's accounting
    // follows the shrunken weight) so the next access re-lists.
    void clearCache();

    virtual void deleteItem();
};

class FileItem : public Item
{
    friend class Executor::LocalVolume;
    short dataWriteAccessRefNum = -1;
    short resWriteAccessRefNum = -1;
    FileRepPtr rep_;
    // The facts the gauntlet relied on when it chose rep_, so the choice can be
    // revalidated cheaply (see ProbeObservation). probeStale_ is set when the
    // item's path changes, since the candidate set is path-derived.
    ProbeTrace probe_;
    bool probeStale_ = false;
    bool probeHolds() const;

    // Forks currently handed out (see ForkGuard). While any is open the rep is
    // neither revalidated nor dropped: the guest holds the fork, so a change
    // surfaces on reopen.
    int openForks_ = 0;

    // Wraps the fork open()/openRF() hand out, so this count follows its lifetime.
    class ForkGuard;

    virtual void moveHost(const fs::path& newPath) override;
    virtual void pathChanged() override;

public:
    using Item::Item;

    // Build the rep on first use (the format gauntlet). Idempotent.
    void ensureMaterialized();

    // Drop the rep and build it again. An encoding upgrade changes what the
    // gauntlet should pick without touching the file's own stamp.
    void rematerialize();

    // The filename this item's Mac name is derived from (MacBinary drops a
    // trailing ".bin"); materializes to ask the rep.
    fs::path nameSource(const fs::path& filename);

    // The host filename this item's guest name is stored under (MacBinary
    // adds a trailing ".bin"); materializes to ask the rep.
    virtual fs::path hostName(mac_string_view macName) override;

    virtual std::unique_ptr<OpenFile> open(int8_t permission);
    virtual std::unique_ptr<OpenFile> openRF(int8_t permission);
    virtual ItemInfo getInfo() override;
    virtual void setInfo(ItemInfo info) override;

    virtual void deleteItem() override;
};

}
