#include "gtest/gtest.h"

#include "compat.h"

#ifdef EXECUTOR
#include <ResourceMgr.h>
#include <FileMgr.h>
#else
#include <Resources.h>
#include <Files.h>
#endif

class ResourceTest : public testing::Test
{
public:
    short ref = -1;
    StringPtr filename;

    ResourceTest()
    {
        filename = PSTR("test-resfile");
        CreateResFile(filename);
        EXPECT_EQ(noErr, ResError());
        open();
    }

    void open()
    {
        ref = OpenResFile(filename);
        EXPECT_EQ(noErr, ResError());
        EXPECT_NE(-1, ref);
    }

    void close()
    {
        if(ref != -1)
            CloseResFile(ref);
        ref = -1;
    }

    void reopen()
    {
        close();
        open();
    }

    ~ResourceTest()
    {
        close();
        FSDelete(filename,0);
    }
};

TEST_F(ResourceTest, CreateDelete)
{
    EXPECT_EQ(0, GetResFileAttrs(ref) & 0xE0);
}

TEST_F(ResourceTest, CreateDuplicate)
{
    CreateResFile(filename);
    EXPECT_EQ(opWrErr, ResError());
}

TEST_F(ResourceTest, GetResourceNotFound)
{
    Handle h;
    h = Get1Resource('QUUX', 128);
    EXPECT_EQ(nullptr, h);
    EXPECT_EQ(noErr, ResError());

    h = GetResource('QUUX', 128);
    EXPECT_EQ(nullptr, h);
    EXPECT_EQ(noErr, ResError());
}

TEST_F(ResourceTest, AddResource)
{
    Handle h;
    h = NewHandle(4);
    *(GUEST<uint32_t>*)*h = 0xDEADBEEF;
    ASSERT_NE(nullptr, h);

    AddResource(h, 'QUUX', 128, PSTR("Hello, world."));
    EXPECT_EQ(noErr, ResError());

    EXPECT_EQ(mapChanged, GetResFileAttrs(ref) & mapChanged);
    EXPECT_EQ(0, GetResFileAttrs(ref) & mapReadOnly);
    //EXPECT_EQ(mapCompact, GetResFileAttrs(ref) & mapCompact);

    Handle h2 = GetResource('QUUX', 128);
    EXPECT_EQ(h, h2);
    EXPECT_EQ(noErr, ResError());

        // ReleaseResource is documented to do nothing if resChanged is set
    ReleaseResource(h);
    EXPECT_NE(nullptr, *h);

        // .. so a new handle will not reuse the same address
    h2 = NewHandleClear(4);
    DisposeHandle(h2);
    EXPECT_NE(h, h2);

    h2 = GetResource('QUUX', 128);
    EXPECT_EQ(h, h2);
    
    CloseResFile(ref);

    h = GetResource('QUUX', 128);
    EXPECT_EQ(nullptr, h);

    ref = OpenResFile(filename);
    EXPECT_EQ(0, GetResFileAttrs(ref) & mapChanged);
    EXPECT_EQ(0, GetResFileAttrs(ref) & mapReadOnly);

    h = GetResource('QUUX', 128);
    EXPECT_NE(nullptr, h);
    EXPECT_NE(nullptr, *h);
    EXPECT_EQ(0xDEADBEEF, *(GUEST<uint32_t>*)*h);

    DetachResource(h);

    h2 = GetResource('QUUX', 128);
    EXPECT_NE(nullptr, h2);
    EXPECT_NE(nullptr, *h2);
    EXPECT_NE(h, h2);
    EXPECT_EQ(0xDEADBEEF, *(GUEST<uint32_t>*)*h2);

    ReleaseResource(h2);    // not really a way to check this

        // .. a new handle might reuse the same address
    Handle h3 = NewHandleClear(4);
        // but it's not guaranteed to.

        // but now the next GetResource probably won't reuse the
        // address.
    Handle h4 = GetResource('QUUX', 128);
    EXPECT_NE(nullptr, h4);
    EXPECT_NE(h2, h4);

    DisposeHandle(h3);
    DisposeHandle(h);
}

