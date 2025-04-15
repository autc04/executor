/* Copyright 1986-1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <rsys/desk.h>

#include <base/common.h>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <EventMgr.h>
#include <WindowMgr.h>
#include <DeviceMgr.h>
#include <DeskMgr.h>
#include <MenuMgr.h>
#include <QuickDraw.h>
#include <OSEvent.h>
#include <ToolboxEvent.h>
#include <OSUtil.h>

#include <quickdraw/cquick.h>
#include <wind/wind.h>
#include <rsys/hook.h>
#include <rsys/aboutbox.h>
#include <rsys/prefpanel.h>
#include <base/cpu.h>
#include <util/macstrings.h>

#include <algorithm>

using namespace Executor;

AppleMenuEntry::AppleMenuEntry(const char32_t* n, void (*f)())
    : function(f)
{
    assignPString(name, toMacRoman(n), 31);
}

const std::vector<AppleMenuEntry>& Executor::appleMenuEntries()
{
    static std::vector<AppleMenuEntry> entries {
        { U"About Executor 2000...", &do_about_box },
        { U"Emulation Settings...", &dopreferences }
    };

    return entries;
}


INTEGER Executor::C_OpenDeskAcc(ConstStringPtr acc) /* IMI-440 */
{
    // THINK Reference says that OpenDeskAcc's return value
    // is undefined on error. This returns zero - otherwise
    // we get a compiler warning that might hide real bugs.
    INTEGER retval = 0;
    GUEST<INTEGER> retval_s;
    DCtlHandle dctlh;
    WindowPtr wp;

    const auto& entries = appleMenuEntries();

    if(auto it = std::find_if(entries.begin(), entries.end(),
            [acc](const AppleMenuEntry& e) { return EqualString(e.name, acc, true, true); });
        it != entries.end())
    {
        it->function();
    }
    else if(OpenDriver(acc, &retval_s) == noErr)
    {
        retval = retval_s;
        dctlh = GetDCtlEntry(retval);
        if(dctlh)
        {
            wp = (*dctlh)->dCtlWindow;
            if(wp)
            {
                ShowWindow(wp);
                SelectWindow(wp);
            }
        }
    }

    LM(SEvtEnb) = true;
    return retval;
}

void Executor::C_CloseDeskAcc(INTEGER rn)
{
    CloseDriver(rn);
}

void Executor::C_SystemClick(EventRecord *evp, WindowPtr wp)
{
    Point p;
    LONGINT pointaslong, val;
    Rect bounds;
    GUEST<LONGINT> templ;

    if(wp)
    {
        p.h = evp->where.h;
        p.v = evp->where.v;
        if(PtInRgn(p, WINDOW_STRUCT_REGION(wp)))
        {
            pointaslong = ((LONGINT)p.v << 16) | (unsigned short)p.h;
            val = WINDCALL((WindowPtr)wp, wHit, pointaslong);
            switch(val)
            {
                case wInContent:
                    if(WINDOW_HILITED(wp))
                    {
                        templ = guest_cast<LONGINT>(evp);
                        Control(WINDOW_KIND(wp), accEvent, (Ptr)&templ);
                    }
                    else
                        SelectWindow(wp);
                    break;
                case wInDrag:
                    bounds.top = LM(MBarHeight) + 4;
                    bounds.left = GD_BOUNDS(LM(TheGDevice)).left + 4;
                    bounds.bottom = GD_BOUNDS(LM(TheGDevice)).bottom - 4;
                    bounds.right = GD_BOUNDS(LM(TheGDevice)).right - 4;
                    DragWindow(wp, p, &bounds);
                    break;
                case wInGoAway:
                    if(TrackGoAway(wp, p))
                        CloseDeskAcc(WINDOW_KIND(wp));
                    break;
            }
        }
        else
        {
            if(LM(DeskHook))
            {
                ROMlib_hook(desk_deskhooknumber);
                EM_D0 = -1;
                EM_A0 = US_TO_SYN68K(evp);
                execute68K((syn68k_addr_t)(guest_cast<LONGINT>(LM(DeskHook))));
            }
        }
    }
}

Boolean Executor::C_SystemEdit(INTEGER editcmd)
{
    WindowPeek wp;
    Boolean retval;

    wp = (WindowPeek)FrontWindow();
    if(!wp)
        retval = false;
    else if((retval = WINDOW_KIND(wp) < 0))
        Control(WINDOW_KIND(wp), editcmd + accUndo, (Ptr)0);
    return retval;
}

#define rntodctlh(rn) (LM(UTableBase)[-((rn) + 1)])
#define itorn(i) ((-i) - 1)

void Executor::C_SystemTask()
{
    DCtlHandle dctlh;
    INTEGER i;

    for(i = 0; i < LM(UnitNtryCnt); ++i)
    {
        dctlh = LM(UTableBase)[i];
        if(((*dctlh)->dCtlFlags & NEEDTIMEBIT) && TickCount() >= (*dctlh)->dCtlCurTicks)
        {
            Control(itorn(i), accRun, (Ptr)0);
            (*dctlh)->dCtlCurTicks = (*dctlh)->dCtlCurTicks + (*dctlh)->dCtlDelay;
        }
    }
}

Boolean Executor::C_SystemEvent(EventRecord *evp)
{
    Boolean retval;
    WindowPeek wp;
    INTEGER rn;
    DCtlHandle dctlh;
    GUEST<LONGINT> templ;

    if(LM(SEvtEnb))
    {
        wp = 0;
        switch(evp->what)
        {
            default:
            case nullEvent:
            case mouseDown:
            case networkEvt:
            case driverEvt:
            case app1Evt:
            case app2Evt:
            case app3Evt:
            case app4Evt:
                break;
            case mouseUp:
            case keyDown:
            case keyUp:
            case autoKey:
                wp = (WindowPeek)FrontWindow();
                break;
            case updateEvt:
            case activateEvt:
                wp = guest_cast<WindowPeek>(evp->message);
                break;
            case diskEvt:
                /* NOTE:  I think the code around toolevent.c:277 should
		      really be here.  I'm not going to get all excited
		      about it right now though. */
                break;
        }
        if(wp)
        {
            rn = WINDOW_KIND(wp);
            if((retval = rn < 0))
            {
                dctlh = rntodctlh(rn);
                if((*dctlh)->dCtlEMask & (1 << evp->what))
                {
                    templ = guest_cast<LONGINT>(evp);
                    Control(rn, accEvent, (Ptr)&templ);
                }
            }
        }
        else
            retval = false;
    }
    else
        retval = false;
    return retval;
}

void Executor::C_SystemMenu(LONGINT menu)
{
    INTEGER i;
    DCtlHandle dctlh;
    GUEST<LONGINT> menu_s;

    for(i = 0; i < LM(UnitNtryCnt); ++i)
    {
        dctlh = LM(UTableBase)[i];
        if((*dctlh)->dCtlMenu == LM(MBarEnable))
        {
            menu_s = menu;
            Control(itorn(i), accMenu, (Ptr)&menu_s);
            /*-->*/ break;
        }
    }
}
