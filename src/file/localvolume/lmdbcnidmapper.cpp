#include "lmdbcnidmapper.h"
#include <file/file.h>
#include <OSUtil.h>
#include <rsys/paths.h>

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace Executor;

class LMDBCNIDMapper::BinMapping
{
    std::vector<char> data_;
    lmdb::val val_;
    CNID cnid;
    lmdb::val cnidval_;
public:
    BinMapping(const LMDBCNIDMapper::StoredMapping& mapping);

    lmdb::val& val() { return val_; }
    lmdb::val& cnidval() { return cnidval_; }
};

LMDBCNIDMapper::StoredMapping LMDBCNIDMapper::decodeMapping(CNID cnid, lmdb::val val)
{
    char *data = val.data();
    CNID parId = *reinterpret_cast<CNID*>(data);
    data += sizeof(CNID);
    uint8_t macNameLen = *reinterpret_cast<uint8_t*>(data);
    data++;
    mac_string macName(reinterpret_cast<unsigned char*>(data), macNameLen);
    data += macNameLen;
    uint16_t pathLen = *reinterpret_cast<uint16_t*>(data);
    data += sizeof(uint16_t);
    std::string pathString(data, pathLen);

    return StoredMapping {
        parId, cnid,
        fs::path(std::move(pathString)),
        std::move(macName)
    };
}

LMDBCNIDMapper::BinMapping::BinMapping(const StoredMapping& mapping)
    : data_(sizeof(CNID) + 1 + mapping.macname.size() + 2 + mapping.path.string().size())
    , val_(data_.data(), data_.size())
    , cnid(mapping.cnid)
    , cnidval_(&cnid, sizeof(CNID))
{
    char *data = data_.data();
    *reinterpret_cast<CNID*>(data) = mapping.parID;
    data += sizeof(CNID);
    *reinterpret_cast<uint8_t*>(data) = mapping.macname.size();
    data++;
    memcpy(data, mapping.macname.data(), mapping.macname.size());
    data += mapping.macname.size();
    *reinterpret_cast<uint16_t*>(data) = mapping.path.string().size();
    data += sizeof(uint16_t);
    memcpy(data, mapping.path.string().data(), mapping.path.string().size());
}


LMDBCNIDMapper::LMDBCNIDMapper(fs::path root, mac_string volumeName)
    : env_(lmdb::env::create())
{
    fs::create_directories(ROMlib_DirectoryMap);
    env_.set_max_dbs(3);
    env_.open(ROMlib_DirectoryMap.string().c_str());

    auto txn = lmdb::txn::begin(env_);
    config_ = lmdb::dbi::open(txn, "config", MDB_CREATE);
    mappings_ = lmdb::dbi::open(txn, "mappings", MDB_CREATE | MDB_INTEGERKEY);
    directories_ = lmdb::dbi::open(txn, "directories", MDB_CREATE | MDB_INTEGERKEY);
    
    BinMapping rootMapping({
        1, 2,
        root,
        volumeName
    });
    
    mappings_.put(txn, rootMapping.cnidval(), rootMapping.val());

    txn.commit();
}

template <class F>
auto growMapIfNecessary(lmdb::env& env, const F& f) -> decltype(f())
{
    try
    {
        return f();
    }
    catch(const lmdb::map_full_error&)
    {
        MDB_envinfo stat;
        lmdb::env_info(env, &stat);
        env.set_mapsize(stat.me_mapsize * 2);
        return growMapIfNecessary(env, f);
    }
}

LMDBCNIDMapper::~LMDBCNIDMapper()
{
}

std::optional<LMDBCNIDMapper::StoredMapping> LMDBCNIDMapper::getMapping(lmdb::txn& txn, CNID cnid)
{
    lmdb::val mapping;

    if(mappings_.get(txn, lmdb::val(&cnid, sizeof(cnid)), mapping))
        return decodeMapping(cnid, mapping);
    else
        return {};
}

void LMDBCNIDMapper::setMapping(lmdb::txn& txn, StoredMapping m)
{
    BinMapping binMapping{m};
    mappings_.put(txn, binMapping.cnidval(), binMapping.val());
}

void LMDBCNIDMapper::deleteMapping(lmdb::txn& txn, CNID cnid)
{
    // TODO
}

std::vector<CNID> LMDBCNIDMapper::getDirectory(lmdb::txn& txn, CNID cnid)
{
    lmdb::val cachedContents;
    
    directories_.get(txn, lmdb::val(&cnid, sizeof(cnid)), cachedContents);

    auto cacheBegin = cachedContents.data<CNID>();
    auto cacheEnd = cacheBegin + cachedContents.size() / sizeof(CNID);

    return { cacheBegin, cacheEnd };
}

void LMDBCNIDMapper::setDirectory(lmdb::txn& txn, CNID cnid, const std::vector<CNID>& contents)
{
    lmdb::val contentVal = lmdb::val(contents.data(), contents.size() * sizeof(CNID));    
    directories_.put(txn, lmdb::val(&cnid, sizeof(cnid)), contentVal);
}

