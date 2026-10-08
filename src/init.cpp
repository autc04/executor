#include <rsys/executor.h>
#include <rsys/macros.h>
#include <error/error.h>
#include <time/time.h>
#include <base/cpu.h>
#include <PowerCore.h>

#include <OSEvent.h>
#include <VRetraceMgr.h>
#include <ScriptMgr.h>
#include <FileMgr.h>
#include <ResourceMgr.h>
#include <SysErr.h>
#include <DialogMgr.h>
#include <StdFilePkg.h>
#include <MemoryMgr.h>
#include <FontMgr.h>
#include <DeskMgr.h>
#include <MenuMgr.h>
#include <SoundMgr.h>
#include <AppleTalk.h>
#include <EventMgr.h>
#include <CQuickDraw.h>
#include <OSUtil.h>
#include <ScrapMgr.h>
#include <DeviceMgr.h>
#include <ControlMgr.h>
#include <sane/float.h>
#include <PrintMgr.h>
#include <SoundDvr.h>
#include <StartMgr.h>
#include <SegmentLdr.h>
#include <LowMem.h>
#include <Package.h>
#include <ADB.h>
#include <ToolboxEvent.h>
#include <AppleEvents.h>

#include <quickdraw/cquick.h> /* SET_HILITE_BIT */
#include <prefs/prefs.h> /* system_version */
#include <textedit/tesave.h> /* ROMlib_dotext */

#include <ctl/ctl.h>
#include <list/list.h>
#include <menu/menu.h>
#include <wind/wind.h>
#include <sound/soundopts.h>
#include <rsys/stdfile.h> /* for unixmount */
#include <base/traps.impl.h>

using namespace Executor;

static syn68k_addr_t
unhandled_trap(syn68k_addr_t callback_address, void *arg)
{
    static const char *trap_description[] = {
        /* I've only put the really interesting ones in. */
        nullptr, nullptr, nullptr, nullptr,
        "Illegal instruction",
        "Integer divide by zero",
        "CHK/CHK2",
        "FTRAPcc, TRAPcc, TRAPV",
        "Privilege violation",
        "Trace",
        "A-line",
        "FPU",
        nullptr,
        nullptr,
        "Format error"
    };
    int trap_num;
    char pc_str[128];

    trap_num = (intptr_t)arg;

    switch(trap_num)
    {
        case 4: /* Illegal instruction. */
        case 8: /* Privilege violation. */
        case 10: /* A-line. */
        case 11: /* F-line. */
        case 14: /* Format error. */
        case 15: /* Uninitialized interrupt. */
        case 24: /* Spurious interrupt. */
        case 25: /* Level 1 interrupt autovector. */
        case 26: /* Level 2 interrupt autovector. */
        case 27: /* Level 3 interrupt autovector. */
        case 28: /* Level 4 interrupt autovector. */
        case 29: /* Level 5 interrupt autovector. */
        case 30: /* Level 6 interrupt autovector. */
        case 31: /* Level 7 interrupt autovector. */
        case 32: /* TRAP #0 vector. */
        case 33: /* TRAP #1 vector. */
        case 34: /* TRAP #2 vector. */
        case 35: /* TRAP #3 vector. */
        case 36: /* TRAP #4 vector. */
        case 37: /* TRAP #5 vector. */
        case 38: /* TRAP #6 vector. */
        case 39: /* TRAP #7 vector. */
        case 40: /* TRAP #8 vector. */
        case 41: /* TRAP #9 vector. */
        case 42: /* TRAP #10 vector. */
        case 43: /* TRAP #11 vector. */
        case 44: /* TRAP #12 vector. */
        case 45: /* TRAP #13 vector. */
        case 46: /* TRAP #14 vector. */
        case 47: /* TRAP #15 vector. */
        case 3: /* Address error. */
        case 6: /* CHK/CHK2 instruction. */
        case 7: /* FTRAPcc, TRAPcc, TRAPV instructions. */
        case 9: /* Trace. */
        case 5: /* Integer divide by zero. */
            sprintf(pc_str, "0x%lX", (unsigned long)READUL(EM_A7 + 2));
            break;
        default:
            strcpy(pc_str, "<unknown>");
            break;
    }

    if(trap_num < (int)std::size(trap_description)
       && trap_description[trap_num] != nullptr)
        gui_fatal("Fatal error:  unhandled trap %d at pc %s (%s)",
                  trap_num, pc_str, trap_description[trap_num]);
    else
        gui_fatal("Fatal error:  unhandled trap %d at pc %s", trap_num, pc_str);

    /* It will never get here; this is just here to quiet gcc. */
    return callback_address;
}

