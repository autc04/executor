/* Copyright 1986, 1989, 1990, 1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in ResourceMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <ResourceMgr.h>
#include <MemoryMgr.h>
#include <FileMgr.h>

#include <res/resource.h>
#include <mman/mman.h>
#include <file/file.h>
#include <osevent/osevent.h>
#include <prefs/prefs.h>
#include <base/functions.impl.h>
#include <algorithm>

using namespace Executor;

void
Executor::HCreateResFile_helper(INTEGER vrefnum, LONGINT parid, ConstStringPtr name,
                                OSType creator, OSType type, ScriptCode script)
{
    INTEGER f;
    LONGINT lc;
    empty_resource_template_t buf;

    ROMlib_setreserr(HCreate(vrefnum, parid, name, creator, type)); /* ????
								       might
								    be wrong */
    if(LM(ResErr) != noErr && LM(ResErr) != dupFNErr)
        return;
    ROMlib_setreserr(HOpenRF(vrefnum, parid, name, fsRdWrPerm, out(f)));
    if(LM(ResErr) != noErr)
        return;
    
    GUEST<LONGINT> leof;
    ROMlib_setreserr(GetEOF(f, &leof));
    if(LM(ResErr) != noErr)
    {
        FSClose(f);
        return;
    }
    if(leof)
    {
        ROMlib_setreserr(dupFNErr);
        FSClose(f);
        return;
    }
    buf.bhead.rdatoff = buf.bhead.rmapoff = sizeof(reshead) + sizeof(rsrvrec);
    buf.bhead.datlen = 0; /* No data */
    buf.bhead.maplen = sizeof(resmap) + sizeof(INTEGER);
    buf.bmap.namoff = sizeof(resmap) + sizeof(INTEGER);
    buf.bmap.resfatr = 0; /* No special attributes */
    buf.bmap.typoff = sizeof(resmap);
    buf.negone = -1; /* zero types (0 - 1) */
    lc = sizeof(buf);
    ROMlib_setreserr(FSWriteAll(f, inout(lc), (Ptr)&buf));
    if(LM(ResErr) != noErr)
        return;
    ROMlib_setreserr(FSClose(f));
}

void Executor::C_CreateResFile(ConstStringPtr fn)
{
    HCreateResFile_helper(0, 0, fn, "????"_4, "????"_4, 0);
}

void Executor::C_HCreateResFile(INTEGER vrefnum, LONGINT parid, ConstStringPtr name)
{
    HCreateResFile_helper(vrefnum, parid, name, "????"_4, "????"_4, 0);
}


Handle Executor::ROMlib_mgetres(resmaphand map, resref *rr)
{
    Handle retval;

    if(!stub_ResourceStub.isPatched())
        retval = ROMlib_mgetres2(map, rr);
    else
    {
        LONGINT saved0, saved1, saved2, savea0, savea1, savea2, savea3, savea4;

        saved0 = EM_D0;
        saved1 = EM_D1;
        saved2 = EM_D2;
        savea0 = EM_A0;
        savea1 = EM_A1;
        savea2 = EM_A2;
        savea3 = EM_A3;
        savea4 = EM_A4;
        EM_A4 = US_TO_SYN68K(map);
        EM_A3 = US_TO_SYN68K(rr);
        EM_A2 = US_TO_SYN68K(rr);
        EM_A0 = (LONGINT)ostraptable[0xFC];
        execute68K(EM_A0);
        retval = (Handle)SYN68K_TO_US(EM_A0);
        EM_D0 = saved0;
        EM_D1 = saved1;
        EM_D2 = saved2;
        EM_A0 = savea0;
        EM_A1 = savea1;
        EM_A2 = savea2;
        EM_A3 = savea3;
        EM_A4 = savea4;
    }
    return retval;
}

/* TODO: decompress_setup also has to pass back the decompressed size
   we need to adjust down the size of "dlen" down below where we read
   the compressed info. */