std::vector<CNIDMapper::Mapping> LMDBCNIDMapper::updateDirectoryContents(CNID dirID,
        std::vector<DirEntry> sortedRealDirEntries)
{
    return growMapIfNecessary(env_, [this, dirID, sortedRealDirEntries] {
        auto txn = lmdb::txn::begin(env_);

        auto directoryContents = getDirectory(txn, dirID);

        std::vector<StoredMapping> mappings;
        mappings.reserve(directoryContents.size());

        for(CNID cnid : directoryContents)
        {
            if(auto optMapping = getMapping(txn, cnid))
                mappings.push_back(std::move(*optMapping));
        }

        std::vector<CNID> newContents;
        newContents.reserve(sortedRealDirEntries.size());
        
        CNID newCNID;
        {
            auto cursor = lmdb::cursor::open(txn, mappings_);
            lmdb::val cnidval;
            if(cursor.get(cnidval, MDB_LAST))
                newCNID = *cnidval.data<CNID>() + 1;
            else
                newCNID = 3;
        }

        std::set<mac_string> usedNames;

        auto cachedMappingIt = mappings.begin(), cachedMappingEnd = mappings.end();
        for(auto& entry : sortedRealDirEntries)
        {
            while(cachedMappingIt != cachedMappingEnd && cachedMappingIt->path < entry.path)
                ++cachedMappingIt;

            if(cachedMappingIt != cachedMappingEnd && cachedMappingIt->path == entry.path)
            {
                mac_string nameUpr = cachedMappingIt->macname;
                ROMlib_UprString(nameUpr.data(), false, nameUpr.size());
                usedNames.emplace(std::move(nameUpr));
                ++cachedMappingIt;
            }
        }

        std::vector<Mapping> dirMappings;
        dirMappings.reserve(sortedRealDirEntries.size());

        cachedMappingIt = mappings.begin(), cachedMappingEnd = mappings.end();
        for(auto& entry : sortedRealDirEntries)
        {
            while(cachedMappingIt != cachedMappingEnd && cachedMappingIt->path < entry.path)
            {
                // discard mapping
                deleteCNID(txn, cachedMappingIt->cnid);
                ++cachedMappingIt;
            }

            if(cachedMappingIt != cachedMappingEnd && cachedMappingIt->path == entry.path)
            {
                // use existing mapping
                dirMappings.push_back({cachedMappingIt->parID, cachedMappingIt->cnid, entry.path, cachedMappingIt->macname, {}, entry.isDir});
                newContents.push_back(cachedMappingIt->cnid);

                ++cachedMappingIt;
            }
            else
            {
                // new mapping
                mac_string macname;
                const fs::path& name = entry.path.filename();
                int index = 0;

                bool nameIsFree;
                do
                {
                    macname = toMacRomanFilename(name, index++);
                    mac_string nameUpr = macname;
                    ROMlib_UprString(nameUpr.data(), false, nameUpr.size());
                    nameIsFree = usedNames.emplace(std::move(nameUpr)).second;
                } while(!nameIsFree);

                assert(macname.size());

                Mapping newMapping {
                    dirID, newCNID++,
                    entry.path,
                    std::move(macname),
                    {},
                    entry.isDir
                };
                dirMappings.push_back(newMapping);
                newContents.push_back(newMapping.cnid);

                setMapping(txn, {
                    newMapping.parID, newMapping.cnid,
                    newMapping.path,
                    std::move(*newMapping.macname)
                });
            }
        }

        setDirectory(txn, dirID, newContents);

        txn.commit();

        return dirMappings;
    });
}


std::vector<CNIDMapper::Mapping> LMDBCNIDMapper::mapDirectoryContents(CNID dirID,
        std::vector<DirEntry> realDirEntries)
{
    auto txn = lmdb::txn::begin(env_, nullptr, MDB_RDONLY);
    auto directoryContents = getDirectory(txn, dirID);
    std::sort(realDirEntries.begin(), realDirEntries.end(),
        [](const DirEntry& a, const DirEntry& b) { return a.path < b.path; });

    if(directoryContents.size() != realDirEntries.size())
    {
        txn.abort();
        return updateDirectoryContents(dirID, realDirEntries);
    }

    std::vector<StoredMapping> mappings;
    mappings.reserve(realDirEntries.size());

    {
        auto cachedIt = directoryContents.begin(), cachedEnd = directoryContents.end();
        auto realIt = realDirEntries.begin();
        for(;cachedIt != cachedEnd; ++cachedIt, ++realIt)
        {
            auto optMapping = getMapping(txn, *cachedIt);

            if(!optMapping || optMapping->path != realIt->path)
            {
                txn.abort();
                return updateDirectoryContents(dirID, realDirEntries);
            }

            mappings.push_back(std::move(*optMapping));
        }
    }

    std::vector<Mapping> dirMappings;
    dirMappings.reserve(mappings.size());

    {
        auto cachedMappingIt = mappings.begin(), cachedMappingEnd = mappings.end();
        auto realIt = realDirEntries.begin();
        for(;cachedMappingIt != cachedMappingEnd; ++cachedMappingIt, ++realIt)
        {
            dirMappings.push_back({cachedMappingIt->parID, cachedMappingIt->cnid, realIt->path, cachedMappingIt->macname, {}, realIt->isDir});
        }
    }
    
    return dirMappings;
}

