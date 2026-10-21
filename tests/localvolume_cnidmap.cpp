// Native-only tests for the persistent CNID store. They drive `ItemCache` with
// an `LMDBCNIDMapper` backend (not through the mounted LocalVolume), so "a
// restart" is simply destroying the cache and reopening it for the same volume
// root. The policy under test belongs to `ItemCache`, not to the backend.

#include "gtest/gtest.h"

#include <file/localvolume/itemcache.h>
#include <file/localvolume/lmdbcnidmapper.h>
#include <rsys/filesystem.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

extern fs::path ExecutorTestTempDir;

using namespace Executor;

namespace
{
mac_string makeName(const char* s)
{
    return mac_string(reinterpret_cast<const unsigned char*>(s));
}

mac_string_view view(const char* s)
{
    // mac_string_view(const unsigned char*) treats its argument as a Pascal
    // string, so pass an explicit length here.
    return mac_string_view(reinterpret_cast<const unsigned char*>(s),
        std::strlen(s));
}

std::string toString(const mac_string& s)
{
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

FileIdentity identity(std::uint64_t inode)
{
    return FileIdentity { 1, inode, true };
}

ItemCache::DirEntry entry(const char* name, std::uint64_t inode, bool isDir)
{
    return ItemCache::DirEntry { fs::path(name), identity(inode), isDir };
}

const CNIDRecord* find(const std::vector<CNIDRecord>& mappings,
    const std::string& name)
{
    for(const auto& mapping : mappings)
        if(mapping.posixName.string() == name)
            return &mapping;
    return nullptr;
}

std::unique_ptr<ItemCache> makeCache(const fs::path& root, const char* volumeName)
{
    return std::make_unique<ItemCache>(root, makeName(volumeName),
        std::make_unique<LMDBCNIDMapper>(root, makeName(volumeName)), nullptr);
}

class CNIDMapFixture : public ::testing::Test
{
protected:
    fs::path root_;

    void SetUp() override
    {
        root_ = ExecutorTestTempDir / fs::unique_path("cnidmap-%%%%-%%%%");
        fs::create_directory(root_);
    }

    void TearDown() override
    {
        boost::system::error_code ec;
        fs::remove_all(root_, ec);
    }
};

TEST(CNIDMapNaming, ReadableVolumeDirectories)
{
    EXPECT_EQ(LMDBCNIDMapper::volumeDirectoryName("/").string(), "root");
    EXPECT_EQ(LMDBCNIDMapper::volumeDirectoryName("C:/").string(), "drive-C");
    EXPECT_EQ(LMDBCNIDMapper::volumeDirectoryName("c:\\").string(), "drive-C");
    EXPECT_EQ(LMDBCNIDMapper::volumeDirectoryName("D:/").string(), "drive-D");
    EXPECT_EQ(LMDBCNIDMapper::volumeDirectoryName("/home/alice/data").string(),
        "path-home-alice-data");
    EXPECT_EQ(LMDBCNIDMapper::volumeDirectoryName("C:/Users/Alice").string(),
        "path-C-Users-Alice");
}

TEST_F(CNIDMapFixture, DontUseInodesDisablesIdentity)
{
    struct Restore
    {
        bool saved = dontUseInodes;
        ~Restore() { dontUseInodes = saved; }
    } restore;

    PathStat withInodes = statPath(root_);
    ASSERT_TRUE(withInodes.exists);
    if(!withInodes.identity.valid)
        GTEST_SKIP() << "host filesystem exposes no stable identities";
    EXPECT_TRUE(withInodes.identity.valid);

    dontUseInodes = true;

    PathStat withoutInodes = statPath(root_);
    ASSERT_TRUE(withoutInodes.exists);
    EXPECT_FALSE(withoutInodes.identity.valid);
    // An unusable identity is treated as "same", so the diff falls back to
    // name+kind matching and skips the inode pairing.
    EXPECT_TRUE(sameIdentity(withInodes.identity, withoutInodes.identity));
}

TEST_F(CNIDMapFixture, DiffAdoptsInodesAfterNoInodeSession)
{
    CNID a = 0;

    {
        auto cache = makeCache(root_, "restamp-diff");
        ItemCache::DirEntry noIdentity { fs::path("a"), FileIdentity {}, false };
        auto mappings = cache->mapDirectoryContents(2, { noIdentity });
        ASSERT_EQ(mappings.size(), 1u);
        a = mappings.at(0).cnid;

        auto rec = cache->lookupCNID(a);
        ASSERT_TRUE(rec.has_value());
        EXPECT_FALSE(rec->identity.valid);
    }

    {
        auto cache = makeCache(root_, "restamp-diff");
        // Validation is back on: the name+kind match re-adopts the live identity
        // without changing the CNID.
        auto mappings = cache->mapDirectoryContents(2, { entry("a", 100, false) });
        ASSERT_EQ(mappings.size(), 1u);
        EXPECT_EQ(mappings.at(0).cnid, a);

        auto rec = cache->lookupCNID(a);
        ASSERT_TRUE(rec.has_value());
        EXPECT_TRUE(rec->identity.valid);
        EXPECT_EQ(rec->identity.device, 1u);
        EXPECT_EQ(rec->identity.inode, 100u);
    }

    {
        auto cache = makeCache(root_, "restamp-diff");
        // The reverse direction does not overwrite a usable identity with an
        // unusable one (as when the flag is enabled again).
        ItemCache::DirEntry noIdentity { fs::path("a"), FileIdentity {}, false };
        auto mappings = cache->mapDirectoryContents(2, { noIdentity });
        ASSERT_EQ(mappings.size(), 1u);
        EXPECT_EQ(mappings.at(0).cnid, a);

        auto rec = cache->lookupCNID(a);
        ASSERT_TRUE(rec.has_value());
        EXPECT_TRUE(rec->identity.valid);
        EXPECT_EQ(rec->identity.inode, 100u);
    }
}

TEST_F(CNIDMapFixture, GetOrCreateChildAdoptsInodesAfterNoInodeSession)
{
    CNID a = 0;

    {
        auto cache = makeCache(root_, "restamp-get");
        ItemCache::DirEntry noIdentity { fs::path("a"), FileIdentity {}, false };
        auto mappings = cache->mapDirectoryContents(2, { noIdentity });
        ASSERT_EQ(mappings.size(), 1u);
        a = mappings.at(0).cnid;
        ASSERT_FALSE(cache->lookupCNID(a)->identity.valid);
    }

    auto cache = makeCache(root_, "restamp-get");
    EXPECT_EQ(cache->getOrCreateChild(2, "a", identity(100), false), a);

    auto rec = cache->lookupCNID(a);
    ASSERT_TRUE(rec.has_value());
    EXPECT_TRUE(rec->identity.valid);
    EXPECT_EQ(rec->identity.inode, 100u);
}

TEST_F(CNIDMapFixture, RecordsAndNamesSurviveReopen)
{
    CNID dir = 0;
    CNID file = 0;

    {
        auto cache = makeCache(root_, "persist");
        dir = cache->getOrCreateChild(2, "dir", identity(10), true);
        cache->nameGroup(2, { { dir, "dir", "dir" } });

        file = cache->getOrCreateChild(dir, "file", identity(11), false);
        cache->nameGroup(dir, { { file, "file", "file" } });

        ASSERT_GT(dir, 2);
        ASSERT_GT(file, dir);
    }

    auto cache = makeCache(root_, "persist");

    auto dirRec = cache->lookupCNID(dir);
    ASSERT_TRUE(dirRec.has_value());
    EXPECT_EQ(dirRec->parID, 2);
    EXPECT_TRUE(dirRec->isDir);
    ASSERT_TRUE(dirRec->macname.has_value());
    EXPECT_EQ(toString(*dirRec->macname), "dir");

    auto fileRec = cache->lookupCNID(file);
    ASSERT_TRUE(fileRec.has_value());
    EXPECT_EQ(fileRec->parID, dir);
    EXPECT_FALSE(fileRec->isDir);
    ASSERT_TRUE(fileRec->macname.has_value());
    EXPECT_EQ(toString(*fileRec->macname), "file");

    EXPECT_EQ(cache->cnidForName(2, view("dir")), std::optional<CNID>(dir));

    auto byPosix = cache->lookupPosixName(dir, "file");
    ASSERT_TRUE(byPosix.has_value());
    EXPECT_EQ(byPosix->cnid, file);

    // The allocator continues rather than reusing a CNID from the old session.
    CNID other = cache->getOrCreateChild(2, "other", identity(12), false);
    EXPECT_GT(other, file);
}

TEST_F(CNIDMapFixture, DiffPreservesCnidsAcrossReopen)
{
    CNID a = 0;
    CNID b = 0;

    {
        auto cache = makeCache(root_, "diff");

        auto mappings = cache->mapDirectoryContents(2,
            { entry("a", 100, false), entry("b", 101, false) });
        ASSERT_EQ(mappings.size(), 2u);
        a = find(mappings, "a")->cnid;
        b = find(mappings, "b")->cnid;
        ASSERT_GT(a, 2);
        ASSERT_GT(b, a);

        cache->nameGroup(2, { { a, "a", "a" }, { b, "b", "b" } });
        ASSERT_TRUE(cache->cnidForName(2, view("a")).has_value());
        ASSERT_TRUE(cache->cnidForName(2, view("b")).has_value());
    }

    {
        auto cache = makeCache(root_, "diff");

        // "b" was renamed to "c" while no mapper was open: the diff pairs by
        // inode, keeps the CNID and clears the decided name.
        auto mappings = cache->mapDirectoryContents(2,
            { entry("a", 100, false), entry("c", 101, false) });
        ASSERT_EQ(mappings.size(), 2u);

        const CNIDRecord* aMapping = find(mappings, "a");
        const CNIDRecord* cMapping = find(mappings, "c");
        ASSERT_NE(aMapping, nullptr);
        ASSERT_NE(cMapping, nullptr);
        EXPECT_EQ(aMapping->cnid, a);
        EXPECT_EQ(cMapping->cnid, b);

        auto aRec = cache->lookupCNID(a);
        ASSERT_TRUE(aRec.has_value());
        ASSERT_TRUE(aRec->macname.has_value());
        EXPECT_EQ(toString(*aRec->macname), "a");

        auto cRec = cache->lookupCNID(b);
        ASSERT_TRUE(cRec.has_value());
        EXPECT_EQ(cRec->posixName.string(), "c");
        EXPECT_FALSE(cRec->macname.has_value());

        cache->nameGroup(2, { { b, "c", "c" } });
        EXPECT_EQ(cache->cnidForName(2, view("c")), std::optional<CNID>(b));
    }

    {
        auto cache = makeCache(root_, "diff");
        EXPECT_EQ(cache->cnidForName(2, view("a")), std::optional<CNID>(a));
        EXPECT_EQ(cache->cnidForName(2, view("c")), std::optional<CNID>(b));
        EXPECT_FALSE(cache->cnidForName(2, view("b")).has_value());
    }
}

TEST_F(CNIDMapFixture, IdentityChangeResetsTheMapping)
{
    CNID a = 0;

    {
        auto cache = makeCache(root_, "reset");
        auto mappings = cache->mapDirectoryContents(2, { entry("a", 100, false) });
        ASSERT_EQ(mappings.size(), 1u);
        a = mappings.at(0).cnid;
    }

    {
        auto cache = makeCache(root_, "reset");
        // A new identity namespace (as if the device changed): Phase 11a accepts
        // that nothing matches, so the old record is deleted and a fresh CNID is
        // assigned.
        auto mappings = cache->mapDirectoryContents(2, { entry("a", 200, false) });
        ASSERT_EQ(mappings.size(), 1u);
        EXPECT_NE(mappings.at(0).cnid, a);
        EXPECT_FALSE(cache->lookupCNID(a).has_value());
    }
}

TEST_F(CNIDMapFixture, MoveAndDeleteSurviveReopen)
{
    CNID dir = 0;
    CNID other = 0;
    CNID file = 0;

    {
        auto cache = makeCache(root_, "move");
        dir = cache->getOrCreateChild(2, "dir", identity(20), true);
        other = cache->getOrCreateChild(2, "other", identity(21), true);
        file = cache->getOrCreateChild(dir, "file", identity(22), false);
        cache->nameGroup(dir, { { file, "file", "file" } });

        // The host rename has already happened; the mapper only re-homes.
        cache->moveCNID(file, other, view("renamed"), "renamed");
    }

    {
        auto cache = makeCache(root_, "move");
        EXPECT_FALSE(cache->lookupPosixName(dir, "file").has_value());

        auto moved = cache->lookupPosixName(other, "renamed");
        ASSERT_TRUE(moved.has_value());
        EXPECT_EQ(moved->cnid, file);
        ASSERT_TRUE(moved->macname.has_value());
        EXPECT_EQ(toString(*moved->macname), "renamed");

        cache->deleteCNID(other);
    }

    {
        auto cache = makeCache(root_, "move");
        EXPECT_FALSE(cache->lookupCNID(other).has_value());
        EXPECT_FALSE(cache->lookupCNID(file).has_value());   // recursive delete
        EXPECT_FALSE(cache->lookupPosixName(2, "other").has_value());
    }
}

}
