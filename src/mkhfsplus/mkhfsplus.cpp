#include "hfs/hfs_plus.h"

#include <commandline/program_options_extended.h>

#include <fstream>
#include <iostream>
#include <sstream>

namespace po = boost::program_options;
namespace fs = boost::filesystem;

using namespace Executor;

SInt32 FastUnicodeCompare ( const GUEST<UniChar>* str1, int length1,
                            const GUEST<UniChar>* str2, int length2);

struct MemView
{
    const void *ptr;
    size_t size;
};

bool operator<(const HFSPlusCatalogKey& a, const HFSPlusCatalogKey& b)
{
    if (a.parentID < b.parentID)
        return true;
    else if (a.parentID > b.parentID)
        return false;
    else
        return FastUnicodeCompare(a.nodeName.unicode, a.nodeName.length, b.nodeName.unicode, b.nodeName.length) < 0;
}

template<typename T>
struct PointerNode
{
    T key;
    GUEST<uint32_t> node;

    friend bool operator<(const PointerNode<T>& a, const PointerNode<T>& b)
    {
        return a.key < b.key;
    }

    MemView getKey() const
    {
        return { &key, size_t(key.keyLength) + 2U };
    }
    MemView getValue() const
    {
        return { &node, sizeof(node) };
    }
};
struct CatalogEntry
{
    using KeyType = HFSPlusCatalogKey;
    HFSPlusCatalogKey key;
    union {
        HFSPlusCatalogFolder folder;
        HFSPlusCatalogFile file;
        HFSPlusCatalogThread thread;
    };

    friend bool operator<(const CatalogEntry& a, const CatalogEntry& b)
    {
        return a.key < b.key;
    }

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

    CatalogEntry* findEntryByCNID(HFSCatalogNodeID cnid);

public:
    VolumeWriter(fs::path dstFilename);

    HFSPlusForkData writeData(const void* data, size_t size);

    void finishUp();

    CatalogEntry& addCatalogEntry(CatalogEntry e) { return catalog.emplace_back(e); }

    template<typename T>
    HFSPlusForkData writeBTree(
        UInt16 maxKeyLength,
        UInt32 attributes,
        const std::vector<T>& entries);
    void writeCatalog();

