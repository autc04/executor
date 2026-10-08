/* Copyright 1986-1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in WindowMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <WindowMgr.h>
#include <ToolboxUtil.h>
#include <ResourceMgr.h>
#include <FontMgr.h>
#include <MemoryMgr.h>
#include <SegmentLdr.h>
#include <OSUtil.h>
#include <OSEvent.h>
#include <ControlMgr.h>
#include <MenuMgr.h>
#include <SysErr.h>
#include <DialogMgr.h>

#include <quickdraw/cquick.h>
#include <wind/wind.h>
#include <menu/menu.h>
#include <res/resource.h>
#include <error/system_error.h>

#include <prefs/prefs.h>
#include <commandline/flags.h>

#include <rsys/segment.h>
#include <file/file.h>
#include <rsys/executor.h>
#include <prefs/options.h>
#include <rsys/launch.h>
#include <res/resource.h>

#include <algorithm>

using namespace Executor;

static void
exit_executor(void)
{
    ROMlib_exit = true;
    ExitToShell();
}

RgnHandle Executor::C_GetGrayRgn()
{
    return LM(GrayRgn);
}

void Executor::ROMLib_InitGrayRgn()
{
    ThePortGuard guard((GrafPtr)LM(WMgrCPort));

        // reset clip and vis, in case this is invoked when resizing the screen
    RectRgn(PORT_VIS_REGION(wmgr_port), &PORT_RECT(wmgr_port));
    RectRgn(PORT_CLIP_REGION(wmgr_port), &PORT_RECT(wmgr_port));
    OpenRgn();
    if(!ROMlib_rect_screen)
        FrameRoundRect(&GD_BOUNDS(LM(TheGDevice)), 16, 16);
    else
        FrameRect(&GD_BOUNDS(LM(TheGDevice)));
    CloseRgn(LM(GrayRgn));
    RgnHandle mrgn = NewRgn();
    SetRectRgn(mrgn, 0, 0, GD_BOUNDS(LM(TheGDevice)).right,
                LM(MBarHeight));
    SectRgn(LM(GrayRgn), mrgn, mrgn);
    RgnHandle corners = NewRgn();
    SetRectRgn(corners, 0, 0, GD_BOUNDS(LM(TheGDevice)).right,
                GD_BOUNDS(LM(TheGDevice)).bottom);
    DiffRgn(corners, LM(GrayRgn), corners);
    PaintRgn(corners);
    CopyRgn(LM(GrayRgn), PORT_VIS_REGION(wmgr_port));
    DiffRgn(LM(GrayRgn), mrgn, LM(GrayRgn));
    PenPat(&qdGlobals().white);
    PaintRgn(mrgn);
    PenPat(&qdGlobals().black);
    MoveTo(0, LM(MBarHeight) - 1);
    Line(GD_BOUNDS(LM(TheGDevice)).right, 0);
    if(!ROMlib_rootless_drawdesk(LM(GrayRgn)))
    {
        if((USE_DESKCPAT_VAR & USE_DESKCPAT_BIT)
        && PIXMAP_PIXEL_SIZE(GD_PMAP(LM(MainDevice))) > 2)
            FillCRgn(LM(GrayRgn), LM(DeskCPat));
        else
            FillRgn(LM(GrayRgn), &LM(DeskPattern));
    }
    DisposeRgn(mrgn);
    DisposeRgn(corners);
    CopyRgn(LM(GrayRgn), PORT_CLIP_REGION(wmgr_port));
}

void Executor::C_InitWindows()
{
    PatHandle ph;
    PixPatHandle new_ph;

    LM(AuxWinHead) = default_aux_win;
    LM(SaveVisRgn) = nullptr;

    {
        ThePortGuard guard(qdGlobals().thePort);

        /* FIXME: is this a memory leak, to just call InitPort () again? */
        InitPort(LM(WMgrPort));
        InitCPort(LM(WMgrCPort));

        ph = GetPattern(deskPatID);
        new_ph = GetPixPat(deskPatID);
        if(new_ph)
            LM(DeskCPat) = new_ph;
        else
            USE_DESKCPAT_VAR &= ~USE_DESKCPAT_BIT;
        InitPalettes();
        InitMenus();
        LM(DeskPattern) = **ph;
        LM(GrayRgn) = NewRgn();
        ROMLib_InitGrayRgn();
        LM(WindowList) = nullptr;
        LM(SaveUpdate) = -1;
        LM(PaintWhite) = -1;
        LM(DeskHook) = nullptr;
        LM(GhostWindow) = nullptr;
        LM(DragPattern) = qdGlobals().gray;
    }
    ROMlib_rootless_update();

    /* since there is no `InitControls ()', we do this here */
    ctl_color_init();

    LM(WWExist) = EXIST_YES;

    {
        static bool issued_system_file_version_skew_warning_p = false;

        if(system_file_version_skew_p
           && !issued_system_file_version_skew_warning_p)
        {
            system_error("\
The system file you have installed appears to be too old. \
Executor may die without warning because of this mismatch",
                         0,
                         "Continue", "Exit", nullptr,
                         nullptr, exit_executor, nullptr);
        }
        issued_system_file_version_skew_warning_p = true;
    }

    switch(ROMlib_launch_failure)
    {
        case launch_no_failure:
            break;
        case launch_cfm_requiring:
            system_error("CFM-requiring applications are not currently supported.",
                         0, "OK", nullptr, nullptr, nullptr, nullptr, nullptr);
            break;
        case launch_ppc_only:
#if !defined(powerpc) && !defined(__ppc__)
            system_error("That application is PowerPC-only.  This version of "
                         "Executor doesn't run PowerPC applications.  "
                         "You need to find an M68k version of that application.",
                         0, "OK", nullptr, nullptr, nullptr, nullptr, nullptr);
#else
            system_error("That application is PowerPC-only.  To attempt to run "
                         "it, Executor must be started using the \"-ppc\" "
                         "command-line switch.",
                         0, "OK", nullptr, nullptr, nullptr, nullptr, nullptr);
#endif
            break;
        case launch_damaged:
            system_error("That application appears damaged (lacks CODE and cfrg).",
                         0, "OK", nullptr, nullptr, nullptr, nullptr, nullptr);
            break;

        case launch_compressed_ge7:
            system_error("That application has a compressed CODE 0.  "
                         "It is probably unusable under Executor but "
                         "might work in System 6 mode",
                         0, "OK", nullptr, nullptr, nullptr, nullptr, nullptr);
            break;
        case launch_compressed_lt7:
            system_error("That application has a compressed CODE 0.  "
                         "It will not run under this version of Executor.",
                         0, "OK", nullptr, nullptr, nullptr, nullptr, nullptr);
            break;

        default:
            warning_unexpected("%d", ROMlib_launch_failure);
            break;
    }
    ROMlib_launch_failure = launch_no_failure;

    if(!size_info.application_p)
        return;

    /* only issue warnings once */
    size_info.application_p = false;

