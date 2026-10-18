/*
 * NOTE: currently we only do an automount when crossing a filesystem.
 *	 This is might be bad:  currently we're seeing an error when
 *	 we try to open "/tmp" on roland, because 4 has already been
 *	 specified as "/Net" from iclone.  I'm really not convinced
 *	 that the proper solution is to do an automount, though.
 */

/* Copyright 1987 - 1997 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in StdFilePkg.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <rsys/stdfile.h>

#include <DialogMgr.h>
#include <FileMgr.h>
#include <EventMgr.h>
#include <StdFilePkg.h>
#include <ControlMgr.h>
#include <MenuMgr.h>
#include <ResourceMgr.h>
#include <ToolboxEvent.h>
#include <ToolboxUtil.h>
#include <MemoryMgr.h>
#include <OSUtil.h>
#include <ScriptMgr.h>

#include <quickdraw/cquick.h>
#include <wind/wind.h>
#include <hfs/hfs.h>
#include <file/file.h>
#include <rsys/arrowkeys.h>
#include <res/resource.h>
#include <time/time.h>
#include <mman/tempalloc.h>
#include <rsys/hook.h>
#include <rsys/toolevent.h>
#include <util/string.h>

#include <rsys/osutil.h>
#include <osevent/osevent.h>

#include <print/print.h>
#include <error/system_error.h>

#include <base/functions.impl.h>
#include <base/traps.impl.h>
#include <prefs/prefs.h>

using namespace Executor;

typedef union {
    SFReply *oreplyp;
    StandardFileReply *nreplyp;
} reply_u;

typedef union {
    FileFilterUPP oflfilef;
    FileFilterYDUPP cflfilef;
} file_filter_u;

typedef union {
    ModalFilterUPP ofilterp;
    ModalFilterYDUPP cfilterp;
} filter_u;

typedef union {
    DlgHookUPP odh;
    DlgHookYDUPP cdh;
} dialog_hook_u;

typedef struct
{
    struct flinfostr
    {
        INTEGER floffs;
        INTEGER flicns;
    };

    sf_flavor_t flavor;
    reply_u flreplyp;
    ControlHandle flsh;
    INTEGER flnmfil;
    INTEGER flnmlin;
    INTEGER fllinht;
    INTEGER flascent;
    INTEGER flsel;
    Rect flrect;
    Rect flcurdirrect;
    GUEST<flinfostr *> *flinfo;
    GUEST<char *> *flstrs;
    file_filter_u flfilef;
    INTEGER flnumt;
    GUEST<OSType> *fltl;
    ControlHandle flch;
    INTEGER flgraynondirs;
    Str255 flcurdirname;
    filter_u magicfp;
    INTEGER fl_cancel_item;
    void *mydata;
} fltype;

static INTEGER movealert(INTEGER);
static void drawminiicon(INTEGER);
static void drawinboxwithicon(StringPtr, Rect *, INTEGER);
static void safeflflip(fltype *, INTEGER);
static void flupdate(fltype *, INTEGER, INTEGER);
static void flscroll(fltype *, INTEGER, INTEGER);
static StringPtr getdiskname(Boolean *, bool *);
static void drawjobberattop(DialogPeek);
static LONGINT getdirid(StringPtr);
static void settype(fltype *, INTEGER);
static INTEGER flwhich(fltype *, Point);
static void flmouse(fltype *, Point, ControlHandle);
static void getcurname(fltype *);
static void flfinit(fltype *);
static void flinsert(fltype *, StringPtr, INTEGER);
static int typeinarray(GUEST<OSType>, INTEGER, GUEST<SFTypeList>);
static LONGINT stdfcmp(const void *, const void *);
static void flfill(fltype *);
static void realcd(DialogPeek, LONGINT);
static LONGINT getparent(LONGINT);
static Boolean moveuponedir(DialogPtr);
static void flinit(fltype *, Rect *, ControlHandle);
static void stdfflip(Rect *, INTEGER, INTEGER);
static Boolean trackdirs(DialogPeek);
static Boolean ejected(HParmBlkPtr);
static void bumpsavedisk(DialogPtr, Boolean);
static void transformsfpdialog(DialogPtr, Point *, Rect *, Boolean);
static void doeject(DialogPtr);
static GUEST<OSType> gettypeX(StringPtr, INTEGER, LONGINT);

static INTEGER movealert(INTEGER id)
{
    Handle h;
    INTEGER dh, dv;
    Rect *rp;

    h = GetResource("ALRT"_4, id);
    if(!*h)
        LoadResource(h);
    rp = (Rect *)*h;
    dh = rp->right - rp->left;
    dv = rp->bottom - rp->top;
    rp->left = 150;
    rp->top = 30;
    rp->right = 150 + dh;
    rp->bottom = 30 + dv;
    return (Alert(id, nullptr));
}

#define SICONS -15744

static void drawminiicon(INTEGER icon)
{
    Handle h;
    BitMap bm;
    Rect r;

    h = ROMlib_getrestid("SICN"_4, SICONS);
    if(!h)
        /*-->*/ return; /* badness */
    HLock(h);
    bm.baseAddr = (Ptr)*h + icon * 16 * 16 / 8;
    bm.rowBytes = 2;
    bm.bounds.left = bm.bounds.top = 0;
    bm.bounds.right = bm.bounds.bottom = 16;
    r.top = PORT_PEN_LOC(qdGlobals().thePort).v;
    r.left = PORT_PEN_LOC(qdGlobals().thePort).h;
    r.bottom = r.top + 16;
    r.right = r.left + 16;
    CopyBits(&bm, PORT_BITS_FOR_COPY(qdGlobals().thePort),
             &bm.bounds, &r, srcCopy, nullptr);
    HUnlock(h);
}

/*
 * NOTE:  the Rect pointed to below has a normal top, left and right,
 *	  BUT the bottom is the amount to add to the top for drawing
 *	  text, not the real bottom.
 */

static void drawinboxwithicon(StringPtr str, Rect *rp, INTEGER icon)
{
    Rect r;
    INTEGER width, strlen, lengthavail;
    GUEST<INTEGER> strwidths[255], *widp;
    static const char *ellipsis = "\003...";

    /*
     * Note:  the ellipsis code is a bit shabby, not truncating at a character
     *	      boundary, nor leaving space on either side, but for now, it's
     *	      ok.
     */

    MoveTo(rp->left + 2, rp->top);
    drawminiicon(icon);
    MoveTo(rp->left + 2 + 16 + 3, rp->top + rp->bottom - 1); /* see note above */
    r.left = rp->left;
    r.right = rp->right;
    r.top = rp->top;
    r.bottom = 32766;
    ClipRect(&r);
    strlen = *str;
    MeasureText(strlen, (Ptr)(str + 1), (Ptr)strwidths);
    lengthavail = rp->right - (rp->left + 2 + 16 + 3);
    if(strwidths[strlen] > lengthavail)
    {
        width = StringWidth((StringPtr)ellipsis);
        /* 4 might be the space on the right of the ellipsis. */
        /* TODO: figure out exactly what the number is. */
        lengthavail -= (width + 4);
        widp = strwidths;
        while(*++widp < lengthavail)
            DrawChar(*++str);
        DrawString((StringPtr)ellipsis);
    }
    else
        DrawString(str);
    r.top = -32767;
    r.left = -32767;
    r.right = 32766;
    ClipRect(&r);
}

static void safeflflip(fltype *f, INTEGER sel)
{
    Rect r;
    INTEGER fltop = GetControlValue(f->flsh);

    if(sel >= fltop && sel < fltop + f->flnmlin)
    {
        r.left = f->flrect.left;
        r.right = f->flrect.right;
        r.top = f->flrect.top + (sel - fltop) * f->fllinht;
        r.bottom = r.top + f->fllinht;
        if(EmptyRgn(((WindowPeek)qdGlobals().thePort)->updateRgn)) /* stuff to draw? */
            InvertRect(&r); /* no: we can flip */
        else
            InvalRect(&r); /* yes: flip later */
    }
}

#define GRAYBIT (1 << 5)
#define ICONMASK (~GRAYBIT)

static void flupdate(fltype *f, INTEGER st, INTEGER n)
{
    INTEGER i;
    fltype::flinfostr *ip;
    INTEGER sel = f->flsel;
    INTEGER fltop;
    Rect r;

    fltop = GetControlValue(f->flsh);
    r.top = f->flrect.top + (st - fltop) * f->fllinht;
    r.bottom = f->flascent;
    r.left = f->flrect.left;
    r.right = f->flrect.right;

    ip = *f->flinfo + st;
    HLock((Handle)f->flstrs);
    for(i = st; i < st + n && i < fltop + f->flnmlin && i < f->flnmfil; i++)
    {
        drawinboxwithicon((StringPtr)(*f->flstrs + ip->floffs), &r,
                          ip->flicns & ICONMASK);
        if(ip->flicns & GRAYBIT)
        {
            r.bottom = r.top + f->fllinht;
            PenMode(notPatBic);
            PenPat(&qdGlobals().gray);
            PaintRect(&r);
            PenPat(&qdGlobals().black);
            PenMode(patCopy);
            r.bottom = f->flascent;
        }
        ip++;
        r.top = r.top + (f->fllinht);
    }
    HUnlock((Handle)f->flstrs);
    if(sel >= st && sel < st + n)
        safeflflip(f, sel);
}

static void flscroll(fltype *f, INTEGER from, INTEGER to)
{
    RgnHandle rh;
    INTEGER toscroll;

    if(from != to)
    {
        rh = NewRgn();
        ScrollRect(&f->flrect, 0, (from - to) * f->fllinht, rh);
        if(to > from)
        {
            toscroll = to - from;
            if(toscroll >= f->flnmlin)
                flupdate(f, to, f->flnmlin);
            else
                flupdate(f, from + f->flnmlin, toscroll);
        }
        else
        {
            toscroll = from - to;
            if(toscroll > f->flnmlin)
                toscroll = f->flnmlin;
            flupdate(f, to, toscroll);
        }
        DisposeRgn(rh);
    }
}

#define CTLFL(sh) \
    (guest_cast<fltype *>(((WindowPeek)(*(sh))->contrlOwner)->refCon))

/*
 * this hack is necessary because Excel 4 can bring up a dialog on top of
 * stdfile at a time when the refcon has been set to 'stdf'.
 */

