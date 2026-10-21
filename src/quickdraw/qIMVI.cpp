/* Copyright 1994, 1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in QuickDraw.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>

#include <vector>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <MemoryMgr.h>
#include <quickdraw/quick.h>
#include <quickdraw/region.h>
#include <quickdraw/picture.h>

#include "sspairtable.ctable"

using namespace Executor;

/*
 * Basically a region is just a bunch of stop start pairs where the pairs
 * are the starting and stopping points of xors off the previous line.
 * As such, the meat of this loop is just to xor all the lines with their
 * immediately preceeding lines and output those values.  To start things
 * off and to finish things we need to do an xor with a line of zeros, and
 * we don't count on the bits past bounds.right necessarily containing zeros,
 * so it is necessary to temporarily hold those characters, mask them off
 * and then replace them after we've finished with the line.
 *
 * After we're done there's a chance that the region we constructed was
 * either a null region or a simple rectangle, so we detect that and adjust
 * by hand.
 *
 * NOTE: with our "special" regions, the maximum size a region can have
 *	 is 32767, unlike what IMVI-17-25 states.
 */

#define MAXRGNSIZE 32767

OSErr Executor::C_BitMapToRegion(RgnHandle rh, const BitMap *bmp)
{
    INTEGER top, left, bottom, right, rowbytes, linelen, rgnsize;
    INTEGER x, y;
    unsigned char scruffmask, scruffhold0, scruffhold1;
    unsigned char *zeroline;
    unsigned char *line0p, *line1p, c, *p, *saveline0p, *saveline1p;
    INTEGER transition;
    Boolean havewritteny;
    /* 0x00 or 0x100, depending on the state of the last xorred byte */
    unsigned int tableindex;

    if((bmp->rowBytes & ~ROWMASK)
       && ((PixMap *)bmp)->pixelSize != 1)
        /*-->*/ return pixmapTooDeepErr;

    /* build the region data in host memory and size the
       handle once at the end. Growing it to MAXRGNSIZE up front wrote past
       the handle whenever that SetHandleSize failed (a full heap). */
    std::vector<GUEST<INTEGER>> out;

#define OUTPUT(v)                                                    \
    do                                                               \
    {                                                                \
        if(RGN_SMALL_SIZE + (out.size() + 1) * sizeof(INTEGER) > MAXRGNSIZE) \
        {                                                            \
            /* ### set the size to something reasonable, although we \
               should see what the mac does */                       \
            SetHandleSize((Handle)rh, RGN_SMALL_SIZE);               \
            return rgnTooBigErr;                                     \
        }                                                            \
        else                                                         \
            out.push_back(v);                                        \
    } while(0)

    top = bmp->bounds.top;
    left = bmp->bounds.left;
    bottom = bmp->bounds.bottom;
    right = bmp->bounds.right;
    rowbytes = bmp->rowBytes & ROWMASK;
    linelen = (right - left + 7) / 8;
    if(linelen <= 0)
        /*-->*/ goto it_is_empty;

    scruffmask = 0xFF << (8 - ((right - left) % 8));
    if(!scruffmask)
        scruffmask = 0xFF;
    zeroline = (unsigned char *)alloca(linelen);
    memset(zeroline, 0, linelen);

    line0p = zeroline;
    line1p = (unsigned char *)bmp->baseAddr;
    for(y = top; y <= bottom; ++y)
    {
        if(y == bottom)
            line1p = zeroline;
        saveline0p = line0p;
        saveline1p = line1p;

        scruffhold0 = line0p[linelen - 1];
        line0p[linelen - 1] &= scruffmask;
        scruffhold1 = line1p[linelen - 1];
        line1p[linelen - 1] &= scruffmask;
        tableindex = 0x0;
        havewritteny = false;
        for(x = left - 1; x < left + linelen * 8 - 1; x += 8)
        {
            c = *line0p++ ^ *line1p++;
            p = sspairtable[tableindex | c];
            if(*p && !havewritteny)
            {
                OUTPUT(y);
                havewritteny = true;
            }
            while((transition = *p++))
                OUTPUT(x + transition);
            tableindex = (c & 1) << 8;
        }
        if(tableindex)
            OUTPUT(x + 1);
        if(havewritteny)
            OUTPUT(RGN_STOP);
        saveline0p[linelen - 1] = scruffhold0;
        saveline1p[linelen - 1] = scruffhold1;
        line0p = saveline1p;
        line1p = saveline1p + rowbytes;
    }
    OUTPUT(RGN_STOP);
    rgnsize = RGN_SMALL_SIZE + out.size() * sizeof(INTEGER);
    if(rgnsize == RGN_SMALL_SIZE + (int)sizeof(INTEGER))
        goto it_is_empty;
    if(rgnsize == RGN_SMALL_SIZE + 9 * (int)sizeof(INTEGER))
    {
        /* one rectangle */
        SetHandleSize((Handle)rh, RGN_SMALL_SIZE);
        if(MemError() != noErr)
            return MemError();
        (*rh)->rgnBBox.top = out[0];
        (*rh)->rgnBBox.left = out[1];
        (*rh)->rgnBBox.bottom = out[4];
        (*rh)->rgnBBox.right = out[2];
        RGN_SET_SMALL(rh);
        return noErr;
    }
    SetHandleSize((Handle)rh, rgnsize);
    if(MemError() != noErr)
        return MemError();
    memcpy((char *)*rh + RGN_SMALL_SIZE, out.data(), out.size() * sizeof(INTEGER));
    (*rh)->rgnBBox = bmp->bounds;
/* #warning we are not setting the bounding box properly */
    (*rh)->rgnSize = rgnsize;
    return noErr;

it_is_empty:
    SetHandleSize((Handle)rh, RGN_SMALL_SIZE);
    if(MemError() != noErr)
        return MemError();
    RGN_BBOX(rh) = {};
    RGN_SET_SMALL(rh);
    return noErr;
}

PicHandle Executor::C_OpenCPicture(OpenCPicParams *newheaderp)
{
    PicHandle retval;

    retval = ROMlib_OpenPicture_helper(&newheaderp->srcRect, newheaderp);
    return retval;
}
