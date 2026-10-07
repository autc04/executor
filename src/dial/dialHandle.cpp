/* Copyright 1986-1996 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in DialogMgr.h (DO NOT DELETE THIS LINE) */

/* HLock checked by ctm on Mon May 13 17:54:22 MDT 1991 */

#include <base/common.h>
#include <dial/dial.h>
#include <EventMgr.h>
#include <DialogMgr.h>
#include <ControlMgr.h>
#include <MemoryMgr.h>
#include <OSUtil.h>
#include <ToolboxUtil.h>
#include <ToolboxEvent.h>
#include <Iconutil.h>
#include <FontMgr.h>

#include <quickdraw/cquick.h>
#include <ctl/ctl.h>
#include <mman/mman.h>
#include <dial/itm.h>
#include <prefs/prefs.h>
#include <rsys/hook.h>
#include <osevent/osevent.h>
#include <base/functions.impl.h>
#include <base/traps.impl.h>
#include <vdriver/responsiveness.h>

using namespace Executor;

namespace Executor
{
    PASCAL_FUNCTION_PTR(ROMlib_myfilt);
}
Boolean Executor::C_ROMlib_myfilt(DialogPtr dlg, EventRecord *evt,
                                  GUEST<INTEGER> *ith) /* IMI-415 */
{
    DialogPeek dp = (DialogPeek)dlg;
    itmp ip;
    ControlHandle c;
    SignedByte flags;

    if(evt->what == keyDown && ((evt->message & 0xFF) == '\r' || (evt->message & 0xFF) == NUMPAD_ENTER))
    {
        ip = ROMlib_dpnotoip(dp, *ith = dp->aDefItem, &flags);
        if(ip && (ip->itmtype & ctrlItem))
        {
            c = (ControlHandle)ip->itmhand;
            if((*c)->contrlVis && (*c)->contrlHilite != INACTIVE)
            {
                HiliteControl(c, inButton);
                Delay(5, nullptr);
                HiliteControl(c, 0);
                HSetState(((DialogPeek)dp)->items, flags);
                /*-->*/ return -1;
            }
        }
        HSetState(((DialogPeek)dp)->items, flags);
    }
    return false;
}

#define DIALOGEVTS \
    (mDownMask | mUpMask | keyDownMask | autoKeyMask | updateMask | activMask)

using modalprocp = UPP<Boolean(DialogPtr dial, EventRecord *evtp,
                               GUEST<INTEGER> *iht)>;

#define CALLMODALPROC(dp, evtp, ip, fp2) \
    ROMlib_CALLMODALPROC((dp), (evtp), (ip), (modalprocp)(fp2))

static inline Boolean
ROMlib_CALLMODALPROC(DialogPtr dp,
                     EventRecord *evtp, GUEST<INTEGER> *ip, modalprocp fp)
{
    ROMlib_hook(dial_modalnumber);
    return fp(dp, evtp, ip);
}

using useritemp = UPP<void(WindowPtr wp, INTEGER item)>;

#define CALLUSERITEM(dp, inum, temph) \
    ROMlib_CALLUSERITEM(dp, inum, (useritemp)(temph))

static inline void
ROMlib_CALLUSERITEM(DialogPtr dp,
                    INTEGER inum, useritemp temph)
{
    ROMlib_hook(dial_usernumber);
    temph(dp, inum);
}

inline int _FindWindow(Point pt, WindowPtr *wp)
{
    GUEST<WindowPtr> __wp;
    int retval;

    retval = FindWindow(pt, &__wp);
    *(wp) = __wp;

    return retval;
}

#define ALLOW_MOVABLE_MODAL /* we've made so many other changes, we \
                   may as well go whole hog */

#if !defined(ALLOW_MOVABLE_MODAL)

/* NOTE: the changes between #if #else and #else #endif should be very
   small, but THEGDEVICE_SAVE_EXCURSION prevents us from using #if, so
   we have a lot of replicated code.  This is scary and should be fixed. */

