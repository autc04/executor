#include "gtest/gtest.h"

#include <Files.h>
#include <Devices.h>

TEST(Files, CreateDelete)
{
    ParamBlockRec pb;
    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioCompletion = nullptr;
    pb.ioParam.ioVRefNum = 0;
    pb.ioParam.ioNamePtr = (StringPtr)"\ptemp-test";
    PBCreateSync(&pb);

    ASSERT_EQ(noErr, pb.ioParam.ioResult);

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioCompletion = nullptr;
    pb.ioParam.ioVRefNum = 0;
    pb.ioParam.ioNamePtr = (StringPtr)"\ptemp-test";
    PBDeleteSync(&pb);

    EXPECT_EQ(noErr, pb.ioParam.ioResult);

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioCompletion = nullptr;
    pb.ioParam.ioVRefNum = 0;
    pb.ioParam.ioNamePtr = (StringPtr)"\ptemp-test";
    PBDeleteSync(&pb);

    EXPECT_EQ(fnfErr, pb.ioParam.ioResult);
}

class FileTest : public testing::Test
{
protected:
    Str255 file1 = "\ptemp-test";
    short vRefNum = 0;
    long dirID = 0;
    short refNum = -1;

    FileTest()
    {
        HParamBlockRec pb;
        memset(&pb, 42, sizeof(pb));
        pb.fileParam.ioCompletion = nullptr;
        pb.fileParam.ioVRefNum = vRefNum;
        pb.fileParam.ioNamePtr = file1;
        pb.fileParam.ioDirID = dirID;
        PBHCreateSync(&pb);

        EXPECT_EQ(noErr, pb.fileParam.ioResult);
    }

    ~FileTest()
    {
        close();

        HParamBlockRec pb;
        memset(&pb, 42, sizeof(pb));
        pb.fileParam.ioCompletion = nullptr;
        pb.fileParam.ioVRefNum = vRefNum;
        pb.fileParam.ioDirID = dirID;
        pb.fileParam.ioNamePtr = file1;
        PBHDeleteSync(&pb);

        EXPECT_EQ(noErr, pb.fileParam.ioResult);
    }

    void close(short rn)
    {
        if(refNum > 0)
        {
            ParamBlockRec pb;
            memset(&pb, 42, sizeof(pb));
            pb.ioParam.ioCompletion = nullptr;
            pb.ioParam.ioRefNum = rn;
            PBCloseSync(&pb);

            EXPECT_EQ(noErr, pb.fileParam.ioResult);
        }
    }


    void close()
    {
        if(refNum > 0)
            close(refNum);
        refNum = -1;
    }

    template<class F>
    short open(F OpenFun, SInt8 perm)
    {
        HParamBlockRec hpb;
        memset(&hpb, 42, sizeof(hpb));
        hpb.ioParam.ioCompletion = nullptr;
        hpb.ioParam.ioVRefNum = vRefNum;
        hpb.fileParam.ioDirID = dirID;
        hpb.ioParam.ioNamePtr = file1;
        hpb.ioParam.ioPermssn = perm;
        OpenFun(&hpb);

        EXPECT_EQ(noErr, hpb.fileParam.ioResult);
        EXPECT_GE(hpb.ioParam.ioRefNum, 2);

        if(hpb.fileParam.ioResult == noErr)
            refNum = hpb.ioParam.ioRefNum;
        return refNum;
    }

    short open(SInt8 perm = fsCurPerm)
    {
        return open([](auto pbp){PBHOpenSync(pbp);}, perm);
    }
    short openDF(SInt8 perm = fsCurPerm)
    {
        return open([](auto pbp){PBHOpenDFSync(pbp);}, perm);
    }
    short openRF(SInt8 perm = fsCurPerm)
    {
        return open([](auto pbp){PBHOpenRFSync(pbp);}, perm);
    }

    void hello()
    {
        open();

        char buf1[] = "Hello, world.";
        char buf2[100];

        ParamBlockRec pb;
        memset(&pb, 42, sizeof(pb));
        pb.ioParam.ioCompletion = nullptr;
        pb.ioParam.ioRefNum = refNum;
        pb.ioParam.ioPosMode = fsFromStart;
        pb.ioParam.ioPosOffset = 0;
        pb.ioParam.ioBuffer = buf1;
        pb.ioParam.ioReqCount = 13;
        PBWriteSync(&pb);

        EXPECT_EQ(noErr, pb.ioParam.ioResult);
        EXPECT_EQ(13, pb.ioParam.ioActCount);

        close();
    }

};



TEST_F(FileTest, Open)
{
    open();
}

TEST_F(FileTest, OpenDF)
{
    openDF();
}
TEST_F(FileTest, OpenRF)
{
    openRF();
}