#ifndef TWENTYFOUR_BIT_ADDRESSING
    if((!size_info.size_resource_present_p
        || (size_info.size_flags & SZis32BitCompatible) != SZis32BitCompatible)
       && !ROMlib_nowarn32)
    {
        system_error("This application doesn't claim to be \"32 bit clean\".  "
                     "It is quite possible that this program will not work "
                     "under Executor.",
                     0,
                     "Continue", "Quit", nullptr,
                     nullptr, C_ExitToShell, nullptr);
    }
#endif

    if(size_info.size_resource_present_p
       && ROMlib_applzone_size < size_info.preferred_size)
    {
        char msg_buf[1024];
        int applzone_in_k, preferred_size_in_k;

        applzone_in_k = ROMlib_applzone_size / 1024;
        preferred_size_in_k = (size_info.preferred_size + 1023) / 1024;

        sprintf(msg_buf, "This application prefers `%dk' of memory, "
                         "but only '%dk' of memory is available in the application "
                         "zone.  You should exit Executor and run it again "
                         "with \"-applzone %dk\".",
                preferred_size_in_k, applzone_in_k,
                preferred_size_in_k);

        system_error(msg_buf, 0,
                     "Continue", "Browser", "Exit",
                     nullptr, C_ExitToShell, exit_executor);
    }
}

