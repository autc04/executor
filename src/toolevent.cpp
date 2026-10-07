/* Copyright 1987 - 1996 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in ToolboxEvent.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <EventMgr.h>
#include <WindowMgr.h>
#include <OSUtil.h>
#include <ToolboxEvent.h>
#include <ToolboxUtil.h>
#include <FileMgr.h>
#include <MemoryMgr.h>
#include <DeskMgr.h>
#include <SysErr.h>
#include <BinaryDecimal.h>
#include <SegmentLdr.h>
#include <TimeMgr.h>

#include <quickdraw/cquick.h>
#include <hfs/hfs.h>
#include <res/resource.h>
#include <hfs/futzwithdosdisks.h>
#include <prefs/prefs.h>
#include <prefs/options.h>
#include <rsys/prefpanel.h>
#include <sound/sounddriver.h>
#include <rsys/segment.h>
#include <rsys/version.h>
#include <time/syncint.h>
#include <time/vbl.h>
#include <rsys/osutil.h>
#include <osevent/osevent.h>
#include <rsys/keyboard.h>
#include <vdriver/refresh.h>
#include <vdriver/vdriver.h>
#include <vdriver/responsiveness.h>
#include <rsys/aboutbox.h>
#include <rsys/redrawscreen.h>
#include <rsys/toolevent.h>
#include <print/nextprint.h>
#include <time/time.h>
#include <menu/menu.h>
#include <base/traps.impl.h>
#include <rsys/screen-dump.h>

#include <algorithm>

using namespace Executor;

/* #define	EVENTTRACE */

#if defined(EVENTTRACE)
LONGINT Executor::eventstate = 0;
#define TRACE(n) (eventstate = (n))
#else /* !defined(EVENTTRACE) */
#define TRACE(n)
#endif /* !defined(EVENTTRACE) */

static Boolean ROMlib_alarmonmbar = false;

#define ALARMSICN -16385

Boolean Executor::ROMlib_beepedonce = false;

static void ROMlib_togglealarm()
{
    Handle alarmh;
    static const unsigned char hard_coded_alarm[] = {
        0xFB, 0xBE, /* XXXXX XXX XXXXX  */
        0x84, 0x42, /* X    X   X    X  */
        0x89, 0x22, /* X   X  X  X   X  */
        0x91, 0x12, /* X  X   X   X  X  */
        0xA1, 0x0A, /* X X    X    X X  */
        0x41, 0x04, /*  X     X     X   */
        0x81, 0x02, /* X      X      X  */
        0x81, 0x02, /* X      X      X  */
        0x80, 0x82, /* X       X     X  */
        0x40, 0x44, /*  X       X   X   */
        0xA0, 0x0A, /* X X         X X  */
        0x90, 0x12, /* X  X       X  X  */
        0x88, 0x22, /* X   X     X   X  */
        0x84, 0x42, /* X    X   X    X  */
        0xFB, 0xBE, /* XXXXX XXX XXXXX  */
        0x00, 0x00, /*                  */
    };

    /* rectangle of alarm on the screen */
    static Rect screen_alarm_rect = { 2, 16, 18, 32 };

    BitMap src_alarm_bitmap = {
        /* the baseAddr field of the src_alarm bitmap will either be
	   the hard coded alarm, or the resource 'sicn' */
        nullptr,
        2,
        { 0, 0, 16, 16 }
    };

    static INTEGER alarm_bits[16];
    BitMap save_alarm_bitmap = {
        (Ptr)&alarm_bits[0],
        2,
        { 0, 0, 16, 16 }
    };

    if(ROMlib_alarmonmbar)
    {
        CopyBits(&save_alarm_bitmap, (BitMap *)*GD_PMAP(LM(TheGDevice)),
                 &save_alarm_bitmap.bounds, &screen_alarm_rect,
                 srcCopy, nullptr);
        ROMlib_alarmonmbar = false;
    }
    else
    {
        if(ROMlib_shouldalarm())
        {
            if(!ROMlib_beepedonce)
            {
                SysBeep(5);
                ROMlib_beepedonce = true;
            }
            /* save the bits underneath */
            /* draw sicon -16385 up there */
            if((alarmh = ROMlib_getrestid("SICN"_4, ALARMSICN)))
                src_alarm_bitmap.baseAddr = *alarmh;
            else
                /* once again, we need to move the (Ptr) cast
		 inside the () because it confuses gcc */
                src_alarm_bitmap.baseAddr = (Ptr)hard_coded_alarm;

            /* save the screen to save_alarm_bitmap */
            CopyBits((BitMap *)*GD_PMAP(LM(TheGDevice)), &save_alarm_bitmap,
                     &screen_alarm_rect, &save_alarm_bitmap.bounds,
                     srcCopy, nullptr);
            /* and copy the new alarm to the screen */
            CopyBits(&src_alarm_bitmap, (BitMap *)*GD_PMAP(LM(TheGDevice)),
                     &src_alarm_bitmap.bounds, &screen_alarm_rect,
                     srcCopy, nullptr);
            ROMlib_alarmonmbar = true;
        }
    }
}

