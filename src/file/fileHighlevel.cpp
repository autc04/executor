/* Copyright 1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* NOTE:  This is the quick hack version that uses old-style calls.
          Eventually we should extract the common functionality and
	  then use that for both.  That will be a little cleaner, a little
	  faster and a little easier to debug, but not enough of any to
	  make it sensible to do that initially.  */

#include <base/common.h>

#include <MemoryMgr.h>
#include <FileMgr.h>
#include <ProcessMgr.h>

#include <file/file.h>
#include <util/string.h>
#include <hfs/hfs.h>
#include <rsys/executor.h>

using namespace Executor;

/*
 * extract the last component of a name:
 *
 * aaa:bbb:ccc  -> ccc
 * aaa:bbb:ccc: -> ccc
 */

static void
extract_name(Str255 dest, ConstStringPtr source)
{
    int len, new_len;
    const Byte *p;

    len = source[0];
    if(source[len] == ':') /* ignore trailing ':' */
        --len;
    for(p = source + len; p >= source + 1 && *p != ':'; --p)
        ;
    new_len = len - (p - source);
    dest[0] = new_len;
    memmove(dest + 1, p + 1, new_len);
}

OSErr Executor::C_FSMakeFSSpec(int16_t vRefNum, int32_t dir_id,
                               ConstStringPtr file_name, FSSpecPtr spec)
{
    Str255 local_file_name;
    OSErr retval;
    HParamBlockRec hpb;

    if(!file_name)
        warning_unexpected("!file_name");

    /*
 * Colons make things tricky because they are used both to identify volumes
 * and to delimit directories and sometimes to identify partial paths.  Right
 * now, most of these uses of colon shouldn't cause problems, but there are
 * some issues that need to be checked on a real Mac.  What do all these
 * mean?
 *
 *     dir_id      file_name
 *     310         foo:
 *     310         :foo:
 *     310         foo:bar
 *     310         :foo:bar
 *     310         :foo:bar:
 *
 * what about all those file_names, but with dir_id = 0, dir_id = 1, dir_id = 2
 * and dir_id = a number that isn't a directory id?
 */

    if(pstr_index_after(file_name, ':', 0))
        warning_unexpected("colon found");

    str255assign(local_file_name, file_name);
    hpb.volumeParam.ioNamePtr = (StringPtr)local_file_name;
    hpb.volumeParam.ioVRefNum = vRefNum;
    if(file_name[0])
        hpb.volumeParam.ioVolIndex = -1;
    else
        hpb.volumeParam.ioVolIndex = 0;

    retval = PBHGetVInfo(&hpb, false);

    if(retval == noErr)
    {
        CInfoPBRec cpb;

        str255assign(local_file_name, file_name);
        cpb.hFileInfo.ioNamePtr = (StringPtr)local_file_name;
        cpb.hFileInfo.ioVRefNum = vRefNum;
        if(file_name[0])
            cpb.hFileInfo.ioFDirIndex = 0;
        else
            cpb.hFileInfo.ioFDirIndex = -1;
        cpb.hFileInfo.ioDirID = dir_id;
        retval = PBGetCatInfo(&cpb, false);
        if(retval == noErr)
        {
            spec->vRefNum = hpb.volumeParam.ioVRefNum;
            spec->parID = cpb.hFileInfo.ioFlParID;
            extract_name((StringPtr)spec->name, cpb.hFileInfo.ioNamePtr);
        }
        else if(retval == fnfErr)
        {
            OSErr err;

            cpb.hFileInfo.ioNamePtr = nullptr;
            cpb.hFileInfo.ioVRefNum = vRefNum;
            cpb.hFileInfo.ioFDirIndex = -1;
            cpb.hFileInfo.ioDirID = dir_id;
            err = PBGetCatInfo(&cpb, false);
            if(err == noErr)
            {
                if(cpb.hFileInfo.ioFlAttrib & ATTRIB_ISADIR)
                {
                    spec->vRefNum = hpb.volumeParam.ioVRefNum;
                    /* Report the directory ID that PBGetCatInfo
                     * resolved rather than the one we were handed:
                     * dir_id is 0 whenever the caller means "the
                     * default directory", and echoing that back leaves
                     * the spec pointing at no directory at all.
                     * Callers depend on the spec being usable after
                     * fnfErr -- that is how a new file gets created. */
                    spec->parID = cpb.dirInfo.ioDrDirID;
                    extract_name((StringPtr)spec->name, file_name);
                }
                else
                    retval = dupFNErr;
            }
        }
    }
    return retval;
}

