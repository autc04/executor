/* Copyright 1994 - 1996 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <base/common.h>
#include <base/debugger.h>

#include <QuickDraw.h>
#include <CQuickDraw.h>
#include <ResourceMgr.h>
#include <SegmentLdr.h>
#include <MemoryMgr.h>
#include <StdFilePkg.h>
#include <VRetraceMgr.h>
#include <OSUtil.h>
#include <ToolboxUtil.h>
#include <FileMgr.h>
#include <SoundDvr.h>
#include <SysErr.h>
#include <DeskMgr.h>
#include <StartMgr.h>
#include <TimeMgr.h>
#include <ProcessMgr.h>
#include <AppleEvents.h>
#include <Gestalt.h>
#include <LowMem.h>

#include <base/trapglue.h>
#include <file/file.h>
#include <sound/sounddriver.h>
#include <prefs/prefs.h>
#include <commandline/flags.h>
#include <rsys/segment.h>
#include <res/resource.h>
#include <hfs/hfs.h>
#include <rsys/osutil.h>
#include <rsys/stdfile.h>
#include <vdriver/refresh.h>
#include <vdriver/responsiveness.h>

#include <prefs/options.h>
#include <quickdraw/cquick.h>
#include <rsys/desk.h>
#include <prefs/parse.h>
#include <rsys/executor.h>
#include <prefs/crc.h>
#include <sane/float.h>
#include <mman/mman.h>
#include <vdriver/vdriver.h>
#include <quickdraw/font.h>
#include <base/emustubs.h>

#include <rsys/adb.h>
#include <print/print.h>
#include <rsys/gestalt.h>
#include <osevent/osevent.h>
#include <time/time.h>
#include <appleevent/apple_events.h>
#include <rsys/cfm.h>
#include <rsys/launch.h>
#include <time/vbl.h>
#include <mpw/mpw.h>

#include <base/logging.h>

#include <rsys/paths.h>

#include <base/builtinlibs.h>
#include <base/cpu.h>
#include <PowerCore.h>

#include <util/macstrings.h>
#include <algorithm>

using namespace Executor;

static void beginexecutingat(LONGINT startpc)
{
    EM_D0 = 0;
    EM_D1 = 0xFFFC000;
    EM_D2 = 0;
    EM_D3 = 0x800008;
    EM_D4 = 0x408;
    EM_D5 = 0x10204000;
    EM_D6 = 0x7F0000;
    EM_D7 = 0x80000800;

    EM_A0 = 0x3EF796;
    EM_A1 = 0x910;
    EM_A2 = EM_D3;
    EM_A3 = 0;
    EM_A4 = 0;
    EM_A5 = guest_cast<LONGINT>(LM(CurrentA5)); /* was smashed when we
					   initialized above */
    EM_A6 = 0x1EF;

    if(base::Debugger::instance)
        base::Debugger::instance->initProcess(startpc);
    execute68K(startpc);
    C_ExitToShell();
}

size_info_t Executor::size_info;
LONGINT Executor::ROMlib_creator;


uint32_t Executor::ROMlib_version_long;


