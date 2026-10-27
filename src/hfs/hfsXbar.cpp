/* Copyright 1992 - 1996 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in FileMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <FileMgr.h>
#include <hfs/hfs.h>
#include <file/file.h>
#include <hfs/futzwithdosdisks.h>
#include <commandline/flags.h>
#include <prefs/prefs.h>
#include <base/cpu.h>
#include <file/volume.h>

#include <stdexcept>

using namespace Executor;

/*
 * TODO: pass the information gleaned by hfsvol and hfsfil into the
 *       various hfsPB and ufsPB routines.
 */

#if defined(CACHECHECK)
void cachecheck(HVCB *vcbp)
{
    cacheentry *cachep;
    cachehead *headp;
    INTEGER i;

    headp = (cachehead *)vcbp->vcbCtlBuf;
    for(i = headp->nitems, cachep = headp->flink; --i >= 0;
        cachep = cachep->flink)
        if(cachep->flags & CACHEBUSY)
            warning_unexpected("busy");
}
#endif /* defined(CACHECHECK) */

Volume *getVolume(void *vpb)
{
    IOParam *pb = (IOParam*)vpb;
    LONGINT dir;
    HVCB *vcbp = ROMlib_findvcb(pb->ioVRefNum, pb->ioNamePtr, &dir, true);
    if(!vcbp)
        vcbp = ROMlib_findvcb(pb->ioVRefNum, (StringPtr)0, &dir, true);

    if(vcbp)
        return ((VCBExtra*)vcbp)->volume;
    else
        return nullptr;
}


Volume *getIVolume(void *vpb)
{
    VolumeParam *pb = (VolumeParam*)vpb;
    if(pb->ioVolIndex > 0)
    {
        HVCB *vcbp = (HVCB *)ROMlib_indexqueue(&LM(VCBQHdr), pb->ioVolIndex);
        if(vcbp)
            return ((VCBExtra*)vcbp)->volume;
        else
            return nullptr;
    }
    else
        return getVolume(pb);
}

Volume *getFileVolume(void *vpb)
{
    IOParam *pb = (IOParam*)vpb;
   
    HVCB *vcbp;

    filecontrolblock *fcbp = ROMlib_refnumtofcbp(pb->ioRefNum);
    if(fcbp)
    {
        vcbp = fcbp->fcbVPtr;
        return ((VCBExtra*)vcbp)->volume;
    }
    else
        return nullptr;
}

static Boolean hfsvol(IOParam *pb)
{
    HVCB *vcbp;
    LONGINT dir;

    vcbp = ROMlib_findvcb(pb->ioVRefNum, pb->ioNamePtr, &dir, true);
    if(!vcbp)
    {
        vcbp = ROMlib_findvcb(pb->ioVRefNum, (StringPtr)0, &dir, true);
        if(!vcbp)
            return true; /* hopefully is a messed up working dir
				    reference */
    }
    if(vcbp->vcbCTRef)
    {
#if defined(CACHECHECK)
        cachecheck(vcbp);
#endif /* defined(CACHECHECK) */
        return true;
    }
    else
        return false;
}

static Boolean hfsIvol(VolumeParam *pb) /* potentially Indexed vol */
{
    Boolean retval;
    HVCB *vcbp;

    retval = false;
    if(pb->ioVolIndex > 0)
    {
        vcbp = (HVCB *)ROMlib_indexqueue(&LM(VCBQHdr), pb->ioVolIndex);
        if(vcbp && vcbp->vcbCTRef)
            retval = true;
    }
    else
        retval = hfsvol((IOParam *)pb);
    return retval;
}

static Boolean hfsfil(IOParam *pb)
{
    filecontrolblock *fcbp;
    HVCB *vcbp;

    fcbp = ROMlib_refnumtofcbp(pb->ioRefNum);
    if(fcbp)
    {
        vcbp = fcbp->fcbVPtr;
        if(vcbp->vcbCTRef)
        {
#if defined(CACHECHECK)
            cachecheck(vcbp);
#endif /* defined(CACHECHECK) */
            return true;
        }
        else
            return false;
    }
    else
        return false;
}

