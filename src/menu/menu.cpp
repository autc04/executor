/* Copyright 1986-2000 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in MenuMgr.h (DO NOT DELETE THIS LINE) */

/*
 * TODO: use update regions if we don't have room to save and restore bitmaps
 */

#include <base/common.h>
#include <QuickDraw.h>
#include <MenuMgr.h>
#include <WindowMgr.h>
#include <ResourceMgr.h>
#include <MemoryMgr.h>
#include <EventMgr.h>
#include <FontMgr.h>
#include <OSUtil.h>
#include <ToolboxUtil.h>
#include <ToolboxEvent.h>
#include <OSEvent.h>
#include <DeskMgr.h>
#include <SysErr.h>

#include <menu/menu.h>
#include <quickdraw/cquick.h>
#include <quickdraw/quick.h>
#include <mman/mman.h>
#include <wind/wind.h>
#include <rsys/hook.h>
#include <res/resource.h>
#include <rsys/aboutbox.h>
#include <error/system_error.h>
#include <rsys/version.h>
#include <base/functions.impl.h>
#include <prefs/prefs.h>
#include <rsys/toolevent.h>
#include <rsys/desk.h>
#include <vdriver/vdriver.h>

using namespace Executor;

typedef void (*menuhookp)(void);

typedef LONGINT (*mbarhookp)(Rect *rp);

#define CALLMENUHOOK(fp) ROMlib_CALLMENUHOOK((menuhookp)(fp))
#define CALLMBARHOOK(arg, fp) ROMlib_CALLMBARHOOK(arg, (mbarhookp)(fp))

static void dirtymenusize(MenuHandle);
static Boolean findroot(INTEGER menuid, INTEGER *root_unswp);
static inline void ROMlib_CALLMENUHOOK(menuhookp fp);
static inline LONGINT ROMlib_CALLMBARHOOK(Rect *rp, mbarhookp fp);
static INTEGER wheretowhich(LONGINT offset);
static void shadowrect(Rect *rp);
static void restoren(INTEGER ntodrop, RgnHandle restoredrgn, Rect *rp);
static void initpairs(startendpairs pairs);
static Boolean mtoggle(INTEGER mid, highstate h);
static MenuHandle menunumtomh(INTEGER mid, INTEGER *sixp);
static MenuHandle itemishierarchical(MenuHandle, INTEGER, INTEGER *);

static void dirtymenusize(MenuHandle mh)
{
    if(mh)
    {
        (*mh)->menuWidth = -1;
        (*mh)->menuHeight = -1;
    }
}

void Executor::C_InvalMenuBar()
{
    DrawMenuBar();
}

void Executor::C_DrawMenuBar()
{
    if(LM(MBDFHndl))
    {
        LM(TheMenu) = 0;
        MBDFCALL(mbDraw, 0, 0L);
    }
}

#define HIEROFFX \
    (*(GUEST<INTEGER> *)((char *)(*MENULIST) + (*MENULIST)->muoff + sizeof(muelem) + 4))

#define HIEROFF \
    HIEROFFX

#define MINMENULISTSIZE ((Size)sizeof(INTEGER) * 4 + sizeof(GUEST<Handle>))

void Executor::C_ClearMenuBar()
{
    if(LM(MenuList))
        SetHandleSize(LM(MenuList), MINMENULISTSIZE);
    else
        LM(MenuList) = NewHandle(MINMENULISTSIZE);
    (*MENULIST)->muoff = 0; /* int 1 */
    (*MENULIST)->muright = MENULEFT; /* int 2 */

    (&(*MENULIST)->mufu)[3] = sizeof(muelem); /* lastHMenu: int 6 */
    (*(GUEST<Handle> *)&(&(*MENULIST)->mufu)[1]) = nullptr; /* menuTitleSave: int 4,5 */
}

#define BLACK_RGB        \
    {                    \
        0           \
        , 0, 0 \
    }
#define WHITE_RGB                                                  \
    {                                                              \
        (unsigned short)0xFFFF                                \
        , (unsigned short)0xFFFF, (unsigned short)0xFFFF \
    }
#define RESERVED_RGB /* arbitrary */ WHITE_RGB
static MCEntry default_menu_ctab[] = {
    {
        0, 0,
        BLACK_RGB, WHITE_RGB, BLACK_RGB, WHITE_RGB,
        /* reserved */ 0,
    },
    /* end marker */
    {
        (short)-99, /* reserved */ 0,
        RESERVED_RGB, RESERVED_RGB, RESERVED_RGB, RESERVED_RGB,
        /* reserved */ 0,
    },
};
#undef BLACK_RGB
#undef WHITE_RGB
#undef RESERVED_RGB

static void
append_end_marker_if_necessary(MCTableHandle h)
{
    Size size;
    MCEntryPtr entries;
    int i, nelem;

    size = GetHandleSize((Handle)h);
    entries = *h;
    nelem = size / sizeof *entries;
    for(i = 0; i < nelem && MCENTRY_ID(&entries[i]) != -99;)
        ++i;
    if(i == nelem)
    {
        size += sizeof *entries;
        SetHandleSize((Handle)h, size);
        if(MemError() == noErr)
        {
            entries = *h;
            entries[nelem] = default_menu_ctab[std::size(default_menu_ctab) - 1];
        }
    }
}

void Executor::C_InitMenus()
{
    Handle default_mcinfo;

    LM(MenuHook) = LM(MBarHook) = 0;
    ClearMenuBar();
    default_mcinfo = ROMlib_getrestid("mctb"_4, 0);
    if(!default_mcinfo)
    {
        /* try to load 'mctb' resource 0; otherwise use default
	 built into `ROM' */

        LM(MenuCInfo) = (MCTableHandle)NewHandle(sizeof default_menu_ctab);
        BlockMoveData((Ptr)default_menu_ctab, (Ptr)*LM(MenuCInfo),
                      sizeof default_menu_ctab);
    }
    else
    {
        int n_entries;

        n_entries = *(GUEST<uint16_t> *)*default_mcinfo;
        LM(MenuCInfo) = (MCTableHandle)NewHandle(n_entries
                                                * sizeof(MCEntry));
        BlockMoveData((Ptr)(&((uint16_t *)*default_mcinfo)[1]),
                      (Ptr)*LM(MenuCInfo),
                      n_entries * sizeof(MCEntry));
        append_end_marker_if_necessary(LM(MenuCInfo));
    }

    InitProcMenu(0); /* sets mbResID */ /* int 3 */

    MBDFCALL(mbHeight, 0, 0L);

    DrawMenuBar();
}