void Executor::C_GetWMgrPort(GUEST<GrafPtr> *wp)
{
    *wp = LM(WMgrPort);
}

void Executor::C_GetCWMgrPort(GUEST<CGrafPtr> *wp)
{
    *wp = LM(WMgrCPort);
}

void Executor::C_SetDeskCPat(PixPatHandle ph)
{
    PatHandle bw_ph;

    if(ph)
    {
        LM(DeskCPat) = ph;
        USE_DESKCPAT_VAR |= USE_DESKCPAT_BIT;
    }
    else
    {
        bw_ph = GetPattern(deskPatID);
        LM(DeskPattern) = **bw_ph;
        USE_DESKCPAT_VAR &= ~USE_DESKCPAT_BIT;
    }
    PaintOne((WindowPeek)0, LM(GrayRgn));
}

static void
ROMlib_new_window_common(WindowPeek w,
                         int allocated_p, int cwindow_p,
                         const Rect *bounds, ConstStringPtr title, Boolean visible_p,
                         INTEGER proc_id, WindowPtr behind,
                         Boolean go_away_flag, LONGINT ref_con)
{
    WindowPeek t_w;
    AuxWinHandle t_aux_w;
    GrafPtr save_port;

    save_port = qdGlobals().thePort;
    if(!title)
        title = (StringPtr) ""; /* thank MS Word for pointing this out */
    if(!behind)
    {
        WINDOW_NEXT_WINDOW(w) = nullptr;
        if(LM(WindowList))
        {
            for(t_w = LM(WindowList);
                WINDOW_NEXT_WINDOW(t_w);
                t_w = WINDOW_NEXT_WINDOW(t_w))
                ;
            WINDOW_NEXT_WINDOW(t_w) = w;
        }
        else
        {
            LM(WindowList) = w;
            if(visible_p)
            {
                /* notify the palette manager that the `FrontWindow ()'
		 may have changed */
                pm_front_window_maybe_changed_hook();
            }
        }
    }
    else if(behind == (WindowPtr)-1L)
    {
        WINDOW_NEXT_WINDOW(w) = LM(WindowList);
        LM(WindowList) = (WindowPeek)w;
        if(visible_p)
        {
            /* notify the palette manager that the `FrontWindow ()' may have
	     changed */
            pm_front_window_maybe_changed_hook();
        }
    }
    else
    {
        WINDOW_NEXT_WINDOW(w) = WINDOW_NEXT_WINDOW(behind);
        WINDOW_NEXT_WINDOW(behind) = (WindowPeek)w;
    }
    WINDOW_KIND(w) = userKind;
    WINDOW_VISIBLE(w) = visible_p;
    for(t_w = LM(WindowList);
        t_w && !WINDOW_VISIBLE(t_w);
        t_w = WINDOW_NEXT_WINDOW(t_w))
        ;
    WINDOW_HILITED(w) = visible_p && (t_w == w);
    if(WINDOW_HILITED(w))
    {
        LM(CurActivate) = (WindowPtr)w;
        for(t_w = WINDOW_NEXT_WINDOW(t_w);
            t_w && !WINDOW_HILITED(t_w);
            t_w = WINDOW_NEXT_WINDOW(t_w))
            ;
    }
    else
        t_w = 0; /* t_w will be used later */
    WINDOW_GO_AWAY_FLAG(w) = go_away_flag;
    WINDOW_SPARE_FLAG(w) = 0; /* will be used zoombox (wNew) */
    WINDOW_DATA(w) = 0;
    WINDOW_STRUCT_REGION(w) = NewRgn();
    WINDOW_CONT_REGION(w) = NewRgn();
    WINDOW_UPDATE_REGION(w) = NewRgn();
    WINDOW_DEF_PROC(w) = GetResource("WDEF"_4, proc_id >> 4);
    if(!WINDOW_DEF_PROC(w))
    {
        WINDOW_DEF_PROC(w) = GetResource("WDEF"_4, 0);
        if(!WINDOW_DEF_PROC(w))
        {
            if(allocated_p)
                DisposePtr((Ptr)w);
            /* fatal_error ("no window (?)"); */
            gui_fatal("Unable to find WDEF.");
        }
    }

    t_aux_w = (AuxWinHandle)NewHandle(sizeof(AuxWinRec));
    (*t_aux_w)->awNext = LM(AuxWinHead);
    (*t_aux_w)->awOwner = (WindowPtr)w;
    (*t_aux_w)->awCTable = (CTabHandle)GetResource("wctb"_4, 0);
    (*t_aux_w)->dialogCItem = 0;
    (*t_aux_w)->awFlags = (proc_id & 0xF) << 24;
    (*t_aux_w)->awReserved = 0;
    (*t_aux_w)->awRefCon = 0;
    LM(AuxWinHead) = t_aux_w;

    {
        GUEST<Handle> t;

        PtrToHand((Ptr)title, &t, (LONGINT)title[0] + 1);
        WINDOW_TITLE(w) = guest_cast<StringHandle>(t);
    }

    if(cwindow_p)
        OpenCPort((CGrafPtr)w);
    else
        OpenPort((GrafPtr)w);
    OffsetRect(&PORT_BOUNDS(w), -bounds->left, -bounds->top);
    PORT_RECT(w) = *bounds;
    OffsetRect(&PORT_RECT(w), -bounds->left, -bounds->top);
    {
        HLockGuard guard(WINDOW_TITLE(w));
        WINDOW_TITLE_WIDTH(w) = StringWidth(*WINDOW_TITLE(w));
    }

    TextFont(applFont);
    WINDOW_CONTROL_LIST(w) = nullptr;
    WINDOW_PIC(w) = nullptr;
    WINDOW_REF_CON(w) = ref_con;
    WINDCALL((WindowPtr)w, wNew, 0);
    if(WINDOW_VISIBLE(w))
    {
        ThePortGuard guard(wmgr_port);
        WINDCALL((WindowPtr)w, wCalcRgns, 0);
        SetClip(WINDOW_STRUCT_REGION(w));
        ClipAbove(w);
        PenPat(&qdGlobals().black);
        WINDCALL((WindowPtr)w, wDraw, 0);
        CalcVis(w);
        EraseRgn(WINDOW_CONT_REGION(w));
        CopyRgn(WINDOW_CONT_REGION(w), WINDOW_UPDATE_REGION(w));
        if(WINDOW_NEXT_WINDOW(w))
            CalcVisBehind(WINDOW_NEXT_WINDOW(w), WINDOW_STRUCT_REGION(w));

        ROMlib_rootless_update();
    }
    else
        SetEmptyRgn(PORT_VIS_REGION(w));
    if(t_w)
    {
        HiliteWindow((WindowPtr)t_w, false);
        LM(CurDeactive) = (WindowPtr)t_w;
    }

    SetPort(save_port);
}

