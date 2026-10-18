#if !defined(_RSYS_EMUSTUBS_H_)
#define _RSYS_EMUSTUBS_H_

/*
 * Copyright 1995, 1998 by Abacus Research and Development, Inc.
 * All rights reserved.
 *
 */

#include <ExMacTypes.h>
#include <DialogMgr.h>
#include <MemoryMgr.h>

namespace Executor
{
struct adbop_t
{
    GUEST_STRUCT;
    GUEST<Ptr> buffer;
    GUEST<ProcPtr> proc;
    GUEST<Ptr> data;
};

typedef struct comm_toolbox_dispatch_args
{
    GUEST_STRUCT;
    GUEST<int16_t> selector;

    union {
        struct
        {
            GUEST<int16_t> n_items;
            GUEST<DialogPtr> dp;
        } shorten_args;
        struct
        {
            GUEST<DITLMethod> method;
            GUEST<Handle> new_items_h;
            GUEST<DialogPtr> dp;
        } append_args;
        struct
        {
            GUEST<DialogPtr> dp;
        } count_args;
        struct
        {
            GUEST<QElemPtr> qp;
        } crm_args;
    } args;
} comm_toolbox_dispatch_args_t;

typedef void *voidptr;

struct initzonehiddenargs_t
{
    GUEST_STRUCT;
    GUEST<voidptr> startPtr;
    GUEST<voidptr> limitPtr;
    GUEST<INTEGER> cMoreMasters;
    GUEST<GrowZoneUPP> pGrowZone;
};

extern void ROMlib_reset_bad_trap_addresses(void);

static_assert(sizeof(adbop_t) == 12);
static_assert(sizeof(comm_toolbox_dispatch_args_t) == 12);
static_assert(sizeof(initzonehiddenargs_t) == 14);
}
#endif
