#include "item.h"
#include "itemcache.h"
#include "cnidmapper.h"
#include "openfile.h"
#include "stats.h"

#include <OSUtil.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <iostream>
#include <variant>

#ifdef __APPLE__
#include <sys/xattr.h>
#endif

using namespace Executor;

namespace
{
// A generated disambiguator ("#123") is at most this many digits; a group's
// truncated prefix leaves room for one.
constexpr size_t maxDisambiguatorDigits = 3;

bool endsWithBin(const mac_string& s)
{
    if(s.size() < 4)
        return false;
    return s[s.size()-4] == (unsigned char)'.'
        && std::tolower(s[s.size()-3]) == 'b'
        && std::tolower(s[s.size()-2]) == 'i'
        && std::tolower(s[s.size()-1]) == 'n';
}

bool endsWithDisambiguator(const mac_string& s, size_t& hashPos)
{
    size_t hash = s.rfind((unsigned char)'#');
    if(hash == mac_string::npos || hash + 1 >= s.size())
        return false;

    for(size_t i = hash + 1; i < s.size(); i++)
        if(s[i] < (unsigned char)'0' || s[i] > (unsigned char)'9')
            return false;

    hashPos = hash;
    return true;
}

// Seconds between the Mac epoch (1904) and the Unix epoch (1970); a Finder
// modification time is the host mtime shifted by this.
constexpr int64_t macToUnixEpoch = 86400 * (365 * (1970 - 1904) + (1970 - 1904) / 4);

int64_t macModTime(const fs::path& path)
{
    return fs::last_write_time(path) + macToUnixEpoch;
}
}

mac_string Executor::macNamePrefix(mac_string_view macName)
{
    mac_string s { macName };
    if(s.size() > 31)
        s.resize(31);

    for(;;)
    {
        if(endsWithBin(s))
        {
            s.resize(s.size() - 4);
            continue;
        }
        size_t hash;
        if(endsWithDisambiguator(s, hash))
        {
            s.resize(hash);
            continue;
        }
        break;
    }

    const size_t maxPrefix = 30 - maxDisambiguatorDigits;
    if(s.size() > maxPrefix)
        s.resize(maxPrefix);

    ROMlib_UprString(s.data(), false, s.size());
    return s;
}

mac_string Executor::hostGroupPrefix(const fs::path& posixName)
{
    return macNamePrefix(toMacRomanFilename(posixName, 0));
}

Item::Item(ItemCache& itemcache, CNID parID, CNID cnid, fs::path p, std::optional<mac_string> name)
    : itemcache_(itemcache), parID_(parID), cnid_(cnid), path_(std::move(p))
{
    if(name)
    {
        name_ = std::move(*name);
        nameDecided_ = true;
    }
}

Item::~Item()
{
    itemcache_.noteItemFreed(cnid_);
}

const mac_string& Item::name()
{
    itemcache_.ensureNamed(*this);
    return name_;
}

void Item::deleteItem()
{
    fs::remove(path());
}

void Item::moveItem(const fs::path& newPath, mac_string_view newName)
{
    moveHost(newPath);
    path_ = newPath;
    if(!newName.empty())
        decideName(mac_string(newName));
}

void Item::moveHost(const fs::path& newPath)
{
    fs::rename(path(), newPath);
}

void Item::relocate(const fs::path& newPath)
{
    if(path_ == newPath)
        return;

    if(path_.filename() != newPath.filename())
    {
        name_.clear();
        nameDecided_ = false;
    }

    path_ = newPath;
    pathChanged();
}

ItemInfo Item::getInfo()
{
    ItemInfo info{};
    info.modTime = macModTime(path());

    return info;
}

void Item::setInfo(ItemInfo info)
{
    if(macModTime(path()) != (int64_t)info.modTime)
        fs::last_write_time(path(), info.modTime - macToUnixEpoch);
}

void FileRep::remove()
{
    fs::remove(item_.path());
}

fs::path FileRep::nameSource(const fs::path& filename) const
{
    return filename;
}

void FileRep::move(const fs::path& newPath)
{
    fs::rename(item_.path(), newPath);
}

// Wraps the fork a FileItem hands out so the item's open-fork count follows the
// fork's lifetime (see ensureMaterialized).
class FileItem::ForkGuard : public OpenFile
{
    std::unique_ptr<OpenFile> inner_;
    FileItem& item_;

public:
    ForkGuard(std::unique_ptr<OpenFile> inner, FileItem& item)
        : inner_(std::move(inner)), item_(item)
    {
        ++item_.openForks_;
    }
    ~ForkGuard() override { --item_.openForks_; }