template<typename PB>
OSErr handleExceptions(Volume& v, void (Volume::*member)(PB*), PB* pb)
{
    try
    {
        (v.*member)(pb);
        return noErr;
    }
    catch(const fs::filesystem_error& e)
    {
        // these should be caught inside LocalVolume, but it's just more convenient here
        auto ec = e.code();

        if(ec == boost::system::errc::no_such_file_or_directory)
            return fnfErr;
        else if(ec == boost::system::errc::permission_denied)
            return permErr;
        else
            return ioErr;
    }
    catch(const OSErrorException& e)
    {
        return e.code;
    }
    catch(const std::runtime_error&)
    {
        // The CNID mapper throws for backend I/O failures (LMDB errors) and for
        // corrupt stored records. These are runtime errors, not programmer
        // errors, so fail the operation with ioErr instead of letting the
        // exception unwind past the trap boundary and terminate the emulator.
        return ioErr;
    }
}

OSErr Executor::PBHRename(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHRename, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBHRename(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHCreate(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHCreate, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBHCreate(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBDirCreate(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBDirCreate, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBDirCreate(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHDelete(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHDelete, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBHDelete(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

static void
try_to_reopen(DrvQExtra *dqp)
{
#if defined(__linux__)
    drive_flags_t flags;
    dqp->hfs.fd = linuxfloppy_open(0, &dqp->hfs.bsize, &flags,
                                   (char *)dqp->devicename);
#else
    /* #warning need to be able to reopen a drive */
    warning_unimplemented("Unable to reopen a drive because the code has not "
                          "yet\nbeen written for this platform.");
#endif
}

OSErr Executor::PBRead(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;
    DrvQExtra *dqp;

    switch(pb->ioParam.ioRefNum)
    {
        case OURHFSDREF:
            if(ROMlib_directdiskaccess)
            {
                dqp = ROMlib_dqbydrive(pb->ioParam.ioVRefNum);
                if(!dqp)
                {
                    pb->ioParam.ioResult = nsvErr;
                    pb->ioParam.ioActCount = 0;
                }
                else
                {
                    if(dqp->hfs.fd == -1)
                        try_to_reopen(dqp);
                    if(dqp->hfs.fd == -1)
                    {
                        pb->ioParam.ioResult = nsvErr;
                        pb->ioParam.ioActCount = 0;
                    }
                    else if(pb->ioParam.ioPosMode != fsFromStart)
                    {
                        pb->ioParam.ioResult = paramErr; /* for now */
                        pb->ioParam.ioActCount = 0;
                    }
                    else
                        pb->ioParam.ioResult = ROMlib_transphysblk(&dqp->hfs,
                                                                      pb->ioParam.ioPosOffset,
                                                                      pb->ioParam.ioReqCount / PHYSBSIZE,
                                                                      pb->ioParam.ioBuffer, reading,
                                                                      &pb->ioParam.ioActCount);
                }
            }
            else
            {
                pb->ioParam.ioResult = vLckdErr;
                pb->ioParam.ioActCount = 0;
            }
            retval = pb->ioParam.ioResult;
            break;

        default:
            if(pb->ioParam.ioPosMode & NEWLINEMODE)
            {
                char *buf, *p_to_find, *p_alternate, *p;
                unsigned char to_find;
                long act_count;
                ParamBlockRec pbr;

                pbr = *pb;
                to_find = pb->ioParam.ioPosMode >> 8;
                pbr.ioParam.ioPosMode &= 0x7F;

                buf = (char *)alloca(pb->ioParam.ioReqCount);

                pbr.ioParam.ioBuffer = (Ptr)buf;
                retval = PBRead(&pbr, false);
                pb->ioParam.ioActCount = pbr.ioParam.ioActCount;
                pb->ioParam.ioPosOffset = pbr.ioParam.ioPosOffset;

                act_count = pb->ioParam.ioActCount;
                p_to_find = (char *)memchr(buf, to_find, act_count);

                if(to_find == '\r' && ROMlib_newlinetocr)
                    p_alternate = (char *)memchr(buf, '\n', act_count);
                else
                    p_alternate = 0;

                if(p_alternate && (!p_to_find || p_alternate < p_to_find))
                    p = p_alternate;
                else
                    p = p_to_find;

                if(p)
                {
                    int32_t to_backup;

                    retval = noErr; /* we couldn't have gotten EOF yet */
                    to_backup = act_count - (p + 1 - buf);
                    pb->ioParam.ioActCount =  pb->ioParam.ioActCount - to_backup ;
                    {
                        ParamBlockRec newpb;
                        OSErr newerr;

                        newpb.ioParam.ioRefNum = pb->ioParam.ioRefNum;
                        newpb.ioParam.ioPosMode = fsFromMark;
                        newpb.ioParam.ioPosOffset = -to_backup;
                        newerr = PBSetFPos(&newpb, false);
                        if(newerr != noErr)
                            warning_unexpected("err = %d", newerr);
                    }

                    if(ROMlib_newlinetocr && to_find == '\r')
                        *p = '\r';
                }
                memcpy(pb->ioParam.ioBuffer, pbr.ioParam.ioBuffer,
                       pb->ioParam.ioActCount);
                ROMlib_destroy_blocks(US_TO_SYN68K(pb->ioParam.ioBuffer),
                                      pb->ioParam.ioActCount, true);
            }
            else
            {
                if(Volume *v = getFileVolume(pb))
                    retval = handleExceptions(*v, &Volume::PBRead, pb);
                else if(hfsfil((IOParam *)pb))
                    retval = hfsPBRead(pb, async);
                else
                    retval = nsvErr;
            }
            break;
    }
    FAKEASYNC(pb, async, retval);
}

#define SOUND_DRIVER_REF (-4)
#define ffMode 0

OSErr Executor::PBWrite(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;
    HVCB *vcbp;
    DrvQExtra *dqp;

    switch(pb->ioParam.ioRefNum)
    {
        case OURHFSDREF:
            if(!ROMlib_directdiskaccess)
                pb->ioParam.ioResult = vLckdErr;
            else
            {
                dqp = ROMlib_dqbydrive(pb->ioParam.ioVRefNum);
                if(dqp && dqp->hfs.fd == -1)
                    try_to_reopen(dqp);
                vcbp = ROMlib_vcbbydrive(pb->ioParam.ioVRefNum);
                if(!dqp)
                    pb->ioParam.ioResult = nsvErr;
                else if(vcbp && (vcbp->vcbAtrb & VSOFTLOCKBIT))
                    pb->ioParam.ioResult = vLckdErr;
                else if(vcbp && (vcbp->vcbAtrb & VHARDLOCKBIT))
                    pb->ioParam.ioResult = wPrErr;
                else if(pb->ioParam.ioPosMode != fsFromStart)
                    pb->ioParam.ioResult = paramErr; /* for now */
                else
                    pb->ioParam.ioResult = ROMlib_transphysblk(&dqp->hfs,
                                                                  pb->ioParam.ioPosOffset,
                                                                  pb->ioParam.ioReqCount / PHYSBSIZE,
                                                                  pb->ioParam.ioBuffer, writing,
                                                                  &pb->ioParam.ioActCount);
            }
            if(pb->ioParam.ioResult != noErr)
                pb->ioParam.ioActCount = 0;
            retval = pb->ioParam.ioResult;
            break;

#if 0
    case SOUND_DRIVER_REF:
	p = (char *) pb->ioParam.ioBuffer;
	if (*(short *)p == ffMode) {
	    n = pb->ioParam.ioReqCount;
	    ROMlib_dosound(p + 4, n - 4, (void (*)(void)) 0);
	}
	retval = noErr;
	break;
#endif
        default:
            if(Volume *v = getFileVolume(pb))
                retval = handleExceptions(*v, &Volume::PBWrite, pb);
            else if(hfsfil((IOParam *)pb))
                retval = hfsPBWrite(pb, async);
            else
                retval = nsvErr;
            break;
    }
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBClose(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBClose, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBClose(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

/*
 * The test for ioBuffer being 0 was determined necessary after MacRoads
 * couldn't open the serial drivers.  When I first tested PBHOpen to see
 * if it could open drivers, it couldn't, but that's because my test was
 * coded according to Inside Macintosh and I was assuming that ioBuffer
 * wasn't examined.  It took a few runs of binary searches stuffing fields
 * with zeros to find out that ioBuffer had this magic property.
 *
 * PBOpen doesn't require ioBuffer to be 0 in order to open a driver.
 */

OSErr Executor::PBHOpen(HParmBlkPtr pb, Boolean async)
{
    OSErr retval = fnfErr; // no driver found

    if(pb->ioParam.ioBuffer == nullptr && pb->ioParam.ioNamePtr && pb->ioParam.ioNamePtr[0]
       && pb->ioParam.ioNamePtr[1] == '.')
        retval = ROMlib_driveropen((ParmBlkPtr)pb, async);
    
    if(retval != fnfErr)
        ;
    else if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHOpenDF, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBHOpen(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHOpenDF(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHOpenDF, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBHOpen(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHOpenRF(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHOpenRF, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBHOpenRF(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

#if 0
static void
swappedstr255print (const char *prefix, Str255 sp)
{
  printf ("%s", prefix);
  if (sp)
    {
      unsigned char *cp;
      int n;

      cp = (unsigned char *) sp;
      n = *cp++;
      while (n-- > 0)
	putchar (*cp++);
    }
  else
    printf ("<empty>");
  printf ("\n");
}
#endif

OSErr Executor::PBGetCatInfo(CInfoPBPtr pb, Boolean async)
{
    OSErr retval;
    Boolean ishfs;
    GUEST<StringPtr> savep;

    if(pb->dirInfo.ioFDirIndex < 0 && pb->hFileInfo.ioDirID == 1)
        retval = -43; /* perhaps we should check for a valid volume
			 first */
    else
    {
        savep = pb->dirInfo.ioNamePtr;
        if(pb->dirInfo.ioFDirIndex != 0) /* IMIV-155, 156 */
            pb->dirInfo.ioNamePtr = 0;
        Volume *v = getVolume(pb);
        ishfs = hfsvol((IOParam *)pb);
        pb->dirInfo.ioNamePtr = savep;

        if(v)
            retval = handleExceptions(*v, &Volume::PBGetCatInfo, pb);
        else if(ishfs)
            retval = hfsPBGetCatInfo(pb, async);
        else
            retval = nsvErr;
    }
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBSetCatInfo(CInfoPBPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBSetCatInfo, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBSetCatInfo(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBCatMove(CMovePBPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBCatMove, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBCatMove(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBGetVInfo(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getIVolume(pb))
        retval = handleExceptions(*v, &Volume::PBGetVInfo, pb);
    else if(hfsIvol((VolumeParam *)pb))
        retval = hfsPBGetVInfo(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBUnmountVol(ParmBlkPtr pb)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBUnmountVol, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBUnmountVol(pb);
    else
        retval = nsvErr;
    PBRETURN(pb, retval);
}

OSErr Executor::PBEject(ParmBlkPtr pb)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBEject, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBEject(pb);
    else
        retval = nsvErr;
    PBRETURN(pb, retval);
}

OSErr Executor::PBAllocate(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBAllocate, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBAllocate(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBAllocContig(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBAllocContig, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBAllocContig(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHGetFInfo(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;
    Boolean ishfs;
    GUEST<StringPtr> savep;

    savep = pb->ioParam.ioNamePtr;
    if(pb->fileParam.ioFDirIndex > 0) /* IMIV-155, 156 */
        pb->ioParam.ioNamePtr = 0;
    Volume *v = getVolume(pb);
    ishfs = hfsvol((IOParam *)pb);
    pb->ioParam.ioNamePtr = savep;

    if(v)
        retval = handleExceptions(*v, &Volume::PBHGetFInfo, pb);
    else if(ishfs)
        retval = hfsPBHGetFInfo(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBSetEOF(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBSetEOF, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBSetEOF(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBOpen(ParmBlkPtr pb, Boolean async)
{
    OSErr retval = fnfErr; // no driver found

    if(pb->ioParam.ioNamePtr && pb->ioParam.ioNamePtr[0]    // fixme: PBHOpen also checks ioBuffer
       && pb->ioParam.ioNamePtr[1] == '.')
        retval = ROMlib_driveropen(pb, async);

    if(retval != fnfErr)
        ;
    else if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBOpenDF, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBOpen(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBOpenDF(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBOpenDF, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBOpen(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}


#if !defined(NDEBUG)
void test_serial(void)
{
    OSErr open_in_val, open_out_val, close_in_val, close_out_val;
    ParamBlockRec pb_in, pb_out;
    static int count = 0;

    memset(&pb_in, 0, sizeof pb_in);
    memset(&pb_out, 0, sizeof pb_out);
    pb_in.ioParam.ioNamePtr = (StringPtr) "\004.AIn";
    pb_out.ioParam.ioNamePtr = (StringPtr) "\005.AOut";
    open_in_val = PBOpen(&pb_in, false);
    open_out_val = PBOpen(&pb_out, false);
    close_in_val = PBClose(&pb_in, false);
    close_out_val = PBClose(&pb_out, false);
    ++count;
}
#endif

OSErr Executor::PBOpenRF(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBOpenRF, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBOpenRF(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBLockRange(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBLockRange, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBLockRange(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBUnlockRange(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBUnlockRange, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBUnlockRange(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBGetFPos(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBGetFPos, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBGetFPos(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBSetFPos(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBSetFPos, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBSetFPos(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBGetEOF(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBGetEOF, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBGetEOF(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBFlushFile(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getFileVolume(pb))
        retval = handleExceptions(*v, &Volume::PBFlushFile, pb);
    else if(hfsfil((IOParam *)pb))
        retval = hfsPBFlushFile(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBCreate(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBCreate, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBCreate(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBDelete(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBDelete, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBDelete(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBOpenWD(WDPBPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBOpenWD, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBOpenWD(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBCloseWD(WDPBPtr pb, Boolean async)
{
    OSErr retval;

    retval = hfsPBCloseWD(pb, async);
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBGetWDInfo(WDPBPtr pb, Boolean async)
{
    OSErr retval;

    retval = hfsPBGetWDInfo(pb, async);
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBGetFInfo(ParmBlkPtr pb, Boolean async)
{
    Boolean ishfs;
    GUEST<StringPtr> savep;
    OSErr retval;

    savep = pb->ioParam.ioNamePtr;
    if(pb->fileParam.ioFDirIndex > 0) /* IMIV-155, 156 */
        pb->ioParam.ioNamePtr = 0;
    Volume *v = getVolume(pb);
    ishfs = hfsvol((IOParam *)pb);
    pb->ioParam.ioNamePtr = savep;

    if(v)
        retval = handleExceptions(*v, &Volume::PBGetFInfo, pb);
    else if(ishfs)
        retval = hfsPBGetFInfo(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBSetFInfo(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBSetFInfo, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBSetFInfo(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHSetFInfo(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHSetFInfo, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBHSetFInfo(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBSetFLock(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBSetFLock, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBSetFLock(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHSetFLock(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHSetFLock, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBHSetFLock(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBRstFLock(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBRstFLock, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBRstFLock(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHRstFLock(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHRstFLock, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBHRstFLock(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBSetFVers(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBSetFVers, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBSetFVers(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBRename(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBRename, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBRename(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

/*
 * The code below has a hack in it that is due to the non-standard way in
 * which we handle floppy drives.  Currently, under Executor, Cmd-Shift-2
 * causes all potentially removable media drives to be scanned for new volumes
 * and any that are found are immediately mounted.  This means that programs
 * that use GetOSEvent to pick up disk inserted events *before* mounting that
 * then do their own mounting will lose, since the volume will already be
 * mounted and hence their own mount will fail.  BodyWorks 3.0's Installer
 * does this.  So currently if drive 1 or drive 2 is explictly being mounted
 * we return noErr.  This can get us into trouble later, but to fix it will
 * require a major rewrite of how we handle removable media.
 *
 * As a matter of fact, Browser used to have problems with this because it
 * would call PBMountVol to determine whether or not the floppy drive had
 * a disk in it that either was mounted or could be mounted, and it would
 * then call PBGetVInfo and not look at the return value and unconditionally
 * use the volume name it expected to be returned.  Ugh.  
 */

/* #warning hacked PBMountVol */

OSErr Executor::PBMountVol(ParmBlkPtr pb)
{
#if 0
    if (hfsfil((IOParam *) pb))
	return hfsPBMountVol(pb);
    else
	return ufsPBMountVol(pb);
#else
    INTEGER vref;
    OSErr retval;

    vref = pb->ioParam.ioVRefNum;
    if(vref == 1 || vref == 2)
        retval = noErr;
    else
        retval = nsvErr;
    PBRETURN(pb, retval);
#endif
}

OSErr Executor::PBHGetVInfo(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getIVolume(pb))
        retval = handleExceptions(*v, &Volume::PBHGetVInfo, pb);
    else if(hfsIvol((VolumeParam *)pb))
        retval = hfsPBHGetVInfo(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

/* Smaller Installer wants to see bit 17 set.  We're guessing the other
 * bits will keep programs from messing with us */

enum
{
    VOL_BITS = ((1L << bHasExtFSVol)
                | (1L << bNoSysDir)
                | (1L << bNoBootBlks)
                | (1L << bNoDeskItems)
                | (1L << bNoSwitchTo)
                | (1L << bNoLclSync)
                | (1L << bNoVNEdit)
                | (1L << bNoMiniFndr))
};

OSErr Executor::PBHGetVolParms(HParmBlkPtr pb, Boolean async)
{
    LONGINT dir;
    HVCB *vcbp;
    getvolparams_info_t *infop;
    OSErr err;
    INTEGER rc, nused;

#define roomfor(ptr, field, byte_count) \
    ((byte_count)                       \
     >= (int)offsetof(std::remove_reference<decltype(*(ptr))>::type, field) + (int)sizeof((ptr)->field))

    vcbp = ROMlib_findvcb(pb->ioParam.ioVRefNum,
                          pb->ioParam.ioNamePtr, &dir, false);
    if(vcbp)
    {
        infop = (getvolparams_info_t *)pb->ioParam.ioBuffer;
        rc = pb->ioParam.ioReqCount;
        nused = 0;
        if(roomfor(infop, vMVersion, rc))
        {
            infop->vMVersion = 2;
            nused += sizeof(infop->vMVersion);
        }
        if(roomfor(infop, vMAttrib, rc))
        {
            infop->vMAttrib = VOL_BITS;
            nused += sizeof(infop->vMAttrib);
        }
        if(roomfor(infop, vMLocalHand, rc))
        {
            infop->vMLocalHand = 0;
            nused += sizeof(infop->vMLocalHand);
        }
        if(roomfor(infop, vMServerAdr, rc))
        {
            infop->vMServerAdr = 0;
            nused += sizeof(infop->vMServerAdr);
        }
        if(roomfor(infop, vMForeignPrivID, rc))
        {
            infop->vMForeignPrivID = 2; /* fsUnixPriv + 1 */
            nused += sizeof(infop->vMForeignPrivID);
        }
        pb->ioParam.ioActCount = (LONGINT)nused;
        err = noErr;
    }
    else
    {
        err = nsvErr;
        pb->ioParam.ioActCount = 0;
    }
    FAKEASYNC(pb, async, err);
}

OSErr Executor::PBSetVInfo(HParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBSetVInfo, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBSetVInfo(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBGetVol(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    retval = hfsPBGetVol(pb, async);
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHGetVol(WDPBPtr pb, Boolean async)
{
    OSErr retval;

    retval = hfsPBHGetVol(pb, async);
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBSetVol(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    retval = hfsPBSetVol(pb, async);
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBHSetVol(WDPBPtr pb, Boolean async)
{
    OSErr retval;

    retval = hfsPBHSetVol(pb, async);
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBFlushVol(ParmBlkPtr pb, Boolean async)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBFlushVol, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBFlushVol(pb, async);
    else
        retval = nsvErr;
    FAKEASYNC(pb, async, retval);
}

OSErr Executor::PBOffLine(ParmBlkPtr pb)
{
    OSErr retval;

    if(Volume *v = getVolume(pb))
        retval = handleExceptions(*v, &Volume::PBOffLine, pb);
    else if(hfsvol((IOParam *)pb))
        retval = hfsPBOffLine(pb);
    else
        retval = nsvErr;
    PBRETURN(pb, retval);
}