/*
 * Part of ugly PAUP-specific hack below
 */

static void
create_temp_name(Str63 name, int i)
{
    OSErr err;
    ProcessSerialNumber psn;

    err = GetCurrentProcess(&psn);
    if(err == noErr)
        sprintf((char *)name + 1,
                "%x%x.%x", toHost(psn.highLongOfPSN), toHost(psn.lowLongOfPSN), i);
    else
        sprintf((char *)name + 1, "%d.%x", err, i);
    name[0] = strlen((char *)name + 1);
}

/*
 * On real HFS volumes we could do some B-tree manipulation to achieve
 * the correct results, *but* we'd need this code for ufs volumes anyway,
 * so for now we do the same thing on both.
 */

OSErr Executor::C_FSpExchangeFiles(FSSpecPtr src, FSSpecPtr dst)
{
    OSErr retval;

    warning_unimplemented("poorly implemented");
    if(src->vRefNum != dst->vRefNum)
        retval = diffVolErr;
    else if(ROMlib_creator != "PAUP"_4 || src->parID != dst->parID)
        retval = wrgVolTypeErr;
    else
    {
        /* Evil hack to get PAUP to work -- doesn't bother adjusting FCBs */
        FSSpec tmp_spec;
        int i;

        i = 0;
        tmp_spec = *dst;
        do
        {
            create_temp_name(tmp_spec.name, i++);
            retval = FSpRename(dst, tmp_spec.name);
        } while(retval == dupFNErr);
        if(retval == noErr)
        {
            retval = FSpRename(src, dst->name);
            if(retval != noErr)
                FSpRename(&tmp_spec, dst->name);
            else
            {
                retval = FSpRename(&tmp_spec, src->name);
                if(retval != noErr)
                {
                    FSpRename(dst, src->name);
                    FSpRename(&tmp_spec, dst->name);
                }
            }
        }
    }
    return retval;
}

typedef OSErr (*open_procp)(HParmBlkPtr pb, Boolean sync);

static OSErr
open_helper(FSSpecPtr spec, SignedByte perms, GUEST<int16_t> *refoutp,
            open_procp procp)
{
    OSErr retval;
    HParamBlockRec hpb;

    hpb.ioParam.ioVRefNum = spec->vRefNum;
    hpb.fileParam.ioDirID = spec->parID;
    hpb.ioParam.ioNamePtr = (StringPtr)spec->name;
    if(perms == fsWrPerm)
    {
        warning_unexpected("");
        perms = fsRdWrPerm;
    }
    hpb.ioParam.ioPermssn = perms;
    retval = procp(&hpb, false);
    if(retval == noErr)
        *refoutp = hpb.ioParam.ioRefNum;
    return retval;
}

OSErr Executor::C_FSpOpenDF(FSSpecPtr spec, SignedByte perms,
                            GUEST<int16_t> *refoutp)
{
    return open_helper(spec, perms, refoutp, PBHOpenDF);
}

OSErr Executor::C_FSpOpenRF(FSSpecPtr spec, SignedByte perms,
                            GUEST<int16_t> *refoutp)
{
    return open_helper(spec, perms, refoutp, PBHOpenRF);
}

OSErr Executor::C_FSpCreate(FSSpecPtr spec, OSType creator, OSType file_type,
                            ScriptCode script)
{
    OSErr retval;

    retval = HCreate(spec->vRefNum, spec->parID, spec->name,
                     creator, file_type);
    return retval;
}