MenuHandle Executor::C_NewMenu(INTEGER mid, ConstStringPtr str)
{
    MenuHandle retval;
    GUEST<Handle> temph;

    if(!str)
        str = (StringPtr) "";
    retval = (MenuHandle)NewHandle((Size)SIZEOFMINFO + str[0] + 1);
    (*retval)->menuID = mid;
    (*retval)->menuWidth = (*retval)->menuHeight = 0;
    /* menuHeight calculated elsewhere */
    SetResLoad(true);
    temph = GetResource("MDEF"_4, textMenuProc);
    (*retval)->menuProc = temph;
    (*retval)->enableFlags = -1;
    str255assign((*retval)->menuData, str);
    *((char *)*retval + SIZEOFMINFO + str[0]) = 0;
    return (retval);
}

void Executor::C_CalcMenuSize(MenuHandle mh)
{
    Point dummy_pt;
    Rect rect;
    GUEST<int16_t> i;

    if(mh)
    {
        i = -1;
        ThePortGuard guard(wmgr_port);

        PORT_TX_FACE(wmgr_port) = (Style)0;
        PORT_TX_FONT(wmgr_port) = 0;

        /* initialize the unused point to a known value */
        memset(&dummy_pt, 0xFF, sizeof dummy_pt);
        memset(&rect, 0, sizeof rect);

        MENUCALL(mSizeMsg, mh, &rect, dummy_pt, &i);
    }
}

/*
 * NOTE: the nastiness below is because it is very hard to tell
 *	 whether or not someone has already converted a menuProc
 *	 from a resource id to a handle.
 */

MenuHandle Executor::C_GetMenu(int16_t rid)
{
    MenuHandle retval;
    Handle mct_res_h;

    SetResLoad(true);
    retval = (MenuHandle)GetResource("MENU"_4, rid);

    mct_res_h = ROMlib_getrestid("mctb"_4, rid);
    if(mct_res_h)
    {
        HLockGuard guard(mct_res_h);

        mct_res_t *mct_res;
        /*
	 MCEntry entry;
	 int i;
	 
	 entry.mctReserved = 0;
	 mct_res = (mct_res_t *) (*mct_res_h);
	 for (i = 0; i < mct_res->n_entries; i ++)
	   {
	     memcpy (&entry, &mct_res->entries[i], sizeof mct_res->entries[i]);
	     SetMCEntries (1, &entry);
	   }
*/
        mct_res = (mct_res_t *)*mct_res_h;
        SetMCEntries(mct_res->n_entries,
                     &mct_res->entries[0]);
    }

    if(retval)
    {
        Handle current_proc;

        current_proc = (*retval)->menuProc;
        if(!HandleZone(current_proc) || !GetHandleSize(current_proc))
        {
            Handle temph;

            LM(MemErr) = noErr;
            temph = GetResource("MDEF"_4,
                                *(GUEST<int16_t> *)&(*retval)->menuProc);
            if(SIZEOFMINFO != 15)
                Munger((Handle)retval, (int32_t)6, (Ptr)0, (int32_t)0,
                       (Ptr) "x", (int32_t)2);
            MI_PROC(retval) = temph;
            CalcMenuSize(retval);
        }
    }

    return retval;
}

void Executor::C_DisposeMenu(MenuHandle mh)
{
    if(mh)
    {
        ReleaseResource((Handle)mh);
        if(LM(ResErr) == resNotFound)
            DisposeHandle((Handle)mh);
    }
}

typedef struct
{ /* NOTE: These structures are strictly local */
    MenuHandle menh; /*	 There's no reason to swap the bytes */
    INTEGER menitem; /*	 in these fields */
    INTEGER menoff;
} endinfo;

static void toend(MenuHandle, endinfo *);
static void app(ConstStringPtr, char, char, char, char, char, endinfo *);
static void handleinsert(Handle, StringPtr);
static void xInsertResMenu(MenuHandle, ConstStringPtr, ResType, INTEGER);

static void toend(MenuHandle mh, endinfo *eip)
{
    char *menuop;

    eip->menh = mh;
    eip->menitem = 0;

    menuop = (char *)*mh + SIZEOFMINFO + (*mh)->menuData[0];
    while(*menuop != 0)
    {
        ++eip->menitem;
        menuop += (*menuop) + SIZEOFMEXT;
    }
    eip->menoff = menuop - (char *)*mh;
}

static void app(ConstStringPtr str, char icon, char marker, char style,
                char keyequiv, char disflag, endinfo *eip)
{
    Size newsize;
    SignedByte state;

    eip->menitem++;
    if(disflag)
        (*eip->menh)->enableFlags = (*eip->menh)->enableFlags & (~((LONGINT)1 << eip->menitem));
    else
        (*eip->menh)->enableFlags = (*eip->menh)->enableFlags | ((LONGINT)1 << eip->menitem);
    newsize = eip->menoff + SIZEOFMEXT + 1 + str[0];
    SetHandleSize((Handle)eip->menh, newsize);
    /*
 * The following lines were put in because Virex 4.0 calls AppendResMenu with
 * a locked handle that happens to have a locked block past it, hence it
 * can't grow.  It's unclear what we should do about this sort of thing
 * in general, but this hack will suffice for now.
 */
    if(MemError() != noErr)
    {
        state = HGetState((Handle)eip->menh);
        HUnlock((Handle)eip->menh);
        SetHandleSize((Handle)eip->menh, newsize);
        HSetState((Handle)eip->menh, state);
    }
    char *menuop = (char *)(*eip->menh) + eip->menoff;
    const unsigned char *ip = str;
    const unsigned char *ep = ip + str[0] + 1;
    while(ip != ep)
        *menuop++ = *ip++;
    *menuop++ = icon;
    *menuop++ = keyequiv;
    *menuop++ = marker;
    *menuop++ = style;
    *menuop = 0;
    eip->menoff = menuop - (char *)(*eip->menh);
}

void Executor::C_AppendMenu(MenuHandle mh, ConstStringPtr str)
{
    Str255 tempstr;
    char *ip, *op;
    char c, icon = 0, marker = 0, keyequiv = 0, disflag = 0;
    int style = 0;
    int i;
    endinfo endinf;

    if(mh)
    {
        toend(mh, &endinf);
        ip = (char *)str + 1;
        op = (char *)tempstr + 1;
        tempstr[0] = 0;
        i = str[0];
        while(i > 0)
        {
            switch(i--, c = *ip++)
            {
                case '^':
                    icon = *ip++ - '0';
                    i--;
                    break;
                case '!':
                    marker = *ip++;
                    i--;
                    break;
                case '<':
                    switch(i--, c = *ip++)
                    {
                        case 'B':
                            style |= (int)bold;
                            break;
                        case 'I':
                            style |= (int)italic;
                            break;
                        case 'U':
                            style |= (int)underline;
                            break;
                        case 'O':
                            style |= (int)outline;
                            break;
                        case 'S':
                            style |= (int)shadow;
                            break;
                        default:
                            *op++ = '<';
                            *op++ = c;
                            tempstr[0] += 2;
                    }
                    break;
                case '/':
                    keyequiv = *ip++;
                    i--;
                    break;
                case '(':
                    disflag = true;
                    break;
                case ';':
                case '\n':
                case '\r':
                    if(tempstr[0])
                        app(tempstr, icon, marker, style, keyequiv, disflag, &endinf);
                    op = (char *)tempstr + 1;
                    tempstr[0] = icon = marker = style = keyequiv = disflag = 0;
                    break;
                default:
                    *op++ = c;
                    tempstr[0]++;
            }
        }
        if(tempstr[0])
            app(tempstr, icon, marker, style, keyequiv, disflag, &endinf);
        dirtymenusize(mh);
    }
}