static LONGINT emergency_save_ref_con;

static fltype *
WINDFL(void *dp)
{
    GUEST<LONGINT> retval;

    retval = ((WindowPeek)dp)->refCon;
    if(retval == "stdf"_4)
        return (fltype *)SYN68K_TO_US(emergency_save_ref_con);
    else
        return guest_cast<fltype *>(retval);
}

#if 0
#define WINDFL(dp) \
    ((fltype *)(long)((WindowPeek)dp)->refCon)
#endif

static void C_ROMlib_stdftrack(ControlHandle sh, INTEGER part)
{
    const uint32_t min_between_scroll_msecs = 100;
    static uint32_t last_scroll_msecs;
    uint32_t current_msecs;
    int16_t from, pg;

    current_msecs = msecs_elapsed();

    if(current_msecs - min_between_scroll_msecs < last_scroll_msecs)
        return;
    else
        last_scroll_msecs = current_msecs;

    from = GetControlValue(sh);
    pg = CTLFL(sh)->flnmlin - 1;
    switch(part)
    {
        case inUpButton:
            SetControlValue(sh, from - 1);
            break;
        case inDownButton:
            SetControlValue(sh, from + 1);
            break;
        case inPageUp:
            SetControlValue(sh, from - pg);
            break;
        case inPageDown:
            SetControlValue(sh, from + pg);
            break;
    }
    flscroll(CTLFL(sh), from, GetControlValue(sh));
}
PASCAL_FUNCTION_PTR(ROMlib_stdftrack);

static GUEST<INTEGER> cachedvrn = 32767;
static INTEGER savesel = -1;
static LONGINT oldticks = -1000;
static LONGINT lastkeydowntime = 0;
static Str255 prefix = { 0 };
static GUEST<char *> *holdstr;

void Executor::ROMlib_init_stdfile(void)
{
    cachedvrn = 32767;
    savesel = -1;
    oldticks = -1000;
    lastkeydowntime = 0;
    prefix[0] = 0;
    holdstr = 0;
}

static StringPtr getdiskname(Boolean *ejectablep, bool *writablep)
{
    static Boolean ejectable;
    static bool writable;
    static Str255 retval;
    ParamBlockRec pbr;

    if(LM(SFSaveDisk) != cachedvrn)
    {
        OSErr err;

        pbr.volumeParam.ioNamePtr = &retval[0];
        pbr.volumeParam.ioVolIndex = 0;
        pbr.volumeParam.ioVRefNum = -LM(SFSaveDisk);
        err = PBGetVInfo(&pbr, false);
        cachedvrn = LM(SFSaveDisk);
        if(err == noErr)
        {
            ejectable = !(pbr.volumeParam.ioVAtrb & VNONEJECTABLEBIT);
            writable = !(pbr.volumeParam.ioVAtrb & (VHARDLOCKBIT | VSOFTLOCKBIT));
            if(writable && pbr.volumeParam.ioVFrBlk == 0)
                writable = false;
        }
        else
        {
            warning_unexpected("surprising PBGetVInfo retval = %d\n", err);
            ejectable = false;
            writable = true;
        }
    }
    if(ejectablep)
        *ejectablep = ejectable;
    if(writablep)
        *writablep = writable;
    return retval;
}

static void drawjobberattop(DialogPeek dp)
{
    INTEGER icon;
    Rect *rp;
    fltype *flp;
    GUEST<INTEGER> savebottom;
    Boolean ejectable;
    PenState ps;

    GetPenState(&ps);
    PenNormal();
    if(LM(CurDirStore) == 2)
    {
#if 1
        /* TODO: ask cliff about a better way to do this */
        /* unused = */ getdiskname(&ejectable, nullptr);
        icon = ejectable ? MICONFLOPPY : MICONDISK;
#else /* 0 */
        icon = MICONDISK;
#endif /* 0 */
    }
    else
        icon = MICONOFOLDER;
    flp = WINDFL(dp);
    rp = &flp->flcurdirrect;
    savebottom = rp->bottom;
    rp->bottom = flp->flascent;
    rp->left = rp->left + (2);
    drawinboxwithicon(flp->flcurdirname, rp, icon);
    rp->left = rp->left - (2);
    rp->bottom = savebottom;
    FrameRect(rp);
    MoveTo(rp->left + 1, rp->bottom);
    LineTo(rp->right, rp->bottom);
    LineTo(rp->right, rp->top + 1);
    SetPenState(&ps);
}

static LONGINT getdirid(StringPtr fname)
{
    CInfoPBRec hpb;
    LONGINT retval;
    OSErr err;

    hpb.dirInfo.ioCompletion = 0;
    hpb.dirInfo.ioNamePtr = fname;
    hpb.dirInfo.ioVRefNum = -LM(SFSaveDisk);
    hpb.dirInfo.ioFDirIndex = 0;
    hpb.dirInfo.ioDrDirID = LM(CurDirStore);
    err = PBGetCatInfo(&hpb, false);
    if(err == noErr)
        retval = hpb.dirInfo.ioDrDirID;
    else
    {
        warning_unexpected("PBGetCatInfo return value err = %d\n", err);
        retval = 2; /* known good id ... an error might be better here */
    }
    return retval;
}

#define MAXPREFIX 64

static void
set_type_and_name(fltype *f, OSType type, Str255 name)
{
    switch(f->flavor)
    {
        case original_sf:
            f->flreplyp.oreplyp->fType = type;
            str31assign(f->flreplyp.oreplyp->fName, name);
            break;
        case new_sf:
        case new_custom_sf:
            f->flreplyp.nreplyp->sfType = type;
            str31assign(f->flreplyp.nreplyp->sfFile.name, name);
            f->flreplyp.nreplyp->sfIsFolder = !!type;
            break;
        default:
            warning_unexpected("flavor = %d", f->flavor);
            break;
    }
}

static void settype(fltype *f, INTEGER newsel)
{
    StringPtr ip;

    ip = (StringPtr)*f->flstrs + (*f->flinfo)[newsel].floffs;
    if((*f->flinfo)[newsel].flicns == MICONCFOLDER)
        set_type_and_name(f, getdirid(ip), (StringPtr) "");
    else
        set_type_and_name(f, 0, ip);
}

static INTEGER flwhich(fltype *f, Point p)
{
    INTEGER retval;
    INTEGER bump, from;

    if(!PtInRect(p, &f->flrect))
    {
        bump = 0;
        if(p.v < f->flrect.top)
            bump = -1;
        else if(p.v >= f->flrect.bottom)
            bump = 1;
        if(bump)
        {
            from = GetControlValue(f->flsh);
            SetControlValue(f->flsh, from + bump);
            flscroll(f, from, GetControlValue(f->flsh));
        }
        /*-->*/ return (-1);
    }
    retval = (p.v - f->flrect.top) / f->fllinht + GetControlValue(f->flsh);
    if(retval >= f->flnmfil || (*f->flinfo)[retval].flicns & GRAYBIT)
        /*-->*/ retval = -1;
    return (retval);
}

static void flmouse(fltype *f, Point p, ControlHandle ch)
{
    INTEGER newsel;
    EventRecord evt;

    evt.where.set(p);
    do
    {
        GlobalToLocal(&evt.where);
        p = evt.where.get();
        if((newsel = flwhich(f, p)) != f->flsel)
        {
            if(f->flsel != -1)
            {
                safeflflip(f, f->flsel);
                if(newsel == -1)
                {
                    if(!f->flgraynondirs)
                        HiliteControl(ch, 255);
                    set_type_and_name(f, 0, (StringPtr) "");
                }
            }
            if(newsel != -1)
            {
                safeflflip(f, newsel);
                if(f->flsel == -1)
                    HiliteControl(ch, 0);
                settype(f, newsel);
            }
            f->flsel = newsel;
        }
    } while(!GetNextEvent(mUpMask, &evt));
}

static void getcurname(fltype *f)
{
    int w;
    CInfoPBRec hpb;
    Rect *r;
    OSErr err;

    hpb.dirInfo.ioCompletion = 0;
    hpb.dirInfo.ioNamePtr = &f->flcurdirname[0];
    f->flcurdirname[0] = 0;
    hpb.dirInfo.ioVRefNum = -LM(SFSaveDisk);
    hpb.dirInfo.ioFDirIndex = -1;
    hpb.dirInfo.ioDrDirID = LM(CurDirStore);
    err = PBGetCatInfo(&hpb, false);
    if(err != noErr)
    {
        warning_unexpected("PBGetCatInfo err = %d\n", err);
        str255_from_c_string(f->flcurdirname, "?error?");
    }
    r = &f->flrect;
    w = StringWidth(f->flcurdirname) + 2 + 16 + 3 + 2 + 2 + 4;
#if 1
    if(w > r->right - r->left + 17)
    {
        f->flcurdirrect.left = r->left;
        f->flcurdirrect.right = r->right + 17;
    }
    else
    {
        f->flcurdirrect.left = (r->left + r->right + 17 - w) / 2 - 2;
        f->flcurdirrect.right = f->flcurdirrect.left + w;
    }
#else /* 1 */
    f->flcurdirrect.left = (r->left + r->right + 17 - w) / 2 - 2;
    f->flcurdirrect.right = f->flcurdirrect.left + w;
#endif /* 1 */
}

static void flfinit(fltype *fp)
{
    DisposeControl(fp->flsh);
    DisposeHandle((Handle)fp->flinfo);
    DisposeHandle((Handle)fp->flstrs);
}

static void flinsert(fltype *f, StringPtr p, INTEGER micon)
{
    fltype::flinfostr finfo;

    finfo.floffs = GetHandleSize((Handle)f->flstrs);
    finfo.flicns = micon;
    PtrAndHand((Ptr)p, (Handle)f->flstrs, (LONGINT)p[0] + 1);
    PtrAndHand((Ptr)&finfo, (Handle)f->flinfo,
               (LONGINT)sizeof(fltype::flinfostr));
    ++f->flnmfil;
}

static int typeinarray(GUEST<OSType> ft, INTEGER numt, GUEST<SFTypeList> tl)
{
    GUEST<OSType> *ostp = tl;

    while(numt--)
        if(ft == *ostp++)
            /*-->*/ return (true);
    return (false);
}

/* ip1 and ip2 would really be "void *" in ANSI C */

