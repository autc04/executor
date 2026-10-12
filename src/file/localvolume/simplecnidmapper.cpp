#include "simplecnidmapper.h"
#include <OSUtil.h>

#include <algorithm>
#include <set>
#include <vector>

using namespace Executor;

namespace
{
std::pair<std::uint64_t, std::uint64_t> inodeKey(const FileIdentity& id)
{
    return { id.device, id.inode };
}

mac_string foldName(mac_string_view name)
{
    mac_string folded { name };
    ROMlib_UprString(folded.data(), false, folded.size());
    return folded;
}
}

SimpleCNIDMapper::SimpleCNIDMapper(fs::path root, mac_string volumeName)
    : rootPath_(std::move(root))
{
    // The volume root (CNID 2) carries no name of its own; pathOf() special-cases
    // it to rootPath_.
    mappings_.emplace(2, StoredMapping { 1, 2, fs::path(), std::move(volumeName), {}, true, 0 });
}

fs::path SimpleCNIDMapper::pathOf(CNID cnid) const
{
    std::vector<fs::path> names;

    for(CNID c = cnid; c != 2;)
    {
        auto it = mappings_.find(c);
        if(it == mappings_.end())
            return {};
        names.push_back(it->second.posixName);
        CNID parent = it->second.parID;
        if(parent == c)   // broken chain; bail out rather than loop
            return {};
        c = parent;
    }

    fs::path path = rootPath_;
    for(auto it = names.rbegin(); it != names.rend(); ++it)
        path /= *it;
    return path;
}

std::optional<CNID> SimpleCNIDMapper::childByName(CNID parID, const fs::path& posixName) const
{
    auto it = children_.find(parID);
    if(it == children_.end())
        return {};

    for(CNID cnid : it->second)
    {
        auto mit = mappings_.find(cnid);
        if(mit != mappings_.end() && mit->second.posixName == posixName)
            return cnid;
    }
    return {};
}

void SimpleCNIDMapper::addBinding(const StoredMapping& rec)
{
    if(rec.identity.valid)
        bindings_[inodeKey(rec.identity)].insert(rec.cnid);
}

void SimpleCNIDMapper::setRecordIdentity(StoredMapping& rec, const FileIdentity& identity)
{
    if(rec.identity.valid)
    {
        auto it = bindings_.find(inodeKey(rec.identity));
        if(it != bindings_.end())
        {
            it->second.erase(rec.cnid);
            if(it->second.empty())
                bindings_.erase(it);
        }
    }

    rec.identity = identity;
    addBinding(rec);
}

CNID SimpleCNIDMapper::createRecord(CNID parID, const fs::path& posixName,
    const FileIdentity& identity, bool isDir, std::uint64_t mtime)
{
    CNID cnid = nextCNID_++;

    mappings_.emplace(cnid, StoredMapping { parID, cnid, posixName, std::nullopt, identity, isDir, mtime });
    children_[parID].insert(cnid);
    addBinding(mappings_.at(cnid));

    return cnid;
}

CNIDMapper::Mapping SimpleCNIDMapper::mappingFor(CNID cnid, const fs::path& dirPath) const
{
    const StoredMapping& m = mappings_.at(cnid);
    return Mapping { m.parID, m.cnid, dirPath / m.posixName, m.macname, m.identity, m.isDir };
}

