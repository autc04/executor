#include "macbinary.h"
#include "plain.h"

#include <OSUtil.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

using namespace Executor;

namespace
{
constexpr uint64_t pad128(uint64_t n)
{
    return (n + 127) & ~(uint64_t)127;
}

uint16_t getU16BE(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint32_t getU32BE(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
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

// CRC-16/XMODEM (polynomial 0x1021, initial value 0), as used for the
// MacBinary II/III header checksum.
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

bool allZero(const uint8_t *p, size_t n)
{
    for(size_t i = 0; i < n; i++)
        if(p[i] != 0)
            return false;
    return true;
}

bool checkHeader(const uint8_t *h, uint64_t size)
{
    // Original version byte is always 0 for MacBinary I, II and III.
    if(h[0] != 0)
        return false;

    uint32_t nameLen = h[1];
    if(nameLen < 1 || nameLen > 63)
        return false;
    if(h[2] == 0)
        return false;
    if(h[74] != 0 || h[82] != 0)
        return false;

    bool isV23 = (h[122] == 129 && h[123] == 129)
        || (h[122] == 130 && (h[123] == 129 || h[123] == 130));

    // MacBinary III signature; some files carry it with earlier version numbers.
    if(std::memcmp(h + 102, "mBIN", 4) == 0)
        return true;

    // Filename characters must be sensible; allow a lone trailing CR.
    for(uint32_t k = 0; k < nameLen; k++)
    {
        uint8_t c = h[2 + k];
        if(c > 0 && c < 32 && !(c == 0x0d && nameLen > 1 && k + 1 == nameLen))
            return false;
    }

    uint64_t dflen = getU32BE(h + 83);
    uint64_t rflen = getU32BE(h + 87);
    uint16_t crcReported = getU16BE(h + 124);

    // Size sanity.  Resource forks that run past the end of file are common
    // enough that we tolerate a small overrun.
    if(128 + dflen > size)
        return false;
    if(128 + dflen + rflen > size + 4096)
        return false;

    uint64_t minLen = 128 + pad128(dflen) + (rflen ? rflen : 0);
    bool goodLen = (size == minLen) || (size == pad128(minLen));

    if(isV23)
    {
        // [102..115] is unused (the "mBIN" signature was handled above).
        if(!allZero(h + 102, 14) && !goodLen)
            return false;
        // Secondary header is unsupported.
        if(getU16BE(h + 120) != 0)
            return false;
    }
    else
    {
        // An all-empty file is not useful and would be matched by any all-zero
        // buffer.  [116..119] is the unpacked total length, which we manage and
        // therefore do not treat as evidence of garbage.
        if(dflen == 0 && rflen == 0)
            return false;
        if((!allZero(h + 99, 17) || !allZero(h + 120, 4)) && !goodLen)
            return false;
    }

    if(crcReported != 0 && crc16(h, 124) != crcReported)
        return false;

    return true;
}
} // namespace

bool Executor::isMacBinaryFile(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if(ext != ".bin")
        return false;

    fs::ifstream in(p, std::ios::binary);
    if(!in)
        return false;

    uint8_t header[128];
    in.read(reinterpret_cast<char*>(header), 128);
    if(in.gcount() != 128)
        return false;

    in.clear();
    in.seekg(0, std::ios::end);
    std::streamoff end = in.tellg();
    if(end < 128)
        return false;

    return checkHeader(header, (uint64_t)end);
}

MacBinaryFile::MacBinaryFile(std::unique_ptr<OpenFile> file)
    : file_(std::move(file))
{
    if(file_->read(0, header_, 128) != 128)
        throw OSErrorException(ioErr);
    if(header_[0] != 0 || header_[1] < 1 || header_[1] > 63)
        throw OSErrorException(ioErr);

    dataLength_ = getU32BE(header_ + 83);
    rsrcLength_ = getU32BE(header_ + 87);
    isV23_ = (header_[122] == 129 && header_[123] == 129)
        || (header_[122] == 130 && (header_[123] == 129 || header_[123] == 130));
    hasCRC_ = isV23_ || getU16BE(header_ + 124) != 0;
}

uint32_t MacBinaryFile::forkLength(Fork fork) const
{
    return fork == Fork::data ? dataLength_ : rsrcLength_;
}

uint64_t MacBinaryFile::rsrcOffset() const
{
    return 128 + pad128(dataLength_);
}

uint64_t MacBinaryFile::trailingOffset() const
{
    return rsrcOffset() + pad128(rsrcLength_);
}

uint64_t MacBinaryFile::forkOffset(Fork fork) const
{
    return fork == Fork::data ? 128 : rsrcOffset();
}

size_t MacBinaryFile::getEOF(Fork fork)
{
    return forkLength(fork);
}

size_t MacBinaryFile::read(Fork fork, size_t offset, void *p, size_t n)
{
    uint32_t len = forkLength(fork);
    if(offset >= len || n == 0)
        return 0;
    if(offset + n > len)
        n = len - offset;

    return file_->read(forkOffset(fork) + offset, p, n);
}

size_t MacBinaryFile::write(Fork fork, size_t offset, void *p, size_t n)
{
    if(n == 0)
        return 0;

    if(offset + n > forkLength(fork))
        setEOF(fork, offset + n);

    return file_->write(forkOffset(fork) + offset, p, n);
}

void MacBinaryFile::moveBlock(uint64_t from, uint64_t to, uint64_t size)
{
    if(from == to || size == 0)
        return;

    const uint64_t bufSize = 128 * 1024;
    std::vector<uint8_t> buf((size_t)std::min<uint64_t>(bufSize, size));

    if(to > from)
    {
        // Destination is above the source: copy from the end backwards.
        uint64_t remaining = size;
        while(remaining)
        {
            uint64_t n = std::min<uint64_t>(bufSize, remaining);
            uint64_t src = from + remaining - n;
            uint64_t dst = to + remaining - n;
            file_->read(src, buf.data(), (size_t)n);
            file_->write(dst, buf.data(), (size_t)n);
            remaining -= n;
        }
    }
    else
    {
        uint64_t done = 0;
        while(done < size)
        {
            uint64_t n = std::min<uint64_t>(bufSize, size - done);
            file_->read(from + done, buf.data(), (size_t)n);
            file_->write(to + done, buf.data(), (size_t)n);
            done += n;
        }
    }
}

void MacBinaryFile::zeroRange(uint64_t offset, uint64_t size)
{
    const uint64_t bufSize = 4096;
    std::vector<uint8_t> buf((size_t)std::min<uint64_t>(bufSize, size), 0);

    uint64_t done = 0;
    while(done < size)
    {
        uint64_t n = std::min<uint64_t>(bufSize, size - done);
        file_->write(offset + done, buf.data(), (size_t)n);
        done += n;
    }
}

void MacBinaryFile::setEOF(Fork fork, size_t sz)
{
    if(sz > 0xFFFFFFFFull)
        sz = 0xFFFFFFFFull;

    uint32_t oldData = dataLength_;
    uint32_t oldRsrc = rsrcLength_;
    uint64_t oldRsrcOff = 128 + pad128(oldData);
    uint64_t oldTrailOff = oldRsrcOff + pad128(oldRsrc);
    uint64_t oldEnd = file_->getEOF();

    if(fork == Fork::data)
    {
        uint32_t newData = (uint32_t)sz;
        if(newData == oldData)
            return;

        uint64_t newRsrcOff = 128 + pad128(newData);

        // Everything at or after the resource fork moves with it.
        moveBlock(oldRsrcOff, newRsrcOff, oldEnd - oldRsrcOff);

        // Zero the newly added data bytes (if we grew) and the data fork's
        // padding, up to the relocated resource fork.
        uint64_t zeroFrom = 128 + std::min<uint32_t>(oldData, newData);
        if(newRsrcOff > zeroFrom)
            zeroRange(zeroFrom, newRsrcOff - zeroFrom);

        dataLength_ = newData;
        writeHeader();
        file_->setEOF(newRsrcOff + (oldEnd - oldRsrcOff));
    }
    else
    {
        uint32_t newRsrc = (uint32_t)sz;
        if(newRsrc == oldRsrc)
            return;

        uint64_t newTrailOff = oldRsrcOff + pad128(newRsrc);

        // Only the trailing data (comment/padding) moves.
        moveBlock(oldTrailOff, newTrailOff, oldEnd - oldTrailOff);

        // Zero the newly added resource bytes (if we grew) and the resource
        // fork's padding, up to the relocated trailing data.
        uint64_t zeroFrom = oldRsrcOff + std::min<uint32_t>(oldRsrc, newRsrc);
        if(newTrailOff > zeroFrom)
            zeroRange(zeroFrom, newTrailOff - zeroFrom);

        rsrcLength_ = newRsrc;
        writeHeader();
        file_->setEOF(newTrailOff + (oldEnd - oldTrailOff));
    }
}

void MacBinaryFile::writeHeader()
{
    putU32BE(header_ + 83, dataLength_);
    putU32BE(header_ + 87, rsrcLength_);
    putU32BE(header_ + 116, (uint32_t)((uint64_t)dataLength_ + rsrcLength_));

    if(hasCRC_)
        putU16BE(header_ + 124, crc16(header_, 124));

    file_->write(0, header_, 128);
}

void MacBinaryFile::readFInfo(ItemInfo& info)
{
    info.file.info.fdType = getU32BE(header_ + 65);
    info.file.info.fdCreator = getU32BE(header_ + 69);
    info.file.info.fdFlags = header_[73];
    info.file.info.fdLocation = Point{ (int16_t)getU16BE(header_ + 75),
        (int16_t)getU16BE(header_ + 77) };
    info.file.info.fdFldr = (int16_t)getU16BE(header_ + 79);

    uint32_t creationTime = getU32BE(header_ + 91);
    uint32_t modTime = getU32BE(header_ + 95);
    if(creationTime != 0)
        info.creationTime = creationTime;
    if(modTime != 0)
        info.modTime = modTime;
}

void MacBinaryFile::writeFInfo(const ItemInfo& info)
{
    putU32BE(header_ + 65, (uint32_t)info.file.info.fdType);
    putU32BE(header_ + 69, (uint32_t)info.file.info.fdCreator);
    header_[73] = (uint8_t)info.file.info.fdFlags;
    putU16BE(header_ + 75, (uint16_t)info.file.info.fdLocation.v);
    putU16BE(header_ + 77, (uint16_t)info.file.info.fdLocation.h);
    putU16BE(header_ + 79, (uint16_t)info.file.info.fdFldr);
    putU32BE(header_ + 91, info.creationTime);
    putU32BE(header_ + 95, info.modTime);

    writeHeader();
}

MacBinaryFork::MacBinaryFork(std::shared_ptr<MacBinaryFile> file, MacBinaryFile::Fork fork)
    : file(std::move(file)), fork(fork)
{
}

size_t MacBinaryFork::getEOF()
{
    return file->getEOF(fork);
}

void MacBinaryFork::setEOF(size_t sz)
{
    file->setEOF(fork, sz);
}

size_t MacBinaryFork::read(size_t offset, void *p, size_t n)
{
    return file->read(fork, offset, p, n);
}

size_t MacBinaryFork::write(size_t offset, void *p, size_t n)
{
    return file->write(fork, offset, p, n);
}

std::shared_ptr<MacBinaryFile> MacBinaryFileItem::access(int8_t permission)
{
    std::shared_ptr<MacBinaryFile> p;
    if((p = openedFile.lock()))
        return p;

    p = std::make_shared<MacBinaryFile>(std::make_unique<PlainDataFork>(path(), permission));
    openedFile = p;
    return p;
}

std::unique_ptr<OpenFile> MacBinaryFileItem::open(int8_t permission)
{
    return std::make_unique<MacBinaryFork>(access(permission), MacBinaryFile::Fork::data);
}

std::unique_ptr<OpenFile> MacBinaryFileItem::openRF(int8_t permission)
{
    return std::make_unique<MacBinaryFork>(access(permission), MacBinaryFile::Fork::resource);
}

ItemInfo MacBinaryFileItem::getInfo()
{
    ItemInfo info = FileItem::getInfo();
    access(fsRdPerm)->readFInfo(info);
    return info;
}

void MacBinaryFileItem::setInfo(ItemInfo info)
{
    FileItem::setInfo(info);
    access(fsRdWrPerm)->writeFInfo(info);
}

ItemPtr MacBinaryItemFactory::createItemForDirEntry(ItemCache& itemcache, CNID parID, CNID cnid,
    const fs::directory_entry& e, mac_string_view macname)
{
    if(!fs::is_regular_file(e.path()))
        return nullptr;
    if(!isMacBinaryFile(e.path()))
        return nullptr;

    return std::make_shared<MacBinaryFileItem>(itemcache, parID, cnid, e.path(), macname);
}