TEST_F(ResourceTest, Modify1)
{
    Handle h1;
    h1 = NewHandle(4);
    *(GUEST<uint32_t>*)*h1 = 0xDEADBEEF;
    ASSERT_NE(nullptr, h1);

    AddResource(h1, 'QUUX', 128, PSTR("Hello, world."));
    EXPECT_EQ(noErr, ResError());

    Handle h2;
    h2 = NewHandle(4);
    *(GUEST<uint32_t>*)*h2 = 0xFEEDFACE;

    AddResource(h2, 'QUUX', 129, PSTR("Hello again."));
    EXPECT_EQ(noErr, ResError());

    reopen();

    h1 = GetResource('QUUX', 128);
    h2 = GetResource('QUUX', 129);

    ASSERT_EQ(0xDEADBEEF, *(GUEST<uint32_t>*)*h1);
    ASSERT_EQ(0xFEEDFACE, *(GUEST<uint32_t>*)*h2);

    SetHandleSize(h1, 1024);
    memset(*h1, 0xAB, 1024);
    ChangedResource(h1);

    reopen();

    h1 = GetResource('QUUX', 128);
    h2 = GetResource('QUUX', 129);

    ASSERT_EQ(0xABABABAB, *(GUEST<uint32_t>*)*h1);
    ASSERT_EQ(0xFEEDFACE, *(GUEST<uint32_t>*)*h2);
}

/* Compressed resources, with decompressors of our own (68k code, in the
   test file as 'dcmp' 200).

   Header version 8: dcmp(source, dest, working, size), Pascal. Copies the
   payload's first long to dest[0] and puts the 4th argument in dest[1]. */
static const uint16_t dcmp8Code[] = {
    0x206F, 0x0010, /* movea.l 16(sp),a0   source */
    0x226F, 0x000C, /* movea.l 12(sp),a1   dest */
    0x22D0,         /* move.l  (a0),(a1)+ */
    0x22AF, 0x0004, /* move.l  4(sp),(a1)  the 4th argument */
    0x205F,         /* movea.l (sp)+,a0 */
    0x4FEF, 0x0010, /* lea     16(sp),sp */
    0x4ED0,         /* jmp     (a0) */
};

/* Header version 9: three entry offsets, then init(header),
   decompress(source, dest, header): INTEGER and cleanup(header), Pascal.
   decompress copies the payload's first long to dest[0] and puts the
   header's uncompressed size in dest[1]. */
static const uint16_t dcmp9Code[] = {
    0x0006, 0x000C, 0x0006, /* init, decompress, cleanup */
    /* 6: init and cleanup */
    0x205F,         /* movea.l (sp)+,a0 */
    0x588F,         /* addq.l  #4,sp */
    0x4ED0,         /* jmp     (a0) */
    /* 12: decompress */
    0x206F, 0x000C, /* movea.l 12(sp),a0   source */
    0x226F, 0x0008, /* movea.l 8(sp),a1    dest */
    0x22D0,         /* move.l  (a0),(a1)+ */
    0x206F, 0x0004, /* movea.l 4(sp),a0    header */
    0x22A8, 0x0008, /* move.l  8(a0),(a1)  its uncompressed size */
    0x426F, 0x0010, /* clr.w   16(sp)      noErr */
    0x205F,         /* movea.l (sp)+,a0 */
    0x4FEF, 0x000C, /* lea     12(sp),sp */
    0x4ED0,         /* jmp     (a0) */
};

static void addCode(const uint16_t *code, size_t words, short id)
{
    Handle h = NewHandle(words * 2);
    for(size_t i = 0; i < words; i++)
        ((GUEST<uint16_t>*)*h)[i] = code[i];
    AddResource(h, 'dcmp', id, PSTR(""));
    EXPECT_EQ(noErr, ResError());
}

static void put32(Ptr p, uint32_t v)
{
    *(GUEST<uint32_t>*)p = v;
}

static void put16(Ptr p, uint16_t v)
{
    *(GUEST<uint16_t>*)p = v;
}

/* A 'QUUX' 128 compressed for 'dcmp' 200 with a version 8 header: an
   uncompressed size, the working buffer's ratio and the expansion. */