static void
setup_trap_vectors(void)
{
#if 0
    syn68k_addr_t timer_callback;

    /* Set up the trap vector for the timer interrupt. */
    timer_callback = callback_install(catchalarm, nullptr);
    *(GUEST<syn68k_addr_t> *)SYN68K_TO_US(M68K_TIMER_VECTOR * 4) = timer_callback;

    getPowerCore().handleInterrupt = &catchalarmPowerPC;
#endif

    /* Fill in unhandled trap vectors so they cause graceful deaths.
   * Skip over those trap vectors which are known to have legitimate
   * values.
   */
    for(intptr_t i = 1; i < 64; i++)
        if(i != 10 /* A line trap.       */
#if defined(M68K_TIMER_VECTOR)
           && i != M68K_TIMER_VECTOR /* timer interrupt.   */
#endif

#if defined(M68K_WATCHDOG_VECTOR)
           && i != M68K_WATCHDOG_VECTOR /* watchdog timer interrupt. */
#endif

#if defined(M68K_EVENT_VECTOR)
           && i != M68K_EVENT_VECTOR
#endif

#if defined(M68K_MOUSE_MOVED_VECTOR)
           && i != M68K_MOUSE_MOVED_VECTOR
#endif

#if defined(M68K_SOUND_VECTOR)
           && i != M68K_SOUND_VECTOR
#endif

           && i != 0x58 / 4) /* Magic ARDI vector. */
        {
            syn68k_addr_t c;
            c = callback_install(unhandled_trap, (void *)i);
            *(GUEST<syn68k_addr_t> *)SYN68K_TO_US(i * 4) = c;
        }
}

PASCAL_FUNCTION_PTR(cdef0);
PASCAL_FUNCTION_PTR(cdef16);
PASCAL_FUNCTION_PTR(cdef1008);
PASCAL_FUNCTION_PTR(ldef0);
PASCAL_FUNCTION_PTR(mdef0);
PASCAL_FUNCTION_PTR(mbdf0);
PASCAL_FUNCTION_PTR(snth5);
PASCAL_FUNCTION_PTR(wdef0);
PASCAL_FUNCTION_PTR(wdef16);
PASCAL_FUNCTION_PTR(unixmount);

void Executor::InitLowMem()
{
    setup_trap_vectors();

    /*
     * Executor Custom Entrypoints
     * There is a pointer to a table of entrypoints hidden
     * behind an unused interrupt vector at address 0x58.
     * 
     * Don't change the order of the routines below.
     * We have made stubs of the form:
     *
     *	0x20780058	movel	0x58:w,		a0
     *	0x206800nn	movel	a0@(nn),	a0
     *	0x4ED0		jmp			a0@
     * 
     * nn is the offset into ROMlib_defs (0, 4, 8, ...)
     * 
     * Addionally, the Browser binary uses the unixmount entrypoint,
     * which is now a no-op, but has to remain here until Browser
     * has been successfully recompiled or replaced.
     */

    static GUEST<LONGINT> ROMlib_defs[] = {
        guest_cast<LONGINT>(&cdef0),
        guest_cast<LONGINT>(&cdef16),
        guest_cast<LONGINT>(&wdef0),
        guest_cast<LONGINT>(&wdef16),
        guest_cast<LONGINT>(&mdef0),
        guest_cast<LONGINT>(&ldef0),
        guest_cast<LONGINT>(&mbdf0),
        guest_cast<LONGINT>(&snth5),
        guest_cast<LONGINT>(&unixmount),
        guest_cast<LONGINT>(&cdef1008)
    };

    const LowMemGlobal<Ptr> magic_vector { 0x58 }; 
    LM(magic_vector) = (Ptr) &ROMlib_defs;


    {   // Mystery Hack: Replace the trap entry for ResourceStub by a piece
        // of code that jumps to the former trap entry of ResourceStub. 
        uint32_t l = ostraptable[0x0FC];
        static GUEST<uint16_t> jmpl_to_ResourceStub[3] = {
            (unsigned short)0x4EF9, 0, 0 /* Filled in below. */
        };
        ((unsigned char *)jmpl_to_ResourceStub)[2] = l >> 24;
        ((unsigned char *)jmpl_to_ResourceStub)[3] = l >> 16;
        ((unsigned char *)jmpl_to_ResourceStub)[4] = l >> 8;
        ((unsigned char *)jmpl_to_ResourceStub)[5] = l;
        ostraptable[0xFC] = US_TO_SYN68K(jmpl_to_ResourceStub);
    }   // End Mystery Hack

    LM(Ticks) = 0;
    LM(nilhandle) = 0; /* so nil dereferences "work" */

    memset(&LM(EventQueue), 0, sizeof(LM(EventQueue)));
    memset(&LM(VBLQueue), 0, sizeof(LM(VBLQueue)));
    //memset(&LM(DrvQHdr), 0, sizeof(LM(DrvQHdr)));     // inited in ROMlib_fileinit
    //memset(&LM(VCBQHdr), 0, sizeof(LM(VCBQHdr)));     // inited in ROMlib_fileinit
    //memset(&LM(FSQHdr), 0, sizeof(LM(FSQHdr)));       // inited in ROMlib_fileinit
    LM(BootDrive) = 0;
    //LM(DefVCBPtr) = 0;  // inited in ROMlib_fileinit
    LM(CurMap) = 0;
    LM(TopMapHndl) = 0;
    LM(SFSaveDisk) = 0;
    memset(LM(KeyMap), 0, sizeof(LM(KeyMap)));
    {
        static GUEST<uint16_t> ret = (unsigned short)0x4E75;

        LM(JCrsrTask) = (ProcPtr)&ret;
    }

    LM(TheGDevice) = LM(MainDevice) = LM(DeviceList) = nullptr;

    LM(TopMapHndl) = 0;
    LM(SysMapHndl) = 0;

    LM(ScrapState) = -1;

    LM(UTableBase) = (DCtlHandlePtr)NewPtrSysClear(4 * NDEVICES);
    LM(VIA) = NewPtrSysClear(16 * 512); /* IMIII-43 */
    *(char *)LM(VIA) = 0x80; /* Sound Off */

    const size_t SCC_SIZE = 1024;
    LM(SCCRd) = NewPtrSysClear(SCC_SIZE);
    LM(SCCWr) = NewPtrSysClear(SCC_SIZE);

    LM(SoundBase) = NewPtrSys(370 * sizeof(INTEGER));
    for(int i = 0; i < 370; ++i)
        ((GUEST<INTEGER> *)LM(SoundBase))[i] = 0x8000; /* reference 0 sound */

    InitPerProcessLowMem();

    LM(TheZone) = LM(ApplZone);
}