static LONGINT stdfcmp(const void *ip1, const void *ip2)
{
    fltype::flinfostr *fp1 = (fltype::flinfostr *)ip1;
    fltype::flinfostr *fp2 = (fltype::flinfostr *)ip2;
    LONGINT retval;

    retval = ROMlib_strcmp((StringPtr)(*holdstr + fp1->floffs),
                           (StringPtr)(*holdstr + fp2->floffs));
    return retval;
}

static bool
passes_filter(fltype *f, CInfoPBRec *cinfop, INTEGER numt)
{
    bool retval;

    ROMlib_hook(stdfile_filefiltnumber);
    switch(f->flavor)
    {
        case original_sf:
            /* NOTE: the code for original_sf was changed after we ran into
	 some trouble with Accordance.  It's not clear whether or not
	 the new code here should also be used for new_sf.  */
            retval = (!f->flfilef.oflfilef
                      || (numt == 0
                          && (cinfop->hFileInfo.ioFlAttrib & ATTRIB_ISADIR))
                      || !f->flfilef.oflfilef(cinfop));
        case new_sf:
            retval = (!f->flfilef.oflfilef
                      || (cinfop->hFileInfo.ioFlAttrib & ATTRIB_ISADIR)
                      || !f->flfilef.oflfilef(cinfop));
            break;
        case new_custom_sf:
            retval = (!f->flfilef.cflfilef
                      || (cinfop->hFileInfo.ioFlAttrib & ATTRIB_ISADIR)
                      || !f->flfilef.cflfilef(cinfop, f->mydata));
            break;
        default:
            warning_unexpected("flavor = %d", f->flavor);
            retval = true;
            break;
    }
    return retval;
}

static void flfill(fltype *f)
{
    CInfoPBRec pb;
    OSErr err;
    Str255 s;
    int micon;
    CursHandle watchh;
    INTEGER errcount;
    INTEGER dirindex;

    SetCursor(*(watchh = GetCursor(watchCursor)));

    pb.hFileInfo.ioNamePtr = &s[0];
    pb.hFileInfo.ioVRefNum = -LM(SFSaveDisk);
    err = noErr;
    errcount = 0;
    for(dirindex = 1; err != fnfErr && errcount != 3; dirindex++)
    {
        pb.hFileInfo.ioFDirIndex = dirindex;
        pb.hFileInfo.ioDirID = LM(CurDirStore);
        err = PBGetCatInfo(&pb, false);
        if(err)
        {
            if(err != fnfErr)
            {
                warning_unexpected("PBGetCatInfo err = %d\n", err);
                ++errcount;
                /* int d7 = err; */
            }
        }
        else
        {
            errcount = 0;
            if(f->flnumt <= 0 || (pb.hFileInfo.ioFlAttrib & ATTRIB_ISADIR) || typeinarray(pb.hFileInfo.ioFlFndrInfo.fdType, f->flnumt,
                                                                                          f->fltl))
                if(passes_filter(f, &pb, f->flnumt))
                {
                    if(pb.hFileInfo.ioFlAttrib & ATTRIB_ISADIR)
                        micon = MICONCFOLDER;
                    else if(pb.hFileInfo.ioFlFndrInfo.fdType == "APPL"_4)
                        micon = MICONAPP | f->flgraynondirs;
                    else
                        micon = MICONLETTER | f->flgraynondirs;
                    flinsert(f, pb.hFileInfo.ioNamePtr, micon);
                }
        }
    }
    if(f->flnmfil > f->flnmlin)
    {
        SetControlMaximum(f->flsh, f->flnmfil - f->flnmlin);
        SetControlValue(f->flsh, 0);
    }
    else
        SetControlMaximum(f->flsh, 0);

    if(f->flnmfil > 0)
    {
        holdstr = f->flstrs;
        ::qsort(*f->flinfo, f->flnmfil, sizeof(fltype::flinfostr), stdfcmp);
        if(!((*f->flinfo)[0].flicns & GRAYBIT) && !f->flgraynondirs)
        {
            f->flsel = 0;
            settype(f, 0);
            HiliteControl(f->flch, 0);
        }
    }
    else if(!f->flgraynondirs)
        HiliteControl(f->flch, 255);
    /* i'm not sure this is right, but there is no obvious way to
       save/restore the cursor */
    SetCursor(&qdGlobals().arrow);
}

/*
 * ROMlib_filebox should be renamed to reflect the fact that it also draws the
 * dotted line (whopee!)
 */

static void C_ROMlib_filebox(DialogPtr dlg, INTEGER which)
{
    DialogPeek dp = (DialogPeek)dlg;
    GUEST<Handle> h;
    Rect r, r2;
    GUEST<INTEGER> i;
    int width, strwidth, offset;
    StringPtr diskname;
    GUEST<Handle> tmpH;
    Handle ejhand;
    Boolean ejectable;
    PenState ps;

    GetPenState(&ps);
    PenNormal();

    h = nullptr;
    GetDialogItem((DialogPtr)dp, which, &i, &h, &r);
    /*    h = h; we don't really use h */
    switch(which)
    {
        case getNmList:
        case putNmList:
            if(h)
                FrameRect(&r);
            flupdate(WINDFL(dp), GetControlValue(WINDFL(dp)->flsh),
                     WINDFL(dp)->flnmlin);
            break;
        case getDotted:
            FillRect(&r, &qdGlobals().gray);
            break;
        case getDiskName:
            /*  case putDiskName:	getDiskName and putDiskName are the same */
            EraseRect(&r);
            width = r.right - r.left;
            diskname = getdiskname(&ejectable, nullptr);
            GetDialogItem((DialogPtr)dp, putEject, &i, &tmpH, &r2);
            ejhand = tmpH;
            if(ejectable)
                HiliteControl((ControlHandle)ejhand, 0);
            else
                HiliteControl((ControlHandle)ejhand, 255);
            strwidth = StringWidth(diskname) + 2 + 16 + 3;
            offset = (width - strwidth) / 2;
            if(offset < 3)
                r.left = r.left + (3);
            else
                r.left = r.left + (offset);
            r.bottom = WINDFL(dp)->flascent;
            drawinboxwithicon(diskname, &r, ejectable ? MICONFLOPPY : MICONDISK);
            break;
    }
    SetPenState(&ps);
}
PASCAL_FUNCTION_PTR(ROMlib_filebox);

static void realcd(DialogPeek dp, LONGINT dir)
{
    fltype *fp;

    LM(CurDirStore) = dir;
    fp = WINDFL(dp);
    SetHandleSize((Handle)fp->flinfo, (Size)0);
    SetHandleSize((Handle)fp->flstrs, (Size)0);
    fp->flnmfil = 0;
    fp->flsel = -1;
    set_type_and_name(fp, 0, (StringPtr) "");
    flfill(fp);
    fp->flcurdirrect.right = fp->flcurdirrect.right + 1;
    fp->flcurdirrect.bottom = fp->flcurdirrect.bottom + 1;
    EraseRect(&fp->flcurdirrect);
    getcurname(fp);
    fp->flcurdirrect.bottom = fp->flcurdirrect.bottom - 1;
    /* don't need to do right; getcurname does */
    drawjobberattop(dp);
    C_ROMlib_filebox((DialogPtr)dp, getDiskName);
    EraseRect(&fp->flrect);
    if(fp->flgraynondirs)
        C_ROMlib_filebox((DialogPtr)dp, putNmList);
    else
        C_ROMlib_filebox((DialogPtr)dp, getNmList);
}

static LONGINT getparent(LONGINT dirid)
{
    OSErr err;
    CInfoPBRec cb;
    LONGINT retval;

    cb.dirInfo.ioCompletion = 0;
    cb.dirInfo.ioNamePtr = 0;
    cb.dirInfo.ioVRefNum = -LM(SFSaveDisk);
    cb.dirInfo.ioFDirIndex = -1;
    cb.dirInfo.ioDrDirID = dirid;
    err = PBGetCatInfo(&cb, false);
    if(err == noErr)
        retval = cb.dirInfo.ioDrParID;
    else
    {
        warning_unexpected("PBGetCatInfo return = %d\n", err);
        retval = 2;
    }
    return retval;
}

static Boolean findparent(GUEST<INTEGER> *vrefp, GUEST<LONGINT> *diridp)
{
#if 1
    /*
        This function is the counterpart to `unixcore`.
        It finds the parent of a mount point,
        whereas `getparent()` above finds the parent directory within a filesystem.

        Yay for function names. ;-)

        See also the comment in `unixcore`.
     */

    return false;
#else
    HVCB *vcbp;
    Boolean retval;
    struct stat sbuf;
    char *namecpy, *slashp;
    INTEGER namelen;

    retval = false;
    vcbp = ROMlib_vcbbyvrn(*vrefp);
    if(((VCBExtra *)vcbp)->unixname)
    {
        namelen = strlen(((VCBExtra *)vcbp)->unixname);
        if(namelen != 1 + SLASH_CHAR_OFFSET)
        { /* i.e. "/" */
            namecpy = (char *)alloca(namelen + 1);
            strcpy(namecpy, ((VCBExtra *)vcbp)->unixname);
            slashp = strrchr(namecpy, '/');
            if(slashp == namecpy + SLASH_CHAR_OFFSET)
            {
                slashp[1] = 0;
            }
            else
            {
                slashp[0] = 0;
            }
            if(Ustat(namecpy, &sbuf) == 0 && (vcbp = ROMlib_vcbbybiggestunixname(namecpy)))
            {
                *vrefp = vcbp->vcbVRefNum;
                *diridp = (LONGINT)ST_INO(sbuf);
                retval = true;
            }
        }
    }
    return retval;
#endif
}

static Boolean moveuponedir(DialogPtr dp)
{
    LONGINT parent;
    GUEST<INTEGER> vrn;
    Boolean retval;

    parent = getparent(LM(CurDirStore));
    if(parent != LM(CurDirStore) && parent != 1)
    {
        LM(CurDirStore) = parent;
        retval = true;
    }
    else
    {
        vrn = -LM(SFSaveDisk);
        retval = findparent(&vrn, &LM(CurDirStore));
        LM(SFSaveDisk) = -vrn;
    }
    return retval;
}