OSErr Executor::C_FSpDirCreate(FSSpecPtr spec, ScriptCode script,
                               GUEST<int32_t> *created_dir_id)
{
    OSErr retval;
    HParamBlockRec hpb;

    hpb.ioParam.ioVRefNum = spec->vRefNum;
    hpb.fileParam.ioDirID = spec->parID;
    hpb.ioParam.ioNamePtr = (StringPtr)spec->name;
    retval = PBDirCreate(&hpb, false);
    if(retval == noErr)
        *created_dir_id = hpb.fileParam.ioDirID;
    return retval;
}

OSErr Executor::C_FSpDelete(FSSpecPtr spec)
{
    OSErr retval;
    HParamBlockRec hpb;

    hpb.ioParam.ioVRefNum = spec->vRefNum;
    hpb.fileParam.ioDirID = spec->parID;
    hpb.ioParam.ioNamePtr = (StringPtr)spec->name;
    retval = PBHDelete(&hpb, false);
    return retval;
}

OSErr Executor::C_FSpGetFInfo(FSSpecPtr spec, FInfo *fndr_info)
{
    OSErr retval;
    HParamBlockRec hpb;

    hpb.fileParam.ioVRefNum = spec->vRefNum;
    hpb.fileParam.ioDirID = spec->parID;
    hpb.fileParam.ioNamePtr = (StringPtr)spec->name;
    hpb.fileParam.ioFDirIndex = 0;
    retval = PBHGetFInfo(&hpb, false);
    if(retval == noErr)
        *fndr_info = hpb.fileParam.ioFlFndrInfo;
    return retval;
}

OSErr Executor::C_FSpSetFInfo(FSSpecPtr spec, FInfo *fndr_info)
{
    OSErr retval;
    HParamBlockRec hpb;

    warning_unimplemented("poorly implemented: call of PBHGetFInfo wasteful");
    hpb.fileParam.ioVRefNum = spec->vRefNum;
    hpb.fileParam.ioDirID = spec->parID;
    hpb.fileParam.ioNamePtr = (StringPtr)spec->name;
    hpb.fileParam.ioFDirIndex = 0;
    retval = PBHGetFInfo(&hpb, false);
    if(retval == noErr)
    {
        hpb.fileParam.ioDirID = spec->parID;
        hpb.fileParam.ioFlFndrInfo = *fndr_info;
        retval = PBHSetFInfo(&hpb, false);
    }
    return retval;
}

typedef OSErr (*lock_procp)(HParmBlkPtr pb, Boolean async);

static OSErr
lock_helper(FSSpecPtr spec, lock_procp procp)
{
    OSErr retval;
    HParamBlockRec hpb;

    hpb.fileParam.ioVRefNum = spec->vRefNum;
    hpb.fileParam.ioDirID = spec->parID;
    hpb.fileParam.ioNamePtr = (StringPtr)spec->name;
    retval = procp(&hpb, false);
    return retval;
}

OSErr Executor::C_FSpSetFLock(FSSpecPtr spec)
{
    return lock_helper(spec, PBHSetFLock);
}

OSErr Executor::C_FSpRstFLock(FSSpecPtr spec)
{
    return lock_helper(spec, PBHRstFLock);
}

OSErr Executor::C_FSpRename(FSSpecPtr spec, ConstStringPtr new_name)
{
    OSErr retval;
    HParamBlockRec hpb;

    hpb.fileParam.ioVRefNum = spec->vRefNum;
    hpb.fileParam.ioDirID = spec->parID;
    hpb.fileParam.ioNamePtr = (StringPtr)spec->name;
    hpb.ioParam.ioMisc = guest_cast<LONGINT>(new_name);
    retval = PBHRename(&hpb, false);
    return retval;
}

