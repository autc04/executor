#include "hfs/hfs_plus.h"

#include <commandline/program_options_extended.h>

#include <fstream>
#include <iostream>
#include <sstream>

namespace po = boost::program_options;
namespace fs = boost::filesystem;

using namespace Executor;

struct MemView
{
    const void *ptr;
    size_t size;
};

struct CatalogEntry
{
    HFSPlusCatalogKey key;
    union {
        HFSPlusCatalogFolder folder;
        HFSPlusCatalogFile file;
        HFSPlusCatalogThread thread;
    };

    MemView getKey() const
    {
        return { &key, size_t(key.keyLength) + 2U };
    }
    MemView getValue() const
    {
        switch(folder.recordType)
        {
            case 1: /* folder */
                return { &folder, sizeof(folder) };
            case 2: /* file */
                return { &file, sizeof(file) };
            default:
                return { &thread, size_t(thread.nodeName.length * 2 + 10) };
        }
    }
};

class VolumeWriter
{
    HFSPlusVolumeHeader header;
    std::ofstream out;

    std::vector<CatalogEntry> catalog;

public:
    VolumeWriter(fs::path dstFilename);

    HFSPlusForkData writeData(const void* data, size_t size);

    void finishUp();

    void addCatalogEntry(CatalogEntry e) { catalog.push_back(e); }

    template<typename T>
    HFSPlusForkData writeBTree(
        UInt16 maxKeyLength,
        UInt32 attributes,
        const std::vector<T>& entries);
    void writeCatalog();
};

VolumeWriter::VolumeWriter(fs::path dstFilename)
    : out(dstFilename.string(), std::ios::trunc | std::ios::binary)
{
    header.signature = ('H' << 8) | '+';
    header.version = 4;
    header.attributes = 1 << 8 /* kHFSVolumeUnmountedBit */;
    header.lastMountedVersion = "EXEC"_4;
    header.blockSize = 0x1000;
    //header.totalBlocks = 0;
    //header.freeBlocks = 0;
    header.nextAllocation = 1;
    header.rsrcClumpSize = 0x10000;
    header.dataClumpSize = 0x10000;
    header.nextCatalogID = 0x10;  // ???
    header.writeCount = 1;
    header.encodingsBitmap = 1;
}
HFSPlusForkData VolumeWriter::writeData(const void* data, size_t size)
{
    out.seekp(size_t(header.blockSize) * header.nextAllocation);
    out.write((const char*)data, size);

    uint32_t firstAllocated = header.nextAllocation;
    uint32_t totalBlocks = (size + (header.blockSize - 1)) / header.blockSize;
    header.nextAllocation += totalBlocks;
    return {
        size,
        header.dataClumpSize,
        totalBlocks,
        { 
            { firstAllocated, totalBlocks }
        }
    };
}

template<typename T>
HFSPlusForkData VolumeWriter::writeBTree(
    UInt16 maxKeyLength,
    UInt32 attributes,
    const std::vector<T>& entries)
{
    uint32_t totalNodes = 1;
    
    constexpr int nodeSize = 8192;
    constexpr int firstRecordOffsetIndex = nodeSize / 2 - 1;
    union Node
    {
        BTNodeDescriptor desc;
        std::byte bytes[nodeSize];
        GUEST<uint16_t> offsets[nodeSize/2];
    };

    std::vector<Node> nodes;

    BTHeaderRec btheader = {};

    btheader.treeDepth = 0;
    //btheader.rootNode = root;
    btheader.rootNode = 0;
    btheader.nodeSize = nodeSize;
    btheader.clumpSize = 1;
    
    btheader.maxKeyLength = maxKeyLength;
    btheader.btreeType = 0;
    btheader.attributes = attributes;

    auto allocNode = [&]() {
        nodes.push_back({});
        uint32_t nodeNum = nodes.size() - 1;
        Node& node = nodes[nodeNum];
        node.offsets[firstRecordOffsetIndex] = 14;
        return nodeNum;
    };

    auto addRecord = [&](uint32_t nodeNum, const void* data, size_t size) {
        Node& node = nodes[nodeNum];
        uint16_t freeSpace = node.offsets[firstRecordOffsetIndex - node.desc.numRecords];

        size_t paddedSize = (size + 1) & ~1;
        if (freeSpace + paddedSize > nodeSize - 2 * (node.desc.numRecords + 2))
            return false;

        node.desc.numRecords++;
        node.offsets[firstRecordOffsetIndex - node.desc.numRecords] = freeSpace + paddedSize;
        std::memcpy(&node.bytes[freeSpace], data, size);
        return true;
    };

    auto addKeyedRecord = [&](uint32_t nodeNum, MemView key, MemView value) {
        Node& node = nodes[nodeNum];
        uint16_t freeSpace = node.offsets[firstRecordOffsetIndex - node.desc.numRecords];

        size_t size = ((key.size + 1) & ~1) + ((value.size + 1) & ~1);

        if (freeSpace + size > nodeSize - 2 * (node.desc.numRecords + 2))
            return false;

        node.desc.numRecords++;
        node.offsets[firstRecordOffsetIndex - node.desc.numRecords] = freeSpace + size;
        std::memcpy(&node.bytes[freeSpace], key.ptr, key.size);
        std::memcpy(&node.bytes[freeSpace + ((key.size + 1) & ~1)], value.ptr, value.size);
        return true;
    };



    auto maxRecordSize = [&](uint32_t nodeNum) {
        Node& node = nodes[nodeNum];
        uint16_t freeSpace = node.offsets[firstRecordOffsetIndex - node.desc.numRecords];

        return nodeSize - 2 * (node.desc.numRecords + 2) - freeSpace;
    };

    allocNode(); // header node;
    nodes[0].desc.kind = 1; /* kBTHeaderNode */

    if (!entries.empty())
    {
        btheader.firstLeafNode = allocNode();
        btheader.lastLeafNode = btheader.firstLeafNode;
        nodes[btheader.firstLeafNode].desc.kind = -1;
        nodes[btheader.firstLeafNode].desc.height = 1;
        btheader.rootNode = btheader.firstLeafNode;
        btheader.treeDepth = 1;
    }
    btheader.totalNodes = nodes.size();
    btheader.freeNodes = 0;

    for (const auto& e : entries)
    {
        addKeyedRecord(btheader.lastLeafNode, e.getKey(), e.getValue());
        btheader.leafRecords++;
    }

    bool added;
    added = addRecord(0, &btheader, sizeof(btheader));
    assert(added);

    uint8_t userData[128] = {};
    added = addRecord(0, &userData, sizeof(userData));
    assert(added);
    
    std::vector<uint8_t> bitmap = {0};
    size_t bitmapRecordSize = maxRecordSize(0);
    bitmap.resize(std::max<size_t>(bitmapRecordSize, nodes.size() / 8 + 1));

    for (int i = 0; i < nodes.size(); i++)
    {
        bitmap[i / 8] |= 0x80 >> (i % 8);
    }

    added = addRecord(0, bitmap.data(), bitmapRecordSize);
    assert(added);
    assert(bitmap.size() <= bitmapRecordSize);

    return writeData(nodes.data(), nodes.size() * sizeof(Node));
}

