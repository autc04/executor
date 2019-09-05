#if !defined(__BINDEC__)
#define __BINDEC__
/*
 * Copyright 1990 by Abacus Research and Development, Inc.
 * All rights reserved.
 *
 */

#include <ExMacTypes.h>

#define MODULE_NAME BinaryDecimal
#include <base/api-module.h>

namespace Executor
{
EXTERN_DISPATCHER_TRAP(Pack7, 0xA9EE, StackW);

extern void C_NumToString(LONGINT l, StringPtr s);
REGISTER_SUBTRAP(NumToString, 0xA9EE, 0x0000, Pack7, void (D0, A0));

extern void C_StringToNum(ConstStringPtr s, GUEST<LONGINT> *lp);
REGISTER_SUBTRAP(StringToNum, 0xA9EE, 0x0001, Pack7, void (A0, Out<LONGINT, D0>));

}
#endif /* __BINDEC__ */