void Executor::ROMlib_alarmoffmbar()
{
    if(ROMlib_alarmonmbar)
        ROMlib_togglealarm();
}

uint32_t Executor::C_KeyTranslate(Ptr mapp, unsigned short code, GUEST<uint32_t> *state)
{
    enum
    {
        MODIFIER_SHIFT = 8,
        VIRT_MASK = 0x7F,
        MODIFIER_MASK = (ControlKey << 1) - 1
    };

    kchr_ptr_t p = (kchr_ptr_t)mapp;
    unsigned char virt = code & VIRT_MASK;

    int table_num_index = (code >> MODIFIER_SHIFT) & MODIFIER_MASK;
    int table_num = (KCHR_MODIFIER_TABLE(p))[table_num_index];
    uint32_t ascii = (unsigned char)(KCHR_TABLE(p)[table_num][virt]);

    if(code & 0x80)
        state = nullptr;

    if(!state || *state == 0)
    {
        if(!ascii)
        {
            int n_dead = KCHR_N_DEAD_KEY_RECS(p);
            dead_key_rec_t *deadp = KCHR_DEAD_KEY_RECS(p);
            while(--n_dead >= 0
                  && (DEAD_KEY_TABLE_NUMBER(deadp) != table_num
                      || DEAD_KEY_VIRT_KEY(deadp) != virt))
                deadp = (dead_key_rec_t *)(&DEAD_KEY_NO_MATCH(deadp) + 1);
            if(n_dead >= 0)
            {
                if(!state) // key up
                    ascii = DEAD_KEY_NO_MATCH(deadp);
                else
                    *state = (char *)deadp - (char *)&KCHR_N_TABLES(p);
            }
        }
    }
    else
    {
        int n_recs, i;

        dead_key_rec_t *deadp = (dead_key_rec_t *)((char *)&KCHR_N_TABLES(p) + *state);
        *state = 0;
        completer_t *completep = &DEAD_KEY_COMPLETER(deadp);
        n_recs = COMPLETER_N_RECS(completep);
        for(i = 0;
            (i < n_recs
             && ((COMPLETER_COMPLETER_RECS(completep))[i].to_look_for
                 != ascii));
            ++i)
            ;
        if(i < n_recs)
            ascii = (unsigned char)
                (COMPLETER_COMPLETER_RECS(completep)[i]).replacement;
        else
            ascii |= DEAD_KEY_NO_MATCH(deadp) << 16;
    }
    return ascii;
}


void Executor::dofloppymount(void)
{
    futzwithdosdisks();
}

static void doquitreallyquits(void)
{
    ROMlib_exit = !ROMlib_exit;
}

extern void ROMlib_updateworkspace(void);

static bool isSuspended = false;
static bool shouldBeSuspended = false;

