#include "gtest/gtest.h"
#include <base/mactype.h>
#include <base/byteswap.h>
#include <base/logging.h>
#include <base/structdump.h>
#include <FileMgr.h>
#include <cstring>
#include <sstream>
using namespace Executor;

TEST(guestvalues, andUL)
{
    EXPECT_EQ(10, GUEST<unsigned long>(10UL) & 0x7FFF);
}

TEST(guestvalues, assignToInt16)
{
    GUEST<int16_t> x;
    int16_t y;

    y = (int8_t)0x81;
    x = (int8_t)0x81;
    EXPECT_EQ(y, x.get());

    y = (int16_t)0x8001;
    x = (int16_t)0x8001;
    EXPECT_EQ(y, x.get());

    y = (int32_t)10;
    x = (int32_t)10;
    EXPECT_EQ(y, x.get());

    y = (int64_t)10;
    x = (int64_t)10;
    EXPECT_EQ(y, x.get());

    y = (uint8_t)0x81;
    x = (uint8_t)0x81;
    EXPECT_EQ(y, x.get());

    y = (uint16_t)10;
    x = (uint16_t)10;
    EXPECT_EQ(y, x.get());

    y = (uint32_t)10;
    x = (uint32_t)10;
    EXPECT_EQ(y, x.get());

    y = (uint64_t)10;
    x = (uint64_t)10;
    EXPECT_EQ(y, x.get());
}

TEST(guestvalues, rawHostOrder)
{
    GUEST<uint32_t> a = 0x12345678;
    GUEST<uint16_t> b = 0x1234;
    GUEST<Point> p = {0x1234, 0x5678};

    EXPECT_EQ(0x12345678, a);
    EXPECT_EQ(0x1234, b);
    EXPECT_EQ((Point{0x1234,0x5678}), p);
    
    EXPECT_EQ(0x12345678, a.raw_host_order()) << "As hexadecimal: " << std::hex << a.raw_host_order();
    EXPECT_EQ(0x1234, b.raw_host_order()) << "As hexadecimal: " << std::hex << b.raw_host_order();
    EXPECT_EQ(0x12345678, p.raw_host_order()) << "As hexadecimal: " << std::hex << p.raw_host_order();

    a.raw_host_order(0xBEEFCAFE);
    EXPECT_EQ(0xBEEFCAFE, a);

    b.raw_host_order(0xCAFE);
    EXPECT_EQ(0xCAFE, b);

    p.raw_host_order(0xBEEFCAFE);
    EXPECT_EQ((Point{(int16_t)0xBEEF, (int16_t)0xCAFE}), p);
}

// The generic logValue fallback should find the generated Executor::describeStruct
// via ADL and print decoded fields instead of "?".
TEST(structdump, logValueDescribesGeneratedStruct)
{
    HFileParam pb;
    std::memset(&pb, 0, sizeof(pb));
    pb.ioFRefNum = 7;
    pb.ioFDirIndex = 3;
    pb.ioFlFndrInfo.fdType = 0x54455854; // 'TEXT'

    std::ostringstream captured;
    auto* old = std::clog.rdbuf(captured.rdbuf());
    logging::logValue(pb);
    std::clog.rdbuf(old);

    const std::string out = captured.str();
    EXPECT_NE(out.find("ioFRefNum=7"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("ioFDirIndex=3"), std::string::npos) << "got: " << out;
    // Nested by-value struct is described recursively.
    EXPECT_NE(out.find("ioFlFndrInfo=FInfo{"), std::string::npos) << "got: " << out;
}

// A union (param block) prints each arm labelled.
TEST(structdump, logValueDescribesUnion)
{
    ParamBlockRec pb;
    std::memset(&pb, 0, sizeof(pb));
    pb.fileParam.ioFRefNum = 5;

    std::ostringstream captured;
    auto* old = std::clog.rdbuf(captured.rdbuf());
    logging::logValue(pb);
    std::clog.rdbuf(old);

    const std::string out = captured.str();
    EXPECT_NE(out.find("fileParam=FileParam{"), std::string::npos) << "got: " << out;
    EXPECT_NE(out.find("ioParam=IOParam{"), std::string::npos) << "got: " << out;
}

// The type registry is populated from every generated module (through the
// ReferenceAllStructDumps translation unit), independent of logValue use.
TEST(structdump, registryFindType)
{
    const structdump::TypeDesc* t = structdump::findType("HFileParam");
    ASSERT_NE(t, nullptr);
    EXPECT_STREQ(t->name, "HFileParam");
    EXPECT_EQ(t->size, sizeof(HFileParam));
    EXPECT_EQ(structdump::findType("NoSuchType"), nullptr);
}

TEST(structdump, registryPrints)
{
    const structdump::TypeDesc* t = structdump::findType("HFileParam");
    ASSERT_NE(t, nullptr);

    HFileParam pb;
    std::memset(&pb, 0, sizeof(pb));
    pb.ioFRefNum = 7;

    std::ostringstream os;
    t->print(os, &pb);
    EXPECT_NE(os.str().find("ioFRefNum=7"), std::string::npos) << "got: " << os.str();
}

// Point is hand-written (not-for: executor in MacTypes.yaml) but still gets a
// hand-written describeStruct.
TEST(structdump, describesPoint)
{
    std::ostringstream captured;
    auto* old = std::clog.rdbuf(captured.rdbuf());
    logging::logValue(Point{3, 4});
    std::clog.rdbuf(old);

    EXPECT_NE(captured.str().find("Point{3, 4}"), std::string::npos) << "got: " << captured.str();
}

TEST(structdump, trapFilter)
{
    logging::setTrapFilter("PB*,*Info");
    EXPECT_TRUE(logging::trapLogEnabled("PBGetFInfo/PBHGetFInfo"));
    EXPECT_TRUE(logging::trapLogEnabled("GetInfo"));
    EXPECT_FALSE(logging::trapLogEnabled("HOpenResFile"));

    logging::setTrapFilter("");
    EXPECT_TRUE(logging::trapLogEnabled("HOpenResFile"));
}
