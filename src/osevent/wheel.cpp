/* Copyright 2026 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Translate host mouse-wheel events into clicks on the scroll bar that is
 * responsible for the window under the pointer.
 *
 * The guest has no concept of a mouse wheel, so instead of inventing an
 * event we post a normal mouseDown/mouseUp pair at the relevant arrow.  The
 * application's own FindControl/TrackControl handling then does the actual
 * scrolling.
 *
 * This runs on the emulator thread from an interrupt (see EventSink), so it
 * deliberately avoids FindWindow/FindControl/TestControl and any port state:
 * those may call guest code.  It only reads WindowRecord/ControlRecord fields
 * and region data, and posts events through ROMlib_PPostEvent.
 */

#include <base/common.h>

#include <WindowMgr.h>
#include <ControlMgr.h>
#include <EventMgr.h>
#include <ToolboxEvent.h>
#include <OSEvent.h>
#include <OSUtil.h>
#include <SysErr.h>
#include <QuickDraw.h>

#include <quickdraw/cquick.h>
#include <wind/wind.h>
#include <ctl/ctl.h>
#include <menu/menu.h>
#include <osevent/osevent.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace Executor;

namespace
{
bool wheelDebug()
{
    static const bool enabled = std::getenv("EXECUTOR_WHEEL_DEBUG") != nullptr;
    return enabled;
}

#define WHEEL_DEBUG(...)                             \
    do                                               \
    {                                                \
        if(wheelDebug())                             \
            fprintf(stderr, "[wheel] " __VA_ARGS__); \
    } while(0)

/* A single host wheel event normally contains one notch, but high-resolution
 * wheels and trackpads may report several.  Cap the synthetic clicks so we do
 * not fill the 20-element event queue (and drop the oldest events) at once. */
constexpr int MAX_CLICKS_PER_EVENT = 3;

/* Corrupt or stale lists must not spin forever. */
constexpr int MAX_WINDOW_WALK = 256;
constexpr int MAX_CONTROL_WALK = 256;

bool window_containing_point(Point global, WindowPeek *wp_out)
{
    int guard = 0;

    for(WindowPeek wp = LM(WindowList); wp && guard++ < MAX_WINDOW_WALK;
        wp = WINDOW_NEXT_WINDOW(wp))
    {
        RgnHandle cont = WINDOW_CONT_REGION(wp);
        bool contains = cont && C_PtInRgn(global, cont);

        WHEEL_DEBUG("window %p visible=%d cont=%p contains=%d\n",
                    (void *)wp, (int)WINDOW_VISIBLE(wp), (void *)cont,
                    (int)contains);

        if(WINDOW_VISIBLE(wp) && contains)
        {
            *wp_out = wp;
            return true;
        }
    }
    return false;
}

/* Best-effort classification of a standard (or standard-shaped) scroll bar.
 * We deliberately do not consult the CDEF: this runs in an interrupt. */
bool is_scrollbar_candidate(WindowPeek wp, ControlHandle c)
{
    if(!c || !*c || !CTL_DEFPROC(c).get())
        return false;
    if(CTL_OWNER(c).get() != (WindowPtr)wp)
        return false;
    if(!CTL_VIS(c) || CTL_HILITE(c) == 255)
        return false;
    if(CTL_MIN(c).get() >= CTL_MAX(c).get())
        return false;

    Rect r = CTL_RECT(c);
    return r.right > r.left && r.bottom > r.top;
}

/* Find the scroll bar responsible for this axis:
 *   - a bar directly under the pointer, or
 *   - otherwise the nearest bar to the right (vertical) / below (horizontal).
 * Long document scroll bars on the right edge are thus found even when the
 * pointer is anywhere in the document body. */
ControlHandle nearest_scrollbar(WindowPeek wp, Point p, bool vertical)
{
    ControlHandle best = nullptr;
    int best_edge = 0x7FFFFFFF;
    int guard = 0;

    for(ControlHandle c = WINDOW_CONTROL_LIST(wp);
        c && guard++ < MAX_CONTROL_WALK; c = CTL_NEXT_CONTROL(c))
    {
        if(!is_scrollbar_candidate(wp, c))
            continue;

        Rect r = CTL_RECT(c);
        int width = r.right - r.left;
        int height = r.bottom - r.top;

        WHEEL_DEBUG("  ctl %p owner=%p vis=%d hilite=%d min=%d max=%d "
                    "rect={%d,%d,%d,%d} defproc=%p\n",
                    (void *)c, (void *)CTL_OWNER(c).get(), (int)CTL_VIS(c),
                    (int)CTL_HILITE(c), (int)CTL_MIN(c).get(),
                    (int)CTL_MAX(c).get(), (int)r.top, (int)r.left,
                    (int)r.bottom, (int)r.right, (void *)CTL_DEFPROC(c).get());

        if(vertical)
        {
            if(height <= width)
                continue;
        }
        else
        {
            if(width <= height)
                continue;
        }

        if(C_PtInRect(p, &r))
        {
            WHEEL_DEBUG("    -> contains pointer\n");
            return c;
        }

        if(vertical)
        {
            if(p.v < r.top || p.v > r.bottom)
                continue;
        }
        else
        {
            if(p.h < r.left || p.h > r.right)
                continue;
        }

        int edge = vertical ? r.left : r.top;
        int across = vertical ? p.h : p.v;
        if(edge >= across && edge < best_edge)
        {
            best = c;
            best_edge = edge;
            WHEEL_DEBUG("    -> candidate edge=%d\n", edge);
        }
    }
    return best;
}

struct ReentryGuard
{
    bool &flag;