static Boolean doevent(INTEGER em, EventRecord *evt,
                       Boolean remflag) /* no DA support */
{
    Boolean retval;
    GUEST<ULONGINT> now_s;
    ULONGINT now;
    static int beenhere = 0;
    ALLOCABEGIN

    /* We tend to call this routine from various ROMlib modal loops, so this
     * is a good place to check for timer interrupts. */
    syncint_check_interrupt();

    if(vdriver->updateMode())
        Executor::gd_vdriver_mode_changed();

    hle_reset();

    evt->message = 0;
    TRACE(2);
    if(LM(SPVolCtl) & 0x80)
    {
        TRACE(3);
        GetDateTime(&now_s);
        now = now_s;
        TRACE(4);
        if(now >= (ULONGINT)LM(SPAlarm))
        {
            TRACE(5);
            if(now & 1)
            {
                TRACE(6);
                if(ROMlib_alarmonmbar)
                    ROMlib_togglealarm();
            }
            else
            {
                TRACE(7);
                if(!ROMlib_alarmonmbar)
                    ROMlib_togglealarm();
            }
            TRACE(8);
        }
        else if(ROMlib_alarmonmbar)
        {
            TRACE(9);
            ROMlib_togglealarm();
        }
    }
    else if(ROMlib_alarmonmbar)
    {
        TRACE(10);
        ROMlib_togglealarm();
    }

    if(em & activMask)
    {
        TRACE(11);
        if(LM(CurDeactive))
        {
            TRACE(12);
            GetOSEvent(0, evt);
            TRACE(13);
            evt->what = activateEvt;
            evt->message = guest_cast<LONGINT>(LM(CurDeactive));
            if(remflag)
                LM(CurDeactive) = nullptr;
            retval = true;
            /*-->*/ goto done;
        }
        if(LM(CurActivate))
        {
            TRACE(14);
            GetOSEvent(0, evt);
            TRACE(15);
            evt->what = activateEvt;
            evt->message = guest_cast<LONGINT>(LM(CurActivate));
            evt->modifiers |= activeFlag;
            if(remflag)
                LM(CurActivate) = nullptr;
            retval = true;
            /*-->*/ goto done;
        }
    }

    /*
 * NOTE: Currently (Sept. 15, 1993), we get different charCodes for
 *	 command-shift-1 on the NeXT and under DOS.  I suspect that
 *	 the DOS behaviour (the lower case stuff) is correct, my
 *	 clue is 1.15 vs 1.16 of osevent.c, but I don't want to mess
 *	 around with things just before releasing Executor/DOS 1.0.
 */

    if(remflag)
    {
        TRACE(16);
        retval = GetOSEvent(em, evt);
        TRACE(17);

        short fkeyModifiers = shiftKey | cmdKey;
#ifdef __APPLE__
        fkeyModifiers = optionKey | cmdKey;
#endif
        if(retval && evt->what == keyDown && LM(ScrDmpEnb) && (evt->modifiers & fkeyModifiers) == fkeyModifiers)
        {
            TRACE(18);
            switch((evt->message & keyCodeMask) >> 8)
            {
                case 0x12: /* command shift 1: About Box / Help */
                    retval = false;
                    do_about_box();
                    break;
                case 0x13: /* command shift 2: Floppy Stuff */
                    retval = false;
                    /* dofloppymount(); already done at a lower level */
                    break;
                case 0x14: /* command shift 3: Screen Dump to File */
                    retval = false;
                    do_dump_screen();
                    break;
                case 0x17: /* command shift 5: Preferences */
                    retval = false;
                    dopreferences();
                    break;
                case 0x16: /* command shift 6: Don't restart Executor */
                    retval = false;
                    doquitreallyquits();
                    break;
                // case 0x1c: // command shift 8
            }
            if(!retval)
                evt->what = nullEvent;
            /*-->*/ goto done;
        }
    }
    else
    {
        TRACE(24);
        retval = OSEventAvail(em, evt);
        TRACE(25);
    }
    /*
 * NOTE: I think the following block of code should probably be in SystemEvent,
 *	 rather than here.  It will probably make a difference when an event
 *	 call is made without diskMask set.  (I think SystemTask does the
 *	 mount anyway and it potentially gets lost if no one is looking for
 *	 it).
 */
    if(!retval && remflag && (em & diskMask))
    {
        TRACE(26);
        if(!beenhere)
        {
            TRACE(26);
            beenhere = 1;
            ROMlib_openharddisk("/tmp/testvol\0\0", &evt->message);
            if(evt->message)
            {
                TRACE(27);
                evt->what = diskEvt;
                retval = true;
            }
        }
    }
    if(!retval && (em & updateMask))
    {
        TRACE(28);
        GetOSEvent(0, evt);
        TRACE(29);
        retval = CheckUpdate(evt);
    }

    if(!retval)
        vdriver->noteUpdatesDone();

    /* check for high level events */
    if(!retval && (em & highLevelEventMask))
    {
        retval = hle_get_event(evt, remflag);
    }

    if(!retval && ROMlib_delay)
    {
        TRACE(30);
        Delay((LONGINT)ROMlib_delay, nullptr);
    }
done:
    ALLOCAEND
    TRACE(31);
    if(SystemEvent(evt))
    {
        TRACE(32);
        evt->what = nullEvent;
        retval = false;
    }
    TRACE(33);

    if(!retval)
    {
        if(isSuspended != shouldBeSuspended)
        {
            WindowPtr w = C_FrontWindow();
            if(w)
            {
                if(shouldBeSuspended)
                    LM(CurDeactive) = w;
                else
                    LM(CurActivate) = w;
                C_HiliteWindow(w, !shouldBeSuspended);
                isSuspended = shouldBeSuspended;
            }
        }
    }

    return retval;
}

