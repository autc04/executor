// Native-only low-level tests for the CNID mapper backends. These test the
// store contract only: records, the two parent-keyed indexes, the allocator and
// (for LMDB) persistence and the transaction seam. Policy belongs to ItemCache
// and is tested in `localvolume_itemcache.cpp`.

#include "gtest/gtest.h"

#include <file/localvolume/cnidmapper.h>
#include <file/localvolume/lmdbcnidmapper.h>
#include <file/localvolume/simplecnidmapper.h>
#include <rsys/filesystem.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

extern fs::path ExecutorTestTempDir;

using namespace Executor;

namespace
{
using Mapping = CNIDMapperBackend::Mapping;

mac_string makeName(const char* s)
{
    return mac_string(reinterpret_cast<const unsigned char*>(s));
}

mac_string_view view(const char* s)
{
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

void exerciseBackend(CNIDMapperBackend& backend)
{
    auto root = backend.get(2);
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(1, root->parID);
    EXPECT_TRUE(root->isDir);

    const CNID a = backend.allocate();
    const CNID b = backend.allocate();
    EXPECT_GE(a, 3);
    EXPECT_LT(a, b);

    Mapping record { 2, a, fs::path("a"), std::nullopt, identity(100), false };
    backend.put(record);
    backend.putChild(2, "a", a);
    backend.putName(2, view("A"), a);

    auto got = backend.get(a);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(2, got->parID);
    EXPECT_EQ("a", got->posixName.string());
    EXPECT_FALSE(got->macname.has_value());
    EXPECT_FALSE(got->isDir);
    EXPECT_EQ(100u, got->identity.inode);

    auto child = backend.getChild(2, "a");
    ASSERT_TRUE(child.has_value());
    EXPECT_EQ(a, child->cnid);

    auto children = backend.getChildren(2);
    ASSERT_EQ(1u, children.size());
    EXPECT_EQ("a", children[0].first.string());
    EXPECT_EQ(a, children[0].second);

    auto byName = backend.getName(2, view("A"));
    ASSERT_TRUE(byName.has_value());
    EXPECT_EQ(a, *byName);

    record.macname = makeName("A");
    record.identity = identity(200);
    backend.put(record);

    got = backend.get(a);
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->macname.has_value());
    EXPECT_EQ("A", toString(*got->macname));
    EXPECT_EQ(200u, got->identity.inode);

    backend.eraseName(2, view("A"));
    EXPECT_FALSE(backend.getName(2, view("A")).has_value());

    backend.eraseChild(2, "a");
    EXPECT_FALSE(backend.getChild(2, "a").has_value());
    EXPECT_TRUE(backend.getChildren(2).empty());

    backend.erase(a);
    EXPECT_FALSE(backend.get(a).has_value());
}

class LMDBFixture : public ::testing::Test
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
}

TEST(CNIDMapperContract, Simple)
{
    SimpleCNIDMapper backend(makeName("simple"));
    exerciseBackend(backend);
}

TEST_F(LMDBFixture, Contract)
{
    LMDBCNIDMapper backend(root_, makeName("contract"));
    exerciseBackend(backend);
}

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

TEST_F(LMDBFixture, PersistsRecordsIndexesAndAllocator)
{
    CNID a = 0;
    CNID b = 0;

    {
        LMDBCNIDMapper backend(root_, makeName("persist"));
        a = backend.allocate();
        b = backend.allocate();

        Mapping record { 2, a, fs::path("a"), makeName("A"), identity(100), false };
        backend.put(record);
        backend.putChild(2, "a", a);
        backend.putName(2, view("A"), a);
    }

    LMDBCNIDMapper backend(root_, makeName("persist"));

    auto record = backend.get(a);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(2, record->parID);
    EXPECT_EQ("a", record->posixName.string());
    ASSERT_TRUE(record->macname.has_value());
    EXPECT_EQ("A", toString(*record->macname));

    auto child = backend.getChild(2, "a");
    ASSERT_TRUE(child.has_value());
    EXPECT_EQ(a, child->cnid);

    auto byName = backend.getName(2, view("A"));
    ASSERT_TRUE(byName.has_value());
    EXPECT_EQ(a, *byName);

    const CNID c = backend.allocate();
    EXPECT_GT(c, b);
}

TEST_F(LMDBFixture, TransactionRollsBackOnException)
{
    LMDBCNIDMapper backend(root_, makeName("rollback"));
    const CNID a = backend.allocate();

    EXPECT_THROW(backend.withTransaction([&] {
        Mapping record { 2, a, fs::path("a"), std::nullopt, identity(100), false };
        backend.put(record);
        backend.putChild(2, "a", a);
        throw std::runtime_error("abort");
    }), std::runtime_error);

    EXPECT_FALSE(backend.get(a).has_value());
    EXPECT_FALSE(backend.getChild(2, "a").has_value());
    EXPECT_TRUE(backend.getChildren(2).empty());
}