static void
cfm_launch(Handle cfrg0, OSType desired_arch, FSSpecPtr fsp)
{
    cfir_t *cfirp;

    cfirp = ROMlib_find_cfrg(cfrg0, desired_arch, kApplicationCFrag,
                             (StringPtr) "");

    if(cfirp)
    {
        GUEST<Ptr> mainAddr;
        Str255 errName;
        GUEST<ConnectionID> c_id;

        unsigned char empty[] = { 0 };

        if(CFIR_LOCATION(cfirp) == kOnDiskFlat)
        {
            // #warning were ignoring a lot of the cfir attributes
           OSErr err = GetDiskFragment(fsp, CFIR_OFFSET_TO_FRAGMENT(cfirp),
                            CFIR_FRAGMENT_LENGTH(cfirp), empty,
                            kLoadLib,
                            &c_id,
                            &mainAddr,
                            errName);
            fprintf(stderr, "GetDiskFragment -> err == %d\n", err);

        }
        else if(CFIR_LOCATION(cfirp) == kOnDiskSegmented)
        {
            ResType typ;
            INTEGER id;
            Handle h;

            typ = (ResType)CFIR_OFFSET_TO_FRAGMENT(cfirp);
            id = CFIR_FRAGMENT_LENGTH(cfirp);
            h = GetResource(typ, id);
            HLock(h);
            GetMemFragment(*h, GetHandleSize(h), empty, kLoadLib,
                           &c_id, &mainAddr, errName);

            fprintf(stderr, "Memory leak from segmented fragment\n");
        }
        {
            void *mainAddr1 = (void*) mainAddr;
            uint32_t new_toc = ((GUEST<uint32_t>*)mainAddr1)[1];
            uint32_t new_pc = ((GUEST<uint32_t>*)mainAddr1)[0];

            printf("ppc start: r2 = %08x, %08x\n", new_toc, new_pc);

            PowerCore& cpu = getPowerCore();
            cpu.r[2] = new_toc;
            cpu.r[1] = EM_A7-1024;

            cpu.syscall = &builtinlibs::handleSC;

#if SIZEOF_CHAR_P == 4
            cpu.memoryBases[0] = (void*)ROMlib_offset;
            cpu.memoryBases[1] = nullptr;
            cpu.memoryBases[2] = nullptr;
            cpu.memoryBases[3] = nullptr;
#elif SIZEOF_CHAR_P >= 8
            cpu.memoryBases[0] = (void*)ROMlib_offsets[0];
            cpu.memoryBases[1] = (void*)ROMlib_offsets[1];
            cpu.memoryBases[2] = (void*)ROMlib_offsets[2];
            cpu.memoryBases[3] = (void*)ROMlib_offsets[3];
#endif

            if(base::Debugger::instance)
                base::Debugger::instance->initProcess(new_pc);
            executePPC(new_pc);
        }
    }
    else
        fprintf(stderr, "Code fragment not found\n");
    C_ExitToShell();
}

launch_failure_t Executor::ROMlib_launch_failure = launch_no_failure;
INTEGER Executor::ROMlib_exevrefnum;