static void handleinsert(Handle h, StringPtr strp)
{
    LONGINT n;
    StringPtr sp;
    Boolean done;

    n = GetHandleSize(h);
    sp = (StringPtr)*h;
    done = false;
    while(n > 0 && !done)
    {
        switch(RelString(sp, strp, true, true))
        {
            case 1:
                done = true;
                break;
            case 0:
                return;
                break;
            default:
                n -= sp[0] + 1;
                sp += sp[0] + 1;
                break;
        }
    }
    Munger(h, sp - (StringPtr)*h, (Ptr)0, 0, (Ptr)strp, strp[0] + 1);
}

void Executor::C_AppendResMenu(MenuHandle mh, ResType restype)
{
    if(mh)
    {
        int nres, n;
        Handle h;
        GUEST<INTEGER> i;
        GUEST<ResType> t;
        Str255 str;
        endinfo endinf;
        Handle temph;
        StringPtr sp;

        SetResLoad(false);
        toend(mh, &endinf);
        temph = NewHandle((Size)0);
        do
        {
            nres = CountResources(restype);
            for(n = 1; n <= nres; n++)
            {
                h = GetIndResource(restype, n);
                GetResInfo(h, &i, &t, str);
                if(str[0] && str[1] != '.' && str[1] != '%')
                    handleinsert(temph, str);
            }
        } while(restype == "FONT"_4 && (restype = "FOND"_4));

        /* Add an "About Executor..." menu to the Apple menu when they
       * ask for desk accessories.
       */

        if(restype == "DRVR"_4)
        {
            for(const auto& entry : appleMenuEntries())
                app(entry.name, 0, 0, 0, 0, false, &endinf);
        }

        n = GetHandleSize(temph);
        sp = (StringPtr)*temph;
        HLock(temph);
        while(n > 0)
        {
            app(sp, 0, 0, 0, 0, false, &endinf);
            n -= sp[0] + 1;
            sp += sp[0] + 1;
        }
        HUnlock(temph);
        DisposeHandle(temph);
        SetResLoad(true); /* IMI-353 says to do this. */
        dirtymenusize(mh);
    }
    /* FIXME - IMI-353 says to call SetResLoad(true), but we only do
   * this "if (mh)".
   */
}

mextp Executor::ROMlib_mitemtop(MenuHandle mh, INTEGER item,
                                StringPtr *stashstringp) /* INTERNAL */
{
    mextp retval;
    StringPtr stashstring;

    if(!mh)
        /*-->*/ return 0;
    retval = (mextp)((char *)*mh + SIZEOFMINFO + (*mh)->menuData[0]);
    if(*(char *)retval != 0)
    {
        stashstring = (StringPtr)retval;
        retval = (mextp)((char *)retval + *(char *)retval + 1);

        while(retval->mnextlen && item > 1)
        {
            stashstring = (StringPtr) & (retval->mnextlen);
            retval = (mextp)((char *)retval + SIZEOFMEXT + retval->mnextlen);
            item--;
        }
        if(stashstringp)
            *stashstringp = stashstring;
        if(item != 1)
            retval = 0;
    }
    else
        retval = 0;
    return retval;
}

void Executor::C_DeleteMenuItem(MenuHandle mh, INTEGER item) /* IMIV-56 */
{
    if(mh)
    {
        ULONGINT unchangedmask;
        StringPtr stashstring;

        if(ROMlib_mitemtop(mh, item, &stashstring))
        {
            Munger((Handle)mh, (char *)stashstring - (char *)*mh, (Ptr)0,
                   SIZEOFMEXT + stashstring[0], (Ptr) "", 0);
            if(item < 32)
            {
                unchangedmask = (1L << item) - 1; /* 2s complement dependent */
                (*mh)->enableFlags = (((*mh)->enableFlags >> 1) & ~unchangedmask) | ((*mh)->enableFlags & unchangedmask) | 0x80000000;
            }
        }
        dirtymenusize(mh);
    }
}

static void xInsertResMenu(MenuHandle mh, ConstStringPtr str, ResType restype,
                           INTEGER after)
{
    LONGINT oldeflags;
    Size hsize;
    Handle h;
    int soff;
    char *sp, *dp, *ep;
    int omen;
    mextp mmm;
    endinfo endinf;

    if(mh)
    {
        toend(mh, &endinf);
        omen = endinf.menitem;
        mmm = 0;
        if(after > 0 && !(mmm = ROMlib_mitemtop(mh, after, (StringPtr *)0)))
            if(str)
                AppendMenu(mh, str);
            else
                AppendResMenu(mh, restype);
        else
        {
            oldeflags = (*mh)->enableFlags & ~(((LONGINT)1 << (after + 1)) - 1);
            if(mmm)
                soff = (char *)&(mmm->mnextlen) - (char *)*mh;
            else
                soff = SIZEOFMINFO + (*mh)->menuData[0];
            hsize = GetHandleSize((Handle)mh) - soff;
            gui_assert(hsize >= 0);
            h = NewHandle(hsize);
            sp = (char *)*mh + soff;
            dp = (char *)*h;
            ep = sp + hsize;
            while(sp != ep)
                *dp++ = *sp++;
            *((char *)*mh + soff) = 0;
            SetHandleSize((Handle)mh, (Size)soff + 1);
            if(str)
                AppendMenu(mh, str);
            else
                AppendResMenu(mh, restype);
            SetHandleSize((Handle)mh, GetHandleSize((Handle)mh) + hsize - 1);
            sp = (char *)*h;
            toend(mh, &endinf);
            dp = (char *)*mh + endinf.menoff;
            ep = sp + hsize;
            while(sp != ep)
                *dp++ = *sp++;
            (*mh)->enableFlags = (*mh)->enableFlags | oldeflags << (endinf.menitem - after);
#if 0 /* RagTime suggests that at least for InsertMenuItem */
	/* CalcMenuSize shouldn't be called */
	CalcMenuSize(mh);
#else
            dirtymenusize(mh);
#endif
            DisposeHandle(h);
        }
    }
}