static Handle addCompressed8(uint32_t size, uint8_t ratio, uint8_t expansion,
                             short attrs)
{
    Handle h = NewHandle(16 + 8);
    put32(*h, 0xA89F6572);
    put32(*h + 4, 0x00120801);
    put32(*h + 8, size);
    (*h)[12] = ratio;
    (*h)[13] = expansion;
    put16(*h + 14, 200);
    put32(*h + 16, 0x600DF00D);
    put32(*h + 20, 0);
    AddResource(h, 'QUUX', 128, PSTR(""));
    EXPECT_EQ(noErr, ResError());
    SetResAttrs(h, resCompressed | attrs);
    ChangedResource(h);
    return h;
}

/* The same with a version 9 header: the decompressor's ID and the bytes
   beyond the uncompressed size the handle needs, 0. */
static Handle addCompressed9(uint32_t size)
{
    Handle h = NewHandle(18 + 8);
    put32(*h, 0xA89F6572);
    put32(*h + 4, 0x00120901);
    put32(*h + 8, size);
    put16(*h + 12, 200);
    put16(*h + 14, 0);
    put16(*h + 16, 0);
    put32(*h + 18, 0x600DF00D);
    put32(*h + 22, 0);
    AddResource(h, 'QUUX', 128, PSTR(""));
    EXPECT_EQ(noErr, ResError());
    SetResAttrs(h, resCompressed);
    ChangedResource(h);
    return h;
}

/* The decompressor is passed the working buffer's size: ratio + 1 256ths
   of the uncompressed size and the expansion, and 4 bytes more. */
TEST_F(ResourceTest, CompressedWorkingSize)
{
    addCode(dcmp8Code, std::size(dcmp8Code), 200);
    addCompressed8(64, 0x40, 4, 0);
    reopen();

    Handle h = GetResource('QUUX', 128);
    ASSERT_EQ(noErr, ResError());
    ASSERT_NE(nullptr, h);
    EXPECT_EQ(64, GetHandleSize(h));
    EXPECT_EQ(0x600DF00Du, *(GUEST<uint32_t>*)*h);
    EXPECT_EQ(((0x40 + 1) * (64 + 4)) / 256 + 4, *(GUEST<int32_t>*)(*h + 4));
}

/* A preloaded compressed resource finds its decompressor in the file
   being opened, whatever handle the map on disk names for it. */
TEST_F(ResourceTest, CompressedPreload)
{
    addCompressed8(64, 0, 0, resPreload);
    addCode(dcmp8Code, std::size(dcmp8Code), 200);
    reopen();

    Handle h = Get1Resource('QUUX', 128);
    ASSERT_NE(nullptr, h);
    EXPECT_EQ(64, GetHandleSize(h));
    EXPECT_EQ(0x600DF00Du, *(GUEST<uint32_t>*)*h);
}

/* Header version 9 (System 7.5.5's compressed resources): its own entry
   points; decompressed in place from the end of the handle. */
TEST_F(ResourceTest, CompressedVersion9)
{
    addCode(dcmp9Code, std::size(dcmp9Code), 200);
    addCompressed9(16);
    reopen();

    Handle h = GetResource('QUUX', 128);
    ASSERT_EQ(noErr, ResError());
    ASSERT_NE(nullptr, h);
    EXPECT_EQ(16, GetHandleSize(h));
    EXPECT_EQ(0x600DF00Du, *(GUEST<uint32_t>*)*h);
    EXPECT_EQ(16u, *(GUEST<uint32_t>*)(*h + 4));
}

/* Not loaded, a compressed resource's size is its uncompressed size, with
   either header. */
TEST_F(ResourceTest, CompressedSizeOnDisk)
{
    addCompressed8(64, 0, 0, 0);
    reopen();

    SetResLoad(false);
    Handle h = Get1Resource('QUUX', 128);
    SetResLoad(true);
    ASSERT_NE(nullptr, h);
    EXPECT_EQ(64, GetResourceSizeOnDisk(h));
    EXPECT_EQ(noErr, ResError());
}

TEST_F(ResourceTest, CompressedVersion9SizeOnDisk)
{
    addCompressed9(48);
    reopen();

    SetResLoad(false);
    Handle h = Get1Resource('QUUX', 128);
    SetResLoad(true);
    ASSERT_NE(nullptr, h);
    EXPECT_EQ(48, GetResourceSizeOnDisk(h));
    EXPECT_EQ(noErr, ResError());
}