void Executor::C_ModalDialog(ModalFilterUPP fp, GUEST<INTEGER> *item) /* IMI-415 */
{
    /*
   * The code used to save qdGlobals().thePort and restore it at the end of the
   * function, but CALLMODALPROC expects qdGlobals().thePort to be unchanged which
   * caused a bug in Macwrite II when size/fontsize... and clicking on
   * a size on the left.
   */
    TheGDeviceGuard guard(LM(MainDevice));

    /* ModalDialog runs its own GetNextEvent loop, but on a real Mac it pumped
     * SystemTask internally, so it counts as the application being alive. */
    Responsiveness::instance().noteResponse();

    EventRecord evt;
    DialogPeek dp;
    GUEST<DialogPtr> ndp;
    TEHandle idle;
    ModalFilterUPP fp2;
    Point whereunswapped;
    bool done;

    dp = (DialogPeek)FrontWindow();
    if(dp->window.windowKind != dialogKind && dp->window.windowKind >= 0)
        *item = -1;
    else
    {
        idle = (dp->editField == -1) ? 0 : dp->textH;
        if(fp)
            fp2 = fp;
        else
            fp2 = &ROMlib_myfilt;
        for(done = false; !done;)
        {
            WindowPtr temp_wp;
            bool mousedown_p;
            int mousedown_where;

            if(idle)
                TEIdle(idle);
            GetNextEvent(DIALOGEVTS, &evt);
            whereunswapped.h = evt.where.h;
            whereunswapped.v = evt.where.v;

            mousedown_p = (evt.what == mouseDown);

            /* dummy initializations to keep gcc happy */
            temp_wp = nullptr;
            mousedown_where = inContent;

            if(mousedown_p)
                mousedown_where = _FindWindow(whereunswapped, &temp_wp);

            if(mousedown_p
               && mousedown_where != inMenuBar
               && !(mousedown_where == inContent
                    && temp_wp == (WindowPtr)dp))
                Beep(1);
            else if(CALLMODALPROC((DialogPtr)dp, &evt, item, fp2))
            {
                /* #### not sure what this means */
                /* above callpascal might need to be `& 0xF0' */
                done = true;
                break;
            }
            else
            {
                if(IsDialogEvent(&evt)
                   && DialogSelect(&evt, &ndp, item))
                    done = true;
            }
        }
    }
}

#else /* defined (ALLOW_MOVABLE_MODAL) */

void Executor::C_ModalDialog(ModalFilterUPP fp, GUEST<INTEGER> *item) /* IMI-415 */
{
    /*
   * The code used to save qdGlobals().thePort and restore it at the end of the
   * function, but CALLMODALPROC expects qdGlobals().thePort to be unchanged which
   * caused a bug in Macwrite II when size/fontsize... and clicking on
   * a size on the left.
   */

    TheGDeviceGuard guard(LM(MainDevice));

    /* ModalDialog runs its own GetNextEvent loop, but on a real Mac it pumped
     * SystemTask internally, so it counts as the application being alive. */
    Responsiveness::instance().noteResponse();

    EventRecord evt;
    DialogPeek dp;
    GUEST<DialogPtr> ndp;
    TEHandle idle;
    ModalFilterUPP fp2;
    Point whereunswapped;
    bool done;

    dp = (DialogPeek)FrontWindow();
    if(dp->window.windowKind != dialogKind && dp->window.windowKind >= 0)
        *item = -1;
    else
    {
        idle = (dp->editField == -1) ? TEHandle(nullptr) : dp->textH.get();
        if(fp)
            fp2 = fp;
        else
            fp2 = &ROMlib_myfilt;
        for(done = false; !done;)
        {
            WindowPtr temp_wp;
            bool mousedown_p;
            int mousedown_where;

            if(idle)
                TEIdle(idle);
            GetNextEvent(DIALOGEVTS, &evt);
            whereunswapped.h = evt.where.h;
            whereunswapped.v = evt.where.v;

            mousedown_p = (evt.what == mouseDown);

            /* dummy initializations to keep gcc happy */
            temp_wp = nullptr;
            mousedown_where = inContent;

            if(mousedown_p)
                mousedown_where = _FindWindow(whereunswapped, &temp_wp);

            if(CALLMODALPROC((DialogPtr)dp, &evt, item, fp2))
            {
                /* #### not sure what this means */
                /* above callpascal might need to be `& 0xF0' */

                done = true;
                break;
            }
            else if(mousedown_p
                    && mousedown_where != inMenuBar
                    && !(mousedown_where == inContent
                         && temp_wp == (WindowPtr)dp))
                Beep(1);
            else
            {
                if(IsDialogEvent(&evt)
                   && DialogSelect(&evt, &ndp, item))
                    done = true;
            }
        }
    }
}
#endif

