/* Copyright 1986, 1989, 1990 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in FontMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <ResourceMgr.h>
#include <CQuickDraw.h>
#include <QuickDraw.h>
#include <MemoryMgr.h>
#include <FontMgr.h>
#include <ToolboxUtil.h>
#include <OSUtil.h>
#include <quickdraw/cquick.h>
#include <quickdraw/font.h>

using namespace Executor;

#define MAXTABLES 12

static void
reset_myfmi(void)
{
    LM(ROMlib_myfmi).family = -1;
    LM(ROMlib_myfmi).size = -1;
    LM(ROMlib_myfmi).face = -1;
}

void Executor::C_InitFonts() /* IMI-222 */
{
    static Boolean beenhere = false;
    GUEST<THz> saveZone;

    if(!beenhere)
    {
        saveZone = LM(TheZone);
        LM(TheZone) = LM(SysZone);
        SetResLoad(true);
        LM(ROMFont0) = GetResource("FONT"_4, FONTRESID(systemFont, 12));
        LM(WidthListHand) = NewHandle(MAXTABLES * sizeof(GUEST<Handle>));
        memset(*LM(WidthListHand), 0, MAXTABLES * sizeof(GUEST<Handle>));
        LM(TheZone) = saveZone;
        beenhere = true;
    }
    LM(ApFontID) = LM(SPFont) + 1;
    LM(SysFontSiz) = 12;
    LM(SysFontFam) = 0;
#if 0
    /* 95-09-20: PAUP uses a built in font that has fried width tables.
       The bad tables won't hurt us if LM(FractEnable) is not set, so I ran
       a test program on the Mac and found that LM(FractEnable) wasn't set
       at the beginning of the program or even after InitFonts was called.
       This suggests that the hack for WordPerfect is wrong.  Perhaps
       there's some bit in the Size resource that controls this thing,
       perhpas the WordPerfect hack was always fried.  Yahoo. */

    LM(FractEnable) = 0xff;	/* new mod for WordPerfect */
#endif
    LM(FScaleDisable) = 0;
    reset_myfmi();
}

static void
invalidate_all_widths(void)
{
    int i, n_entries;
    GUEST<Handle> *hp;
    Handle wlh;

    wlh = LM(WidthListHand);
    HLock(wlh);
    n_entries = GetHandleSize(wlh) / sizeof(GUEST<Handle>);
    hp = (GUEST<Handle> *)*wlh;
    for(i = 0; i < n_entries; ++i)
    {
        DisposeHandle(hp[i]);
        hp[i] = nullptr;
    }
    HUnlock(wlh);
}

void
Executor::ROMlib_shutdown_font_manager(void)
{
    invalidate_all_widths();
}

void Executor::C_GetFontName(INTEGER fnum, StringPtr fnam) /* IMI-223 */
{
    Handle h;
    GUEST<INTEGER> i;
    GUEST<ResType> rest;

    if(fnum == systemFont)
        fnum = LM(SysFontFam);
    else if(fnum == applFont)
        fnum = LM(ApFontID);
    SetResLoad(false);
    h = GetResource("FONT"_4, FONTRESID(fnum, 0));
    if(!h)
        h = GetResource("NFNT"_4, FONTRESID(fnum, 0));
    if(!h)
        h = GetResource("FOND"_4, fnum);
    GetResInfo(h, &i, &rest, fnam);
    if(ResError())
        *fnam = 0;
    SetResLoad(true);
}

void Executor::C_GetFNum(ConstStringPtr fnam, GUEST<INTEGER> *fnum) /* IMI-223 */
{
    Handle h;
    GUEST<ResType> rest;
    Boolean shift;

    SetResLoad(false);

    h = GetNamedResource("FOND"_4, fnam);
    if(h)
        shift = false;
    else
    {
        shift = true;
        h = GetNamedResource("FONT"_4, fnam);
        if(!h)
            h = GetNamedResource("NFNT"_4, fnam);
    }

    GetResInfo(h, fnum, &rest, nullptr);
    if(ResError())
        *fnum = 0;
    else if(shift)
        *fnum = *(GUEST<uint16_t> *)fnum >> 7;
    SetResLoad(true);
}

