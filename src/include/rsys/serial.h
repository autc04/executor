/*
 * Copyright 1998 by Abacus Research and Development, Inc.
 * All rights reserved.
 *
 */

#if !defined(_SERIAL_H_)

#include <MemoryMgr.h>
#include <DeviceMgr.h>

#define _SERIAL_H_

namespace Executor
{
struct sersetbuf_t
{
    GUEST_STRUCT;
    GUEST<Ptr> p;
    GUEST<INTEGER> i;
};

static_assert(sizeof(sersetbuf_t) == 6);

void InitSerialDriver();
}
#endif