static void resetAllGlobalsBut(std::byte *base)
{
    /* Set low globals to 0xFF, but don't touch exception vectors. */
    memset((char *)&LM(nilhandle) + 64 * sizeof(ULONGINT),
           0xFF,
           ((char *)&LM(lastlowglobal) - (char *)&LM(nilhandle)
            - 64 * sizeof(ULONGINT)));
}

template<typename A, typename... Args>
static void resetAllGlobalsBut(std::byte *base, LowMemGlobal<A> var1, Args... vars)
{
    std::byte save[sizeof(A)];
    memcpy(save, base + var1.address, sizeof(A));
    resetAllGlobalsBut(base, vars...);
    memcpy(base + var1.address, save, sizeof(A));
}

void Executor::InitPerProcessLowMem()
{
    resetAllGlobalsBut(
        (std::byte*)&LM(nilhandle),
        SysZone,
        ApplZone,
        TheZone,
        Ticks,
        BootDrive,
        VIA,
        SCCRd,
        SCCWr,
        SoundBase,
        AppParmHandle,
        VCBQHdr,
        DrvQHdr,
        FSFCBLen,
        FCBSPtr,
        WDCBsPtr,
        SFSaveDisk,
        CurDirStore,
        EventQueue,
        VBLQueue,
        DefVCBPtr,
        CurApName,
        CurApRefNum,
        CurMap,
        TopMapHndl,
        SysMapHndl,
        SysMap,
        ScrapSize,
        ScrapHandle,
        ScrapCount,
        ScrapState,
        ScrapName,
        ROMFont0,
        WidthListHand,
        SPValid,
        SPATalkA,
        SPATalkB,
        SPConfig,
        SPPortA,
        SPPortB,
        SPAlarm,
        SPFont,
        SPKbd,
        SPPrint,
        SPVolCtl,
        SPClikCaret,
        SPMisc2,
        KeyThresh,
        KeyRepThresh,
        MenuFlash,
        CaretTime,
        DoubleTime,
        HiliteRGB,
        TheGDevice,
        MainDevice,
        DeviceList,
        DAStrings,
        MemTop,
        UTableBase,
        MouseLocation,
        DABeeper,
        FinderName,
        WMgrCPort,
        JCrsrTask,
        AE_info,
        KeyMap
        
        // if it ever becomes a LoMem global: DefDirID
     );
 
    LM(nilhandle) = 0; /* so nil dereferences "work" */
    *(GUEST<int16_t> *)SYN68K_TO_US(4) = 0x4e75; /* RTS, so when we dynamically recompile
				    code starting at 0 we won't get far */
    
    LM(WindowList) = nullptr;

    LM(CrsrBusy) = 0;
    LM(TESysJust) = 0;
    LM(DSAlertTab) = 0;
    LM(ResumeProc) = 0;
    LM(GZRootHnd) = 0;
    LM(ANumber) = 0;
    LM(ResErrProc) = 0;
    LM(FractEnable) = 0;
    LM(SEvtEnb) = 0;
    LM(MenuList) = 0;
    LM(MBarEnable) = 0;
    LM(TheMenu) = 0;
    LM(MBarHook) = 0;
    LM(MenuHook) = 0;
    LM(HeapEnd) = 0;
    LM(ApplLimit) = 0;
    LM(SoundActive) = soundactiveoff;
    LM(PortBUse) = 2; /* configured for Serial driver */

    LM(OneOne) = 0x00010001;
    LM(Lo3Bytes) = 0xFFFFFF;
    LM(DragHook) = 0;
    LM(MBDFHndl) = 0;
    LM(MBSaveLoc) = 0;
    LM(SysFontFam) = 0;

    LM(SysVersion) = system_version;


    LM(TEDoText) = (ProcPtr)&ROMlib_dotext; /* where should this go ? */

    LM(WWExist) = LM(QDExist) = EXIST_NO;
    LM(SCSIFlags) = 0xEC00; /* scsi+clock+xparam+mmu+adb
				 (no fpu,aux or pwrmgr) */

    LM(MMUType) = 5;
    LM(KbdType) = 2;

    LM(MCLKPCmiss1) = 0; /* &LM(MCLKPCmiss1) = 0x358 + 72 (MacLinkPC starts
			   adding the 72 byte offset to VCB pointers too
			   soon, starting with 0x358, which is not the
			   address of a VCB) */

    LM(MCLKPCmiss2) = 0; /* &LM(MCLKPCmiss1) = 0x358 + 78 (MacLinkPC misses) */
    LM(AuxCtlHead) = 0;
    LM(CurDeactive) = 0;
    LM(CurActivate) = 0;
    LM(macfpstate)[0] = 0;
    LM(fondid) = 0;
    LM(PrintErr) = 0;
    LM(mouseoffset) = 0;
    LM(heapcheck) = 0;
    LM(DefltStack) = 0x2000; /* nobody really cares about these two */
    LM(MinStack) = 0x400; /* values ... */
    LM(IAZNotify) = 0;
    LM(CurPitch) = 0;
    LM(JSwapFont) = (ProcPtr)&FMSwapFont;
    LM(JInitCrsr) = (ProcPtr)&InitCursor;

    LM(JHideCursor) = (ProcPtr)&HideCursor;
    LM(JShowCursor) = (ProcPtr)&ShowCursor;
    LM(JShieldCursor) = (ProcPtr)&ShieldCursor;
    LM(JSetCrsr) = (ProcPtr)&SetCursor;
    LM(JCrsrObscure) = (ProcPtr)&ObscureCursor;

    LM(JUnknown574) = (ProcPtr)&stub_SwapMMUMode;

    LM(Key1Trans) = (Ptr)&stub_Key1Trans;
    LM(Key2Trans) = (Ptr)&stub_Key2Trans;
    LM(JFLUSH) = &FlushCodeCache;
    LM(JResUnknown1) = LM(JFLUSH); /* I don't know what these are supposed to */
    LM(JResUnknown2) = LM(JFLUSH); /* do, but they're not called enough for
				   us to worry about the cache flushing
				   overhead */

    LM(CPUFlag) = 4; /* mc68040 */
    LM(UnitNtryCnt) = 0; /* how many units in the table */



    LM(HiliteMode) = 0xFF; /* I think this is correct */
    LM(ROM85) = 0x3FFF; /* We be color now */
#ifdef TWENTYFOUR_BIT_ADDRESSING
    LM(MMU32Bit) = 0x00;
#else
    LM(MMU32Bit) = 0x01;
#endif
    LM(loadtrap) = 0;
    *(GUEST<LONGINT> *)SYN68K_TO_US(0x1008) = 0x4; /* Quark XPress 3.0 references 0x1008
					explicitly.  It takes the value
					found there, subtracts four from
					it and dereferences that value.
					Yahoo */

    /* Micro-cap dereferences location one of the LM(AppPacks) locations */

    {
        int i;

        for(i = 0; i < (int)std::size(LM(AppPacks)); ++i)
            LM(AppPacks)[i] = 0;
    }
    LM(SysEvtMask) = ~(1L << keyUp); /* EVERYTHING except keyUp */
    LM(SdVolume) = 7; /* for Beebop 2 */
}