    explicit ReentryGuard(bool &flag)
        : flag(flag)
    {
        flag = true;
    }
    ~ReentryGuard()
    {
        flag = false;
    }
};
} // namespace

void Executor::ROMlib_handleWheelEvent(int deltaX, int deltaY, int h, int v)
{
    WHEEL_DEBUG("handle dx=%d dy=%d h=%d v=%d WWExist=%d auxctl=%p "
                "windowList=%p MBState=%d menuDepth=%d SysEvtMask=0x%x\n",
                deltaX, deltaY, h, v, (int)LM(WWExist),
                (void *)default_aux_ctl, (void *)LM(WindowList),
                (int)LM(MBState), ROMlib_menu_tracking_depth,
                (unsigned)LM(SysEvtMask));

    /* The Window Manager sets WWExist at the end of InitWindows(), after
     * WDEFs, regions and ctl_color_init() are ready.  There is no explicit
     * Control Manager initialisation trap; default_aux_ctl is its equivalent
     * ready flag. */
    if(LM(WWExist) != EXIST_YES || !default_aux_ctl || !LM(WindowList))
    {
        WHEEL_DEBUG("bail: Window/Control Manager not initialised\n");
        return;
    }

    /* Do not inject clicks while a menu owns the mouse, or while a mouse
     * button is held (drag/tracking loops). */
    if(ROMlib_menu_tracking_depth > 0)
    {
        WHEEL_DEBUG("bail: menu tracking\n");
        return;
    }
    if(LM(MBState) == 0)
    {
        WHEEL_DEBUG("bail: mouse button is down\n");
        return;
    }

    static bool in_handler = false;
    if(in_handler)
        return;
    ReentryGuard guard(in_handler);

    bool vertical = deltaY != 0
        && std::abs(deltaY) >= std::abs(deltaX);
    int delta = vertical ? deltaY : deltaX;
    WHEEL_DEBUG("axis=%s delta=%d\n", vertical ? "vertical" : "horizontal",
                delta);
    if(delta == 0)
    {
        WHEEL_DEBUG("bail: zero delta\n");
        return;
    }

    /* Avoid posting half a click if the application disabled one of them. */
    if(!((1 << mouseDown) & LM(SysEvtMask))
       || !((1 << mouseUp) & LM(SysEvtMask)))
    {
        WHEEL_DEBUG("bail: mouse events disabled in SysEvtMask\n");
        return;
    }

    Point global = {(int16_t)v, (int16_t)h};
    WindowPeek wp = nullptr;
    if(!window_containing_point(global, &wp))
    {
        WHEEL_DEBUG("bail: no window content contains the pointer\n");
        return;
    }
    WHEEL_DEBUG("window=%p bounds={%d,%d,%d,%d}\n", (void *)wp,
                (int)PORT_BOUNDS(wp).top, (int)PORT_BOUNDS(wp).left,
                (int)PORT_BOUNDS(wp).bottom, (int)PORT_BOUNDS(wp).right);

    /* Window-local coordinates.  GlobalToLocal/LocalToGlobal are exactly this
     * arithmetic; doing it directly avoids touching qdGlobals().thePort from
     * an interrupt. */
    Point local = {(int16_t)(global.v + PORT_BOUNDS(wp).top),
                   (int16_t)(global.h + PORT_BOUNDS(wp).left)};

    WHEEL_DEBUG("pointer local=(%d,%d)\n", (int)local.h, (int)local.v);
    ControlHandle bar = nearest_scrollbar(wp, local, vertical);
    if(!bar)
    {
        WHEEL_DEBUG("bail: no %s scroll bar found\n",
                    vertical ? "vertical" : "horizontal");
        return;
    }

    Rect r = CTL_RECT(bar);
    int width = r.right - r.left;
    int height = r.bottom - r.top;
    Point click;

    if(vertical)
    {
        click.h = r.left + width / 2;
        click.v = delta > 0 ? r.top + width / 2 : r.bottom - width / 2;
    }
    else
    {
        click.h = delta > 0 ? r.right - height / 2 : r.left + height / 2;
        click.v = r.top + height / 2;
    }

    WHEEL_DEBUG("bar=%p rect={%d,%d,%d,%d} click local=(%d,%d)\n",
                (void *)bar, (int)r.top, (int)r.left, (int)r.bottom,
                (int)r.right, (int)click.h, (int)click.v);

    /* Make sure the arrow is actually visible in the window: a scroll bar can
     * be covered by a window in front even though the pointer itself is on a
     * visible part of this window.  visRgn is in port-local coordinates, like
     * contrlRect. */
    RgnHandle vis = PORT_VIS_REGION(wp);
    bool visible = vis && C_PtInRgn(click, vis);
    WHEEL_DEBUG("visRgn=%p contains click=%d\n", (void *)vis, (int)visible);
    if(!visible)
    {
        WHEEL_DEBUG("bail: arrow is not in the window's visRgn\n");
        return;
    }

    Point click_global = {(int16_t)(click.v - PORT_BOUNDS(wp).top),
                          (int16_t)(click.h - PORT_BOUNDS(wp).left)};

    int clicks = std::min(std::abs(delta), MAX_CLICKS_PER_EVENT);
    if(clicks == 0)
    {
        WHEEL_DEBUG("bail: zero clicks\n");
        return;
    }

    LONGINT now = C_TickCount();
    short mods = ROMlib_GetModifiers();
    WHEEL_DEBUG("click global=(%d,%d) clicks=%d mods=0x%x\n", (int)click_global.h,
                (int)click_global.v, clicks, (int)mods);

    /* btnState in EventRecord.modifiers means the button is *up*; a real
     * mouseDown clears it and a real mouseUp sets it. */
    for(int i = 0; i < clicks; i++)
    {
        OSErr downErr = ROMlib_PPostEvent(mouseDown, 0, (GUEST<EvQElPtr> *)0,
                                          now, click_global,
                                          mods & ~btnState);
        OSErr upErr = ROMlib_PPostEvent(mouseUp, 0, (GUEST<EvQElPtr> *)0, now,
                                        click_global, mods | btnState);
        WHEEL_DEBUG("posted mouseDown=%d mouseUp=%d\n", (int)downErr,
                    (int)upErr);
    }
}
