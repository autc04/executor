#if !defined(_RSYS_ADB_H_)
#define _RSYS_ADB_H_

/*
 * Copyright 1995 by Abacus Research and Development, Inc.
 * All rights reserved.
 *
 */

#include <base/traps.h>

#define MODULE_NAME rsys_adb
#include <base/api-module.h>

namespace Executor
{
extern void adb_apeiron_hack(int /*bool*/ deltas_p, ...);
extern void reset_adb_vector(void);
}
#endif