WindowPtr Executor::C_NewWindow(void* window_storage, const Rect *bounds,
                                ConstStringPtr title, Boolean visible_p,
                                INTEGER proc_id, WindowPtr behind,
                                Boolean go_away_flag, LONGINT ref_con)
{
    WindowPeek w;
    int allocated_p = 0;

    if(!window_storage)
    {
        allocated_p = 1;
        /* Hack for Dark Castle Demo.  They call NewWindow and expect us to
	 create the storage.  Immediately after calling NewWindow they set
	 the windowKind field to dialogKind.  Later they call UpdateDialog
	 on this window and we die a horrible death since we try to refer
	 to a field that isn't present.  I don't know how they get away with
	 it on the Mac, but I doubt that this particular hack will hurt us
	 elsewhere.  At some point we should find out why it works on the
	 Mac and then get rid of this evil hack.  ctm 97/06/01 */
        {
            int size;

#define DARK_CASTLE_HACK
#if defined(DARK_CASTLE_HACK)
            // FIXME: #warning DARK_CASTLE_HACK
            if(strncmp((char *)title + 1, "Modal", 5) == 0)
                size = sizeof(DialogRecord);
            else
                size = sizeof *w;
            w = (WindowPeek)_NewPtr_flags(size, false, true);
#else
            w = (WindowPeek)NewPtr(sizeof *w);
#endif
        }
    }
    else
        w = (WindowPeek)window_storage;

    ROMlib_new_window_common(w, allocated_p, 0,
                             bounds, title, visible_p, proc_id, behind,
                             go_away_flag, ref_con);
    return (WindowPtr)w;
}