static void launchchain(ConstStringPtr fName, INTEGER vRefNum, Boolean resetmemory,
                        LaunchParamBlockRec *lpbp)
{
    OSErr err;
    FInfo finfo;
    Handle code0;
    Handle cfrg0;
    Handle h;
    vers_t *vp;
    WDPBRec wdpb;

    {
        INTEGER toskip;
        const Byte *p;
        for(p = fName + fName[0] + 1; p > fName && *--p != ':';)
            ;
        toskip = p - fName;
        LM(CurApName)[0] = std::min(fName[0] - toskip, 31);
        BlockMoveData((Ptr)fName + 1 + toskip, (Ptr)LM(CurApName) + 1,
                    (Size)LM(CurApName)[0]);
    }
    std::string appNameUTF8 = toUnicodeFilename(LM(CurApName)).string();
#if 0
    Munger(LM(AppParmHandle), 2L*sizeof(INTEGER), (Ptr) 0,
				  (LONGINT) sizeof(AppFile), (Ptr) "", 0L);
#endif
    if(!lpbp || lpbp->launchBlockID != extendedBlock)
    {
        CInfoPBRec hpb;

        hpb.hFileInfo.ioNamePtr = (StringPtr) &fName[0];
        hpb.hFileInfo.ioVRefNum = vRefNum;
        hpb.hFileInfo.ioFDirIndex = 0;
        hpb.hFileInfo.ioDirID = 0;
        PBGetCatInfo(&hpb, false);
        wdpb.ioVRefNum = vRefNum;
        wdpb.ioWDDirID = hpb.hFileInfo.ioFlParID;
    }
    else
    {
        FSSpecPtr fsp;

        fsp = lpbp->launchAppSpec;
        wdpb.ioVRefNum = fsp->vRefNum;
        wdpb.ioWDDirID = fsp->parID;
    }
    /* Do not do this -- Loser does it SFSaveDisk_Update (vRefNum, fName); */
    wdpb.ioWDProcID = "Xctr"_4;
    wdpb.ioNamePtr = 0;
    PBOpenWD(&wdpb, false);
    ROMlib_exevrefnum = wdpb.ioVRefNum;
    ROMlib_exefname = LM(CurApName);
#if 0
/* I'm skeptical that this is correct */
    if (LM(CurMap) != LM(SysMap))
	CloseResFile(LM(CurMap));
#endif
    SetVol((StringPtr)0, ROMlib_exevrefnum);
    LM(CurApRefNum) = OpenResFile(ROMlib_exefname);

    err = GetFInfo(ROMlib_exefname, ROMlib_exevrefnum, &finfo);

    process_create(false, finfo.fdType, finfo.fdCreator);

    ROMlib_creator = finfo.fdCreator;

#define LEMMINGSHACK
#if defined(LEMMINGSHACK)
    {
        if(finfo.fdCreator == "Psyg"_4
           || finfo.fdCreator == "Psod"_4)
            ROMlib_flushoften = true;
    }
#endif /* defined(LEMMINGSHACK) */

#if defined(ULTIMA_III_HACK)
    ROMlib_ultima_iii_hack = (finfo.fdCreator == "Ult3"_4);
#endif

    h = GetResource("vers"_4, 2);
    if(!h)
        h = GetResource("vers"_4, 1);

    ROMlib_version_long = 0;
    if(h)
    {
        vp = (vers_t *)*h;
        ROMlib_version_long = ((vp->c[0] << 24) | (vp->c[1] << 16) | (vp->c[2] << 8) | (vp->c[3] << 0));
    }
    

    ROMlib_directdiskaccess = false;
    ROMlib_clear_gestalt_list();
    ParseConfigFile("ExecutorDefault", 0);
    ParseConfigFile(appNameUTF8, err == noErr ? finfo.fdCreator.raw() : 0);
    ROMlib_clockonoff(!ROMlib_noclock);
    code0 = Get1Resource("CODE"_4, 0);
    cfrg0 = Get1Resource("cfrg"_4, 0);

    bool havePowerPCCode = cfrg0
            && ROMlib_find_cfrg(cfrg0, "pwpc"_4,
                                kApplicationCFrag, (StringPtr) "");

    if(havePowerPCCode && ROMlib_prefer_ppc)
        code0 = nullptr;
    
    if(!havePowerPCCode && !code0)
    {
        if(cfrg0 && ROMlib_find_cfrg(cfrg0, "m68k"_4,
                                     kApplicationCFrag, (StringPtr) ""))
            ROMlib_launch_failure = launch_cfm_requiring;
        else
            ROMlib_launch_failure = launch_damaged;
        C_ExitToShell();
    }

    {
        Handle size_resource_h = Get1Resource("SIZE"_4, 0);
        if(size_resource_h == nullptr)
            size_resource_h = Get1Resource("SIZE"_4, -1);
        if(size_resource_h)
        {
            SIZEResource *size_resource;

            size_resource = (SIZEResource *)*size_resource_h;
            size_info.size_flags = size_resource->size_flags;
            size_info.preferred_size = size_resource->preferred_size;
            size_info.minimum_size = size_resource->minimum_size;
            size_info.size_resource_present_p = true;
        }
        else
        {
            memset(&size_info, '\000', sizeof size_info);
            size_info.size_resource_present_p = false;
        }
        size_info.application_p = true;

        /* we don't accept open app events until a handler is installed */
        application_accepts_open_app_aevt_p = false;
        send_application_open_aevt_p
            = system_version >= 0x700
            && ((size_info.size_flags & SZisHighLevelEventAware)
                == SZisHighLevelEventAware);
    }

    LM(BufPtr) = LM(MemTop);

    if(!code0)
    {
        LM(CurrentA5) = LM(BufPtr) - 4;
        LM(CurStackBase) = LM(CurrentA5);       
    }
    else
    {
        HLock(code0);

        auto lp = (GUEST<LONGINT> *)*code0;
        LONGINT abovea5 = *lp++;
        LONGINT belowa5 = *lp++;
        LONGINT jumplen = *lp++;
        LONGINT jumpoff = *lp++;

        /*
         * NOTE: The stack initialization code that was here has been moved
         *	     to ROMlib_InitZones in mman.c
         */
        /* #warning Stack is getting reinitialized even when Chain is called ... */

        LM(CurrentA5) = LM(BufPtr) - abovea5;
        LM(CurStackBase) = LM(CurrentA5) - belowa5;

        LM(CurJTOffset) = jumpoff;
        memcpy(LM(CurrentA5) + jumpoff, lp, jumplen); /* copy in the
							 jump table */
    }
    EM_A7 = ptr_to_longint(LM(CurStackBase)) - 4096;
    EM_A5 = ptr_to_longint(LM(CurrentA5));

    ROMlib_destroy_blocks(0, ~0, false);

    GetDateTime(&LM(Time));
    LM(ROMBase) = (Ptr)ROMlib_phoneyrom;
    LM(dodusesit) = LM(ROMBase);
    LM(QDExist) = LM(WWExist) = EXIST_NO;
    LM(TheZone) = LM(ApplZone);

    // disallow moving relocatable blocks until this flag is reset
    // in OSEventCommon(). Allegedly (according to some old comment found
    // in that function), this is needed for some unspecified version of Excel.
    ROMlib_memnomove_p = true;

    SetCursor(*GetCursor(watchCursor));

    /* Call this routine in case the refresh value changed, either just
    * now or when the config file was parsed.  We want to do this
    * before the screen's depth might be changed.
    */
    {
        int save_ROMlib_refresh = ROMlib_refresh;
        dequeue_refresh_task();
        set_refresh_rate(0);
        set_refresh_rate(save_ROMlib_refresh);
    }

    if(finfo.fdType == "MPST"_4)
        mpw::SetupTool();

    if(code0)
        beginexecutingat(guest_cast<LONGINT>(LM(CurrentA5)) + LM(CurJTOffset) + 2);
    else
    {
        FSSpec fs;

        FSMakeFSSpec(ROMlib_exevrefnum, 0, ROMlib_exefname, &fs);
        cfm_launch(cfrg0, "pwpc"_4, &fs);
    }
}

