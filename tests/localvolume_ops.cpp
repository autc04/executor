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

// F2/F3 target: enumerating a directory must not descend into its
// subdirectories. Currently it does: getInfoCommon caches a child directory to
// read ioDrNmFls, so listing a directory with two subdirectories performs three
// directory scans. Disabled until the Count stage lands.
TEST_F(LocalVolumeFixture, DISABLED_ListingDoesNotEnumerateSubdirectories)
{
    hostDir("a");
    hostFile("a/x");
    hostDir("b");
    hostFile("b/y");

    resetLocalVolumeStats();
    (void)fmList(rootDirID_);

    EXPECT_EQ(1u, localVolumeStats().directoryIterations.load());
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

// Phase 4/6: host-side changes should become visible without going through
// the FileManager (validation-based caching), rather than only after the 1s
// timeout. Disabled until that lands.
TEST_F(LocalVolumeFixture, DISABLED_HostChangeVisibleWithoutInvalidation)
{
    hostDir("a");
    auto a = fmGet(rootDirID_, "a");
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(fmList(a->cnid).empty()); // 'a' is now cached, and empty

    hostFile("a/late", "x");

    EXPECT_EQ((std::set<std::string> { "late" }), namesOf(fmList(a->cnid)));
}

// --------------------------------------------------------------------------
// Forward-looking (target behaviour of the refactor). Run with
//   --gtest_also_run_disabled_tests
// once the relevant phase lands.
// --------------------------------------------------------------------------

// Phase 4: a MacBinary "foo.bin" should be presented to the guest as "foo".
TEST_F(LocalVolumeFixture, DISABLED_MacBinaryStripsBinExtension)
{
    hostBytes("foo.bin", makeMacBinary("foo", { 1, 2, 3 }, { 4, 5 }));
    hostFile("plain.txt", "hi");

    EXPECT_EQ((std::set<std::string> { "foo", "plain.txt" }), namesOf(fmList(rootDirID_)));
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