WindowPtr Executor::C_NewCWindow(void* window_storage, const Rect *bounds,
                                  ConstStringPtr title, Boolean visible_p,
                                  INTEGER proc_id, WindowPtr behind,
                                  Boolean go_away_flag, LONGINT ref_con)
{
    WindowPeek w;
    int allocated_p = 0;

    if(!window_storage)
    {
        allocated_p = 1;
        w = (WindowPeek)NewPtr(sizeof *w);
    }
    else
        w = (WindowPeek)window_storage;

    ROMlib_new_window_common(w, allocated_p, 1,
                             bounds, title, visible_p, proc_id,
                             (WindowPtr)behind,
                             go_away_flag, ref_con);
    return (WindowPtr)w;
}

typedef windrestype *windrestypeptr;

typedef GUEST<windrestypeptr> *windrestypehand;

WindowPtr Executor::C_GetNewCWindow(INTEGER window_id, void* window_storage,
                                     WindowPtr behind)
{
    WindowPtr new_cwin;
    windrestypehand win_res;
    Handle win_ctab_res;
    PaletteHandle palette;

    win_res = (windrestypehand)ROMlib_getrestid("WIND"_4, window_id);
    if(!win_res)
        return nullptr;

    new_cwin = NewCWindow(window_storage, &(*win_res)->_wrect,
                          (StringPtr)((char *)&(*win_res)->_wrect + 18),
                          (*win_res)->_wvisible != 0,
                          (*win_res)->_wprocid,
                          behind, (*win_res)->_wgoaway != 0,
                          *(GUEST<LONGINT> *)((char *)&(*win_res)->_wrect + 14));

    win_ctab_res = ROMlib_getrestid("wctb"_4, window_id);
    if(win_ctab_res != nullptr)
    {
        ThePortGuard guard(qdGlobals().thePort);
        SetWinColor(new_cwin, (CTabHandle)win_ctab_res);
    }

    /* if this is a color window we must check if a palette
     corresponding to this window id exists */

    palette = GetNewPalette(window_id);
    if(palette)
        NSetPalette((WindowPtr)new_cwin, palette, (INTEGER)pmAllUpdates);

    return new_cwin;
}

WindowPtr Executor::C_GetNewWindow(INTEGER wid, void* wst, WindowPtr behind)
{
    windrestypehand wh;
    WindowPtr tp;

    wh = (windrestypehand)GetResource("WIND"_4, wid);
    if(!wh)
        return (0);
    if(!*wh)
        LoadResource((Handle)wh);
    tp = NewWindow(wst, &((*wh)->_wrect),
                   (StringPtr)((char *)&(*wh)->_wrect + 18),
                   (*wh)->_wvisible != 0, (*wh)->_wprocid, (WindowPtr)behind,
                   (*wh)->_wgoaway != 0,
                   *(GUEST<LONGINT> *)((char *)&(*wh)->_wrect + 14));
    return (tp);
}

/*
 * NOTE below:  On the Mac+ if after you close a window, the top most
 *		window is non-visible, it will shuffle things.
 */

