/* Copyright 1986-1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in QuickDraw.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <ToolboxEvent.h>
#include <MemoryMgr.h>
#include <WindowMgr.h>
#include <SysErr.h>

#include <mman/mman.h>
#include <quickdraw/cquick.h>
#include <quickdraw/picture.h>
#include <util/handle_vector.h>

#include <algorithm>

using namespace Executor;

void Executor::C_InitGraf(GUEST<GrafPtr> *gp)
{
    PixMapHandle main_gd_pixmap;

    (*(GUEST<GUEST<GrafPtr>*> *)SYN68K_TO_US(EM_A5)) = gp;

    main_gd_pixmap = GD_PMAP(LM(MainDevice));

    /* qdGlobals().screenBits flag bits must not be set */
    qdGlobals().screenBits.baseAddr = PIXMAP_BASEADDR(main_gd_pixmap);
    qdGlobals().screenBits.rowBytes = PIXMAP_ROWBYTES(main_gd_pixmap) / PIXMAP_PIXEL_SIZE(main_gd_pixmap);
    qdGlobals().screenBits.bounds = PIXMAP_BOUNDS(main_gd_pixmap);

#define patinit(d, s) (*(GUEST<LONGINT> *)(d.pat) = s, *((GUEST<LONGINT> *)(d.pat) + 1) = s)

    TheZoneGuard guard(LM(SysZone));

    patinit(qdGlobals().white, 0x00000000);
    patinit(qdGlobals().black, 0xffffffff);
    patinit(qdGlobals().gray, 0xaa55aa55);
    patinit(qdGlobals().ltGray, 0x88228822);
    patinit(qdGlobals().dkGray, 0x77dd77dd);

        // list of all open ports.
        // On MacOS, this might be kept in the system heap
        // and initialized earlier.
    LM(PortList) = NewHandleClear(2);

    LM(WMgrPort) = (WindowPtr)NewPtr(sizeof(GrafPort));
    OpenPort(LM(WMgrPort));

    LM(WMgrCPort) = (CWindowPtr)NewPtr(sizeof(CGrafPort));
    OpenCPort(LM(WMgrCPort));

    qdGlobals().thePort = guest_cast<GrafPtr>(LM(WMgrCPort));
    LM(ScrnBase) = qdGlobals().screenBits.baseAddr;

    StuffHex((Ptr)qdGlobals().arrow.data,
             (StringPtr)("\100000040006000700078007c007e007f"
                         "007f807c006c0046000600030003000000"));
    StuffHex((Ptr)qdGlobals().arrow.mask,
             (StringPtr)("\100c000e000f000f800fc00fe00ff00ff"
                         "80ffc0ffe0fe00ef00cf00878007800380"));
    qdGlobals().arrow.hotSpot.h = qdGlobals().arrow.hotSpot.v = 1;
    LM(CrsrState) = 0;

    LM(RndSeed) = TickCount();
    LM(ScrVRes) = LM(ScrHRes) = 72;
    LM(ScreenRow) = qdGlobals().screenBits.rowBytes;
    qdGlobals().randSeed = 1;
    LM(QDExist) = EXIST_YES;
}

/*
 * ROMlib_initport does everything except play with cliprgn and visrgn.
 * It exists so that SysError can call it and maniuplate clip and
 * vis without using the memory manager.
 */

