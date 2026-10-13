#include "simplecnidmapper.h"
#include <OSUtil.h>

#include <algorithm>
#include <map>
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

CNID SimpleCNIDMapper::createRecord(CNID parID, const fs::path& posixName,
    const FileIdentity& identity, bool isDir, std::uint64_t mtime)
{
    CNID cnid = nextCNID_++;

    mappings_.emplace(cnid, StoredMapping { parID, cnid, posixName, std::nullopt, identity, isDir, mtime });
    posixNames_[parID][posixName] = cnid;

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

    std::vector<std::pair<fs::path, CNID>> stored;
    if(auto it = posixNames_.find(dirID); it != posixNames_.end())
        stored.assign(it->second.begin(), it->second.end());

    const size_t n = entries.size();
    std::vector<bool> consumed(n, false);
    std::set<CNID> surviving;

    // Pass 1: a child whose name, kind and identity still match a scanned entry is
    // unchanged. A child's name is unique, so each entry matches at most one child.
    if(auto dirIt = posixNames_.find(dirID); dirIt != posixNames_.end())
    {
        for(size_t i = 0; i < n; i++)
        {
            const DirEntry& e = entries[i];

            auto eit = dirIt->second.find(e.posixName);
            if(eit == dirIt->second.end())
                continue;

            auto mit = mappings_.find(eit->second);
            if(mit == mappings_.end())
                continue;

            StoredMapping& rec = mit->second;
            if(rec.isDir != e.isDir || !sameIdentity(rec.identity, e.identity))
                continue;

            consumed[i] = true;
            rec.identity = e.identity;   // adopt an inode that is usable now
            rec.mtime = e.mtime;
            surviving.insert(rec.cnid);
        }
    }

    // The inode pairing for rename recovery is scratch, built from the scan.
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<size_t>> byInode;
    for(size_t i = 0; i < n; i++)
        if(entries[i].identity.valid && !consumed[i])
            byInode[inodeKey(entries[i].identity)].push_back(i);

    // Pass 2: a child that was not kept was renamed, split, or is gone. Process in
    // CNID order so the result does not depend on scan order.
    std::vector<CNID> remaining;
    for(auto& [name, cnid] : stored)
        if(!surviving.count(cnid))
            remaining.push_back(cnid);
    std::sort(remaining.begin(), remaining.end());

    for(CNID cnid : remaining)
    {
        auto& rec = mappings_.at(cnid);

        std::vector<size_t> rem;
        if(rec.identity.valid)
            if(auto bit = byInode.find(inodeKey(rec.identity)); bit != byInode.end())
                for(size_t i : bit->second)
                    if(!consumed[i])
                        rem.push_back(i);

        if(rem.size() == 1 && entries[rem[0]].isDir == rec.isDir)
        {
            // Exactly one occurrence at a new name: the entry was renamed.
            size_t i = rem[0];
            consumed[i] = true;

            auto& dirMap = posixNames_[dirID];
            dirMap.erase(rec.posixName);
            forgetRecordName(rec);   // the Mac name is re-derived
            rec.macname.reset();
            rec.posixName = entries[i].posixName;
            rec.identity = entries[i].identity;
            rec.mtime = entries[i].mtime;
            dirMap[rec.posixName] = cnid;
            surviving.insert(cnid);
        }
        else
        {
            // No occurrence: gone. Several: the old CNID is dropped and each
            // occurrence becomes a new entry.
            deleteCNID(cnid);
        }
    }

    std::vector<Mapping> result;
    result.reserve(n);

    for(auto& [name, cnid] : stored)
        if(surviving.count(cnid))
            result.push_back(mappingFor(cnid, dirPath));

    for(size_t i = 0; i < n; i++)
        if(!consumed[i])
            result.push_back(mappingFor(
                createRecord(dirID, entries[i].posixName, entries[i].identity,
                    entries[i].isDir, entries[i].mtime),
                dirPath));

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

    if(auto dirIt = posixNames_.find(parID); dirIt != posixNames_.end())
    {
        if(auto it = dirIt->second.find(posixName); it != dirIt->second.end())
        {
            auto& rec = mappings_.at(it->second);
            if(rec.isDir == isDir && sameIdentity(rec.identity, identity))
            {
                rec.identity = identity;
                rec.mtime = mtime;
                return it->second;
            }

            // A different known identity or kind at this name: delete + create.
            deleteCNID(it->second);
        }
    }

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
    fs::path posixName = it->second.posixName;

    forgetRecordName(it->second);

    if(auto pit = posixNames_.find(parID); pit != posixNames_.end())
    {
        pit->second.erase(posixName);
        if(pit->second.empty())
            posixNames_.erase(pit);
    }

    // deleteCNID is only invoked for empty directories by ItemCache, but the diff
    // above also invokes it on files and directories that have been deleted or
    // moved by other programs.
    if(auto cit = posixNames_.find(cnid); cit != posixNames_.end())
    {
        std::vector<CNID> childList;
        childList.reserve(cit->second.size());
        for(auto& [name, child] : cit->second)
            childList.push_back(child);
        posixNames_.erase(cit);
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

    // Both name maps are keyed by parent, so a move re-homes the entries.
    forgetRecordName(it->second);

    if(!newMacName.empty())
        it->second.macname = newMacName;

    fs::path oldName = it->second.posixName;
    if(auto pit = posixNames_.find(oldParent); pit != posixNames_.end())
    {
        pit->second.erase(oldName);
        if(pit->second.empty())
            posixNames_.erase(pit);
    }

    it->second.posixName = newPath.filename();
    it->second.parID = newParent;

    posixNames_[newParent][it->second.posixName] = cnid;
    if(it->second.macname)
        names_[newParent][foldName(*it->second.macname)] = cnid;
}