    size_t getEOF() override { return inner_->getEOF(); }
    void setEOF(size_t sz) override { inner_->setEOF(sz); }
    size_t read(size_t offset, void* p, size_t n) override { return inner_->read(offset, p, n); }
    size_t write(size_t offset, void* p, size_t n) override { return inner_->write(offset, p, n); }
    bool canWrite() const override { return inner_->canWrite(); }
};

namespace
{
// The revalidation policy: how each recorded fact is re-obtained. Note
// UnchangedFileObservation -- the content the probe read is deliberately not read
// again, so a rep built from a since-modified file survives until an existence or
// attribute fact flips (see *Rep staleness* in the plan). The attribute facts are
// macOS xattrs; no factory records them elsewhere.
bool attributeExists(const fs::path& path, const char* attrName)
{
#ifdef __APPLE__
    return getxattr(path.string().c_str(), attrName, nullptr, 0, 0, 0) > 0;
#else
    (void)path;
    (void)attrName;
    return false;
#endif
}

bool holds(const RegularFileObservation& o) { return fs::is_regular_file(o.path); }
bool holds(const NoRegularFileObservation& o) { return !fs::is_regular_file(o.path); }
bool holds(const UnchangedFileObservation& o) { return statFile(o.path) == o.stamp; }
bool holds(const AttributeExistsObservation& o) { return attributeExists(o.path, o.attrName); }
bool holds(const AttributeDoesntExistObservation& o) { return !attributeExists(o.path, o.attrName); }
}

bool FileItem::probeHolds() const
{
    for(const ProbeObservation& o : probe_)
    {
        bool factHolds = std::visit([](const auto& fact) { return holds(fact); }, o);

        // An IsUnchanged fact is ignored as a whole: we trust the content the probe
        // read, so a change to it does not invalidate the rep. Every other fact must
        // still hold. See *Rep staleness* in the plan.
        if(!factHolds && !std::holds_alternative<UnchangedFileObservation>(o))
            return false;
    }
    return true;
}

void FileItem::pathChanged()
{
    probeStale_ = true;   // the candidate set is derived from the path
}

void FileItem::ensureMaterialized()
{
    if(rep_)
    {
        // Never rebuild while a fork we handed out is open: the guest holds it, so
        // a change surfaces on reopen, and rebuilding now would strand the parsed
        // file object that fork is using and let a second one appear beside it.
        if(openForks_ > 0)
            return;

        // The rep is a function of what the gauntlet probed. Re-test the facts we
        // can (the candidates' existence); the content checks are trusted. A path
        // change invalidates the whole trace, since the candidates are path-derived.
        if(!probeStale_ && probeHolds())
            return;

        rep_.reset();
    }
    probe_.clear();
    probeStale_ = false;
    rep_ = itemcache_.itemFactory().createRepForFile(itemcache_, *this, fs::directory_entry(path_), probe_);
    assert(rep_);
}

void FileItem::rematerialize()
{
    rep_.reset();
    ensureMaterialized();
}

fs::path FileItem::nameSource(const fs::path& filename)
{
    ensureMaterialized();
    return rep_->nameSource(filename);
}

std::unique_ptr<OpenFile> FileItem::open(int8_t permission)
{
    ensureMaterialized();
    return std::make_unique<ForkGuard>(rep_->open(permission), *this);
}

std::unique_ptr<OpenFile> FileItem::openRF(int8_t permission)
{
    ensureMaterialized();
    return std::make_unique<ForkGuard>(rep_->openRF(permission), *this);
}

ItemInfo FileItem::getInfo()
{
    ensureMaterialized();
    ItemInfo info = Item::getInfo();
    rep_->getInfo(info);
    return info;
}

void FileItem::setInfo(ItemInfo info)
{
    ensureMaterialized();
    Item::setInfo(info);
    rep_->setInfo(info);
}

void FileItem::deleteItem()
{
    ensureMaterialized();
    rep_->remove();
}

void FileItem::moveHost(const fs::path& newPath)
{
    ensureMaterialized();
    rep_->move(newPath);
}

DirectoryItemPtr DirectoryItem::self()
{
    return std::static_pointer_cast<DirectoryItem>(shared_from_this());
}

int DirectoryItem::countVisible()
{
    // List implies Count.
    if(listed_)
        return countItems();

    FileStamp stamp = statFile(path());
    if(count_ && countStamp_ == stamp)
    {
        itemcache_.cacheItem(self());
        return *count_;
    }

    int count = 0;
    localVolumeStats().countScans++;
    try
    {
        ItemFactory& factory = itemcache_.itemFactory();
        for(const auto& e : fs::directory_iterator(path()))
            if(!factory.isHidden(e))
                ++count;
    }
    catch(const boost::filesystem::filesystem_error& exc)
    {
        // A failed scan is not a tally, so report it rather than caching a wrong
        // count as valid.
        std::cerr << exc.what() << std::endl;
        throw OSErrorException(ioErr);
    }

    count_ = count;
    countStamp_ = stamp;   // as for a listing: keyed to the state we started
    itemcache_.cacheItem(self());
    return *count_;
}