void Executor::C_InsertResMenu(MenuHandle mh, ResType restype, INTEGER after)
{
    if(mh)
        xInsertResMenu(mh, (StringPtr)0, restype, after);
}

void Executor::C_InsertMenuItem(MenuHandle mh, ConstStringPtr str,
                             INTEGER after) /* IMIV-55 */
{
    if(mh)
        xInsertResMenu(mh, str, (ResType)0, after);
}

#define FIRSTHIER \
    ((muelem *)((char *)(*MENULIST) + (*MENULIST)->muoff) + 2)

void Executor::C_InsertMenu(MenuHandle mh, INTEGER before)
{
    muelem *mp, *mpend, *bindex, newmuelem;
    INTEGER mid1, mid2;
    LONGINT binoff;

    if(mh)
    {
        mid1 = (*mh)->menuID;
        if(before == -1)
        {
            mpend = (*MENULIST)->mulist + HIEROFF / sizeof(muelem);
            for(mp = FIRSTHIER;
                mp != mpend && ((*mp->muhandle)->menuID != mid1);
                mp++)
                ;
            if(mp != mpend) /* already there */
                /*-->*/ return;
            SetHandleSize(LM(MenuList), (Size)HIEROFF + 2 * sizeof(muelem));
            mpend = (*MENULIST)->mulist + HIEROFF / sizeof(muelem);
            mpend->muhandle = mh;
            mpend->muleft = 0;
        }
        else
        {
            bindex = 0;
            mpend = (*MENULIST)->mulist + (*MENULIST)->muoff / sizeof(muelem);
            for(mp = (*MENULIST)->mulist; mp != mpend; mp++)
            {
                if((mid2 = (*mp->muhandle)->menuID) == mid1)
                    /*-->*/ return;
                if(mid2 == before)
                    bindex = mp;
            }
            newmuelem.muhandle = mh;
            if(bindex == 0)
            {
                newmuelem.muleft = (*MENULIST)->muright;
                Munger(LM(MenuList), (*MENULIST)->muoff + sizeof(muelem),
                       (Ptr)0, (LONGINT)0, (Ptr)&newmuelem,
                       (LONGINT)sizeof(newmuelem));
                binoff = (*MENULIST)->muoff;
            }
            else
            {
                newmuelem.muleft = bindex->muleft;
                binoff = (char *)bindex - (char *)*LM(MenuList);
                Munger(LM(MenuList), binoff, (Ptr)0, (LONGINT)0,
                       (Ptr)&newmuelem, (LONGINT)sizeof(newmuelem));
            }
            (*MENULIST)->muoff = (*MENULIST)->muoff + sizeof(muelem);
            MBDFCALL(mbCalc, 0, binoff);
        }
        HIEROFFX = HIEROFF + sizeof(muelem);
    }
}

void Executor::C_DeleteMenu(int16_t mid)
{
    muelem *mp, *mpend;
    int32_t deleteloc;

    menu_delete_entries(mid);

    mpend = (*MENULIST)->mulist + (*MENULIST)->muoff / sizeof(muelem);
    for(mp = (*MENULIST)->mulist;
        mp != mpend && (*mp->muhandle)->menuID != mid;
        mp++)
        ;
    if(mp != mpend)
    {
        deleteloc = (LONGINT)((char *)mp - (char *)*LM(MenuList));
        Munger(LM(MenuList), deleteloc, (Ptr)0, (int32_t)sizeof(muelem),
               (Ptr) "", (int32_t)0);
        (*MENULIST)->muoff = (*MENULIST)->muoff - sizeof(muelem);
        MBDFCALL(mbCalc, 0, deleteloc);
    }
    else
    {
        mpend = (*MENULIST)->mulist + HIEROFF / sizeof(muelem);
        for(mp = FIRSTHIER;
            mp != mpend && ((*mp->muhandle)->menuID != mid);
            mp++)
            ;
        if(mp == mpend)
            /*-->*/ return; /* not there */
        Munger(LM(MenuList),
               (int32_t)((char *)mp - (char *)*LM(MenuList)),
               (Ptr)0, (int32_t)sizeof(muelem), (Ptr) "", (int32_t)0);
    }
    HIEROFFX = HIEROFF - sizeof(muelem);
}

typedef mbartype *mbarptr;

typedef GUEST<mbarptr> *mbarhandle;

Handle Executor::C_GetNewMBar(INTEGER mbarid)
{
    mbarhandle mb;
    mlhandle saveml;
    GUEST<INTEGER> *ip, *ep;
    MenuHandle mh;
    Handle retval;

    mb = (mbarhandle)GetResource("MBAR"_4, mbarid);
    if(!mb)
        retval = 0;
    else
    {
        if(!*mb)
            LoadResource((Handle)mb);

        HLockGuard guard(mb);

        ip = (*mb)->mrid;
        ep = ip + (*mb)->nmen;
        saveml = MENULIST;

        LM(MenuList) = 0;
        ClearMenuBar();
        retval = LM(MenuList);

        (*MENULIST)->mufu = (*saveml)->mufu; /* int 3 */

        while(ip != ep)
        {
            mh = GetMenu(*ip);
            InsertMenu(mh, 0);
            ip++;
        }

        LM(MenuList) = (Handle)saveml;
    }
    return retval;
}

Handle Executor::C_GetMenuBar()
{
    GUEST<Handle> retval;

    retval = LM(MenuList);
    HandToHand(&retval);
    return retval;
}

void Executor::C_SetMenuBar(Handle ml)
{
    GUEST<Handle> temph;

    DisposeHandle(LM(MenuList));
    temph = ml;
    HandToHand(&temph);
    LM(MenuList) = temph;
}

enum
{
    nonhier = 0,
    hier = 1
};

static void initpairs(startendpairs pairs)
{
    pairs[(int)nonhier].startp = (*MENULIST)->mulist;
    pairs[(int)nonhier].endp = (*MENULIST)->mulist + (*MENULIST)->muoff / sizeof(muelem);
    pairs[(int)hier].startp = FIRSTHIER;
    pairs[(int)hier].endp = (*MENULIST)->mulist + HIEROFF / sizeof(muelem);
}

static bool
menu_id_exists_p(int id)
{
    muelem *mp;
    startendpairs mps;

    initpairs(mps);
    for(mp = mps[nonhier].startp; mp != mps[nonhier].endp; mp++)
    {
        if((*mp->muhandle)->menuID == id)
            return true;
    }
    return false;
}

