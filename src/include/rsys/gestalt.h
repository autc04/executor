/*
 * Copyright 1996-1999 by Abacus Research and Development, Inc.
 * All rights reserved.
 *
 */
#pragma once

#include <ExMacTypes.h>


namespace Executor
{
enum
{
    DONGLE_GESTALT = 0xb7d20e84,
};

extern void replace_physgestalt_selector(OSType selector, uint32_t new_value);
extern void ROMlib_clear_gestalt_list(void);
extern void ROMlib_add_to_gestalt_list(OSType selector, OSErr retval,
                                       uint32_t new_value);

extern OSErr C_GestaltTablesOnly(OSType selector,
                                         GUEST<LONGINT> *responsep);

extern void gestalt_set_system_version(uint32_t version);
extern void gestalt_set_memory_size(uint32_t size);

}