std::optional<std::vector<Descriptor>> DirectoryItem::listDirectory()
{
    // Re-scan in two passes. The readdir runs to completion before any entry is
    // stat'ed, so the directory stream never spans the per-entry stats: on a
    // directory that needs more than one getdents fill, that is the difference
    // between a cursor that is live for the whole loop and one whose fills are
    // back to back. ensureListed() keys the result to the stamp it read *before*
    // calling us, so a change arriving while we scan shows up as a mismatch on the
    // next access and re-lists, instead of this listing claiming to describe a
    // state it never observed. Concurrent modification is otherwise unguarded
    // (see *Future Ideas*).
    ItemFactory& factory = itemcache_.itemFactory();

    std::vector<CNIDMapper::DirEntry> entries;
    try
    {
        for(const auto& e : fs::directory_iterator(path()))
        {
            if(factory.isHidden(e))
                continue;

            // Identity and kind are filled in by the second pass.
            entries.push_back({ e.path().filename(), {}, false });
        }
    }
    catch(const boost::filesystem::filesystem_error& exc)
    {
        // A failed scan says nothing about the directory's contents, so it must
        // not be committed: the diff would read the (empty) result as every child
        // having gone and renumber the whole directory. Report it instead -- the
        // caller drops its listing and the next access retries from scratch.
        std::cerr << exc.what() << std::endl;
        return std::nullopt;
    }

    for(auto& entry : entries)
    {
        PathStat st = statPath(path() / entry.posixName);
        if(!st.exists || (!st.isDirectory && !st.isRegular))
        {
            entry.posixName.clear();   // not addressable; dropped below
            continue;
        }

        entry.identity = st.identity;
        entry.isDir = st.isDirectory;
    }

    // readdir never yields an empty name, so an empty one marks a dropped entry.
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [](const CNIDMapper::DirEntry& e) { return e.posixName.empty(); }),
        entries.end());

    localVolumeStats().directoryIterations++;
    localVolumeStats().entriesSeen += entries.size();

    std::vector<CNIDMapper::Mapping> mappings =
        itemcache_.cnidMapper().mapDirectoryContents(cnid(), std::move(entries));

    std::vector<Descriptor> descriptors;
    descriptors.reserve(mappings.size());

    for(auto& m : mappings)
    {
        // The diff may have recovered a rename (or dropped the entry), so re-point
        // any live Item at its current path before anything can use the stale one:
        // a live Item is otherwise only relocated when it is handed out, which
        // leaves the invariant unenforced between a listing and the next hand-out.
        // relocate() also clears a cached Mac name when the name changed, matching
        // the record's reset.
        if(ItemPtr item = itemcache_.findRealizedItem(m.cnid))
            item->relocate(path() / m.posixName);

        Descriptor d;
        d.posixName = m.posixName;
        d.isDir = m.isDir;
        d.cnid = m.cnid;
        d.prefix = hostGroupPrefix(d.posixName);
        descriptors.push_back(std::move(d));
    }

    std::sort(descriptors.begin(), descriptors.end(),
        [](const Descriptor& a, const Descriptor& b) {
            if(a.prefix != b.prefix)
                return a.prefix < b.prefix;
            return a.posixName.native() < b.posixName.native();
        });

    return descriptors;
}

bool DirectoryItem::ensureListed()
{
    FileStamp stamp = statFile(path());
    if(listed_ && stamp_ == stamp)
        return true;   // L0: the stamp is unchanged, so everything is still valid

    auto descriptors = listDirectory();
    if(!descriptors)
    {
        // A failed scan says nothing about the directory's contents, so it must
        // not be committed. Drop the listing instead -- the caller turns the
        // false into ioErr, and the next access retries from scratch.
        clearCache();
        return false;
    }

    storeDescriptors(std::move(*descriptors), stamp);
    itemcache_.cacheItem(self());
    return true;
}

