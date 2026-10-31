/* Copyright 1986 - 1996 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in OSEvent.h (DO NOT DELETE THIS LINE) */

/*
 * really should be divided into two sections, just like main.c (that
 * is to say just like main.c should be, it isn't yet)
 */

#include <base/common.h>

#include <MemoryMgr.h>
#include <ToolboxEvent.h>
#include <OSEvent.h>
#include <EventMgr.h>
#include <OSUtil.h>
#include <ResourceMgr.h>
#include <ProcessMgr.h>
#include <AppleEvents.h>

#include <mman/mman.h>
#include <prefs/prefs.h>
#include <vdriver/vdriver.h>
#include <rsys/toolevent.h>
#include <osevent/osevent.h>
#include <rsys/osutil.h>
#include <vdriver/dirtyrect.h>
#include <time/syncint.h>

#include <util/string.h>
#include <rsys/keyboard.h>

#include <SegmentLdr.h>

#include <vdriver/eventrecorder.h>

using namespace Executor;

#define NEVENT 20

static int nevent = 0;
static EvQEl evs[NEVENT], *freeelem = evs + NEVENT - 1;

static LONGINT autoticks;
static LONGINT lastdown = -1;

static Ptr kchr_ptr;

void
Executor::invalidate_kchr_ptr(void)
{
    kchr_ptr = 0;
}

static INTEGER kchr_id = 0;

Ptr
Executor::ROMlib_kchr_ptr(void)
{
    if(!kchr_ptr)
    {
        TheZoneGuard guard(LM(SysZone));
        Handle kchr_hand;

        kchr_hand = GetResource("KCHR"_4, kchr_id);
        gui_assert(kchr_hand);
        LoadResource(kchr_hand);
        HLock(kchr_hand);
        kchr_ptr = *kchr_hand;
    }
    return kchr_ptr;
}

bool
Executor::ROMlib_set_keyboard(const char *keyboardname)
{
    Handle new_h;

    TheZoneGuard guard(LM(SysZone));
    Str255 pkeyboardname;

    str255_from_c_string(pkeyboardname, keyboardname);
    new_h = GetNamedResource("KCHR"_4, pkeyboardname);
    if(new_h)
    {
        GUEST<INTEGER> tmpid;
        GetResInfo(new_h, &tmpid, 0, 0);
        kchr_id = tmpid;
        LoadResource(new_h);
        if(kchr_ptr)
        {
            Handle kchr_hand;

            kchr_hand = RecoverHandle(kchr_ptr);
            HUnlock(kchr_hand);
        }
        HLock(new_h);
        kchr_ptr = *new_h;
    }
    return !!new_h;
}

static bool map_right_to_left = true;

uint16_t
Executor::ROMlib_right_to_left_key_map(uint16_t what)
{
    uint16_t retval;

    retval = what;
    if(map_right_to_left)
        switch(what)
        {
            default:
                break;
            case MKV_RIGHTSHIFT:
                retval = MKV_LEFTSHIFT;
                break;
            case MKV_RIGHTOPTION:
                retval = MKV_LEFTOPTION;
                break;
            case MKV_RIGHTCNTL:
                retval = MKV_LEFTCNTL;
                break;
        }
    return retval;
}

void Executor::ROMlib_eventinit() /* INTERNAL */
{
    static int beenhere = 0;
    EvQEl *p, *ep;

    if(!beenhere)
    {
        LM(MouseLocation).h = 0;
        LM(MouseLocation).v = 0;
        LM(MouseLocation2).h = 0;
        LM(MouseLocation2).v = 0;
        LM(ScrDmpEnb) = true;
        evs[0].qLink = 0; /* end of the line */
        beenhere = 1;
        for(p = evs + 1, ep = evs + NEVENT; p != ep; p++)
            p->qLink = (QElemPtr)(p - 1);
        LM(SysEvtMask) = ~(1L << keyUp); /* EVERYTHING except keyUp */
    }
}

static void dropevent(EvQEl *);

static Boolean OSEventCommon(INTEGER evmask, EventRecord *eventp,
                             Boolean dropit);

static void dropevent(EvQEl *qp)
{
    Dequeue((QElemPtr)qp, &LM(EventQueue));
    qp->qLink = (QElemPtr)freeelem;
    freeelem = qp;
    nevent--;
}

EvQEl *
Executor::geteventelem(void)
{
    EvQEl *retval = freeelem;

    if(nevent == NEVENT)
    {
        dropevent((EvQEl *)LM(EventQueue).qHead);
        retval = freeelem;
    }
    freeelem = (EvQEl *)freeelem->qLink;
    nevent++;
    return retval;
}

