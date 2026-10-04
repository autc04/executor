#include "gtest/gtest.h"

#include "compat.h"

#ifdef EXECUTOR
#include <CQuickDraw.h>
#else
#include <Quickdraw.h>
#include <QDOffscreen.h>
#include <Windows.h>
#endif

#include <bitset>

struct OffscreenPort
{
    GrafPort port;
    Rect r;
    GUEST<GrafPtr> savedPort;

    OffscreenPort(int width = 2, int height = 2)
    {
        GetPort(&savedPort);
        OpenPort(&port);

        short rowBytes = (width + 31) / 31 * 4;
        port.portBits.rowBytes = rowBytes;
        port.portBits.baseAddr = NewPtrClear(rowBytes * height);
        SetRect(&r, 0,0,width,height);
        port.portBits.bounds = r;

        set();
    }

    ~OffscreenPort()
    {
        // ClosePort does not change the current port, so put the previous one
        // back first; otherwise we leave a closed port current.
        SetPort(savedPort);
        ClosePort(&port);
    }

    void set()
    {
        SetPort(&port);
    }

    uint8_t& data(int row, int offset)
    {
        return reinterpret_cast<uint8_t&>(
            port.portBits.baseAddr[row * port.portBits.rowBytes + offset]);
    }
};

struct OffscreenWorld
{
    GWorldPtr world = nullptr;
    Rect r;
    GUEST<CGrafPtr> savedPort;
    GUEST<GDHandle> savedGDevice;

    OffscreenWorld(int depth, int height = 2, int width = 2)
    {
        GetGWorld(&savedPort, &savedGDevice);
        SetRect(&r,0,0,2,2);
        NewGWorld(PTR(world), depth, &r, nullptr, nullptr, 0);
        LockPixels(world->portPixMap);
        set();
    }

    ~OffscreenWorld()
    {
        // SetGWorld changed the current port and device; restore them before
        // disposing the world so we don't leave the disposed world current.
        SetGWorld(savedPort, savedGDevice);
        DisposeGWorld(world);
    }

    void set()
    {
        SetGWorld(world, nullptr);
    }

    uint8_t& data(int row, int offset)
    {
        Ptr baseAddr = (*world->portPixMap)->baseAddr;
        uint8_t *data = (uint8_t*)baseAddr;
        short rowBytes = (*world->portPixMap)->rowBytes;
        rowBytes &= 0x3FFF;

        return data[row * rowBytes + offset];
    }

    uint16_t data16(int row, int offset)
    {
        return data(row, offset*2) * 256 + data(row, offset*2+1);
    }
    uint32_t data32(int row, int offset)
    {
        return data16(row, offset*2) * 65536 + data16(row, offset*2+1);
    }
};


TEST(QuickDraw, BWLine1)
{
    OffscreenPort port;

    EraseRect(&port.r);
    MoveTo(0,0);
    LineTo(2,2);

    EXPECT_EQ(0x80, port.data(0,0));
    EXPECT_EQ(0x40, port.data(1,0));
}

TEST(QuickDraw, GWorld8)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world(8);

    EraseRect(&world.r);
    MoveTo(0,0);
    LineTo(2,2);
    
    EXPECT_EQ(255, world.data(0,0));
    EXPECT_EQ(0, world.data(0,1));
    EXPECT_EQ(0, world.data(1,0));
    EXPECT_EQ(255, world.data(1,1));
}

TEST(QuickDraw, GWorld16)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world(16);

    EraseRect(&world.r);
    MoveTo(0,0);
    LineTo(2,2);
    
    EXPECT_EQ(0, world.data16(0,0));
    EXPECT_EQ(0x7FFF, world.data16(0,1));
}

TEST(QuickDraw, GWorld32)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world(32);

    EraseRect(&world.r);
    MoveTo(0,0);
    LineTo(2,2);
    
    EXPECT_EQ(0, world.data32(0,0));
    EXPECT_EQ(0x00FFFFFF, world.data32(0,1));
}