void Executor::Chain(ConstStringPtr fName, INTEGER vRefNum)
{
    launchchain(fName, vRefNum, false, 0);
}


static void reset_traps(void)
{
    static syn68k_addr_t savetooltraptable[0x400];
    static syn68k_addr_t saveostraptable[0x100];
    static Boolean beenhere = false;

    ROMlib_reset_bad_trap_addresses();
    if(!beenhere)
    {
        memcpy(saveostraptable, ostraptable, sizeof(saveostraptable));
        memcpy(savetooltraptable, tooltraptable, sizeof(savetooltraptable));
        beenhere = true;
    }
    else
    {
        /*
 * NOTE: I'm not preserving patches that go into the SystemZone.  Right now
 *       that just seems like an unnecessary source of mystery bugs.
 */
        memcpy(ostraptable, saveostraptable, sizeof(saveostraptable));
        memcpy(tooltraptable, savetooltraptable, sizeof(savetooltraptable));
    }
}

void Executor::empty_timer_queues(void)
{
    TMTask *tp, *nexttp;
    VBLTaskPtr vp, nextvp;

    dequeue_refresh_task();
    clear_pending_sounds();
    for(vp = (VBLTaskPtr)LM(VBLQueue).qHead; vp; vp = nextvp)
    {
        nextvp = (VBLTaskPtr)vp->qLink;
        VRemove(vp);
    }
    for(tp = (TMTask *)ROMlib_timehead.qHead; tp; tp = nexttp)
    {
        nexttp = (TMTask *)tp->qLink;
        RmvTime((QElemPtr)tp);
    }
}

static void reinitialize_things(void)
{
    resmaphand map, nextmap;
    filecontrolblock *fcbp, *efcbp;
    INTEGER length;
    INTEGER special_fn;
    int i;

    ROMlib_shutdown_font_manager();
    SetZone(LM(SysZone));
    /* NOTE: we really shouldn't be closing desk accessories at all, but
       since we don't properly handle them when they're left open, it is
       better to close them down than not.  */

    for(i = DESK_ACC_MIN; i <= DESK_ACC_MAX; ++i)
        CloseDriver(-i - 1);

    empty_timer_queues();
    ROMlib_clock = 0; /* CLOCKOFF */

    special_fn = 0;
    for(map = (resmaphand)LM(TopMapHndl); map; map = nextmap)
    {
        nextmap = (resmaphand)(*map)->nextmap;
        if((*map)->resfn == LM(SysMap))
            UpdateResFile((*map)->resfn);
        else
            CloseResFile((*map)->resfn);
    }

    length = *(GUEST<int16_t> *)LM(FCBSPtr);
    fcbp = (filecontrolblock *)((short *)LM(FCBSPtr) + 1);
    efcbp = (filecontrolblock *)((char *)LM(FCBSPtr) + length);
    for(; fcbp < efcbp;
        fcbp = (filecontrolblock *)((char *)fcbp + LM(FSFCBLen)))
    {
        INTEGER rn;

        rn = (char *)fcbp - (char *)LM(FCBSPtr);
        if(fcbp->fcbCName[0]
           /* && rn != Param_ram_rn */
           && rn != LM(SysMap)
           && rn != special_fn)
            FSClose((char *)fcbp - (char *)LM(FCBSPtr));
    }

    LM(CurMap) = (*(resmaphand)LM(TopMapHndl))->resfn;

    ROMlib_destroy_blocks(0, ~0, false);
}