INTEGER Executor::ROMlib_mentosix(INTEGER menuid)
{
    muelem *mp, *mpend;
    startendpairs mps;
    INTEGER i;

    initpairs(mps);
    for(i = (int)nonhier; i <= (int)hier; i++)
    {
        for(mp = mps[i].startp, mpend = mps[i].endp;
            mp < mpend && (*mp->muhandle)->menuID != menuid; mp++)
            ;
        if(mp < mpend)
            /*-->*/ return (char *)mp - (char *)*LM(MenuList);
    }
    return -1;
}

static Boolean mtoggle(INTEGER mid, highstate h)
{
    LONGINT l;

    l = ROMlib_mentosix(mid);
    if(l != -1)
    {
        l |= (LONGINT)h << 16;
        MBDFCALL(mbHilite, 0, l);
        /*-->*/ return true;
    }
    return false;
}

void Executor::C_HiliteMenu(INTEGER mid)
{
    if(mid != LM(TheMenu))
    {
        if(LM(TheMenu))
            mtoggle(LM(TheMenu), RESTORE);
        if(!menu_id_exists_p(mid))
            mid = 0;
        if(mid)
        {
            if(mid == (*(*MENULIST)->mulist[0].muhandle)->menuID)
                ROMlib_alarmoffmbar();
            mtoggle(mid, HILITE);
        }
    }
    LM(TheMenu) = mid;
}

static inline void ROMlib_CALLMENUHOOK(menuhookp fp)
{
    ROMlib_hook(menu_menuhooknumber);
    execute68K(US_TO_SYN68K(fp));
}

#define CALLMBARHOOK(arg, fp) ROMlib_CALLMBARHOOK(arg, (mbarhookp)(fp))

static inline LONGINT ROMlib_CALLMBARHOOK(Rect *rp, mbarhookp fp)
{
    ROMlib_hook(menu_mbarhooknumber);

    PUSHADDR(US_TO_SYN68K(rp));
    execute68K(US_TO_SYN68K(fp));
    return EM_D0;
}

static INTEGER wheretowhich(LONGINT offset)
{
    mbdfentry *p, *ep;

    ep = (mbdfentry *)*LM(MBSaveLoc);
    for(p = (mbdfentry *)((char *)*LM(MBSaveLoc) + (*MBSAVELOC)->lastMBSave);
        p != ep && p->mbMLOffset != offset; p--)
        ;
    return p - ep;
}

static void shadowrect(Rect *rp)
{
    rp->top = rp->top - 1;
    rp->left = rp->left - 1;
    rp->bottom = rp->bottom + 2;
    rp->right = rp->right + 2;
}

static void restoren(INTEGER ntodrop, RgnHandle restoredrgn, Rect *rp)
{
    mbdfentry *p;
    RgnHandle tmprgn;

    if(restoredrgn)
    {
        p = (mbdfentry *)((char *)*LM(MBSaveLoc) + (*MBSAVELOC)->lastMBSave);
        RectRgn(restoredrgn, &p->mbRectSave);
        shadowrect(&(*restoredrgn)->rgnBBox);
    }
#if !defined(LETGCCWAIL)
    else
        p = (mbdfentry *)-1;
#endif /* LETGCCWAIL */
    MBDFCALL(mbRestore, 0, 0L);
    ROMlib_rootless_closemenu();

    while(--ntodrop > 0)
    {
        if(restoredrgn)
        {
            --p;
            tmprgn = NewRgn();
            RectRgn(tmprgn, &p->mbRectSave);
            shadowrect(&(*tmprgn)->rgnBBox);
            UnionRgn(restoredrgn, tmprgn, restoredrgn);
            DisposeRgn(tmprgn);
        }
        MBDFCALL(mbRestore, 0, 0L);
        ROMlib_rootless_closemenu();
    }
    if(restoredrgn && rp)
        *rp = p[-1].mbRectSave;
}

static MenuHandle menunumtomh(INTEGER mid, INTEGER *sixp)
{
    muelem *mp, *mpend;

    mpend = (*MENULIST)->mulist + HIEROFF / sizeof(muelem);
    for(mp = FIRSTHIER; mp != mpend && (*mp->muhandle)->menuID != mid; mp++)
        ;
    if(mp != mpend)
    {
        *sixp = (char *)mp - (char *)*LM(MenuList);
        /*-->*/ return mp->muhandle;
    }
    return 0;
}

static MenuHandle itemishierarchical(MenuHandle mh, INTEGER item, INTEGER *sixp)
{
    mextp mep;

    if((mep = ROMlib_mitemtop(mh, item, (StringPtr *)0)) && mep->mkeyeq == 0x1B)
        /*-->*/ return menunumtomh(mep->mmarker, sixp);
    return 0;
}

#define HIERRECTBIT (1L << 16)

