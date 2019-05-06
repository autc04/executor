#if !defined(__OSUTIL__)
#define __OSUTIL__

/*
 * Copyright 1986, 1989, 1990 by Abacus Research and Development, Inc.
 * All rights reserved.
 *

 */

#include <VRetraceMgr.h>
#include <FileMgr.h>
#include <OSEvent.h>

#define MODULE_NAME OSUtil
#include <base/api-module.h>

namespace Executor
{
enum
{
    macXLMachine = 0,
    macMachine = 1,
    UNIXMachine = 1127,
};

enum
{
    clkRdErr = (-85),
    clkWrErr = (-86),
    prInitErr = (-88),
    prWrErr = (-87),
    hwParamErr = (-502),
};

struct SysParmType
{
    GUEST_STRUCT;
    GUEST<Byte> valid;
    GUEST<Byte> aTalkA;
    GUEST<Byte> aTalkB;
    GUEST<Byte> config;
    GUEST<INTEGER> portA;
    GUEST<INTEGER> portB;
    GUEST<LONGINT> alarm;
    GUEST<INTEGER> font;
    GUEST<INTEGER> kbdPrint;
    GUEST<INTEGER> volClik;
    GUEST<INTEGER> misc;
};
typedef SysParmType *SysPPtr;

typedef enum { dummyType,
               vType,
               ioQType,
               drvQType,
               evType,
               fsQType } QTypes;

union __qe {
    VBLTask vblQElem;
    ParamBlockRec ioQElem;
    DrvQEl drvQElem;
    EvQEl evQElem;
    VCB vcbQElem;
};
typedef union __qe QElem;

struct DateTimeRec
{
    GUEST_STRUCT;
    GUEST<INTEGER> year;
    GUEST<INTEGER> month;
    GUEST<INTEGER> day;
    GUEST<INTEGER> hour;
    GUEST<INTEGER> minute;
    GUEST<INTEGER> second;
    GUEST<INTEGER> dayOfWeek;
};

typedef struct SysEnvRec
{
    GUEST_STRUCT;
    GUEST<INTEGER> environsVersion;
    GUEST<INTEGER> machineType;
    GUEST<INTEGER> systemVersion;
    GUEST<INTEGER> processor;
    GUEST<BOOLEAN> hasFPU;
    GUEST<BOOLEAN> hasColorQD;
    GUEST<INTEGER> keyBoardType;
    GUEST<INTEGER> atDrvrVersNum;
    GUEST<INTEGER> sysVRefNum;
} * SysEnvRecPtr;

enum
{
    SYSRECVNUM = 2,
};

/* sysEnv machine types */
enum
{
    envMachUnknown = 0,
    env512KE = 1,
    envMacPlus = 2,
    envSE = 3,
    envMacII = 4,
    envMac = -1,
    envXL = -2,
};

enum
{
    envCPUUnknown = 0,
    env68000 = 1,
    env68020 = 3,
    env68030 = 4,
    env68040 = 5,
};

enum
{
    envUnknownKbd = 0,
    envMacKbd = 1,
    envMacAndPad = 2,
    envMacPlusKbd = 3,
    envAExtendKbd = 4,
    envStandADBKbd = 5,
};

enum
{
    envBadVers = (-5501),
    envVersTooBig = (-5502),
};

typedef SignedByte TrapType;

enum
{
    kOSTrapType,
    kToolboxTrapType,
};


const LowMemGlobal<INTEGER> SysVersion { 0x15A }; // OSUtil ThinkC (true);
const LowMemGlobal<Byte> SPValid { 0x1F8 }; // OSUtil IMII-392 (true);
const LowMemGlobal<Byte> SPATalkA { 0x1F9 }; // OSUtil IMII-392 (true);
const LowMemGlobal<Byte> SPATalkB { 0x1FA }; // OSUtil IMII-392 (true);
const LowMemGlobal<Byte> SPConfig { 0x1FB }; // OSUtil IMII-392 (true);
const LowMemGlobal<INTEGER> SPPortA { 0x1FC }; // OSUtil IMII-392 (true);
const LowMemGlobal<INTEGER> SPPortB { 0x1FE }; // OSUtil IMII-392 (true);
const LowMemGlobal<LONGINT> SPAlarm { 0x200 }; // OSUtil IMII-392 (true);
const LowMemGlobal<INTEGER> SPFont { 0x204 }; // OSUtil IMII-392 (true);
const LowMemGlobal<Byte> SPKbd { 0x206 }; // OSUtil IMII-369 (true);
const LowMemGlobal<Byte> SPPrint { 0x207 }; // OSUtil IMII-392 (true);
const LowMemGlobal<Byte> SPVolCtl { 0x208 }; // OSUtil IMII-392 (true);
const LowMemGlobal<Byte> SPClikCaret { 0x209 }; // OSUtil IMII-392 (true);
const LowMemGlobal<Byte> SPMisc2 { 0x20B }; // OSUtil IMII-392 (true);
const LowMemGlobal<ULONGINT> Time { 0x20C }; // OSUtil IMI-260 (true);
const LowMemGlobal<INTEGER> CrsrThresh { 0x8EC }; // OSUtil IMII-372 (false);
const LowMemGlobal<Byte> MMUType { 0xCB1 }; // OSUtil MPW (false);
const LowMemGlobal<Byte> MMU32Bit { 0xCB2 }; // OSUtil IMV-592 (true-b);
const LowMemGlobal<QHdr> DTQueue { 0xD92 }; // OSUtil IMV-466 (false);
const LowMemGlobal<ProcPtr> JDTInstall { 0xD9C }; // OSUtil IMV (false);

extern OSErr HandToHand(GUEST<Handle> *h);
REGISTER_TRAP2(HandToHand, 0xA9E1, D0(InOut<Handle,A0>), SaveA1D1D2, CCFromD0);
extern OSErr PtrToHand(Ptr p, GUEST<Handle> *h, LONGINT s);
REGISTER_TRAP2(PtrToHand, 0xA9E3, D0(A0, Out<Handle,A0>, D0), SaveA1D1D2, CCFromD0);
extern OSErr PtrToXHand(Ptr p, Handle h, LONGINT s);
REGISTER_TRAP2(PtrToXHand, 0xA9E2, D0(A0,A1,D0), MoveA1ToA0, SaveA1D1D2, CCFromD0);
extern OSErr HandAndHand(Handle h1, Handle h2);
REGISTER_TRAP2(HandAndHand, 0xA9E4, D0(A0,A1), MoveA1ToA0, SaveA1D1D2, CCFromD0);
extern OSErr PtrAndHand(Ptr p, Handle h, LONGINT s1);
REGISTER_TRAP2(PtrAndHand, 0xA9EF, D0(A0,A1,D0), MoveA1ToA0, SaveA1D1D2, CCFromD0);

extern LONGINT ROMlib_RelString(const unsigned char *s1, const unsigned char *s2,
                                BOOLEAN casesig, BOOLEAN diacsig, LONGINT d0);
extern INTEGER RelString(StringPtr s1, StringPtr s2,
                                 BOOLEAN casesig, BOOLEAN diacsig);
extern BOOLEAN EqualString(StringPtr s1, StringPtr s2,
                                   BOOLEAN casesig, BOOLEAN diacsig);
extern void ROMlib_UprString(StringPtr s, BOOLEAN diac, INTEGER len);
extern void UpperString(StringPtr s, BOOLEAN diac);
extern void GetDateTime(GUEST<ULONGINT> *mactimepointer);
NOTRAP_FUNCTION2(GetDateTime);

extern OSErr ReadDateTime(GUEST<ULONGINT> *secs);
REGISTER_TRAP2(ReadDateTime, 0xA039, D0(A0));
extern OSErr SetDateTime(ULONGINT mactime);
REGISTER_TRAP2(SetDateTime, 0xA03A, D0(D0));
extern void DateToSeconds(DateTimeRec *d, GUEST<ULONGINT> *s);
REGISTER_TRAP2(DateToSeconds, 0xA9C7, void(A0, Out<ULONGINT,D0>), SaveA1D1D2);
extern void SecondsToDate(ULONGINT mactime, DateTimeRec *d);
REGISTER_TRAP2(SecondsToDate, 0xA9C6, void(D0,A0), SaveA1D1D2);

extern void GetTime(DateTimeRec *d);
extern void SetTime(DateTimeRec *d);
extern OSErr InitUtil(void);
REGISTER_TRAP2(InitUtil, 0xA03F, D0());

extern SysPPtr GetSysPPtr(void);

extern OSErr WriteParam(void);
REGISTER_TRAP2(WriteParam, 0xA038, D0());
extern void Enqueue(QElemPtr e, QHdrPtr h);
REGISTER_TRAP2(Enqueue, 0xA96F, void(A0,A1), SaveA1D1D2);
extern OSErr Dequeue(QElemPtr e, QHdrPtr h);
REGISTER_TRAP2(Dequeue, 0xA96E, D0(A0,A1), SaveA1D1D2);

//extern LONGINT GetTrapAddress(INTEGER n); // 68K in emustubs, not supported on ppc
//extern void SetTrapAddress(LONGINT addr,
//                           INTEGER n);

extern LONGINT C_NGetTrapAddress(INTEGER n, TrapType ttype);
NOTRAP_FUNCTION(NGetTrapAddress);
extern void C_NSetTrapAddress(LONGINT addr, INTEGER n, TrapType ttype);
NOTRAP_FUNCTION(NSetTrapAddress);

// trap implementation in emustubs for historical reasons (TODO: clean this up)
extern LONGINT C_GetOSTrapAddress(INTEGER n);
NOTRAP_FUNCTION(GetOSTrapAddress);
extern void C_SetOSTrapAddress(LONGINT addr, INTEGER n);
NOTRAP_FUNCTION(SetOSTrapAddress);
extern LONGINT C_GetToolTrapAddress(INTEGER n);
NOTRAP_FUNCTION(GetToolTrapAddress);
extern void C_SetToolTrapAddress(LONGINT addr, INTEGER n);
NOTRAP_FUNCTION(SetToolTrapAddress);
inline LONGINT C_GetToolboxTrapAddress(INTEGER n)
    { return C_GetToolTrapAddress(n); }
NOTRAP_FUNCTION(GetToolboxTrapAddress);
inline void C_SetToolboxTrapAddress(LONGINT addr, INTEGER n)
    { C_SetToolTrapAddress(addr,n); }
NOTRAP_FUNCTION(SetToolboxTrapAddress);


extern void Delay(LONGINT n, GUEST<LONGINT> *ftp);
REGISTER_TRAP2(Delay, 0xA03B, void(A0,Out<LONGINT,D0>));

extern void C_SysBeep(INTEGER i);
PASCAL_TRAP(SysBeep, 0xA9C8);

extern void Environs(GUEST<INTEGER> *rom, GUEST<INTEGER> *machine);
extern OSErr SysEnvirons(INTEGER vers, SysEnvRecPtr p);
REGISTER_TRAP2(SysEnvirons, 0xA090, D0(D0,A0));

extern void Restart(void);
extern void SetUpA5(void);
extern void RestoreA5(void);
#undef GetMMUMode
#undef SwapMMUMode
extern Byte GetMMUMode();
extern void SwapMMUMode(Byte *bp);

extern uint32_t StripAddress(uint32_t l);
REGISTER_TRAP2(StripAddress, 0xA055, D0(D0));

extern void C_DebugStr(StringPtr p);
PASCAL_TRAP(DebugStr, 0xABFF);

extern void C_Debugger();
PASCAL_TRAP(Debugger, 0xA9FF);

extern void
C_MakeDataExecutable(void *ptr, uint32_t sz);
NOTRAP_FUNCTION(MakeDataExecutable);
extern void C_FlushCodeCache(void);
PASCAL_TRAP(FlushCodeCache, 0xA0BD);

extern void HWPriv(LONGINT d0, LONGINT a0);
REGISTER_TRAP2(HWPriv, 0xA198, void(D0,A0));

extern LONGINT SetCurrentA5();
NOTRAP_FUNCTION2(SetCurrentA5);
extern LONGINT SetA5(LONGINT newA5);
NOTRAP_FUNCTION2(SetA5);

static_assert(sizeof(SysParmType) == 20);
static_assert(sizeof(DateTimeRec) == 14);
static_assert(sizeof(SysEnvRec) == 16);
static_assert(sizeof(QElem) == 178);
}
#endif /* __OSUTIL__ */
