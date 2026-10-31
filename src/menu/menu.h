
#if !defined(__MENU_H_)
#define __MENU_H_

/*
 * Copyright 1986, 1989, 1990 by Abacus Research and Development, Inc.
 * All rights reserved.
 *

 */

#include <MenuMgr.h>
#include <ResourceMgr.h>
#include <mman/mman.h>


namespace Executor
{
#define MI_ID(mi) ((*mi)->menuID)
#define MI_WIDTH(mi) ((*mi)->menuWidth)
#define MI_HEIGHT(mi) ((*mi)->menuHeight)
#define MI_PROC(mi) ((*mi)->menuProc)
#define MI_ENABLE_FLAGS(mi) ((*mi)->enableFlags)
#define MI_DATA(mi) ((*mi)->menuData)
#define MI_TITLE(mi) ((*mi)->menuTitle)

typedef struct mext
{
    GUEST_STRUCT;
    GUEST<Byte> micon;
    GUEST<Byte> mkeyeq;
    GUEST<Byte> mmarker;
    GUEST<Byte> mstyle;
    GUEST<Byte> mnextlen;
} * mextp;

extern void C_mdef0(INTEGER, MenuHandle, Rect *, Point, GUEST<INTEGER> *);
extern int32_t C_mbdf0(int16_t, int16_t, int16_t, int32_t);

#define SIZEOFMEXT 5
#define SIZEOFMINFO (sizeof(MenuInfo) - sizeof(Str255) + 1)

extern mextp ROMlib_mitemtop(MenuHandle mh, INTEGER item, StringPtr *ssp);
extern INTEGER ROMlib_mentosix(INTEGER mid);
extern LONGINT ROMlib_menuhelper(MenuHandle mh, Rect *saver, LONGINT where,
                                 Boolean ispopup, INTEGER ndisplayed);

/* Non-zero while a menu is being tracked (pull-down, popup, hierarchical or
 * sticky).  Mouse-wheel handling must not inject clicks into the tracking
 * loop's event queue. */
extern int ROMlib_menu_tracking_depth;
extern Boolean ROMlib_shouldalarm(void);

#define mbDraw 0
#define mbHit 1
#define mbCalc 2
#define mbInit 3
#define mbDispose 4
#define mbHilite 5
#define mbHeight 6
#define mbSave 7
#define mbRestore 8
#define mbRect 9
#define mbSaveAlt 10
#define mbResetAlt 11
#define mbMenuRgn 12

#define MLMAX 16

struct muelem
{
    GUEST_STRUCT;
    GUEST<MenuHandle> muhandle;
    GUEST<INTEGER> muleft;
};

typedef struct menu_elt
{
    MenuHandle /* elt */ muhandle;
    /* for a hierarchical menu, the menu_left field is reserved */
    INTEGER /* menu_left */ muleft;
} menu_elt;

#define ML_MENU(ml, index) \
    (((menu_elt *)&(*ml)->data)[index])
#define ML_HMENU(ml, index)                          \
    (((menu_elt *)((char *)*(ml)                 \
                   + ML_LAST_MENU_OFFSET(ml)         \
                   + sizeof(menu_list)               \
                   + sizeof(INTEGER) /* lastHMenu */ \
                   + sizeof(PixMapHandle) /* menuTitleSave */))[index])

#define ML_LAST_MENU_OFFSET(ml) ((*ml)->last_menu_offset)
#define ML_LAST_RIGHT(ml) ((*ml)->last_right)
#define ML_RES_ID(ml) ((*ml)->mb_res_id)
#define ML_LAST_HMENU_OFFSET(ml)          \
    (*(INTEGER *)((char *)*(ml)         \
                  + ML_LAST_MENU_OFFSET(ml) \
                  + sizeof(menu_elt)))
#define ML_MENU_TITLE_SAVE(ml)                                   \
    ((*(GUEST<PixMapHandle> *)((char *)*(ml)                   \
                               + ML_LAST_MENU_OFFSET(ml)           \
                               + sizeof(menu_elt)                  \
                               + sizeof(INTEGER) /* lastHMenu */)) \
         .p)

struct menu_list
{
    GUEST_STRUCT;
    GUEST<INTEGER> last_menu_offset;
    GUEST<INTEGER> last_right;
    GUEST<INTEGER> mb_res_id;
    /* other stuff... */
    GUEST<char> data;
};

typedef menu_list *menu_list_ptr;

typedef GUEST<menu_list_ptr> *menu_list_handle;

struct menulist
{
    GUEST_STRUCT;
    GUEST<INTEGER> muoff;
    GUEST<INTEGER> muright;
    GUEST<INTEGER> mufu;
    GUEST<muelem[MLMAX]> mulist; /* WILL NEED Cx() */
};

typedef menulist *menulistp;

typedef GUEST<menulistp> *mlhandle;

/* Menu Color Entry accessors */
#define MCENTRY_RGB1(entry) ((entry)->mctRGB1)
#define MCENTRY_RGB2(entry) ((entry)->mctRGB2)
#define MCENTRY_RGB3(entry) ((entry)->mctRGB3)
#define MCENTRY_RGB4(entry) ((entry)->mctRGB4)

#define MCENTRY_ID(entry) ((entry)->mctID)
#define MCENTRY_ITEM(entry) ((entry)->mctItem)
#define MCENTRY_RESERVED(entry) ((entry)->mctReserved)

#define MENULIST ((mlhandle)LM(MenuList))

#define MENULEFT 10

#define DRAWMENUBAR 0
#define CLEARMENUBAR (-1)

struct mbdfheader
{
    GUEST_STRUCT;
    GUEST<INTEGER> lastMBSave; /* offset to top most menu saved */
    GUEST<Handle> mbCustomStorage; /* for custom jobs (i.e. we don't use) */
    GUEST<Rect> mbItemRect; /* currently chosen menu */
    GUEST<Byte> mbMenuDelay; /* MenuDelay from param ram */
    GUEST<Byte> mbMenuDrag; /* MenuDrag from param ram */
    GUEST<INTEGER> mbUglyScroll; /* HMenu flag having to do with scrolling?? */
    GUEST<INTEGER> mbIconState; /* ??? NMgr icon state */
};

typedef mbdfheader *mbdfheaderptr;

typedef GUEST<mbdfheaderptr> *mbdfheaderhand;

struct mbdfentry
{
    GUEST_STRUCT;
    GUEST<Rect> mbRectSave; /* where it is on screen */
    GUEST<Handle> mbBitsSave; /* where the bits are */
    GUEST<INTEGER> mbMenuDir; /* what direction the menu was placed */
    GUEST<INTEGER> mbMLOffset; /* 6 byte offset into LM(MenuList) */
    GUEST<MenuHandle> mbMLHandle; /* the handle from LM(MenuList) */
    GUEST<INTEGER> mbTopScroll; /* copy of LM(TopMenuItem) */
    GUEST<INTEGER> mbBotScroll; /* copy of LM(AtMenuBottom) */
    GUEST<LONGINT> mbReserved; /* i dunno */
};

#define NMBDFENTRIES 5
#define MBDFSTRUCTBYTES ((NMBDFENTRIES + 1) * sizeof(mbdfentry))

#define MBRIGHTDIR 0
#define MBLEFTDIR 1

#define MBSAVELOC ((mbdfheaderhand)LM(MBSaveLoc))

#define SLOP 13

extern void ROMlib_menucall(INTEGER mess, MenuHandle themenu, Rect *menrect,
                            Point hit, GUEST<INTEGER> *which);
extern LONGINT ROMlib_mbdfcall(INTEGER msg, INTEGER param1, LONGINT param2);

#define MENUCALL ROMlib_menucall
#define MBDFCALL ROMlib_mbdfcall

#define MBDFDECL() INTEGER mbdfstate

#define MBDFBEGIN() (mbdfstate = HGetState(LM(MBDFHndl)), HSetState(LM(MBDFHndl), \
                                                                    mbdfstate | LOCKBIT))