static bool
keymap_index_and_bit(LONGINT loc, int *indexp, uint8_t *bitp)
{
    bool retval;

    if(loc < 0 || loc / 8 >= sizeof_KeyMap)
        retval = false;
    else
    {
        retval = true;
        *indexp = loc / 8;
        *bitp = (1 << (loc % 8));
    }
    return retval;
}

void Executor::ROMlib_SetKey(uint8_t mkvkey, bool down)
{
    int i;
    uint8_t bit;

    if(keymap_index_and_bit(mkvkey, &i, &bit))
    {
        if(down)
            LM(KeyMap)[i] |= bit;
        else
            LM(KeyMap)[i] &= ~(bit);
    }
}

bool
Executor::ROMlib_GetKey(uint8_t mkvkey)
{
    bool retval;
    int i;
    uint8_t bit;

    if(!keymap_index_and_bit(mkvkey, &i, &bit))
        retval = false;
    else
        retval = !!(LM(KeyMap)[i] & bit);
    return retval;
}

short Executor::ROMlib_GetModifiers()
{
    short modifiers = 0;

    if(LM(MBState))
        modifiers |= btnState;
    if(ROMlib_GetKey(MKV_CLOVER))
        modifiers |= cmdKey;
    if(ROMlib_GetKey(MKV_LEFTSHIFT) || ROMlib_GetKey(MKV_RIGHTSHIFT))
        modifiers |= shiftKey;
    if(ROMlib_GetKey(MKV_LEFTOPTION) || ROMlib_GetKey(MKV_RIGHTOPTION))
        modifiers |= optionKey;
    if(ROMlib_GetKey(MKV_LEFTCNTL) || ROMlib_GetKey(MKV_RIGHTCNTL))
        modifiers |= ControlKey;

    return modifiers;
}

OSErr Executor::ROMlib_PPostEvent(INTEGER evcode, LONGINT evmsg,
                                  GUEST<EvQElPtr> *qelp, LONGINT when,
                                  Point where, INTEGER butmods)
{
    if(!((1 << evcode) & LM(SysEvtMask)))
        /*-->*/ return evtNotEnb;

    EvQEl *qp = geteventelem();
    qp->evtQWhat = evcode;
    qp->evtQMessage = evmsg;
    qp->evtQWhen = when;
    qp->evtQWhere = where;
    qp->evtQModifiers = butmods;
    Enqueue((QElemPtr)qp, &LM(EventQueue));
    if(qelp)
        *qelp = qp;
    return noErr;
}

OSErr Executor::PPostEvent(INTEGER evcode, LONGINT evmsg,
                              GUEST<EvQElPtr> *qelp) /* IMIV-85 */
{
    short modifiers = ROMlib_GetModifiers();

    if(evcode == keyDown)
    {
        if((evmsg & 0xff) == '2' && /* cmd-shift-2 */
            (modifiers & (cmdKey | shiftKey)) == (cmdKey | shiftKey))
        {
            dofloppymount();
        }
    }

    if(!((1 << evcode) & LM(SysEvtMask)))
        /*-->*/ return evtNotEnb;

    return ROMlib_PPostEvent(evcode, evmsg, qelp, TickCount(),
                             LM(MouseLocation2), modifiers);
}

void Executor::ROMlib_SetAutokey(uint32_t message)
{
    lastdown = message;
    autoticks = TickCount() + LM(KeyThresh);
}

OSErr Executor::PostEvent(INTEGER evcode, LONGINT evmsg)
{
    return PPostEvent(evcode, evmsg, (GUEST<EvQElPtr> *)0);
}

void Executor::FlushEvents(INTEGER evmask, INTEGER stopmask) /* II-69 */
{
    EvQEl *qp, *next;
    int x;

    for(qp = (EvQEl *)LM(EventQueue).qHead;
        qp && !((x = 1 << qp->evtQWhat) & stopmask); qp = next)
    {
        next = (EvQEl *)qp->qLink; /* save before dropping event */
        if(x & evmask)
            dropevent(qp);
    }
    /* NOTE:  According to IMII-69 we should be leaving stuff in d0 */
}

Boolean Executor::ROMlib_bewaremovement;