std::vector<CNIDMapper::Mapping> SimpleCNIDMapper::mapDirectoryContents(CNID dirID,
        std::vector<DirEntry> entries)
{
    const fs::path dirPath = pathOf(dirID);

    std::vector<CNID> stored;
    if(auto it = children_.find(dirID); it != children_.end())
        stored.assign(it->second.begin(), it->second.end());
    std::sort(stored.begin(), stored.end());   // deterministic order

    const size_t n = entries.size();

    // occ: stored child -> the scanned entries that share its identity. Built
    // from the scan (not a name -> cnid map of the resolver's kind).
    std::unordered_map<CNID, std::vector<size_t>> occ;

    // Fallback for entries with no usable inode: match by host name.
    std::unordered_map<std::string, CNID> byName;
    bool byNameBuilt = false;

    for(size_t i = 0; i < n; i++)
    {
        const DirEntry& e = entries[i];

        if(e.identity.valid)
        {
            auto bit = bindings_.find(inodeKey(e.identity));
            if(bit != bindings_.end())
                for(CNID c : bit->second)
                    if(auto mit = mappings_.find(c); mit != mappings_.end() && mit->second.parID == dirID)
                        occ[c].push_back(i);
        }
        else
        {
            if(!byNameBuilt)
            {
                for(CNID c : stored)
                    if(auto mit = mappings_.find(c); mit != mappings_.end())
                        byName.emplace(mit->second.posixName.string(), c);
                byNameBuilt = true;
            }

            auto bit = byName.find(e.posixName.string());
            if(bit != byName.end())
                occ[bit->second].push_back(i);
        }
    }

    std::vector<bool> consumed(n, false);
    std::set<CNID> surviving;

    // Keep: a child whose name and kind are unchanged.
    for(CNID c : stored)
    {
        auto oit = occ.find(c);
        if(oit == occ.end())
            continue;

        const StoredMapping& rec = mappings_.at(c);
        for(size_t i : oit->second)
        {
            if(consumed[i])
                continue;
            const DirEntry& e = entries[i];
            if(e.posixName != rec.posixName || e.isDir != rec.isDir)
                continue;

            consumed[i] = true;
            auto& m = mappings_.at(c);
            setRecordIdentity(m, e.identity);
            m.mtime = e.mtime;
            surviving.insert(c);
            break;
        }
    }

    // Rename or delete the rest.
    for(CNID c : stored)
    {
        if(surviving.count(c))
            continue;

        auto& rec = mappings_.at(c);

        std::vector<size_t> rem;
        if(auto oit = occ.find(c); oit != occ.end())
            for(size_t i : oit->second)
                if(!consumed[i])
                    rem.push_back(i);

        if(rem.size() == 1 && entries[rem[0]].isDir == rec.isDir)
        {
            // Exactly one occurrence at a new name: the entry was renamed.
            size_t i = rem[0];
            consumed[i] = true;

            forgetRecordName(rec);   // the Mac name is re-derived
            rec.macname.reset();
            rec.posixName = entries[i].posixName;
            setRecordIdentity(rec, entries[i].identity);
            rec.mtime = entries[i].mtime;
            surviving.insert(c);
        }
        else
        {
            // No occurrence: gone. Several: the old CNID is dropped and each
            // occurrence becomes a new entry.
            deleteCNID(c);
        }
    }

    std::vector<Mapping> result;
    result.reserve(n);

    for(CNID c : stored)
        if(surviving.count(c))
            result.push_back(mappingFor(c, dirPath));

    for(size_t i = 0; i < n; i++)
        if(!consumed[i])
        {
            CNID c = createRecord(dirID, entries[i].posixName, entries[i].identity,
                entries[i].isDir, entries[i].mtime);
            consumed[i] = true;
            result.push_back(mappingFor(c, dirPath));
        }

    return result;
}

std::optional<CNIDMapper::Mapping> SimpleCNIDMapper::lookupCNID(CNID cnid)
{
    auto it = mappings_.find(cnid);
    if(it == mappings_.end())
        return {};

    const StoredMapping& m = it->second;
    return Mapping { m.parID, m.cnid, pathOf(cnid), m.macname, m.identity, m.isDir };
}

CNID SimpleCNIDMapper::resolveChild(CNID parID, const fs::path& path,
    const FileIdentity& identity, bool isDir, std::uint64_t mtime)
{
    const fs::path posixName = path.filename();

    // 1. An identity match (a hard link has several bindings).
    if(identity.valid)
    {
        auto bit = bindings_.find(inodeKey(identity));
        if(bit != bindings_.end())
            for(CNID cnid : bit->second)
            {
                auto mit = mappings_.find(cnid);
                if(mit != mappings_.end() && mit->second.parID == parID
                   && mit->second.posixName == posixName && mit->second.isDir == isDir)
                {
                    mit->second.mtime = mtime;
                    return cnid;
                }
            }
    }

    // 2. An existing binding at this exact name. A different known identity or
    //    kind means the entry was replaced (delete + create).
    if(auto existing = childByName(parID, posixName))
    {
        auto& rec = mappings_.at(*existing);
        if(rec.isDir == isDir && sameIdentity(rec.identity, identity))
        {
            setRecordIdentity(rec, identity);
            rec.mtime = mtime;
            return *existing;
        }
        deleteCNID(*existing);
    }

    // 3. A new binding.
    return createRecord(parID, posixName, identity, isDir, mtime);
}

