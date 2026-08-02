/* Copyright 1989 - 1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in DeviceMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <DeviceMgr.h>
#include <FileMgr.h>
#include <MemoryMgr.h>
#include <OSUtil.h>
#include <ResourceMgr.h>
#include <MenuMgr.h>
#include <ToolboxEvent.h>
#include <mman/mman.h>
#include <rsys/device.h>
#include <file/file.h>
#include <rsys/serial.h>
#include <base/cpu.h>
#include <base/functions.impl.h>

using namespace Executor;

/*
 * NOTE:  The device manager now executes "native code" and code read
 *	  in from resources.  The latter is "RAM" based, the former is
 *	  our version of "ROM" based.  We use the same bit that the Mac
 *	  uses to distinguish between the two, but our implementation of
 *	  ROM based is incompatible with theirs.  NOTE: even in our
 *	  incompatible ROM based routines, we still byte swap the pointers
 *	  that get us to the routines.
 */

OSErr Executor::ROMlib_dispatch(ParmBlkPtr p, Boolean async,
                                DriverRoutineType routine,
                                INTEGER trapn) /* INTERNAL */
{
    int devicen;
    ramdriverhand ramdh;
    DCtlHandle h;
    OSErr retval;
    DriverUPP procp;

    devicen = -p->cntrlParam.ioCRefNum - 1;
    if(devicen < 0 || devicen >= NDEVICES)
        retval = badUnitErr;
    else if(LM(UTableBase) == guest_cast<DCtlHandlePtr>(-1) || (h = LM(UTableBase)[devicen]) == nullptr || *h == nullptr)
        retval = unitEmptyErr;
    else
    {
        HLock((Handle)h);
        p->ioParam.ioTrap = trapn;
        if(async)
            p->ioParam.ioTrap |= asyncTrpBit;
        else
            p->ioParam.ioTrap |= noQueueBit;
        if(!((*h)->dCtlFlags & RAMBASEDBIT))
        {
            switch(routine)
            {
                case Open:
                    retval = ((*h)->dCtlDriver->udrvrOpen)(p, *h);
                    break;
                case Prime:
                    retval = ((*h)->dCtlDriver->udrvrPrime)(p, *h);
                    break;
                case Ctl:
                    retval = ((*h)->dCtlDriver->udrvrCtl)(p, *h);
                    break;
                case Stat:
                    retval = ((*h)->dCtlDriver->udrvrStatus)(p, *h);
                    break;
                case Close:
                    retval = ((*h)->dCtlDriver->udrvrClose)(p, *h);
                    break;
                default:
                    retval = fsDSIntErr;
                    break;
            }
        }
        else
        {
            ramdh = (ramdriverhand)(*h)->dCtlDriver;
            LoadResource((Handle)ramdh);
            HLock((Handle)ramdh);
            switch(routine)
            {
                case Open:
                    procp = (DriverUPP)(*ramdh + (*ramdh)->drvrOpen);
                    break;
                case Prime:
                    procp = (DriverUPP)(*ramdh + (*ramdh)->drvrPrime);
                    break;
                case Ctl:
                    procp = (DriverUPP)(*ramdh + (*ramdh)->drvrCtl);
                    break;
                case Stat:
                    procp = (DriverUPP)(*ramdh + (*ramdh)->drvrStatus);
                    break;
                case Close:
                    procp = (DriverUPP)(*ramdh + (*ramdh)->drvrClose);
                    break;
                default:
                    procp = 0;
                    break;
            }
            if(procp)
            {
                LONGINT saved1, saved2, saved3, saved4, saved5, saved6, saved7,
                    savea2, savea3, savea4, savea5, savea6;

                savea2 = EM_A2;
                EM_A0 = US_TO_SYN68K(p);
                EM_A1 = US_TO_SYN68K(*h);
                EM_A2 = US_TO_SYN68K((ProcPtr)procp); /* for compatibility with above */
                saved1 = EM_D1;
                saved2 = EM_D2;
                saved3 = EM_D3;
                saved4 = EM_D4;
                saved5 = EM_D5;
                saved6 = EM_D6;
                saved7 = EM_D7;
                savea3 = EM_A3;
                savea4 = EM_A4;
                savea5 = EM_A5;
                savea6 = EM_A6;
                execute68K(US_TO_SYN68K((ProcPtr)procp));
                EM_D1 = saved1;
                EM_D2 = saved2;
                EM_D3 = saved3;
                EM_D4 = saved4;
                EM_D5 = saved5;
                EM_D6 = saved6;
                EM_D7 = saved7;
                EM_A3 = savea3;
                EM_A4 = savea4;
                EM_A5 = savea5;
                EM_A6 = savea6;
                retval = EM_D0;
                EM_A2 = savea2;
            }
            else
                retval = fsDSIntErr;
            if(routine == Open)
                (*h)->dCtlFlags |= DRIVEROPENBIT;
            else if(routine == Close)
            {
                (*h)->dCtlFlags &= ~DRIVEROPENBIT;
                HUnlock((Handle)h);
                HUnlock((Handle)ramdh);
                LM(MBarEnable) = 0;
                /* NOTE: It's not clear whether we should zero out this
		   field or just check for DRIVEROPEN bit up above and never
		   send messages except open to non-open drivers.  */
                LM(UTableBase)[devicen] = nullptr;
            }

            if(routine < Close)
                retval = p->ioParam.ioResult; /* see II-193 */
        }
    }
    fs_err_hook(retval);
    return retval;
}

