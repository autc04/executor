/* Copyright 1989, 1990 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in ListMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <ListMgr.h>
#include <ControlMgr.h>

#include <quickdraw/cquick.h>
#include <list/list.h>
#include <rsys/hook.h>
#include <base/functions.impl.h>

using namespace Executor;

void Executor::C_LDraw(Cell cell, ListHandle list) /* IMIV-275 */
{
    GrafPtr saveport;
    GUEST<RgnHandle> saveclip;
    Rect r;
    GUEST<INTEGER> *ip;
    INTEGER off0, off1;
    Boolean setit;
    LISTDECL();

    if((ip = ROMlib_getoffp(cell, list)))
    {
        off0 = ip[0] & 0x7FFF;
        off1 = ip[1] & 0x7FFF;
        setit = (ip[0] & 0x8000) && (*list)->lActive;
        saveport = qdGlobals().thePort;
        SetPort((*list)->port);
        saveclip = PORT_CLIP_REGION(qdGlobals().thePort);
        PORT_CLIP_REGION(qdGlobals().thePort) = NewRgn();

        C_LRect(&r, cell, list);
        ClipRect(&r);
        LISTBEGIN(list);
        LISTCALL(lDrawMsg, setit, &r, cell, off0, off1 - off0, list);
        LISTEND(list);

        DisposeRgn(PORT_CLIP_REGION(qdGlobals().thePort));
        PORT_CLIP_REGION(qdGlobals().thePort) = saveclip;
        SetPort(saveport);
    }
}

void Executor::C_LSetDrawingMode(Boolean draw, ListHandle list) /* IMIV-275 */
{
    if(draw)
    {
        (*list)->listFlags &= ~lDrawingModeOff;
        
        if((*list)->lActive)
        {
            if((*list)->vScroll)
                ShowControl((*list)->vScroll);
            if((*list)->hScroll)
                ShowControl((*list)->hScroll);
        }
    }
    else
        (*list)->listFlags |= lDrawingModeOff;
}

void Executor::C_LScroll(INTEGER ncol, INTEGER nrow,
                         ListHandle list) /* IMIV-275 */
{
    RgnHandle rh;
    Rect r;
    ControlHandle ch;
    INTEGER tmpi;
    Point p;

    r = (*list)->rView;

    /*
 * TODO:  if either the horizontal or vertical component of a view rectangle
 *        isn't a multiple of the appropriate component of the  cell size then
 *	  you need to allow an extra (blank) cell when scrolled totally to
 *	  the bottom or to the right.
 */

    if(nrow < 0)
    {
        tmpi = (*list)->dataBounds.top - (*list)->visible.top;
        if(tmpi > nrow)
            nrow = tmpi;
    }
    else if(nrow > 0)
    {
        tmpi = (*list)->dataBounds.bottom - (*list)->visible.bottom + 1;
        if(tmpi < nrow)
            nrow = tmpi;
    }

    if(ncol < 0)
    {
        tmpi = (*list)->dataBounds.left - (*list)->visible.left;
        if(tmpi > ncol)
            ncol = tmpi;
    }
    else if(ncol > 0)
    {
        tmpi = (*list)->dataBounds.right - (*list)->visible.right + 1;
        if(tmpi < ncol)
            ncol = tmpi;
    }

    (*list)->visible.top = (*list)->visible.top + nrow;
    (*list)->visible.left = (*list)->visible.left + ncol;

    p.h = (*list)->cellSize.h;
    p.v = (*list)->cellSize.v;
    C_LCellSize(p, list); /*  recalculates visible */

    if(ncol && (ch = (*list)->hScroll))
        SetControlValue(ch, (*list)->visible.left);
    if(nrow && (ch = (*list)->vScroll))
        SetControlValue(ch, (*list)->visible.top);

    if(((*list)->listFlags & lDrawingModeOff) == 0)
    {
        rh = NewRgn();
        ScrollRect(&r, -ncol * (*list)->cellSize.h,
                   -nrow * (*list)->cellSize.v, rh);
        C_LUpdate(rh, list);
        DisposeRgn(rh);
    }
}