void Executor::C_SetFontLock(Boolean lflag) /* IMI-223 */
{
    INTEGER attrs;

    attrs = GetResAttrs(LM(ROMlib_fmo).fontHandle);
    if(lflag)
    {
        LoadResource(LM(ROMlib_fmo).fontHandle);
        SetResAttrs(LM(ROMlib_fmo).fontHandle, attrs & ~resPurgeable);
    }
    else
    { /* TODO: is the next line necessary */
        SetResAttrs(LM(ROMlib_fmo).fontHandle, attrs | resPurgeable);
    }
}

typedef signed char ctrip[3];
typedef struct
{
    INTEGER dpiv, dpih;
    ctrip boldt, italt, nat, outt, shadt, condt, extt, undert;
} fchartstr;

static fchartstr ftstr = {
    80, 80, /* dpiv, dpih */
    {
        0, 1, 1,
    }, /* boldt */
    {
        1, 8, 0,
    }, /* italt */
    {
        0, 0, 0,
    }, /* Not Used */
    {
        5, 1, 1,
    }, /* outt */
    {
        5, 2, 2,
    }, /* shadt */
    {
        0, 0, -1,
    }, /* condt */
    {
        0, 0, 1,
    }, /* extt */
    {
        1, 1, 1,
    }, /* undert */
};

static void mungfmo(ctrip, FMOutput *);
static Boolean widthlistmatch(FMInput *);
static int countones(unsigned short);
static GUEST<INTEGER> *findfondwidths();
static void buildtable(INTEGER);
static void findclosestfont(INTEGER family, INTEGER size, INTEGER *lesserp,
                            INTEGER *greaterp);
static void findclosestfond(FHandle fh, INTEGER size, INTEGER *powerof2p,
                            INTEGER *lesserp, INTEGER *greaterp);
static INTEGER closestface();
static void newwidthtable(FMInput *fmip);

static void mungfmo(ctrip cp, FMOutput *fmop)
{
    Byte *p;
    INTEGER i;

    p = &(fmop->bold);
    i = cp[1];
    if(i & 0x80)
        i |= 0xFF00;
    *(p + cp[0]) += i;
    i = cp[2];
    if(i & 0x80)
        i |= 0xFF00;
    fmop->extra = fmop->extra + i;
}

#define MAXTABLES 12

static Boolean widthlistmatch(FMInput *fmip)
{

    GUEST<WHandle> *whp, *ewhp;

    for(whp = *WIDTHLISTHAND, ewhp = whp + MAXTABLES; whp != ewhp; whp++)
    {
        if(*whp && (LONGINT)(*whp).raw() != (LONGINT)-1)
        {
            LM(WidthPtr) = *(*whp);
            if(WIDTHPTR->aFID == fmip->family && WIDTHPTR->aSize == fmip->size && (char)WIDTHPTR->aFace == (char)fmip->face && WIDTHPTR->device == fmip->device && WIDTHPTR->inNumer.h == fmip->numer.h && WIDTHPTR->inNumer.v == fmip->numer.v && WIDTHPTR->inDenom.h == fmip->denom.h && WIDTHPTR->inDenom.v == fmip->denom.v && WIDTHPTR->fSize != -1 && WIDTHPTR->fSize != 0 && !WIDTHPTR->usedFam == !LM(FractEnable))
            {
                LM(WidthTabHandle) = guest_cast<WidthTableHandle>(*whp);
                HLock((Handle)LM(WidthTabHandle));
                return true;
            }
        }
    }
    return false;
}

static int countones(unsigned short i)
{
    int retval;
    static int counts[] = {
        0, /* 0 0 0 0 */
        1, /* 0 0 0 1 */
        1, /* 0 0 1 0 */
        2, /* 0 0 1 1 */
        1, /* 0 1 0 0 */
        2, /* 0 1 0 1 */
        2, /* 0 1 1 0 */
        3, /* 0 1 1 1 */
        1, /* 1 0 0 0 */
        2, /* 1 0 0 1 */
        2, /* 1 0 1 0 */
        3, /* 1 0 1 1 */
        2, /* 1 1 0 0 */
        3, /* 1 1 0 1 */
        3, /* 1 1 1 0 */
        4, /* 1 1 1 1 */
    };

    retval = counts[i & 0xF];
    i >>= 4;
    retval += counts[i & 0xF];
    i >>= 4;
    retval += counts[i & 0xF];
    i >>= 4;
    return retval + counts[i & 0xF];
}