OSErr Executor::C_FSpCatMove(FSSpecPtr src, FSSpecPtr dst)
{
    OSErr retval;
    CMovePBRec cbr;

    if(src->vRefNum != dst->vRefNum)
        retval = paramErr;
    else
    {
        cbr.ioVRefNum = src->vRefNum;
        cbr.ioDirID = src->parID;
        cbr.ioNamePtr = (StringPtr)src->name;
        cbr.ioNewName = (StringPtr)dst->name;
        cbr.ioNewDirID = dst->parID;
        retval = PBCatMove(&cbr, false);
    }
    return retval;
}

void Executor::C_FSpCreateResFile(FSSpecPtr spec, OSType creator,
                                  OSType file_type, ScriptCode script)
{
    HCreateResFile_helper(spec->vRefNum, spec->parID,
                          spec->name, creator, file_type, script);
}

INTEGER Executor::C_FSpOpenResFile(FSSpecPtr spec, SignedByte perms)
{
    INTEGER retval;

    retval = HOpenResFile(spec->vRefNum, spec->parID, spec->name,
                          perms);
    return retval;
}

/* NOTE: the HCreate and HOpenRF are not traps, they're just high level
   calls that are handy to use elsewhere, so they're included here. */

OSErr
Executor::HCreate(INTEGER vref, LONGINT dirid, ConstStringPtr name, OSType creator, OSType type)
{
    HParamBlockRec hpb;
    OSErr retval;

    hpb.fileParam.ioNamePtr = (StringPtr)name;
    hpb.fileParam.ioVRefNum = vref;
    hpb.fileParam.ioDirID = dirid;
    retval = PBHCreate(&hpb, false);
    if(retval == noErr)
    {
        hpb.fileParam.ioFDirIndex = 0;
        retval = PBHGetFInfo(&hpb, false);
        if(retval == noErr)
        {
            hpb.fileParam.ioFlFndrInfo.fdCreator = creator;
            hpb.fileParam.ioFlFndrInfo.fdType = type;
            hpb.fileParam.ioDirID = dirid;
            retval = PBHSetFInfo(&hpb, false);
        }
    }
    return retval;
}

OSErr
Executor::HOpenRF(INTEGER vref, LONGINT dirid, ConstStringPtr name, SignedByte perm,
                  GUEST<INTEGER> *refp)
{
    HParamBlockRec hpb;
    OSErr retval;

    hpb.fileParam.ioNamePtr = (StringPtr)name;
    hpb.fileParam.ioVRefNum = vref;
    hpb.ioParam.ioPermssn = perm;
    hpb.ioParam.ioMisc = 0;
    hpb.fileParam.ioDirID = dirid;
    retval = PBHOpenRF(&hpb, false);
    if(retval == noErr)
        *refp = hpb.ioParam.ioRefNum;
    return retval;
}

OSErr
Executor::HOpenDF(INTEGER vref, LONGINT dirid, ConstStringPtr name, SignedByte perm,
                  GUEST<INTEGER> *refp)
{
    HParamBlockRec hpb;
    OSErr retval;

    hpb.fileParam.ioNamePtr = (StringPtr)name;
    hpb.fileParam.ioVRefNum = vref;
    hpb.ioParam.ioPermssn = perm;
    hpb.ioParam.ioMisc = 0;
    hpb.fileParam.ioDirID = dirid;
    retval = PBHOpenDF(&hpb, false);
    if(retval == noErr)
        *refp = hpb.ioParam.ioRefNum;
    return retval;
}

OSErr
Executor::HOpen(INTEGER vref, LONGINT dirid, ConstStringPtr name, SignedByte perm,
                  GUEST<INTEGER> *refp)
{
    HParamBlockRec hpb;
    OSErr retval;

    hpb.fileParam.ioNamePtr = (StringPtr)name;
    hpb.fileParam.ioVRefNum = vref;
    hpb.ioParam.ioPermssn = perm;
    hpb.ioParam.ioMisc = 0;
    hpb.fileParam.ioDirID = dirid;
    retval = PBHOpen(&hpb, false);
    if(retval == noErr)
        *refp = hpb.ioParam.ioRefNum;
    return retval;
}