#define MBDFEND() HSetState(LM(MBDFHndl), mbdfstate)

#define NOTHITINMBAR 0
#define NOTHIT (-1)

#define image_bits(bits)                               \
    (((((0x##bits##UL) >> (7 * 4)) << 7) & (1 << 7))   \
     | ((((0x##bits##UL) >> (6 * 4)) << 6) & (1 << 6)) \
     | ((((0x##bits##UL) >> (5 * 4)) << 5) & (1 << 5)) \
     | ((((0x##bits##UL) >> (4 * 4)) << 4) & (1 << 4)) \
     | ((((0x##bits##UL) >> (3 * 4)) << 3) & (1 << 3)) \
     | ((((0x##bits##UL) >> (2 * 4)) << 2) & (1 << 2)) \
     | ((((0x##bits##UL) >> (1 * 4)) << 1) & (1 << 1)) \
     | ((((0x##bits##UL) >> (0 * 4)) << 0) & (1 << 0)))

typedef enum { HILITE,
               RESTORE } highstate;

/* menu item icon code */

#define ICON_PAD 4 /* space on either side */
typedef struct con_info
{
    /* true if this is a color icon */
    int color_icon_p;

    /* pointer to the icon data */
    Handle icon;

    /* for convient access */
    int width, height;
} icon_info_t;

typedef struct mct_res
{
    GUEST_STRUCT;
    GUEST<int16_t> n_entries;
    GUEST<MCEntry[1]> entries;
} mct_res_t;

struct mbartype
{
    GUEST_STRUCT;
    GUEST<INTEGER> nmen;
    GUEST<INTEGER[1]> mrid;
};

typedef struct
{
    muelem *startp;
    muelem *endp;
} startendpairs[2];

typedef struct table
{
    int32_t lasttick;
    int16_t count;
    struct tableentry
    {
        int16_t top;
        StringPtr name;
        mextp options;
    } entry[1];
} * tablePtr;
typedef GUEST<tablePtr> *tableHandle;

void cleanup_icon_info(icon_info_t *info);
int get_icon_info(mextp item_info, icon_info_t *info, int need_icon_p);


void menu_bar_color(RGBColor *bar_color);
void menu_title_color(int16_t id, RGBColor *title_color);
void menu_bk_color(int16_t id, RGBColor *bk_color);
void menu_item_colors(int16_t id, int16_t item,
                      RGBColor *bk_color, RGBColor *name_color,
                      RGBColor *mark_color, RGBColor *command_color);

void menu_delete_entries(int16_t menu_id);

static_assert(sizeof(mext) == 5);
static_assert(sizeof(muelem) == 6);
static_assert(sizeof(menu_list) == 8);
static_assert(sizeof(menulist) == 102);
static_assert(sizeof(mbdfheader) == 20);
static_assert(sizeof(mbdfentry) == 28);
static_assert(sizeof(mct_res_t) == 32);
static_assert(sizeof(mbartype) == 4);
}

#endif /* !_MENU_H_ */
