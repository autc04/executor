/* Copyright 1986, 1989, 1990 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <base/common.h>

#include <ProcessMgr.h>
#include <ResourceMgr.h>
#include <MemoryMgr.h>
#include <ToolboxEvent.h>

#include <mman/mman.h>
#include <rsys/process.h>

using namespace Executor;

#define declare_handle_type(type_prefix)        \
    typedef type_prefix##_t *type_prefix##_ptr; \
    typedef GUEST<type_prefix##_ptr> *type_prefix##_handle

declare_handle_type(size_resource);

#define SIZE_FLAGS(size) ((*size)->flags)

static size_resource_handle
get_size_resource()
{
    Handle size;

    size = Get1Resource("SIZE"_4, 0);
    if(size == nullptr)
        size = Get1Resource("SIZE"_4, -1);
    return (size_resource_handle)size;
}

#define PSN_EQ_P(psn0, psn1)                      \
    ((psn0).highLongOfPSN == (psn1).highLongOfPSN \
     && (psn0).lowLongOfPSN == (psn1).lowLongOfPSN)

#define PROCESS_INFO_SERIAL_NUMBER(info) ((info)->processNumber)
#define PROCESS_INFO_LAUNCHER(info) ((info)->processLauncher)

#define PROCESS_INFO_LENGTH(info) ((info)->processInfoLength)
#define PROCESS_INFO_NAME(info) ((info)->processName)
#define PROCESS_INFO_TYPE(info) ((info)->processType)
#define PROCESS_INFO_SIGNATURE(info) ((info)->processSignature)
#define PROCESS_INFO_MODE(info) ((info)->processMode)
#define PROCESS_INFO_LOCATION(info) ((info)->processLocation)
#define PROCESS_INFO_SIZE(info) ((info)->processSize)
#define PROCESS_INFO_FREE_MEM(info) ((info)->processFreeMem)
#define PROCESS_INFO_LAUNCH_DATE(info) ((info)->processLaunchDate)
#define PROCESS_INFO_ACTIVE_TIME(info) ((info)->processActiveTime)



typedef struct process_info
{
    struct process_info *next;

    uint32_t mode;
    uint32_t type;
    uint32_t signature;
    uint32_t size;
    uint32_t launch_ticks;

    ProcessSerialNumber serial_number;
} process_info_t;

static process_info_t *process_info_list;
static process_info_t *current_process_info;

static const int default_process_mode_flags = 0;

/* ### not currently used */
static ProcessSerialNumber system_process = { 0, kSystemProcess };
static ProcessSerialNumber no_process = { 0, kNoProcess };
static ProcessSerialNumber current_process = { 0, kCurrentProcess };

void Executor::process_create(bool desk_accessory_p,
                              uint32_t type, uint32_t signature)
{
    size_resource_handle size;
    process_info_t *info;
    static uint32_t next_free_psn = 4;

    size = get_size_resource();

    {
        TheZoneGuard guard(LM(SysZone));

        info = (process_info_t *)NewPtr(sizeof *info);
    }

    /* ### we are seriously fucked */
    if(info == nullptr)
        gui_fatal("unable to allocate process info record");

    info->mode = ((size
                       ? toHost(SIZE_FLAGS(size))
                       : default_process_mode_flags)
                  | (desk_accessory_p
                         ? modeDeskAccessory
                         : 0));
    info->type = type;
    info->signature = signature;

    /* ### fixme; major bogosity */
    info->size = (zone_size(LM(ApplZone))
                  /* + A5 world size */
                  /* + stack size */);
    info->launch_ticks = TickCount();

    info->serial_number.highLongOfPSN = -1;
    info->serial_number.lowLongOfPSN = next_free_psn++;

    info->next = process_info_list;
    process_info_list = info;

    /* ### hack */
    current_process_info = info;
}

process_info_t *
get_process_info(ProcessSerialNumber *serial_number)
{
    process_info_t *t;

    if(PSN_EQ_P(*serial_number, current_process))
        return current_process_info;

    for(t = process_info_list; t; t = t->next)
    {
        if(PSN_EQ_P(*serial_number, t->serial_number))
            return t;
    }
    return nullptr;
}

OSErr Executor::C_GetCurrentProcess(ProcessSerialNumber *serial_number)
{
    *serial_number = current_process_info->serial_number;
    return noErr;
}

OSErr Executor::C_GetNextProcess(ProcessSerialNumber *serial_number)
{
    process_info_t *t;

    if(PSN_EQ_P(*serial_number, no_process))
    {
        if(!process_info_list)
            return procNotFound;
        else
        {
            *serial_number = process_info_list->serial_number;
            return noErr;
        }
    }

    t = get_process_info(serial_number);
    if(t == nullptr)
        return paramErr;
    else if(t->next == nullptr)
    {
        memset(serial_number, 0, sizeof *serial_number);
        return procNotFound;
    }
    else
    {
        *serial_number = t->next->serial_number;
        return noErr;
    }
}

OSErr Executor::C_GetProcessInformation(ProcessSerialNumber *serial_number,
                                        ProcessInfoPtr process_info)
{
    process_info_t *info;
    int32_t current_ticks;

    info = get_process_info(serial_number);
    if(info == nullptr
       || PROCESS_INFO_LENGTH(process_info) != sizeof *process_info)
        return paramErr;

    PROCESS_INFO_SERIAL_NUMBER(process_info) = info->serial_number;
    PROCESS_INFO_TYPE(process_info) = info->type;
    PROCESS_INFO_SIGNATURE(process_info) = info->signature;
    PROCESS_INFO_MODE(process_info) = info->mode;
    PROCESS_INFO_LOCATION(process_info) = guest_cast<Ptr>(LM(ApplZone));
    PROCESS_INFO_SIZE(process_info) = info->size;

    /* ### set current zone to applzone? */
    PROCESS_INFO_FREE_MEM(process_info) = FreeMem();

    PROCESS_INFO_LAUNCHER(process_info) = no_process;

    PROCESS_INFO_LAUNCH_DATE(process_info) = info->launch_ticks;
    current_ticks = TickCount();
    PROCESS_INFO_ACTIVE_TIME(process_info)
        = current_ticks - info->launch_ticks;

    return noErr;
}

OSErr Executor::C_SameProcess(ProcessSerialNumber *serial_number0,
                              ProcessSerialNumber *serial_number1,
                              Boolean *same_out)
{
    process_info_t *info0, *info1;

    info0 = get_process_info(serial_number0);
    info1 = get_process_info(serial_number1);

    if(info0 == nullptr
       || info1 == nullptr)
        return paramErr;

    *same_out = (info0 == info1);
    return noErr;
}

OSErr Executor::C_GetFrontProcess(ProcessSerialNumber *serial_number, int32_t dummy)
{
    *serial_number = current_process_info->serial_number;
    return noErr;
}

OSErr Executor::C_SetFrontProcess(ProcessSerialNumber *serial_number)
{
    warning_unimplemented("");
    return paramErr;
}

OSErr Executor::C_WakeUpProcess(ProcessSerialNumber *serial_number)
{
    warning_unimplemented("");
    return paramErr;
}

OSErr Executor::C_GetProcessSerialNumberFromPortName(
    PPCPortPtr port_name, ProcessSerialNumber *serial_number)
{
    warning_unimplemented("");
    return paramErr;
}

OSErr Executor::C_GetPortNameFromProcessSerialNumber(
    PPCPortPtr port_name, ProcessSerialNumber *serial_number)
{
    warning_unimplemented("");
    return paramErr;
}

/* ### temp memory spew; these go elsewhere */

/* ### launch/da spew; these go elsewhere */