OSErr Executor::GetVInfo(INTEGER drv, StringPtr voln, GUEST<INTEGER> *vrn,
                         GUEST<LONGINT> *freeb) /* IMIV-107 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.volumeParam.ioVolIndex = 0;
    pbr.volumeParam.ioVRefNum = drv;
    pbr.volumeParam.ioNamePtr = (StringPtr)voln;
    temp = PBGetVInfo(&pbr, 0);
    *vrn = pbr.volumeParam.ioVRefNum;
    *freeb = pbr.volumeParam.ioVFrBlk * pbr.volumeParam.ioVAlBlkSiz;
    return (temp);
}

OSErr Executor::GetVol(StringPtr voln, GUEST<INTEGER> *vrn) /* IMIV-107 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.volumeParam.ioNamePtr = voln;
    temp = PBGetVol(&pbr, 0);
    *vrn = pbr.volumeParam.ioVRefNum;
    return (temp);
}

OSErr Executor::SetVol(ConstStringPtr voln, INTEGER vrn) /* IMIV-107 */
{
    ParamBlockRec pbr;

    pbr.volumeParam.ioNamePtr = (StringPtr)voln;
    pbr.volumeParam.ioVRefNum = vrn;
    return (PBSetVol(&pbr, 0));
}

OSErr Executor::FlushVol(ConstStringPtr voln, INTEGER vrn) /* IMIV-108 */
{
    ParamBlockRec pbr;

    pbr.ioParam.ioNamePtr = (StringPtr)voln;
    pbr.ioParam.ioVRefNum = vrn;
    return (PBFlushVol(&pbr, 0));
}

OSErr Executor::UnmountVol(ConstStringPtr voln, INTEGER vrn) /* IMIV-108 */
{
    ParamBlockRec pbr;

    pbr.ioParam.ioNamePtr = (StringPtr)voln;
    pbr.ioParam.ioVRefNum = vrn;
    return (PBUnmountVol(&pbr));
}

OSErr Executor::Eject(ConstStringPtr voln, INTEGER vrn) /* IMIV-108 */
{
    ParamBlockRec pbr;

    pbr.ioParam.ioNamePtr = (StringPtr)voln;
    pbr.ioParam.ioVRefNum = vrn;
    return (PBEject(&pbr));
}