TEST(QuickDraw, BasicQDColorBW)
{
    OffscreenPort port;

    FillRect(&port.r, &qd.gray);
    EXPECT_EQ(0x80, port.data(0,0));
    EXPECT_EQ(0x40, port.data(1,0));
    
    ForeColor(whiteColor);
    BackColor(redColor);
    FillRect(&port.r, &qd.gray);

    EXPECT_EQ(0x40, port.data(0,0));
    EXPECT_EQ(0x80, port.data(1,0));

    EXPECT_EQ(whiteColor, port.port.fgColor);
    EXPECT_EQ(redColor, port.port.bkColor);
}

TEST(QuickDraw, BasicQDColor32)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world(32);


    FillRect(&world.r, &qd.gray);
    EXPECT_EQ(0x00000000, world.data32(0,0));
    EXPECT_EQ(0x00FFFFFF, world.data32(0,1));
    EXPECT_EQ(0x00FFFFFF, world.data32(1,0));
    EXPECT_EQ(0x00000000, world.data32(1,1));
    
    ForeColor(whiteColor);
    BackColor(redColor);

    const uint32_t rgbRed = 0xDD0806;

    EXPECT_EQ(0x00FFFFFF, world.world->fgColor);
    EXPECT_EQ(rgbRed, world.world->bkColor);

    FillRect(&world.r, &qd.gray);

    EXPECT_EQ(0x00FFFFFF, world.data32(0,0));
    EXPECT_EQ(rgbRed, world.data32(0,1));
    EXPECT_EQ(rgbRed, world.data32(1,0));
    EXPECT_EQ(0x00FFFFFF, world.data32(1,1));
} 

TEST(QuickDraw, GrayPattern32)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world(32);

    FillRect(&world.r, &qd.gray);
    EXPECT_EQ(0x00000000, world.data32(0,0));
    EXPECT_EQ(0x00FFFFFF, world.data32(0,1));
    EXPECT_EQ(0x00FFFFFF, world.data32(1,0));
    EXPECT_EQ(0x00000000, world.data32(1,1));
}

TEST(QuickDraw, GrayPattern8)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world(8);

    FillRect(&world.r, &qd.gray);
    EXPECT_EQ(0xFF, world.data(0,0));
    EXPECT_EQ(0x00, world.data(0,1));
    EXPECT_EQ(0x00, world.data(1,0));
    EXPECT_EQ(0xFF, world.data(1,1));
}

TEST(QuickDraw, GrayPattern1)
{
    OffscreenPort port;

    FillRect(&port.r, &qd.gray);
    EXPECT_EQ(0x80, port.data(0,0));
    EXPECT_EQ(0x40, port.data(1,0));
}

TEST(QuickDraw, BlackPattern32)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world(32);

    FillRect(&world.r, &qd.black);
    EXPECT_EQ(0x00000000, world.data32(0,0));
    EXPECT_EQ(0x00000000, world.data32(0,1));
    EXPECT_EQ(0x00000000, world.data32(1,0));
    EXPECT_EQ(0x00000000, world.data32(1,1));
}

TEST(QuickDraw, CopyBit8)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world1(8);

    world1.data(0,0) = 1;
    world1.data(0,1) = 2;
    world1.data(1,0) = 3;
    world1.data(1,1) = 4;
    OffscreenWorld world2(8);
    EraseRect(&world2.r);

    RgnHandle rgn1 = NewRgn();
    SetRectRgn(rgn1, 0,0,2,1);
    RgnHandle rgn2 = NewRgn();
    SetRectRgn(rgn2, 0,0,1,2);
    UnionRgn(rgn1, rgn2, rgn1);
    DisposeRgn(rgn2);

    CopyBits(
        &GrafPtr(world1.world)->portBits,
        &GrafPtr(world2.world)->portBits,
        &world1.r, &world2.r, srcCopy, rgn1);

    DisposeRgn(rgn1);

    EXPECT_EQ(1, world2.data(0,0));
    EXPECT_EQ(2, world2.data(0,1));
    EXPECT_EQ(3, world2.data(1,0));
    EXPECT_EQ(0, world2.data(1,1));
}