static INTEGER nhappybits(unsigned short want, unsigned short have)
{
    return (have & ~want) ? -1 : countones(have);
}

#define WIDTHBIT (1 << 1)

static GUEST<INTEGER> *findfondwidths()
{
    GUEST<INTEGER> *retval, *numentriesminusone;
    LONGINT offset;
    INTEGER bitsmatched, newbits;
    INTEGER want;
    INTEGER tabsize;
    widentry_t *widp;
    INTEGER i;

    retval = 0;
    if((*(FHandle)LM(LastFOND))->ffFamID == WIDTHPTR->aFID && (offset = (*(FHandle)LM(LastFOND))->ffWTabOff))
    {
        bitsmatched = -1;
        want = WIDTHPTR->aFace;
        /*
 * NOTE: we add 3 to lastchar - firstchar to include the missing character
 *	 entry and what appears to be a zero entry located thereafter.
 */
        tabsize = ((*(FHandle)LM(LastFOND))->ffLastChar - (*(FHandle)LM(LastFOND))->ffFirstChar + 3)
                * sizeof(INTEGER)
            + sizeof(INTEGER);
        numentriesminusone = (GUEST<INTEGER> *)((char *)&(*(FHandle)LM(LastFOND))->ffFlags + offset);
        i = *numentriesminusone + 1;
        widp = (widentry_t *)(numentriesminusone + 1);
        for(; --i >= 0; widp = (widentry_t *)((char *)widp + tabsize))
        {
            newbits = nhappybits(want, widp->style);
            if(newbits > bitsmatched)
            {
                retval = widp->table;
                bitsmatched = newbits;
            }
        }
    }

    return retval;
}

#define FIXED(n) ((LONGINT)(n) << 16)
#define FIXED8(n) ((LONGINT)((unsigned short)n) << 8)
#define FIXED4(n) (((LONGINT)((unsigned short)n) << 4) * (WIDTHPTR->aSize))

Fixed
Executor::font_width_expand(Fixed width, Fixed fixed_extra, Fixed hOutputInverse)
{
    Fixed retval;

    retval = FixMul(width + fixed_extra, hOutputInverse);
    return retval;
}

typedef enum { FontFract,
               FontInt,
               FondFract } howtobuild_t;

static void buildtabdata(howtobuild_t howtobuild, INTEGER extra,
                         GUEST<INTEGER> *fondwidthtable)
{
    INTEGER c, firstchar, lastchar;
    GUEST<INTEGER> *widp, width;
    GUEST<Fixed> *p, *ep;
    Fixed misswidth, hOutputInverse, fixed_extra;
    FontRec *fp;

    fp = *(GUEST<FontRec *> *)WIDTHPTR->tabFont;
    firstchar = fp->firstChar;
    lastchar = fp->lastChar;

    switch(howtobuild)
    {
        case FontFract:
            WIDTHPTR->usedFam = 0; /* don't really know what this should be */
            widp = &fp->owTLoc + fp->owTLoc + (lastchar - firstchar + 2);
            fixed_extra = FIXED(extra);
            misswidth = FIXED8(widp[lastchar - firstchar + 1]) + fixed_extra;

            /* NOTE: this assumes missing characters have the missing width in the
	 table.  If this is wrong we need to look for missing characters
	 explictly */

            for(c = 0, p = WIDTHPTR->tabData, ep = p + 256; p < ep; c++)
            {
                if(c < firstchar || c > lastchar)
                    *p++ = misswidth;
                else
                    *p++ = FIXED8(*widp++) + fixed_extra;
            }
            break;
        case FontInt:
            WIDTHPTR->usedFam = 0;
            widp = &fp->owTLoc + fp->owTLoc;
            misswidth = FIXED((widp[lastchar - firstchar + 1] & 0xFF) + extra);
            for(c = 0, p = WIDTHPTR->tabData, ep = p + 256; p < ep; c++)
            {
                if(c < firstchar || c > lastchar)
                    *p++ = misswidth;
                else
                {
                    *p++ = (width = *widp++) == -1 ? misswidth
                                                        : FIXED((width & 0xFF) + extra);
                }
            }
            break;
        case FondFract:
            WIDTHPTR->usedFam = -1;
            hOutputInverse = FixRatio(1 << 8, WIDTHPTR->hOutput);
            widp = fondwidthtable;
            fixed_extra = FIXED(extra);
            misswidth = FIXED4(widp[lastchar - firstchar + 1]);
            misswidth = font_width_expand(misswidth, fixed_extra, hOutputInverse);
            for(c = 0, p = WIDTHPTR->tabData, ep = p + 256; p < ep; c++)
            {
                if(c < firstchar || c > lastchar)
                    *p++ = misswidth;
                else
                {
                    *p++ = FixMul(FIXED4(*widp) + fixed_extra,
                                     hOutputInverse);
                    widp++;
                }
            }
            break;
    }
    WIDTHPTR->tabData[(unsigned)' '] = WIDTHPTR->tabData[(unsigned)' '] + WIDTHPTR->sExtra;
}