OSErr Executor::FSOpen(ConstStringPtr filen, INTEGER vrn, GUEST<INTEGER> *rn) /* IMIV-109 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.ioParam.ioNamePtr = (StringPtr)filen;
    pbr.ioParam.ioVRefNum = vrn;
    pbr.ioParam.ioVersNum = 0;
    pbr.ioParam.ioPermssn = fsCurPerm;
    pbr.ioParam.ioMisc = 0;
    temp = PBOpen(&pbr, 0);
    *rn = pbr.ioParam.ioRefNum;
    fs_err_hook(temp);
    return (temp);
}

OSErr Executor::OpenRF(ConstStringPtr filen, INTEGER vrn, GUEST<INTEGER> *rn) /* IMIV-109 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.ioParam.ioNamePtr = (StringPtr)filen;
    pbr.ioParam.ioVRefNum = vrn;
    pbr.ioParam.ioVersNum = 0;
    pbr.ioParam.ioPermssn = fsCurPerm;
    pbr.ioParam.ioMisc = 0;
    temp = PBOpenRF(&pbr, 0);
    *rn = pbr.ioParam.ioRefNum;
    fs_err_hook(temp);
    return (temp);
}

OSErr Executor::OpenDF(ConstStringPtr filen, INTEGER vrn, GUEST<INTEGER> *rn) /* IMIV-109 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.ioParam.ioNamePtr = (StringPtr)filen;
    pbr.ioParam.ioVRefNum = vrn;
    pbr.ioParam.ioVersNum = 0;
    pbr.ioParam.ioPermssn = fsCurPerm;
    pbr.ioParam.ioMisc = 0;
    temp = PBOpenDF(&pbr, 0);
    *rn = pbr.ioParam.ioRefNum;
    fs_err_hook(temp);
    return (temp);
}


OSErr Executor::FSRead(INTEGER rn, GUEST<LONGINT> *count, void* buffp) /* IMIV-109 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.ioParam.ioRefNum = rn;
    pbr.ioParam.ioBuffer = (Ptr)buffp;
    pbr.ioParam.ioReqCount = *count;
    pbr.ioParam.ioPosMode = fsAtMark;
    temp = PBRead(&pbr, 0);
    *count = pbr.ioParam.ioActCount;
    fs_err_hook(temp);
    return (temp);
}

OSErr
Executor::FSReadAll(INTEGER rn, GUEST<LONGINT> *countp, Ptr buffp)
{
    OSErr retval;

    GUEST<LONGINT> orig_count = *countp;
    retval = FSRead(rn, countp, buffp);
    if(retval == noErr && *countp != orig_count)
        retval = eofErr;
    return retval;
}

OSErr Executor::FSWrite(INTEGER rn, GUEST<LONGINT> *count, const void* buffp) /* IMIV-110 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.ioParam.ioRefNum = rn;
    pbr.ioParam.ioBuffer = (Ptr)buffp;
    pbr.ioParam.ioReqCount = *count;
    pbr.ioParam.ioPosMode = fsAtMark;
    temp = PBWrite(&pbr, 0);
    *count = pbr.ioParam.ioActCount;
    fs_err_hook(temp);
    return (temp);
}

OSErr
Executor::FSWriteAll(INTEGER rn, GUEST<LONGINT> *countp, Ptr buffp)
{
    GUEST<LONGINT> orig_count;
    OSErr retval;

    orig_count = *countp;
    retval = FSWrite(rn, countp, buffp);
    if(retval == noErr && *countp != orig_count)
        retval = ioErr;
    return retval;
}

OSErr Executor::GetFPos(INTEGER rn, GUEST<LONGINT> *filep) /* IMIV-110 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.ioParam.ioRefNum = rn;
    temp = PBGetFPos(&pbr, 0);
    *filep = pbr.ioParam.ioPosOffset;
    fs_err_hook(temp);
    return (temp);
}

OSErr Executor::SetFPos(INTEGER rn, INTEGER posmode,
                        LONGINT possoff) /* IMIV-110 */
{
    ParamBlockRec pbr;
    OSErr err;

    pbr.ioParam.ioRefNum = rn;
    pbr.ioParam.ioPosMode = posmode;
    pbr.ioParam.ioPosOffset = possoff;
    err = PBSetFPos(&pbr, 0);
    fs_err_hook(err);
    return err;
}

OSErr Executor::GetEOF(INTEGER rn, GUEST<LONGINT> *eof) /* IMIV-111 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.ioParam.ioRefNum = rn;
    temp = PBGetEOF(&pbr, 0);
    *eof = pbr.ioParam.ioMisc;
    fs_err_hook(temp);
    return (temp);
}

OSErr Executor::SetEOF(INTEGER rn, LONGINT eof) /* IMIV-111 */
{
    ParamBlockRec pbr;
    OSErr err;

    pbr.ioParam.ioRefNum = rn;
    pbr.ioParam.ioMisc = eof;
    err = PBSetEOF(&pbr, 0);
    fs_err_hook(err);
    return (err);
}

OSErr Executor::Allocate(INTEGER rn, GUEST<LONGINT> *count) /* IMIV-112 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.ioParam.ioRefNum = rn;
    pbr.ioParam.ioReqCount = *count;
    temp = PBAllocate(&pbr, 0);
    *count = pbr.ioParam.ioActCount;
    fs_err_hook(temp);
    return (temp);
}

OSErr Executor::AllocContig(INTEGER rn, GUEST<LONGINT> *count)
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.ioParam.ioRefNum = rn;
    pbr.ioParam.ioReqCount = *count;
    temp = PBAllocContig(&pbr, 0);
    *count = pbr.ioParam.ioActCount;
    fs_err_hook(temp);
    return (temp);
}

OSErr Executor::FSClose(INTEGER rn) /* IMIV-112 */
{
    ParamBlockRec pbr;
    OSErr err;

    pbr.ioParam.ioRefNum = rn;
    err = PBClose(&pbr, 0);
    fs_err_hook(err);
    return err;
}


