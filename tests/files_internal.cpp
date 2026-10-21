#include "gtest/gtest.h"

#include <file/file.h>
#include <FileMgr.h>

#include <boost/filesystem/fstream.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <util/macstrings.h>

using namespace Executor;

TEST(FilesInternal, pathToFSSpec)
{
    auto path = fs::current_path();

    std::string macpath;
    if(path.root_name().empty())
        macpath = "vol" + path.string();
    else
    {
        macpath = path.root_name().string();
        if(macpath.back() == ':')
            macpath.pop_back();
        macpath += "/" + path.relative_path().string();
    }

    for(char& c : macpath)
    {
        if(c == ':')
            c = '/';
        else if(c == '/')
            c = ':';
#ifdef _WIN32
        else if(c == '\\')
            c = ':';
#endif
    }
    std::cout << macpath;

    auto native = nativePathToFSSpec(path);
    auto mac = cmdlinePathToFSSpec(macpath);

    ASSERT_EQ(true, native.has_value());
    ASSERT_EQ(true, mac.has_value());

    EXPECT_EQ(mac->vRefNum, native->vRefNum);
    EXPECT_EQ(mac->parID, native->parID);
    EXPECT_EQ(toUnicode(mac->name), toUnicode(native->name));
}

namespace
{
/* The local volume caches a folder's listing for a second
   (ItemCache::cleanDirectoryCache): wait that out after changing the host
   behind its back. */
void letCacheExpire()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
}

/* A folder on the host, under the tests' working directory (so on the
   local volume), and its Mac directory ID. */
struct HostFolder
{
    fs::path path;
    short vRefNum = 0;
    long dirID = 0;

    HostFolder()
        : path(fs::current_path() / fs::unique_path("hostfolder-%%%%-%%%%"))
    {
        fs::create_directory(path);
        letCacheExpire();
        auto spec = nativePathToFSSpec(path);
        EXPECT_TRUE(spec.has_value());
        if(!spec)
            return;
        CInfoPBRec pb = {};
        Str255 name;
        memcpy(name, spec->name, spec->name[0] + 1);
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = spec->vRefNum;
        pb.dirInfo.ioDrDirID = spec->parID;
        pb.dirInfo.ioFDirIndex = 0;
        PBGetCatInfoSync(&pb);
        EXPECT_EQ(noErr, pb.dirInfo.ioResult);
        vRefNum = spec->vRefNum;
        dirID = pb.dirInfo.ioDrDirID;
    }

    ~HostFolder()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }

    void create(const std::string& name)
    {
        fs::ofstream(path / name) << name;
    }

    /* The folder's contents as the File Manager lists them. */
    std::vector<std::string> list()
    {
        std::vector<std::string> names;
        for(int i = 1;; i++)
        {
            CInfoPBRec pb = {};
            Str255 name;
            pb.hFileInfo.ioNamePtr = name;
            pb.hFileInfo.ioVRefNum = vRefNum;
            pb.hFileInfo.ioDirID = dirID;
            pb.hFileInfo.ioFDirIndex = i;
            PBGetCatInfoSync(&pb);
            if(pb.hFileInfo.ioResult != noErr)
            {
                EXPECT_EQ(fnfErr, pb.hFileInfo.ioResult);
                break;
            }
            names.emplace_back((const char *)name + 1, name[0]);
        }
        return names;
    }

    /* A file's number (its catalog node ID). */
    long fileID(const std::string& filename)
    {
        CInfoPBRec pb = {};
        Str255 name;
        name[0] = filename.size();
        memcpy(name + 1, filename.data(), filename.size());
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vRefNum;
        pb.hFileInfo.ioDirID = dirID;
        pb.hFileInfo.ioFDirIndex = 0;
        PBGetCatInfoSync(&pb);
        EXPECT_EQ(noErr, pb.hFileInfo.ioResult);
        return pb.hFileInfo.ioDirID;
    }

    uint32_t modDate(const std::string& filename)
    {
        CInfoPBRec pb = {};
        Str255 name;
        name[0] = filename.size();
        memcpy(name + 1, filename.data(), filename.size());
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vRefNum;
        pb.hFileInfo.ioDirID = dirID;
        pb.hFileInfo.ioFDirIndex = 0;
        PBGetCatInfoSync(&pb);
        EXPECT_EQ(noErr, pb.hFileInfo.ioResult);
        return pb.hFileInfo.ioFlMdDat;
    }
};
}

/* Host file times are seconds since 1970; Mac dates seconds since 1904,
   2082844800 apart (66 years with 17 leap days: 1904 is a leap year). */
TEST(FilesInternal, HostFileDates)
{
    HostFolder folder;
    folder.create("f");
    fs::last_write_time(folder.path / "f", 0);
    letCacheExpire();
    EXPECT_EQ(2082844800u, folder.modDate("f"));
    fs::last_write_time(folder.path / "f", 86400 * 365);
    EXPECT_EQ(2082844800u + 86400u * 365u, folder.modDate("f"));
}

/* Files deleted on the host between two listings: in the middle of the
   folder, then at its end. The others keep their file numbers (aliases
   and file ID references depend on them). */
TEST(FilesInternal, ListingAfterHostDelete)
{
    HostFolder folder;
    folder.create("a");
    folder.create("b");
    folder.create("c");
    folder.create("d");
    letCacheExpire();
    EXPECT_EQ((std::vector<std::string>{ "a", "b", "c", "d" }), folder.list());
    long a = folder.fileID("a"), c = folder.fileID("c"), d = folder.fileID("d");

    fs::remove(folder.path / "b");
    letCacheExpire();
    EXPECT_EQ((std::vector<std::string>{ "a", "c", "d" }), folder.list());
    EXPECT_EQ(a, folder.fileID("a"));
    EXPECT_EQ(c, folder.fileID("c"));
    EXPECT_EQ(d, folder.fileID("d"));

    fs::remove(folder.path / "c");
    fs::remove(folder.path / "d");
    letCacheExpire();
    EXPECT_EQ((std::vector<std::string>{ "a" }), folder.list());
    EXPECT_EQ(a, folder.fileID("a"));

    folder.create("e");
    letCacheExpire();
    EXPECT_EQ((std::vector<std::string>{ "a", "e" }), folder.list());
}
