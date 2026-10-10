#include "simplecnidmapper.h"
#include <OSUtil.h>

#include <algorithm>
#include <set>
#include <vector>

using namespace Executor;

SimpleCNIDMapper::SimpleCNIDMapper(fs::path root, mac_string volumeName)
{
    mappings_.emplace(2, StoredMapping { 1, 2, root, std::move(volumeName) });
}

std::set<mac_string> SimpleCNIDMapper::namesInUse(CNID dirID)
{
    std::set<mac_string> used;

    auto it = children_.find(dirID);
    if(it == children_.end())
        return used;

    for(auto& [filename, cnid] : it->second)
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

std::optional<CNIDMapper::Mapping> SimpleCNIDMapper::lookupChild(CNID parID, const fs::path& filename)
{
    auto dirIt = children_.find(parID);
    if(dirIt == children_.end())
        return {};

    auto childIt = dirIt->second.find(filename.string());
    if(childIt == dirIt->second.end())
        return {};

    auto mit = mappings_.find(childIt->second);
    if(mit == mappings_.end())
        return {};

    return Mapping { mit->second.parID, mit->second.cnid,
        fs::directory_entry(mit->second.path), mit->second.macname };
}

CNIDMapper::Mapping SimpleCNIDMapper::getOrCreateChild(CNID parID, const fs::path& path,
    const fs::path& filename, const fs::path& nameSource)
{
    if(auto existing = lookupChild(parID, filename))
        return *existing;

    auto usedNames = namesInUse(parID);
    mac_string macname = chooseMacName(nameSource, usedNames);
    CNID cnid = nextCNID_++;

    mappings_.emplace(cnid, StoredMapping { parID, cnid, path, macname });
    children_[parID].emplace(filename.string(), cnid);

    return Mapping { parID, cnid, fs::directory_entry(path), macname };
}

std::vector<CNIDMapper::Mapping> SimpleCNIDMapper::mapDirectoryContents(CNID dirID,
        std::vector<DirEntry> realPaths)
{
    auto& children = children_[dirID];

    std::sort(realPaths.begin(), realPaths.end(),
        [](const DirEntry& a, const DirEntry& b) { return a.entry < b.entry; });

    // Names already assigned in this directory, including lazily-created
    // identities, so a new entry never takes an existing entry's name.
    std::set<mac_string> usedNames = namesInUse(dirID);

    std::vector<Mapping> dirMappings;
    dirMappings.reserve(realPaths.size());

    ChildIndex newChildren;
    newChildren.reserve(realPaths.size());

    for(auto& de : realPaths)
    {
        const std::string filename = de.entry.path().filename().string();

        CNID cnid;
        auto cit = children.find(filename);
        if(cit != children.end())
        {
            // use existing mapping
            cnid = cit->second;
        }
        else
        {
            // new mapping
            cnid = nextCNID_++;
            mac_string macname = chooseMacName(de.nameSource, usedNames);
            mappings_.emplace(cnid, StoredMapping { dirID, cnid, de.entry.path(), macname });
        }

        auto mit = mappings_.find(cnid);
        assert(mit != mappings_.end());
        dirMappings.push_back({ dirID, cnid, std::move(de.entry), mit->second.macname });
        newChildren.emplace(filename, cnid);
    }

    // Stored children not seen in the scan are gone; drop their mappings
    // (recursively, for directories). Collect first: deleteCNID mutates
    // children_[dirID].
    std::vector<CNID> deleted;
    for(auto& [filename, cnid] : children)
        if(newChildren.find(filename) == newChildren.end())
            deleted.push_back(cnid);
    for(CNID cnid : deleted)
        deleteCNID(cnid);

    children = std::move(newChildren);

    return dirMappings;
}

std::optional<CNIDMapper::Mapping> SimpleCNIDMapper::lookupCNID(CNID cnid)
{
    auto it = mappings_.find(cnid);
    if(it == mappings_.end())
        return {};

    if(fs::exists(it->second.path))
    {
        try
        {
            return Mapping {
                it->second.parID,
                it->second.cnid,
                fs::directory_entry(it->second.path),
                it->second.macname
            };
        }
        catch(const fs::filesystem_error&)
        {
            // file no longer exists,
            // pass through
        }
    }

    deleteCNID(cnid);
    return {};
}

void SimpleCNIDMapper::deleteCNID(CNID cnid)
{
    auto it = mappings_.find(cnid);
    if(it == mappings_.end())
        return;

    CNID parID = it->second.parID;
    std::string filename = it->second.path.filename().string();

    auto parentIt = children_.find(parID);
    if(parentIt != children_.end())
        parentIt->second.erase(filename);

    mappings_.erase(it);

    auto dirIt = children_.find(cnid);
    if(dirIt != children_.end())
    {
        // deleteCNID is only invoked for empty directories by ItemCache,
        // but mapDirectoryContents and lookupCNID above also invoke it
        // on files and directories that have been deleted or moved by other
        // programs.
        std::vector<CNID> childList;
        childList.reserve(dirIt->second.size());
        for(auto& [childFilename, childCnid] : dirIt->second)
            childList.push_back(childCnid);
        for(CNID childCnid : childList)
            deleteCNID(childCnid);
        children_.erase(cnid);
    }
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

    std::string oldFilename = it->second.path.filename().string();

    fs::path newPath = fsop();

    it->second.path = newPath;
    it->second.parID = newParent;
    if(!newMacName.empty())
        it->second.macname = newMacName;

    auto parentIt = children_.find(oldParent);
    if(parentIt != children_.end())
        parentIt->second.erase(oldFilename);

    children_[newParent].emplace(newPath.filename().string(), cnid);
}