Boolean keyarrow(fltype *fl, INTEGER incr) /* -1: up, 1: down */
{
    INTEGER nsel, oldval, newval;
    fltype::flinfostr *flp;
    /*
 * If there's no selection we start at the top or bottom, depending on
 * whether we're going down or p
 */
    if(fl->flsel == -1)
        nsel = incr > 0 ? 0 : fl->flnmfil - 1;
    else
        nsel = fl->flsel + incr;

    /*
 * If we land on a grayed out file, advance until non-grayed
 */

    for(flp = (*fl->flinfo) + nsel;
        (flp->flicns & GRAYBIT) && nsel >= 0 && nsel < fl->flnmfil;
        nsel += incr, flp += incr)
        ;

    /*
 * If we don't find a non-grayed out entry, we leave
 */
    if(nsel < 0 || nsel >= fl->flnmfil)
        /*-->*/ return false;

    /*
 * Figure out what should be visible and scroll there if necessary
 */

    newval = oldval = GetControlValue(fl->flsh);
    if(nsel < oldval)
        newval = nsel;
    else if(nsel >= oldval + fl->flnmlin)
        newval = nsel - fl->flnmlin + 1;
    if(newval != oldval)
    {
        SetControlValue(fl->flsh, newval);
        flscroll(fl, oldval, newval);
    }

    /*
 * Get rid of the previously selected rectangle
 */
    if(fl->flsel != -1)
        safeflflip(fl, fl->flsel);
    /*
 * Flip the new rectangle, set up the type information and make assignment
 */
    safeflflip(fl, nsel);
    settype(fl, nsel);
    fl->flsel = nsel;
    return true;
}

static bool
folder_selected_p(fltype *fl)
{
    bool retval;

    switch(fl->flavor)
    {
        case original_sf:
            retval = !!fl->flreplyp.oreplyp->fType;
            break;
        case new_sf:
        case new_custom_sf:
            retval = fl->flreplyp.nreplyp->sfIsFolder;
            break;
        default:
            warning_unexpected("flavor = %d", fl->flavor);
            retval = false;
            break;
    }

    return retval;
}

static Boolean
call_magicfp(fltype *fl, DialogPtr dp, EventRecord *evt, GUEST<INTEGER> *ith)
{
    Boolean retval;
    LONGINT save_ref_con;

    save_ref_con = GetWRefCon(dp);
    SetWRefCon(dp, "stdf"_4);
    ROMlib_hook(stdfile_filtnumber);

    switch(fl->flavor)
    {
        case original_sf:
        case new_sf:
            retval = fl->magicfp.ofilterp
                ? fl->magicfp.ofilterp(dp, evt, ith)
                : false;
            break;
        case new_custom_sf:
            retval = fl->magicfp.cfilterp
                ? fl->magicfp.cfilterp(dp, evt, ith, fl->mydata)
                : false;
            break;
        default:
            warning_unexpected("flavor = %d", fl->flavor);
            retval = false;
    }

    SetWRefCon(dp, save_ref_con);

    return retval;
}

#define keydownbit 0x1000

static Boolean C_ROMlib_stdffilt(DialogPtr dlg, EventRecord *evt,
                                    GUEST<INTEGER> *ith) /* handle disk insert */
{
    DialogPeek dp = (DialogPeek)dlg;
    LONGINT ticks;
    GUEST<INTEGER> i;
    INTEGER from;
    ControlHandle h;
    Rect r;
    GUEST<Point> gp;
    Point p;
    INTEGER t;
    fltype *fl;
    INTEGER opentoken;
    fltype::flinfostr *flp, *flep;
    INTEGER nsel, fltop;
    INTEGER part;
    INTEGER retval, retval2;
    GUEST<Handle> tmpH;

    fl = WINDFL(dp);
    opentoken = getOpen; /* getOpen and putSave are both 1 */
    retval = false;
    switch(evt->what)
    {
        case keyDown:
            *ith = (evt->message & 0xFF) + keydownbit;
            switch(evt->message & 0xFF)
            {
                case NUMPAD_ENTER:
                case '\r':
                    GetDialogItem((DialogPtr)dp, dp->aDefItem, &i, &tmpH, &r);
                    h = (ControlHandle)tmpH;
                    if((*h)->contrlVis && (*h)->contrlHilite != 255)
                    {
                        prefix[0] = 0;
                        oldticks = -1000;
                        *ith = opentoken;
                        retval = -1;
                        HiliteControl(h, inButton);
                        Delay((LONGINT)5, nullptr);
                        HiliteControl(h, 0);
                    }
                    break;
                case ASCIIUPARROW:
                    if(evt->modifiers & cmdKey)
                        *ith = getDiskName; /* the same as putDiskName */
                    else
                        keyarrow(fl, -1);
                    retval = -1;
                    break;
                case ASCIIDOWNARROW:
                    if((evt->modifiers & cmdKey) && fl->flsel != -1 && ((*fl->flinfo) + fl->flsel)->flicns == MICONCFOLDER)
                    {
                        prefix[0] = 0;
                        oldticks = -1000;
                        *ith = opentoken;
                    }
                    else
                        keyarrow(fl, 1);
                    retval = -1;
                    break;
                case '\t':
                    *ith = putDrive; /* PutDrive is 6 which is also getDrive. */
                    retval = -1;
                    break;
                case '.':
                    if(evt->modifiers & cmdKey)
                    {
                        *ith = fl->fl_cancel_item;
                        retval = -1;
                        break;
                    }
                default:
                    /*
 * The dp->editField check was made to get HFS_XFer to work.  There
 * may be a better place to put it, but not enough tests have been done
 * to say where.
 */
                    if(!fl->flgraynondirs && dp->editField == -1)
                    {
                        flep = (*fl->flinfo) + fl->flnmfil - 1;
                        if(evt->when > lastkeydowntime + LM(DoubleTime))
                        {
                            flp = *fl->flinfo;
                            prefix[0] = 0;
                            oldticks = -1000;
                        }
                        else
                            flp = (*fl->flinfo) + ((fl->flsel) == -1 ? 0 : fl->flsel);
                        lastkeydowntime = evt->when;
                        prefix[++prefix[0]] = evt->message & 0xff;
                        while(flp < flep && RelString((StringPtr)((*fl->flstrs) + flp->floffs), prefix, false, true) < 0)
                            ++flp;
                        nsel = flp - (*fl->flinfo);
                        if(nsel != fl->flsel)
                        {
                            safeflflip(fl, fl->flsel);
                            fltop = GetControlValue(fl->flsh);
                            if(nsel < fltop || nsel >= fltop + fl->flnmlin)
                            {
                                SetControlValue(fl->flsh, nsel - fl->flnmlin / 2);
                                flscroll(fl, fltop, GetControlValue(fl->flsh));
                            }
                            safeflflip(fl, fl->flsel = nsel);
                            settype(fl, nsel);
                        }
                        retval = -1;
                    }
                    break;
            }
            break;
        case mouseDown:
            gp = evt->where;
            GlobalToLocal(&gp);
            p = gp.get();
            if(PtInRect(p, &fl->flrect))
            {
                GUEST<Handle> h_s;
                GetDialogItem((DialogPtr)dp, getOpen, &i, &h_s, &r);
                h = (ControlHandle)h_s;
                flmouse(fl, evt->where.get(), h);
                ticks = TickCount();
                if(fl->flsel != -1 && savesel == fl->flsel && (ticks < oldticks + LM(DoubleTime)))
                {
                    prefix[0] = 0;
                    *ith = opentoken;
                    oldticks = -1000;
                    retval = -1;
                }
                else
                    oldticks = ticks;
                savesel = fl->flsel;
            }
            else if((t = TestControl(fl->flsh, p)))
            {
                if(t == inThumb)
                {
                    from = GetControlValue(fl->flsh);
                    TrackControl(fl->flsh, p, nullptr);
                    flscroll(fl, from, GetControlValue(fl->flsh));
                }
                else
                    TrackControl(fl->flsh, p, &ROMlib_stdftrack);
            }
            else if(PtInRect(p, &fl->flcurdirrect))
            {
                *ith = sfHookFolderPopUp;
                retval = -1;
            }
            else
            {
                GUEST<Handle> h_s;
                GetDialogItem((DialogPtr)dp, getOpen, &i, &h_s, &r);
                h = (ControlHandle)h_s;
                if((part = TestControl(h, p)) && TrackControl(h, p, nullptr))
                {
                    prefix[0] = 0;
                    oldticks = -1000;
                    *ith = opentoken;
                    retval = -1;
                }
            }
            break;
        case nullEvent:
            *ith = sfHookNullEvent;
            retval = -1;
            break;
        case updateEvt:
            if(guest_cast<DialogPeek>(evt->message) == dp)
                drawjobberattop(dp);
            *ith = sfHookNullEvent;
            break;
    }
    retval2 = call_magicfp(fl, (DialogPtr)dp, evt, ith);

    if(*ith == getOpen && folder_selected_p(fl)) /* 1 is getOpen and putSave */
        *ith = sfHookOpenFolder;

    return retval ? retval : retval2;
}
PASCAL_FUNCTION_PTR(ROMlib_stdffilt);

static void flinit(fltype *f, Rect *r, ControlHandle sh)
{
    FontInfo fi;
    GUEST<THz> savezone;

    GetFontInfo(&fi);

    f->flsh = sh;
    f->flrect = *r;
    f->fllinht = fi.ascent + fi.descent + fi.leading;

    f->flcurdirrect.top = r->top - f->fllinht - 5;
    f->flcurdirrect.bottom = f->flcurdirrect.top + f->fllinht;
    getcurname(f);

    f->flascent = fi.ascent;
    f->flnmlin = (r->bottom - r->top) / f->fllinht;
    f->flnmfil = 0;
    f->flsel = -1;
    savezone = LM(TheZone);
    LM(TheZone) = LM(SysZone);
    f->flinfo = (GUEST<fltype::flinfostr *> *)NewHandle((Size)0);
    f->flstrs = (GUEST<char *> *)NewHandle((Size)0);
    LM(TheZone) = savezone;
}

static void stdfflip(Rect *rp, INTEGER n, INTEGER height)
{
    GUEST<INTEGER> savetop = rp->top;

    rp->top = rp->top + (n * height + 1);
    rp->bottom = rp->top + height - 2;
    InvertRect(rp);
    rp->top = savetop;
}