TEST_F(FileTest, OpenTwiceForRead)
{
    open(fsRdPerm);

    HParamBlockRec hpb;
    memset(&hpb, 42, sizeof(hpb));
    hpb.ioParam.ioCompletion = nullptr;
    hpb.ioParam.ioVRefNum = vRefNum;
    hpb.fileParam.ioDirID = dirID;
    hpb.ioParam.ioNamePtr = file1;
    hpb.ioParam.ioPermssn = fsRdPerm;
    PBHOpenSync(&hpb);

    EXPECT_EQ(noErr, hpb.fileParam.ioResult);
    EXPECT_GE(hpb.ioParam.ioRefNum, 2);

    if(hpb.fileParam.ioResult == noErr)
    {
        short ref2 = hpb.ioParam.ioRefNum;

        EXPECT_NE(refNum, ref2);
        if(ref2 > 0 && refNum != ref2)
            close(ref2);
    }
}

TEST_F(FileTest, OpenTwiceForWrite)
{
    open(fsRdWrPerm);

    HParamBlockRec hpb;
    memset(&hpb, 42, sizeof(hpb));
    hpb.ioParam.ioCompletion = nullptr;
    hpb.ioParam.ioVRefNum = vRefNum;
    hpb.fileParam.ioDirID = dirID;
    hpb.ioParam.ioNamePtr = file1;
    hpb.ioParam.ioPermssn = fsRdWrPerm;
    PBHOpenSync(&hpb);

    EXPECT_EQ(opWrErr, hpb.fileParam.ioResult);
    EXPECT_GE(hpb.ioParam.ioRefNum, 2);
    EXPECT_EQ(hpb.ioParam.ioRefNum, refNum);

    if(hpb.fileParam.ioResult == noErr || hpb.fileParam.ioResult == opWrErr)
    {
        short ref2 = hpb.ioParam.ioRefNum;

        if(ref2 > 0 && refNum != ref2)
            close(ref2);
    }
}

TEST_F(FileTest, WriteRead)
{
    open();

    char buf1[] = "Hello, world.";
    char buf2[100];

    ParamBlockRec pb;
    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioCompletion = nullptr;
    pb.ioParam.ioRefNum = refNum;
    pb.ioParam.ioPosMode = fsFromStart;
    pb.ioParam.ioPosOffset = 0;
    pb.ioParam.ioBuffer = buf1;
    pb.ioParam.ioReqCount = 13;
    PBWriteSync(&pb);

    EXPECT_EQ(noErr, pb.ioParam.ioResult);
    EXPECT_EQ(13, pb.ioParam.ioActCount);

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioCompletion = nullptr;
    pb.ioParam.ioRefNum = refNum;
    pb.ioParam.ioPosMode = fsFromStart;
    pb.ioParam.ioPosOffset = 0;
    pb.ioParam.ioBuffer = buf2;
    pb.ioParam.ioReqCount = 13;
    PBReadSync(&pb);

    EXPECT_EQ(noErr, pb.ioParam.ioResult);
    EXPECT_EQ(13, pb.ioParam.ioActCount);

    EXPECT_EQ(0, memcmp(buf1, buf2, 13));


    close();


    memset(buf2, 0, 13);
    open();

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioCompletion = nullptr;
    pb.ioParam.ioRefNum = refNum;
    pb.ioParam.ioPosMode = fsFromStart;
    pb.ioParam.ioPosOffset = 0;
    pb.ioParam.ioBuffer = buf2;
    pb.ioParam.ioReqCount = 13;
    PBReadSync(&pb);

    EXPECT_EQ(noErr, pb.ioParam.ioResult);
    EXPECT_EQ(13, pb.ioParam.ioActCount);

    EXPECT_EQ(0, memcmp(buf1, buf2, 13));
}

TEST_F(FileTest, CaseInsensitive)
{
    CInfoPBRec ipb;

    Str255 name = "\pTEMP-test";

    // Files are found with case-insensitive match on file names
    // but GetCatInfo does not write to ioNamePtr

    memset(&ipb, 42, sizeof(ipb));
    ipb.hFileInfo.ioCompletion = nullptr;
    ipb.hFileInfo.ioVRefNum = vRefNum;
    Str255 buf;
    ipb.hFileInfo.ioNamePtr = name;
    ipb.hFileInfo.ioFDirIndex = 0;
    ipb.hFileInfo.ioDirID = dirID;

    PBGetCatInfoSync(&ipb);

    EXPECT_EQ(noErr, ipb.hFileInfo.ioResult);

    EXPECT_EQ("TEMP-test", std::string(name + 1, name + 1 + name[0]));
}

