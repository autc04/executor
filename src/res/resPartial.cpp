/* Copyright 1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <base/common.h>

#include <ResourceMgr.h>
#include <FileMgr.h>
#include <res/resource.h>
#include <file/file.h>

using namespace Executor;

void Executor::C_ReadPartialResource(Handle res, int32_t offset, Ptr buffer,
                                     int32_t count)
{
    resmaphand map;
    typref *tr;
    resref *rr;
    OSErr err;

    err = ROMlib_findres(res, &map, &tr, &rr);
    if(err == noErr)
    {
        if(resource_not_written_p(rr))
            err = resNotFound;
        else
        {
            LONGINT cur_size;
            LONGINT loc;

            cur_size = ROMlib_SizeResource(res, false);
            err = LM(ResErr);
            if(err == noErr && (uint32_t)offset + count > (uint32_t)cur_size)
                err = inputOutOfBounds;
            else
            {
                INTEGER rn;

                rn = (*map)->resfn;
                loc = ((*map)->rh.rdatoff + B3TOLONG(rr->doff)
                       + sizeof(Size) + (uint32_t)offset);
                err = SetFPos(rn, fsFromStart, loc);
                if(err == noErr)
                {
                    LONGINT lcount;

                    lcount = count;
                    err = FSReadAll(rn, inout(lcount), buffer);
                    if(err == noErr)
                        if(*res)
                            err = resourceInMemory;
                }
            }
        }
    }
    ROMlib_setreserr(err);
}

void Executor::C_WritePartialResource(Handle resource, int32_t offset,
                                      Ptr buffer, int32_t count)
{
    warning_unimplemented("");
}

void Executor::C_SetResourceSize(Handle resource, int32_t size)
{
    warning_unimplemented("");
}

/* Word6 has this lovely code in it

0xe49a6:	beqs 0xe49ba
0xe49a8:	subqw #4,sp
0xe49aa:	movel fp@(12),sp@-
0xe49ae:	moveq #10,d0
0xe49b0:	movel sp,fp@
0xe49b2:	_ResourceDispatch
0xe49b4:	movel sp@+,fp@(16)
0xe49b8:	bras 0xe49be

**************************************/

Handle Executor::C_GetNextFOND(Handle fondHandle)
{
    Handle retval;
    warning_unimplemented("");

    retval = 0;
    return retval;
}