static void buildtable(INTEGER extra)
{
    howtobuild_t howtobuild;
    GUEST<INTEGER> *fondwidthtable;
    Fixed tempfix;
    INTEGER numerh, numerv, denomh, denomv;

#if !defined(LETGCCWAIL)
    fondwidthtable = 0;
#endif
    if(!LM(FractEnable))
        howtobuild = FontInt;
    else if((*(GUEST<FontRec *> *)WIDTHPTR->tabFont)->fontType & WIDTHBIT)
    {
        howtobuild = FontFract;
        extra = 0;
    }
    /*
 * NOTE: The sleazy hack for checking for geneva below.  This is because we
 *	 used to use a System file that didn't have widths for geneva and
 *	 everything worked fine.  Now we have widths for geneva, but they
 *	 appear to be too wide.  I don't have time to figure out what's
 *	 going on here, but since geneva is a toy font, I'm not too concerned
 *	 right now.  The trouble that the new sizes cause is in the Word 5.1
 *	 ribbon control labels.
 */
    else if(WIDTHPTR->aFID != kFontIDGeneva && (fondwidthtable = findfondwidths()))
    {
        howtobuild = FondFract;
        extra = 0;
    }
    else
        howtobuild = FontInt;
    numerh = WIDTHPTR->inNumer.h;
    numerv = WIDTHPTR->inNumer.v;
    denomh = WIDTHPTR->inDenom.h;
    denomv = WIDTHPTR->inDenom.v;
    if(WIDTHPTR->fSize == WIDTHPTR->aSize)
    {
        WIDTHPTR->hOutput = FixRatio(numerh, denomh) >> 8;
        WIDTHPTR->vOutput = FixRatio(numerv, denomv) >> 8;
        WIDTHPTR->hFactor = WIDTHPTR->vFactor = 256;
    }
    else if(WIDTHPTR->fSize < WIDTHPTR->aSize && LM(FScaleDisable))
    {
        WIDTHPTR->hOutput = FixRatio(numerh, denomh) >> 8;
        WIDTHPTR->vOutput = FixRatio(numerv, denomv) >> 8;
        WIDTHPTR->hFactor = WIDTHPTR->vFactor = FixRatio(WIDTHPTR->aSize,
                                                            WIDTHPTR->fSize)
                                                   >> 8;
    }
    else
    {
        tempfix = FixRatio(WIDTHPTR->aSize, WIDTHPTR->fSize);
        WIDTHPTR->hOutput = FixMul(tempfix, FixRatio(numerh, denomh)) >> 8;
        WIDTHPTR->vOutput = FixMul(tempfix, FixRatio(numerv, denomv)) >> 8;
        WIDTHPTR->hFactor = WIDTHPTR->vFactor = 256;
    }
#if 0 /* WTF is going on here? */
    WIDTHPTR->style = extra;
#endif

    buildtabdata(howtobuild, extra, fondwidthtable);
}

static void findclosestfont(INTEGER family, INTEGER size, INTEGER *lesserp,
                            INTEGER *greaterp)
{
    INTEGER i, lesser, greater, newsize, nres;
    GUEST<ResType> rest;
    GUEST<INTEGER> id_s;
    Handle h;

    lesser = 0;
    greater = 32767;
    SetResLoad(false);
    nres = CountResources("FONT"_4); /* how about NFNT? */
    for(i = 1; i <= nres; i++)
    {
        h = GetIndResource("FONT"_4, i);
        GetResInfo(h, &id_s, &rest, (StringPtr)0);
        INTEGER id = id_s.get();
        if(((unsigned short)id >> 7) == family)
        { // ### WTF ENDIAN
            if((newsize = id & ((1 << 7) - 1)) <= size)
            {
                if(newsize > lesser)
                    lesser = newsize;
            }
            else
            {
                if(newsize < greater)
                    greater = newsize;
            }
        }
    }
    SetResLoad(true);
    *lesserp = lesser;
    *greaterp = greater == 32767 ? 0 : greater;
}

