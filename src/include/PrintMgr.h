#if !defined(__PRINTING__)
#define __PRINTING__

#include <DialogMgr.h>

#define MODULE_NAME PrintMgr
#include <base/api-module.h>

/*
 * Copyright 1986, 1989, 1990 by Abacus Research and Development, Inc.
 * All rights reserved.
 *

 */

namespace Executor
{
enum
{
    bDraftLoop = 0,
    bSpoolLoop = 1,
};

enum
{
    bDevCItoh = 1,
    bDevLaser = 3,
};

enum
{
    iPFMaxPgs = 128,
};

enum
{
    iPrSavPFil = (-1),
    iIOAbort = (-27),
    MemFullErr = (-108),
    iPrAbort = 128,
};

enum
{
    iPrDevCtl = 7,
    lPrReset = 0x10000,
    lPrLineFeed = 0x30000,
    lPrLFSixth = 0x3FFFF,
    lPrPageEnd = 0x20000,
    iPrBitsCtl = 4,
    lScreenBits = 0,
    lPaintBits = 1,
    iPrIOCtl = 5,
};

BEGIN_EXECUTOR_ONLY
const char *const sPrDrvr = ".Print";
END_EXECUTOR_ONLY

enum
{
    iPrDrvrRef = (-3),
};

struct TPrPort
{
    GUEST_STRUCT;
    GUEST<GrafPort> gPort;
    GUEST<QDProcs> saveprocs;
    GUEST<LONGINT[4]> spare;
    GUEST<Boolean> fOurPtr;
    GUEST<Boolean> fOurBits;
};
typedef TPrPort *TPPrPort;

struct TPrInfo
{
    GUEST_STRUCT;
    GUEST<INTEGER> iDev;
    GUEST<INTEGER> iVRes;
    GUEST<INTEGER> iHRes;
    GUEST<Rect> rPage;
};

typedef enum { feedCut,
               feedFanFold,
               feedMechCut,
               feedOther } TFeed;

struct TPrStl
{
    GUEST_STRUCT;
    GUEST<INTEGER> wDev;
    GUEST<INTEGER> iPageV;
    GUEST<INTEGER> iPageH;
    GUEST<SignedByte> bPort;
    GUEST<char> feed;
};

typedef enum { scanTB,
               scanBL,
               scanLR,
               scanRL } TScan;
struct TPrXInfo
{
    GUEST_STRUCT;
    GUEST<INTEGER> iRowBytes;
    GUEST<INTEGER> iBandV;
    GUEST<INTEGER> iBandH;
    GUEST<INTEGER> iDevBytes;
    GUEST<INTEGER> iBands;
    GUEST<SignedByte> bPatScale;
    GUEST<SignedByte> bULThick;
    GUEST<SignedByte> bULOffset;
    GUEST<SignedByte> bULShadow;
    GUEST<char> scan;
    GUEST<SignedByte> bXInfoX;
};

struct TPrJob
{
    GUEST_STRUCT;
    GUEST<INTEGER> iFstPage;
    GUEST<INTEGER> iLstPage;
    GUEST<INTEGER> iCopies;
    GUEST<SignedByte> bJDocLoop;
    GUEST<Boolean> fFromUsr;
    GUEST<ProcPtr> pIdleProc;
    GUEST<StringPtr> pFileName;
    GUEST<INTEGER> iFileVol;
    GUEST<SignedByte> bFileVers;
    GUEST<SignedByte> bJobX;
};

struct TPrint
{
    GUEST_STRUCT;
    GUEST<INTEGER> iPrVersion;
    GUEST<TPrInfo> prInfo;
    GUEST<Rect> rPaper;
    GUEST<TPrStl> prStl;
    GUEST<TPrInfo> prInfoPT;
    GUEST<TPrXInfo> prXInfo;
    GUEST<TPrJob> prJob;
    GUEST<INTEGER[19]> printX;
};
typedef TPrint *TPPrint;

typedef GUEST<TPPrint> *THPrint;

typedef Rect *TPRect;

struct TPrStatus
{
    GUEST_STRUCT;
    GUEST<INTEGER> iTotPages;
    GUEST<INTEGER> iCurPage;
    GUEST<INTEGER> iTotCopies;
    GUEST<INTEGER> iCurCopy;
    GUEST<INTEGER> iTotBands;
    GUEST<INTEGER> iCurBand;
    GUEST<Boolean> fPgDirty;
    GUEST<Boolean> fImaging;
    GUEST<THPrint> hPrint;
    GUEST<TPPrPort> pPRPort;
    GUEST<PicHandle> hPic;
};

/* From Technote 095 */
/* more stuff may be here */
typedef struct TPrDlg
{
    GUEST_STRUCT;
    GUEST<DialogRecord> dlg;
    GUEST<ModalFilterUPP> pFltrProc;
    GUEST<UserItemUPP> pItemProc;
    GUEST<THPrint> hPrintUsr;
    GUEST<Boolean> fDoIt;
    GUEST<Boolean> fDone;
    GUEST<LONGINT> lUser1;
    GUEST<LONGINT> lUser2;
    GUEST<LONGINT> lUser3;
    GUEST<LONGINT> lUser4;
    GUEST<INTEGER> iNumFst;
    GUEST<INTEGER> iNumLst;
} * TPPrDlg;

const LowMemGlobal<INTEGER> PrintErr { 0x944 }; // PrintMgr IMII-161 (true-b);

DISPATCHER_TRAP(PrGlue, 0xA8FD, StackL);

extern INTEGER C_PrError(void);
PASCAL_SUBTRAP(PrError, 0xA8FD, 0xBA000000, PrGlue);
extern void C_PrSetError(INTEGER iErr);
PASCAL_SUBTRAP(PrSetError, 0xA8FD, 0xC0000200, PrGlue);
extern void C_PrOpen(void);
PASCAL_SUBTRAP(PrOpen, 0xA8FD, 0xC8000000, PrGlue);
extern void C_PrClose(void);
PASCAL_SUBTRAP(PrClose, 0xA8FD, 0xD0000000, PrGlue);
extern void C_PrDrvrOpen(void);
PASCAL_SUBTRAP(PrDrvrOpen, 0xA8FD, 0x80000000, PrGlue);
extern void C_PrDrvrClose(void);
PASCAL_SUBTRAP(PrDrvrClose, 0xA8FD, 0x88000000, PrGlue);
extern void C_PrCtlCall(INTEGER iWhichCtl, LONGINT lParam1,
                                    LONGINT lParam2, LONGINT lParam3);
PASCAL_SUBTRAP(PrCtlCall, 0xA000, 0xA0000E00, PrGlue);
extern Handle C_PrDrvrDCE(void);
PASCAL_SUBTRAP(PrDrvrDCE, 0xA8FD, 0x94000000, PrGlue);
extern INTEGER C_PrDrvrVers(void);
PASCAL_SUBTRAP(PrDrvrVers, 0xA8FD, 0x9A000000, PrGlue);


extern TPPrDlg C_PrJobInit(THPrint hPrint);
PASCAL_SUBTRAP(PrJobInit, 0xA8FD, 0x44040410, PrGlue);
extern TPPrDlg C_PrStlInit(THPrint hPrint);
PASCAL_SUBTRAP(PrStlInit, 0xA8FD, 0x3C04040C, PrGlue);
extern Boolean C_PrDlgMain(THPrint hPrint, ProcPtr initfptr);
PASCAL_SUBTRAP(PrDlgMain, 0xA8FD, 0x4A040894, PrGlue);
extern void C_PrGeneral(Ptr pData);
PASCAL_SUBTRAP(PrGeneral, 0xA8FD, 0x70070480, PrGlue);
extern TPPrPort C_PrOpenDoc(THPrint hPrint, TPPrPort port,
                                        Ptr pIOBuf);
PASCAL_SUBTRAP(PrOpenDoc, 0xA8FD, 0x04000C00, PrGlue);
extern void C_PrOpenPage(TPPrPort port, TPRect pPageFrame);
PASCAL_SUBTRAP(PrOpenPage, 0xA8FD, 0x10000808, PrGlue);
extern void C_PrClosePage(TPPrPort pPrPort);
PASCAL_SUBTRAP(PrClosePage, 0xA8FD, 0x1800040C, PrGlue);
extern void C_PrCloseDoc(TPPrPort port);
PASCAL_SUBTRAP(PrCloseDoc, 0xA8FD, 0x08000484, PrGlue);
extern void C_PrPicFile(THPrint hPrint, TPPrPort pPrPort,
                                    Ptr pIOBuf, Ptr pDevBuf,
                                    TPrStatus *prStatus);
PASCAL_SUBTRAP(PrPicFile, 0xA8FD, 0x60051480, PrGlue);
extern void C_PrintDefault(THPrint hPrint);
PASCAL_SUBTRAP(PrintDefault, 0xA8FD, 0x20040480, PrGlue);
extern Boolean C_PrValidate(THPrint hPrint);
PASCAL_SUBTRAP(PrValidate, 0xA8FD, 0x52040498, PrGlue);
extern Boolean C_PrStlDialog(THPrint hPrint);
PASCAL_SUBTRAP(PrStlDialog, 0xA8FD, 0x2A040484, PrGlue);
extern Boolean C_PrJobDialog(THPrint hPrint);
PASCAL_SUBTRAP(PrJobDialog, 0xA8FD, 0x32040488, PrGlue);
extern void C_PrJobMerge(THPrint hPrintSrc, THPrint hPrintDst);
PASCAL_SUBTRAP(PrJobMerge, 0xA8FD, 0x5804089C, PrGlue);

static_assert(sizeof(TPrPort) == 178);
static_assert(sizeof(TPrInfo) == 14);
static_assert(sizeof(TPrStl) == 8);
static_assert(sizeof(TPrXInfo) == 16);
static_assert(sizeof(TPrJob) == 20);
static_assert(sizeof(TPrint) == 120);
static_assert(sizeof(TPrStatus) == 26);
static_assert(sizeof(TPrDlg) == 204);
}
#endif /* __PRINTING__ */
