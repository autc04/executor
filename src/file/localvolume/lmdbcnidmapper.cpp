#include "lmdbcnidmapper.h"

#include <rsys/paths.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

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

CNID readCNID(const char *data, std::size_t size)
{
    const char *p = data;
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
    lmdb::val k{keyBytes.data(), keyBytes.size()};
    lmdb::val v{valueBytes.data(), valueBytes.size()};
    db.put(txn, k, v);
}

std::optional<std::uint32_t> getConfigU32(lmdb::txn& txn, lmdb::dbi& db,
                                          const char* key)
{
    const Bytes keyBytes(key, std::strlen(key));
    lmdb::val k{keyBytes.data(), keyBytes.size()};
    lmdb::val v;
    if(!db.get(txn, k, v) || v.size() < 4)
        return {};

    const char *p = v.data();
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
    key.append(reinterpret_cast<const char *>(folded.data()), folded.size());
    return key;
}

Bytes cnidKey(CNID cnid)
{
    return encodeCNID(cnid);
}

Bytes encodeRecord(const CNIDRecord& rec)
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
        out.append(reinterpret_cast<const char *>(rec.macname->data()),
                   rec.macname->size());
    }

    appendBE(out, rec.identity.device, 8);
    appendBE(out, rec.identity.inode, 8);
    out.push_back(rec.identity.valid ? 1 : 0);

    return out;
}

CNIDRecord decodeRecord(CNID cnid, const char *data, std::size_t size)
{
    const char *p = data;
    const char* end = data + size;
    CNIDRecord rec;

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
        rec.macname = mac_string(reinterpret_cast<const unsigned char *>(p),
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

// Run `f`, retrying after a map-size change. A map-full error aborts the
// transaction, so the callback is re-run from a clean state; MDB_MAP_RESIZED
// means another process grew the map, so the environment adopts the new size
// and the callback is re-run. This wraps both write transactions and read-only
// lookups.
template<class F>
auto withMapRetry(lmdb::env& env, const F& f) -> decltype(f())
{
    for(;;)
    {
        try
        {
            return f();
        }
        catch(const lmdb::map_full_error &)
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
} // namespace

fs::path LMDBCNIDMapper::volumeDirectoryName(const fs::path& root)
{
    std::string name = root.string();

    // Normalize away trailing separators and a Windows trailing colon:
    // "/" -> "", "C:/" -> "C", "C:\\" -> "C".
    while(!name.empty() && (name.back() == '/' || name.back() == '\\'))
        name.pop_back();
    if(!name.empty() && name.back() == ':')
        name.pop_back();

    if(name.empty())
        return "root";

    // A bare drive letter is a Windows drive root.
    if(name.size() == 1 && std::isalpha(static_cast<unsigned char>(name[0])))
        return fs::path(std::string("drive-")
                        + static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))));

    // Anything else: a readable, separator-free name. The stored root check
    // catches the (currently unreachable) case of two roots sanitizing alike.
    std::string readable;
    bool pendingDash = false;
    for(char c : name)
    {
        if(c == '/' || c == '\\' || c == ':')
        {
            pendingDash = !readable.empty();
            continue;
        }

        if(pendingDash)
        {
            readable.push_back('-');
            pendingDash = false;
        }
        readable.push_back(c);
    }

    if(readable.empty())
        return "root";
    return fs::path("path-" + readable);
}

LMDBCNIDMapper::LMDBCNIDMapper(fs::path root, mac_string volumeName)
    : env_(lmdb::env::create())
{
    // One environment per volume, under the configured directory-map folder.
    const fs::path envPath = ROMlib_DirectoryMap / volumeDirectoryName(root);
    fs::create_directories(envPath);

    env_.set_max_dbs(4);
    // A generous mapping avoids growth in normal use (it is virtual address
    // space, not allocation). Growth is still handled if it runs out.
    env_.set_mapsize(std::size_t{256} * 1024 * 1024);
    env_.open(envPath.string().c_str());

    withMapRetry(env_, [&]
                 {
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
            CNIDRecord rootRecord;
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

        txn.commit(); });
}

LMDBCNIDMapper::~LMDBCNIDMapper() = default;

std::optional<CNIDRecord> LMDBCNIDMapper::getMapping(lmdb::txn& txn, CNID cnid)
{
    Bytes keyBytes = cnidKey(cnid);
    lmdb::val key{keyBytes.data(), keyBytes.size()};
    lmdb::val value;

    if(!mappings_.get(txn, key, value))
        return {};

    return decodeRecord(cnid, value.data(), value.size());
}

void LMDBCNIDMapper::putMapping(lmdb::txn& txn, const CNIDRecord& rec)
{
    Bytes keyBytes = cnidKey(rec.cnid);
    Bytes valueBytes = encodeRecord(rec);
    lmdb::val key{keyBytes.data(), keyBytes.size()};
    lmdb::val value{valueBytes.data(), valueBytes.size()};

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
    lmdb::val key{keyBytes.data(), keyBytes.size()};
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
    lmdb::val key{keyBytes.data(), keyBytes.size()};
    lmdb::val value{valueBytes.data(), valueBytes.size()};

    posixNames_.put(txn, key, value);
}

void LMDBCNIDMapper::delPosixName(lmdb::txn& txn, CNID parID,
                                  const fs::path& name)
{
    Bytes keyBytes = posixKey(parID, name);
    lmdb::val key{keyBytes.data(), keyBytes.size()};

    posixNames_.del(txn, key);
}