OSErr Executor::GetFInfo(ConstStringPtr filen, INTEGER vrn,
                         FInfo *fndrinfo) /* IMIV-113 */
{
    ParamBlockRec pbr;
    OSErr temp;

    pbr.fileParam.ioNamePtr = (StringPtr)filen;
    pbr.fileParam.ioVRefNum = vrn;
    pbr.fileParam.ioFVersNum = 0;
    pbr.fileParam.ioFDirIndex = 0;
    temp = PBGetFInfo(&pbr, 0);
    if(temp == noErr)
    {
        fndrinfo->fdType = pbr.fileParam.ioFlFndrInfo.fdType;
        fndrinfo->fdCreator = pbr.fileParam.ioFlFndrInfo.fdCreator;
        fndrinfo->fdFlags = pbr.fileParam.ioFlFndrInfo.fdFlags;
        fndrinfo->fdLocation = pbr.fileParam.ioFlFndrInfo.fdLocation;
        fndrinfo->fdFldr = pbr.fileParam.ioFlFndrInfo.fdFldr;
    }
    return temp;
}

OSErr Executor::HGetFInfo(INTEGER vref, LONGINT dirid, ConstStringPtr name, FInfo *fndrinfo)
{
    HParamBlockRec pbr;
    OSErr retval;

    memset(&pbr, 0, sizeof pbr);
    pbr.fileParam.ioNamePtr = (StringPtr)name;
    pbr.fileParam.ioVRefNum = vref;
    pbr.fileParam.ioDirID = dirid;
    retval = PBHGetFInfo(&pbr, false);
    if(retval == noErr)
    {
        fndrinfo->fdType = pbr.fileParam.ioFlFndrInfo.fdType;
        fndrinfo->fdCreator = pbr.fileParam.ioFlFndrInfo.fdCreator;
        fndrinfo->fdFlags = pbr.fileParam.ioFlFndrInfo.fdFlags;
        fndrinfo->fdLocation = pbr.fileParam.ioFlFndrInfo.fdLocation;
        fndrinfo->fdFldr = pbr.fileParam.ioFlFndrInfo.fdFldr;
    }

    return retval;
}

OSErr Executor::SetFInfo(ConstStringPtr filen, INTEGER vrn,
                         FInfo *fndrinfo) /* IMIV-114 */
{
    ParamBlockRec pbr;
    OSErr temp;
    GUEST<ULONGINT> t;

    pbr.fileParam.ioNamePtr = (StringPtr)filen;
    pbr.fileParam.ioVRefNum = vrn;
    pbr.fileParam.ioFVersNum = 0;
    pbr.fileParam.ioFDirIndex = 0;
    temp = PBGetFInfo(&pbr, 0);
    if(temp != noErr)
        return (temp);
    pbr.fileParam.ioFlFndrInfo.fdType = fndrinfo->fdType;
    pbr.fileParam.ioFlFndrInfo.fdCreator = fndrinfo->fdCreator;
    pbr.fileParam.ioFlFndrInfo.fdFlags = fndrinfo->fdFlags;
    pbr.fileParam.ioFlFndrInfo.fdLocation = fndrinfo->fdLocation;
    pbr.fileParam.ioFlFndrInfo.fdFldr = fndrinfo->fdFldr = pbr.fileParam.ioFlFndrInfo.fdFldr;

    GetDateTime(&t);
    pbr.fileParam.ioFlMdDat = t;

    return (PBSetFInfo(&pbr, 0));
}

OSErr Executor::SetFLock(ConstStringPtr filen, INTEGER vrn) /* IMIV-114 */
{
    ParamBlockRec pbr;

    pbr.fileParam.ioNamePtr = (StringPtr)filen;
    pbr.fileParam.ioVRefNum = vrn;
    pbr.fileParam.ioFVersNum = 0;
    return (PBSetFLock(&pbr, 0));
}

