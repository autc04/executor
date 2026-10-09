#pragma once

// Native-only helpers for tests that drive the FileManager against the
// mounted-'/' LocalVolume while interleaving host filesystem operations.
//
// Not built for Retro68: it uses Executor internals and std::filesystem.
// Include this only from NATIVE_TEST_SOURCES.
//
// Isolation: each test asks the FileManager to create a uniquely-named
// directory under the test environment's temporary working directory (see
// ExecutorTestTempDir, defined in main_executor.cpp), then builds a host tree
// inside it. All FileManager helpers take an explicit (vRefNum, dirID) and
// never touch the process-global default directory.
//
// Note on staleness: the current LocalVolume only invalidates a directory's
// cache when a mutation goes *through* the FileManager (create/delete/rename/
// move all flush the parent). Host-side changes made after a directory has been
// enumerated are therefore not observed until the 1s cache timeout. Tests below
// either populate a directory on the host before its first enumeration, or
// interleave a FileManager mutation before re-listing. The desired
// validation-based behaviour is captured by a DISABLED_ test.

#include "compat.h"

#include <FileMgr.h>
#include <rsys/filesystem.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Host path of the test environment's temporary working directory (set by
// ExecutorTestEnvironment in main_executor.cpp).
extern fs::path ExecutorTestTempDir;

namespace Executor::Test
{

struct ListEntry
{
    std::string name;
    long cnid = 0;
    bool isDir = false;
};

inline constexpr int kIsDirMask = 0x10; // kioFlAttribDirMask

inline std::string pstring(const unsigned char* p)
{
    return std::string(reinterpret_cast<const char*>(p + 1), p[0]);
}

inline std::array<unsigned char, 256> str255(std::string_view s)
{
    std::array<unsigned char, 256> out {};
    size_t n = std::min<size_t>(s.size(), 255);
    out[0] = static_cast<unsigned char>(n);
    std::memcpy(out.data() + 1, s.data(), n);
    return out;
}

inline GUEST<LONGINT> toIoMisc(void* p)
{
    return guest_cast<LONGINT>(p);
}

class LocalVolumeFixture : public ::testing::Test
{
protected:
    short vRefNum_ = 0;
    long rootDirID_ = 0;
    std::string rootName_;
    fs::path rootPath_;

    void SetUp() override
    {
        WDPBRec wdpb;
        std::memset(&wdpb, 42, sizeof(wdpb));
        wdpb.ioCompletion = nullptr;
        wdpb.ioVRefNum = 0;
        wdpb.ioNamePtr = nullptr;
        wdpb.ioWDIndex = 0;
        PBGetWDInfoSync(&wdpb);
        ASSERT_EQ(noErr, wdpb.ioResult);

        vRefNum_ = wdpb.ioWDVRefNum;
        const long parentDirID = wdpb.ioWDDirID;

        rootName_ = "lvtest-" + fs::unique_path("%%%%-%%%%").string();
        long dirID = 0;
        ASSERT_EQ(noErr, fmMakeDir(parentDirID, rootName_, &dirID));
        ASSERT_GT(dirID, 0);
        rootDirID_ = dirID;

        rootPath_ = ExecutorTestTempDir / rootName_;
        ASSERT_TRUE(fs::is_directory(rootPath_)) << rootPath_;
    }

    void TearDown() override
    {
        if(!rootPath_.empty())
        {
            boost::system::error_code ec;
            fs::remove_all(rootPath_, ec);
        }
    }

    // ---- host side ----

    fs::path host(std::string_view rel) const
    {
        return rel.empty() ? rootPath_ : rootPath_ / std::string(rel);
    }

    void hostDir(std::string_view rel) { fs::create_directories(host(rel)); }

    void hostFile(std::string_view rel, std::string_view contents = {})
    {
        fs::create_directories(host(rel).parent_path());
        fs::ofstream out(host(rel), std::ios::binary);
        out.write(contents.data(), contents.size());
    }