/* PBOpen, PBClose, PBRead and PBWrite are part of the file manager */

OSErr Executor::PBControl(ParmBlkPtr pbp, Boolean a) /* IMII-186 */
{
    OSErr err;

    err = ROMlib_dispatch(pbp, a, Ctl, 0);
    fs_err_hook(err);
    return err;
}

OSErr Executor::PBStatus(ParmBlkPtr pbp, Boolean a) /* IMII-186 */
{
    OSErr err;

    err = ROMlib_dispatch(pbp, a, Stat, 0);
    fs_err_hook(err);
    return err;
}

OSErr Executor::PBKillIO(ParmBlkPtr pbp, Boolean a) /* IMII-187 */
{
    OSErr err;

    pbp->cntrlParam.csCode = killCode;
    err = ROMlib_dispatch(pbp, a, Ctl, 0);
    fs_err_hook(err);
    return err;
}

/*
 * OpenDriver is defined below ROMlib_driveropen.
 * CloseDriver is defined below ROMlib_driverclose.
 */

/* FSRead, FSWrite are part of the file manager */

OSErr Executor::Control(INTEGER rn, INTEGER code, const void *param) /* IMII-179 */
{
    ParamBlockRec pb;
    OSErr err;

    pb.cntrlParam.ioVRefNum = 0;
    pb.cntrlParam.ioCRefNum = rn;
    pb.cntrlParam.csCode = code;
    if(param)
        BlockMoveData(param, (Ptr)pb.cntrlParam.csParam,
                      (Size)sizeof(pb.cntrlParam.csParam));
    err = PBControl(&pb, false);
    fs_err_hook(err);
    return err;
}

OSErr Executor::Status(INTEGER rn, INTEGER code, Ptr param) /* IMII-179 */
{
    ParamBlockRec pb;
    OSErr retval;

    pb.cntrlParam.ioVRefNum = 0;
    pb.cntrlParam.ioCRefNum = rn;
    pb.cntrlParam.csCode = code;
    retval = PBStatus(&pb, false);
    if(param)
        BlockMoveData((Ptr)pb.cntrlParam.csParam, param,
                      (Size)sizeof(pb.cntrlParam.csParam));
    fs_err_hook(retval);
    return retval;
}

OSErr Executor::KillIO(INTEGER rn) /* IMII-179 */
{
    ParamBlockRec pb;
    OSErr err;

    pb.cntrlParam.ioCRefNum = rn;
    err = PBKillIO(&pb, false);
    fs_err_hook(err);
    return err;
}

DCtlHandle Executor::GetDCtlEntry(INTEGER rn)
{
    int devicen;

    devicen = -rn - 1;
    return (devicen < 0 || devicen >= NDEVICES) ? 0
                                                : LM(UTableBase)[devicen];
}

static std::vector<driverinfo> knowndrivers;

void Executor::RegisterDriver(const driverinfo& di)
{
    knowndrivers.push_back(di);
}

/* Re-enter emulated code to run a driver call's completion routine.
 * A0 = param block, A1 = the routine, D0 = result. */
void Executor::callcomp(ParmBlkPtr pbp, ProcPtr comp, OSErr err)
{
    EM_A0 = US_TO_SYN68K(pbp);
    EM_A1 = US_TO_SYN68K(comp);
    EM_D0 = (unsigned short)err; /* TODO: unsigned short ? */
    execute68K((syn68k_addr_t)(uintptr_t)comp);
}

static void InitBuiltinDrivers()
{
    InitSerialDriver();
}

/*
 * ROMlib_driveropen will be called by PBOpen if it encounters a name
 * beginning with * a period.
 */
