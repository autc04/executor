// Native-only tests for `ItemCache`'s CNID policy. They use the in-memory
// `SimpleCNIDMapper` backend and drive the policy through the public ItemCache /
// DirectoryItem / Item API, with real host files. The backend's own persistence
// and primitive contract live in `localvolume_cnidmap.cpp`.

#include "gtest/gtest.h"

#include <file/localvolume/item.h>
#include <file/localvolume/itemcache.h>
#include <file/localvolume/plain.h>
#include <file/localvolume/simplecnidmapper.h>
#include <rsys/filesystem.h>

#include <cstring>
#include <memory>
#include <set>
#include <string>

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
    return mac_string_view(reinterpret_cast<const unsigned char*>(s),
        std::strlen(s));
}

std::string toString(const mac_string& s)
{
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

class PlainOnlyItemFactory : public ItemFactory
{
public:
    FileRepPtr createRepForFile(ItemCache&, FileItem& item,
        const fs::directory_entry&, ProbeTrace&) override
    {
        return std::make_shared<PlainRep>(item);
    }
};

class ItemCachePolicyFixture : public ::testing::Test
{
protected:
    PlainOnlyItemFactory factory_;
    fs::path root_;
    std::unique_ptr<ItemCache> cache_;

    void SetUp() override
    {
        root_ = ExecutorTestTempDir / fs::unique_path("itemcache-%%%%-%%%%");
        fs::create_directory(root_);
        cache_ = std::make_unique<ItemCache>(root_, makeName("policy"),
            std::make_unique<SimpleCNIDMapper>(makeName("policy")), &factory_);
    }

    void TearDown() override
    {
        cache_.reset();
        boost::system::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path host(const std::string& rel) const { return root_ / rel; }

    void hostDir(const std::string& rel) { fs::create_directories(host(rel)); }

    void hostFile(const std::string& rel, const std::string& contents = {})
    {
        fs::create_directories(host(rel).parent_path());
        fs::ofstream out(host(rel), std::ios::binary);
        out.write(contents.data(), contents.size());
    }

    // Nudge a directory's mtime so the validation stamp notices the change even
    // on filesystems with coarse timestamp granularity.
    void bumpMtime(const std::string& rel)
    {
        auto p = host(rel);
        fs::last_write_time(p, fs::last_write_time(p) + 1);
    }

    bool haveIdentity() const { return statPath(root_).identity.valid; }

    ItemPtr realize(const std::string& rel)
    {
        return cache_->realizeByHostPath(host(rel));
    }

    DirectoryItemPtr realizeDir(const std::string& rel)
    {
        return std::dynamic_pointer_cast<DirectoryItem>(realize(rel));
    }

    static ItemPtr child(const DirectoryItemPtr& dir, const std::string& name)
    {
        return dir->child(mac_string_view(
            reinterpret_cast<const unsigned char*>(name.data()), name.size()));
    }
};

TEST_F(ItemCachePolicyFixture, DiffKeepsCnidsAndRecoversSameDirectoryRename)
{
    if(!haveIdentity())
        GTEST_SKIP() << "host filesystem exposes no stable identities";

    hostFile("d/a", "a");
    hostFile("d/b", "b");

    auto d = realizeDir("d");
    ASSERT_TRUE(d);
    ASSERT_TRUE(d->ensureListed());
    auto a = child(d, "a");
    auto b = child(d, "b");
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    const CNID aCnid = a->cnid();
    const CNID bCnid = b->cnid();

    fs::rename(host("d/b"), host("d/c"));
    bumpMtime("d");
    d->clearCache();
    ASSERT_TRUE(d->ensureListed());

    auto c = child(d, "c");
    ASSERT_TRUE(c);
    EXPECT_EQ(bCnid, c->cnid());
    EXPECT_FALSE(child(d, "b"));
    auto a2 = child(d, "a");
    ASSERT_TRUE(a2);
    EXPECT_EQ(aCnid, a2->cnid());
}

// A host-side rename over an existing entry is one entry left with the
// survivor's identity, at a name the deleted record also used. Rename recovery
// rebinds that name to the survivor (the lower CNID is processed first), and the
// stale record's removal must not erase the rebinding.
TEST_F(ItemCachePolicyFixture, OverwriteRenameKeepsSurvivingChildIndex)
{
    if(!haveIdentity())
        GTEST_SKIP() << "host filesystem exposes no stable identities";

    // Create the entries in separate listings: CNIDs are allocated in scan
    // order, which readdir does not define, so this pins "a" below "b".
    hostFile("d/a", "a");
    auto d = realizeDir("d");
    ASSERT_TRUE(d);
    ASSERT_TRUE(d->ensureListed());
    auto a = child(d, "a");
    ASSERT_TRUE(a);

    hostFile("d/b", "b");
    bumpMtime("d");
    d->clearCache();
    ASSERT_TRUE(d->ensureListed());
    auto b = child(d, "b");
    ASSERT_TRUE(b);

    const CNID aCnid = a->cnid();
    const CNID bCnid = b->cnid();
    ASSERT_LT(aCnid, bCnid);

    // POSIX rename replaces "b"; "a"'s identity now lives at "b".
    fs::rename(host("d/a"), host("d/b"));
    bumpMtime("d");
    d->clearCache();
    ASSERT_TRUE(d->ensureListed());
    auto b2 = child(d, "b");
    ASSERT_TRUE(b2);
    EXPECT_EQ(aCnid, b2->cnid());
    EXPECT_FALSE(cache_->realizeByCNID(bCnid));

    // The persisted child index must still reach the survivor, so a later scan
    // does not mint a new CNID for it. Read the cnid out of the fresh listing
    // itself: a lookup by name would still find the record through the surviving
    // Mac-name entry and hide the lost index.
    d->clearCache();
    ASSERT_TRUE(d->ensureListed());
    ASSERT_EQ(1, d->countItems());
    auto listed = d->child(1, true);
    ASSERT_TRUE(listed);
    EXPECT_EQ(aCnid, listed->cnid());
}

// Two renames in sequence: "b" to the free name "c", then "a" over "b". Both
// records are renamed, and the second recovery re-homes onto a name the first
// recovery just took. Neither may erase the other's child index.
TEST_F(ItemCachePolicyFixture, RenameChainKeepsBothChildIndexes)
{
    if(!haveIdentity())
        GTEST_SKIP() << "host filesystem exposes no stable identities";

    hostFile("d/a", "a");
    auto d = realizeDir("d");
    ASSERT_TRUE(d);
    ASSERT_TRUE(d->ensureListed());
    auto a = child(d, "a");
    ASSERT_TRUE(a);

    hostFile("d/b", "b");
    bumpMtime("d");
    d->clearCache();
    ASSERT_TRUE(d->ensureListed());
    auto b = child(d, "b");
    ASSERT_TRUE(b);

    const CNID aCnid = a->cnid();
    const CNID bCnid = b->cnid();
    ASSERT_LT(aCnid, bCnid);

    fs::rename(host("d/b"), host("d/c"));
    fs::rename(host("d/a"), host("d/b"));
    bumpMtime("d");
    d->clearCache();
    ASSERT_TRUE(d->ensureListed());
    auto b2 = child(d, "b");
    auto c2 = child(d, "c");
    ASSERT_TRUE(b2);
    ASSERT_TRUE(c2);
    EXPECT_EQ(aCnid, b2->cnid());
    EXPECT_EQ(bCnid, c2->cnid());

    d->clearCache();
    ASSERT_TRUE(d->ensureListed());
    ASSERT_EQ(2, d->countItems());
    std::set<CNID> listedCnids;
    for(int i = 1; i <= d->countItems(); ++i)
        if(auto listed = d->child(i, true))
            listedCnids.insert(listed->cnid());
    EXPECT_EQ((std::set<CNID> { aCnid, bCnid }), listedCnids);
}

TEST_F(ItemCachePolicyFixture, DiffDropsRemovedChild)
{
    hostFile("d/a", "a");

    auto d = realizeDir("d");
    ASSERT_TRUE(d);
    ASSERT_TRUE(d->ensureListed());
    auto a = child(d, "a");
    ASSERT_TRUE(a);
    const CNID cnid = a->cnid();

    fs::remove(host("d/a"));
    bumpMtime("d");
    d->clearCache();
    ASSERT_TRUE(d->ensureListed());

    EXPECT_FALSE(child(d, "a"));
    EXPECT_FALSE(cache_->realizeByCNID(cnid));
}

TEST_F(ItemCachePolicyFixture, NameGroupDisambiguatesCaseInsensitiveCollisions)
{
    hostFile("d/A", "upper");
    hostFile("d/a", "lower");

    boost::system::error_code ec;
    if(fs::equivalent(host("d/A"), host("d/a"), ec))
        GTEST_SKIP() << "host filesystem is case-insensitive";

    auto d = realizeDir("d");
    ASSERT_TRUE(d);
    ASSERT_TRUE(d->ensureListed());

    auto first = d->child(1, true);
    auto second = d->child(2, true);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    // Descriptors sort as (folded prefix, posix name), so "A" comes first and
    // the later "a" must take a disambiguator to stay distinct case-insensitively.
    EXPECT_EQ("A", toString(first->name()));
    EXPECT_EQ("a#1", toString(second->name()));
}

TEST_F(ItemCachePolicyFixture, GetOrCreateAdoptsIdentityAfterNoInodeSession)
{
    if(!haveIdentity())
        GTEST_SKIP() << "host filesystem exposes no stable identities";

    hostFile("a", "a");

    struct Restore
    {
        bool saved = dontUseInodes;
        ~Restore() { dontUseInodes = saved; }
    } restore;

    dontUseInodes = true;
    auto item = realize("a");
    ASSERT_TRUE(item);
    const CNID cnid = item->cnid();

    dontUseInodes = false;
    auto again = realize("a");
    ASSERT_TRUE(again);
    EXPECT_EQ(cnid, again->cnid());

    // Replacing the file with one that already has a different inode proves the
    // record re-adopted an identity: without one, sameIdentity() would report
    // "same" and keep the old CNID.
    hostFile("tmp", "replacement");
    fs::remove(host("a"));
    fs::rename(host("tmp"), host("a"));

    auto replaced = realize("a");
    ASSERT_TRUE(replaced);
    EXPECT_NE(cnid, replaced->cnid());
}

TEST_F(ItemCachePolicyFixture, DiffAdoptsIdentityAfterNoInodeSession)
{
    if(!haveIdentity())
        GTEST_SKIP() << "host filesystem exposes no stable identities";

    hostFile("d/a", "a");

    struct Restore
    {
        bool saved = dontUseInodes;
        ~Restore() { dontUseInodes = saved; }
    } restore;

    dontUseInodes = true;
    auto d = realizeDir("d");
    ASSERT_TRUE(d);
    ASSERT_TRUE(d->ensureListed());
    auto a = child(d, "a");
    ASSERT_TRUE(a);
    const CNID cnid = a->cnid();

    dontUseInodes = false;
    d->clearCache();
    ASSERT_TRUE(d->ensureListed());
    auto again = child(d, "a");
    ASSERT_TRUE(again);
    EXPECT_EQ(cnid, again->cnid());

    // A replacement with a distinct inode must renumber once the diff has
    // re-adopted an identity (see the get-or-create test above).
    hostFile("d/tmp", "replacement");
    fs::remove(host("d/a"));
    fs::rename(host("d/tmp"), host("d/a"));
    bumpMtime("d");
    d->clearCache();
    ASSERT_TRUE(d->ensureListed());

    auto replaced = child(d, "a");
    ASSERT_TRUE(replaced);
    EXPECT_NE(cnid, replaced->cnid());
}

TEST_F(ItemCachePolicyFixture, RenameKeepsCnid)
{
    hostFile("f", "data");

    auto f = realize("f");
    ASSERT_TRUE(f);
    const CNID cnid = f->cnid();

    cache_->renameItem(f, view("renamed"));

    EXPECT_FALSE(fs::exists(host("f")));
    EXPECT_TRUE(fs::exists(host("renamed")));

    auto renamed = realize("renamed");
    ASSERT_TRUE(renamed);
    EXPECT_EQ(cnid, renamed->cnid());
    EXPECT_FALSE(realize("f"));
}

TEST_F(ItemCachePolicyFixture, MoveKeepsCnidAndRehomesRecord)
{
    hostDir("src");
    hostDir("dst");
    hostFile("src/f", "data");

    auto src = realizeDir("src");
    auto dst = realizeDir("dst");
    ASSERT_TRUE(src);
    ASSERT_TRUE(dst);
    auto f = child(src, "f");
    ASSERT_TRUE(f);
    const CNID cnid = f->cnid();

    cache_->moveItem(f, dst);

    EXPECT_FALSE(fs::exists(host("src/f")));
    EXPECT_TRUE(fs::exists(host("dst/f")));

    auto moved = realize("dst/f");
    ASSERT_TRUE(moved);
    EXPECT_EQ(cnid, moved->cnid());
    EXPECT_FALSE(realize("src/f"));

    // The record, not just the live Item, was re-homed: both listings agree.
    dst->clearCache();
    ASSERT_TRUE(dst->ensureListed());
    auto inDst = child(dst, "f");
    ASSERT_TRUE(inDst);
    EXPECT_EQ(cnid, inDst->cnid());

    src->clearCache();
    ASSERT_TRUE(src->ensureListed());
    EXPECT_FALSE(child(src, "f"));
}

TEST_F(ItemCachePolicyFixture, RecursiveDeleteDropsDescendantRecords)
{
    hostFile("d/sub/f", "data");

    auto d = realizeDir("d");
    ASSERT_TRUE(d);
    ASSERT_TRUE(d->ensureListed());
    auto sub = std::dynamic_pointer_cast<DirectoryItem>(child(d, "sub"));
    ASSERT_TRUE(sub);
    ASSERT_TRUE(sub->ensureListed());
    auto f = child(sub, "f");
    ASSERT_TRUE(f);

    const CNID subCnid = sub->cnid();
    const CNID fileCnid = f->cnid();

    // Leave the directory empty on the host, but keep the descendant records.
    fs::remove_all(host("d/sub"));

    cache_->deleteItem(d);

    EXPECT_FALSE(cache_->realizeByCNID(subCnid));
    EXPECT_FALSE(cache_->realizeByCNID(fileCnid));
}

}
