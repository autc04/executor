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
}

SimpleCNIDMapper::SimpleCNIDMapper(fs::path root, mac_string volumeName)
{
    mappings_.emplace(2, StoredMapping { 1, 2, std::move(root), std::move(volumeName), {}, true, 0 });
}

std::set<mac_string> SimpleCNIDMapper::namesInUse(CNID dirID)
{
    std::set<mac_string> used;

    auto it = children_.find(dirID);
    if(it == children_.end())
        return used;

    for(CNID cnid : it->second)
    {
        auto mit = mappings_.find(cnid);
        if(mit != mappings_.end())
        {
            mac_string nameUpr = mit->second.macname;
            ROMlib_UprString(nameUpr.data(), false, nameUpr.size());
            used.insert(std::move(nameUpr));
        }
    }

    return used;
}

mac_string SimpleCNIDMapper::chooseMacName(const fs::path& filename, std::set<mac_string>& usedNames)
{
    mac_string macname;
    int index = 0;

    bool nameIsFree;
    do
    {
        macname = toMacRomanFilename(filename, index++);
        mac_string nameUpr = macname;
        ROMlib_UprString(nameUpr.data(), false, nameUpr.size());
        nameIsFree = usedNames.emplace(std::move(nameUpr)).second;
    } while(!nameIsFree);

    return macname;
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

CNID SimpleCNIDMapper::createRecord(CNID parID, const fs::path& path, const fs::path& nameSource,
    const FileIdentity& identity, bool isDir, std::uint64_t mtime)
{
    auto usedNames = namesInUse(parID);
    mac_string macname = chooseMacName(nameSource, usedNames);
    CNID cnid = nextCNID_++;

    mappings_.emplace(cnid, StoredMapping { parID, cnid, path, std::move(macname), identity, isDir, mtime });
    children_[parID].insert(cnid);
    if(identity.valid)
        inodeIndex_[inodeKey(identity)] = cnid;

    return cnid;
}

CNID SimpleCNIDMapper::resolveChild(CNID parID, const fs::path& path, const fs::path& nameSource,
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
            if(mit != mappings_.end() && mit->second.parID == parID && mit->second.path == path)
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
            if(sameIdentity(mit->second.identity, identity))
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
    return createRecord(parID, path, nameSource, identity, isDir, mtime);
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
                if(mit != mappings_.end() && mit->second.parID == dirID && mit->second.path == e.path)
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

    // Pass 2: stored children the scan did not match are gone, freeing their
    // names before Pass 3 picks names for the new entries.
    for(CNID c : stored)
        if(surviving.find(c) == surviving.end())
            deleteCNID(c);

    // Pass 3: refresh the survivors and create the new entries. Names of
    // survivors are sticky; a delete-and-recreate at a name is then free to
    // take that name back.
    std::set<mac_string> usedNames = namesInUse(dirID);

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
            mac_string macname = chooseMacName(e.nameSource, usedNames);
            mappings_.emplace(cnid, StoredMapping { dirID, cnid, e.path, std::move(macname), e.identity, e.isDir, e.mtime });
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

void SimpleCNIDMapper::setIdentity(CNID cnid, const FileIdentity& identity)
{
    auto it = mappings_.find(cnid);
    if(it == mappings_.end())
        return;

    if(it->second.identity.valid)
    {
        auto iit = inodeIndex_.find(inodeKey(it->second.identity));
        if(iit != inodeIndex_.end() && iit->second == cnid)
            inodeIndex_.erase(iit);
    }

    it->second.identity = identity;
    if(identity.valid)
        inodeIndex_[inodeKey(identity)] = cnid;
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

    if(auto pit = children_.find(oldParent); pit != children_.end())
        pit->second.erase(cnid);

    it->second.path = newPath;
    it->second.parID = newParent;
    if(!newMacName.empty())
        it->second.macname = newMacName;

    children_[newParent].insert(cnid);
}