static void findclosestfond(FHandle fh, INTEGER size, INTEGER *powerof2p,
                            INTEGER *lesserp, INTEGER *greaterp)
{
    fatabentry *p, *ep;
    INTEGER newsize;
    INTEGER powerof2, lesser, greater;
    GUEST<INTEGER> *ip;

    powerof2 = 0;
    lesser = 0;
    greater = 32767;
    ip = &(*fh)->ffVersion + 1;
    for(p = (fatabentry *)(ip + 1), ep = p + *ip + 1; p < ep; p++)
    {
        newsize = p->size;
        if(newsize < size)
        {
            if(newsize == size / 2)
                powerof2 = newsize; /* lower priority than  *2, but */
            /* we know these entries are ordered */
            if(newsize > lesser)
                lesser = newsize;
        }
        else if(newsize == size)
        {
            powerof2 = newsize;
            /*-->*/ break;
        }
        else
        {
            if(newsize == size * 2)
                powerof2 = newsize;
            if(newsize < greater)
                greater = newsize;
        }
    }
    *powerof2p = powerof2;
    *lesserp = lesser;
    *greaterp = greater == 32767 ? 0 : greater;
}

Boolean Executor::C_RealFont(INTEGER fnum, INTEGER sz) /* IMI-223 */
{
    Handle h;
    int retval;

    SetResLoad(false);
    h = GetResource("FONT"_4, FONTRESID(fnum, sz));
    if(!h)
        h = GetResource("NFNT"_4, FONTRESID(fnum, sz));
    retval = !!h;
    SetResLoad(true);
    if(!retval)
    {
        FHandle fh;

        fh = (FHandle)GetResource("FOND"_4, fnum);
        if(fh)
        {
            INTEGER powerof2, lesser, greater;
            findclosestfond(fh, sz, &powerof2, &lesser, &greater);
            if(powerof2 == sz)
                retval = true;
        }
    }
    return retval;
}

static INTEGER closestface() /* no args, uses WIDTHPTR */
{
    fatabentry *p, *ep, *bestp;
    INTEGER size;
    GUEST<INTEGER> *ip;
    INTEGER nmatch, want, newmatch;

    bestp = 0;
    size = WIDTHPTR->fSize;
    ip = &(*(FHandle)LM(LastFOND))->ffVersion + 1;
    nmatch = -1;
    want = WIDTHPTR->aFace;
    for(p = (fatabentry *)(ip + 1), ep = p + *ip + 1;
        p < ep && p->size <= size; p++)
    {
        if(!bestp)
            bestp = p; /* pick something */
        if((p->size == size) && !(p->style & ~want))
        {
            if((newmatch = countones(p->style & want)) >= nmatch)
            {
                /* note, IMIV is vague on precedence, so I choose the
			 last entry, though all I need is some comparison
			 function for bestp->style and p->style */
                nmatch = newmatch;
                bestp = p;
            }
        }
    }
    WIDTHPTR->face = bestp->style;
    return bestp->fontresid;
}

static FHandle
at_least_one_fond_entry(INTEGER family)
{
    FHandle retval;

    retval = (FHandle)GetResource("FOND"_4, family);
    if(retval)
    {
        INTEGER powerof2, lesser, greater;
        LoadResource((Handle)retval);
        findclosestfond(retval, 1, &powerof2, &lesser, &greater);
        if(greater == 0)
            retval = 0;
    }
    return retval;
}

/*
 * TODO:  NFNT below
 */

#define AVAILABLE(x) (WIDTHPTR->fSize = (x), WIDTHPTR->tabFont = GetResource("FONT"_4, FONTRESID(family, (x))))