static bool
decompress_setup(INTEGER rn, int32_t *dlenp, int32_t *final_sizep, int32_t *offsetp,
                 Handle *dcmp_handlep, Ptr *workspacep)
{
    bool retval;
    OSErr err;
    LONGINT len;
    dcomp_info_t info;
    GUEST<LONGINT> master_save_pos;

    *final_sizep = *dlenp;
    *offsetp = 0;
    *dcmp_handlep = nullptr;
    *workspacep = nullptr;

    GetFPos(rn, &master_save_pos);
    len = sizeof info;
    err = FSReadAll(rn, inout(len), (Ptr)&info);

    /*
   * If we can't read the entire header in or if we don't get the correct tag
   * then we'll return false, but clear LM(ResErr).  This is a sign that the
   * resource is to be treated as a non-compressed resource.
   */

    if(err != noErr || info.compressedResourceTag != COMPRESSED_TAG)
    {
        SetFPos(rn, fsFromStart, master_save_pos);
        ROMlib_setreserr(noErr);
        /*->*/ return false;
    }

    if(info.typeFlags != COMPRESSED_FLAGS)
        retval = false;
    else
    {
        GUEST<LONGINT> save_pos;

        GetFPos(rn, &save_pos);
        *dcmp_handlep = GetResource("dcmp"_4, info.dcmpID);
        SetFPos(rn, fsFromStart, save_pos);

        if(!*dcmp_handlep)
            retval = false;
        else
        {
            int32_t final_size;
            int32_t working_size;

            LoadResource(*dcmp_handlep);
            final_size = info.uncompressedSize;

            /* 
	   * The MacTech article says that the workingBufferFractionalSize
	   * byte is a fixed point value, but it doesn't give enough
	   * information to be sure how to interpret it.  I tried  using it
	   * in a variety of ways and found I was not allocating enough bytes.
	   * This use seems to work, but I'm sufficiently nervous to merit
	   * possibly allocating more room than we needed.
	   */

            working_size = (*dlenp + (double)*dlenp * info.workingBufferFractionalRatio / (1 << 8));

#define DONT_TRUST_FRACTIONAL_RATIO
#if defined(DONT_TRUST_FRACTIONAL_RATIO)
            working_size = std::max(final_size, working_size);
#endif

            *workspacep = NewPtr(working_size);
            if(!*workspacep)
                retval = false;
            else
            {
                *dlenp -= sizeof info;
                *final_sizep = final_size;
                *offsetp = info.expansionBufferSize;
                retval = true;
            }
        }
    }

    if(!retval)
        ROMlib_setreserr(CantDecompress);
    return retval;
}

/* ROMlib_mgetres: given a resource map handle and a
	    resource reference pointer, ROMlib_mgetres returns a handle to
	    the appropriate resource */

/*
 * TODO: see whether or not lock bits and whatnot get reset on a
 *       mgetres when the handle's already there.
 */