    void hostBytes(std::string_view rel, const std::vector<uint8_t>& bytes)
    {
        fs::create_directories(host(rel).parent_path());
        fs::ofstream out(host(rel), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    std::string hostRead(std::string_view rel) const
    {
        fs::ifstream in(host(rel), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }

    // Mirrors LocalVolume's name-only hidden predicate (AppleDouble `._`/`%`,
    // Basilisk `.rsrc`/`.finf`).
    static bool isHiddenName(const std::string& n)
    {
        return (n.size() >= 2 && n.compare(0, 2, "._") == 0)
            || (!n.empty() && n[0] == '%')
            || n == ".rsrc" || n == ".finf";
    }

    std::vector<std::string> hostList(std::string_view rel = {}) const
    {
        std::vector<std::string> names;
        for(auto& e : fs::directory_iterator(host(rel)))
        {
            std::string n = e.path().filename().string();
            if(!isHiddenName(n))
                names.push_back(n);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    // ---- FileManager side ----

    static void initPB(HParamBlockRec& pb)
    {
        std::memset(&pb, 42, sizeof(pb));
        pb.ioParam.ioCompletion = nullptr;
    }

    std::optional<ListEntry> fmGet(long parentDirID, std::string_view name)
    {
        CInfoPBRec ipb;
        std::memset(&ipb, 42, sizeof(ipb));
        ipb.hFileInfo.ioCompletion = nullptr;
        ipb.hFileInfo.ioVRefNum = vRefNum_;
        ipb.hFileInfo.ioDirID = parentDirID;
        auto n = str255(name);
        ipb.hFileInfo.ioNamePtr = n.data();
        ipb.hFileInfo.ioFDirIndex = 0;
        PBGetCatInfoSync(&ipb);
        if(ipb.hFileInfo.ioResult != noErr)
            return std::nullopt;

        ListEntry e;
        e.name = pstring(n.data());
        e.cnid = ipb.hFileInfo.ioDirID;
        e.isDir = (ipb.hFileInfo.ioFlAttrib & kIsDirMask) != 0;
        return e;
    }

    OSErr fmMakeDir(long parentDirID, std::string_view name, long* outDirID = nullptr)
    {
        HParamBlockRec pb;
        initPB(pb);
        pb.ioParam.ioVRefNum = vRefNum_;
        pb.fileParam.ioDirID = parentDirID;
        auto n = str255(name);
        pb.ioParam.ioNamePtr = n.data();
        PBDirCreateSync(&pb);
        if(outDirID && pb.ioParam.ioResult == noErr)
            *outDirID = pb.fileParam.ioDirID;
        return pb.ioParam.ioResult;
    }

    OSErr fmCreateFile(long parentDirID, std::string_view name, long* outCNID = nullptr)
    {
        HParamBlockRec pb;
        initPB(pb);
        pb.ioParam.ioVRefNum = vRefNum_;
        pb.fileParam.ioDirID = parentDirID;
        auto n = str255(name);
        pb.ioParam.ioNamePtr = n.data();
        PBHCreateSync(&pb);
        if(outCNID && pb.ioParam.ioResult == noErr)
        {
            if(auto e = fmGet(parentDirID, name))
                *outCNID = e->cnid;
        }
        return pb.ioParam.ioResult;
    }

    OSErr fmDelete(long parentDirID, std::string_view name)
    {
        HParamBlockRec pb;
        initPB(pb);
        pb.ioParam.ioVRefNum = vRefNum_;
        pb.fileParam.ioDirID = parentDirID;
        auto n = str255(name);
        pb.ioParam.ioNamePtr = n.data();
        PBHDeleteSync(&pb);
        return pb.ioParam.ioResult;
    }

    OSErr fmRename(long parentDirID, std::string_view name, std::string_view newName)
    {
        HParamBlockRec pb;
        initPB(pb);
        pb.ioParam.ioVRefNum = vRefNum_;
        pb.fileParam.ioDirID = parentDirID;
        auto n = str255(name);
        auto nn = str255(newName);
        pb.ioParam.ioNamePtr = n.data();
        pb.ioParam.ioMisc = toIoMisc(nn.data());
        PBHRenameSync(&pb);
        return pb.ioParam.ioResult;
    }

    OSErr fmMove(long parentDirID, std::string_view name, long newParentDirID,
        std::string_view newName = {})
    {
        CMovePBRec mpb;
        std::memset(&mpb, 42, sizeof(mpb));
        mpb.ioCompletion = nullptr;
        mpb.ioVRefNum = vRefNum_;
        mpb.ioDirID = parentDirID;
        auto n = str255(name);
        auto nn = str255(newName);
        mpb.ioNamePtr = n.data();
        mpb.ioNewDirID = newParentDirID;
        mpb.ioNewName = newName.empty() ? nullptr : nn.data();
        PBCatMoveSync(&mpb);
        return mpb.ioResult;
    }

    // Indexed PBGetCatInfo enumeration (files and directories).
    std::vector<ListEntry> fmList(long dirID, bool includeDirectories = true)
    {
        std::vector<ListEntry> result;
        for(int i = 1;; i++)
        {
            Str255 buffer;
            buffer[0] = 0;
            CInfoPBRec ipb;
            std::memset(&ipb, 42, sizeof(ipb));
            ipb.hFileInfo.ioCompletion = nullptr;
            ipb.hFileInfo.ioVRefNum = vRefNum_;
            ipb.hFileInfo.ioDirID = dirID;
            ipb.hFileInfo.ioNamePtr = buffer;
            ipb.hFileInfo.ioFDirIndex = i;
            PBGetCatInfoSync(&ipb);
            if(ipb.hFileInfo.ioResult == fnfErr)
                break;
            EXPECT_EQ(noErr, ipb.hFileInfo.ioResult);
            bool isDir = (ipb.hFileInfo.ioFlAttrib & kIsDirMask) != 0;
            if(isDir && !includeDirectories)
                continue;
            result.push_back({ pstring(buffer), ipb.hFileInfo.ioDirID, isDir });
        }
        return result;
    }

    // Indexed PBHGetFInfo enumeration (files only).
    std::vector<ListEntry> fmListFiles(long dirID)
    {
        std::vector<ListEntry> result;
        for(int i = 1;; i++)
        {
            Str255 buffer;
            buffer[0] = 0;
            HParamBlockRec pb;
            initPB(pb);
            pb.ioParam.ioVRefNum = vRefNum_;
            pb.fileParam.ioDirID = dirID;
            pb.ioParam.ioNamePtr = buffer;
            pb.fileParam.ioFDirIndex = i;
            PBHGetFInfoSync(&pb);
            if(pb.ioParam.ioResult == fnfErr)
                break;
            EXPECT_EQ(noErr, pb.ioParam.ioResult);
            result.push_back({ pstring(buffer), pb.fileParam.ioDirID, false });
        }
        return result;
    }

    std::optional<int> fmChildCount(long dirID)
    {
        Str255 buffer;
        buffer[0] = 0;
        CInfoPBRec ipb;
        std::memset(&ipb, 42, sizeof(ipb));
        ipb.dirInfo.ioCompletion = nullptr;
        ipb.dirInfo.ioVRefNum = vRefNum_;
        ipb.dirInfo.ioNamePtr = buffer;
        ipb.dirInfo.ioDrDirID = dirID;
        ipb.dirInfo.ioFDirIndex = -1;
        PBGetCatInfoSync(&ipb);
        if(ipb.hFileInfo.ioResult != noErr)
            return std::nullopt;
        return (int)ipb.dirInfo.ioDrNmFls;
    }
};

} // namespace Executor::Test
