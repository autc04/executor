// Native-only tests that drive the Mac FileManager against the mounted-'/'
// LocalVolume and interleave host (std::filesystem) operations.
//
// These are the regression net for the staged-materialization refactor
// (docs/ai/2026-10-08-localvolume-staged-materialization-plan.md, Phase 0):
// they exercise listing, creation/deletion, move/rename, and interleaved
// sequences, and assert the invariants the refactor must preserve (CNID
// stability, count-vs-listing agreement, "enumerate one level only").

#include "gtest/gtest.h"
#include "localvolume_test_util.h"

#include <file/file.h>
#include <file/localvolume/stats.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Executor;
using namespace Executor::Test;

namespace
{
std::set<std::string> namesOf(const std::vector<ListEntry>& v)
{
    std::set<std::string> s;
    for(auto& e : v)
        s.insert(e.name);
    return s;
}

std::set<std::string> setOf(const std::vector<std::string>& v)
{
    return { v.begin(), v.end() };
}

// A >31-byte host name. The Mac name is truncated to 30 bytes + ellipsis, so
// names sharing a 30-byte prefix collapse to the same Mac name and force
// disambiguation — without relying on case-insensitive comparison.
std::string longCollidingName(const std::string& tag)
{
    return std::string(30, 'a') + tag;
}

// Minimal well-formed MacBinary II builder, for the forward-looking test below.
void putU32BE(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void putU16BE(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

uint16_t crc16(const uint8_t* p, size_t n)
{
    uint16_t crc = 0;
    while(n--)
    {
        crc ^= (uint16_t)(*p++) << 8;
        for(int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint64_t pad128(uint64_t n) { return (n + 127) & ~(uint64_t)127; }

std::vector<uint8_t> makeMacBinary(std::string_view name,
    const std::vector<uint8_t>& data, const std::vector<uint8_t>& rsrc)
{
    std::vector<uint8_t> h(128, 0);
    h[1] = (uint8_t)name.size();
    std::memcpy(&h[2], name.data(), name.size());
    putU32BE(&h[83], (uint32_t)data.size());
    putU32BE(&h[87], (uint32_t)rsrc.size());
    putU32BE(&h[91], 0x11111111);
    putU32BE(&h[95], 0x22222222);
    h[122] = 129;
    h[123] = 129;
    putU32BE(&h[116], (uint32_t)(data.size() + rsrc.size()));
    putU16BE(&h[124], crc16(h.data(), 124));

    std::vector<uint8_t> out = h;
    out.resize(128 + (size_t)pad128(data.size()), 0);
    std::memcpy(out.data() + 128, data.data(), data.size());
    out.resize(128 + (size_t)pad128(data.size()) + (size_t)pad128(rsrc.size()), 0);
    std::memcpy(out.data() + 128 + pad128(data.size()), rsrc.data(), rsrc.size());
    return out;
}
} // namespace

// --------------------------------------------------------------------------
// Listing
// --------------------------------------------------------------------------

TEST_F(LocalVolumeFixture, ListingEmptyDirectory)
{
    EXPECT_EQ(0, *fmChildCount(rootDirID_));
    EXPECT_TRUE(fmList(rootDirID_).empty());
    EXPECT_TRUE(fmListFiles(rootDirID_).empty());
}

TEST_F(LocalVolumeFixture, ListingHostTree)
{
    hostDir("a");
    hostFile("a/x", "x");
    hostFile("a/y", "yy");
    hostDir("a/sub");

    auto a = fmGet(rootDirID_, "a");
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(a->isDir);

    EXPECT_EQ((std::set<std::string> { "sub", "x", "y" }), namesOf(fmList(a->cnid)));
    EXPECT_EQ((std::set<std::string> { "x", "y" }), namesOf(fmListFiles(a->cnid)));
    EXPECT_EQ(3, *fmChildCount(a->cnid));
}

TEST_F(LocalVolumeFixture, ListingMatchesHost)
{
    hostFile("f1", "");
    hostFile("f2", "x");
    hostDir("d1");
    hostFile("d1/g", "y");

    EXPECT_EQ(setOf(hostList()), namesOf(fmList(rootDirID_)));
}

TEST_F(LocalVolumeFixture, ListingExcludesHiddenSidecars)
{
    hostFile("a/main", "data");
    hostFile("a/._main", "appledouble");
    hostFile("a/%main", "percent");
    hostFile("a/.rsrc/main", "rsrc");
    hostFile("a/.finf/main", "finf");

    auto a = fmGet(rootDirID_, "a");
    ASSERT_TRUE(a.has_value());

    EXPECT_EQ((std::set<std::string> { "main" }), namesOf(fmList(a->cnid)));
    EXPECT_EQ((std::set<std::string> { "main" }), namesOf(fmListFiles(a->cnid)));
    EXPECT_EQ(1, *fmChildCount(a->cnid));
}

// F3: reporting a child directory's ioDrNmFls during listing must not
// materialize that child's contents — only the two child directories
// themselves are constructed, not their files.
TEST_F(LocalVolumeFixture, ListingDoesNotMaterializeSubdirectories)
{
    hostDir("a");
    hostFile("a/x");
    hostFile("a/y");
    hostFile("a/z");
    hostDir("b");
    hostFile("b/p");
    hostFile("b/q");

    resetLocalVolumeStats();
    auto listed = fmList(rootDirID_);
    ASSERT_EQ(2u, listed.size());

    EXPECT_EQ(2u, localVolumeStats().itemsConstructed.load());
}

// Caching guard: a directory is enumerated once and reused while valid.
TEST_F(LocalVolumeFixture, ListingEnumeratesOnceWhileCached)
{
    for(int i = 0; i < 8; i++)
        hostFile("f" + std::to_string(i));

    resetLocalVolumeStats();
    (void)fmList(rootDirID_);
    auto first = localVolumeStats().directoryIterations.load();
    ASSERT_GT(first, 0u);

    (void)fmList(rootDirID_);
    EXPECT_EQ(first, localVolumeStats().directoryIterations.load());
}

// --------------------------------------------------------------------------
// Creation / deletion
// --------------------------------------------------------------------------

TEST_F(LocalVolumeFixture, CreateFileViaFileManager)
{
    long cnid = 0;
    ASSERT_EQ(noErr, fmCreateFile(rootDirID_, "newfile", &cnid));
    EXPECT_GT(cnid, 0);
    EXPECT_TRUE(fs::is_regular_file(host("newfile")));
    EXPECT_EQ(setOf({ "newfile" }), namesOf(fmList(rootDirID_)));
    EXPECT_EQ(1, *fmChildCount(rootDirID_));
}

TEST_F(LocalVolumeFixture, CreateDirViaFileManager)
{
    long dirID = 0;
    ASSERT_EQ(noErr, fmMakeDir(rootDirID_, "newdir", &dirID));
    EXPECT_TRUE(fs::is_directory(host("newdir")));

    auto e = fmGet(rootDirID_, "newdir");
    ASSERT_TRUE(e.has_value());
    EXPECT_TRUE(e->isDir);
    EXPECT_EQ(dirID, e->cnid);
}

TEST_F(LocalVolumeFixture, DuplicateCreateFails)
{
    ASSERT_EQ(noErr, fmCreateFile(rootDirID_, "dup"));
    EXPECT_EQ(dupFNErr, fmCreateFile(rootDirID_, "dup"));
    EXPECT_EQ(dupFNErr, fmMakeDir(rootDirID_, "dup"));
}

TEST_F(LocalVolumeFixture, DeleteRoundTrip)
{
    ASSERT_EQ(noErr, fmCreateFile(rootDirID_, "f"));
    ASSERT_EQ(noErr, fmDelete(rootDirID_, "f"));
    EXPECT_FALSE(fs::exists(host("f")));
    EXPECT_EQ(fnfErr, fmDelete(rootDirID_, "f"));
}

TEST_F(LocalVolumeFixture, DeleteNonEmptyDirFails)
{
    hostDir("d");
    hostFile("d/f", "x");

    auto d = fmGet(rootDirID_, "d");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(fBsyErr, fmDelete(rootDirID_, "d"));

    ASSERT_EQ(noErr, fmDelete(d->cnid, "f"));
    ASSERT_EQ(noErr, fmDelete(rootDirID_, "d"));
    EXPECT_FALSE(fs::exists(host("d")));
}

// --------------------------------------------------------------------------
// Move / rename
// --------------------------------------------------------------------------

TEST_F(LocalVolumeFixture, RenameKeepsCNID)
{
    hostFile("orig", "hello");
    auto before = fmGet(rootDirID_, "orig");
    ASSERT_TRUE(before.has_value());

    ASSERT_EQ(noErr, fmRename(rootDirID_, "orig", "renamed"));

    EXPECT_FALSE(fs::exists(host("orig")));
    EXPECT_TRUE(fs::exists(host("renamed")));

    auto after = fmGet(rootDirID_, "renamed");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(before->cnid, after->cnid);
    EXPECT_FALSE(fmGet(rootDirID_, "orig").has_value());
}

TEST_F(LocalVolumeFixture, MoveKeepsCNIDAndUpdatesCounts)
{
    hostDir("src");
    hostDir("dst");
    hostFile("src/f", "x");

    auto src = fmGet(rootDirID_, "src");
    auto dst = fmGet(rootDirID_, "dst");
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(dst.has_value());
    auto f = fmGet(src->cnid, "f");
    ASSERT_TRUE(f.has_value());

    ASSERT_EQ(noErr, fmMove(src->cnid, "f", dst->cnid));

    EXPECT_FALSE(fs::exists(host("src/f")));
    EXPECT_TRUE(fs::exists(host("dst/f")));

    auto moved = fmGet(dst->cnid, "f");
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(f->cnid, moved->cnid);
    EXPECT_EQ(0, *fmChildCount(src->cnid));
    EXPECT_EQ(1, *fmChildCount(dst->cnid));
}

TEST_F(LocalVolumeFixture, MoveAndRename)
{
    hostDir("src");
    hostDir("dst");
    hostFile("src/f");

    auto src = fmGet(rootDirID_, "src");
    auto dst = fmGet(rootDirID_, "dst");
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(dst.has_value());

    ASSERT_EQ(noErr, fmMove(src->cnid, "f", dst->cnid, "g"));

    EXPECT_FALSE(fs::exists(host("src/f")));
    EXPECT_TRUE(fs::exists(host("dst/g")));
    EXPECT_TRUE(fmGet(dst->cnid, "g").has_value());
}

TEST_F(LocalVolumeFixture, CatMoveRenamesWithinDirectory)
{
    hostFile("orig");
    auto before = fmGet(rootDirID_, "orig");
    ASSERT_TRUE(before.has_value());

    ASSERT_EQ(noErr, fmMove(rootDirID_, "orig", rootDirID_, "renamed"));

    EXPECT_FALSE(fs::exists(host("orig")));
    EXPECT_TRUE(fs::exists(host("renamed")));
    auto after = fmGet(rootDirID_, "renamed");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(before->cnid, after->cnid);
}

TEST_F(LocalVolumeFixture, RenameCollisionFails)
{
    hostFile("a");
    hostFile("b");
    EXPECT_EQ(dupFNErr, fmRename(rootDirID_, "a", "b"));
}

// --------------------------------------------------------------------------
// CNIDs and name disambiguation
// --------------------------------------------------------------------------

// A file's CNID survives a rename and a subsequent move.
TEST_F(LocalVolumeFixture, CnidStableAcrossRenameThenMove)
{
    requireInodeIdentity();
    hostDir("src");
    hostDir("dst");
    hostFile("src/f", "x");

    auto src = fmGet(rootDirID_, "src");
    auto dst = fmGet(rootDirID_, "dst");
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(dst.has_value());
    auto f = fmGet(src->cnid, "f");
    ASSERT_TRUE(f.has_value());

    ASSERT_EQ(noErr, fmRename(src->cnid, "f", "renamed"));
    auto renamed = fmGet(src->cnid, "renamed");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(f->cnid, renamed->cnid);

    ASSERT_EQ(noErr, fmMove(src->cnid, "renamed", dst->cnid));
    auto moved = fmGet(dst->cnid, "renamed");
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(f->cnid, moved->cnid);
}

// Deleting any one member of a truncation-collision set must not renumber the
// survivors' Mac names or CNIDs.
TEST_F(LocalVolumeFixture, DisambiguatedNamesStableAcrossDelete)
{
    const std::string names[3] = {
        longCollidingName("A01"), longCollidingName("A02"), longCollidingName("A03")
    };

    // One directory per deletion target, all created before enumeration (host
    // entries added under an already-enumerated parent are not observed).
    for(int i = 0; i < 3; i++)
    {
        const std::string dir = "dir" + std::to_string(i);
        hostDir(dir);
        for(const auto& n : names)
            hostFile(dir + "/" + n);
    }

    for(int i = 0; i < 3; i++)
    {
        const std::string dir = "dir" + std::to_string(i);
        SCOPED_TRACE("dir=" + dir);

        auto d = fmGet(rootDirID_, dir);
        ASSERT_TRUE(d.has_value());

        auto before = fmList(d->cnid);
        ASSERT_EQ(3u, before.size());

        // Distinct Mac names: one base and two disambiguated.
        std::set<std::string> distinct;
        int disambiguated = 0;
        for(auto& e : before)
        {
            distinct.insert(e.name);
            if(e.name.find('#') != std::string::npos)
                disambiguated++;
        }
        EXPECT_EQ(3u, distinct.size());
        EXPECT_EQ(2, disambiguated);

        const ListEntry victim = before[i];
        ASSERT_EQ(noErr, fmDelete(d->cnid, victim.name));

        auto after = fmList(d->cnid);
        ASSERT_EQ(2u, after.size());
        for(auto& e : after)
        {
            auto it = std::find_if(before.begin(), before.end(),
                [&](const ListEntry& b) { return b.name == e.name; });
            ASSERT_TRUE(it != before.end()) << "Mac name changed: " << e.name;
            EXPECT_EQ(it->cnid, e.cnid);
        }
    }
}

// Moving a member of a truncation-collision set without specifying a new name
// must preserve the POSIX name, the (disambiguated) Mac name, and the CNID.
TEST_F(LocalVolumeFixture, MoveKeepsDisambiguatedNameAndPosixName)
{
    const std::string p = longCollidingName("B01");
    const std::string q = longCollidingName("B02");

    hostDir("a");
    hostDir("b");
    hostFile("a/" + p);
    hostFile("a/" + q);

    auto a = fmGet(rootDirID_, "a");
    auto b = fmGet(rootDirID_, "b");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    auto before = fmList(a->cnid);
    ASSERT_EQ(2u, before.size());

    const ListEntry* disambiguated = nullptr;
    for(auto& e : before)
        if(e.name.find('#') != std::string::npos)
            disambiguated = &e;
    ASSERT_TRUE(disambiguated != nullptr);

    // PBCatMove with ioNewName == nil: move without renaming.
    ASSERT_EQ(noErr, fmMove(a->cnid, disambiguated->name, b->cnid));

    // The moved POSIX name is one of the originals, unchanged; 'a' keeps the
    // other.
    auto aFiles = hostList("a");
    auto bFiles = hostList("b");
    ASSERT_EQ(1u, aFiles.size());
    ASSERT_EQ(1u, bFiles.size());
    EXPECT_TRUE(aFiles[0] == p || aFiles[0] == q);
    EXPECT_TRUE(bFiles[0] == p || bFiles[0] == q);
    EXPECT_NE(aFiles[0], bFiles[0]);

    // Mac name (with disambiguator) and CNID preserved.
    auto moved = fmGet(b->cnid, disambiguated->name);
    ASSERT_TRUE(moved.has_value()) << disambiguated->name;
    EXPECT_EQ(disambiguated->name, moved->name);
    EXPECT_EQ(disambiguated->cnid, moved->cnid);
}

// Ambiguity that arises only from case-insensitive comparison. This is the one
// case that needs a case-sensitive host filesystem (Foo and foO cannot coexist
// on macOS's default filesystem), so it is skipped there.
TEST_F(LocalVolumeFixture, CaseInsensitiveAmbiguity)
{
    hostFile("caseProbe.tmp");
    if(fs::exists(host("CASEPROBE.TMP")))
        GTEST_SKIP() << "host filesystem is case-insensitive";
    fs::remove(host("caseProbe.tmp"));

    hostDir("a");
    hostFile("a/Foo");
    hostFile("a/foO"); // same uppercased Mac name as "Foo"

    auto a = fmGet(rootDirID_, "a");
    ASSERT_TRUE(a.has_value());

    auto entries = fmList(a->cnid);
    ASSERT_EQ(2u, entries.size());
    EXPECT_NE(entries[0].name, entries[1].name);
    EXPECT_TRUE(entries[0].name.find('#') != std::string::npos
        || entries[1].name.find('#') != std::string::npos);
}

// --------------------------------------------------------------------------
// Path resolution (F1)
// --------------------------------------------------------------------------

// Path resolution (F1): resolving the *path* walks every component by stat +
// (dev, ino) and never enumerates an ancestor. Producing the FSSpec *name* is
// NameResolve, which decides the target's macNamePrefix group and so lists the
// target's immediate parent once. The bound asserted is therefore ≤ 1 scan.
TEST_F(LocalVolumeFixture, ResolvePathListsAtMostTheTargetParent)
{
    hostDir("a");
    for(int i = 0; i < 50; i++)
        hostFile("a/f" + std::to_string(i), "x");
    hostDir("sibling");
    for(int i = 0; i < 50; i++)
        hostFile("sibling/g" + std::to_string(i), "x");

    resetLocalVolumeStats();
    auto spec = nativePathToFSSpec(host("a/f7"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(1u, localVolumeStats().directoryIterations.load());

    // The name is correct, and the CNID of the parent matches the FileManager's.
    EXPECT_EQ("f7", pstring(spec->name));
    auto a = fmGet(rootDirID_, "a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->cnid, spec->parID);
    EXPECT_TRUE(fmGet(spec->parID, "f7").has_value());
}

// --------------------------------------------------------------------------
// Interleaved host / FileManager sequences
// --------------------------------------------------------------------------

TEST_F(LocalVolumeFixture, InterleavedHostAndFileManagerSequence)
{
    hostDir("box");
    hostFile("box/one", "1");
    hostFile("box/two", "22");

    auto box = fmGet(rootDirID_, "box");
    ASSERT_TRUE(box.has_value());
    const long boxID = box->cnid;
    auto one = fmGet(boxID, "one");
    ASSERT_TRUE(one.has_value());

    // FM create -> host sees it, and the published listing picks it up
    ASSERT_EQ(noErr, fmCreateFile(boxID, "three"));
    EXPECT_TRUE(fs::exists(host("box/three")));
    EXPECT_EQ(setOf({ "one", "three", "two" }), setOf(hostList("box")));
    EXPECT_EQ((std::set<std::string> { "one", "three", "two" }), namesOf(fmList(boxID)));
    EXPECT_EQ(3, *fmChildCount(boxID));

    // FM rename -> host reflects it, CNID stable
    ASSERT_EQ(noErr, fmRename(boxID, "one", "uno"));
    EXPECT_FALSE(fs::exists(host("box/one")));
    EXPECT_TRUE(fs::exists(host("box/uno")));
    EXPECT_EQ((std::set<std::string> { "three", "two", "uno" }), namesOf(fmList(boxID)));
    auto uno = fmGet(boxID, "uno");
    ASSERT_TRUE(uno.has_value());
    EXPECT_EQ(one->cnid, uno->cnid);

    // FM delete -> host reflects it
    ASSERT_EQ(noErr, fmDelete(boxID, "two"));
    EXPECT_FALSE(fs::exists(host("box/two")));
    EXPECT_EQ((std::set<std::string> { "three", "uno" }), namesOf(fmList(boxID)));
    EXPECT_EQ(2, *fmChildCount(boxID));
}

TEST_F(LocalVolumeFixture, HostMoveVisibleOnFreshEnumeration)
{
    hostDir("src");
    hostDir("dst");
    hostFile("src/f", "x");

    // Move on the host before either directory has been enumerated.
    fs::rename(host("src/f"), host("dst/f"));

    auto src = fmGet(rootDirID_, "src");
    auto dst = fmGet(rootDirID_, "dst");
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(dst.has_value());

    EXPECT_FALSE(fmGet(src->cnid, "f").has_value());
    EXPECT_TRUE(fmGet(dst->cnid, "f").has_value());
}

// Host-side changes become visible without going through the FileManager, via
// validation-based caching, rather than only after a timeout.
TEST_F(LocalVolumeFixture, HostChangeVisibleWithoutInvalidation)
{
    hostDir("a");
    auto a = fmGet(rootDirID_, "a");
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(fmList(a->cnid).empty()); // 'a' is now enumerated, and empty

    hostFile("a/late", "x");
    bumpMtime("a"); // make the change deterministic on coarse-timestamp hosts

    EXPECT_EQ((std::set<std::string> { "late" }), namesOf(fmList(a->cnid)));
}

// A scan that fails must not be committed. Reading the failure as an empty
// directory would delete every child's record and renumber the directory, and
// Count would cache a wrong 0 -- so both funnels report ioErr and leave the
// directory to be retried instead.
TEST_F(LocalVolumeFixture, UnreadableDirectoryReportsIoErr)
{
    hostDir("d");
    hostFile("d/a", "a");
    hostFile("d/b", "b");

    long eID = 0;
    ASSERT_EQ(noErr, fmMakeDir(rootDirID_, "e", &eID));
    hostFile("e/one", "1");   // 'e' is never listed, only counted

    auto d = fmGet(rootDirID_, "d");
    ASSERT_TRUE(d.has_value());
    auto a = fmGet(d->cnid, "a");
    auto b = fmGet(d->cnid, "b");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    // Drop read, keep write and execute: reading a directory's contents fails,
    // while stat'ing the directory itself still works.
    const auto unreadable = fs::perms::owner_write | fs::perms::owner_exe;
    fs::permissions(host("d"), unreadable);
    fs::permissions(host("e"), unreadable);

    bool enforced = false;
    try
    {
        for(auto& entry : fs::directory_iterator(host("d")))
            (void)entry;
    }
    catch(const boost::filesystem::filesystem_error&)
    {
        enforced = true;
    }

    if(!enforced)   // e.g. running as root, where permissions are not enforced
    {
        fs::permissions(host("d"), fs::perms::owner_all);
        fs::permissions(host("e"), fs::perms::owner_all);
        GTEST_SKIP() << "host filesystem does not enforce directory permissions";
    }

    bumpMtime("d");   // stale listing, so the next access re-scans

    EXPECT_EQ(ioErr, fmDirInfoResult(d->cnid, 1));    // the listing funnel
    EXPECT_EQ(ioErr, fmDirInfoResult(eID, -1));       // the count funnel

    fs::permissions(host("d"), fs::perms::owner_all);
    fs::permissions(host("e"), fs::perms::owner_all);

    // Neither failed scan left a trace: the count is not the 0 the failed scan
    // saw, the children are still listed, and their CNIDs are unchanged.
    EXPECT_EQ(1, *fmChildCount(eID));
    EXPECT_EQ((std::set<std::string> { "a", "b" }), namesOf(fmList(d->cnid)));
    auto aAfter = fmGet(d->cnid, "a");
    auto bAfter = fmGet(d->cnid, "b");
    ASSERT_TRUE(aAfter.has_value());
    ASSERT_TRUE(bAfter.has_value());
    EXPECT_EQ(a->cnid, aAfter->cnid);
    EXPECT_EQ(b->cnid, bAfter->cnid);
}

// Phase 5.2: eviction under the item budget is transparent - re-accessing an
// evicted directory rebuilds the same names and CNIDs from the mapper.
TEST_F(LocalVolumeFixture, EvictionPreservesIdentity)
{
    hostDir("a");
    for(int i = 0; i < 20; i++)
        hostFile("a/f" + std::to_string(i));

    auto a = fmGet(rootDirID_, "a");
    ASSERT_TRUE(a.has_value());

    auto first = fmList(a->cnid);
    auto known = fmGet(a->cnid, "f3");
    ASSERT_TRUE(known.has_value());

    size_t saved = localVolumeCacheBudget();
    localVolumeCacheBudget() = 3; // force eviction of 'a' on the next enumeration

    hostDir("b");
    for(int i = 0; i < 20; i++)
        hostFile("b/g" + std::to_string(i));
    auto b = fmGet(rootDirID_, "b");
    ASSERT_TRUE(b.has_value());
    (void)fmList(b->cnid); // evicts the least-recently-used directory ('a')

    localVolumeCacheBudget() = saved;

    // 'a' is rebuilt on demand, but its names and CNIDs are unchanged.
    EXPECT_EQ(namesOf(first), namesOf(fmList(a->cnid)));
    auto rebuilt = fmGet(a->cnid, "f3");
    ASSERT_TRUE(rebuilt.has_value());
    EXPECT_EQ(known->cnid, rebuilt->cnid);
}

// Phase 5.3: a counted subdirectory is not re-scanned while its stamp is valid.
TEST_F(LocalVolumeFixture, CountScansOnceWhileValid)
{
    hostDir("a");
    hostFile("a/x");
    hostFile("a/y");

    resetLocalVolumeStats();
    (void)fmList(rootDirID_); // counts 'a'
    auto first = localVolumeStats().countScans.load();
    ASSERT_GT(first, 0u);

    (void)fmList(rootDirID_); // 'a' is still valid, so no rescan
    EXPECT_EQ(first, localVolumeStats().countScans.load());
}

// *Rep staleness*: revalidation follows the candidates the gauntlet probed. An
// in-place content change is not re-probed -- the content checks are trusted -- so
// a plain ".bin" rewritten as MacBinary keeps the plain rep.
TEST_F(LocalVolumeFixture, RepStalenessTrustsContent)
{
    hostFile("foo.bin", "not a macbinary header at all");

    auto before = fmGetType(rootDirID_, "foo.bin");
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(0x54455854u, *before); // the plain default ("TEXT")

    hostBytes("foo.bin", makeMacBinary("foo", { 1, 2, 3 }, { 4, 5 }));
    bumpMtime("foo.bin");

    resetLocalVolumeStats();
    auto after = fmGetType(rootDirID_, "foo.bin");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(0x54455854u, *after);                         // still plain
    EXPECT_EQ(0u, localVolumeStats().factoryProbes.load()); // not re-probed
}

// *Rep staleness*: a candidate appearing or disappearing does re-run the gauntlet.
// An AppleDouble upgrade creates its sidecar; taking it away falls back to plain.
TEST_F(LocalVolumeFixture, RepStalenessFollowsCandidates)
{
    ASSERT_EQ(noErr, fmCreateFile(rootDirID_, "doc", nullptr));
    ASSERT_EQ(noErr, fmSetType(rootDirID_, "doc", 0x41424344u)); // upgrades to AppleDouble
    ASSERT_TRUE(fmGetType(rootDirID_, "doc").has_value());
    ASSERT_EQ(0x41424344u, *fmGetType(rootDirID_, "doc"));

    fs::remove(host("%doc")); // the AppleDouble sidecar
    bumpMtime("");

    resetLocalVolumeStats();
    auto after = fmGetType(rootDirID_, "doc");
    ASSERT_TRUE(after.has_value());
    EXPECT_GT(localVolumeStats().factoryProbes.load(), 0u);
    EXPECT_EQ(0x54455854u, *after); // plain again
}

// Phase 8: a kind change at a name is a different item. A file replaced by a
// directory at the same name is rebuilt as a directory with a new CNID, even
// when the host reused the freed inode.
TEST_F(LocalVolumeFixture, KindChangeRebuildsItem)
{
    hostFile("x", "data");
    auto before = fmGet(rootDirID_, "x");
    ASSERT_TRUE(before.has_value());
    EXPECT_FALSE(before->isDir);

    fs::remove(host("x"));
    hostDir("x");
    hostFile("x/inner", "y");
    bumpMtime("");

    auto after = fmGet(rootDirID_, "x");
    ASSERT_TRUE(after.has_value());
    EXPECT_TRUE(after->isDir);
    EXPECT_NE(before->cnid, after->cnid);
    EXPECT_EQ((std::set<std::string> { "inner" }), namesOf(fmList(after->cnid)));
}

// Phase 10: only same-directory renames are recovered. A host move across
// directories is delete + create, a new CNID.
TEST_F(LocalVolumeFixture, CrossDirectoryHostMoveIsDeleteCreate)
{
    hostDir("src");
    hostDir("dst");
    hostFile("src/f", "data");

    auto src = fmGet(rootDirID_, "src");
    auto dst = fmGet(rootDirID_, "dst");
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(dst.has_value());

    auto before = fmGet(src->cnid, "f");
    ASSERT_TRUE(before.has_value());

    fs::rename(host("src/f"), host("dst/f"));
    bumpMtime("src");
    bumpMtime("dst");

    EXPECT_FALSE(fmGet(src->cnid, "f").has_value());
    auto after = fmGet(dst->cnid, "f");
    ASSERT_TRUE(after.has_value());
    EXPECT_NE(before->cnid, after->cnid);
}

// Phase 8: a file replaced by a new file at the same name must take the name
// back, not a disambiguated one. (The old entry's name is freed before the new
// entry is named, whether or not the host reused the inode.)
TEST_F(LocalVolumeFixture, ReplacedFileReclaimsItsName)
{
    requireInodeIdentity();
    hostFile("doc", "first");
    auto before = fmGet(rootDirID_, "doc");
    ASSERT_TRUE(before.has_value());

    fs::remove(host("doc"));
    hostFile("doc", "second, different length");
    bumpMtime("");

    auto after = fmGet(rootDirID_, "doc");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ("doc", after->name);
    EXPECT_EQ((std::set<std::string> { "doc" }), namesOf(fmList(rootDirID_)));
    EXPECT_EQ(1, *fmChildCount(rootDirID_));
}

// Phase 9: naming is scoped to one macNamePrefix group. Deciding one entry's
// name must not probe (or name) another group's entries.
TEST_F(LocalVolumeFixture, NameResolveIsGroupScoped)
{
    hostBytes("alpha.bin", makeMacBinary("alpha", { 1, 2, 3 }, { 4, 5 }));
    hostBytes("beta.bin", makeMacBinary("beta", { 1, 2, 3 }, { 4, 5 }));

    resetLocalVolumeStats();
    auto alpha = fmGet(rootDirID_, "alpha");
    ASSERT_TRUE(alpha.has_value());
    EXPECT_EQ(1u, localVolumeStats().nameProbes.load()); // only alpha.bin

    auto beta = fmGet(rootDirID_, "beta");
    ASSERT_TRUE(beta.has_value());
    EXPECT_EQ(2u, localVolumeStats().nameProbes.load()); // now beta.bin too
}

// Phase 8: an encoding upgrade (Plain -> AppleDouble, triggered by setting a
// non-TEXT type) only adds its storage beside the existing data fork, so it must
// keep both the CNID and the Mac name.
TEST_F(LocalVolumeFixture, UpgradePreservesCnidAndName)
{
    ASSERT_EQ(noErr, fmCreateFile(rootDirID_, "doc", nullptr));
    auto before = fmGet(rootDirID_, "doc");
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(0x54455854u, *fmGetType(rootDirID_, "doc")); // the plain default

    const uint32_t newType = 0x41424344u; // 'ABCD'
    ASSERT_EQ(noErr, fmSetType(rootDirID_, "doc", newType));

    auto after = fmGet(rootDirID_, "doc");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(before->cnid, after->cnid);
    EXPECT_EQ("doc", after->name);
    ASSERT_TRUE(fmGetType(rootDirID_, "doc").has_value());
    EXPECT_EQ(newType, *fmGetType(rootDirID_, "doc"));
    EXPECT_EQ((std::set<std::string> { "doc" }), namesOf(fmList(rootDirID_)));
    EXPECT_EQ(1, *fmChildCount(rootDirID_));
}

// --------------------------------------------------------------------------
// Forward-looking (target behaviour of the refactor). Run with
//   --gtest_also_run_disabled_tests
// once the relevant phase lands.
// --------------------------------------------------------------------------

// Phase 4: a MacBinary "foo.bin" is presented to the guest as "foo".
TEST_F(LocalVolumeFixture, MacBinaryStripsBinExtension)
{
    hostBytes("foo.bin", makeMacBinary("foo", { 1, 2, 3 }, { 4, 5 }));
    hostFile("plain.txt", "hi");

    EXPECT_EQ((std::set<std::string> { "foo", "plain.txt" }), namesOf(fmList(rootDirID_)));
}

// Phase 4: a MacBinary-looking "foo.bin" with an AppleDouble sidecar is
// AppleDouble, so it keeps its ".bin" (sidecars win over MacBinary).
TEST_F(LocalVolumeFixture, SidecarNameWinsOverMacBinary)
{
    hostBytes("foo.bin", makeMacBinary("foo", { 1, 2, 3 }, { 4, 5 }));
    hostFile("%foo.bin", "appledouble");

    EXPECT_EQ((std::set<std::string> { "foo.bin" }), namesOf(fmList(rootDirID_)));
}

// --------------------------------------------------------------------------
// Rename recovery and hard links (Phase 10)
// --------------------------------------------------------------------------

// A host-side rename within one directory keeps the CNID, adopts the new host
// name, and re-derives the Mac name.
TEST_F(LocalVolumeFixture, SameDirectoryHostRenameKeepsCnid)
{
    requireInodeIdentity();
    hostFile("old", "data");
    auto before = fmGet(rootDirID_, "old");
    ASSERT_TRUE(before.has_value());

    fs::rename(host("old"), host("new"));
    bumpMtime("");

    EXPECT_FALSE(fmGet(rootDirID_, "old").has_value());
    EXPECT_EQ((std::set<std::string> { "new" }), namesOf(fmList(rootDirID_)));
    auto after = fmGet(rootDirID_, "new");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(before->cnid, after->cnid);
}

// Phase 7: the listing order is `macNamePrefix`-major (case-folded), host path as
// the tie-break — not raw host-path order. "a" and "B" are in different groups,
// so the folded order (a, B) differs from the byte order (B, a).
TEST_F(LocalVolumeFixture, ListingOrderIsMacNamePrefixMajor)
{
    hostFile("B");
    hostFile("a");

    auto list = fmList(rootDirID_);
    ASSERT_EQ(2u, list.size());
    EXPECT_EQ("a", list[0].name);
    EXPECT_EQ("B", list[1].name);
}

// A hard-linked inode holds one binding per name, so the names get distinct
// CNIDs; deleting one leaves the other's CNID alone.
TEST_F(LocalVolumeFixture, HardLinkedNamesAreDistinct)
{
    requireInodeIdentity();
    hostFile("orig", "data");
    boost::system::error_code ec;
    fs::create_hard_link(host("orig"), host("link"), ec);
    if(ec)
        GTEST_SKIP() << "hard links unsupported: " << ec.message();

    auto a = fmGet(rootDirID_, "orig");
    auto b = fmGet(rootDirID_, "link");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_NE(a->cnid, b->cnid);

    ASSERT_EQ(noErr, fmDelete(rootDirID_, "link"));
    EXPECT_FALSE(fmGet(rootDirID_, "link").has_value());
    auto a2 = fmGet(rootDirID_, "orig");
    ASSERT_TRUE(a2.has_value());
    EXPECT_EQ(a->cnid, a2->cnid);
}

// Adding a hard link to a listed directory leaves the original name's CNID
// alone; the new name is a new entry.
TEST_F(LocalVolumeFixture, AddedHardLinkGetsNewCnid)
{
    requireInodeIdentity();
    hostFile("orig", "data");
    auto orig = fmGet(rootDirID_, "orig");
    ASSERT_TRUE(orig.has_value());

    boost::system::error_code ec;
    fs::create_hard_link(host("orig"), host("link"), ec);
    if(ec)
        GTEST_SKIP() << "hard links unsupported: " << ec.message();
    bumpMtime("");

    EXPECT_EQ((std::set<std::string> { "link", "orig" }), namesOf(fmList(rootDirID_)));
    auto orig2 = fmGet(rootDirID_, "orig");
    ASSERT_TRUE(orig2.has_value());
    EXPECT_EQ(orig->cnid, orig2->cnid);

    auto link = fmGet(rootDirID_, "link");
    ASSERT_TRUE(link.has_value());
    EXPECT_NE(orig->cnid, link->cnid);
}

// Renaming one of two hard-linked names keeps that name's CNID; the sibling is
// untouched.
TEST_F(LocalVolumeFixture, RenamingOneHardLinkKeepsItsCnid)
{
    requireInodeIdentity();
    hostFile("a", "data");
    boost::system::error_code ec;
    fs::create_hard_link(host("a"), host("b"), ec);
    if(ec)
        GTEST_SKIP() << "hard links unsupported: " << ec.message();

    auto a = fmGet(rootDirID_, "a");
    auto b = fmGet(rootDirID_, "b");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_NE(a->cnid, b->cnid);

    fs::rename(host("b"), host("c"));
    bumpMtime("");

    EXPECT_FALSE(fmGet(rootDirID_, "b").has_value());
    auto c = fmGet(rootDirID_, "c");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(b->cnid, c->cnid);

    auto a2 = fmGet(rootDirID_, "a");
    ASSERT_TRUE(a2.has_value());
    EXPECT_EQ(a->cnid, a2->cnid);
}

// An open file survives a same-directory host rename: the Item is updated in
// place, and the rename is recovered on a fresh lookup.
TEST_F(LocalVolumeFixture, HostRenameRecoveredWhileFileIsOpen)
{
    requireInodeIdentity();
    hostFile("old", "hello, world");
    auto before = fmGet(rootDirID_, "old");
    ASSERT_TRUE(before.has_value());

    short refNum = fmOpen(rootDirID_, "old");
    ASSERT_GE(refNum, 0);
    EXPECT_EQ("hello, world", fmRead(refNum, 32));

    fs::rename(host("old"), host("new"));
    bumpMtime("");

    // The guest's handle still reads the file (its fd follows the inode)...
    fmSetFPos(refNum, 0);
    EXPECT_EQ("hello, world", fmRead(refNum, 32));

    // ...and the rename is recovered even while the file is open.
    auto after = fmGet(rootDirID_, "new");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(before->cnid, after->cnid);
    EXPECT_FALSE(fmGet(rootDirID_, "old").has_value());

    fmClose(refNum);
}

// --------------------------------------------------------------------------
// Defect-pinning tests (written first)
//
// These pin the straightforward defects from the 2026-10-08 plan review, plus
// the previously untested access-by-CNID binding-retry path. Tests that fail
// today are DISABLED_ so the default suite stays green until the defect is
// fixed; run them with, for example,
//   build/tests --gtest_also_run_disabled_tests \
//               --gtest_filter='*MoveUpdatesParID*'
// --------------------------------------------------------------------------

// A FileManager move must update the live Item's parent (ioFlParID and the
// FSSpec's parID), not just its host path and mapper record.
TEST_F(LocalVolumeFixture, DISABLED_MoveUpdatesParID)
{
    hostDir("src");
    hostDir("dst");
    hostFile("src/f", "x");

    auto src = fmGet(rootDirID_, "src");
    auto dst = fmGet(rootDirID_, "dst");
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(dst.has_value());
    auto f = fmGet(src->cnid, "f");
    ASSERT_TRUE(f.has_value());

    ASSERT_EQ(noErr, fmMove(src->cnid, "f", dst->cnid));

    auto moved = fmGet(dst->cnid, "f");
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(f->cnid, moved->cnid);

    auto parID = fmParID(dst->cnid, "f");
    ASSERT_TRUE(parID.has_value());
    EXPECT_EQ(dst->cnid, *parID);

    auto spec = nativePathToFSSpec(host("dst/f"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(dst->cnid, spec->parID);
}

// PBCatMove must keep an open FCB's cached name and parent current, as
// PBHRename does.
TEST_F(LocalVolumeFixture, DISABLED_CatMoveUpdatesOpenFCB)
{
    hostDir("src");
    hostDir("dst");
    hostFile("src/f", "hello");

    auto src = fmGet(rootDirID_, "src");
    auto dst = fmGet(rootDirID_, "dst");
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(dst.has_value());
    auto f = fmGet(src->cnid, "f");
    ASSERT_TRUE(f.has_value());

    short refNum = fmOpen(src->cnid, "f");
    ASSERT_GE(refNum, 0);

    ASSERT_EQ(noErr, fmMove(src->cnid, "f", dst->cnid, "g"));

    auto fcb = fmFCBInfo(refNum);
    fmClose(refNum);
    ASSERT_TRUE(fcb.has_value());
    EXPECT_EQ(f->cnid, fcb->fileID);
    EXPECT_EQ("g", fcb->name);
    EXPECT_EQ(dst->cnid, fcb->parID);
}

// The guest sees a directory containing only hidden sidecars as empty, so it
// must be deletable; the invisible entries must not produce fBsyErr.
TEST_F(LocalVolumeFixture, DISABLED_DeleteDirectoryWithOnlyHiddenSidecars)
{
    hostDir("d");
    hostFile("d/._foo", "sidecar");

    auto d = fmGet(rootDirID_, "d");
    ASSERT_TRUE(d.has_value());
    auto count = fmChildCount(d->cnid);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(0, *count);
    EXPECT_TRUE(fmList(d->cnid).empty());

    EXPECT_EQ(noErr, fmDelete(rootDirID_, "d"));
    EXPECT_FALSE(fs::exists(host("d")));
}

// A directory's own AppleDouble companion must be removed with it.
TEST_F(LocalVolumeFixture, DISABLED_DirectoryDeleteRemovesItsOwnSidecar)
{
    hostDir("d");
    hostFile("._d", "sidecar");

    ASSERT_EQ(noErr, fmDelete(rootDirID_, "d"));
    EXPECT_FALSE(fs::exists(host("d")));
    EXPECT_FALSE(fs::exists(host("._d")));
}

// A directory's own AppleDouble companion must follow a rename.
TEST_F(LocalVolumeFixture, DISABLED_DirectoryRenameMovesItsOwnSidecar)
{
    hostDir("d");
    hostFile("._d", "sidecar");

    ASSERT_EQ(noErr, fmRename(rootDirID_, "d", "e"));
    EXPECT_TRUE(fs::exists(host("e")));
    EXPECT_FALSE(fs::exists(host("._d")));
}

// Deleting a file must remove every AppleDouble companion, not just the
// scheme that won classification.
TEST_F(LocalVolumeFixture, DISABLED_DeleteRemovesBothAppleDoubleSchemes)
{
    hostFile("foo", "data");
    hostFile("%foo", "percent sidecar");
    hostFile("._foo", "dot-underscore sidecar");

    ASSERT_EQ(noErr, fmDelete(rootDirID_, "foo"));
    EXPECT_FALSE(fs::exists(host("foo")));
    EXPECT_FALSE(fs::exists(host("%foo")));
    EXPECT_FALSE(fs::exists(host("._foo")));
}

// A realized item whose name cannot be decided must not yield an FSSpec with
// a zero-length name. A hidden sidecar is not in the parent's listing, so it
// has no name to resolve.
TEST_F(LocalVolumeFixture, DISABLED_HiddenSidecarPathHasNoEmptyFSSpecName)
{
    hostFile("._foo", "sidecar");

    auto spec = nativePathToFSSpec(host("._foo"));
    if(spec)
        EXPECT_GT(spec->name[0], 0) << "an FSSpec must not carry an undecided name";
}

// If the parent cannot be listed, NameResolve cannot decide the child's name;
// that must surface as "no FSSpec", not an FSSpec with a zero-length name.
TEST_F(LocalVolumeFixture, DISABLED_UnreadableParentHasNoEmptyFSSpecName)
{
    hostDir("d");
    hostFile("d/f", "x");

    // Drop read, keep write and execute: stat'ing d/f still works, but listing
    // d fails.
    const auto unreadable = fs::perms::owner_write | fs::perms::owner_exe;
    fs::permissions(host("d"), unreadable);

    bool enforced = false;
    try
    {
        for(auto& entry : fs::directory_iterator(host("d")))
            (void)entry;
    }
    catch(const boost::filesystem::filesystem_error&)
    {
        enforced = true;
    }

    if(!enforced)
    {
        fs::permissions(host("d"), fs::perms::owner_all);
        GTEST_SKIP() << "host filesystem does not enforce directory permissions";
    }

    auto spec = nativePathToFSSpec(host("d/f"));
    fs::permissions(host("d"), fs::perms::owner_all);

    if(spec)
        EXPECT_GT(spec->name[0], 0) << "an FSSpec must not carry an undecided name";
}

// A per-entry stat failure must not delete the entry's CNID: the file is still
// there, and a transient stat error must not be read as "gone".
TEST_F(LocalVolumeFixture, DISABLED_StatFailureDoesNotRenumberChild)
{
    hostDir("d");
    hostFile("d/f", "x");

    auto d = fmGet(rootDirID_, "d");
    ASSERT_TRUE(d.has_value());
    auto before = fmGet(d->cnid, "f");
    ASSERT_TRUE(before.has_value());

    // Read but no execute: readdir succeeds, stat("d/f") fails with EACCES.
    fs::permissions(host("d"), fs::perms::owner_read);

    bool enforced = false;
    boost::system::error_code ec;
    if(!fs::is_regular_file(host("d/f"), ec) && ec)
        enforced = true;

    bool canList = true;
    try
    {
        for(auto& entry : fs::directory_iterator(host("d")))
            (void)entry;
    }
    catch(const boost::filesystem::filesystem_error&)
    {
        canList = false;
    }

    if(!enforced || !canList)
    {
        fs::permissions(host("d"), fs::perms::owner_all);
        GTEST_SKIP() << "host filesystem does not enforce directory search permissions";
    }

    // A readdir that succeeds while a child cannot be stat'ed must not be
    // committed as a listing that omits the child.
    bumpMtime("d");
    auto listed = fmList(d->cnid);
    fs::permissions(host("d"), fs::perms::owner_all);

    if(!listed.empty())
        GTEST_SKIP() << "stat of a child succeeded without directory search permission";

    bumpMtime("d");
    auto after = fmGet(d->cnid, "f");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(before->cnid, after->cnid);
}

// A directory renamed on the host is recovered by an access by CNID alone: the
// binding check flushes the parent, the diff recovers the rename, and the
// access retries. (This path had no direct test; it passes today.)
TEST_F(LocalVolumeFixture, DirectoryRenameRecoveredByCnidAccess)
{
    requireInodeIdentity();
    hostDir("d");
    hostFile("d/f/inner", "x");

    auto d = fmGet(rootDirID_, "d");
    ASSERT_TRUE(d.has_value());
    auto f = fmGet(d->cnid, "f");
    ASSERT_TRUE(f.has_value());
    ASSERT_TRUE(f->isDir);

    fs::rename(host("d/f"), host("d/g"));
    bumpMtime("d");

    // No name lookup first: go straight to the CNID.
    auto count = fmChildCount(f->cnid);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(1, *count);

    auto g = fmGet(d->cnid, "g");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(f->cnid, g->cnid);
    EXPECT_FALSE(fmGet(d->cnid, "f").has_value());
}

// --------------------------------------------------------------------------
// Benchmark (not CTest-gated; run explicitly)
//   build/tests --gtest_also_run_disabled_tests \
//               --gtest_filter='*BenchmarkLargeDirectory*'
// --------------------------------------------------------------------------

TEST_F(LocalVolumeFixture, DISABLED_BenchmarkLargeDirectory)
{
    const char* nEnv = std::getenv("EXECUTOR_LV_BENCH_N");
    const int N = nEnv ? std::atoi(nEnv) : 1000;
    for(int i = 0; i < N; i++)
        hostFile("f" + std::to_string(i), "");

    resetLocalVolumeStats();
    auto list = fmList(rootDirID_);

    std::cout << "LocalVolume large-directory benchmark: entries=" << N
              << " listed=" << list.size()
              << " directoryIterations=" << localVolumeStats().directoryIterations.load()
              << " entriesSeen=" << localVolumeStats().entriesSeen.load()
              << " itemsConstructed=" << localVolumeStats().itemsConstructed.load()
              << " factoryProbes=" << localVolumeStats().factoryProbes.load()
              << std::endl;

    EXPECT_EQ((size_t)N, list.size());
}
