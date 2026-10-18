#if !defined(_RSYS_ADB_H_)
#define _RSYS_ADB_H_

/*
 * Copyright 1995 by Abacus Research and Development, Inc.
 * All rights reserved.
 *
 */

#include <base/traps.h>


namespace Executor
{
extern void adb_apeiron_hack();
extern void reset_adb_vector(void);
}
#endif
