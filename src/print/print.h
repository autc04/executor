#if !defined(__RSYS_PRINT__)
#define __RSYS_PRINT__

/*
 * Copyright 1989 - 1997 by Abacus Research and Development, Inc.
 * All rights reserved.
 *

 */

#include <DialogMgr.h>
#include <PrintMgr.h>
#include <print/nextprint.h>
#include <print/ini.h>
#include <prefs/prefs.h>

#if defined(CYGWIN32)
#include "win_print.h"

extern win_printp_t ROMlib_wp;
#endif


namespace Executor
{
enum
{
    postscriptbegin = 190,
    postscriptend = 191,
    postscripthandle = 192,
    postscriptfile = 193,
    postscripttextis = 194,
    psbeginnosave = 196,
    rotatebegin = 200,
    rotateend = 201,
    rotatecenter = 202,
    textbegin = 150,
    textend = 151,
    textcenter = 154,
};

enum
{
    LAYOUT_OK_NO = 1,
    LAYOUT_PRINTER_NAME_NO = 3,
    LAYOUT_PAPER_NO = 5,
    LAYOUT_PRINTER_TYPE_NO = 6,
    LAYOUT_PORTRAIT_ICON_NO = 8,
    LAYOUT_LANDSCAPE_ICON_NO = 9,
    LAYOUT_PRINTER_TYPE_LABEL_NO = 11,
    LAYOUT_CIRCLE_OK_NO = 12,
    LAYOUT_PORTRAIT_NO = 13,
    LAYOUT_LANDSCAPE_NO = 14,
    LAYOUT_PORT_LABEL_NO = 15,
    LAYOUT_PORT_MENU_NO = 16,
    LAYOUT_FILENAME_LABEL_NO = 17,
    LAYOUT_FILENAME_NO = 18,
};

enum
{
    PRINT_OK_NO = 1,
    PRINT_COPIES_LABEL_NO = 6,
    PRINT_COPIES_BOX_NO = 7,
    PRINT_ALL_RADIO_NO = 9,
    PRINT_FROM_RADIO_NO = 10,
    PRINT_FIRST_BOX_NO = 11,
    PRINT_LAST_BOX_NO = 13,
    PRINT_CIRCLE_OK_NO = 14,
};

extern GrafPort printport;
extern INTEGER ROMlib_printresfile;
extern LONGINT pagewanted;
extern bool ROMlib_suppressclip;

extern char *ROMlib_spool_file;
extern ini_key_t ROMlib_printer;
extern ini_key_t ROMlib_port;

enum
{
    GetRslData = 4,
    SetRsl,
    DraftBits,
    NoDraftBits,
    GetRotn
};

struct TGnlData
{
    GUEST_STRUCT;
    GUEST<INTEGER> iOpCode;
    GUEST<INTEGER> iError;
    GUEST<LONGINT> lReserved;
};

struct TRslRg
{
    GUEST_STRUCT;
    GUEST<INTEGER> iMin;
    GUEST<INTEGER> iMax;
};

struct TRslRec
{
    GUEST_STRUCT;
    GUEST<INTEGER> iXRsl;
    GUEST<INTEGER> iYRsl;
};

struct TGetRslBlk
{
    GUEST_STRUCT;
    GUEST<INTEGER> iOpCode;
    GUEST<INTEGER> iError;
    GUEST<LONGINT> lReserved;
    GUEST<INTEGER> iRgType;
    GUEST<TRslRg> xRslRg;
    GUEST<TRslRg> yRslRg;
    GUEST<INTEGER> iRslRecCnt;
    GUEST<TRslRec[27]> rgRslRec;
};

struct TSetRslBlk
{
    GUEST_STRUCT;
    GUEST<INTEGER> iOpCode;
    GUEST<INTEGER> iError;
    GUEST<LONGINT> lReserved;
    GUEST<THPrint> hPrint;
    GUEST<INTEGER> iXRsl;
    GUEST<INTEGER> iYRsl;
};

typedef struct TTxtPicRec
{
    GUEST_STRUCT;
    GUEST<Byte> tJus;
    GUEST<Byte> tFlop;
    GUEST<INTEGER> tAngle;
    GUEST<Byte> tLine;
    GUEST<Byte> tCmnt;
    GUEST<Fixed> tAngleFixed;
} * TTxtPicPtr;

#define TEXTPIC_JUST(h) ((*h)->tJus)
#define TEXTPIC_FLOP(h) ((*h)->tFlop)
#define TEXTPIC_ANGLE(h) ((*h)->tAngle)
#define TEXTPIC_LINE(h) ((*h)->tLine)
#define TEXTPIC_COMMENT(h) ((*h)->tCmnt)
#define TEXTPIC_ANGLE_FIXED(h) ((*h)->tAngleFixed)


typedef GUEST<TTxtPicPtr> *TTxtPicHdl;

enum
{
    tJusNone = 0,
    tJusLeft = 1,
    tJusCenter = 2,
    tJusRight = 3,
    tJusFull = 4
};

enum
{
    tFlipNone = 0,
    tFlipHorizontal = 1,
    tFlipVertical = 2,
};

typedef struct TCenterRec
{
    GUEST_STRUCT;
    GUEST<Fixed> y;
    GUEST<Fixed> x;
} * TCenterRecPtr;

#define TEXTCENTER_Y(h) ((*h)->y)

#define TEXTCENTER(h) ((*h)->x)


typedef GUEST<TCenterRecPtr> *TCenterRecHdl;

extern std::string ROMlib_document_paper_sizes;
extern ini_key_t ROMlib_paper_orientation;
extern std::string ROMlib_paper_size;
extern std::string ROMlib_paper_size_name;
extern std::string ROMlib_paper_size_name_terminator;
extern int ROMlib_rotation;
extern int ROMlib_translate_x;
extern int ROMlib_translate_y;
extern int ROMlib_paper_x;
extern int ROMlib_paper_y;

/* optional resolution made available via GetRslData call */
extern INTEGER ROMlib_optional_res_x, ROMlib_optional_res_y;

/* actual resolution chosen -- currently either 72dpi (default) or
   the ROMlib_optional_res */

extern int ROMlib_resolution_x;
extern int ROMlib_resolution_y;

extern bool error_parse_print_size(const char *size_string);

enum
{
    NoSuchRsl = 1,
    OpNotImpl
};

extern bool ROMlib_pick_likely_print_name(StringPtr name);

extern void ROMlib_trytomatch(char *retval, LONGINT index);

extern void ROMlib_set_default_resolution(THPrint hPrint,
                                          INTEGER vres, INTEGER hres);

extern void do_textbegin(TTxtPicHdl h);
extern void do_textcenter(TCenterRecHdl h);
extern void do_textend(void);

extern void print_reinit(void);

extern void printer_init(void);
extern void update_printing_globals(void);
extern char *cstring_from_str255(Str255 text);

extern void disable_stdtext(void);
extern void enable_stdtext(void);

extern void ROMlib_rotatebegin(LONGINT flippage, LONGINT angle);
extern void ROMlib_rotatecenter(double yoffset, double xoffset);
extern void ROMlib_rotateend(void);
extern void ROMlib_gsave(void);
extern void ROMlib_grestore(void);

extern void ROMlib_acknowledge_job_dialog(THPrint thprint);



static_assert(sizeof(TGnlData) == 8);
static_assert(sizeof(TRslRg) == 4);
static_assert(sizeof(TRslRec) == 4);
static_assert(sizeof(TGetRslBlk) == 128);
static_assert(sizeof(TSetRslBlk) == 16);
static_assert(sizeof(TTxtPicRec) == 10);
static_assert(sizeof(TCenterRec) == 8);
}

extern "C" {
extern FILE *ROMlib_printfile;
}
#endif /* !defined(__RSYS_PRINT__) */
