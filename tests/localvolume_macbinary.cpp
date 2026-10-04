#include "gtest/gtest.h"

#include <file/localvolume/macbinary.h>
#include <file/localvolume/plain.h>
#include <rsys/filesystem.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Executor;

namespace
{
uint64_t pad128(uint64_t n)
{
    return (n + 127) & ~(uint64_t)127;
}

void putU16BE(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void putU32BE(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint16_t crc16(const uint8_t *p, size_t n)
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

// Build a well-formed MacBinary II file (unless v2 is false, in which case it
// is a MacBinary I file that still carries a valid header CRC).
std::vector<uint8_t> makeMacBinary(const std::string& name,
    uint32_t type, uint32_t creator,
    const std::vector<uint8_t>& data, const std::vector<uint8_t>& rsrc,
    bool v2 = true)
{
    std::vector<uint8_t> h(128, 0);
    h[0] = 0;
    h[1] = (uint8_t)name.size();
    std::memcpy(&h[2], name.data(), name.size());
    putU32BE(&h[65], type);
    putU32BE(&h[69], creator);
    h[73] = 0x01;
    putU32BE(&h[83], (uint32_t)data.size());
    putU32BE(&h[87], (uint32_t)rsrc.size());
    putU32BE(&h[91], 0x11111111);
    putU32BE(&h[95], 0x22222222);
    if(v2)
    {
        h[122] = 129;
        h[123] = 129;
    }
    putU32BE(&h[116], (uint32_t)(data.size() + rsrc.size()));
    putU16BE(&h[124], crc16(h.data(), 124));

    std::vector<uint8_t> out = h;
    out.resize(128 + (size_t)pad128(data.size()), 0);
    std::memcpy(out.data() + 128, data.data(), data.size());
    out.resize(128 + (size_t)pad128(data.size()) + (size_t)pad128(rsrc.size()), 0);
    std::memcpy(out.data() + 128 + pad128(data.size()), rsrc.data(), rsrc.size());
    return out;
}

struct TempFile
{
    fs::path path;

    explicit TempFile(const std::string& suffix)
        : path(fs::temp_directory_path()
              / (fs::unique_path("executor-mbtest-%%%%-%%%%").string() + suffix))
    {
    }

    ~TempFile()
    {
        boost::system::error_code ec;
        fs::remove(path, ec);
    }

    void write(const std::vector<uint8_t>& bytes)
    {
        fs::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
};

std::unique_ptr<MacBinaryFile> openMB(const fs::path& path, int8_t permission = fsRdWrPerm)
{
    return std::make_unique<MacBinaryFile>(std::make_unique<PlainDataFork>(path, permission));
}

std::vector<uint8_t> readFork(MacBinaryFile& file, MacBinaryFile::Fork fork)
{
    size_t n = file.getEOF(fork);
    std::vector<uint8_t> buf(n);
    file.read(fork, 0, buf.data(), n);
    return buf;
}

const std::vector<uint8_t> dataBytes { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
const std::vector<uint8_t> rsrcBytes { 20, 21, 22, 23, 24 };
} // namespace

TEST(MacBinaryDetection, AcceptsBothForks)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes));
    EXPECT_TRUE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, AcceptsDataOnly)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, {}));
    EXPECT_TRUE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, AcceptsResourceOnly)
{
    TempFile f(".bin");
    f.write(makeMacBinary("suspend", 0x4150504c, 0x3f3f3f3f, {}, rsrcBytes));
    EXPECT_TRUE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, AcceptsMacBinaryIWithCRC)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes, false));
    EXPECT_TRUE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, RejectsWrongExtension)
{
    TempFile f(".dat");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes));
    EXPECT_FALSE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, RejectsTruncatedHeader)
{
    TempFile f(".bin");
    auto bytes = makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes);
    bytes.resize(100);
    f.write(bytes);
    EXPECT_FALSE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, RejectsBadNameLength)
{
    TempFile f(".bin");
    auto bytes = makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes);
    bytes[1] = 64;
    putU16BE(&bytes[124], crc16(bytes.data(), 124));
    f.write(bytes);
    EXPECT_FALSE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, RejectsNonZeroReservedByte)
{
    TempFile f(".bin");
    auto bytes = makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes);
    bytes[74] = 1;
    putU16BE(&bytes[124], crc16(bytes.data(), 124));
    f.write(bytes);
    EXPECT_FALSE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, RejectsInconsistentLengths)
{
    TempFile f(".bin");
    auto bytes = makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes);
    putU32BE(&bytes[83], 0xffff0000u);
    putU16BE(&bytes[124], crc16(bytes.data(), 124));
    f.write(bytes);
    EXPECT_FALSE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, RejectsCorruptCRC)
{
    TempFile f(".bin");
    auto bytes = makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes);
    bytes[124] ^= 0xff;
    f.write(bytes);
    EXPECT_FALSE(isMacBinaryFile(f.path));
}

TEST(MacBinaryDetection, RejectsEmptyMacBinaryI)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, {}, {}, false));
    EXPECT_FALSE(isMacBinaryFile(f.path));
}

#ifdef EXECUTOR_SOURCE_DIR
// A real MacBinary II file checked into the repository (a resource-fork-only
// "APPL" with data fork length 0 and resource fork length 515).
TEST(MacBinaryDetection, AcceptsRealFixture)
{
    fs::path p = fs::path(EXECUTOR_SOURCE_DIR) / "cxmon" / "utils" / "suspend.bin";
    ASSERT_TRUE(fs::exists(p));
    EXPECT_TRUE(isMacBinaryFile(p));

    auto file = openMB(p, fsRdPerm);
    EXPECT_EQ(file->getEOF(MacBinaryFile::Fork::data), 0u);
    EXPECT_EQ(file->getEOF(MacBinaryFile::Fork::resource), 515u);
}
#endif

