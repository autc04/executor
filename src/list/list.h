#if !defined(__RSYS_LIST__)
#define __RSYS_LIST__

/*
 * Copyright 1989, 1990 by Abacus Research and Development, Inc.
 * All rights reserved.
 *

 */

#include <ListMgr.h>
#include <MemoryMgr.h>
#include <ResourceMgr.h>
#include <mman/mman.h>


namespace Executor
{
extern void
C_ldef0(INTEGER, Boolean, Rect *, Cell, INTEGER, INTEGER, ListHandle);

using listprocp = UPP<void (INTEGER mess, Boolean sel, Rect *rectp,
                                 Cell cell, INTEGER off, INTEGER len, ListHandle lhand)>;

extern void ROMlib_listcall(INTEGER mess, Boolean sel, Rect *rp, Cell cell,
                            INTEGER off, INTEGER len, ListHandle lhand);

#define LISTCALL(msg, sel, rect, cell, doff, dlen, list) \
    ROMlib_listcall((msg), (sel), (rect), (cell), (doff), (dlen), (list))


#define LISTDECL() INTEGER liststate

#define LISTBEGIN(l) (liststate = HGetState((*l)->listDefProc), \
                      HSetState((*l)->listDefProc, liststate | LOCKBIT))

#define LISTEND(l) HSetState((*l)->listDefProc, liststate)

extern void ROMlib_vminmax(INTEGER *minp, INTEGER *maxp, ListPtr lp);
extern void ROMlib_hminmax(INTEGER *minp, INTEGER *maxp, ListPtr lp);
extern GUEST<INTEGER> *ROMlib_getoffp(Cell cell, ListHandle list);

}
#endif /* !defined(__RSYS_LIST__) */
