#if !defined(__PACKAGE__)
#define __PACKAGE__

/*
 * Copyright 1986, 1989, 1990, 1996 by Abacus Research and Development, Inc.
 * All rights reserved.
 *
 */

#include <base/lowglobals.h>

#define MODULE_NAME Package
#include <base/api-module.h>

namespace Executor
{
enum
{
    dskInit = 2,
    stdFile = 3,
    flPoint = 4,
    trFunc = 5,
    intUtil = 6,
    bdConv = 7,
};

const LowMemGlobal<Handle[8]> AppPacks { 0xAB8 }; // PackageMgr ThinkC (true-b);

extern void C_InitPack(INTEGER packid);
PASCAL_TRAP(InitPack, 0xA9E5);

extern void C_InitAllPacks(void);
PASCAL_TRAP(InitAllPacks, 0xA9E6);

BEGIN_EXECUTOR_ONLY
RAW_68K_TRAP(Pack1, 0xA9E8);
END_EXECUTOR_ONLY
}
#endif /* __PACKAGE__ */