Boolean Executor::C_IsDialogEvent(EventRecord *evt) /* IMI-416 */
{
    GUEST<WindowPtr> wp;
    DialogPeek dp;
    Point p;

    if(evt->what == activateEvt || evt->what == updateEvt)
        /*-->*/ return guest_cast<WindowPeek>(evt->message)->windowKind == dialogKind;
    dp = (DialogPeek)FrontWindow();
    if(dp && dp->window.windowKind == dialogKind)
    {
        if(dp->editField != -1)
            TEIdle(dp->textH);
        p.h = evt->where.h;
        p.v = evt->where.v;
        /*-->*/ return evt->what != mouseDown || (FindWindow(p, &wp) == inContent && wp == (WindowPtr)dp);
    }
    return false;
}

bool Executor::get_item_style_info(DialogPtr dp, int item_no,
                                   uint16_t *flags_return, item_style_info_t *style_info)
{
    AuxWinHandle aux_win_h;

    aux_win_h = *lookup_aux_win(dp);
    if(aux_win_h && (*aux_win_h)->dialogCItem)
    {
        Handle items_color_info_h;
        item_color_info_t *items_color_info, *item_color_info;

        items_color_info_h = (*aux_win_h)->dialogCItem;
        items_color_info = (item_color_info_t *)*items_color_info_h;

        item_color_info = &items_color_info[item_no - 1];
        if(item_color_info->data || item_color_info->offset)
        {
            uint16_t flags;
            int style_info_offset;

            flags = item_color_info->data;
            style_info_offset = item_color_info->offset;

            *style_info = *(item_style_info_t *)((char *)items_color_info
                                                 + style_info_offset);
            if(flags & doFontName)
            {
                char *font_name;

                font_name = (char *)items_color_info + style_info->font;
                GetFNum((StringPtr)font_name, &style_info->font);
            }

            *flags_return = flags;
            return true;
        }
    }
    return false;
}

void Executor::ROMlib_drawiptext(DialogPtr dp, itmp ip, int item_no)
{
    bool restore_draw_state_p = false;
    draw_state_t draw_state;
    uint16_t flags;
    item_style_info_t style_info;
    Rect r;

    if(get_item_style_info(dp, item_no, &flags, &style_info))
    {
        draw_state_save(&draw_state);
        restore_draw_state_p = true;

        if(flags & TEdoFont)
            TextFont(style_info.font);
        if(flags & TEdoFace)
            TextFace(style_info.face);
        if(flags & TEdoSize)
            TextSize(style_info.size);
        if(flags & TEdoColor)
            RGBForeColor(&style_info.foreground);
#if 1
        /* NOTE: this code has been "#if 0"d out since it was first written,
	 but testing on the Mac leads me to believe that this works properly.
	 Perhaps we should only do this if we're pretending to be  running
	 System 7, but other than that possibility, I don't see a downside
	 to including this. */

        if(flags & doBColor)
            RGBBackColor(&style_info.background);
#endif
    }

    r = ip->itmr;
    if(ip->itmtype & statText)
    {
        GUEST<Handle> nh_s;
        Handle nh;
        LONGINT l;
        char subsrc[2], *sp;
        GUEST<Handle> *hp;

        *subsrc = '^';
        sp = subsrc + 1;

        nh_s = ip->itmhand;
        HandToHand(&nh_s);
        nh = nh_s;

        for(*sp = '0', hp = LM(DAStrings);
            *sp != '4'; ++*sp, hp++)
        {
            if(*hp)
            {
                for(l = 0; l >= 0;
                    l = Munger(nh, l,
                               (Ptr)subsrc, (LONGINT)2, **hp + 1,
                               (LONGINT)(unsigned char)***hp))
                    ;
            }
        }
        HLock(nh);
        TETextBox(*nh, GetHandleSize(nh), &r, teFlushDefault);
        HUnlock(nh);
        DisposeHandle(nh);
    }
    else if(ip->itmtype & editText)
    {
        Handle text_h;

        text_h = ip->itmhand;
        {
            HLockGuard guard(text_h);
            TETextBox(*text_h, GetHandleSize(text_h),
                      &r, teFlushDefault);
        }

        PORT_PEN_SIZE(qdGlobals().thePort).h = PORT_PEN_SIZE(qdGlobals().thePort).v = 1;
        InsetRect(&r, -3, -3);
        FrameRect(&r);
    }

    if(restore_draw_state_p)
        draw_state_restore(&draw_state);
}

