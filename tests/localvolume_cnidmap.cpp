// Native-only tests for the persistent CNID mapper. They drive `LMDBCNIDMapper`
// directly (not through the mounted LocalVolume), so "a restart" is simply
// destroying the mapper and reopening it for the same volume root.

#include "gtest/gtest.h"

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

CNIDMapper::DirEntry entry(const char* name, std::uint64_t inode, bool isDir)
{
    return CNIDMapper::DirEntry { fs::path(name), identity(inode), isDir };
}

const CNIDMapper::Mapping* find(const std::vector<CNIDMapper::Mapping>& mappings,
    const std::string& name)
{
    for(const auto& mapping : mappings)
        if(mapping.posixName.string() == name)
            return &mapping;
    return nullptr;
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
        LMDBCNIDMapper mapper(root_, makeName("restamp-diff"));
        CNIDMapper::DirEntry noIdentity { fs::path("a"), FileIdentity {}, false };
        auto mappings = mapper.mapDirectoryContents(2, { noIdentity });
        ASSERT_EQ(mappings.size(), 1u);
        a = mappings.at(0).cnid;

        auto rec = mapper.lookupCNID(a);
        ASSERT_TRUE(rec.has_value());
        EXPECT_FALSE(rec->identity.valid);
    }

    {
        LMDBCNIDMapper mapper(root_, makeName("restamp-diff"));
        // Validation is back on: the name+kind match re-adopts the live identity
        // without changing the CNID.
        auto mappings = mapper.mapDirectoryContents(2, { entry("a", 100, false) });
        ASSERT_EQ(mappings.size(), 1u);
        EXPECT_EQ(mappings.at(0).cnid, a);

        auto rec = mapper.lookupCNID(a);
        ASSERT_TRUE(rec.has_value());
        EXPECT_TRUE(rec->identity.valid);
        EXPECT_EQ(rec->identity.device, 1u);
        EXPECT_EQ(rec->identity.inode, 100u);
    }

    {
        LMDBCNIDMapper mapper(root_, makeName("restamp-diff"));
        // The reverse direction does not overwrite a usable identity with an
        // unusable one (as when the flag is enabled again).
        CNIDMapper::DirEntry noIdentity { fs::path("a"), FileIdentity {}, false };
        auto mappings = mapper.mapDirectoryContents(2, { noIdentity });
        ASSERT_EQ(mappings.size(), 1u);
        EXPECT_EQ(mappings.at(0).cnid, a);

        auto rec = mapper.lookupCNID(a);
        ASSERT_TRUE(rec.has_value());
        EXPECT_TRUE(rec->identity.valid);
        EXPECT_EQ(rec->identity.inode, 100u);
    }
}

TEST_F(CNIDMapFixture, GetOrCreateChildAdoptsInodesAfterNoInodeSession)
{
    CNID a = 0;

    {
        LMDBCNIDMapper mapper(root_, makeName("restamp-get"));
        CNIDMapper::DirEntry noIdentity { fs::path("a"), FileIdentity {}, false };
        auto mappings = mapper.mapDirectoryContents(2, { noIdentity });
        ASSERT_EQ(mappings.size(), 1u);
        a = mappings.at(0).cnid;
        ASSERT_FALSE(mapper.lookupCNID(a)->identity.valid);
    }

    LMDBCNIDMapper mapper(root_, makeName("restamp-get"));
    EXPECT_EQ(mapper.getOrCreateChild(2, "a", identity(100), false), a);

    auto rec = mapper.lookupCNID(a);
    ASSERT_TRUE(rec.has_value());
    EXPECT_TRUE(rec->identity.valid);
    EXPECT_EQ(rec->identity.inode, 100u);
}

TEST_F(CNIDMapFixture, RecordsAndNamesSurviveReopen)
{
    CNID dir = 0;
    CNID file = 0;

    {
        LMDBCNIDMapper mapper(root_, makeName("persist"));
        dir = mapper.getOrCreateChild(2, "dir", identity(10), true);
        mapper.nameGroup(2, { { dir, "dir", "dir" } });

        file = mapper.getOrCreateChild(dir, "file", identity(11), false);
        mapper.nameGroup(dir, { { file, "file", "file" } });

        ASSERT_GT(dir, 2);
        ASSERT_GT(file, dir);
    }

    LMDBCNIDMapper mapper(root_, makeName("persist"));

    auto dirRec = mapper.lookupCNID(dir);
    ASSERT_TRUE(dirRec.has_value());
    EXPECT_EQ(dirRec->parID, 2);
    EXPECT_TRUE(dirRec->isDir);
    ASSERT_TRUE(dirRec->macname.has_value());
    EXPECT_EQ(toString(*dirRec->macname), "dir");

    auto fileRec = mapper.lookupCNID(file);
    ASSERT_TRUE(fileRec.has_value());
    EXPECT_EQ(fileRec->parID, dir);
    EXPECT_FALSE(fileRec->isDir);
    ASSERT_TRUE(fileRec->macname.has_value());
    EXPECT_EQ(toString(*fileRec->macname), "file");

    EXPECT_EQ(mapper.cnidForName(2, view("dir")), std::optional<CNID>(dir));

    auto byPosix = mapper.lookupPosixName(dir, "file");
    ASSERT_TRUE(byPosix.has_value());
    EXPECT_EQ(byPosix->cnid, file);

    // The allocator continues rather than reusing a CNID from the old session.
    CNID other = mapper.getOrCreateChild(2, "other", identity(12), false);
    EXPECT_GT(other, file);
}