TEST_F(FileTest, Pathname)
{
    CInfoPBRec ipb;
    long curDirID = dirID;
    short realVRefNum;
    std::string path(file1 + 1, file1 + 1 + file1[0]);


    memset(&ipb, 42, sizeof(ipb));
    ipb.dirInfo.ioCompletion = nullptr;
    ipb.dirInfo.ioVRefNum = vRefNum;
    Str255 buf;
    ipb.dirInfo.ioNamePtr = nullptr;
    ipb.dirInfo.ioFDirIndex = -1;
    ipb.dirInfo.ioDrDirID = dirID;

    PBGetCatInfoSync(&ipb);

    realVRefNum = ipb.dirInfo.ioVRefNum;
    curDirID = ipb.dirInfo.ioDrDirID;

    for(int i = 0; i < 100 && curDirID != 1; i++)
    {
        memset(&ipb, 42, sizeof(ipb));
        ipb.dirInfo.ioCompletion = nullptr;
        ipb.dirInfo.ioVRefNum = realVRefNum;
        Str255 buf;
        ipb.dirInfo.ioNamePtr = buf;
        ipb.dirInfo.ioFDirIndex = -1;
        ipb.dirInfo.ioDrDirID = curDirID;

        PBGetCatInfoSync(&ipb);

        ASSERT_EQ(noErr, ipb.dirInfo.ioResult);

        if(curDirID == 2)
        {
            EXPECT_EQ(1, ipb.dirInfo.ioDrParID);
        }
        curDirID = ipb.dirInfo.ioDrParID;
        
        EXPECT_EQ(realVRefNum, ipb.dirInfo.ioVRefNum);
        path = std::string(buf + 1, buf + 1 + buf[0]) + ":" + path;
    };

    ASSERT_EQ(curDirID, 1);

    EXPECT_NE("", path);
    EXPECT_NE(std::string::npos, path.find(':'));
    EXPECT_NE(0, path.find(':'));

    printf("%s\n", path.c_str());

    hello();

    Str255 pathP;
    pathP[0] = std::min(size_t(255), path.size());
    memcpy(pathP+1, path.data(), pathP[0]);

    HParamBlockRec hpb;
    memset(&hpb, 42, sizeof(hpb));
    hpb.ioParam.ioCompletion = nullptr;
    hpb.ioParam.ioVRefNum = 0;
    hpb.fileParam.ioDirID = 0;
    hpb.ioParam.ioNamePtr = pathP;
    hpb.ioParam.ioPermssn = fsRdPerm;
    PBHOpenSync(&hpb);

    EXPECT_EQ(noErr, hpb.fileParam.ioResult);
    EXPECT_GE(hpb.ioParam.ioRefNum, 2);

    if(hpb.fileParam.ioResult == noErr)
    {
        refNum = hpb.ioParam.ioRefNum;

        char buf1[] = "Hello, world.";
        char buf2[100];

        ParamBlockRec pb;

        memset(&pb, 42, sizeof(pb));
        pb.ioParam.ioCompletion = nullptr;
        pb.ioParam.ioRefNum = refNum;
        pb.ioParam.ioPosMode = fsFromStart;
        pb.ioParam.ioPosOffset = 0;
        pb.ioParam.ioBuffer = buf2;
        pb.ioParam.ioReqCount = 13;
        PBReadSync(&pb);

        EXPECT_EQ(noErr, pb.ioParam.ioResult);
        EXPECT_EQ(13, pb.ioParam.ioActCount);

        EXPECT_EQ(0, memcmp(buf1, buf2, 13));
    }
}

TEST_F(FileTest, Seek)
{
    hello();
    open(fsRdPerm);

    ParamBlockRec pb;

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioRefNum = refNum;
    pb.ioParam.ioPosMode = fsAtMark;
    pb.ioParam.ioPosOffset = -1;
    PBSetFPosSync(&pb);

    EXPECT_EQ(noErr, pb.ioParam.ioResult);
    EXPECT_EQ(0, pb.ioParam.ioPosOffset);

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioRefNum = refNum;
    pb.ioParam.ioPosMode = fsFromStart;
    pb.ioParam.ioPosOffset = 3;
    PBSetFPosSync(&pb);

    EXPECT_EQ(noErr, pb.ioParam.ioResult);
    EXPECT_EQ(3, pb.ioParam.ioPosOffset);

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioRefNum = refNum;
    pb.ioParam.ioPosMode = fsFromMark;
    pb.ioParam.ioPosOffset = 4;
    PBSetFPosSync(&pb);

    EXPECT_EQ(noErr, pb.ioParam.ioResult);
    EXPECT_EQ(7, pb.ioParam.ioPosOffset);

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioRefNum = refNum;
    pb.ioParam.ioPosMode = fsFromLEOF;
    pb.ioParam.ioPosOffset = -3;
    PBSetFPosSync(&pb);

    EXPECT_EQ(noErr, pb.ioParam.ioResult);
    EXPECT_EQ(10, pb.ioParam.ioPosOffset);

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioRefNum = refNum;
    pb.ioParam.ioPosMode = fsFromStart;
    pb.ioParam.ioPosOffset = 42;
    PBSetFPosSync(&pb);

    EXPECT_EQ(eofErr, pb.ioParam.ioResult);
    EXPECT_EQ(13, pb.ioParam.ioPosOffset);

    memset(&pb, 42, sizeof(pb));
    pb.ioParam.ioRefNum = refNum;
    pb.ioParam.ioPosMode = fsFromLEOF;
    pb.ioParam.ioPosOffset = -42;
    PBSetFPosSync(&pb);

    EXPECT_EQ(posErr, pb.ioParam.ioResult);
    EXPECT_PRED1(
        [](long off) { 
            return off == 13        // Mac OS 9
                || off == 13-42;    // System 6
        }, pb.ioParam.ioPosOffset);

}