void VolumeWriter::writeCatalog()
{
    header.catalogFile = writeBTree(
        516,
        6 /*kBTBigKeysMask + kBTVariableIndexKeysMask */,
        catalog);

    uint32_t maxId = 15;

    for (const auto& c : catalog)
    {
        if (c.folder.recordType == 1)
            header.folderCount++;
        if (c.file.recordType == 2)
            header.fileCount++;
        if (c.key.parentID > maxId)
            maxId = c.key.parentID;
    }
    header.folderCount--;
    header.nextCatalogID = maxId + 1;

}

void VolumeWriter::finishUp()
{
    header.extentsFile = writeBTree(
        10, 2 /*kBTBigKeysMask*/, std::vector<CatalogEntry>{});
    writeCatalog();

    header.freeBlocks = 10 * 256;   // 10MB
    header.totalBlocks = header.nextAllocation + header.freeBlocks + 1;
    
    uint32_t bitmapSize = (header.totalBlocks + (8 * header.blockSize) - 1) / (8 * header.blockSize);
    header.totalBlocks += bitmapSize;

    uint32_t fullUntil = header.nextAllocation + bitmapSize;
    std::vector<uint8_t> bitmap(bitmapSize * header.blockSize);
    for(int i = 0; i < fullUntil / 8; i++)
        bitmap[i] = 0xFF;
    for (uint32_t block = fullUntil & ~7; block < fullUntil; block++)
    {
        bitmap[block/8] |= 0x80 >> (block%8);
    }
    bitmap[(header.totalBlocks-1)/8] |= 0x80 >> ((header.totalBlocks - 1) % 8);

    header.allocationFile = writeData(bitmap.data(), bitmap.size());

    out.seekp(0x400);
    out.write((const char*)&header, sizeof(header));
    out.seekp(size_t(header.totalBlocks) * header.blockSize - 1);
    out.put(0);
    out.seekp(size_t(header.totalBlocks) * header.blockSize - 1024);
    out.write((const char*)&header, sizeof(header));
}

std::vector<std::byte> slurpFile(std::istream& in, const HFSPlusVolumeHeader& header, HFSPlusForkData forkData)
{
    std::vector<std::byte> data;
    size_t offset = 0;
    data.resize(size_t(header.blockSize) * forkData.totalBlocks);
    for (int i = 0; i < 8; i++)
    {
        if (forkData.extents[i].blockCount == 0)
            break;
        in.seekg(size_t(header.blockSize) * forkData.extents[i].startBlock);
        in.read((char*) &data[offset], size_t(header.blockSize) * forkData.extents[i].blockCount);
        offset += size_t(header.blockSize) * forkData.extents[i].blockCount;
    }
    data.resize(forkData.logicalSize);
    return data;
}