TEST_F(CNIDMapFixture, DiffPreservesCnidsAcrossReopen)
{
    CNID a = 0;
    CNID b = 0;

    {
        LMDBCNIDMapper mapper(root_, makeName("diff"));

        auto mappings = mapper.mapDirectoryContents(2,
            { entry("a", 100, false), entry("b", 101, false) });
        ASSERT_EQ(mappings.size(), 2u);
        a = find(mappings, "a")->cnid;
        b = find(mappings, "b")->cnid;
        ASSERT_GT(a, 2);
        ASSERT_GT(b, a);

        mapper.nameGroup(2, { { a, "a", "a" }, { b, "b", "b" } });
        ASSERT_TRUE(mapper.cnidForName(2, view("a")).has_value());
        ASSERT_TRUE(mapper.cnidForName(2, view("b")).has_value());
    }

    {
        LMDBCNIDMapper mapper(root_, makeName("diff"));

        // "b" was renamed to "c" while no mapper was open: the diff pairs by
        // inode, keeps the CNID and clears the decided name.
        auto mappings = mapper.mapDirectoryContents(2,
            { entry("a", 100, false), entry("c", 101, false) });
        ASSERT_EQ(mappings.size(), 2u);

        const CNIDMapper::Mapping* aMapping = find(mappings, "a");
        const CNIDMapper::Mapping* cMapping = find(mappings, "c");
        ASSERT_NE(aMapping, nullptr);
        ASSERT_NE(cMapping, nullptr);
        EXPECT_EQ(aMapping->cnid, a);
        EXPECT_EQ(cMapping->cnid, b);

        auto aRec = mapper.lookupCNID(a);
        ASSERT_TRUE(aRec.has_value());
        ASSERT_TRUE(aRec->macname.has_value());
        EXPECT_EQ(toString(*aRec->macname), "a");

        auto cRec = mapper.lookupCNID(b);
        ASSERT_TRUE(cRec.has_value());
        EXPECT_EQ(cRec->posixName.string(), "c");
        EXPECT_FALSE(cRec->macname.has_value());

        mapper.nameGroup(2, { { b, "c", "c" } });
        EXPECT_EQ(mapper.cnidForName(2, view("c")), std::optional<CNID>(b));
    }

    {
        LMDBCNIDMapper mapper(root_, makeName("diff"));
        EXPECT_EQ(mapper.cnidForName(2, view("a")), std::optional<CNID>(a));
        EXPECT_EQ(mapper.cnidForName(2, view("c")), std::optional<CNID>(b));
        EXPECT_FALSE(mapper.cnidForName(2, view("b")).has_value());
    }
}

TEST_F(CNIDMapFixture, IdentityChangeResetsTheMapping)
{
    CNID a = 0;

    {
        LMDBCNIDMapper mapper(root_, makeName("reset"));
        auto mappings = mapper.mapDirectoryContents(2, { entry("a", 100, false) });
        ASSERT_EQ(mappings.size(), 1u);
        a = mappings.at(0).cnid;
    }

    {
        LMDBCNIDMapper mapper(root_, makeName("reset"));
        // A new identity namespace (as if the device changed): Phase 11a accepts
        // that nothing matches, so the old record is deleted and a fresh CNID is
        // assigned.
        auto mappings = mapper.mapDirectoryContents(2, { entry("a", 200, false) });
        ASSERT_EQ(mappings.size(), 1u);
        EXPECT_NE(mappings.at(0).cnid, a);
        EXPECT_FALSE(mapper.lookupCNID(a).has_value());
    }
}

TEST_F(CNIDMapFixture, MoveAndDeleteSurviveReopen)
{
    CNID dir = 0;
    CNID other = 0;
    CNID file = 0;

    {
        LMDBCNIDMapper mapper(root_, makeName("move"));
        dir = mapper.getOrCreateChild(2, "dir", identity(20), true);
        other = mapper.getOrCreateChild(2, "other", identity(21), true);
        file = mapper.getOrCreateChild(dir, "file", identity(22), false);
        mapper.nameGroup(dir, { { file, "file", "file" } });

        // The host rename has already happened; the mapper only re-homes.
        mapper.moveCNID(file, other, view("renamed"), "renamed");
    }

    {
        LMDBCNIDMapper mapper(root_, makeName("move"));
        EXPECT_FALSE(mapper.lookupPosixName(dir, "file").has_value());

        auto moved = mapper.lookupPosixName(other, "renamed");
        ASSERT_TRUE(moved.has_value());
        EXPECT_EQ(moved->cnid, file);
        ASSERT_TRUE(moved->macname.has_value());
        EXPECT_EQ(toString(*moved->macname), "renamed");

        mapper.deleteCNID(other);
    }

    {
        LMDBCNIDMapper mapper(root_, makeName("move"));
        EXPECT_FALSE(mapper.lookupCNID(other).has_value());
        EXPECT_FALSE(mapper.lookupCNID(file).has_value());   // recursive delete
        EXPECT_FALSE(mapper.lookupPosixName(2, "other").has_value());
    }
}

}