    CatalogEntry& createFile(HFSCatalogNodeID parent, std::string name);
    CatalogEntry& createFolder(HFSCatalogNodeID parent, std::string name);
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
    header.nextCatalogID = 0x10;
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
        assert(freeSpace + size <= 2 * (firstRecordOffsetIndex - node.desc.numRecords));
        return true;
    };



    auto maxRecordSize = [&](uint32_t nodeNum) {
        Node& node = nodes[nodeNum];
        uint16_t freeSpace = node.offsets[firstRecordOffsetIndex - node.desc.numRecords];

        return nodeSize - 2 * (node.desc.numRecords + 2) - freeSpace;
    };

    allocNode(); // header node;
    nodes[0].desc.kind = 1; /* kBTHeaderNode */
    std::vector<PointerNode<typename T::KeyType>> unlinkedNodes;

    for (const auto& e : entries)
    {
        if (!btheader.lastLeafNode || !addKeyedRecord(btheader.lastLeafNode, e.getKey(), e.getValue()))
        {
            auto newNode = allocNode();
            unlinkedNodes.push_back({e.key, newNode});
            if (btheader.lastLeafNode)
                nodes[btheader.lastLeafNode].desc.fLink = newNode;
            else
                btheader.firstLeafNode = newNode;
            nodes[newNode].desc.bLink = btheader.lastLeafNode;
            nodes[newNode].desc.kind = -1;
            nodes[newNode].desc.height = 1;
            btheader.lastLeafNode = newNode;

            addKeyedRecord(btheader.lastLeafNode, e.getKey(), e.getValue());
        }
        btheader.leafRecords++;
        btheader.treeDepth = 1;
    }

    while (unlinkedNodes.size() > 1)
    {
        std::vector<PointerNode<typename T::KeyType>> nodesToLink(std::move(unlinkedNodes));
        unlinkedNodes.clear();

        btheader.treeDepth++;

        for (const auto& e : nodesToLink)
        {
            if (unlinkedNodes.empty() 
                || !addKeyedRecord(unlinkedNodes.back().node, e.getKey(), e.getValue()))
            {
                auto newNode = allocNode();
                if (!unlinkedNodes.empty())
                {
                    nodes[unlinkedNodes.back().node].desc.fLink = newNode;
                    nodes[newNode].desc.bLink = unlinkedNodes.back().node;
                }
                unlinkedNodes.push_back({e.key, newNode});
                nodes[newNode].desc.kind = 0;   /* index node */
                nodes[newNode].desc.height = btheader.treeDepth;
                
                addKeyedRecord(unlinkedNodes.back().node, e.getKey(), e.getValue());
            }
        }
    }

    if (!unlinkedNodes.empty())
    {
        btheader.rootNode = unlinkedNodes[0].node;
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
    size_t writtenBitmapSize = bitmapRecordSize;

    uint32_t lastMapNode = 0;
    while (writtenBitmapSize < bitmap.size())
    {
        bitmap[nodes.size() / 8] |= 0x80 >> (nodes.size() % 8);

        auto newNode = allocNode();
        nodes[newNode].desc.kind = 2;
        nodes[lastMapNode].desc.fLink = newNode;
        lastMapNode = newNode;

        bitmapRecordSize = maxRecordSize(newNode);
        bitmap.resize(std::max<size_t>(writtenBitmapSize + bitmapRecordSize, bitmap.size()));
        added = addRecord(newNode, bitmap.data() + writtenBitmapSize, bitmapRecordSize);
        writtenBitmapSize += bitmapRecordSize;
    }

    {
        BTHeaderRec& newHeader = *(BTHeaderRec*)&nodes[0].bytes[14];
        newHeader.totalNodes = nodes.size();
        newHeader.freeNodes = 0;
    }

    return writeData(nodes.data(), nodes.size() * sizeof(Node));
}

CatalogEntry* VolumeWriter::findEntryByCNID(HFSCatalogNodeID cnid)
{
    auto threadIt = std::lower_bound(catalog.begin(), catalog.end(),
        cnid,
        [](const CatalogEntry& entry, HFSCatalogNodeID id)
        { return entry.key.parentID < id; });
    assert(threadIt < catalog.end());
    if (threadIt == catalog.end())
        return nullptr;
    CatalogEntry searchEntry;
    searchEntry.key.nodeName = threadIt->thread.nodeName;
    searchEntry.key.parentID = threadIt->thread.parentID;
    auto elemIt = std::lower_bound(catalog.begin(), catalog.end(), searchEntry);
    if (elemIt == catalog.end())
        return nullptr;
    if (searchEntry < *elemIt)
        return nullptr;
    return &*elemIt;;
}

void VolumeWriter::writeCatalog()
{
    std::sort(catalog.begin(), catalog.end());

    uint32_t maxId = 15;

    for (const auto& c : catalog)
    {
        if (c.folder.recordType == 1)
            header.folderCount++;
        else if (c.file.recordType == 2)
            header.fileCount++;
        else if (c.thread.recordType == 3 || c.thread.recordType == 4)
        {
            if (HFSCatalogNodeID parentID = c.thread.parentID; parentID != 1)
            {
                if (CatalogEntry *entry = findEntryByCNID(parentID))
                {
                    assert(entry->folder.recordType == 1);
                    entry->folder.valence++;
                }
            }
            
        }
        if (c.key.parentID > maxId)
            maxId = c.key.parentID;
    }
    header.folderCount--;
    header.nextCatalogID = maxId + 1;

    header.catalogFile = writeBTree(
        516,
        6 /*kBTBigKeysMask + kBTVariableIndexKeysMask */,
        catalog);
}