TEST(QuickDraw, CopyBit32)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world1(32);

    world1.data(0,3) = 1;
    world1.data(0,7) = 2;
    world1.data(1,3) = 3;
    world1.data(1,7) = 4;
    OffscreenWorld world2(32);
    EraseRect(&world2.r);

    RgnHandle rgn1 = NewRgn();
    SetRectRgn(rgn1, 0,0,2,1);
    RgnHandle rgn2 = NewRgn();
    SetRectRgn(rgn2, 0,0,1,2);
    UnionRgn(rgn1, rgn2, rgn1);
    DisposeRgn(rgn2);

    CopyBits(
        &GrafPtr(world1.world)->portBits,
        &GrafPtr(world2.world)->portBits,
        &world1.r, &world2.r, srcCopy, rgn1);

    DisposeRgn(rgn1);

    EXPECT_EQ(1, world2.data(0,3));
    EXPECT_EQ(2, world2.data(0,7));
    EXPECT_EQ(3, world2.data(1,3));
    EXPECT_EQ(255, world2.data(1,7));
}

TEST(QuickDraw, CopyMask8)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenPort mask;

    mask.data(0,0) = 0xC0;
    mask.data(1,0) = 0x80;
    
    OffscreenWorld world1(8);

    world1.data(0,0) = 1;
    world1.data(0,1) = 2;
    world1.data(1,0) = 3;
    world1.data(1,1) = 4;
    OffscreenWorld world2(8);
    EraseRect(&world2.r);

    CopyMask(
        &GrafPtr(world1.world)->portBits,
        &mask.port.portBits,
        &GrafPtr(world2.world)->portBits,
        &world1.r, &mask.r, &world2.r);


    EXPECT_EQ(1, world2.data(0,0));
    EXPECT_EQ(2, world2.data(0,1));
    EXPECT_EQ(3, world2.data(1,0));
    EXPECT_EQ(0, world2.data(1,1));
}

TEST(QuickDraw, CopyMask32)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenPort mask;

    mask.data(0,0) = 0xC0;
    mask.data(1,0) = 0x80;
    
    OffscreenWorld world1(32);

    world1.data(0,3) = 1;
    world1.data(0,7) = 2;
    world1.data(1,3) = 3;
    world1.data(1,7) = 4;
    OffscreenWorld world2(32);
    EraseRect(&world2.r);

    CopyMask(
        &GrafPtr(world1.world)->portBits,
        &mask.port.portBits,
        &GrafPtr(world2.world)->portBits,
        &world1.r, &mask.r, &world2.r);


    EXPECT_EQ(1, world2.data(0,3));
    EXPECT_EQ(2, world2.data(0,7));
    EXPECT_EQ(3, world2.data(1,3));
    EXPECT_EQ(255, world2.data(1,7));
}


TEST(QuickDraw, Hilite8)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world(8);

    Rect upper = world.r;
    upper.bottom /= 2;

    FillRect(&world.r, &qd.black);
    FillRect(&upper, &qd.white);

    EXPECT_EQ(0x00, world.data(0,0));
    EXPECT_EQ(0x00, world.data(0,1));
    EXPECT_EQ(0xFF, world.data(1,0));
    EXPECT_EQ(0xFF, world.data(1,1));

    LMSetHiliteMode(LMGetHiliteMode() & ~(1<<7));
    InvertRect(&world.r);

    EXPECT_NE(0x00, world.data(0,0));
    EXPECT_NE(0x00, world.data(0,1));
    EXPECT_EQ(0xFF, world.data(1,0));
    EXPECT_EQ(0xFF, world.data(1,1));

    LMSetHiliteMode(LMGetHiliteMode() & ~(1<<7));
    InvertRect(&world.r);

    EXPECT_EQ(0x00, world.data(0,0));
    EXPECT_EQ(0x00, world.data(0,1));
    EXPECT_EQ(0xFF, world.data(1,0));
    EXPECT_EQ(0xFF, world.data(1,1));
}