void Executor::C_LAutoScroll(ListHandle list) /* IMIV-275 */
{
    GUEST<Cell> gcell;
    Cell cell;

    gcell.h = (*list)->dataBounds.left;
    gcell.v = (*list)->dataBounds.top;
    if(C_LGetSelect(true, &gcell, list))
    {
        cell = gcell.get();
        if(!PtInRect(cell, &(*list)->visible))
        {
            C_LScroll(cell.h - (*list)->visible.left,
                      cell.v - (*list)->visible.top, list);
        }
    }
}

void Executor::C_LUpdate(RgnHandle rgn, ListHandle list) /* IMIV-275 */
{
    Rect r;
    Cell c, csize;
    INTEGER cleft;
    INTEGER top, left, bottom, right;
    ControlHandle ch;

    cleft = c.h = (*list)->visible.left;
    c.v = (*list)->visible.top;
    csize.h = (*list)->cellSize.h;
    csize.v = (*list)->cellSize.v;
    C_LRect(&r, c, list);
    top = r.top;
    left = r.left;
    bottom = top + ((*list)->visible.bottom - (*list)->visible.top) * csize.v;
    right = left + ((*list)->visible.right - (*list)->visible.left) * csize.h;
    while(r.top < bottom)
    {
        while(r.left < right)
        {
            if(RectInRgn(&r, rgn))
                C_LDraw(c, list);
            r.left = r.left + (csize.h);
            r.right = r.right + (csize.h);
            c.h++;
        }
        c.h = cleft;
        c.v++;
        r.top = r.top + (csize.v);
        r.bottom = r.bottom + (csize.v);
        r.left = left;
        r.right = left + csize.h;
    }
    if((ch = (*list)->hScroll))
    {
        if(RectInRgn(&(*ch)->contrlRect, rgn))
            Draw1Control(ch);
    }
    if((ch = (*list)->vScroll))
    {
        if(RectInRgn(&(*ch)->contrlRect, rgn))
            Draw1Control(ch);
    }
}

void Executor::C_LActivate(Boolean act, ListHandle list) /* IMIV-276 */
{
    Cell c;
    Rect r;
    ControlHandle ch;
    Boolean sel;
    GUEST<INTEGER> *ip;
    INTEGER off0, off1;
    GUEST<RgnHandle> saveclip;
    LISTDECL();

    if(!act ^ !(*list)->lActive)
    {

        if(act)
        {
            sel = true;
            if((ch = (*list)->hScroll))
                ShowControl(ch);
            if((ch = (*list)->vScroll))
                ShowControl(ch);
        }
        else
        {
            sel = false;
            if((ch = (*list)->hScroll))
                HideControl(ch);
            if((ch = (*list)->vScroll))
                HideControl(ch);
        }
        LISTBEGIN(list);
        for(c.v = (*list)->visible.top; c.v < (*list)->visible.bottom;
            c.v++)
        {
            for(c.h = (*list)->visible.left; c.h < (*list)->visible.right;
                c.h++)
            {
                if((ip = ROMlib_getoffp(c, list)) && (*ip & 0x8000))
                {
                    off0 = ip[0] & 0x7FFF;
                    off1 = ip[1] & 0x7FFF;
                    saveclip = PORT_CLIP_REGION(qdGlobals().thePort);
                    PORT_CLIP_REGION(qdGlobals().thePort) = NewRgn();
                    C_LRect(&r, c, list);
                    ClipRect(&r);
                    LISTCALL(lHiliteMsg, sel, &r, c, off0, off1 - off0, list);
                    DisposeRgn(PORT_CLIP_REGION(qdGlobals().thePort));
                    PORT_CLIP_REGION(qdGlobals().thePort) = saveclip;
                }
            }
        }
        LISTEND(list);
        (*list)->lActive = !!act;
    }
}

void Executor::ROMlib_listcall(INTEGER mess, Boolean sel, Rect *rp, Cell cell, INTEGER off,
                               INTEGER len, ListHandle lhand)
{
    Handle listdefhand;

    listdefhand = (*lhand)->listDefProc;
    if(listdefhand)
    {
        ListDefUPP lp;

        lp = *(GUEST<ListDefUPP> *)listdefhand;
        if(!lp)
        {
            LoadResource((*lhand)->listDefProc);
            lp = *(GUEST<ListDefUPP> *)(*lhand)->listDefProc;
        }

        ROMlib_hook(list_ldefnumber);
        lp(mess, sel, rp, cell, off, len, lhand);
    }
}
