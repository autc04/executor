/* Copyright 1986, 1989, 1990 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in ResourceMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <ResourceMgr.h>
#include <FileMgr.h>
#include <MemoryMgr.h>
#include <res/resource.h>
#include <file/file.h>
#include <prefs/prefs.h>

#define STEF_GetResInfoFix

using namespace Executor;

INTEGER Executor::C_UniqueID(ResType typ)
{
    static INTEGER startid = 0;
    GUEST<INTEGER> curmap;
    resmaphand map;
    resref *rr;

    curmap = LM(CurMap);
    LM(CurMap) = ((resmap *)*LM(TopMapHndl))->resfn;
    while(ROMlib_typidtop(typ, ++startid, &map, &rr) != resNotFound)
        ;
    LM(CurMap) = curmap;
    return (startid);
}

INTEGER Executor::C_Unique1ID(ResType typ) /* IMIV-16 */
{
    resmaphand map;
    static INTEGER startid = 0;
    resref *rr;

    map = ROMlib_rntohandl(LM(CurMap), (Handle *)0);
    if(!map)
    {
        ROMlib_setreserr(resFNotFound);
        /*-->*/ return 0;
    }
    while(ROMlib_maptypidtop(map, typ, ++startid, &rr) != resNotFound)
        ;
    return (startid);
}

void Executor::C_GetResInfo(Handle res, GUEST<INTEGER> *id,
                            GUEST<ResType> *typ, StringPtr name)
{
    resmaphand map;
    typref *tr;
    resref *rr;

    ROMlib_setreserr(ROMlib_findres(res, &map, &tr, &rr));

#if !defined(STEF_GetResInfoFix)
    if(LM(ResErr) != noErr)
        return;
#else
    if(LM(ResErr) != noErr)
    {
        if(id)
            *id = -1;
        if(typ)
            *typ = (ResType)0;
        if(name)
            name[0] = 0;
        /*-->*/ return;
    }
#endif
    if(id)
        *id = rr->rid;
    if(typ)
        *typ = tr->rtyp;
    if(name)
    {
        if(rr->noff != -1)
            str255assign(name,
                         (StringPtr)((char *)*map + (*map)->namoff + rr->noff));
        else
            name[0] = 0;
    }
}

INTEGER Executor::C_GetResAttrs(Handle res)
{
    resmaphand map;
    typref *tr;
    resref *rr;

    ROMlib_setreserr(ROMlib_findres(res, &map, &tr, &rr));
    if(LM(ResErr) != noErr)
        /*-->*/ return 0;
    return (rr->ratr);
}

LONGINT Executor::ROMlib_SizeResource(Handle res, Boolean usehandle)
{
    resmaphand map;
    typref *tr;
    resref *rr;
    Size retval;
    LONGINT loc, lc;

    ROMlib_setreserr(ROMlib_findres(res, &map, &tr, &rr));
    if(LM(ResErr) != noErr)
        /*-->*/ return -1;

    if(usehandle && *res)
        retval = GetHandleSize(res);
    else
    {
        loc = (*map)->rh.rdatoff + B3TOLONG(rr->doff);
        ROMlib_setreserr(SetFPos((*map)->resfn, fsFromStart, loc));
        if(LM(ResErr) != noErr)
            /*-->*/ return -1;

        /* If the resource is compressed and we want the memory size, not
	   the disk size, we have to look at more than the length on the 
	   disk */

        if(usehandle && (rr->ratr & resCompressed) && system_version >= 0x700)
        {
            GUEST<LONGINT> l[4]; /* [0] == diskSize,
			     [1] == compressedResourceTag, 
			     [2] == typeFlags,
			     [3] == uncompressedSize */
            GUEST<LONGINT> master_save_pos;

            lc = sizeof(l);
            GetFPos((*map)->resfn, &master_save_pos);
            ROMlib_setreserr(FSReadAll((*map)->resfn, inout(lc), (Ptr)l));
            if(LM(ResErr) != noErr || l[1] != COMPRESSED_TAG)
            {
                SetFPos((*map)->resfn, fsFromStart, master_save_pos);
                goto not_compressed_after_all;
            }
            else
            {
                if(l[2] != COMPRESSED_FLAGS)
                {
                    ROMlib_setreserr(CantDecompress);
                    return -1;
                }
                else
                    retval = l[3];
            }
        }
        else
        {
        not_compressed_after_all:
            lc = sizeof(retval);
            GUEST<Size> tmpRet;
            ROMlib_setreserr(FSReadAll((*map)->resfn, inout(lc), (Ptr)&tmpRet));
            retval = tmpRet;
            if(LM(ResErr) != noErr)
                /*-->*/ return -1;
        }
    }

    return retval;
}

LONGINT Executor::C_GetResourceSizeOnDisk(Handle res)
{
    /* the length stored on disk (for a compressed resource, its
       compressed size), not the loaded handle's size. Finder 7.5 compares
       this with the uncompressed size to decide whether to decompress the
       code it reads straight from its resource fork. */
    return ROMlib_SizeResource(res, false);
}