static Handle mgetres_helper(resmaphand map, resref *rr, int32_t dlen,
                             Handle retval)
{
    int32_t dcmp_offset = 0;
    Handle dcmp_handle = nullptr;
    Ptr dcmp_workspace = nullptr;
    int32_t uncompressed_size = 0;
    Ptr xxx;
    OSErr err;
    bool compressed_p;
    bool done_p = false;

    compressed_p = (rr->ratr & resCompressed) && system_version >= 0x700;

    if(compressed_p)
    {
        if(!decompress_setup((*map)->resfn, &dlen, &uncompressed_size,
                             &dcmp_offset, &dcmp_handle, &dcmp_workspace))
        {
            if(LM(ResErr) == noErr)
                compressed_p = false;
            else
            {
                retval = nullptr;
                done_p = true;
            }
        }
    }

    if(!done_p)
    {
        if(!compressed_p)
        {
            dcmp_offset = 0;
            dcmp_handle = nullptr;
            dcmp_workspace = nullptr;
            uncompressed_size = dlen;
        }

        if(!rr->rhand)
        {
            LM(TheZone) = ((rr->ratr & resSysHeap)
                           ? LM(SysZone)
                           : (GUEST<THz>)HandleZone((Handle)map));
            retval = NewHandle(uncompressed_size + dcmp_offset);
            rr->rhand = retval;
        }
        else
        {
            retval = rr->rhand;
            ReallocateHandle(retval, uncompressed_size + dcmp_offset);
        }
        err = MemError();
        xxx = *retval + uncompressed_size + dcmp_offset - dlen;
        if((ROMlib_setreserr(err)) || (ROMlib_setreserr(err = FSReadAll((*map)->resfn, inout(dlen), xxx))))
        {
            if(dcmp_workspace)
                DisposePtr(dcmp_workspace);
            DisposeHandle(rr->rhand);
            rr->rhand = nullptr;
            retval = nullptr;
        }
        else
        {
            if(dcmp_handle)
            {
                ResourceDecompressionUPP dcmp;
                SignedByte state;

                state = hlock_return_orig_state(dcmp_handle);
                dcmp = (ResourceDecompressionUPP)*dcmp_handle;
                HLock(retval);
                dcmp(xxx, *retval, dcmp_workspace, dlen);
                HUnlock(retval);
                SetHandleSize(retval, uncompressed_size);
                HSetState(dcmp_handle, state);
                if(dcmp_workspace)
                    DisposePtr(dcmp_workspace);
            }
        }
    }

    return retval;
}

/* ROMlib_mgetres: given a resource map handle and a
	    resource reference pointer, ROMlib_mgetres returns a handle to
	    the appropriate resource */

/*
 * TODO: see whether or not lock bits and whatnot get reset on a
 *       mgetres when the handle's already there.
 */

Handle
Executor::ROMlib_mgetres2(resmaphand map, resref *rr)
{
    Handle retval;

    retval = rr->rhand;
    if(retval && *retval)
        ROMlib_setreserr(noErr);
    else
    {
        GUEST<THz> savezone;
        SignedByte state;
        int32_t loc;

        savezone = LM(TheZone);
        state = hlock_return_orig_state((Handle)map);
        loc = (*map)->rh.rdatoff + B3TOLONG(rr->doff);
        ROMlib_setreserr(SetFPos((*map)->resfn, fsFromStart, loc));
        if(LM(ResErr) != noErr)
            retval = nullptr;
        else
        {
            int32_t lc;
            OSErr err;
            GUEST<int32_t> dlen_s; /* length on disk (remaining) */

            lc = sizeof(Size);
            err = FSReadAll((*map)->resfn, inout(lc), (Ptr)&dlen_s);
            ROMlib_setreserr(err);
            if(LM(ResErr) != noErr)
                retval = nullptr;
            else
            {
                int32_t dlen = dlen_s;
                if(LM(ResLoad))
                    retval = mgetres_helper(map, rr, dlen, retval);
                else if(!rr->rhand)
                {
                    LM(TheZone) = ((rr->ratr & resSysHeap)
                                   ? LM(SysZone)
                                   : (GUEST<THz>)HandleZone((Handle)map));
                    retval = NewEmptyHandle();
                    rr->rhand = retval;
                }

                /* we can only set the state bits if the block pointer
		 is non-nil */

                if(retval && *retval)
                    HSetState(retval,
                              (RSRCBIT
                               | ((rr->ratr & resLocked) ? LOCKBIT : 0)
                               | ((rr->ratr & resPurgeable) ? PURGEBIT : 0)));
            }
        }
        HSetState((Handle)map, state);
        LM(TheZone) = savezone;
    }
    return retval;
}

/* ROMlib_rntohandl:  ROMlib_rntohandl returns the resmap handle
	       of the resource file
               with the reference number rn, *pph is filled in with the
               Handle to the previous (on the linked list of files)
               file.  Note pph is undefined if rn is at the top, nor
               is it filled in if pph is nil */

resmaphand Executor::ROMlib_rntohandl(INTEGER rn, Handle *pph) /* INTERNAL */
{
    resmaphand map, ph;

    ph = 0;
    WALKMAPTOP(map)
    if((*map)->resfn == rn)
        break;
    ph = map;
    EWALKMAP()

    if(pph)
        *pph = (Handle)ph;
    return (map);
}