static Boolean trackdirs(DialogPeek dp)
{
    WRAPPER_PIXMAP_FOR_COPY(wrapper);
    PixMapHandle save_bits;
    Rect therect, fillinrect;
    struct link
    {
        Str255 name;
        LONGINT dirid;
        struct link *next;
        INTEGER vrefnum;
    } first, *next;
    int count, i;
    CInfoPBRec hpb;
    int max_width;
    fltype *fl;
    EventRecord evt;
    LONGINT id;
    int sel, newsel, firstsel;
    Boolean done;
    ALLOCABEGIN
    Boolean ejectable;
    bool seen_up_already;
    TEMP_ALLOC_DECL(temp_save_bits);

    {
        ThePortGuard guard(wmgr_port);
        int str_width;

        next = &first;
        hpb.dirInfo.ioCompletion = 0;
        hpb.dirInfo.ioNamePtr = &next->name[0];
        next->name[0] = 0;
        hpb.dirInfo.ioVRefNum = -LM(SFSaveDisk);
        hpb.dirInfo.ioFDirIndex = -1;
        hpb.dirInfo.ioDrDirID = LM(CurDirStore);
        max_width = 0;
        count = 0;
        done = false;

        do
        {
            OSErr err;

            err = PBGetCatInfo(&hpb, false);
            if(err != noErr)
            {
                warning_unexpected("PBGetCatInfo returns err = %d\n", err);
                done = true;
            }
            id = next->dirid = hpb.dirInfo.ioDrDirID;
            next->vrefnum = hpb.dirInfo.ioVRefNum;
            next->next = (struct link *)ALLOCA(sizeof(struct link));
            gui_assert(next->next);
            str_width = StringWidth(next->name);
            if(str_width > max_width)
                max_width = str_width;
            next = next->next; /* make Steve Jobs happy */
            hpb.dirInfo.ioDrDirID = hpb.dirInfo.ioDrParID;
            hpb.dirInfo.ioNamePtr = &next->name[0];
            next->name[0] = 0;
            count++;
            if(id == 2)
            {
                if(!findparent(&hpb.dirInfo.ioVRefNum, &hpb.dirInfo.ioDrDirID))
                    done = true;
            }
        } while(!done);
        fl = WINDFL(dp);
        therect.top = fl->flcurdirrect.top
                         - PORT_BOUNDS(dp).top;
        therect.left = fl->flcurdirrect.left
                          - PORT_BOUNDS(dp).left;
        therect.bottom = therect.top + count * fl->fllinht + 1;
        therect.right = therect.left + 2 + 16 + 3 + max_width + 4 + 2 + 3;
        ClipRect(&therect);

        {
            Rect *bounds;
            PixMapHandle port_pixmap;
            GUEST<INTEGER> save_bpp_x;
            int row_bytes;
            void *save_bits_mem;

            save_bits = NewPixMap();
            port_pixmap = CPORT_PIXMAP(qdGlobals().thePort);

            bounds = &PIXMAP_BOUNDS(save_bits);
            bounds->top = 0;
            bounds->left = 0;
            bounds->bottom = RECT_HEIGHT(&therect);
            bounds->right = RECT_WIDTH(&therect);
            PIXMAP_PIXEL_SIZE(save_bits) = save_bpp_x
                = PIXMAP_PIXEL_SIZE(port_pixmap);
            ROMlib_copy_ctab(PIXMAP_TABLE(port_pixmap),
                             PIXMAP_TABLE(save_bits));
            row_bytes = ((bounds->right * save_bpp_x + 31) / 32) * 4;
            PIXMAP_SET_ROWBYTES(save_bits, row_bytes);

            /* Allocate potentially large temporary pixmap space. */
            TEMP_ALLOC_ALLOCATE(save_bits_mem, temp_save_bits,
                                bounds->bottom * row_bytes);
            PIXMAP_BASEADDR(save_bits) = (Ptr)save_bits_mem;
            WRAPPER_SET_PIXMAP(wrapper, save_bits);

            CopyBits(PORT_BITS_FOR_COPY(qdGlobals().thePort), wrapper,
                     &therect, bounds, srcCopy, nullptr);
        }

        EraseRect(&therect);

        /* loop through, displaying stuff */
        /* highlite the appropriate box, etc. */

        fillinrect.top = therect.top;
        fillinrect.left = therect.left + 2;
        fillinrect.bottom = fl->flascent;
        fillinrect.right = therect.right;

        for(i = count, next = &first; --i >= 0; next = next->next)
        {

            /* TODO: ask cliff about a better way to do this */
            /* unused = */
            getdiskname(&ejectable, nullptr);
            drawinboxwithicon(next->name, &fillinrect,
                              i ? MICONCFOLDER : ejectable ? MICONFLOPPY : MICONDISK);
            /*
	   drawinboxwithicon(next->name, &fillinrect,
			     i ? MICONCFOLDER : MICONDISK);
*/
            fillinrect.top = fillinrect.top + (fl->fllinht);
        }

        therect.right = therect.right - (1);
        therect.bottom = therect.bottom - (1);
        FrameRect(&therect);
        MoveTo(therect.left + 1, therect.bottom);
        LineTo(therect.right, therect.bottom);
        LineTo(therect.right, therect.top + 1);
        therect.right = therect.right + (1);

        sel = 0;
        fillinrect.top = therect.top;
        fillinrect.left = fillinrect.left - 1;
        fillinrect.right = fillinrect.right - 2;
        stdfflip(&fillinrect, sel, fl->fllinht);
        done = false;
        seen_up_already = false;
        firstsel = -1;
        while(!done)
        {
            Point p = evt.where.get();

            if(PtInRect(p, &therect))
                newsel = (p.v - therect.top) / fl->fllinht;
            else
                newsel = -1;
            if(newsel != sel)
            {
                if(firstsel == -1)
                    firstsel = newsel;
                if(sel != -1)
                    stdfflip(&fillinrect, sel, fl->fllinht);
                if(newsel != -1)
                    stdfflip(&fillinrect, newsel, fl->fllinht);
                sel = newsel;
            }
            if(!ROMlib_sticky_menus_p)
                done = GetNextEvent(mUpMask, &evt);
            else
            {
                if(OSEventAvail(mUpMask, &evt))
                {
                    if(seen_up_already || (sel != firstsel && firstsel != -1))
                        done = true;
                    else
                    {
                        GetOSEvent(mUpMask, &evt);
                        seen_up_already = true;
                    }
                }
                if(!done && OSEventAvail(mDownMask, &evt))
                {
                    GetOSEvent(mDownMask, &evt);
                    done = sel != -1;
                }
            }
        }

        therect.bottom = therect.bottom + 1;

        /* restore the rect and clean up after ourselves */
        CopyBits(wrapper, PORT_BITS_FOR_COPY(qdGlobals().thePort),
                 &PIXMAP_BOUNDS(save_bits), &therect, srcCopy, nullptr);
        DisposePixMap(save_bits);
    }
    if(sel != -1)
    {
        for(i = 0, next = &first; i != sel; ++i, next = next->next)
            ;
        LM(CurDirStore) = next->dirid;
        LM(SFSaveDisk) = -next->vrefnum;
        return true;
    }
    ALLOCAEND

    TEMP_ALLOC_FREE(temp_save_bits);

    return false;
}

static void
makeworking(fltype *f)
{

    switch(f->flavor)
    {
        case original_sf:
        {
            WDPBRec wdpb;
            OSErr err;

            wdpb.ioVRefNum = -LM(SFSaveDisk);
            wdpb.ioWDDirID = LM(CurDirStore);
            wdpb.ioWDProcID = "STDF"_4;
            wdpb.ioNamePtr = 0;
            err = PBOpenWD(&wdpb, false);
            if(err != noErr)
                warning_unexpected("PBOpenWD returns %d\n", err);

            f->flreplyp.oreplyp->vRefNum = wdpb.ioVRefNum;
        }
        break;
        case new_sf:
        case new_custom_sf:
            f->flreplyp.nreplyp->sfFile.vRefNum = -LM(SFSaveDisk);
            f->flreplyp.nreplyp->sfFile.parID = LM(CurDirStore);
            break;
        default:
            warning_unexpected("flavor = %d", f->flavor);
            break;
    }
}

static Boolean ejected(HParmBlkPtr pb)
{
    return pb->volumeParam.ioVDrvInfo == 0;
}

/*
 * returns true if the filesystem that pb refers to is part of a single
 * tree, as in UNIX.  Under MSDOS, filesystems are distinct (i.e. C:, D:,
 * etc.)
 */

static bool single_tree_fs_p(HParmBlkPtr pb)
{
#if 1
    return false;
#else
    HVCB *vcbp = ROMlib_vcbbyvrn(pb->volumeParam.ioVRefNum);

    return vcbp && ((VCBExtra *)vcbp)->unixname;
#endif
}


static void bumpsavedisk(DialogPtr dp, Boolean always)
{
    GUEST<INTEGER> current;
    HParamBlockRec pb;
    INTEGER vref;
    OSErr err;
    Boolean is_single_tree_fs, seenus;

    pb.volumeParam.ioVRefNum = -LM(SFSaveDisk);
    pb.volumeParam.ioNamePtr = 0;
    pb.volumeParam.ioVolIndex = 0;
    err = PBHGetVInfo(&pb, false);
    if(err != noErr)
        warning_unexpected("PBHGetVInfo returns %d", err);
    else if(!LM(SFSaveDisk) || ISWDNUM(-LM(SFSaveDisk)))
        LM(SFSaveDisk) = -pb.volumeParam.ioVRefNum;
    if(always || pb.ioParam.ioResult != noErr || ejected(&pb))
    {
        current = pb.volumeParam.ioVRefNum;
        is_single_tree_fs = single_tree_fs_p(&pb);
        pb.volumeParam.ioVolIndex = 0;
        seenus = false;
        vref = 0;
        do
        {
            pb.volumeParam.ioVolIndex = pb.volumeParam.ioVolIndex + 1;
            err = PBHGetVInfo(&pb, false);
            if(err != noErr && !seenus)
                warning_unexpected("PBHGetVInfo = %d\n", err);
            else
            {
                if(pb.volumeParam.ioVRefNum == current)
                    seenus = true;
                else if(!ejected(&pb) && (!is_single_tree_fs || !single_tree_fs_p(&pb)))
                {
                    if(!vref || seenus)
                        vref = pb.volumeParam.ioVRefNum;
                    if(seenus)
                        /*-->*/ break;
                }
            }
        } while(err == noErr);
        if(vref)
        {
            LM(SFSaveDisk) = -vref;
            LM(CurDirStore) = 2;
        }
    }
}

