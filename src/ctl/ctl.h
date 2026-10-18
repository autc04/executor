#if !defined(_CTL_H_)
#define _CTL_H_

/*
 * Copyright 1986, 1989, 1990, 1995 by Abacus Research and Development, Inc.
 * All rights reserved.
 *

 */

#include <ControlMgr.h>
#include <MenuMgr.h>
#include <MemoryMgr.h>
#include <ResourceMgr.h>

#include <mman/mman.h>
#include <quickdraw/cquick.h>


namespace Executor
{
extern const ColorSpec default_ctl_colors[];
extern CTabHandle default_ctl_ctab;
extern AuxCtlHandle default_aux_ctl;

/* accessor macros */
#define CTL_RECT(ctl) ((*ctl)->contrlRect)
#define CTL_TITLE(ctl) ((*ctl)->contrlTitle)

#define CTL_NEXT_CONTROL(ctl) ((*ctl)->nextControl)
#define CTL_OWNER(ctl) ((*ctl)->contrlOwner)
#define CTL_VIS(ctl) ((*ctl)->contrlVis)
#define CTL_HILITE(ctl) ((*ctl)->contrlHilite)
#define CTL_VALUE(ctl) ((*ctl)->contrlValue)
#define CTL_MIN(ctl) ((*ctl)->contrlMin)
#define CTL_MAX(ctl) ((*ctl)->contrlMax)

#define CTL_DEFPROC(ctl) ((*ctl)->contrlDefProc)
#define CTL_DATA(ctl) ((*ctl)->contrlData)
#define CTL_ACTION(ctl) ((*ctl)->contrlAction)
#define CTL_REF_CON(ctl) ((*ctl)->contrlRfCon)




//#define CTL_HILITE(ctl) ((uint8_t)CTL_HILITE(ctl))
//#define CTL_DATA(ctl) (CTL_DATA(ctl))

#define CTL_ACTION_AS_LONG(ctl) (guest_cast<LONGINT>(CTL_ACTION(ctl)))


extern CTabHandle default_ctl_ctab;
extern GUEST<AuxCtlHandle> *lookup_aux_ctl(ControlHandle ctl);

extern int32_t C_cdef0(int16_t var, ControlHandle ctl, int16_t mess,
                       int32_t param);
extern int32_t C_cdef16(int16_t var, ControlHandle ctl, int16_t mess,
                        int32_t param);
extern int32_t C_cdef1008(int16_t var, ControlHandle ctl, int16_t mess,
                          int32_t param);

#define VAR(w) (GetControlVariant((w)))

extern void sb_ctl_init(void);

extern LONGINT ROMlib_ctlcall(ControlHandle c, INTEGER i, LONGINT l);
#define CTLCALL(c, i, l) ROMlib_ctlcall((c), (i), (l))

class CtlCallGuard : private ThePortGuard
{
public:
    CtlCallGuard(ControlHandle ctl)
        : ThePortGuard(CTL_OWNER(ctl).get())
    {
    }
};

#define ENTIRECONTROL 0
#define MAXPARTCODE 253
#define INACTIVE 255
#define ALLINDICATORS 129

#define POPUP_MENU(popup) ((*popup)->menu)
#define POPUP_MENU_ID(popup) ((*popup)->menu_id)




#define POPUP_TITLE_WIDTH(popup) ((*popup)->title_width)
#define POPUP_FLAGS(popup) ((*popup)->flags)

/* private fields */
struct popup_data
{
    GUEST_STRUCT;
    GUEST<MenuHandle> menu;
    GUEST<int16_t> menu_id;
    int16_t title_width;
    int flags;
};

typedef struct popup_data popup_data_t;
typedef popup_data_t *popup_data_ptr;

typedef GUEST<popup_data_ptr> *popup_data_handle;

struct thumbstr
{
    GUEST_STRUCT;
    GUEST<Rect> _tlimit;
    GUEST<Rect> _tslop;
    GUEST<INTEGER> _taxis;
};

struct contrlrestype
{
    GUEST_STRUCT;
    GUEST<Rect> _crect;
    GUEST<INTEGER> _cvalue;
    GUEST<INTEGER> _cvisible;
    GUEST<INTEGER> _cmax;
    GUEST<INTEGER> _cmin;
    GUEST<INTEGER> _cprocid;
    GUEST<LONGINT> _crefcon;
    GUEST<Byte> _ctitle;
};

struct lsastr
{
    GUEST_STRUCT;
    GUEST<Rect> limitRect;
    GUEST<Rect> slopRect;
    GUEST<INTEGER> axis;
};

extern void image_arrow_up_active_init(void);
extern void image_arrow_up_inactive_init(void);
extern void image_arrow_down_active_init(void);
extern void image_arrow_down_inactive_init(void);
extern void image_arrow_left_active_init(void);
extern void image_arrow_left_inactive_init(void);
extern void image_arrow_right_active_init(void);
extern void image_arrow_right_inactive_init(void);
extern void image_thumb_horiz_init(void);
extern void image_thumb_vert_init(void);
extern void image_active_init(void);
extern void image_ractive_init(void);
extern void image_go_away_init(void);
extern void image_grow_init(void);
extern void image_zoom_init(void);
extern void image_apple_init(void);

extern void C_new_draw_scroll(INTEGER depth, INTEGER flags, GDHandle target,
                              LONGINT l);

extern void C_new_pos_ctl(INTEGER depth, INTEGER flags, GDHandle target,
                          LONGINT l);

static_assert(sizeof(popup_data) == 12);
static_assert(sizeof(thumbstr) == 18);
static_assert(sizeof(contrlrestype) == 24);
static_assert(sizeof(lsastr) == 18);
}
#endif /* !_CTL_H_ */