int32_t Executor::ROMlib_menuhelper(MenuHandle mh, Rect *saverp,
                                    int32_t oldwhere, Boolean ispopup,
                                    int16_t nmenusdisplayed)
{
    mbdfentry *oldentry, *newentry;
    Rect r, r2;
    Point tempp;
    INTEGER mid, item, olditem, tempi;
    GUEST<INTEGER> item_swapped;
    MenuHandle newmh;
    int i;
    LONGINT pointaslong;
    LONGINT where, templ;
    GUEST<RgnHandle> saveclip;
    RgnHandle restoredrgn;
    bool changedmenus = false;
    INTEGER oldwhichmenuhit, whichmenuhit;
    GrafPtr saveport;
    
    Point pt;
    LONGINT myd0;
    GUEST<Point> ptTmp;
    bool seen_up_already = false;

    GUEST<GrafPtr> saveport_swapped;
    GetPort(&saveport_swapped);
    saveport = saveport_swapped;
    SetPort(wmgr_port);

    olditem = -1;
    item = 0;

    short firstitem = ispopup ? -1 : 0;

    r = *saverp;
    Point dummy_pt = {-1, -1};
    oldwhichmenuhit = whichmenuhit = wheretowhich(oldwhere);
    restoredrgn = NewRgn();

    auto shouldKeepTracking = [&seen_up_already, &item, &firstitem]() -> bool {
        if(!ROMlib_sticky_menus_p)
            return WaitMouseUp();
    
        EventRecord ev;
        if(GetOSEvent(mUpMask | mDownMask, &ev))
        {
            if(ev.what == mouseUp)
            {
                if(seen_up_already || (item != firstitem && firstitem != -1))
                    return false;
                else
                    seen_up_already = true;
            }
        }

        return true;
    };

    bool firstIteration = true;

    while(shouldKeepTracking())
    {
        GetMouse(&ptTmp);
        pt = ptTmp.get();
        pointaslong = ((int32_t)pt.v << 16) | (unsigned short)pt.h;
        where = MBDFCALL(mbHit, 0, pointaslong);

        if(firstIteration && !ispopup && pt.v < LM(MBarHeight) && where == NOTHITINMBAR)
        {
            if(vdriver->handleMenuBarDrag())
                break;
        }
        firstIteration = false;

        if(LM(MenuHook))
            CALLMENUHOOK(LM(MenuHook));
        if(where == oldwhere)
        {
            if(mh)
            {
                PORT_TX_FACE(wmgr_port) = (Style)0;
                PORT_TX_FONT(wmgr_port) = 0;
                item_swapped = item;
                MENUCALL(mChooseMsg, mh, &r, pt, &item_swapped);
                item = item_swapped;
                if(item != olditem || changedmenus)
                {
                    if(firstitem == -1)
                        firstitem = item;
                    changedmenus = false;
                    if((i = nmenusdisplayed - whichmenuhit))
                    {
                        restoren(i, restoredrgn, &r);
                        nmenusdisplayed -= i;
                        saveclip = PORT_CLIP_REGION(wmgr_port);

                        PORT_CLIP_REGION(wmgr_port) = NewRgn();
                        RectRgn(PORT_CLIP_REGION(wmgr_port), &r);

                        if(item == 0)
                        { /* may have been 'cause of */
                            pt.v = 32767; /* scrolling, in which case */
                            pt.h = 32767; /* we don't want to scroll */
                            /* a messed clip region */
                        }
                        item = olditem;
                        PORT_TX_FACE(wmgr_port) = (Style)0;
                        PORT_TX_FONT(wmgr_port) = 0;
                        item_swapped = item;
                        MENUCALL(mChooseMsg, mh, &r, pt, &item_swapped);
                        item = item_swapped;
                        DisposeRgn(PORT_CLIP_REGION(wmgr_port));
                        PORT_CLIP_REGION(wmgr_port) = saveclip;
                    }
                    if((newmh = itemishierarchical(mh, item, &tempi)))
                    {

                        r2 = *ptr_from_longint<Rect *>(MBDFCALL(mbRect, 0,
                                                                tempi | HIERRECTBIT));
                        MBDFCALL(mbSave, tempi, ptr_to_longint(&r2));
                        if(LM(MBarHook))
                        {
                            myd0 = CALLMBARHOOK(&r2, LM(MBarHook));
                            if(myd0 != 0)
                                break;
                        }
                        ROMlib_rootless_openmenu(r2);
                        ((mbdfentry *)*LM(MBSaveLoc))
                            [wheretowhich(tempi)]
                                .mbReserved
                            = 0;
                        auto oldtopmenuitem = LM(TopMenuItem);
                        auto saveatmenubottom = LM(AtMenuBottom);
                        LM(TopMenuItem) = r2.top;
                        PORT_TX_FACE(wmgr_port) = (Style)0;
                        PORT_TX_FONT(wmgr_port) = 0;
                        saveclip = PORT_CLIP_REGION(qdGlobals().thePort);
                        PORT_CLIP_REGION(qdGlobals().thePort) = NewRgn();
                        RectRgn(PORT_CLIP_REGION(qdGlobals().thePort), &r2);
                        MENUCALL(mDrawMsg, newmh, &r2, dummy_pt, nullptr);
                        DisposeRgn(PORT_CLIP_REGION(qdGlobals().thePort));
                        PORT_CLIP_REGION(qdGlobals().thePort) = saveclip;
                        nmenusdisplayed++;
                        MBDFCALL(mbSaveAlt, 0, tempi);
                        LM(TopMenuItem) = oldtopmenuitem;
                        LM(AtMenuBottom) = saveatmenubottom;
                    }
                    olditem = item;
                }
            }
        }
        else
        {
            if(pt.v < LM(MBarHeight) && !ispopup)
            {
                if(nmenusdisplayed)
                {
                    restoren(nmenusdisplayed, (RgnHandle)0, 0);
                    nmenusdisplayed = 0;
                }
                whichmenuhit = 0;
                if(where == NOTHITINMBAR)
                {
                    mh = nullptr;
                    HiliteMenu(0);
                }
                else
                {
                    mh = ((muelem *)((char *)*LM(MenuList) + where))->muhandle;
                    HiliteMenu((*mh)->menuID);
                    r = *ptr_from_longint<Rect *>(MBDFCALL(mbRect, 0, where));
                    MBDFCALL(mbSave, where, ptr_to_longint(&r));
                    if(LM(MBarHook))
                    {
                        myd0 = CALLMBARHOOK(&r, LM(MBarHook));
                        if(myd0 != 0)
                            break;
                    }
                    ROMlib_rootless_openmenu(r);                    
                    olditem = item = 0;
                    LM(TopMenuItem) = LM(MBarHeight);
                    PORT_TX_FACE(wmgr_port) = (Style)0;
                    PORT_TX_FONT(wmgr_port) = 0;
                    saveclip = PORT_CLIP_REGION(qdGlobals().thePort); /* ick */
                    PORT_CLIP_REGION(qdGlobals().thePort) = NewRgn();
                    RectRgn(PORT_CLIP_REGION(qdGlobals().thePort), &r);
                    MENUCALL(mDrawMsg, mh, &r, dummy_pt, nullptr);
                    DisposeRgn(PORT_CLIP_REGION(qdGlobals().thePort));
                    PORT_CLIP_REGION(qdGlobals().thePort) = saveclip;
                    nmenusdisplayed++;
                    whichmenuhit = wheretowhich(where);
                }
            }
            else if(where == NOTHIT || (ispopup && pt.v < LM(MBarHeight)))
            {
                if((i = nmenusdisplayed - whichmenuhit))
                {
                    restoren(i, (RgnHandle)0, 0);
                    nmenusdisplayed -= i;
                }
                if(mh)
                {
                    wmgr_port->txFace = (Style)0;
                    wmgr_port->txFont = 0;
                    item_swapped = item;
                    MENUCALL(mChooseMsg, mh, &r, pt, &item_swapped);
                    item = item_swapped;
                }
                else
                    item = 0;
                where = oldwhere;
                olditem = 0;
            }
            else
            {
                whichmenuhit = wheretowhich(where);
                newentry = (mbdfentry *)*LM(MBSaveLoc) + whichmenuhit;
                oldentry = (mbdfentry *)*LM(MBSaveLoc) + oldwhichmenuhit;
                oldentry->mbReserved = (ULONGINT)item;
                olditem = item = newentry->mbReserved;
                changedmenus = true;
                mh = ((muelem *)((char *)*LM(MenuList) + where))->muhandle;
                templ = where;
                if(where > (*MENULIST)->muoff)
                    templ |= HIERRECTBIT;
                r = *ptr_from_longint<Rect *>(MBDFCALL(mbRect, 0, templ));
                whichmenuhit = wheretowhich(where);
                MBDFCALL(mbSaveAlt, 0, oldwhere);
                MBDFCALL(mbResetAlt, 0, where);
            }
            oldwhere = where;
            oldwhichmenuhit = whichmenuhit;
        }
    }

    if(mh)
    {
        if(item)
        {
            mid = (*mh)->menuID;
            tempi = item;
            tempp.v = 0;
            tempp.h = 0;
            PORT_TX_FACE(wmgr_port) = (Style)0;
            PORT_TX_FONT(wmgr_port) = 0;
            item_swapped = tempi;
            MENUCALL(mChooseMsg, mh, &r, tempp, &item_swapped);
            tempi = item_swapped;
            if(LM(MenuFlash))
            {
                for(i = 0; i < LM(MenuFlash); i++)
                {
                    Delay(3L, nullptr);
                    PORT_TX_FACE(wmgr_port) = (Style)0;
                    PORT_TX_FONT(wmgr_port) = 0;
                    item_swapped = tempi;
                    MENUCALL(mChooseMsg, mh, &r, pt, &item_swapped);
                    tempi = item_swapped;
                    Delay(3L, nullptr);
                    PORT_TX_FACE(wmgr_port) = (Style)0;
                    PORT_TX_FONT(wmgr_port) = 0;
                    item_swapped = tempi;
                    MENUCALL(mChooseMsg, mh, &r, tempp, &item_swapped);
                    tempi = item_swapped;
                }
                Delay(3L, nullptr);
            }
        }
        else
            mid = 0;
        if(nmenusdisplayed)
            restoren(nmenusdisplayed, (RgnHandle)0, 0);
    }
    else
    {
        mid = 0;
        item = 0;
    }
    if(!mid)
        HiliteMenu(0);
    SetPort(saveport); /* Does SystemMenu() expect the LM(WMgrPort) also? */

    /* Illustrator 5.5 behavior suggests that hits on hierarchical menus
   * for non leaf-node menus are handled differently.  [PR #1683].
   * Returning the "correct" mid/item for a non-leaf-node menu causes
   * Illustrator to crash when a routine of theirs that looks up some
   * value fails to find a match and doesn't fill in a value via
   * reference.  This is a workaround, but it's known to be incorrect.
   * On a real Mac, choosing a non-leaf node menu for a font name in
   * the Character window switches to that font.  This patch, as well
   * as one trying to use newmh's menu id, causes such a selection to
   * be ignored.
   */
    newmh = itemishierarchical(mh, item, &tempi);
    if(newmh)
    {
        mid = 0;
        item = 0;
    }

    if(LM(MBarEnable))
    {
        SystemMenu(((LONGINT)mid << 16) | (unsigned short)item);
        return 0;
    }
    else
        return ((LONGINT)mid << 16) | (unsigned short)item;
}