void dumpBTreeFile(std::vector<std::byte> data)
{
    const BTHeaderRec& header = *(const BTHeaderRec*)&data[14];

    std::cout << "treeDepth: " << header.treeDepth << std::endl;
    std::cout << "rootNode: " << header.rootNode << std::endl;
    std::cout << "leafRecords: " << header.leafRecords << std::endl;
    std::cout << "firstLeafNode: " << header.firstLeafNode << std::endl;
    std::cout << "lastLeafNode: " << header.lastLeafNode << std::endl;
    std::cout << "nodeSize: " << header.nodeSize << std::endl;
    std::cout << "maxKeyLength: " << header.maxKeyLength << std::endl;
    std::cout << "totalNodes: " << header.totalNodes << std::endl;
    std::cout << "freeNodes: " << header.freeNodes << std::endl;
    std::cout << "clumpSize: " << header.clumpSize << std::endl;
    std::cout << "btreeType: " << header.btreeType << std::endl;
    std::cout << "attributes: " << header.attributes << std::endl;

    for (int i = 0; i < header.totalNodes; i++)
    {
        std::cout << "node " << i << std::endl;
        const BTNodeDescriptor& desc = *(const BTNodeDescriptor*)&data[i * header.nodeSize];
        std::cout << "  fLink: " << desc.fLink << std::endl;
        std::cout << "  bLink: " << desc.bLink << std::endl;
        std::cout << "  kind: " << (int)desc.kind << std::endl;
        std::cout << "  height: " << (int)desc.height << std::endl;
        std::cout << "  numRecords: " << desc.numRecords << std::endl;

    }
}
void dumpHfsPlus(fs::path path)
{
    std::ifstream in(path.string());

    HFSPlusVolumeHeader header;
    in.seekg(0x400);
    in.read((char*)&header, sizeof(header));

    std::cout << "Extents BTree:\n";
    dumpBTreeFile(slurpFile(in, header, header.extentsFile));
    std::cout << "Catalog BTree:\n";
    dumpBTreeFile(slurpFile(in, header, header.catalogFile));
}

int main(int argc, char* argv[])
{
    po::options_description desc;

    fs::path output, dumpFile;

    desc.add_options()
        ("output,o", po::value(&output)->default_value("./test.img"), "where to put the fs")
        ("dump,d", po::value(&dumpFile), "file to dump")
    ;

    auto parsed = po::command_line_parser(argc, argv)
        .options(desc)
        .allow_unregistered()
        .run();
    po::variables_map vm;
    po::store(parsed, vm);
    po::notify(vm);

    if (!dumpFile.empty())
    {
        dumpHfsPlus(dumpFile);
        return 0;
    }

    VolumeWriter writer(output);

    {
        CatalogEntry e = {};
        e.key.keyLength = 8;
        e.key.parentID = 1;
        e.key.nodeName.length = 1;
        e.key.nodeName.unicode[0] = 'a';
        e.folder.recordType = 1;
        e.folder.folderID = 2;
        e.folder.valence = 1;
        writer.addCatalogEntry(e);
    }
    {
        CatalogEntry e = {};
        e.key.keyLength = 8;
        e.key.parentID = 2;
        e.key.nodeName.length = 0;
        e.thread.recordType = 3;
        e.thread.parentID = 1;
        e.thread.nodeName.length = 1;
        e.thread.nodeName.unicode[0] = 'a';

        writer.addCatalogEntry(e);
    }


    {
        CatalogEntry e = {};
        e.key.keyLength = 8;
        e.key.parentID = 2;
        e.key.nodeName.length = 1;
        e.key.nodeName.unicode[0] = 'b';
        e.file.recordType = 2;
        e.file.fileID = 16;
        
        std::string hello = "Hello, world.\n";
        e.file.dataFork = writer.writeData(hello.data(), hello.size());

        writer.addCatalogEntry(e);
    }
    {
        CatalogEntry e = {};
        e.key.keyLength = 8;
        e.key.parentID = 16;
        e.key.nodeName.length = 0;
        e.thread.recordType = 4;
        e.thread.parentID = 2;
        e.thread.nodeName.length = 1;
        e.thread.nodeName.unicode[0] = 'b';

        writer.addCatalogEntry(e);
    }

    writer.finishUp();
    std::cout << "Done.\n";

/*

00000400  48 2b 00 04 80 00 01 00  31 30 2e 30 00 00 00 00  |H+......10.0....|
00000410  e0 75 61 f6 e0 75 47 4e  00 00 00 00 e0 75 45 d6  |.ua..uGN.....uE.|
00000420  00 00 00 09 00 00 00 04  00 00 10 00 00 00 09 f6  |................|
00000430  00 00 09 b1 00 00 01 b5  00 01 00 00 00 01 00 00  |................|
00000440  00 00 00 1d 00 00 00 13  00 00 00 00 00 00 00 01  |................|
00000450  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00000460  00 00 00 00 00 00 00 00  3b 2a 76 33 ce f5 e9 8e  |........;*v3....|
00000470  00 00 00 00 00 00 10 00  00 00 10 00 00 00 00 01  |................|
00000480  00 00 00 01 00 00 00 01  00 00 00 00 00 00 00 00  |................|
00000490  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*/
    return 0;
}