void Executor::ROMlib_initport(GrafPtr p) /* INTERNAL */
{
    /* this is a grafport, not a cgrafport, so the flag bits must not be
     set.  when we get here, it is likely they they contain garbage,
     so initialize them before using any accesssor macros */
    p->portBits.rowBytes = 0;

    PORT_DEVICE(p) = 0;
    PORT_BITS(p) = qdGlobals().screenBits;
    PORT_RECT(p) = qdGlobals().screenBits.bounds;
    PORT_BK_PAT(p) = qdGlobals().white;
    PORT_FILL_PAT(p) = qdGlobals().black;
    PORT_PEN_PAT(p) = qdGlobals().black;
    PORT_PEN_LOC(p).h = PORT_PEN_LOC(p).v = 0;
    PORT_PEN_SIZE(p).h = PORT_PEN_SIZE(p).v = 1;
    PORT_PEN_MODE(p) = patCopy;
    PORT_PEN_VIS(p) = 0;
    PORT_TX_FONT(p) = 0;
    /* txFace is a Style (signed char); don't swap */
    PORT_TX_FACE(p) = 0;
    *((char *)&p->txFace + 1) = 0; /* Excel & tests show we need to do this. */
    PORT_TX_MODE(p) = srcOr;
    PORT_TX_SIZE(p) = 0;
    PORT_SP_EXTRA(p) = 0;
    PORT_FG_COLOR(p) = blackColor;
    PORT_BK_COLOR(p) = whiteColor;
    PORT_COLR_BIT(p) = 0;
    PORT_PAT_STRETCH(p) = 0;
    PORT_PIC_SAVE(p) = nullptr;
    PORT_REGION_SAVE(p) = nullptr;
    PORT_POLY_SAVE(p) = nullptr;
    PORT_GRAF_PROCS(p) = nullptr;
}

void Executor::C_SetPort(GrafPtr p)
{
    if(p == nullptr)
        warning_unexpected("SetPort("")");
    qdGlobals().thePort = p;
}

void Executor::C_InitPort(GrafPtr p)
{
    ROMlib_initport(p);
    SetEmptyRgn(PORT_VIS_REGION(p));
    SetEmptyRgn(PORT_CLIP_REGION(p));
    (*PORT_VIS_REGION(p))->rgnBBox = qdGlobals().screenBits.bounds;

    SetRect(&(*PORT_CLIP_REGION(p))->rgnBBox, -32767, -32767, 32767, 32767);
    SetPort(p);
}

void Executor::C_OpenPort(GrafPtr p)
{
    PORT_VIS_REGION(p) = NewRgn();
    PORT_CLIP_REGION(p) = NewRgn();
    InitPort(p);

    handle_vector<GrafPtr, Handle, 2, true> portList(LM(PortList));
    portList.push_back(p);
    *(GUEST<int16_t>*) *LM(PortList) = portList.size();
}

/*
 * "Five of a Kind" calls CloseWindow (x) followed by ClosePort (x).
 * That used to kill us, but checking LM(MemErr) gets around that problem.
 * I'm not sure how the Mac gets around it.
 */

void Executor::C_ClosePort(GrafPtr p)
{
    handle_vector<GrafPtr, Handle, 2, true> portList(LM(PortList));
    portList.erase(std::remove(portList.begin(), portList.end(), p), portList.end());
    *(GUEST<int16_t>*) *LM(PortList) = portList.size();

    DisposeRgn(PORT_VIS_REGION(p));
    if(LM(MemErr) == noErr)
    {
        DisposeRgn(PORT_CLIP_REGION(p));

        if(LM(MemErr) == noErr)
        {
            if(CGrafPort_p(p))
            {
                DisposePixPat(CPORT_BK_PIXPAT(p));
                DisposePixPat(CPORT_PEN_PIXPAT(p));
                DisposePixPat(CPORT_FILL_PIXPAT(p));
                DisposeHandle((Handle)CPORT_PIXMAP(p)); /* NOT DisposePixMap */

                DisposeHandle((Handle)CPORT_GRAFVARS(p));
            }
        }
    }
}

void Executor::C_GetPort(GUEST<GrafPtr> *pp)
{
    *pp = qdGlobals().thePort;
}

void Executor::C_GrafDevice(INTEGER d)
{
    PORT_DEVICE(qdGlobals().thePort) = d;
}

void Executor::C_SetPortBits(BitMap *bm)
{
    if(!CGrafPort_p(qdGlobals().thePort))
        PORT_BITS(qdGlobals().thePort) = *bm;
}

void Executor::C_PortSize(INTEGER w, INTEGER h)
{
    PORT_RECT(qdGlobals().thePort).bottom = PORT_RECT(qdGlobals().thePort).top + h;
    PORT_RECT(qdGlobals().thePort).right = PORT_RECT(qdGlobals().thePort).left + w;
}