static void newwidthtable(FMInput *fmip)
{
    FHandle fh;
    INTEGER lesser, greater, wanted_family, family, fontresid, powerof2;
    LONGINT todelete;
    GUEST<THz> savezone;
    bool tried_app_font;
    int n_tried_sys_font;

    savezone = LM(TheZone);
    LM(TheZone) = LM(SysZone);
    LM(WidthTabHandle) = (WidthTableHandle)NewHandle((Size)sizeof(WidthTable));
    LM(TheZone) = savezone;
    Munger((Handle)LM(WidthListHand), (LONGINT)0, (Ptr)0, 0,
           (Ptr)&LM(WidthTabHandle), sizeof(LM(WidthTabHandle)));

    /*
 * NOTE: the code below is a bit confusing.  What is going on is we always want
 *       to put the new entry in the front, but if we've grown too large then
 *	 we try to find a (Handle) 0 or a (Handle) -1) to delete.  If we find
 *	 neither we delete the last entry.
 */

    if(GetHandleSize((Handle)LM(WidthListHand)) > (int)sizeof(GUEST<Handle>) * MAXTABLES)
    {
        todelete = 0;
        if(Munger((Handle)LM(WidthListHand), (LONGINT)0, (Ptr)&todelete,
                  sizeof(todelete), (Ptr) "", 0)
           < 0)
        {
            todelete = -1;
            if(Munger((Handle)LM(WidthListHand), (LONGINT)0, (Ptr)&todelete,
                      sizeof(todelete), (Ptr) "", 0)
               < 0)
            {
                DisposeHandle((Handle)(*WIDTHLISTHAND)[MAXTABLES]);
                SetHandleSize(LM(WidthListHand),
                              (Size)sizeof(GUEST<Handle>) * MAXTABLES);
            }
        }
    }
    HLock((Handle)LM(WidthTabHandle));
    LM(WidthPtr) = *LM(WidthTabHandle);

    WIDTHPTR->sExtra = PORT_SP_EXTRA(qdGlobals().thePort);
    WIDTHPTR->inNumer = fmip->numer;
    WIDTHPTR->inDenom = fmip->denom;
    WIDTHPTR->aFID = fmip->family;
    WIDTHPTR->aFace = fmip->face;
    WIDTHPTR->aSize = fmip->size;
    WIDTHPTR->device = fmip->device;
    WIDTHPTR->tabSize = 0; /* I don't know what this field is, but it */
    /* appears to have flags in it */

    SetResLoad(true);
    fh = 0;
    wanted_family = fmip->family;
    tried_app_font = false;
    n_tried_sys_font = 0;
    if((fh = at_least_one_fond_entry(wanted_family))
       || GetResource("FONT"_4, FONTRESID(wanted_family, fmip->size))
       || GetResource("FONT"_4, FONTRESID(wanted_family, 0)))
        family = wanted_family;
    else if((fh = at_least_one_fond_entry(LM(ApFontID)))
            || GetResource("FONT"_4, FONTRESID(LM(ApFontID), 0)))
    {
        family = LM(ApFontID);
        tried_app_font = true;
    }
    else
    {
        fh = at_least_one_fond_entry(LM(SysFontFam));
        family = LM(SysFontFam);
        tried_app_font = true;
        ++n_tried_sys_font;
    }
    WIDTHPTR->tabFont = 0;
    while(!WIDTHPTR->tabFont && n_tried_sys_font < 2)
    {
        WIDTHPTR->fHand = (Handle)fh;
        WIDTHPTR->fID = family;
        if(fh)
        {
            LM(LastFOND) = (FamRecHandle)fh;

            findclosestfond(fh, fmip->size, &powerof2, &lesser, &greater);
            if(powerof2 == fmip->size)
                WIDTHPTR->fSize = powerof2;
            else
            {
                if(LM(FScaleDisable))
                    WIDTHPTR->fSize = lesser ? lesser : greater;
                else
                {
                    if(powerof2)
                        WIDTHPTR->fSize = powerof2;
                    else
                    {
                        if(!greater || (lesser && fmip->size - lesser <= greater - fmip->size))
                            WIDTHPTR->fSize = lesser;
                        else
                            WIDTHPTR->fSize = greater;
                    }
                }
            }
            if(!WIDTHPTR->fSize)
                fprintf(stderr, "fh:  fSize = 0\n");
            fontresid = closestface();
            if(!(WIDTHPTR->tabFont = GetResource("NFNT"_4, fontresid)))
                WIDTHPTR->tabFont = GetResource("FONT"_4, fontresid);
            if(!WIDTHPTR->tabFont)
                WIDTHPTR->tabFont = GetResource("FONT"_4,
                                                   FONTRESID(family, WIDTHPTR->fSize));
            if(!WIDTHPTR->tabFont)
                warning_unexpected("");
        }
        if(!WIDTHPTR->tabFont)
        {
            WIDTHPTR->face = 0;

            if(!AVAILABLE(fmip->size))
            {
                if(LM(FScaleDisable))
                {
                    findclosestfont(family, fmip->size, &lesser, &greater);
                    WIDTHPTR->fSize = lesser ? lesser : greater;
                    WIDTHPTR->tabFont = GetResource("FONT"_4,
                                                       FONTRESID(family, WIDTHPTR->fSize));
                }
                else
                {
                    if(!AVAILABLE(fmip->size * 2) && !AVAILABLE(fmip->size / 2))
                    {
                        findclosestfont(family, fmip->size, &lesser, &greater);
                        if(lesser && (fmip->size - lesser <= greater - fmip->size))
                            WIDTHPTR->fSize = lesser;
                        else
                            WIDTHPTR->fSize = greater;
                        WIDTHPTR->tabFont = GetResource("FONT"_4,
                                                           FONTRESID(family, WIDTHPTR->fSize));
                    }
                }
            }
        }
        if(!WIDTHPTR->tabFont)
        {
            if(!tried_app_font)
            {
                tried_app_font = true;
                family = LM(ApFontID);
                fh = at_least_one_fond_entry(family);
            }
            else if(n_tried_sys_font < 2)
            {
                ++n_tried_sys_font;
                family = LM(SysFontFam);
                fh = at_least_one_fond_entry(family);
            }
        }
    }
    LoadResource(WIDTHPTR->tabFont);
    if(!WIDTHPTR->fSize)
        warning_unexpected("!fh:  fSize = 0, family = %d\n",
                           (LONGINT)fmip->family);
}