Boolean Executor::C_GetNextEvent(INTEGER em, EventRecord *evt)
{
    Boolean retval;

    TRACE(1);
    retval = doevent(em, evt, true);
    TRACE(0);
    return retval;
}

static bool wneTimeout = false;;

static void C_ROMlib_WNETimeout()
{
    wneTimeout = true;
}
PASCAL_FUNCTION_PTR(ROMlib_WNETimeout);

Boolean Executor::C_WaitNextEvent(INTEGER mask, EventRecord *evp,
                                  LONGINT sleep, RgnHandle mousergn)
{
    Boolean retval;
    Point p;
    TMTask tm;

    /* Unlike GetNextEvent, WaitNextEvent yields to the Window Manager on a real
     * Mac, so it counts as proof that the application is alive. */
    Responsiveness::instance().noteResponse();

    if(sleep > 0)
    {
        wneTimeout = false;
        tm.tmAddr = (ProcPtr)&ROMlib_WNETimeout;
        InsTime((QElemPtr)&tm);
        PrimeTime((QElemPtr)&tm, sleep * 1000 / 60);
    }
    else
        wneTimeout = true;

    do
    {
        retval = GetNextEvent(mask, evp);
        if(!retval)
        {
            static INTEGER saved_h, saved_v;

            /* TODO: see what PtInRgn does with 0 as a RgnHandle */
            p.h = evp->where.h;
            p.v = evp->where.v;
            if(mousergn && !EmptyRgn(mousergn) && !PtInRgn(p, mousergn)
               && (p.h != saved_h || p.v != saved_v))
            {
                evp->what = osEvt;
                evp->message = mouseMovedMessage << 24;
                retval = true;
            }
            else if(!wneTimeout)
            {
                syncint_wait_interrupt();
            }
            saved_h = p.h;
            saved_v = p.v;
        }
    } while(!retval && !wneTimeout);

    if(sleep > 0)
        RmvTime((QElemPtr)&tm);

    return retval;
}