void DirectoryItem::resolveGroup(mac_string_view prefix)
{
    if(!ensureListed())
        return;   // unreadable: leave the names undecided

    auto [begin, end] = groupRange(prefix);
    if(begin == end)
        return;

    CNIDMapper& mapper = itemcache_.cnidMapper();

    std::vector<CNIDMapper::NameRequest> group;
    group.reserve(end - begin);
    for(size_t i = begin; i < end; i++)
    {
        const Descriptor& d = descriptors_[i];
        // The whole group is offered, so already-decided names seed the collision
        // set; only the undecided entries need a name source. The binding is
        // passed too, so a rename that raced the listing is rejected.
        if(auto rec = mapper.lookupCNID(d.cnid); rec && rec->macname)
            group.push_back({ d.cnid, d.posixName, {} });
        else
            group.push_back({ d.cnid, d.posixName, nameSourceOf(d) });
    }

    mapper.nameGroup(cnid(), group);
}

fs::path DirectoryItem::nameSourceOf(const Descriptor& d)
{
    // Only a ".bin" file's rep can change its name (MacBinary drops the
    // extension), so that is the one place naming needs the format: materialize
    // the entry itself rather than probing a throwaway (plan: *Names / The final
    // name*). The rep it builds is kept and reused if the entry is accessed.
    std::string ext = d.posixName.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if(d.isDir || ext != ".bin")
        return d.posixName;

    localVolumeStats().nameProbes++;

    auto file = std::static_pointer_cast<FileItem>(itemcache_.realizeDescriptor(self(), d));
    return file->nameSource(d.posixName);
}

std::optional<mac_string> DirectoryItem::resolveName(const Item& item)
{
    if(!ensureListed())
        return {};

    resolveGroup(hostGroupPrefix(item.path().filename()));

    if(auto rec = itemcache_.cnidMapper().lookupCNID(item.cnid()); rec && rec->macname)
        return *rec->macname;
    return {};
}

ItemPtr DirectoryItem::child(mac_string_view name)
{
    CNIDMapper& mapper = itemcache_.cnidMapper();

    // Fast path: the name is already decided for this directory.
    if(auto foundCnid = mapper.cnidForName(cnid(), name))
        if(ItemPtr item = itemcache_.realizeByCNID(*foundCnid))
        {
            // Realizing may have recovered a rename, invalidating this name.
            if(mapper.cnidForName(cnid(), name) == foundCnid)
                return item;
        }

    if(!ensureListed())
        throw OSErrorException(ioErr);

    // Decide the group the name falls in, then look it up.
    resolveGroup(macNamePrefix(name));

    if(auto foundCnid = mapper.cnidForName(cnid(), name))
        if(ItemPtr item = itemcache_.realizeByCNID(*foundCnid))
            return item;

    return {};
}

ItemPtr DirectoryItem::child(int index, bool includeDirectories)
{
    if(!ensureListed())
        throw OSErrorException(ioErr);
    const Descriptor* d = childByIndex(index, includeDirectories);
    if(!d)
        throw OSErrorException(fnfErr);
    // The name is decided lazily, when the caller reads item->name().
    return itemcache_.realizeDescriptor(self(), *d);
}

void DirectoryItem::clearCache()
{
    listed_ = false;
    stamp_ = {};
    count_.reset();
    countStamp_ = {};
    descriptors_.clear();

    // The payload just shrank, so re-cache to bring the LRU's accounting in step.
    // The object itself is kept: identity lives in the mapper, so a re-list
    // rebuilds the payload in place.
    itemcache_.cacheItem(self());
}

void DirectoryItem::storeDescriptors(std::vector<Descriptor> descriptors, FileStamp stamp)
{
    descriptors_ = std::move(descriptors);
    listed_ = true;
    stamp_ = stamp;
}

std::pair<size_t, size_t> DirectoryItem::groupRange(mac_string_view prefix) const
{
    assert(listed_);
    mac_string key { prefix };

    auto first = std::lower_bound(descriptors_.begin(), descriptors_.end(), key,
        [](const Descriptor& d, const mac_string& k) { return d.prefix < k; });
    auto last = std::upper_bound(first, descriptors_.end(), key,
        [](const mac_string& k, const Descriptor& d) { return k < d.prefix; });

    return { (size_t)(first - descriptors_.begin()), (size_t)(last - descriptors_.begin()) };
}

const Descriptor* DirectoryItem::childByIndex(int index, bool includeDirectories) const
{
    assert(listed_);
    if(index < 1)
        return nullptr;
    for(const Descriptor& d : descriptors_)
    {
        if(!includeDirectories && d.isDir)
            continue;
        if(--index == 0)
            return &d;
    }
    return nullptr;
}

void DirectoryItem::deleteItem()
{
    boost::system::error_code ec;
    fs::remove(path() / ".rsrc", ec);
    fs::remove(path() / ".finf", ec);       // TODO: individual ItemFactories should provide this info

    fs::remove(path(), ec);

    if(ec)
    {
        if(ec == boost::system::errc::directory_not_empty)
            throw OSErrorException(fBsyErr);
        else
            throw OSErrorException(paramErr);
    }
}