FMOutPtr Executor::C_FMSwapFont(FMInput *fmip) /* IMI-223 */
{
    Style style;
    FontRec *fp;
    GUEST<INTEGER> savesize, savefamily;
    Boolean needtobuild;

    savesize = fmip->size;
    if(fmip->size <= 0)
        fmip->size = LM(SysFontSiz);

    if(fmip->size <= 0)
        fmip->size = 12;

    if(fmip->denom.h == fmip->numer.h)
    {
        fmip->denom.h = 256;
        fmip->numer.h = 256;
    }
    if(fmip->denom.v == fmip->numer.v)
    {
        fmip->denom.v = 256;
        fmip->numer.v = 256;
    }

    if(fmip->family == applFont)
    {
        savefamily = fmip->family;
        fmip->family = LM(ApFontID);
    }
    else
        savefamily = fmip->family;

    if(fmip->family == 0)
    {
        savefamily = 0;
        fmip->family = LM(SysFontFam);
    }

    if(fmip->family != LM(ROMlib_myfmi).family || fmip->size != LM(ROMlib_myfmi).size || fmip->face != LM(ROMlib_myfmi).face || fmip->device != LM(ROMlib_myfmi).device || fmip->numer.h != LM(ROMlib_myfmi).numer.h || fmip->numer.v != LM(ROMlib_myfmi).numer.v || fmip->denom.h != LM(ROMlib_myfmi).denom.h || fmip->denom.v != LM(ROMlib_myfmi).denom.v || WIDTHPTR->fSize == -1 || WIDTHPTR->fSize == 0 || (fmip->needBits && !LM(ROMlib_myfmi).needBits))
    {
        if((needtobuild = !widthlistmatch(fmip)))
            newwidthtable(fmip);

        LM(ROMlib_myfmi) = *fmip;
        LM(ROMlib_fmo).errNum = 0;
        LM(ROMlib_fmo).bold = 0;
        LM(ROMlib_fmo).italic = 0;
        LM(ROMlib_fmo).ulOffset = 0;
        LM(ROMlib_fmo).ulShadow = 0;
        LM(ROMlib_fmo).ulThick = 0;
        LM(ROMlib_fmo).shadow = 0;
        LM(ROMlib_fmo).extra = 0;
        LM(ROMlib_fmo).numer.h = 
            (LONGINT)fmip->numer.h * 256 * fmip->size / fmip->denom.h / WIDTHPTR->fSize;
        LM(ROMlib_fmo).numer.v = 
            (LONGINT)fmip->numer.v * 256 * fmip->size / fmip->denom.v / WIDTHPTR->fSize;
        LM(ROMlib_fmo).denom.h = 256;
        LM(ROMlib_fmo).denom.v = 256;
        LM(ROMlib_fmo).fontHandle = WIDTHPTR->tabFont;
        LoadResource(LM(ROMlib_fmo).fontHandle);
        fp = (FontRec *)*((LM(ROMlib_fmo).fontHandle));
        style = fmip->face ^ WIDTHPTR->face;
        if(style & (int)bold)
            mungfmo(ftstr.boldt, &LM(ROMlib_fmo));
        if(style & (int)italic)
            mungfmo(ftstr.italt, &LM(ROMlib_fmo));
        if(style & (int)outline)
            mungfmo(ftstr.outt, &LM(ROMlib_fmo));
        if(style & (int)shadow)
            mungfmo(ftstr.shadt, &LM(ROMlib_fmo));
        if(style & (int)condense)
            mungfmo(ftstr.condt, &LM(ROMlib_fmo));
        if(style & (int)extend)
            mungfmo(ftstr.extt, &LM(ROMlib_fmo));
        if(style & (int)underline)
        {
            LM(ROMlib_fmo).ulOffset += ftstr.undert[0];
            LM(ROMlib_fmo).ulShadow += ftstr.undert[1];
            LM(ROMlib_fmo).ulThick += ftstr.undert[2];
        }
        if(needtobuild)
            buildtable(LM(ROMlib_fmo).extra);
        /*
	 * TODO:  examine the stuff below, particularly widMax.  I don't think
	 *	  widMax will be correct if scaling is disabled and we expand
	 *	  characters.  This will cause serious trouble when we allocate
	 *	  too small a bitmap
	 */
        LM(ROMlib_fmo).ascent = fp->ascent + !!LM(ROMlib_fmo).shadow;
        LM(ROMlib_fmo).descent = fp->descent + LM(ROMlib_fmo).shadow;
        LM(ROMlib_fmo).widMax = fp->widMax + !!LM(ROMlib_fmo).shadow + LM(ROMlib_fmo).shadow + LM(ROMlib_fmo).bold;
        LM(ROMlib_fmo).leading = fp->leading;
    }
    else
    {
        LoadResource(LM(ROMlib_fmo).fontHandle);
        LM(WidthPtr) = *LM(WidthTabHandle);
    }
    fmip->size = savesize;
    fmip->family = savefamily;
    HUnlock((Handle)LM(WidthTabHandle));
    LM(LastFOND) = guest_cast<FamRecHandle>(WIDTHPTR->fHand);
    return &LM(ROMlib_fmo);
}