LONGINT Executor::C_MenuSelect(Point p)
{
    Rect spooeyr;
    LONGINT retval;

    LM(TopMenuItem) = LM(MBarHeight);
    retval = ROMlib_menuhelper((MenuHandle)0, &spooeyr, 0, false, 0);
    return retval;
}

void Executor::C_FlashMenuBar(INTEGER mid)
{
    LONGINT l;

    if(mid == 0 || (l = ROMlib_mentosix(mid)) == -1)
        l = 0;
    else if(mid == LM(TheMenu))
    {
        l |= (LONGINT)RESTORE << 16;
        LM(TheMenu) = 0;
    }
    else
    {
        l |= (LONGINT)HILITE << 16;
        LM(TheMenu) = mid;
    }
    MBDFCALL(mbHilite, 0, l);
}

static Boolean findroot(INTEGER menuid, INTEGER *root_unswp)
{
    INTEGER loopcount, i, maxi;
    enum
    {
        noparent,
        hierparent,
        nonhierparent
    } partype;
    startendpairs mps;
    muelem *mp, *mpend;
    MenuHandle mh;
    unsigned char *p;
    mextp mxp;
    INTEGER mitem;

    initpairs(mps);
    maxi = mps[(int)hier].endp - mps[(int)nonhier].startp; /* upper bound */
    for(partype = hierparent, loopcount = 0;
        partype == hierparent && loopcount < maxi; loopcount++)
    {
        for(i = (int)nonhier; i <= (int)hier; i++)
        {
            for(mp = mps[i].startp, mpend = mps[i].endp; mp != mpend; mp++)
            {
                mh = mp->muhandle;
                p = (unsigned char *)*mh + SIZEOFMINFO + *(char *)((*mh)->menuData);
                mitem = 1;
                while(*p != 0)
                {
                    mxp = (mextp)(p + *p + 1);
                    if(mxp->mkeyeq == 0x1B && mxp->mmarker == menuid)
                    {
                        menuid = (*mh)->menuID;
                        partype = i == (int)hier ? hierparent : nonhierparent;
                        goto out;
                    }
                    mitem++;
                    p += *p + SIZEOFMEXT;
                }
            }
        }
    out:;
    }
    if(loopcount == maxi)
        gui_assert(0); /* should be SysError */
    if(partype == nonhierparent)
    {
        *root_unswp = menuid;
        /*-->*/ return true;
    }
    return false;
}

LONGINT Executor::C_MenuKey(CharParameter thec)
{
    muelem *mp, *mpend;
    startendpairs mps;
    unsigned char *p;
    MenuHandle mh;
    int mitem;
    mextp mxp;
    LONGINT e, retval;
    Byte c;
    INTEGER i, menuid;

    if(thec >= 0x1B && thec <= 0x1F)
        /*-->*/ return 0;
    c = thec;
    if(c >= 'a' && c <= 'z')
        c = 'A' + c - 'a';

    initpairs(mps);
    for(i = (int)nonhier; i <= (int)hier; i++)
    {
        for(mpend = mps[i].startp - 1, mp = mps[i].endp - 1; mp != mpend;
            mp--)
        {
            mh = mp->muhandle;
            p = (unsigned char *)*mh + SIZEOFMINFO + *(unsigned char *)((*mh)->menuData);
            mitem = 1;
            while(*p != 0)
            {
                mxp = (mextp)(p + *p + 1);
                if(mxp->mkeyeq == c && ((e = (*mh)->enableFlags) & 1) && e & ((LONGINT)1 << mitem))
                {
                    if(i == (int)nonhier)
                        menuid = (*mh)->menuID;
                    else if(!findroot((*mh)->menuID, &menuid))
                        /*-->*/ return 0L;
                    retval = ((LONGINT)(*mh)->menuID << 16) | (unsigned short)mitem;
                    FlashMenuBar(menuid);
                    if((*mh)->menuID < 0)
                    {
                        SystemMenu(retval);
                        retval = 0;
                    }
                    /*-->*/ return retval;
                }
                mitem++;
                p += *p + SIZEOFMEXT;
            }
        }
    }
    return (0L);
}