void Executor::dialog_draw_item(DialogPtr dp, itmp itemp, int itemno)
{
    if(itemp->itmtype & ctrlItem)
    {
        /* controls will already have been drawn */
    }
    else if(itemp->itmtype & (statText | editText))
    {
        Rect r;

        // FIXME: #warning This fix helps Energy Scheming, but we really should find out the
        // FIXME: #warning exact semantics for when we should try to draw items, different
        // FIXME: #warning item types may have different behaviors, we also might want to
        // FIXME: #warning look at visRgn and clipRgn.  BTW, we should also test to see
        // FIXME: #warning whether SectRect will really write to location 0

        if(SectRect(&itemp->itmr, &dp->portRect, &r))
            ROMlib_drawiptext(dp, itemp, itemno);
    }
    else if(itemp->itmtype & iconItem)
    {
        Handle icon;

        icon = itemp->itmhand;
        if(CICON_P(icon))
            PlotCIcon(&itemp->itmr, (CIconHandle)icon);
        else
            PlotIcon(&itemp->itmr, icon);
    }
    else if(itemp->itmtype & picItem)
    {
        DrawPicture((PicHandle)itemp->itmhand, &itemp->itmr);
    }
    else
    {
        Handle h;

        /* useritem */
        h = itemp->itmhand;
        if(h)
            CALLUSERITEM(dp, itemno, h);
    }
}

/* #### look into having DrawDialog not draw stuff that can't be seen */

void Executor::C_DrawDialog(DialogPtr dp) /* IMI-418 */
{
    GUEST<INTEGER> *intp;
    INTEGER i, inum;
    itmp ip;
    GrafPtr gp;
    SignedByte state;

    if(dp)
    {
        gp = qdGlobals().thePort;
        SetPort((GrafPtr)dp);
        if(((DialogPeek)dp)->editField != -1)
            TEDeactivate(((DialogPeek)dp)->textH);
        DrawControls((WindowPtr)dp);
        state = HGetState(((DialogPeek)dp)->items);
        HSetState(((DialogPeek)dp)->items, state | LOCKBIT);
        intp = (GUEST<INTEGER> *)*(((DialogPeek)dp)->items);
        ip = (itmp)(intp + 1);
        for(i = *intp, inum = 1; i-- >= 0; inum++, BUMPIP(ip))
        {
            dialog_draw_item(dp, ip, inum);
        }
        if(((DialogPeek)dp)->editField != -1)
            TEActivate(((DialogPeek)dp)->textH);
        HSetState(((DialogPeek)dp)->items, state);
        SetPort(gp);
    }
}

INTEGER Executor::C_FindDialogItem(DialogPtr dp, Point pt) /* IMIV-60 */
{
    GUEST<INTEGER> *intp;
    INTEGER i, inum;
    itmp ip;

    intp = (GUEST<INTEGER> *)*(((DialogPeek)dp)->items);
    ip = (itmp)(intp + 1);
    for(i = *intp, inum = 0; i-- >= 0; inum++, BUMPIP(ip))
        if(PtInRect(pt, &ip->itmr))
            /*-->*/ return inum;
    return -1;
}

void Executor::C_UpdateDialog(DialogPtr dp, RgnHandle rgn) /* IMIV-60 */
{
    GUEST<INTEGER> *intp;
    INTEGER i, inum;
    itmp ip;
    GrafPtr gp;
    SignedByte state;

    gp = qdGlobals().thePort;
    SetPort((GrafPtr)dp);
    ShowWindow((WindowPtr)dp);
    DrawControls((WindowPtr)dp);
    state = HGetState(((DialogPeek)dp)->items);
    HSetState(((DialogPeek)dp)->items, state | LOCKBIT);
    intp = (GUEST<INTEGER> *)*(((DialogPeek)dp)->items);
    ip = (itmp)(intp + 1);
    for(i = *intp, inum = 1; i-- >= 0; inum++, BUMPIP(ip))
    {
        if(RectInRgn(&ip->itmr, rgn))
        {
            dialog_draw_item(dp, ip, inum);
        }
    }
    HSetState(((DialogPeek)dp)->items, state);
    SetPort(gp);
}