OSErr Executor::ROMlib_driveropen(ParmBlkPtr pbp, Boolean a) /* INTERNAL */
{
    if(knowndrivers.empty())
        InitBuiltinDrivers();

    OSErr err;
    INTEGER devicen;
    umacdriverptr up;
    DCtlHandle h;
    ramdriverhand ramdh;
    GUEST<ResType> typ;
    Boolean alreadyopen;

    TheZoneGuard guard(LM(SysZone));

    err = noErr;

    if((ramdh = (ramdriverhand)GetNamedResource("DRVR"_4,
                                                pbp->ioParam.ioNamePtr)))
    {
        LoadResource((Handle)ramdh);
        GUEST<INTEGER> resid;
        GetResInfo((Handle)ramdh, &resid, &typ, (StringPtr)0);
        devicen = resid;
        h = LM(UTableBase)[devicen];
        alreadyopen = h && ((*h)->dCtlFlags & DRIVEROPENBIT);
        if(!h && !(h = LM(UTableBase)[devicen] = (DCtlHandle)NewHandle(sizeof(DCtlEntry))))
            err = MemError();
        else if(!alreadyopen)
        {
            memset((char *)*h, 0, sizeof(DCtlEntry));
            (*h)->dCtlDriver = (umacdriverptr)ramdh;
            (*h)->dCtlFlags = (*ramdh)->drvrFlags | RAMBASEDBIT;
            (*h)->dCtlRefNum = -(devicen + 1);
            (*h)->dCtlDelay = (*ramdh)->drvrDelay;
            (*h)->dCtlEMask = (*ramdh)->drvrEMask;
            (*h)->dCtlMenu = (*ramdh)->drvrMenu;
            if((*h)->dCtlFlags & NEEDTIMEBIT)
                (*h)->dCtlCurTicks = TickCount() + (*h)->dCtlDelay;
            else
                (*h)->dCtlCurTicks = 0x7FFFFFFF;
            /*
	    * NOTE: this code doesn't check to see if something is already open.
	    *	 TODO:  fix this
	    */
            pbp->cntrlParam.ioCRefNum = (*h)->dCtlRefNum;
            err = ROMlib_dispatch(pbp, a, Open, 0);
        }
        else
        {
            pbp->cntrlParam.ioCRefNum = (*h)->dCtlRefNum;
            err = noErr;
        }
    }
    else
    {
        auto dip = knowndrivers.begin(), edip = knowndrivers.end();
        for(;
            dip != edip && !EqualString(dip->name, pbp->ioParam.ioNamePtr, false, true);
            dip++)
            ;
        
        if(dip != edip)
        {
            devicen = -dip->refnum - 1;
            if(devicen < 0 || devicen >= NDEVICES)
                err = badUnitErr;
            else if(LM(UTableBase)[devicen])
                err = noErr; /* note:  if we choose to support desk */
            /*	  accessories, we will have to */
            /*	  check to see if this is one and */
            /*	  call the open routine if it is */
            else
            {
                if(!(h = LM(UTableBase)[devicen] = (DCtlHandle)NewHandle(sizeof(DCtlEntry))))
                    err = MemError();
                else
                {
                    memset((char *)*h, 0, sizeof(DCtlEntry));
                    up = (umacdriverptr)NewPtr(sizeof(umacdriver));
                    if(!((*h)->dCtlDriver = up))
                        err = MemError();
                    else
                    {
                        up->udrvrOpen = dip->open;
                        up->udrvrPrime = dip->prime;
                        up->udrvrCtl = dip->ctl;
                        up->udrvrStatus = dip->status;
                        up->udrvrClose = dip->close;
                        str255assign(up->udrvrName, dip->name);
                        err = noErr;
                    }
                }
            }
            if(err == noErr)
            {
                pbp->cntrlParam.ioCRefNum = dip->refnum;
                err = ROMlib_dispatch(pbp, a, Open, 0);
            }
        }
        else
            err = fnfErr;
    }

    fs_err_hook(err);
    return err;
}

OSErr Executor::OpenDriver(ConstStringPtr name, GUEST<INTEGER> *rnp) /* IMII-178 */
{
    ParamBlockRec pb;
    OSErr retval;

    pb.ioParam.ioRefNum = 0; /* so we can't get garbage for ioRefNum. */
    pb.ioParam.ioPermssn = fsCurPerm;
    pb.ioParam.ioNamePtr = (StringPtr)name;
    retval = ROMlib_driveropen(&pb, false);
    *rnp = pb.ioParam.ioRefNum;
    fs_err_hook(retval);
    return retval;
}

OSErr Executor::CloseDriver(INTEGER rn) /* IMII-178 */
{
    ParamBlockRec pb;
    OSErr err;

    pb.cntrlParam.ioCRefNum = rn;
    err = PBClose(&pb, false);
    fs_err_hook(err);
    return err;
}
