#include "lmdbcnidmapper.h"

#include <OSUtil.h>
#include <rsys/paths.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

using namespace Executor;

namespace
{
constexpr std::uint32_t kSchemaVersion = 1;

// The stored record is a versioned byte string, not a host-endian struct, so the
// database stays portable between builds on the same machine and cannot be
// misread if a field is added later. Integers are big-endian, and a fixed
// 4-byte parent id prefixes every index key so a directory's children are a
// contiguous cursor range.
using Bytes = std::string;

void appendBE(Bytes& out, std::uint64_t value, int bytes)
{
    for(int i = bytes - 1; i >= 0; --i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

std::uint64_t readBE(const char*& p, const char* end, int bytes)
{
    if(end - p < bytes)
        throw std::runtime_error("corrupt CNID record");

    std::uint64_t value = 0;
    for(int i = 0; i < bytes; ++i)
        value = (value << 8) | static_cast<unsigned char>(*p++);
    return value;
}

Bytes encodeCNID(CNID cnid)
{
    Bytes out;
    appendBE(out, static_cast<std::uint32_t>(cnid), 4);
    return out;
}

CNID readCNID(const char* data, std::size_t size)
{
    const char* p = data;
    return static_cast<CNID>(readBE(p, data + size, 4));
}

// Config values are stored big-endian like the records, so the handful of
// scalars in the config DB have one representation.
void putConfigU32(lmdb::txn& txn, lmdb::dbi& db, const char* key,
    std::uint32_t value)
{
    const Bytes keyBytes(key, std::strlen(key));
    Bytes valueBytes;
    appendBE(valueBytes, value, 4);
    lmdb::val k { keyBytes.data(), keyBytes.size() };
    lmdb::val v { valueBytes.data(), valueBytes.size() };
    db.put(txn, k, v);
}

std::optional<std::uint32_t> getConfigU32(lmdb::txn& txn, lmdb::dbi& db,
    const char* key)
{
    const Bytes keyBytes(key, std::strlen(key));
    lmdb::val k { keyBytes.data(), keyBytes.size() };
    lmdb::val v;
    if(!db.get(txn, k, v) || v.size() < 4)
        return {};

    const char* p = v.data();
    return static_cast<std::uint32_t>(readBE(p, v.data() + v.size(), 4));
}

Bytes parentPrefix(CNID parID)
{
    return encodeCNID(parID);
}

Bytes posixKey(CNID parID, const fs::path& name)
{
    Bytes key = parentPrefix(parID);
    key += name.string();
    return key;
}

Bytes nameKey(CNID parID, mac_string_view folded)
{
    Bytes key = parentPrefix(parID);
    key.append(reinterpret_cast<const char*>(folded.data()), folded.size());
    return key;
}

Bytes cnidKey(CNID cnid)
{
    return encodeCNID(cnid);
}

mac_string foldName(mac_string_view name)
{
    mac_string folded { name };
    ROMlib_UprString(folded.data(), false, folded.size());
    return folded;
}

// A stable, deterministic per-volume directory name: FNV-1a over the root path.
// The root itself is stored in the config DB and checked on open, so a hash
// collision is detected rather than silently mixing two volumes.
std::string volumeKey(const fs::path& root)
{
    std::uint64_t h = 1469598103934665603ull;
    for(unsigned char c : root.string())
    {
        h ^= c;
        h *= 1099511628211ull;
    }

    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx",
        static_cast<unsigned long long>(h));
    return buffer;
}

Bytes encodeRecord(const LMDBCNIDMapper::StoredMapping& rec)
{
    Bytes out;

    appendBE(out, static_cast<std::uint32_t>(rec.parID), 4);

    std::uint8_t flags = (rec.isDir ? 1 : 0) | (rec.macname ? 2 : 0);
    out.push_back(static_cast<char>(flags));

    const std::string name = rec.posixName.string();
    appendBE(out, name.size(), 2);
    out += name;

    if(rec.macname)
    {
        appendBE(out, rec.macname->size(), 2);
        out.append(reinterpret_cast<const char*>(rec.macname->data()),
            rec.macname->size());
    }

    appendBE(out, rec.identity.device, 8);
    appendBE(out, rec.identity.inode, 8);
    out.push_back(rec.identity.valid ? 1 : 0);

    return out;
}

LMDBCNIDMapper::StoredMapping decodeRecord(CNID cnid, const char* data,
    std::size_t size)
{
    const char* p = data;
    const char* end = data + size;
    LMDBCNIDMapper::StoredMapping rec;

    rec.cnid = cnid;
    rec.parID = static_cast<CNID>(readBE(p, end, 4));

    if(p == end)
        throw std::runtime_error("corrupt CNID record");
    const std::uint8_t flags = static_cast<unsigned char>(*p++);
    rec.isDir = (flags & 1) != 0;

    const std::uint64_t nameLen = readBE(p, end, 2);
    if(static_cast<std::size_t>(end - p) < nameLen)
        throw std::runtime_error("corrupt CNID record");
    rec.posixName = fs::path(std::string(p, static_cast<std::size_t>(nameLen)));
    p += nameLen;

    if(flags & 2)
    {
        const std::uint64_t macLen = readBE(p, end, 2);
        if(static_cast<std::size_t>(end - p) < macLen)
            throw std::runtime_error("corrupt CNID record");
        rec.macname = mac_string(reinterpret_cast<const unsigned char*>(p),
            static_cast<std::size_t>(macLen));
        p += macLen;
    }

    rec.identity.device = readBE(p, end, 8);
    rec.identity.inode = readBE(p, end, 8);
    if(p == end)
        throw std::runtime_error("corrupt CNID record");
    rec.identity.valid = static_cast<unsigned char>(*p++) != 0;

    return rec;
}

CNIDMapper::Mapping toMapping(const LMDBCNIDMapper::StoredMapping& rec)
{
    return CNIDMapper::Mapping {
        rec.parID, rec.cnid, rec.posixName, rec.macname, rec.identity, rec.isDir
    };
}

// Retry a whole read-modify-write after the map grows. A map-full error aborts
// the transaction, so re-running the callback starts from a clean state.
template <class F>
auto growMapIfNecessary(lmdb::env& env, const F& f) -> decltype(f())
{
    for(;;)
    {
        try
        {
            return f();
        }
        catch(const lmdb::map_full_error&)
        {
            MDB_envinfo info;
            lmdb::env_info(env, &info);
            env.set_mapsize(info.me_mapsize * 2);
        }
        catch(const lmdb::error& e)
        {
            if(e.code() == MDB_MAP_RESIZED)
            {
                env.set_mapsize(0);
                continue;
            }
            throw;
        }
    }
}
}

LMDBCNIDMapper::LMDBCNIDMapper(fs::path root, mac_string volumeName)
    : env_(lmdb::env::create())
{
    // One environment per volume, under the configured directory-map folder.
    const fs::path envPath = ROMlib_DirectoryMap / ("v-" + volumeKey(root));
    fs::create_directories(envPath);

    env_.set_max_dbs(4);
    // A generous mapping avoids growth in normal use (it is virtual address
    // space, not allocation). Growth is still handled if it runs out.
    env_.set_mapsize(std::size_t{256} * 1024 * 1024);
    env_.open(envPath.string().c_str());

    auto txn = lmdb::txn::begin(env_);
    config_ = lmdb::dbi::open(txn, "config", MDB_CREATE);
    mappings_ = lmdb::dbi::open(txn, "mappings", MDB_CREATE);
    posixNames_ = lmdb::dbi::open(txn, "posixNames", MDB_CREATE);
    names_ = lmdb::dbi::open(txn, "names", MDB_CREATE);

    std::uint32_t schema = 0;
    if(auto stored = getConfigU32(txn, config_, "schema"))
        schema = *stored;
    else
    {
        schema = kSchemaVersion;
        putConfigU32(txn, config_, "schema", schema);
    }
    if(schema != kSchemaVersion)
        throw std::runtime_error("unsupported CNID database schema");

    // The environment is per volume, so a different root here means a hash
    // collision or a reused folder; refuse rather than mix CNIDs.
    lmdb::val rootKey { "root", 4 };
    lmdb::val rootValue;
    const std::string rootString = root.string();
    if(config_.get(txn, rootKey, rootValue))
    {
        if(std::string(rootValue.data(), rootValue.size()) != rootString)
            throw std::runtime_error("CNID database belongs to a different volume");
    }
    else
    {
        lmdb::val value { rootString.data(), rootString.size() };
        config_.put(txn, rootKey, value);
    }

    if(!getMapping(txn, 2))
    {
        StoredMapping rootRecord;
        rootRecord.parID = 1;
        rootRecord.cnid = 2;
        rootRecord.macname = volumeName;
        rootRecord.isDir = true;
        putMapping(txn, rootRecord);
    }

    std::uint32_t nextCNID = 3;
    if(auto stored = getConfigU32(txn, config_, "nextCNID"))
        nextCNID = *stored;
    else
        putConfigU32(txn, config_, "nextCNID", nextCNID);

    txn.commit();
}

LMDBCNIDMapper::~LMDBCNIDMapper() = default;

std::optional<LMDBCNIDMapper::StoredMapping> LMDBCNIDMapper::getMapping(
    lmdb::txn& txn, CNID cnid)
{
    Bytes keyBytes = cnidKey(cnid);
    lmdb::val key { keyBytes.data(), keyBytes.size() };
    lmdb::val value;

    if(!mappings_.get(txn, key, value))
        return {};

    return decodeRecord(cnid, value.data(), value.size());
}

void LMDBCNIDMapper::putMapping(lmdb::txn& txn, const StoredMapping& rec)
{
    Bytes keyBytes = cnidKey(rec.cnid);
    Bytes valueBytes = encodeRecord(rec);
    lmdb::val key { keyBytes.data(), keyBytes.size() };
    lmdb::val value { valueBytes.data(), valueBytes.size() };

    mappings_.put(txn, key, value);
}

CNID LMDBCNIDMapper::allocateCNID(lmdb::txn& txn)
{
    const std::uint32_t next = getConfigU32(txn, config_, "nextCNID").value_or(3);
    putConfigU32(txn, config_, "nextCNID", next + 1);
    return static_cast<CNID>(next);
}

std::optional<CNID> LMDBCNIDMapper::getPosixName(lmdb::txn& txn, CNID parID,
    const fs::path& name)
{
    Bytes keyBytes = posixKey(parID, name);
    lmdb::val key { keyBytes.data(), keyBytes.size() };
    lmdb::val value;

    if(!posixNames_.get(txn, key, value))
        return {};

    return readCNID(value.data(), value.size());
}

void LMDBCNIDMapper::putPosixName(lmdb::txn& txn, CNID parID,
    const fs::path& name, CNID cnid)
{
    Bytes keyBytes = posixKey(parID, name);
    Bytes valueBytes = encodeCNID(cnid);
    lmdb::val key { keyBytes.data(), keyBytes.size() };
    lmdb::val value { valueBytes.data(), valueBytes.size() };

    posixNames_.put(txn, key, value);
}

void LMDBCNIDMapper::delPosixName(lmdb::txn& txn, CNID parID,
    const fs::path& name)
{
    Bytes keyBytes = posixKey(parID, name);
    lmdb::val key { keyBytes.data(), keyBytes.size() };

    posixNames_.del(txn, key);
}

std::vector<std::pair<fs::path, CNID>> LMDBCNIDMapper::getChildren(
    lmdb::txn& txn, CNID parID)
{
    std::vector<std::pair<fs::path, CNID>> children;
    const Bytes prefix = parentPrefix(parID);

    auto cursor = lmdb::cursor::open(txn, posixNames_);
    lmdb::val key { prefix.data(), prefix.size() };
    lmdb::val value;

    bool found = cursor.get(key, value, MDB_SET_RANGE);
    while(found)
    {
        if(key.size() < prefix.size()
           || std::memcmp(key.data(), prefix.data(), prefix.size()) != 0)
            break;

        children.emplace_back(
            fs::path(std::string(key.data() + prefix.size(),
                key.size() - prefix.size())),
            readCNID(value.data(), value.size()));

        found = cursor.get(key, value, MDB_NEXT);
    }

    return children;
}

std::optional<CNID> LMDBCNIDMapper::getName(lmdb::txn& txn, CNID parID,
    mac_string_view folded)
{
    Bytes keyBytes = nameKey(parID, folded);
    lmdb::val key { keyBytes.data(), keyBytes.size() };
    lmdb::val value;

    if(!names_.get(txn, key, value))
        return {};

    return readCNID(value.data(), value.size());
}

void LMDBCNIDMapper::putName(lmdb::txn& txn, CNID parID, mac_string_view folded,
    CNID cnid)
{
    Bytes keyBytes = nameKey(parID, folded);
    Bytes valueBytes = encodeCNID(cnid);
    lmdb::val key { keyBytes.data(), keyBytes.size() };
    lmdb::val value { valueBytes.data(), valueBytes.size() };

    names_.put(txn, key, value);
}

void LMDBCNIDMapper::delName(lmdb::txn& txn, CNID parID, mac_string_view folded)
{
    Bytes keyBytes = nameKey(parID, folded);
    lmdb::val key { keyBytes.data(), keyBytes.size() };

    names_.del(txn, key);
}

void LMDBCNIDMapper::forgetRecordName(lmdb::txn& txn, const StoredMapping& rec)
{
    if(!rec.macname)
        return;

    mac_string folded = foldName(*rec.macname);
    if(auto existing = getName(txn, rec.parID, folded); existing && *existing == rec.cnid)
        delName(txn, rec.parID, folded);
}

CNID LMDBCNIDMapper::createRecord(lmdb::txn& txn, CNID parID,
    const fs::path& posixName, const FileIdentity& identity, bool isDir)
{
    CNID cnid = allocateCNID(txn);

    StoredMapping rec;
    rec.parID = parID;
    rec.cnid = cnid;
    rec.posixName = posixName;
    rec.identity = identity;
    rec.isDir = isDir;

    putMapping(txn, rec);
    putPosixName(txn, parID, posixName, cnid);

    return cnid;
}

void LMDBCNIDMapper::deleteCNID(lmdb::txn& txn, CNID cnid)
{
    auto rec = getMapping(txn, cnid);
    if(!rec)
        return;

    forgetRecordName(txn, *rec);
    delPosixName(txn, rec->parID, rec->posixName);

    // Collect the children before mutating, so deleting from the index while
    // iterating it is not an issue.
    std::vector<CNID> children;
    for(auto& child : getChildren(txn, cnid))
        children.push_back(child.second);

    Bytes keyBytes = cnidKey(cnid);
    lmdb::val key { keyBytes.data(), keyBytes.size() };
    mappings_.del(txn, key);

    for(CNID child : children)
        deleteCNID(txn, child);
}

std::vector<CNIDMapper::Mapping> LMDBCNIDMapper::mapDirectoryContents(
    CNID dirID, std::vector<DirEntry> entries)
{
    return growMapIfNecessary(env_, [this, dirID, entries]() {
        auto txn = lmdb::txn::begin(env_);

        // The stored child set, in posix-name order. An index entry whose record
        // is gone is an orphan; drop it.
        std::vector<StoredMapping> stored;
        for(auto& child : getChildren(txn, dirID))
        {
            if(auto rec = getMapping(txn, child.second))
                stored.push_back(std::move(*rec));
            else
                delPosixName(txn, dirID, child.first);
        }

        std::unordered_map<CNID, std::size_t> storedIndex;
        std::map<std::string, std::size_t> storedByName;
        for(std::size_t i = 0; i < stored.size(); ++i)
        {
            storedIndex[stored[i].cnid] = i;
            storedByName[stored[i].posixName.string()] = i;
        }

        const std::size_t n = entries.size();
        std::vector<bool> consumed(n, false);
        std::set<CNID> surviving;

        // Pass 1: a child whose name, kind and identity still match a scanned
        // entry is unchanged.
        for(std::size_t i = 0; i < n; ++i)
        {
            auto it = storedByName.find(entries[i].posixName.string());
            if(it == storedByName.end())
                continue;

            StoredMapping& rec = stored[it->second];
            if(rec.isDir != entries[i].isDir
               || !sameIdentity(rec.identity, entries[i].identity))
                continue;

            consumed[i] = true;
            surviving.insert(rec.cnid);
        }

        // The inode pairing for rename recovery is scratch, built from the scan.
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::size_t>> byInode;
        for(std::size_t i = 0; i < n; ++i)
            if(entries[i].identity.valid && !consumed[i])
                byInode[{ entries[i].identity.device, entries[i].identity.inode }]
                    .push_back(i);

        // Pass 2: a child that was not kept was renamed, split, or is gone.
        // Process in CNID order so the result does not depend on scan order.
        std::vector<CNID> remaining;
        for(auto& rec : stored)
            if(!surviving.count(rec.cnid))
                remaining.push_back(rec.cnid);
        std::sort(remaining.begin(), remaining.end());

        for(CNID cnid : remaining)
        {
            StoredMapping& rec = stored[storedIndex.at(cnid)];

            std::vector<std::size_t> occurrences;
            if(rec.identity.valid)
                if(auto bit = byInode.find({ rec.identity.device, rec.identity.inode });
                   bit != byInode.end())
                    for(std::size_t i : bit->second)
                        if(!consumed[i])
                            occurrences.push_back(i);

            if(occurrences.size() == 1
               && entries[occurrences[0]].isDir == rec.isDir)
            {
                // Exactly one occurrence at a new name: the entry was renamed.
                std::size_t i = occurrences[0];
                consumed[i] = true;

                forgetRecordName(txn, rec);
                delPosixName(txn, dirID, rec.posixName);
                rec.macname.reset();
                rec.posixName = entries[i].posixName;
                putMapping(txn, rec);
                putPosixName(txn, dirID, rec.posixName, rec.cnid);
                surviving.insert(rec.cnid);
            }
            else
            {
                // No occurrence: gone. Several: the old CNID is dropped and each
                // occurrence becomes a new entry.
                deleteCNID(txn, rec.cnid);
            }
        }

        std::vector<Mapping> result;
        result.reserve(n);

        for(auto& rec : stored)
            if(surviving.count(rec.cnid))
                result.push_back(toMapping(rec));

        for(std::size_t i = 0; i < n; ++i)
            if(!consumed[i])
            {
                CNID cnid = createRecord(txn, dirID, entries[i].posixName,
                    entries[i].identity, entries[i].isDir);

                Mapping mapping;
                mapping.parID = dirID;
                mapping.cnid = cnid;
                mapping.posixName = entries[i].posixName;
                mapping.identity = entries[i].identity;
                mapping.isDir = entries[i].isDir;
                result.push_back(std::move(mapping));
            }

        txn.commit();
        return result;
    });
}

std::optional<CNIDMapper::Mapping> LMDBCNIDMapper::lookupCNID(CNID cnid)
{
    auto txn = lmdb::txn::begin(env_, nullptr, MDB_RDONLY);

    if(auto rec = getMapping(txn, cnid))
        return toMapping(*rec);

    return {};
}

std::optional<CNIDMapper::Mapping> LMDBCNIDMapper::lookupPosixName(CNID parID,
    const fs::path& posixName)
{
    auto txn = lmdb::txn::begin(env_, nullptr, MDB_RDONLY);

    if(auto cnid = getPosixName(txn, parID, posixName))
        if(auto rec = getMapping(txn, *cnid))
            return toMapping(*rec);

    return {};
}

CNID LMDBCNIDMapper::createRecord(CNID parID, const fs::path& posixName,
    const FileIdentity& identity, bool isDir)
{
    return growMapIfNecessary(env_, [&] {
        auto txn = lmdb::txn::begin(env_);
        CNID cnid = createRecord(txn, parID, posixName, identity, isDir);
        txn.commit();
        return cnid;
    });
}

CNID LMDBCNIDMapper::getOrCreateChild(CNID parID, const fs::path& posixName,
    const FileIdentity& identity, bool isDir)
{
    return growMapIfNecessary(env_, [&] {
        auto txn = lmdb::txn::begin(env_);

        if(auto existing = getPosixName(txn, parID, posixName))
        {
            auto rec = getMapping(txn, *existing);
            if(rec && rec->isDir == isDir
               && sameIdentity(rec->identity, identity))
            {
                txn.abort();
                return *existing;
            }

            if(rec)
                deleteCNID(txn, *existing);
            else
                delPosixName(txn, parID, posixName);
        }

        CNID cnid = createRecord(txn, parID, posixName, identity, isDir);
        txn.commit();
        return cnid;
    });
}

std::optional<CNID> LMDBCNIDMapper::cnidForName(CNID parID,
    mac_string_view macName)
{
    auto txn = lmdb::txn::begin(env_, nullptr, MDB_RDONLY);
    return getName(txn, parID, foldName(macName));
}

void LMDBCNIDMapper::nameGroup(CNID parID,
    const std::vector<NameRequest>& group)
{
    growMapIfNecessary(env_, [&] {
        auto txn = lmdb::txn::begin(env_);

        for(const NameRequest& req : group)
        {
            auto rec = getMapping(txn, req.cnid);
            if(!rec || rec->parID != parID)
                continue;
            if(rec->posixName != req.posixName)
                continue;   // the binding changed after the listing
            if(rec->macname)
                continue;   // already decided; names are sticky

            int index = 0;
            for(;;)
            {
                mac_string candidate = toMacRomanFilename(req.nameSource, index++);
                mac_string folded = foldName(candidate);
                if(!getName(txn, parID, folded))
                {
                    rec->macname = std::move(candidate);
                    putMapping(txn, *rec);
                    putName(txn, parID, folded, req.cnid);
                    break;
                }
            }
        }

        txn.commit();
    });
}

void LMDBCNIDMapper::deleteCNID(CNID cnid)
{
    growMapIfNecessary(env_, [&] {
        auto txn = lmdb::txn::begin(env_);
        deleteCNID(txn, cnid);
        txn.commit();
    });
}

void LMDBCNIDMapper::moveCNID(CNID cnid, CNID newParent,
    mac_string_view newMacName, const fs::path& newPosixName)
{
    growMapIfNecessary(env_, [&] {
        auto txn = lmdb::txn::begin(env_);

        auto rec = getMapping(txn, cnid);
        if(!rec)
            return;

        const CNID oldParent = rec->parID;
        const CNID targetParent = newParent == 0 ? oldParent : newParent;
        if(oldParent == targetParent && newMacName.empty()
           && newPosixName == rec->posixName)
            return;

        // Both name maps are keyed by parent, so a move re-homes the entries.
        forgetRecordName(txn, *rec);
        delPosixName(txn, oldParent, rec->posixName);

        rec->parID = targetParent;
        rec->posixName = newPosixName;
        if(!newMacName.empty())
            rec->macname = mac_string(newMacName.data(), newMacName.size());

        putMapping(txn, *rec);
        putPosixName(txn, targetParent, rec->posixName, cnid);
        if(rec->macname)
            putName(txn, targetParent, foldName(*rec->macname), cnid);

        txn.commit();
    });
}