Boolean Executor::C_DialogSelect(EventRecord *evt, GUEST<DialogPtr> *dpp,
                                 GUEST<INTEGER> *itemp) /* IMI-417 */
{
    DialogPeek dp;
    Byte c;
    itmp ip;
    GUEST<INTEGER> *intp;
    INTEGER i, iend;
    Point localp;
    GUEST<Point> glocalp;
    GrafPtr gp;
    Boolean itemenabled, retval;
    SignedByte flags;

    dp = (DialogPeek)FrontWindow();
    retval = false;
    *itemp = -1;
    switch(evt->what)
    {
        case mouseDown:
            glocalp = evt->where;
            gp = qdGlobals().thePort;
            SetPort((GrafPtr)dp);
            GlobalToLocal(&glocalp);
            localp = glocalp.get();
            SetPort(gp);
            intp = (GUEST<INTEGER> *)(*dp->items);
            iend = *intp + 2;
            ip = (itmp)(intp + 1);
            for(i = 0;
                ++i != iend && !PtInRect(localp, &(ip->itmr));
                BUMPIP(ip))
                ;
            itemenabled = !(ip->itmtype & itemDisable);
            if(i == iend)
                break;
            if(ip->itmtype & editText)
            {
                if(dp->editField != i - 1)
                    ROMlib_dpntoteh(dp, i);
                TEClick(localp, (evt->modifiers & shiftKey) ? true : false,
                        dp->textH);
            }
            else if(ip->itmtype & ctrlItem)
            {
                ControlHandle c;

                c = (ControlHandle)ip->itmhand;
                if(CTL_HILITE(c) == INACTIVE
                   || !TrackControl(c, localp,
                                    CTL_ACTION(c)))
                    break;
            }
            if(itemenabled)
            {
                *itemp = i;
                retval = true;
                break;
            }
            break;
        case keyDown:
        case autoKey:
            if(dp->editField == -1)
                break;
            c = evt->message & 0xff;
            switch(c)
            {
                case '\t':
                    ROMlib_dpntoteh(dp, 0);
                    TESetSelect((LONGINT)0, (LONGINT)32767,
                                DIALOG_TEXTH(dp));
                    break;
                default:
                    TEKey(c, DIALOG_TEXTH(dp));
            }
            *itemp = dp->editField + 1;
            ip = ROMlib_dpnotoip(dp, *itemp, &flags);
            if(ip)
                retval = !(ip->itmtype & itemDisable);
            else
            {
                warning_unexpected("couldn't resolve editField -- dp = %p, "
                                   "*itemp = %d",
                                   dp, toHost(*itemp));
                retval = false;
            }
            HSetState(((DialogPeek)dp)->items, flags);
            break;
        case updateEvt:
            dp = guest_cast<DialogPeek>(evt->message);
            BeginUpdate((WindowPtr)dp);
            DrawDialog((DialogPtr)dp);
            if(dp->editField != -1)
                TEUpdate(&dp->window.port.portRect, dp->textH);
            EndUpdate((WindowPtr)dp);
            break;
        case activateEvt:
            dp = guest_cast<DialogPeek>(evt->message);
            if(dp->editField != -1)
            {
                if(evt->modifiers & activeFlag)
                    TEActivate(dp->textH);
                else
                    TEDeactivate(dp->textH);
            }
            break;
    }
    *dpp = (DialogPtr)dp;
    return retval;
}

void Executor::DialogCut(DialogPtr dp) /* IMI-418 */
{
    if((((DialogPeek)dp)->editField) != -1)
        TECut(((DialogPeek)dp)->textH);
}

void Executor::DialogCopy(DialogPtr dp) /* IMI-418 */
{
    if((((DialogPeek)dp)->editField) != -1)
        TECopy(((DialogPeek)dp)->textH);
}

void Executor::DialogPaste(DialogPtr dp) /* IMI-418 */
{
    if((((DialogPeek)dp)->editField) != -1)
        TEPaste(((DialogPeek)dp)->textH);
}

void Executor::DialogDelete(DialogPtr dp) /* IMI-418 */
{
    if((((DialogPeek)dp)->editField) != -1)
        TEDelete(((DialogPeek)dp)->textH);
}

void Executor::Beep(INTEGER n)
{
    if(LM(DABeeper))
    {
        ROMlib_hook(dial_soundprocnumber);
        LM(DABeeper)(n);
    }
}