static Boolean OSEventCommon(INTEGER evmask, EventRecord *eventp,
                             Boolean dropit)
{
    EvQEl *qp;
    Boolean retval;
    static Point oldpoint = { -1, -1 };
    LONGINT ticks;

    /* We tend to call this routine from various ROMlib modal loops, so this
     * is a good place to check for timer interrupts, etc. */
    syncint_check_interrupt();

    if(send_application_open_aevt_p
       && application_accepts_open_app_aevt_p)
    {
        ProcessSerialNumber psn;
        OSErr err;

        GetCurrentProcess(&psn);

        {
            AppleEvent *aevt = (AppleEvent *)alloca(sizeof *aevt);
            AEAddressDesc *target = (AEAddressDesc *)alloca(sizeof *target);

            AEDescList *list = (AEDescList *)alloca(sizeof *list);
            int16_t count;
            GUEST<int16_t> count_s, dummy;

            err = AECreateDesc(typeProcessSerialNumber,
                               (Ptr)&psn, sizeof psn, target);

            CountAppFiles(&dummy, &count_s);
            count = count_s;

            if(count)
            {
                int i;

                err = AECreateAppleEvent(kCoreEventClass, kAEOpenDocuments,
                                         target,
                                         /* dummy */ -1, /* dummy */ -1,
                                         aevt);
                err = AECreateList(nullptr, 0, false, list);

                for(i = 1; i <= count; i++)
                {
                    FSSpec spec;
                    AppFile file;

                    GetAppFiles(i, &file);

                    FSMakeFSSpec(file.vRefNum, 0, file.fName, &spec);

                    AEPutPtr(list, i, typeFSS, (Ptr)&spec, sizeof spec);
                }

                AEPutParamDesc(aevt, keyDirectObject, list);

                AESend(aevt,
                       /* dummy */ nullptr,
                       kAENoReply,
                       /* dummy */ -1, /* dummy */ -1,
                       nullptr, nullptr);
            }
            else
            {
                err = AECreateAppleEvent(kCoreEventClass, kAEOpenApplication,
                                         target,
                                         /* dummy */ -1, /* dummy */ -1,
                                         aevt);

                AESend(aevt, /* dummy */ nullptr,
                       kAENoReply, /* dummy */ -1,
                       /* dummy */ -1, nullptr, nullptr);
            }

            send_application_open_aevt_p = false;
        }
    }

    eventp->message = 0;
    ROMlib_memnomove_p = false; /* this is an icky hack needed for Excel */
    ticks = TickCount();

    if(auto *playback = EventPlayback::getInstance())
        playback->pumpEvents();

    for(qp = (EvQEl *)LM(EventQueue).qHead; qp && !((1 << qp->evtQWhat) & evmask);
        qp = (EvQEl *)qp->qLink)
        ;
    if(qp)
    {
        *eventp = *(EventRecord *)(&qp->evtQWhat);
        if(dropit)
        {
            dropevent(qp);
        }
        retval = true;
    }
    else
    {
        eventp->when = TickCount();

        eventp->where = LM(MouseLocation2);

        eventp->modifiers = ROMlib_GetModifiers();
        if((evmask & autoKeyMask) && lastdown != -1 && ticks > autoticks)
        {
            autoticks = ticks + LM(KeyRepThresh);
            eventp->what = autoKey;
            eventp->message = lastdown;
            retval = true;
        }
        else
        {
            eventp->what = nullEvent;
            retval = false;
        }
    }
    if(eventp->where.h.get() != oldpoint.h || eventp->where.v.get() != oldpoint.v)
    {
        oldpoint = eventp->where.get();
        if(ROMlib_bewaremovement)
        {
            ROMlib_showhidecursor();
            ROMlib_bewaremovement = false;
        }
    }

    return retval;
}

Boolean Executor::GetOSEvent(INTEGER evmask, EventRecord *eventp)
{
    return OSEventCommon(evmask, eventp, true);
}

Boolean Executor::OSEventAvail(INTEGER evmask, EventRecord *eventp)
{
    return OSEventCommon(evmask, eventp, false);
}

void Executor::SetEventMask(INTEGER evmask)
{
    LM(SysEvtMask) = evmask;
}

QHdrPtr Executor::GetEvQHdr()
{
    return &LM(EventQueue);
}

static int
compare(const void *p1, const void *p2)
{
    int retval;

    retval = ROMlib_strcmp((const Byte *)p1, (const Byte *)p2);
    return retval;
}

void
Executor::display_keyboard_choices(void)
{
    INTEGER nres, i, nfound;
    unsigned char(*names)[256];

    vdriver = {};
    printf("Available keyboard maps:\n");
    SetResLoad(false);
    nres = CountResources("KCHR"_4);
    names = (decltype(names))alloca(nres * sizeof(*names));
    nfound = 0;
    for(i = 1; i <= nres; ++i)
    {
        Handle h;

        h = GetIndResource("KCHR"_4, i);
        if(h)
        {
            GetResInfo(h, 0, 0, (StringPtr)names[nfound]);
            ++nfound;
        }
    }
    qsort(names, nfound, sizeof(names[0]), compare);

    for(i = 0; i < nfound; ++i)
        printf("%.*s\n", names[i][0], (char *)&names[i][1]);

    exit(0);
}