/*
 * NOTE: we no longer swap out the refcon when making the sfHookFirstCall (-1)
 *       or the sfHookLastCall (-2).  This fixes the bug that had Photoshop 3.0
 *	 dying the second time that SFCustomGetFile was called.  This fix
 *	 was determined by trial and error.
 *
 * I have just verified that the fix is incorrect.  It works because we
 * bypass some startup code that Photoshop 3.0 uses to set up its type list.
 * This means that the type list won't work properly.  However if we don't
 * swap in 'stdf' then we wind up making calls to InsertMenuItem that add
 * entries to the "Format" menu which somehow results in a crash eventually.
 * If we have ROMlib_CALLDHOOK temporarily disable InsertMenuItem then PS3.0
 * will work even w/ 'stdf' in the refcon.  It will also work if we rig
 * CountMItems to always return 0 inside ROMlib_CALLDHOOK.  So something
 * appears to go wrong if we call the startup code and actually add (and
 * notice that we add) items the Format menu.
 */

static INTEGER
ROMlib_CALLDHOOK(fltype *fl, INTEGER ihit, DialogPtr dp, dialog_hook_u dhu)
{
    INTEGER retval;
    LONGINT save_ref_con;

    ROMlib_hook(stdfile_dialhooknumber);

    save_ref_con = GetWRefCon(dp);
    emergency_save_ref_con = save_ref_con;
    SetWRefCon(dp, "stdf"_4);

    switch(fl->flavor)
    {
        case original_sf:
        case new_sf:
            retval = dhu.odh(ihit, dp);
            break;
        case new_custom_sf:
            retval = dhu.cdh(ihit, dp, fl->mydata);
            break;
        default:
            warning_unexpected("flavor = %d", fl->flavor);
            retval = 0;
            break;
    }
    SetWRefCon(dp, save_ref_con);
    return retval;
}

static void transformsfpdialog(DialogPtr dp, Point *offset, Rect *scrollrect,
                               Boolean getting)
{
    INTEGER numitems, windheight, i, j, extrasizeneeded;
    GUEST<INTEGER> swapped_itype;
    Handle h;
    GUEST<Handle> tmpH;
    Rect r;
    TEPtr tep;

    if(getting)
    {
        extrasizeneeded = 20;
    }
    else
    {
        extrasizeneeded = 110;
        SetRect(scrollrect, 16, 24, 231, 106);
        tep = *(((DialogPeek)dp)->textH);
        tep->destRect.top = tep->destRect.top + (extrasizeneeded);
        tep->destRect.bottom = tep->destRect.bottom + (extrasizeneeded);
        tep->viewRect.top = tep->viewRect.top + (extrasizeneeded);
        tep->viewRect.bottom = tep->viewRect.bottom + (extrasizeneeded);
    }
    numitems = *(GUEST<INTEGER> *)*((((DialogPeek)dp)->items)) + 1;
    for(j = 1; j <= numitems; j++)
    {
        GetDialogItem(dp, j, &swapped_itype, &tmpH, &r);
        i = swapped_itype;
        h = tmpH;
        if(!getting || r.bottom > scrollrect->top)
        {
            r.top = r.top + (extrasizeneeded);
            r.bottom = r.bottom + (extrasizeneeded);
            if(i <= 7 && i >= 4) /* It's a control */
                MoveControl((ControlHandle)h, r.left, r.top);
            SetDialogItem(dp, j, i, h, &r);
        }
    }
    windheight = dp->portRect.bottom - dp->portRect.top + extrasizeneeded;
    SizeWindow((WindowPtr)dp, dp->portRect.right - dp->portRect.left,
               windheight, false);
    if(getting)
    {
        scrollrect->top = scrollrect->top + (extrasizeneeded);
        scrollrect->bottom = scrollrect->bottom + (extrasizeneeded);
    }
    InvalRect(&dp->portRect);
    offset->v -= extrasizeneeded / 2;
}

void adjustdrivebutton(DialogPtr dp)
{
    Handle drhand;
    GUEST<Handle> tmpH;
    GUEST<INTEGER> i;
    Rect r;

    int count = 0;
    for(auto vcbp = (HVCB *)LM(VCBQHdr).qHead; vcbp; vcbp = (HVCB *)vcbp->qLink)
        ++count;

    GetDialogItem(dp, putDrive, &i, &tmpH, &r); /* putDrive == getDrive */
    drhand = tmpH;
    HiliteControl((ControlHandle)drhand, count > 1 ? 0 : 255);
}

static void doeject(DialogPtr dp)
{
    Eject((StringPtr) "", -LM(SFSaveDisk));
    adjustdrivebutton(dp);
    bumpsavedisk(dp, true);
}

static GUEST<OSType> gettypeX(StringPtr name, INTEGER vref, LONGINT dirid)
{
    OSErr err;
    HParamBlockRec pbr;

    pbr.fileParam.ioNamePtr = name;
    pbr.fileParam.ioVRefNum = vref;
    pbr.fileParam.ioFVersNum = 0;
    pbr.fileParam.ioFDirIndex = 0;
    pbr.fileParam.ioDirID = dirid;
    err = PBHGetFInfo(&pbr, false);
    if(err == noErr)
        return pbr.fileParam.ioFlFndrInfo.fdType;
    else
        return 0;
}

static OSErr
unixcore(StringPtr namep, INTEGER *vrefnump, LONGINT *diridp)
{
    // if (namep, *vrefnump, *diridp) refers to a mount point
    // (a concept which doesn't exist on real Macs),
    // set *vrefnump and *diridp and return noErr.
    // Otherwise, return nsvErr.

    // Real MacOS Classic under Mac OS X simulates alias files
    // for mount points.

    // Executor used to handle mount points via this routine,
    // and currently pretends everything on Unix is one volume.

    return nsvErr;
}

OSErr Executor::C_unixmount(CInfoPBRec *cbp)
{
    OSErr err;

    if(!cbp)
    {
        dofloppymount();
        err = noErr;
    }
    else
    {
        INTEGER vrefnum;
        LONGINT dirid;

        vrefnum = cbp->hFileInfo.ioVRefNum;
        dirid = cbp->hFileInfo.ioDirID;
        err = unixcore(cbp->hFileInfo.ioNamePtr, &vrefnum, &dirid);
        if(err == noErr)
        {
            cbp->hFileInfo.ioVRefNum = vrefnum;
            cbp->hFileInfo.ioDirID = dirid;
        }
    }
    return err;
}

static void unixcd(fltype *f)
{
    StringPtr name;
    INTEGER vrefnum;
    LONGINT dirid;

    name = (StringPtr)((*f->flstrs) + (*f->flinfo)[f->flsel].floffs);
    vrefnum = -LM(SFSaveDisk);
    dirid = LM(CurDirStore);
    if(unixcore(name, &vrefnum, &dirid) == noErr)
    {
        LM(SFSaveDisk) = -vrefnum;
        LM(CurDirStore) = dirid;
    }
}

#if 0
	if (rep->fName[0] > 63) /* remember to clip to 63 characters */
	    rep->fName[0] = 63;
#endif

static void
get_starting_point(Point *pp)
{
    INTEGER screen_width, screen_height;
    Rect main_gd_rect;

    main_gd_rect = PIXMAP_BOUNDS(GD_PMAP(LM(MainDevice)));
    screen_width = main_gd_rect.right;
    screen_height = main_gd_rect.bottom;
    pp->h = (screen_width - STANDARD_WIDTH) / 2;
    pp->v = (screen_height - STANDARD_HEIGHT) / 2;
}

static ControlHandle
create_new_folder_button(DialogPtr dp)
{
    ControlHandle retval;
    Handle dial_test;

    dial_test = GetResource("DLOG"_4, -6044);
    if(!dial_test)
        retval = nullptr;
    else
    {
        GUEST<INTEGER> i;
        GUEST<Handle> h;
        Rect r;

        GetDialogItem(dp, sfItemNewFolderUser, &i, &h, &r);
        retval = NewControl((WindowPtr)dp, &r, (StringPtr) "\012New Folder",
                            true, 0, 0, 0, 0, 0L);
        SetDialogItem(dp, sfItemNewFolderUser, ctrlItem | btnCtrl,
                 (Handle)retval, &r);
    }
    return retval;
}

static void
destroy_new_folder_button(DialogPtr dp, ControlHandle ch)
{
    /* May not need to do anything since CloseDialog should clean up all
     outstanding controls */
}

#define SF_NAME(fp) ((fp)->flavor == original_sf         \
                         ? (fp)->flreplyp.oreplyp->fName \
                         : (fp)->flreplyp.nreplyp->sfFile.name)

#define SF_GOOD_XP(fp) ((fp)->flavor == original_sf         \
                            ? &(fp)->flreplyp.oreplyp->good \
                            : &(fp)->flreplyp.nreplyp->sfGood)

#define SF_FTYPE_XP(fp) ((fp)->flavor == original_sf          \
                             ? &(fp)->flreplyp.oreplyp->fType \
                             : &(fp)->flreplyp.nreplyp->sfType)

#define SF_VREFNUM(fp) ((fp)->flavor == original_sf           \
                              ? (fp)->flreplyp.oreplyp->vRefNum \
                              : (fp)->flreplyp.nreplyp->sfFile.vRefNum)

#define SF_DIRID(fp) ((fp)->flavor == original_sf \
                            ? (GUEST<LONGINT>)0     \
                            : (fp)->flreplyp.nreplyp->sfFile.parID)

static void
getditext(DialogPtr dp, INTEGER item, StringPtr text)
{
    GUEST<INTEGER> i;
    GUEST<Handle> h;
    Rect r;

    h = 0;
    GetDialogItem(dp, item, &i, &h, &r);
    if(!h)
        text[0] = 0;
    else
    {
        GetDialogItemText(h, text);
    }
}