std::optional<CNIDMapper::Mapping> LMDBCNIDMapper::lookupCNID(CNID cnid)
{
    return growMapIfNecessary(env_, [this, cnid]() -> std::optional<CNIDMapper::Mapping> {
        for(int pass = 0; pass < 2; pass++)
        {
            auto txn = lmdb::txn::begin(env_, nullptr, pass ? MDB_RDONLY : 0);

            auto mapping = getMapping(txn, cnid);

            if(!mapping)
                return {};
                
            if(fs::exists(mapping->path))
            {
                try
                {
                    return Mapping {
                        mapping->parID,
                        mapping->cnid,
                        mapping->path,
                        mapping->macname,
                        {},
                        false
                    };
                }
                catch(const fs::filesystem_error&)
                {
                    // file no longer exists,
                    // pass through
                }
            }
                
            if(pass)
            {
                deleteCNID(txn, cnid);
                txn.commit();
            }
        }
        return {};
    });
}

CNID LMDBCNIDMapper::resolveChild(CNID parID, const fs::path& path,
    const FileIdentity& identity, bool isDir, std::uint64_t mtime)
{
    // The LMDB backend is disabled; it is reworked together with its indexes in
    // Phase 11 of docs/ai/2026-10-08-localvolume-staged-materialization-plan.md.
    throw std::logic_error("LMDBCNIDMapper::resolveChild not implemented");
}

std::optional<CNID> LMDBCNIDMapper::cnidForName(CNID parID, mac_string_view macName)
{
    throw std::logic_error("LMDBCNIDMapper::cnidForName not implemented");
}

void LMDBCNIDMapper::nameGroup(CNID parID, const std::vector<NameRequest>& group)
{
    throw std::logic_error("LMDBCNIDMapper::nameGroup not implemented");
}

void LMDBCNIDMapper::deleteCNID(lmdb::txn& txn, CNID cnid)
{
    auto mapping = getMapping(txn, cnid);
    if(!mapping)
        return;
    
    CNID parID = mapping->parID;

    auto cachedContents = getDirectory(txn, parID);

    cachedContents.erase(
        std::remove(cachedContents.begin(), cachedContents.end(), cnid),
        cachedContents.end());

    setDirectory(txn, parID, cachedContents);

    deleteMapping(txn, cnid);

    // deleteCNID is only invoked for empty directories by ItemCacher,
    // but mapDirectoryContents and lookupCNID above also invoke it
    // on files and directories that have been deleted or moved by other programs.
    for(CNID child : getDirectory(txn, cnid))
        deleteCNID(txn, child);
    directories_.del(txn, lmdb::val(&cnid, sizeof(cnid)));
}

void LMDBCNIDMapper::deleteCNID(CNID cnid)
{
    growMapIfNecessary(env_, [this, cnid] {
        auto txn = lmdb::txn::begin(env_);
        deleteCNID(txn, cnid);
        txn.commit();
    });
}
void LMDBCNIDMapper::moveCNID(CNID cnid, CNID newParentOrZero, mac_string_view newMacName, std::function<fs::path()> fsop)
{
    growMapIfNecessary(env_, [=] {
        auto txn = lmdb::txn::begin(env_);

        auto mapping = getMapping(txn, cnid);
        if(!mapping)
            return;

        CNID oldParent = mapping->parID;
        CNID newParent = newParentOrZero ? newParentOrZero : oldParent;
        if(oldParent == newParent && newMacName.empty())
            return;

        fs::path newPath = fsop();

        mapping->path = newPath;
        mapping->parID = newParent;
        if(!newMacName.empty())
            mapping->macname = newMacName;

        setMapping(txn, *mapping);

        auto cachedContents = getDirectory(txn, oldParent);
        cachedContents.erase(
            std::remove(cachedContents.begin(), cachedContents.end(), cnid),
            cachedContents.end());

        if(newParent != oldParent)
        {
            setDirectory(txn, oldParent, cachedContents);
            cachedContents = getDirectory(txn, newParent);
        }

        cachedContents.insert(
            std::upper_bound(cachedContents.begin(), cachedContents.end(), cnid,
                [&](CNID a, CNID b) {
                    return getMapping(txn, a)->path < getMapping(txn, b)->path;
                }),
            cnid);

        setDirectory(txn, newParent, cachedContents);

        txn.commit();
    });
}