void Executor::C_CloseWindow(WindowPtr w)
{
    WindowPeek wptmp;
    GrafPtr savgp;

    AuxWinHandle saveauxh;
    GUEST<AuxWinHandle> *auxhp;
    ControlHandle c, t;

    if(FrontWindow() == w)
    {
        wptmp = ROMlib_firstvisible((WindowPtr)WINDOW_NEXT_WINDOW(w));
        if(wptmp)
        {
            HiliteWindow((WindowPtr)wptmp, true);
            LM(CurActivate) = (WindowPtr)wptmp;
        }
    }
    if(LM(WindowList) == (WindowPeek)w)
    {
        LM(WindowList) = WINDOW_NEXT_WINDOW(w);
        wptmp = LM(WindowList);
    }
    else
    {
        for(wptmp = LM(WindowList);
            wptmp && WINDOW_NEXT_WINDOW(wptmp) != (WindowPeek)w;
            wptmp = WINDOW_NEXT_WINDOW(wptmp))
            ;
        if(wptmp)
            WINDOW_NEXT_WINDOW(wptmp) = WINDOW_NEXT_WINDOW(w);
    }

    /* notify the palette manager this window has been deleted */
    pm_window_closed(w);

    /* notify the palette manager that the `FrontWindow ()' may have
       changed */
    pm_front_window_maybe_changed_hook();

    /* NOTE: tests have shown that the behaviour implemented below is
       indeed what a Mac+ does */

    /* NOTE: we can't use THEPORT_SAVE_EXCURSION here, becuase of this
       odd behavior */
    savgp = qdGlobals().thePort == (GrafPtr)w ? wmgr_port : qdGlobals().thePort;
    SetPort(wmgr_port);
    SetClip(LM(GrayRgn));
    PaintBehind(WINDOW_NEXT_WINDOW(w), WINDOW_STRUCT_REGION(w));
    if(WINDOW_NEXT_WINDOW(w))
        CalcVisBehind(WINDOW_NEXT_WINDOW(w), WINDOW_STRUCT_REGION(w));

    DisposeRgn(WINDOW_STRUCT_REGION(w));
    DisposeRgn(WINDOW_CONT_REGION(w));
    DisposeRgn(WINDOW_UPDATE_REGION(w));
    DisposeHandle((Handle)WINDOW_TITLE(w));
    for(auxhp = (GUEST<AuxWinHandle> *)&LM(AuxWinHead);
        *auxhp && (**auxhp)->awOwner != w;
        auxhp = (GUEST<AuxWinHandle> *)&(**auxhp)->awNext)
        ;
    if(*auxhp)
    {
        saveauxh = *auxhp;
        *auxhp = (**auxhp)->awNext;
        DisposeHandle((Handle)saveauxh);
    }

#if defined(NOTAGOODIDEA)
    // FIXME: #warning "what the hell does this mean?! DANGER WILL ROBINSON!"
    Cx (*(Ptr *)Cx)(((WindowPeek)w)->windowDefProc) = 0;
    DisposeHandle(((WindowPeek)w)->windowDefProc);
#endif /* NOTAGOODIDEA */

/*
 * TODO: Fix this.  Tests on the mac show that KillControls is called,
 * but just replacing the for loop causes many apps to die.  It could
 * be because some window information that DisposeControl wants is
 * destroyed already, or it could be DisposeControl or KillControl
 * makes some false assumptions.  More tests need to be written.
 */
#if 1
    for(c = WINDOW_CONTROL_LIST(w); c;)
    {
        t = c;
        c = (*c)->nextControl;
#if 0
	DisposeHandle((*t)->contrlDefProc);
#endif /* 0 */
        DisposeHandle((Handle)t);
    }
#else /* 0 */
    KillControls(w);
#endif /* 0 */

    if(WINDOW_PIC(w))
        KillPicture(WINDOW_PIC(w));
    ClosePort((GrafPtr)w);
    SetPort(savgp);
    if(LM(CurActivate) == w)
        LM(CurActivate) = 0;
    if(LM(CurDeactive) == w)
        LM(CurDeactive) = 0;
    WINDCALL((WindowPtr)w, wDispose, 0);

    ROMlib_rootless_update();
}

void Executor::C_DisposeWindow(WindowPtr w)
{
    CloseWindow(w);
    DisposePtr((Ptr)w);
}