OSErr
Executor::NewLaunch(ConstStringPtr fName_arg, INTEGER vRefNum_arg, LaunchParamBlockRec *lpbp)
{
    OSErr retval;
    static char beenhere = false;
    static jmp_buf buf;
    static Str255 fName;
    static INTEGER vRefNum;
    static LaunchParamBlockRec lpb;
    Boolean extended_p;

    retval = noErr;
    if(lpbp && lpbp->launchBlockID == extendedBlock)
    {
        lpb = *lpbp;
        str255assign(fName, (lpbp->launchAppSpec)->name);
        extended_p = true;
    }
    else
    {
        lpb.launchBlockID = 0;
        str255assign(fName, fName_arg);
        vRefNum = vRefNum_arg;
        extended_p = false;
    }

#if 0
    if(extended_p && (lpbp->launchControlFlags & launchContinue))
    {
        int n_filenames;
        char **filenames;
        int i;
        AppParametersPtr ap;
        int n_filename_bytes;

#if !defined(LETGCCWAIL)
        ap = nullptr;
#endif
        n_filenames = 1;
        if(lpbp->launchAppParameters)
        {
            ap = lpbp->launchAppParameters;
            n_filenames += ap->n_fsspec;
        }
        n_filename_bytes = n_filenames * sizeof *filenames;
        filenames = (char **)alloca(n_filename_bytes);
        memset(filenames, 0, n_filename_bytes);
        retval = ROMlib_filename_from_fsspec(&filenames[0],
                                             lpbp->launchAppSpec);
        for(i = 1; retval == noErr && i < n_filenames; ++i)
            retval = ROMlib_filename_from_fsspec(&filenames[1],
                                                 &ap->fsspec[i - 1]);
        if(retval == noErr)
            retval = ROMlib_launch_native_app(n_filenames, filenames);
        for(i = 0; i < n_filenames; ++i)
            free(filenames[i]);
    }
    else
#endif
    {
        /* This setjmp/longjmp code might be better put in launchchain */
        if(!beenhere)
        {
            beenhere = true;
            setjmp(buf);
        }
        else
            longjmp(buf, 1);
        logging::resetNestingLevel();

        retval = noErr;
        reset_adb_vector();
        reinitialize_things();
        reset_traps();
        InitPerProcessLowMem();
        /* Set up default floating point environment. */
        {
            INTEGER env = 0;
            ROMlib_Fsetenv(inout(env), 0);
        }
        ROMlib_InitZones();
        ROMlib_InitGWorlds();
        hle_reinit();
        AE_reinit();
        print_reinit();
        /* A freshly launched application has not had a chance to pump events
         * yet; don't flag it as stuck before it has run. */
        Responsiveness::instance().reset();

        ROMlib_init_stdfile();

        launchchain(fName, vRefNum, true, &lpb);
    }
    return retval;
}

void Executor::Launch(ConstStringPtr fName_arg, INTEGER vRefNum_arg)
{
    NewLaunch(fName_arg, vRefNum_arg, 0);
}

OSErr Executor::LaunchApplication(LaunchParamBlockRec *lpbp)
{
    StringPtr strp;

    if(lpbp->launchBlockID == extendedBlock)
        strp = 0;
    else
        strp = *(GUEST<StringPtr> *)lpbp;
    return NewLaunch(strp, 0, lpbp);
}