static void
report_new_folder_failure(OSErr err)
{
    char *message = (char *)alloca(256);
    Str255 str;

    switch(err)
    {
        default:
            sprintf(message, "Error #%d occurred; you may not be able to create a new folder.", err);
            break;
        case dupFNErr:
            snprintf(message, 256, "That name is already taken; please use another name.");
            break;
        case dirFulErr:
            snprintf(message, 256, "This directory is full; you may not be able to create a new folder.");
            break;
        case dskFulErr:
            snprintf(message, 256, "This disk is full; you may not be able to create a new folder.");
            break;
        case permErr:
        case wrPermErr:
            snprintf(message, 256, "Filesystem permissions prevent the creation of a new folder.");
            break;
        case vLckdErr:
            snprintf(message, 256, "This volume is locked; you can not create a new folder.");
            break;
    }
    str255_from_c_string(str, message);
    ParamText(str, 0, 0, 0);
    NoteAlert(GENERIC_COMPLAINT_ID, nullptr);
}

static bool
new_folder_from_dp(DialogPtr dp, fltype *f)
{
    Str255 str;
    HParamBlockRec hpb;
    OSErr err;
    bool retval;

    getditext(dp, 3, str);
    hpb.ioParam.ioVRefNum = -LM(SFSaveDisk);
    hpb.fileParam.ioDirID = LM(CurDirStore);
    if(str[0] > 31)
        str[0] = 31;
    hpb.ioParam.ioNamePtr = &str[0];
    err = PBDirCreate(&hpb, false);
    if(err != noErr)
        report_new_folder_failure(err);
    else
    {
        *SF_FTYPE_XP(f) = hpb.fileParam.ioDirID;
        retval = true;
    }
    retval = err == noErr;
    return retval;
}

static bool
do_new_folder(fltype *f)
{
    bool retval;
    GrafPtr gp;
    DialogPtr dp;

    retval = false;
    gp = qdGlobals().thePort;
    dp = GetNewDialog(-6044, (Ptr)0, (WindowPtr)-1);
    if(dp)
    {
        bool done;
        GUEST<INTEGER> ihit;

        SelectDialogItemText(dp, 3, 0, 32767);
        ShowWindow(dp);
        SelectWindow(dp);
        done = false;
        while(!done)
        {
            /* TODO: consider a filter that limits the length
	     of the string to 31 letters */
            ModalDialog(nullptr, &ihit);
            switch(ihit)
            {
                default:
                    break;
                case 1:
                    done = new_folder_from_dp(dp, f);
                    if(done)
                        retval = true;
                    break;
                case 2:
                    done = true;
                    break;
            }
        }
        CloseDialog(dp);
        SetPort(gp);
    }
    return retval;
}

void spfcommon(Point p, ConstStringPtr prompt, ConstStringPtr name, dialog_hook_u dh,
               reply_u rep, INTEGER dig, filter_u fp, file_filter_u filef,
               INTEGER numt, GUEST<SFTypeList> tl, getorput_t getorput,
               sf_flavor_t flavor, Ptr activeList,
               ActivateYDUPP activateproc, void *yourdatap)
{
    bool reply_valid;

    reply_valid = false;
#if 0
    if(is_normal_dlgid(getorput, dig) && host_has_spfcommon())
    {
        host_spf_reply_block reply;
        char *c_prompt;
        const char *local_name = "currently ignored";

        c_prompt = cstring_from_str255(prompt);
#if 0
    local_name = ...;
#else
// FIXME: #warning "TODO: support native file pickers"
#endif
        reply_valid = host_spfcommon(&reply, c_prompt, local_name, &fp, &filef,
                                     numt, tl, getorput, flavor, activeList,
                                     (void *)activateproc, yourdatap);
        free(c_prompt);
        if(reply_valid)
            rep_from_host_reply_block(&rep, &reply);
    }
#endif
    if(!reply_valid)
    {
        Handle h;
        DialogPtr dp;
        GUEST<INTEGER> ihit_s, i;
        INTEGER ihit;
        int done, sav;
        Rect r, scrollrect;
        GUEST<Handle> tmpH;
        Handle pnhand = nullptr, ejhand, drhand, sahand;
        OSErr err;
        ControlHandle scrollh;
        // FIXME: #warning structure referenced using 32 bit pointer from refcon
        fltype f;
        GrafPtr gp;
        INTEGER openorsave, promptitem, nmlistitem, diskname, ejectitem, driveitem;
        Boolean transform;
        EventRecord evt;
        ParamBlockRec pbr;
        CInfoPBRec hpb;
        ControlHandle new_folder_button;

        done = false;
        memset(&f, 0, sizeof f);
        f.magicfp = fp;
        f.mydata = yourdatap;
        f.flavor = flavor;
        f.flreplyp = rep;

        if(p.h < 2 || p.v < LM(MBarHeight) + 7)
            get_starting_point(&p);

        *SF_GOOD_XP(&f) = false;
        if(f.flavor == original_sf)
            f.flreplyp.oreplyp->version = 0;
        else
        {
            f.flreplyp.nreplyp->sfIsFolder = 0;
            f.flreplyp.nreplyp->sfIsVolume = 0;
        }
        *SF_FTYPE_XP(&f) = 0;

        new_folder_button = nullptr;
        if(getorput == put)
        {
            str31assign(SF_NAME(&f), name);
            if(f.flavor == original_sf)
            {
                openorsave = putSave;
                promptitem = putPrompt;
                nmlistitem = putNmList;
                diskname = putDiskName;
                ejectitem = putEject;
                driveitem = putDrive;
                f.fl_cancel_item = putCancel;
            }
            else
            {
                openorsave = sfItemOpenButton;
                promptitem = sfItemPromptStaticText;
                nmlistitem = sfItemFileListUser;
                diskname = sfItemVolumeUser;
                ejectitem = sfItemEjectButton;
                driveitem = sfItemDesktopButton;
                f.fl_cancel_item = sfItemCancelButton;
            }
        }
        else
        {
            if(f.flavor == original_sf)
            {
                openorsave = getOpen;
                promptitem = 10; /* according to whom? bill? */
                nmlistitem = getNmList;
                diskname = getDiskName;
                ejectitem = getEject;
                driveitem = getDrive;
                f.fl_cancel_item = getCancel;
            }
            else
            {
                openorsave = sfItemOpenButton;
                promptitem = sfItemPromptStaticText;
                nmlistitem = sfItemFileListUser;
                diskname = sfItemVolumeUser;
                ejectitem = sfItemEjectButton;
                driveitem = sfItemDesktopButton;
                f.fl_cancel_item = sfItemCancelButton;
            }
        }
        gp = qdGlobals().thePort;
        dp = GetNewDialog(dig, (Ptr)0, (WindowPtr)-1);
        bumpsavedisk(dp, false);
        SetPort(dp);
        GetDialogItem(dp, openorsave, &i, &tmpH, &r);
        sahand = tmpH;
        if(getorput == put && SF_NAME(&f)[0])
        {
            bool writable;

            getdiskname(nullptr, &writable);
            sav = true;
        }
        else
        {
            sav = false;
            HiliteControl((ControlHandle)sahand, 255);
        }
        GetDialogItem(dp, ejectitem, &i, &tmpH, &r);
        ejhand = tmpH;
        HiliteControl((ControlHandle)ejhand, 255);
        GetDialogItem(dp, driveitem, &i, &tmpH, &r);
        drhand = tmpH;
        adjustdrivebutton(dp);

        if(getorput == put)
        {
            GetDialogItem(dp, promptitem, &i, &tmpH, &r);
            h = tmpH;
            SetDialogItemText(h, prompt ? prompt : (StringPtr) "");
        }

        GetDialogItem(dp, nmlistitem, &i, &tmpH, &scrollrect);
        h = tmpH;

        if(getorput == put)
        {
            INTEGER putname;

            if(f.flavor == original_sf)
            {
                putname = putName;
                transform = scrollrect.right - scrollrect.left == 1;
            }
            else
            {
                putname = sfItemFileNameTextEdit;
                transform = false;
                new_folder_button = create_new_folder_button(dp);
            }

            GetDialogItem(dp, putname, &i, &tmpH, &r);
            pnhand = tmpH;
            SetDialogItemText(pnhand, SF_NAME(&f));
            SelectDialogItemText((DialogPtr)dp, putname, 0, 32767);
        }
        else
        {
            if(f.flavor == original_sf)
            {
                GetDialogItem(dp, getScroll, &i, &tmpH, &r);
                h = tmpH;
                transform = r.right - r.left == 16;
                GetDialogItem(dp, getDotted, &i, &tmpH, &r);
                h = tmpH;
                SetDialogItem(dp, getDotted, userItem, (Handle)(void*)&ROMlib_filebox, &r);
            }
            else
                transform = false;
        }

        if(transform)
            transformsfpdialog(dp, &p, &scrollrect, getorput == get);

        SetDialogItem(dp, nmlistitem, userItem, (Handle)(void*)&ROMlib_filebox, &scrollrect);

        GetDialogItem(dp, diskname, &i, &tmpH, &r);
        h = tmpH;
        SetDialogItem(dp, diskname, userItem, (Handle)(void*)&ROMlib_filebox, &r);

        r.left = scrollrect.left + 1;
        r.right = scrollrect.right - 16;
        r.top = scrollrect.top + 1;
        r.bottom = scrollrect.bottom - 1;
        scrollrect.left = scrollrect.right - 16;
        scrollh = NewControl((WindowPtr)dp, &scrollrect, (StringPtr) "", true,
                             0, 0, 0, scrollBarProc, 0L);
        flinit(&f, &r, scrollh);
        f.flfilef = filef;
        f.flnumt = numt;
        f.fltl = tl;
        f.flch = (ControlHandle)sahand;
        f.flgraynondirs = getorput == get ? 0 : GRAYBIT;

        if(getorput == get)
        {
            if(f.flnmfil > 0)
            {
                StringPtr ip;

                ip = (StringPtr)(*f.flstrs) + (*f.flinfo)[0].floffs;
                str31assign(SF_NAME(&f), ip);
            }
            else
                (SF_NAME(&f))[0] = 0;
        }
        // FIXME: #warning not 64-bit clean
        SetWRefCon((WindowPtr)dp, US_TO_SYN68K(&f));
        if(dp->portRect.bottom + p.v + 7 > qdGlobals().screenBits.bounds.bottom)
            p.v = qdGlobals().screenBits.bounds.bottom - dp->portRect.bottom - 7;
        if(p.v < LM(MBarHeight) + 7)
            p.v = LM(MBarHeight) + 7;
        MoveWindow((WindowPtr)dp, p.h, p.v, false);

        ihit = -1;
        if(dh.odh)
            ihit = ROMlib_CALLDHOOK(&f, -1, dp, dh); /* the mac does this */

        flfill(&f); /* moved to after dhook call of -1 suggested by
		   Wieslaw Kuzmicz */

        ShowWindow((WindowPtr)dp);
        SelectWindow((WindowPtr)dp);
        while(!done)
        {
            ModalDialog(&ROMlib_stdffilt, &ihit_s);
            ihit = ihit_s;
            if(getorput == put)
                GetDialogItemText(pnhand, SF_NAME(&f));
            if(dh.odh)
                ihit = ROMlib_CALLDHOOK(&f, ihit, dp, dh);
            if(ihit == openorsave)
            {
                makeworking(&f);
                if(getorput == get)
                {
                    if(*SF_FTYPE_XP(&f)) /* will never happen unless someone */
                        (SF_NAME(&f))[0] = 0; /* has a tricky filterproc */
                    else
                        *SF_FTYPE_XP(&f) = gettypeX(SF_NAME(&f), SF_VREFNUM(&f),
                                                    SF_DIRID(&f));
                    done = true;
                    *SF_GOOD_XP(&f) = true;
                }
                else
                {
                    GetDialogItemText(pnhand, SF_NAME(&f));
                    hpb.dirInfo.ioCompletion = 0;
                    hpb.dirInfo.ioNamePtr = (StringPtr)SF_NAME(&f);
                    hpb.dirInfo.ioVRefNum = SF_VREFNUM(&f);
                    hpb.dirInfo.ioFDirIndex = 0;
                    hpb.dirInfo.ioDrDirID = 0;
                    err = PBGetCatInfo(&hpb, false);
                    switch(err)
                    {
                        case noErr:
                            ParamText(SF_NAME(&f), (StringPtr)0, (StringPtr)0,
                                      (StringPtr)0);
                            if(movealert(-3996) == 1) /* overwrite ... */
                                break;
                            *SF_FTYPE_XP(&f) = hpb.hFileInfo.ioFlFndrInfo.fdType;
                        /* FALL THROUGH */
                        default:
                            if(err != noErr)
                                warning_unexpected("err = %d", err);
                        /* FALL THROUGH */
                        case fnfErr:
                            done = true;
                            *SF_GOOD_XP(&f) = true; /* great.  That's a take */
                            break;
                        case bdNamErr:
                        case nsvErr:
                        case paramErr:
                            movealert(-3994); /* disknotfound */
                            break;
                    }
                }
            }
            else if((ihit == f.fl_cancel_item) || (ihit == putCancel))
            { /* MYM 6.0 suggests that putCancel
					     cancels a get, too */
                done = true;
            }
            else if(ihit == ejectitem)
            {
                doeject(dp);
                ihit = sfHookRebuildList;
            }
            else if(ihit == driveitem)
            {
                bumpsavedisk(dp, true);
                ihit = sfHookRebuildList;
            }
            else if(ihit == diskname)
            {
                if(moveuponedir(dp))
                    ihit = sfHookRebuildList;
            }
            else if(ihit == sfHookFolderPopUp)
            {
                if(trackdirs((DialogPeek)dp))
                    ihit = sfHookRebuildList;
            }
            else if(ihit == sfHookOpenFolder)
            {
                if(folder_selected_p(&f))
                {
                    LM(CurDirStore) = *SF_FTYPE_XP(&f);
                    unixcd(&f);
                    ihit = sfHookRebuildList;
                }
            }
            else if(ihit == sfItemNewFolderUser)
            {
                if(do_new_folder(&f))
                {
                    LM(CurDirStore) = *SF_FTYPE_XP(&f);
                    unixcd(&f);
                    ihit = sfHookRebuildList;
                }
            }
            if(getorput == put)
            {
                Str255 file_name;
                bool writable;

                GetDialogItemText(pnhand, file_name);
                str31assign(SF_NAME(&f), file_name);
                getdiskname(nullptr, &writable);
                if((SF_NAME(&f))[0] && writable && !sav)
                {
                    HiliteControl((ControlHandle)sahand, 0);
                    sav = true;
                }
                else if((!(SF_NAME(&f)[0]) || !writable) && sav)
                {
                    HiliteControl((ControlHandle)sahand, 255);
                    sav = false;
                }
            }
            if(WaitNextEvent(diskMask, &evt, 4, 0) && (evt.message & 0xFFFF0000) == 0)
            {
                pbr.volumeParam.ioNamePtr = 0;
                pbr.volumeParam.ioVolIndex = 0;
                pbr.volumeParam.ioVRefNum = evt.message & 0xFFFF;
                err = PBGetVInfo(&pbr, false);
                gui_assert(err == noErr);
                if(err == noErr)
                {
                    adjustdrivebutton(dp);
                    LM(SFSaveDisk) = -pbr.volumeParam.ioVRefNum;
                    LM(CurDirStore) = 2;
                    ihit = sfHookRebuildList;
                }
            }
            if(ihit == sfHookRebuildList)
                realcd((DialogPeek)dp, LM(CurDirStore));
        }
        if(f.flavor != original_sf && dh.odh)
            ihit = ROMlib_CALLDHOOK(&f, -2, dp, dh); /* the mac does this */
        flfinit(&f);
        destroy_new_folder_button(dp, new_folder_button);
        CloseDialog((DialogPtr)dp);
        makeworking(&f);
        SetPort(gp);
    }
}