std::optional<CNID> SimpleCNIDMapper::cnidForName(CNID parID, mac_string_view macName)
{
    auto it = names_.find(parID);
    if(it == names_.end())
        return {};

    auto nit = it->second.find(foldName(macName));
    if(nit == it->second.end())
        return {};
    return nit->second;
}

void SimpleCNIDMapper::nameGroup(CNID parID, const std::vector<NameRequest>& group)
{
    // Names can only collide within one prefix group (two names with different
    // folded prefixes cannot be equal), so the collision set is exactly the
    // group's already-decided members. Seeding only those keeps naming linear.
    std::set<mac_string> used;
    for(const NameRequest& req : group)
        if(auto mit = mappings_.find(req.cnid); mit != mappings_.end() && mit->second.macname)
            used.insert(foldName(*mit->second.macname));

    for(const NameRequest& req : group)
    {
        auto mit = mappings_.find(req.cnid);
        if(mit == mappings_.end() || mit->second.parID != parID)
            continue;
        if(mit->second.macname)
            continue;   // already decided; names are sticky

        int index = 0;
        for(;;)
        {
            mac_string candidate = toMacRomanFilename(req.nameSource, index++);
            mac_string folded = foldName(candidate);
            if(used.insert(folded).second)
            {
                mit->second.macname = std::move(candidate);
                names_[parID][std::move(folded)] = req.cnid;
                break;
            }
        }
    }
}

void SimpleCNIDMapper::forgetRecordName(StoredMapping& rec)
{
    if(!rec.macname)
        return;

    auto it = names_.find(rec.parID);
    if(it == names_.end())
        return;

    auto nit = it->second.find(foldName(*rec.macname));
    if(nit != it->second.end() && nit->second == rec.cnid)
        it->second.erase(nit);
}

void SimpleCNIDMapper::deleteCNID(CNID cnid)
{
    auto it = mappings_.find(cnid);
    if(it == mappings_.end())
        return;

    CNID parID = it->second.parID;

    if(it->second.identity.valid)
    {
        auto bit = bindings_.find(inodeKey(it->second.identity));
        if(bit != bindings_.end())
        {
            bit->second.erase(cnid);
            if(bit->second.empty())
                bindings_.erase(bit);
        }
    }

    forgetRecordName(it->second);

    if(auto pit = children_.find(parID); pit != children_.end())
        pit->second.erase(cnid);

    // deleteCNID is only invoked for empty directories by ItemCache, but the
    // diff above also invokes it on files and directories that have been
    // deleted or moved by other programs.
    if(auto cit = children_.find(cnid); cit != children_.end())
    {
        std::vector<CNID> childList(cit->second.begin(), cit->second.end());
        children_.erase(cit);
        for(CNID childCnid : childList)
            deleteCNID(childCnid);
    }

    mappings_.erase(it);

    if(auto nit = names_.find(parID); nit != names_.end() && nit->second.empty())
        names_.erase(nit);
}

void SimpleCNIDMapper::moveCNID(CNID cnid, CNID newParent, mac_string_view newMacName, std::function<fs::path()> fsop)
{
    auto it = mappings_.find(cnid);
    if(it == mappings_.end())
        return;

    CNID oldParent = it->second.parID;
    if(newParent == 0)
        newParent = oldParent;
    if(oldParent == newParent && newMacName.empty())
        return;

    fs::path newPath = fsop();

    // The name index is keyed by parent, so a move must re-home the entry.
    forgetRecordName(it->second);

    if(!newMacName.empty())
        it->second.macname = newMacName;

    if(auto pit = children_.find(oldParent); pit != children_.end())
        pit->second.erase(cnid);

    it->second.posixName = newPath.filename();
    it->second.parID = newParent;

    children_[newParent].insert(cnid);

    if(it->second.macname)
        names_[newParent][foldName(*it->second.macname)] = cnid;
}