HFSUniStr255 makeUniStr(std::string str)
{
    if (str.size() >= 256)
        str.resize(255);
    
    HFSUniStr255 res;
    res.length = str.size();
    for (int i = 0; i < str.size(); i++)
    {
        char c = str[i];
        if (c == '/')
            throw std::logic_error("/ in file name");
        else if (c == ':')
            res.unicode[i] = '/';
        else if (c >= 0 && c <= 127)
            res.unicode[i] = c;
        else
            res.unicode[i] = '?';
    }
    return res;
}

HFSPlusCatalogKey makeCatalogKey(HFSCatalogNodeID cnid, std::string s)
{
    HFSPlusCatalogKey key;
    key.parentID = cnid;
    key.nodeName = makeUniStr(s);
    key.keyLength = key.nodeName.length * 2 + 6;
    return key;
}

CatalogEntry& VolumeWriter::createFile(HFSCatalogNodeID parent, std::string name)
{
    CatalogEntry e = {};
    e.key = makeCatalogKey(parent, name);
    e.file.recordType = 2;
    e.file.fileID = header.nextCatalogID++;;
    

    CatalogEntry thread = {};
    thread.key = makeCatalogKey(e.file.fileID, "");
    thread.thread.recordType = 4;
    thread.thread.parentID = parent;
    thread.thread.nodeName = e.key.nodeName;

    addCatalogEntry(thread);
    return addCatalogEntry(e);
}

CatalogEntry& VolumeWriter::createFolder(HFSCatalogNodeID parent, std::string name)
{
    CatalogEntry e = {};
    e.key = makeCatalogKey(parent, name);
    e.folder.recordType = 1;
    e.folder.folderID = header.nextCatalogID++;;
    

    CatalogEntry thread = {};
    thread.key = makeCatalogKey(e.folder.folderID, "");
    thread.thread.recordType = 3;
    thread.thread.parentID = parent;
    thread.thread.nodeName = e.key.nodeName;

    addCatalogEntry(thread);
    return addCatalogEntry(e);;
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

        const GUEST<uint16_t> *offsets = (const GUEST<uint16_t>*)&data[i * header.nodeSize];
        for (int j = 0; j < desc.numRecords; j++)
        {
            uint16_t off = offsets[header.nodeSize/2 - 1 - j];
            std::cout << "    record #" << j << " at " << off
                << " -- key size: " << offsets[off/2]
                << "\n";
        }
        std::cout << "  free space at " << offsets[header.nodeSize/2 - 1 - desc.numRecords] << "\n";
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
        e.key = makeCatalogKey(1, "My Volume");
        e.folder.recordType = 1;
        e.folder.folderID = 2;
        //e.folder.valence = 1;
        writer.addCatalogEntry(e);
    }
    {
        CatalogEntry e = {};
        e.key = makeCatalogKey(2, "");
        e.thread.recordType = 3;
        e.thread.parentID = 1;
        e.thread.nodeName = makeUniStr("My Volume");

        writer.addCatalogEntry(e);
    }

    {
        CatalogEntry& e = writer.createFile(2, "My first file");
        std::string hello = "Hello, world.\n";
        e.file.dataFork = writer.writeData(hello.data(), hello.size());
    }

    HFSCatalogNodeID myFolderCNID = writer.createFolder(2, "Folder").folder.folderID;
    for (int i = 0; i < 200; i++)
    {
        std::string content = std::to_string(i) + "\n";
        std::string name = "File #" + std::to_string(i);
        CatalogEntry& e = writer.createFile(myFolderCNID, name);
        e.file.dataFork = writer.writeData(content.data(), content.size());
        e.file.userInfo.fdType = "TEXT"_4;
        e.file.userInfo.fdCreator = "ttxt"_4;
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