void Executor::C_SetMenuItemText(MenuHandle mh, INTEGER item, ConstStringPtr str)
{
    int oldsize, newsize, growth;
    Size hsize, nbyte;
    int start, soff;
    char *sb;
    StringPtr stashstring;

    if(ROMlib_mitemtop(mh, item, &stashstring))
    {
        soff = (char *)stashstring - (char *)*mh;
        oldsize = stashstring[0];
        newsize = str[0];
        if(oldsize != newsize)
        {
            growth = newsize - oldsize;
            hsize = GetHandleSize((Handle)mh);
            start = ((char *)stashstring + stashstring[0] + 1) - (char *)*mh;
            nbyte = hsize - start;
            hsize += growth;
            if(growth > 0)
            {
                SetHandleSize((Handle)mh, hsize);
                sb = (char *)*mh + start;
                BlockMoveData((Ptr)sb, (Ptr)sb + growth, (Size)nbyte);
            }
            else
            {
                sb = (char *)*mh + start;
                BlockMoveData((Ptr)sb, (Ptr)sb + growth, (Size)nbyte);
                SetHandleSize((Handle)mh, hsize);
            }
        }
        str255assign((char *)*mh + soff, str);
        dirtymenusize(mh);
    }
}

void Executor::C_GetMenuItemText(MenuHandle mh, INTEGER item, StringPtr str)
{
    StringPtr stashstring;

    if(ROMlib_mitemtop(mh, item, &stashstring))
        str255assign(str, stashstring);
    else
        str[0] = 0;
}

void Executor::C_DisableItem(MenuHandle mh, INTEGER item)
{
    if(mh)
        (*mh)->enableFlags = (*mh)->enableFlags & ~((LONGINT)1 << item);
}

void Executor::C_EnableItem(MenuHandle mh, INTEGER item)
{
    if(mh)
        (*mh)->enableFlags = (*mh)->enableFlags | (LONGINT)1 << item;
}

void Executor::C_CheckItem(MenuHandle mh, INTEGER item, Boolean cflag)
{
    if(mh)
    {
        mextp mep;

        if((mep = ROMlib_mitemtop(mh, item, (StringPtr *)0)))
            mep->mmarker = cflag ? checkMark : 0;
    }
}

void Executor::C_SetItemMark(MenuHandle mh, INTEGER item, CharParameter mark)
{
    mextp mep;

    if((mep = ROMlib_mitemtop(mh, item, (StringPtr *)0)))
        mep->mmarker = mark;
}

void Executor::C_GetItemMark(MenuHandle mh, INTEGER item, GUEST<INTEGER> *markp)
{
    mextp mep;

    if((mep = ROMlib_mitemtop(mh, item, (StringPtr *)0)))
        *markp = (INTEGER)(unsigned char)mep->mmarker;
}

void Executor::C_SetItemIcon(MenuHandle mh, INTEGER item, Byte icon)
{
    mextp mep;

    if((mep = ROMlib_mitemtop(mh, item, (StringPtr *)0)))
    {
        mep->micon = icon;
        dirtymenusize(mh);
    }
}

void Executor::C_GetItemIcon(MenuHandle mh, INTEGER item, GUEST<INTEGER> *iconp)
{
    mextp mep;

    if((mep = ROMlib_mitemtop(mh, item, (StringPtr *)0)))
        *iconp = (INTEGER)(unsigned char)mep->micon;
}

void Executor::C_SetItemStyle(MenuHandle mh, INTEGER item, INTEGER style)
{
    mextp mep;

    if((mep = ROMlib_mitemtop(mh, item, (StringPtr *)0)))
    {
        mep->mstyle = style;
        dirtymenusize(mh);
    }
}

void Executor::C_GetItemStyle(MenuHandle mh, INTEGER item,
                              GUEST<INTEGER> *stylep)
{
    mextp mep;

    if((mep = ROMlib_mitemtop(mh, item, (StringPtr *)0)))
        *stylep = (INTEGER)(unsigned char)mep->mstyle;
}

INTEGER Executor::C_CountMItems(MenuHandle mh)
{
    endinfo endinf;

    if(mh)
    {
        toend(mh, &endinf);
        /*-->*/ return endinf.menitem;
    }
    else
        return 0;
}

MenuHandle Executor::C_GetMenuHandle(INTEGER mid)
{
    MenuHandle retval;

    if(!LM(MenuList))
        retval = 0;
    else
    {
        muelem *mp, *mpend;

        mpend = (*MENULIST)->mulist + HIEROFF / sizeof(muelem);
        for(mp = FIRSTHIER;
            mp != mpend && (*mp->muhandle)->menuID != mid; mp++)
            ;
        if(mp != mpend)
            retval = mp->muhandle;
        else
        {
            mpend = (*MENULIST)->mulist + (*MENULIST)->muoff / sizeof(muelem);
            for(mp = (*MENULIST)->mulist;
                mp != mpend && (*mp->muhandle)->menuID != mid; mp++)
                ;
            if(mp != mpend)
                retval = mp->muhandle;
            else
                return (0);
        }
    }
    return retval;
}

void Executor::C_SetMenuFlash(INTEGER i)
{
    LM(MenuFlash) = i;
}

Boolean Executor::ROMlib_shouldalarm()
{
    return MENULIST && (*(*MENULIST)->mulist[0].muhandle)->menuData[0] == 1;
}

void Executor::ROMlib_menucall(INTEGER mess, MenuHandle themenu, Rect *menrect, Point hit,
                               GUEST<INTEGER> *which)
{
    Handle defproc;
    MenuDefUPP mp;

    defproc = (*themenu)->menuProc;

    if(defproc)
    {
        if(*defproc == nullptr)
            LoadResource(defproc);

        mp = (MenuDefUPP)*defproc;

        ROMlib_hook(menu_mdefnumber);
        HLockGuard guard(defproc);
        mp(mess, themenu, menrect, hit, which);
    }
}

LONGINT
Executor::ROMlib_mbdfcall(INTEGER msg, INTEGER param1, LONGINT param2)
{
    Handle defproc;
    LONGINT retval;
    MenuBarDefUPP mp;

    defproc = LM(MBDFHndl);

    if(*defproc == nullptr)
        LoadResource(defproc);

    mp = (MenuBarDefUPP)*defproc;

    ROMlib_hook(menu_mbdfnumber);
    HLockGuard guard(defproc);

    retval = mp((*MENULIST)->mufu & 7, msg, param1, param2);

    return retval;
}

int16_t Executor::C_GetMBarHeight()
{
    return LM(MBarHeight);
}