std::vector<std::pair<fs::path, CNID>> LMDBCNIDMapper::getChildren(
    lmdb::txn& txn, CNID parID)
{
    std::vector<std::pair<fs::path, CNID>> children;
    const Bytes prefix = parentPrefix(parID);

    auto cursor = lmdb::cursor::open(txn, posixNames_);
    lmdb::val key{prefix.data(), prefix.size()};
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
    lmdb::val key{keyBytes.data(), keyBytes.size()};
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
    lmdb::val key{keyBytes.data(), keyBytes.size()};
    lmdb::val value{valueBytes.data(), valueBytes.size()};

    names_.put(txn, key, value);
}

void LMDBCNIDMapper::delName(lmdb::txn& txn, CNID parID, mac_string_view folded)
{
    Bytes keyBytes = nameKey(parID, folded);
    lmdb::val key{keyBytes.data(), keyBytes.size()};

    names_.del(txn, key);
}

std::optional<CNIDRecord> LMDBCNIDMapper::get(CNID cnid)
{
    if(activeTxn_)
        return getMapping(*activeTxn_, cnid);

    return withMapRetry(env_, [&]() -> std::optional<CNIDRecord>
                        {
        auto txn = lmdb::txn::begin(env_, nullptr, MDB_RDONLY);
        return getMapping(txn, cnid); });
}

std::optional<CNIDRecord> LMDBCNIDMapper::getChild(CNID parID,
                                                   const fs::path& posixName)
{
    if(activeTxn_)
    {
        if(auto cnid = getPosixName(*activeTxn_, parID, posixName))
            return getMapping(*activeTxn_, *cnid);
        return {};
    }

    return withMapRetry(env_, [&]() -> std::optional<CNIDRecord>
                        {
        auto txn = lmdb::txn::begin(env_, nullptr, MDB_RDONLY);
        if(auto cnid = getPosixName(txn, parID, posixName))
            return getMapping(txn, *cnid);
        return {}; });
}

std::vector<std::pair<fs::path, CNID>> LMDBCNIDMapper::getChildren(CNID parID)
{
    if(activeTxn_)
        return getChildren(*activeTxn_, parID);

    return withMapRetry(env_, [&]
                        {
        auto txn = lmdb::txn::begin(env_, nullptr, MDB_RDONLY);
        return getChildren(txn, parID); });
}

std::optional<CNID> LMDBCNIDMapper::getName(CNID parID,
                                            mac_string_view foldedMacName)
{
    if(activeTxn_)
        return getName(*activeTxn_, parID, foldedMacName);

    return withMapRetry(env_, [&]() -> std::optional<CNID>
                        {
        auto txn = lmdb::txn::begin(env_, nullptr, MDB_RDONLY);
        return getName(txn, parID, foldedMacName); });
}

void LMDBCNIDMapper::put(const CNIDRecord& record)
{
    if(activeTxn_)
    {
        putMapping(*activeTxn_, record);
        return;
    }

    withMapRetry(env_, [&]
                 {
        auto txn = lmdb::txn::begin(env_);
        putMapping(txn, record);
        txn.commit(); });
}

void LMDBCNIDMapper::erase(CNID cnid)
{
    if(activeTxn_)
    {
        Bytes keyBytes = cnidKey(cnid);
        lmdb::val key{keyBytes.data(), keyBytes.size()};
        mappings_.del(*activeTxn_, key);
        return;
    }

    withMapRetry(env_, [&]
                 {
        auto txn = lmdb::txn::begin(env_);
        Bytes keyBytes = cnidKey(cnid);
        lmdb::val key { keyBytes.data(), keyBytes.size() };
        mappings_.del(txn, key);
        txn.commit(); });
}

void LMDBCNIDMapper::putChild(CNID parID, const fs::path& posixName, CNID cnid)
{
    if(activeTxn_)
    {
        putPosixName(*activeTxn_, parID, posixName, cnid);
        return;
    }

    withMapRetry(env_, [&]
                 {
        auto txn = lmdb::txn::begin(env_);
        putPosixName(txn, parID, posixName, cnid);
        txn.commit(); });
}

void LMDBCNIDMapper::eraseChild(CNID parID, const fs::path& posixName)
{
    if(activeTxn_)
    {
        delPosixName(*activeTxn_, parID, posixName);
        return;
    }

    withMapRetry(env_, [&]
                 {
        auto txn = lmdb::txn::begin(env_);
        delPosixName(txn, parID, posixName);
        txn.commit(); });
}

void LMDBCNIDMapper::putName(CNID parID, mac_string_view foldedMacName, CNID cnid)
{
    if(activeTxn_)
    {
        putName(*activeTxn_, parID, foldedMacName, cnid);
        return;
    }

    withMapRetry(env_, [&]
                 {
        auto txn = lmdb::txn::begin(env_);
        putName(txn, parID, foldedMacName, cnid);
        txn.commit(); });
}

void LMDBCNIDMapper::eraseName(CNID parID, mac_string_view foldedMacName)
{
    if(activeTxn_)
    {
        delName(*activeTxn_, parID, foldedMacName);
        return;
    }

    withMapRetry(env_, [&]
                 {
        auto txn = lmdb::txn::begin(env_);
        delName(txn, parID, foldedMacName);
        txn.commit(); });
}

CNID LMDBCNIDMapper::allocate()
{
    if(activeTxn_)
        return allocateCNID(*activeTxn_);

    return withMapRetry(env_, [&]
                        {
        auto txn = lmdb::txn::begin(env_);
        CNID cnid = allocateCNID(txn);
        txn.commit();
        return cnid; });
}

void LMDBCNIDMapper::withTransaction(const std::function<void()>& f)
{
    assert(activeTxn_ == nullptr);

    withMapRetry(env_, [&]
                 {
        auto txn = lmdb::txn::begin(env_);

        struct ActiveTxn
        {
            lmdb::txn*& slot;
            ~ActiveTxn() { slot = nullptr; }
        } active { activeTxn_ };
        activeTxn_ = &txn;

        f();
        txn.commit(); });
}