#define SCALE(x) FixRatio(x *fmop->numer.v, fmop->denom.v)

void Executor::C_FontMetrics(FMetricRec *metrp) /* IMIV-32 */
{
    FMInput fmi;
    FMOutPtr fmop;

    fmi.family = PORT_TX_FONT(qdGlobals().thePort);
    fmi.size = PORT_TX_SIZE(qdGlobals().thePort);
    fmi.face = PORT_TX_FACE(qdGlobals().thePort);
    fmi.needBits = false;
    fmi.device = PORT_DEVICE(qdGlobals().thePort);
    fmi.numer.v = 1;
    fmi.numer.h = 1;
    fmi.denom.v = 1;
    fmi.denom.h = 1;

    fmop = FMSwapFont(&fmi);

    /* TODO:  check out qdGlobals().thePort->device and use the FOND stuff if not
	      going to the screen */

    metrp->ascent = SCALE(fmop->ascent);
    metrp->descent = SCALE(fmop->descent);
    metrp->leading = SCALE(fmop->leading);
    metrp->widMax = SCALE(fmop->widMax);
    metrp->wTabHandle = guest_cast<Handle>(LM(WidthTabHandle));
}

void Executor::C_SetFScaleDisable(Boolean disable) /* IMIV-32 */
{
    if(LM(FScaleDisable) != (Byte)disable)
    {
        LM(FScaleDisable) = disable;
        invalidate_all_widths();
        reset_myfmi();
    }
}

void Executor::C_SetFractEnable(Boolean enable) /* IMIV-32 */
{
    LM(FractEnable) = enable;
}