TEST(MacBinaryFileTest, ReadsForks)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes));

    auto file = openMB(f.path, fsRdPerm);
    EXPECT_EQ(dataBytes, readFork(*file, MacBinaryFile::Fork::data));
    EXPECT_EQ(rsrcBytes, readFork(*file, MacBinaryFile::Fork::resource));
}

TEST(MacBinaryFileTest, ReadsFInfo)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes));

    auto file = openMB(f.path, fsRdPerm);
    ItemInfo info{};
    file->readFInfo(info);

    EXPECT_EQ((uint32_t)info.file.info.fdType, 0x54455854u);
    EXPECT_EQ((uint32_t)info.file.info.fdCreator, 0x74747874u);
    EXPECT_EQ((unsigned)info.file.info.fdFlags, 1u);
    EXPECT_EQ((int)info.file.info.fdLocation.v, 0);
    EXPECT_EQ((int)info.file.info.fdLocation.h, 0);
    EXPECT_EQ((int)info.file.info.fdFldr, 0);
    EXPECT_EQ(info.creationTime, 0x11111111u);
    EXPECT_EQ(info.modTime, 0x22222222u);
}

TEST(MacBinaryFileTest, WritesFInfo)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes));

    {
        auto file = openMB(f.path);
        ItemInfo info{};
        info.file.info.fdType = 0x4150504c;
        info.file.info.fdCreator = 0x3f3f3f3f;
        info.file.info.fdFlags = 0x21;
        info.file.info.fdLocation = Point{ 12, 34 };
        info.file.info.fdFldr = 42;
        info.creationTime = 0x33333333;
        info.modTime = 0x44444444;
        file->writeFInfo(info);
    }

    ASSERT_TRUE(isMacBinaryFile(f.path));
    auto file = openMB(f.path, fsRdPerm);
    ItemInfo info{};
    file->readFInfo(info);

    EXPECT_EQ((uint32_t)info.file.info.fdType, 0x4150504cu);
    EXPECT_EQ((uint32_t)info.file.info.fdCreator, 0x3f3f3f3fu);
    EXPECT_EQ((unsigned)info.file.info.fdFlags, 0x21u);
    EXPECT_EQ((int)info.file.info.fdLocation.v, 12);
    EXPECT_EQ((int)info.file.info.fdLocation.h, 34);
    EXPECT_EQ((int)info.file.info.fdFldr, 42);
    EXPECT_EQ(info.creationTime, 0x33333333u);
    EXPECT_EQ(info.modTime, 0x44444444u);
}

TEST(MacBinaryFileTest, GrowsDataForkPreservingResource)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes));

    {
        auto file = openMB(f.path);
        std::vector<uint8_t> ext(500, 0xab);
        file->write(MacBinaryFile::Fork::data, 0, ext.data(), ext.size());
    }

    ASSERT_TRUE(isMacBinaryFile(f.path));
    auto file = openMB(f.path, fsRdPerm);
    EXPECT_EQ(file->getEOF(MacBinaryFile::Fork::data), 500u);
    EXPECT_EQ(rsrcBytes, readFork(*file, MacBinaryFile::Fork::resource));
}

TEST(MacBinaryFileTest, ShrinksDataForkPreservingResource)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes));

    {
        auto file = openMB(f.path);
        file->setEOF(MacBinaryFile::Fork::data, 4);
    }

    ASSERT_TRUE(isMacBinaryFile(f.path));
    auto file = openMB(f.path, fsRdPerm);
    EXPECT_EQ(file->getEOF(MacBinaryFile::Fork::data), 4u);
    EXPECT_EQ(std::vector<uint8_t>({ 1, 2, 3, 4 }), readFork(*file, MacBinaryFile::Fork::data));
    EXPECT_EQ(rsrcBytes, readFork(*file, MacBinaryFile::Fork::resource));
}

TEST(MacBinaryFileTest, GrowsAndShrinksResourceForkPreservingData)
{
    TempFile f(".bin");
    f.write(makeMacBinary("hello", 0x54455854, 0x74747874, dataBytes, rsrcBytes));

    {
        auto file = openMB(f.path);
        std::vector<uint8_t> ext(300, 0xcd);
        file->write(MacBinaryFile::Fork::resource, 0, ext.data(), ext.size());
    }

    ASSERT_TRUE(isMacBinaryFile(f.path));
    {
        auto file = openMB(f.path, fsRdPerm);
        EXPECT_EQ(file->getEOF(MacBinaryFile::Fork::resource), 300u);
        EXPECT_EQ(dataBytes, readFork(*file, MacBinaryFile::Fork::data));
    }

    {
        auto file = openMB(f.path);
        file->setEOF(MacBinaryFile::Fork::resource, 2);
    }

    ASSERT_TRUE(isMacBinaryFile(f.path));
    auto file = openMB(f.path, fsRdPerm);
    EXPECT_EQ(file->getEOF(MacBinaryFile::Fork::resource), 2u);
    EXPECT_EQ(std::vector<uint8_t>({ 0xcd, 0xcd }), readFork(*file, MacBinaryFile::Fork::resource));
    EXPECT_EQ(dataBytes, readFork(*file, MacBinaryFile::Fork::data));
}