void Executor::C_MovePortTo(INTEGER lg, INTEGER tg)
{
    GrafPtr current_port;
    Rect *port_bounds, *port_rect;
    int w, h;

    current_port = qdGlobals().thePort;
    port_bounds = &PORT_BOUNDS(current_port);
    port_rect = &PORT_RECT(current_port);
    lg = port_rect->left - lg;
    tg = port_rect->top - tg;
    w = RECT_WIDTH(port_bounds);
    h = RECT_HEIGHT(port_bounds);
    SetRect(port_bounds, lg, tg, lg + w, tg + h);
}

void Executor::C_SetOrigin(INTEGER h, INTEGER v)
{
    int32_t dh, dv;

    dh = h - PORT_RECT(qdGlobals().thePort).left;
    dv = v - PORT_RECT(qdGlobals().thePort).top;
    if(qdGlobals().thePort->picSave)
    {
        GUEST<int16_t> swappeddh;
        GUEST<int16_t> swappeddv;

        PICOP(OP_Origin);

        swappeddh = dh;
        PICWRITE(&swappeddh, sizeof swappeddh);
        swappeddv = dv;
        PICWRITE(&swappeddv, sizeof swappeddv);
    }
    OffsetRect(&PORT_BOUNDS(qdGlobals().thePort), dh, dv);
    OffsetRect(&PORT_RECT(qdGlobals().thePort), dh, dv);
    OffsetRgn(PORT_VIS_REGION(qdGlobals().thePort), dh, dv);
    if(LM(SaveVisRgn))
        OffsetRgn(LM(SaveVisRgn), dh, dv);
}

void Executor::C_SetClip(RgnHandle r)
{
    CopyRgn(r, PORT_CLIP_REGION(qdGlobals().thePort));
}

void Executor::C_GetClip(RgnHandle r)
{
    CopyRgn(PORT_CLIP_REGION(qdGlobals().thePort), r);
}

void Executor::C_ClipRect(Rect *r)
{
    Rect r_copy;

    /* We copy r to a local, in case it points to memory that might move
   * during the RectRgn.
   */
    r_copy = *r;
    RectRgn(PORT_CLIP_REGION(qdGlobals().thePort), &r_copy);
}

void Executor::C_BackPat(const Pattern * pp)
{
    if(CGrafPort_p(qdGlobals().thePort))
    {
        PixPatHandle old_bk;

        old_bk = CPORT_BK_PIXPAT(theCPort);
        if(old_bk && PIXPAT_TYPE(old_bk) == pixpat_type_orig)
            PIXPAT_1DATA(old_bk) = *pp;
        else
        {
            PixPatHandle new_bk = NewPixPat();

            PIXPAT_TYPE(new_bk) = 0;
            PIXPAT_1DATA(new_bk) = *pp;
            BackPixPat(new_bk);
        }
        /*
 * On a Mac, PIXPAT_1DATA is *not* updated here, instead a new pixpat
 * is created and the other fields are set up to point to this guy.
 */

        /* #warning BackPat not currently implemented properly ... */
    }
    else
        PORT_BK_PAT(qdGlobals().thePort) = *pp;
}

void Executor::ROMlib_fill_pat(const Pattern& pp) /* INTERNAL */
{
    if(CGrafPort_p(qdGlobals().thePort))
    {
        PixPatHandle old_fill;

        old_fill = CPORT_FILL_PIXPAT(theCPort);
        if(PIXPAT_TYPE(old_fill) == pixpat_type_orig)
            PIXPAT_1DATA(old_fill) = pp;
        else
        {
            PixPatHandle new_fill = NewPixPat();

            PIXPAT_TYPE(new_fill) = 0;
            PIXPAT_1DATA(new_fill) = pp;

            ROMlib_fill_pixpat(new_fill);
        }
        /*
 * It's not clear what we're supposed to be doing here.
 */
        /* #warning ROMlib_fill_pat not currently implemented properly... */
    }
    else
        PORT_FILL_PAT(qdGlobals().thePort) = pp;
}