OSErr Executor::RstFLock(ConstStringPtr filen, INTEGER vrn) /* IMIV-114 */
{
    ParamBlockRec pbr;

    pbr.fileParam.ioNamePtr = (StringPtr)filen;
    pbr.fileParam.ioVRefNum = vrn;
    pbr.fileParam.ioFVersNum = 0;
    return (PBRstFLock(&pbr, 0));
}

OSErr Executor::Rename(ConstStringPtr filen, INTEGER vrn,
                       ConstStringPtr newf) /* IMIV-114 */
{
    ParamBlockRec pbr;

    pbr.ioParam.ioNamePtr = (StringPtr)filen;
    pbr.ioParam.ioVRefNum = vrn;
    pbr.ioParam.ioVersNum = 0;
    pbr.ioParam.ioMisc = guest_cast<LONGINT>(newf);
    return (PBRename(&pbr, 0));
}


OSErr Executor::Create(ConstStringPtr filen, INTEGER vrn, OSType creator,
                       OSType filtyp) /* IMIV-112 */
{
    ParamBlockRec pbr;
    OSErr temp;
    GUEST<ULONGINT> t;

    pbr.fileParam.ioNamePtr = (StringPtr)filen;
    pbr.fileParam.ioVRefNum = vrn;
    pbr.fileParam.ioFVersNum = 0;

    temp = PBCreate(&pbr, 0);
    if(temp != noErr)
        return (temp);

    pbr.fileParam.ioFlFndrInfo.fdType = filtyp;
    pbr.fileParam.ioFlFndrInfo.fdCreator = creator;
    pbr.fileParam.ioFlFndrInfo.fdFlags = 0;
    pbr.fileParam.ioFlFndrInfo.fdLocation = Point{0,0};
    pbr.fileParam.ioFlFndrInfo.fdFldr = 0;

    GetDateTime(&t);
    pbr.fileParam.ioFlCrDat = t;
    pbr.fileParam.ioFlMdDat = t;

    temp = PBSetFInfo(&pbr, 0);
    /*
 * The dodge below of not returning fnfErr is necessary because people might
 * want to create a file in a directory without a .Rsrc.  This has some
 * unpleasant side effects (notably no file type and no logical eof), but we
 * allow it anyway (for now).
 */
    return temp == fnfErr ? noErr : temp;
}

OSErr Executor::FSDelete(ConstStringPtr filen, INTEGER vrn) /* IMIV-113 */
{
    ParamBlockRec pbr;

    pbr.fileParam.ioNamePtr = (StringPtr)filen;
    pbr.fileParam.ioVRefNum = vrn;
    pbr.fileParam.ioFVersNum = 0;
    return (PBDelete(&pbr, 0));
}


OSErr Executor::PBHOpenDeny(HParmBlkPtr pb, Boolean a) /* IMV-397 */
{ /* HACK */
    HParamBlockRec block;
    OSErr retval;

    block.ioParam.ioCompletion = pb->ioParam.ioCompletion;
    block.ioParam.ioNamePtr = pb->ioParam.ioNamePtr;
    block.ioParam.ioVRefNum = pb->ioParam.ioVRefNum;
    block.ioParam.ioVersNum = 0;
    switch(pb->ioParam.ioPermssn & 3)
    {
        case 0:
            block.ioParam.ioPermssn = fsCurPerm;
            break;
        case 1:
            block.ioParam.ioPermssn = fsRdPerm;
            break;
        case 2:
            block.ioParam.ioPermssn = fsWrPerm;
            break;
        case 3:
            block.ioParam.ioPermssn = fsRdWrPerm;
            break;
    }
    block.ioParam.ioMisc = 0;
    block.fileParam.ioDirID = pb->fileParam.ioDirID;

    retval = PBHOpen(&block, a);
    pb->ioParam.ioResult = block.ioParam.ioResult;
    pb->ioParam.ioRefNum = block.ioParam.ioRefNum;
    fs_err_hook(retval);
    return retval;
}
