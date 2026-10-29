#include "simplecnidmapper.h"

using namespace Executor;

SimpleCNIDMapper::SimpleCNIDMapper(mac_string volumeName)
{
    // The volume root (CNID 2) carries no name of its own.
    mappings_.emplace(2, Mapping{1, 2, fs::path(), std::move(volumeName), {}, true});
}

std::optional<CNIDMapper::Mapping> SimpleCNIDMapper::get(CNID cnid)
{
    auto it = mappings_.find(cnid);
    if(it == mappings_.end())
        return {};

    return it->second;
}

std::optional<CNIDMapper::Mapping> SimpleCNIDMapper::getChild(CNID parID,
                                                                     const fs::path& posixName)
{
    auto dirIt = posixNames_.find(parID);
    if(dirIt == posixNames_.end())
        return {};

    auto it = dirIt->second.find(posixName);
    if(it == dirIt->second.end())
        return {};

    auto mit = mappings_.find(it->second);
    if(mit == mappings_.end())
        return {};

    return mit->second;
}

std::vector<std::pair<fs::path, CNID>> SimpleCNIDMapper::getChildren(CNID parID)
{
    std::vector<std::pair<fs::path, CNID>> result;
    if(auto it = posixNames_.find(parID); it != posixNames_.end())
        result.assign(it->second.begin(), it->second.end());
    return result;
}

std::optional<CNID> SimpleCNIDMapper::getName(CNID parID, mac_string_view foldedMacName)
{
    auto it = names_.find(parID);
    if(it == names_.end())
        return {};

    auto nit = it->second.find(mac_string(foldedMacName));
    if(nit == it->second.end())
        return {};
    return nit->second;
}

void SimpleCNIDMapper::put(const Mapping& record)
{
    mappings_[record.cnid] = record;
}

void SimpleCNIDMapper::erase(CNID cnid)
{
    mappings_.erase(cnid);
}

void SimpleCNIDMapper::putChild(CNID parID, const fs::path& posixName, CNID cnid)
{
    posixNames_[parID][posixName] = cnid;
}

void SimpleCNIDMapper::eraseChild(CNID parID, const fs::path& posixName)
{
    auto it = posixNames_.find(parID);
    if(it == posixNames_.end())
        return;

    it->second.erase(posixName);
    if(it->second.empty())
        posixNames_.erase(it);
}

void SimpleCNIDMapper::putName(CNID parID, mac_string_view foldedMacName, CNID cnid)
{
    names_[parID][mac_string(foldedMacName)] = cnid;
}

void SimpleCNIDMapper::eraseName(CNID parID, mac_string_view foldedMacName)
{
    auto it = names_.find(parID);
    if(it == names_.end())
        return;

    it->second.erase(mac_string(foldedMacName));
    if(it->second.empty())
        names_.erase(it);
}

CNID SimpleCNIDMapper::allocate()
{
    return nextCNID_++;
}