TEST(QuickDraw, Hilite32)
{
    if(!hasDeepGWorlds())
        return;
    OffscreenWorld world(32);

    Rect upper = world.r;
    upper.bottom /= 2;

    FillRect(&world.r, &qd.black);
    FillRect(&upper, &qd.white);

    EXPECT_EQ(0x00FFFFFF, world.data32(0,0));
    EXPECT_EQ(0x00FFFFFF, world.data32(0,1));
    EXPECT_EQ(0x00000000, world.data32(1,0));
    EXPECT_EQ(0x00000000, world.data32(1,1));

    LMSetHiliteMode(LMGetHiliteMode() & ~(1<<7));
    InvertRect(&world.r);

    EXPECT_NE(0x00FFFFFF, world.data32(0,0));
    EXPECT_NE(0x00FFFFFF, world.data32(0,1));
    EXPECT_EQ(0x00000000, world.data32(1,0));
    EXPECT_EQ(0x00000000, world.data32(1,1));

    LMSetHiliteMode(LMGetHiliteMode() & ~(1<<7));
    InvertRect(&world.r);

    EXPECT_EQ(0x00FFFFFF, world.data32(0,0));
    EXPECT_EQ(0x00FFFFFF, world.data32(0,1));
    EXPECT_EQ(0x00000000, world.data32(1,0));
    EXPECT_EQ(0x00000000, world.data32(1,1));
}


TEST(QuickDraw, UnionRect)
{
    Rect r1,r2,r;
    
    r1 = {100,200,1000,2000};
    r2 = {1,2,10,20};
    UnionRect(&r1, &r2, &r);

    EXPECT_EQ(1, r.top);
    EXPECT_EQ(2, r.left);
    EXPECT_EQ(1000, r.bottom);
    EXPECT_EQ(2000, r.right);

    r1 = {1000,2000,100,200};
    r2 = {1,2,10,20};
    UnionRect(&r1, &r2, &r);

    EXPECT_EQ(1, r.top);
    EXPECT_EQ(2, r.left);
    EXPECT_EQ(100, r.bottom);
    EXPECT_EQ(200, r.right);

    r1 = {1,2,10,20};
    r2 = {0,0,0,0};
    UnionRect(&r1, &r2, &r);

    EXPECT_EQ(0, r.top);
    EXPECT_EQ(0, r.left);
    EXPECT_EQ(10, r.bottom);
    EXPECT_EQ(20, r.right);
}

TEST(QuickDraw, PinRect)
{
    auto pin = [](Rect& r, Point p) -> Point {
        auto l = PinRect(&r, p);
        return { (int16_t)(l >> 16), (int16_t)l };
    };
    Rect r = { 10,20,30,40 };
    Point p;

    p = pin(r, {15,25});
    EXPECT_EQ(15, p.v);
    EXPECT_EQ(25, p.h);

    p = pin(r, {30,40});
    EXPECT_EQ(29, p.v);
    EXPECT_EQ(39, p.h);

    p = pin(r, {35,45});
    EXPECT_EQ(29, p.v);
    EXPECT_EQ(39, p.h);
}

TEST(QuickDraw, InvalidCloseRgn)
{
    OffscreenPort port;

    RgnHandle rgn = NewRgn();
    CloseRgn(rgn);      // CloseRgn without OpenRgn is invalid, but doesn't crash on real Macs.
    DisposeRgn(rgn);
}

TEST(QuickDraw, PaintCircle)
{
    using whlist = std::initializer_list<std::pair<int,int>>;
    for(auto [w,h] : whlist{
        {16,16},{15,15},{15,16},{16,15},{4,4},{3,3},{3,10},{10,3},

        // {9,4},{8,5},{90,47} // real macOS doesn't quite agree with my theory for these
        
        })
    {
        std::ostringstream str;
        str << "oval " << w << "x" << h;
        SCOPED_TRACE(str.str());

        OffscreenPort port(w, h);

        PaintOval(&port.r);

        int count = 0;
        for(int row = 0; row < port.r.bottom; row++)
            for(int offset = 0; offset < (port.r.right+7)/8; offset++)
                count += std::bitset<8>(port.data(row,offset)).count();
        
        int perfect = 0;
        for(int y = 0; y < port.r.bottom; y++)
        {
            float yy = (y + 0.5f) * 2.0f / port.r.bottom - 1.0f;

                
            int whitepixels = 0;
            int mask = 0x80;
            int offset = 0;
            while(offset < (port.r.right+7)/8 && (port.data(y,offset) & mask) == 0)
            {
                whitepixels++;
                mask >>= 1;
                if(!mask)
                {
                    mask = 0x80;
                    offset++;
                }
            }

            int expectedWhitePixels = 0;

                // takes a while on an ancient mac
            /*for(int x = 0; x < port.r.right; x++)
            {
                float xx = (x + 0.5f) * 2.0f / port.r.right - 1.0f;
                
                if(xx*xx + yy*yy < 1.0f)
                    expectedWhitePixels ++;
                else
                    break;
            }*/

                // binary search makes 8MHz feel fast.
            if(float xx = 0.5f * 2.0f / port.r.right - 1.0f;
                xx*xx + yy*yy >= 1.0f)
            {
                int a = 0;
                int b = port.r.right/2;

                while(b - a > 1)
                {
                    int m = (a + b) / 2;

                    if(float xx = (m + 0.5f) * 2.0f / port.r.right - 1.0f;
                        xx*xx + yy*yy < 1.0f)
                        b = m;
                    else
                        a = m;
                }
                expectedWhitePixels = b;
            }

            perfect += port.r.right - 2 * expectedWhitePixels;
            EXPECT_EQ(expectedWhitePixels, whitepixels);
        }
        EXPECT_EQ(perfect, count);
    }
}