void Executor::C_SFPPutFile(Point p, ConstStringPtr prompt, ConstStringPtr name,
                            DlgHookUPP dh, SFReply *rep, INTEGER dig,
                            ModalFilterUPP fp)
{
    dialog_hook_u dhu;
    reply_u repu;
    filter_u filteru;
    file_filter_u zero_file_filter = { 0 };

    dhu.odh = dh;
    repu.oreplyp = rep;
    filteru.ofilterp = fp;

    spfcommon(p, prompt, name, dhu, repu, dig, filteru, zero_file_filter, -1,
              nullptr, put, original_sf, 0, 0, 0);
}

void Executor::C_SFPutFile(Point p, ConstStringPtr prompt, ConstStringPtr name,
                           DlgHookUPP dh, SFReply *rep)
{
    SFPPutFile(p, prompt, name, dh, rep, putDlgID, nullptr);
}

void Executor::C_SFPGetFile(Point p, ConstStringPtr prompt, FileFilterUPP filef,
                            INTEGER numt, GUEST<SFTypeList> tl,
                            DlgHookUPP dh, SFReply *rep, INTEGER dig,
                            ModalFilterUPP fp)
{
    dialog_hook_u dhu;
    reply_u repu;
    filter_u filteru;
    file_filter_u file_filteru;

    dhu.odh = dh;
    repu.oreplyp = rep;
    filteru.ofilterp = fp;
    file_filteru.oflfilef = filef;

    spfcommon(p, prompt, (StringPtr) "", dhu, repu, dig, filteru, file_filteru,
              numt, tl, get, original_sf, 0, 0, 0);
}

void Executor::C_SFGetFile(Point p, ConstStringPtr prompt, FileFilterUPP filef,
                           INTEGER numt, GUEST<SFTypeList> tl,
                           DlgHookUPP dh, SFReply *rep)
{
    SFPGetFile(p, prompt, filef, numt, tl, dh, rep, getDlgID, nullptr);
}

void Executor::C_CustomPutFile(ConstStringPtr prompt, ConstStringPtr defaultName,
                               StandardFileReply *replyp, INTEGER dlgid,
                               Point where, DlgHookYDUPP dlghook,
                               ModalFilterYDUPP filterproc, Ptr activeList,
                               ActivateYDUPP activateproc, void *yourdatap)
{
    dialog_hook_u dhu;
    reply_u repu;
    filter_u filteru;
    file_filter_u file_filteru;

    dhu.cdh = dlghook;
    repu.nreplyp = replyp;
    file_filteru.cflfilef = 0;
    filteru.cfilterp = filterproc;

    if(dlgid == 0)
        dlgid = sfPutDialogID;

    spfcommon(where, prompt, defaultName, dhu, repu, dlgid, filteru,
              file_filteru, -1, 0, put, new_custom_sf, activeList,
              activateproc, yourdatap);
}

void Executor::C_CustomGetFile(FileFilterYDUPP filefilter,
                               INTEGER numtypes, GUEST<SFTypeList> typelist,
                               StandardFileReply *replyp, INTEGER dlgid,
                               Point where, DlgHookYDUPP dlghook,
                               ModalFilterYDUPP filterproc, Ptr activeList,
                               ActivateYDUPP activateproc, void *yourdatap)
{
    dialog_hook_u dhu;
    reply_u repu;
    filter_u filteru;
    file_filter_u file_filteru;

    dhu.cdh = dlghook;
    repu.nreplyp = replyp;
    filteru.cfilterp = filterproc;
    file_filteru.cflfilef = filefilter;

    if(dlgid == 0)
        dlgid = sfGetDialogID;

    spfcommon(where, (StringPtr) "", (StringPtr) "", dhu, repu, dlgid,
              filteru, file_filteru, numtypes,
              typelist, get, new_custom_sf, activeList, activateproc,
              yourdatap);
}

void Executor::C_StandardGetFile(FileFilterUPP filef, INTEGER numt,
                                 GUEST<SFTypeList> tl,
                                 StandardFileReply *replyp)
{
    Point p;
    reply_u repu;
    file_filter_u file_filteru;
    dialog_hook_u dhu;
    filter_u filteru;

    repu.nreplyp = replyp;
    file_filteru.oflfilef = filef;
    dhu.cdh = 0;
    filteru.cfilterp = 0;

    p.h = -1;
    p.v = -1;
    spfcommon(p, (StringPtr) "", (StringPtr) "", dhu, repu, sfGetDialogID,
              filteru, file_filteru, numt, tl, get, new_sf, 0, 0, 0);
}

void Executor::C_StandardPutFile(ConstStringPtr prompt, ConstStringPtr defaultname,
                                 StandardFileReply *replyp)
{
    Point p;
    reply_u repu;
    file_filter_u file_filteru;
    dialog_hook_u dhu;
    filter_u filteru;

    filteru.cfilterp = 0;
    repu.nreplyp = replyp;
    file_filteru.cflfilef = 0;
    dhu.cdh = 0;

    p.h = -1;
    p.v = -1;
    spfcommon(p, prompt, defaultname, dhu, repu, sfPutDialogID, filteru,
              file_filteru, -1, 0, put, new_sf, 0, 0, 0);
}