Boolean Executor::C_EventAvail(INTEGER em, EventRecord *evt)
{
    return (doevent(em, evt, false));
}

void Executor::C_GetMouse(GUEST<Point> *p)
{
    EventRecord evt;

    GetOSEvent(0, &evt);
    *p = evt.where;
    GlobalToLocal(p);
}

// Note: Button not coded per IM Macintosh Toolbox Essentials 2-108
//   ... because it's completely wrong. Trust the original IM instead.
Boolean Executor::C_Button()
{
    EventRecord evt;
    Boolean retval;

    GetOSEvent(0, &evt);
    retval = (evt.modifiers & btnState) ? false : true;
    return retval;
}

// Note: StillDown not coded per IM Macintosh Toolbox Essentials 2-109
//   ... because it's wrong. Trust the original IM instead.
Boolean Executor::C_StillDown() /* IMI-259 */
{
    EventRecord evt;

    return Button() ? !OSEventAvail((mDownMask | mUpMask), &evt) : false;
}

// Note: Button not coded per IM Macintosh Toolbox Essentials 2-110
//   ... because it's completely wrong. Trust the original IM instead.
Boolean Executor::C_WaitMouseUp()
{
    EventRecord evt;
    int retval;

    retval = StillDown();
    if(!retval)
        GetOSEvent(mUpMask, &evt); /* just remove one ? */
    return (retval);
}

void Executor::C_GetKeys(unsigned char *keys)
{
    BlockMoveData((Ptr)LM(KeyMap), (Ptr)keys, (Size)sizeof_KeyMap);
}

/*
 * The weirdness below is because Word 5.1a gets very unhappy if
 * TickCount makes large adjustments to LM(Ticks).  Even when we
 * just increase by one there are problems... The "no clock" option
 * might be retiring soon.
 */

ULONGINT Executor::C_TickCount()
{
    unsigned long ticks;
    unsigned long new_time;

    ticks = msecs_elapsed() * 3.0 / 50; /* == 60 / 1000 */

    /* Update LM(Ticks) and LM(Time).  We only update LM(Ticks) if the clock is on;
   * this seems to be necessary to make Word happy.
   */

    if(ROMlib_clock)
        LM(Ticks) = ticks;

    new_time = UNIXTIMETOMACTIME(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            
    LM(Time) = new_time;
    return ticks;
}

LONGINT Executor::GetDblTime()
{
    return (LM(DoubleTime));
}

LONGINT Executor::GetCaretTime()
{
    return (LM(CaretTime));
}

static bool debug_supress_suspend_resume = false;
    /* Leave this off and let the person doing the
        debugging turn it on if he/she wants.  If this
        is set to true, it breaks code.  Not a
        very nice thing to do. */

void
Executor::sendsuspendevent(void)
{
    shouldBeSuspended = true;
    if(
        (size_info.size_flags & SZacceptSuspendResumeEvents)
        && !debug_supress_suspend_resume)
    {
        PostEvent(osEvt, SUSPENDRESUMEBITS | SUSPEND | CONVERTCLIPBOARD);
    }
}

void
Executor::sendresumeevent(bool cvtclip)
{
    LONGINT msg;

    shouldBeSuspended = false;
    if(
        (size_info.size_flags & SZacceptSuspendResumeEvents)
        && !debug_supress_suspend_resume
        )
    {
        msg = SUSPENDRESUMEBITS | RESUME;
        if(cvtclip)
            msg |= CONVERTCLIPBOARD;
        PostEvent(osEvt, msg);
    }
}

void
Executor::ROMlib_send_quit(void)
{
    constexpr long q = (MKV_q << 8) | 'q';
    bool saveCmd = ROMlib_GetKey(MKV_CLOVER);
    ROMlib_SetKey(MKV_CLOVER, true);
    PostEvent(keyDown, q);
    PostEvent(keyUp, q);
    ROMlib_SetKey(MKV_CLOVER, saveCmd);
}