INTEGER Executor::C_OpenRFPerm(ConstStringPtr fn, INTEGER vref,
                               Byte perm) /* IMIV-17 */
{
    INTEGER retval;

    retval = HOpenResFile(vref, 0, fn, perm);
    return retval;
}

INTEGER Executor::C_OpenResFile(ConstStringPtr fn)
{
    return OpenRFPerm(fn, 0, fsCurPerm);
}

void Executor::C_CloseResFile(INTEGER rn)
{
    resmaphand map, ph, nextmap;
    INTEGER i, j;
    typref *tr;
    resref *rr;

    invalidate_kchr_ptr();

    ROMlib_resTypesChanged();
    if(rn == REF0)
    {
        for(map = (resmaphand)LM(TopMapHndl); map; map = nextmap)
        {
            nextmap = (resmaphand)(*map)->nextmap;
            CloseResFile((*map)->resfn);
        }
        /*-->*/ return;
    }
    else
    {
        Handle temph;

        map = ROMlib_rntohandl(rn, &temph);
        ph = (resmaphand)temph;
    }
    if(map)
    {
        OSErr save_ResErr;

        UpdateResFile(rn);
        save_ResErr = LM(ResErr);

        /* update linked list */

        if(map == (resmaphand)LM(TopMapHndl))
            LM(TopMapHndl) = (*map)->nextmap;
        else
            (*ph)->nextmap = (*map)->nextmap;

        if(LM(CurMap) == rn)
        {
            //                printf("curmap %02x topmaphndl %08x\n", (int) LM(CurMap).raw(), (int)LM(TopMapHndl).raw());
            if(LM(TopMapHndl))
                LM(CurMap) = (*(resmaphand)LM(TopMapHndl))->resfn;
            else
                LM(CurMap) = 0;
        }

        /* release individual resource memory */

        WALKTANDR(map, i, tr, j, rr)
        {
            if(Handle h = rr->rhand)
            {
                if(*h)
                    HClrRBit(h);
                DisposeHandle(h);
            }
        }
        EWALKTANDR(tr, rr)

        DisposeHandle((Handle)map);
        FSClose(rn);
        ROMlib_setreserr(save_ResErr);
    }
    else
        ROMlib_setreserr(resFNotFound);
}

static INTEGER
already_open_res_file(GUEST<INTEGER> swapped_vref, GUEST<LONGINT> swapped_file_num)
{
    resmaphand map;
    fcbrec *fcbp;
    OSErr err;
    INTEGER retval;

    retval = -1;
    WALKMAPTOP(map)
    fcbp = PRNTOFPERR((*map)->resfn, &err);
    if(err == noErr && fcbp->fdfnum == swapped_file_num)
    {
        VCB *vptr;
        vptr = fcbp->fcvptr;
        if(vptr->vcbVRefNum == swapped_vref && (fcbp->fcflags & fcfisres))
            retval = (*map)->resfn;
    }
    EWALKMAP()
    return retval;
}