TEST(QuickDraw, FrameCircle1)
{
    OffscreenPort port(32, 32);

    FrameOval(&port.r);

    int count = 0;
    for(int row = 0; row < port.r.bottom; row++)
        for(int offset = 0; offset < port.r.right/8; offset++)
            count += std::bitset<8>(port.data(row,offset)).count();
    
    EXPECT_EQ(96, count);
}

TEST(QuickDraw, FrameCircle2)
{
    OffscreenPort port(32, 32);

    PenSize(3, 3);
    FrameOval(&port.r);

    Rect r2 = port.r;
    InsetRect(&r2, 3, 3);
    InvertOval(&r2);

    InvertOval(&port.r);

    int count = 0;
    for(int row = 0; row < port.r.bottom; row++)
        for(int offset = 0; offset < port.r.right/8; offset++)
            count += std::bitset<8>(port.data(row,offset)).count();
    
    EXPECT_EQ(0, count);
}

TEST(QuickDraw, FrameRoundRect)
{
    OffscreenPort port(32, 32);

    PenSize(3, 3);
    FrameRoundRect(&port.r, 16, 16);

    Rect r2 = port.r;
    InsetRect(&r2, 3, 3);
    InvertRoundRect(&r2, 10, 10);

    InvertRoundRect(&port.r, 16, 16);

    int count = 0;
    for(int row = 0; row < port.r.bottom; row++)
        for(int offset = 0; offset < port.r.right/8; offset++)
            count += std::bitset<8>(port.data(row,offset)).count();
    
    EXPECT_EQ(0, count);
}

TEST(QuickDraw, FrameArc1)
{
    OffscreenPort port(32, 32);

    FrameArc(&port.r, 45, 90);
    
    int count = 0;
    for(int row = 0; row < port.r.bottom; row++)
    {
        int rowcount = 0;
        for(int offset = 0; offset < port.r.right/8; offset++)
            rowcount += std::bitset<8>(port.data(row,offset)).count();

        count += rowcount;
        
        EXPECT_LE(rowcount, 2);
    }
    
    EXPECT_LT(port.r.bottom / 2, count);
}


TEST(QuickDraw, PaintRoundRect_TooBig)
{
    OffscreenPort port(32, 24);

    PaintRoundRect(&port.r, 64, 64);

    InvertOval(&port.r);

    int count = 0;
    for(int row = 0; row < port.r.bottom; row++)
        for(int offset = 0; offset < port.r.right/8; offset++)
            count += std::bitset<8>(port.data(row,offset)).count();
    
    EXPECT_EQ(0, count);
}



TEST(QuickDraw, PaintRoundRect_TooSmall)
{
    OffscreenPort port(32, 1);

    PaintRoundRect(&port.r, 5, 5);

    InvertOval(&port.r);

    int count = 0;
    for(int row = 0; row < port.r.bottom; row++)
        for(int offset = 0; offset < port.r.right/8; offset++)
            count += std::bitset<8>(port.data(row,offset)).count();
    
    EXPECT_EQ(0, count);
}