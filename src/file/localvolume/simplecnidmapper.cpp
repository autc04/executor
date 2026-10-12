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
{
    mappings_.emplace(2, StoredMapping { 1, 2, std::move(root), std::move(volumeName), {}, true, 0 });
}

std::optional<CNID> SimpleCNIDMapper::childByPath(CNID parID, const fs::path& path) const
{
    auto it = children_.find(parID);
    if(it == children_.end())
        return {};

    for(CNID cnid : it->second)
    {
        auto mit = mappings_.find(cnid);
        if(mit != mappings_.end() && mit->second.path == path)
            return cnid;
    }
    return {};
}

CNID SimpleCNIDMapper::createRecord(CNID parID, const fs::path& path,
    const FileIdentity& identity, bool isDir, std::uint64_t mtime)
{
    CNID cnid = nextCNID_++;

    mappings_.emplace(cnid, StoredMapping { parID, cnid, path, std::nullopt, identity, isDir, mtime });
    children_[parID].insert(cnid);
    if(identity.valid)
        inodeIndex_[inodeKey(identity)] = cnid;

    return cnid;
}

CNID SimpleCNIDMapper::resolveChild(CNID parID, const fs::path& path,
    const FileIdentity& identity, bool isDir, std::uint64_t mtime)
{
    // 1. Prefer identity: the record this inode already has.
    if(identity.valid)
    {
        auto iit = inodeIndex_.find(inodeKey(identity));
        if(iit != inodeIndex_.end())
        {
            CNID cnid = iit->second;
            auto mit = mappings_.find(cnid);
            // A kind change at a name is not the same item, even if the host
            // reused the inode.
            if(mit != mappings_.end() && mit->second.parID == parID && mit->second.path == path
               && mit->second.isDir == isDir)
            {
                mit->second.mtime = mtime;
                return cnid;
            }

            // The inode now lives here under a new name, or in another
            // directory: the old binding is stale (delete + create).
            if(mit != mappings_.end())
                deleteCNID(cnid);
            else
                inodeIndex_.erase(iit);
        }
    }

    // 2. An existing binding at the same path. A different *known* identity
    //    means the file was replaced (delete + create).
    if(auto existing = childByPath(parID, path))
    {
        auto mit = mappings_.find(*existing);
        if(mit != mappings_.end())
        {
            if(mit->second.isDir == isDir && sameIdentity(mit->second.identity, identity))
            {
                mit->second.identity = identity;
                mit->second.mtime = mtime;
                if(identity.valid)
                    inodeIndex_[inodeKey(identity)] = *existing;
                return *existing;
            }
            deleteCNID(*existing);
        }
    }

    // 3. A new binding.
    return createRecord(parID, path, identity, isDir, mtime);
}

std::vector<CNIDMapper::Mapping> SimpleCNIDMapper::mapDirectoryContents(CNID dirID,
        std::vector<DirEntry> entries)
{
    std::vector<CNID> stored;
    if(auto it = children_.find(dirID); it != children_.end())
        stored.assign(it->second.begin(), it->second.end());

    const size_t n = entries.size();
    std::vector<CNID> reused(n, 0);   // 0 = the entry is new
    std::set<CNID> surviving;

    // Fallback for entries with no usable inode: match by host path. Built once,
    // lazily, from the stored children.
    std::map<std::string, CNID> byPath;
    bool byPathBuilt = false;

    // Pass 1: match each scanned entry to a stored record, by identity (or, when
    // the host gives no usable inode, by path).
    for(size_t i = 0; i < n; i++)
    {
        const DirEntry& e = entries[i];
        CNID match = 0;

        if(e.identity.valid)
        {
            auto iit = inodeIndex_.find(inodeKey(e.identity));
            if(iit != inodeIndex_.end())
            {
                CNID candidate = iit->second;
                auto mit = mappings_.find(candidate);
                // A kind change at a name is not the same item, even if the host
                // reused the inode.
                if(mit != mappings_.end() && mit->second.parID == dirID && mit->second.path == e.path
                   && mit->second.isDir == e.isDir)
                    match = candidate;
            }
        }
        else
        {
            if(!byPathBuilt)
            {
                for(CNID c : stored)
                    if(auto mit = mappings_.find(c); mit != mappings_.end())
                        byPath.emplace(mit->second.path.string(), c);
                byPathBuilt = true;
            }

            auto bit = byPath.find(e.path.string());
            if(bit != byPath.end())
                match = bit->second;
        }

        if(match && surviving.find(match) == surviving.end())
        {
            reused[i] = match;
            surviving.insert(match);
        }
    }

    // Pass 2: stored children the scan did not match are gone. This also frees
    // their names for the entries that replace them.
    for(CNID c : stored)
        if(surviving.find(c) == surviving.end())
            deleteCNID(c);

    // Pass 3: refresh the survivors and create the new entries. No names are
    // decided here; the survivors keep theirs, the new entries stay unnamed.
    std::vector<Mapping> result;
    result.reserve(n);

    for(size_t i = 0; i < n; i++)
    {
        const DirEntry& e = entries[i];
        CNID cnid = reused[i];

        if(cnid)
        {
            auto& rec = mappings_.at(cnid);
            rec.identity = e.identity;
            rec.mtime = e.mtime;
            rec.isDir = e.isDir;
        }
        else
        {
            cnid = nextCNID_++;
            mappings_.emplace(cnid, StoredMapping { dirID, cnid, e.path, std::nullopt, e.identity, e.isDir, e.mtime });
            surviving.insert(cnid);
        }

        if(e.identity.valid)
            inodeIndex_[inodeKey(e.identity)] = cnid;

        const auto& rec = mappings_.at(cnid);
        result.push_back({ dirID, cnid, e.path, rec.macname, e.identity, e.isDir });
    }

    children_[dirID] = std::move(surviving);

    return result;
}

std::optional<CNIDMapper::Mapping> SimpleCNIDMapper::lookupCNID(CNID cnid)
{
    auto it = mappings_.find(cnid);
    if(it == mappings_.end())
        return {};

    const StoredMapping& m = it->second;
    return Mapping { m.parID, m.cnid, m.path, m.macname, m.identity, m.isDir };
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
    FileIdentity identity = it->second.identity;

    if(identity.valid)
    {
        auto iit = inodeIndex_.find(inodeKey(identity));
        if(iit != inodeIndex_.end() && iit->second == cnid)
            inodeIndex_.erase(iit);
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

    it->second.path = newPath;
    it->second.parID = newParent;

    children_[newParent].insert(cnid);

    if(it->second.macname)
        names_[newParent][foldName(*it->second.macname)] = cnid;
}