INTEGER Executor::C_HOpenResFile(INTEGER vref, LONGINT dirid, ConstStringPtr fn,
                                 SignedByte perm)
{
    INTEGER f;
    reshead hd;
    LONGINT lc;
    resmaphand map;
    INTEGER i, j;
    typref *tr;
    resref *rr;
    HParamBlockRec pbr = {};
    OSErr err;

    invalidate_kchr_ptr();

    ROMlib_setreserr(noErr);

    /* check for file already opened */

    {
        CInfoPBRec cpb = {};
        Str255 local_name;

        str255assign(local_name, fn);
        pbr.volumeParam.ioNamePtr = (StringPtr)local_name;
        pbr.volumeParam.ioVRefNum = vref;
        pbr.volumeParam.ioVolIndex = -1;
        err = PBHGetVInfo(&pbr, false);
        if(err)
        {
            ROMlib_setreserr(err);
            return -1;
        }

        cpb.hFileInfo.ioNamePtr = (StringPtr)fn;
        cpb.hFileInfo.ioVRefNum = vref;
        cpb.hFileInfo.ioFDirIndex = 0;
        cpb.hFileInfo.ioDirID = dirid;
        if((ROMlib_setreserr(PBGetCatInfo(&cpb, 0))) == noErr
           && perm > fsRdPerm)
        {
            INTEGER fref;

            fref = already_open_res_file(pbr.volumeParam.ioVRefNum,
                                         cpb.hFileInfo.ioDirID);
            if(fref != -1)
            {
                LM(CurMap) = fref;
                /*-->*/ return fref;
            }
        }
    }

    if(LM(ResErr) != noErr)
        /*-->*/ return -1;

    ROMlib_resTypesChanged();
    pbr.ioParam.ioNamePtr = (StringPtr)fn;
    pbr.ioParam.ioVRefNum = vref;
    pbr.fileParam.ioFDirIndex = 0;
    pbr.ioParam.ioPermssn = perm;
    pbr.ioParam.ioMisc = 0;
    pbr.fileParam.ioDirID = dirid;
    ROMlib_setreserr(PBHOpenRF(&pbr, false));
    if(LM(ResErr) != noErr)
        return (-1);
    f = pbr.ioParam.ioRefNum;
    lc = sizeof(hd);
    ROMlib_setreserr(FSReadAll(f, inout(lc), (Ptr)&hd));
    if(LM(ResErr) != noErr)
    {
        FSClose(f);
        return (-1);
    }
    map = (resmaphand)NewHandle(hd.maplen);
    err = MemError();
    if(ROMlib_setreserr(err))
    {
        FSClose(f);
        return (-1);
    }

    ROMlib_setreserr(SetFPos(f, fsFromStart, hd.rmapoff));
    if(LM(ResErr) != noErr)
    {
        DisposeHandle((Handle)map);
        FSClose(f);
        return (-1);
    }
    lc = hd.maplen;
    ROMlib_setreserr(FSReadAll(f, inout(lc), (Ptr)*map));
    if(LM(ResErr) != noErr)
    {
        DisposeHandle((Handle)map);
        FSClose(f);
        return (-1);
    }

    (*map)->rh = hd;

    /* IMIV: consistency checks */

    if(
#if 0 /* See NOTE below */
	(*map)->rh.rdatoff != sizeof(reshead) + sizeof(rsrvrec) ||
        (*map)->rh.rmapoff < (*map)->rh.rdatoff + (*map)->rh.datlen ||
#else
        (*map)->rh.rdatoff < (int)sizeof(reshead) + (int)sizeof(rsrvrec) ||
#endif
        (*map)->rh.datlen < 0 || (*map)->rh.maplen < (int)sizeof(resmap) + (int)sizeof(INTEGER) || (*map)->typoff < (int)sizeof(resmap)
#if 0
/*
 * NOTE:  I used to have the following test in here, but when I ran
 *	  Disinfectant 1.5 from the BCS Mac PD-CD, we found 6 files
 *	  that were "corrupt" here, but not there.  The first file
 *	  that gave us trouble didn't pass the following test, so
 *	  presumably, the Mac doesn't make this test.
 */
	||
        (*map)->namoff < sizeof(resmap) +
                (NUMTMINUS1(map)+1) * (sizeof(resref) + sizeof(typref))
#endif
            )
    {
        ROMlib_setreserr(mapReadErr);
        DisposeHandle((Handle)map);
        FSClose(f);
        return (-1);
    }

    (*map)->nextmap = LM(TopMapHndl);
    (*map)->resfn = f;
    LM(TopMapHndl) = (Handle)map;
    LM(CurMap) = f;

    /* check for resprload bits */

    WALKTANDR(map, i, tr, j, rr)
    rr->rhand = 0;
    rr->ratr &= ~resChanged;
    if(rr->ratr & resPreload)
        ROMlib_mgetres(map, rr);
    EWALKTANDR(tr, rr)
    return (f);
}
