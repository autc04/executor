/* Copyright 1986, 1989, 1990, 1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <base/common.h>

#include <AppleEvents.h>
#include <MemoryMgr.h>
#include <SegmentLdr.h>

#include <error/system_error.h>
#include <mman/mman.h>
#include "apple_events.h"

using namespace Executor;

/*
 * The handler tables are internal to the Apple Event Manager.  The pack uses
 * an open-addressed hash table (see appendix A of the spec); nothing outside
 * the manager inspects one, so this implementation uses a simple sorted-by-
 * insertion array instead.  Only the ABI arguments (the four zone tables and
 * the private selectors) are observable.
 */

#define AE_TABLE_SIZE_INFO(table) ((*table)->pad_1)
#define AE_TABLE_ELT_FLAGS(elt) ((elt).pad_1)
#define AE_TABLE_ELT_PAD2(elt) ((elt).pad_2)

#define AE_HDLRTABLE_ELT_FLAG_FROM_TYPE_IS_DESC 0x00000001

inline AE_zone_tables_h get_zone_tables(bool system_p)
{
    auto info = LM(AE_info);
    return system_p ? info->system_zone_tables : info->appl_zone_tables;
}
#define hdlr_table(system_p, class) (*get_zone_tables(system_p))->class##_hdlr_table

static OSErr alloc_hdlr_table(GUEST<AE_hdlr_table_h> *table_return,
                             int32_t capacity)
{
    AE_hdlr_table_h table;
    int initial_size;

    /* The hint is an expected size, not a limit.  Keep a little headroom. */
    initial_size = capacity > 0 ? capacity : 8;
    if(initial_size > 1024)
        initial_size = 1024;

    table = (AE_hdlr_table_h)NewHandleClear(
        sizeof(AE_hdlr_table_t) + initial_size * sizeof(AE_hdlr_table_elt_t));
    if(LM(MemErr) != noErr)
        return LM(MemErr);

    (*table)->n_allocated_bytes
        = sizeof(AE_hdlr_table_t) + initial_size * sizeof(AE_hdlr_table_elt_t);
    (*table)->n_elts = 0;

    *table_return = table;
    return noErr;
}

OSErr Executor::C__AE_hdlr_table_alloc(GUEST<AE_hdlr_table_h> *table_return,
                                       uint8_t system_zone,
                                       const void *imports, int32_t sizes,
                                       int32_t capacity_hint)
{
    OSErr err;

    (void)imports;

    if(sizes != 0x00080008)
        return paramErr; /* every observed caller uses 8-byte keys and values */

    if(system_zone)
    {
        TheZoneGuard guard(LM(SysZone));
        err = alloc_hdlr_table(table_return, capacity_hint);
    }
    else
        err = alloc_hdlr_table(table_return, capacity_hint);

    return err;
}

OSErr Executor::C__AE_hdlr_table_free(const void *imports,
                                      GUEST<AE_hdlr_table_h> *table)
{
    (void)imports;

    if(table && *table)
    {
        DisposeHandle((Handle)*table);
        *table = nullptr;
    }
    return noErr;
}

static int32_t hdlr_table_n_elts(AE_hdlr_table_h table)
{
    if(!table)
        return 0;
    return (*table)->n_elts;
}

static AE_hdlr_table_elt_t *hdlr_table_elt_ptr(AE_hdlr_table_h table, int index)
{
    return &(*table)->elts[index];
}

static OSErr hdlr_table_find(AE_hdlr_table_h table,
                             const AE_hdlr_selector_t *key,
                             AE_hdlr_table_elt_t **elt_out)
{
    int i, n;

    if(!table)
        return paramErr;

    n = hdlr_table_n_elts(table);
    for(i = 0; i < n; i++)
    {
        AE_hdlr_table_elt_t *elt = hdlr_table_elt_ptr(table, i);

        if(elt->selector.sel0 == key->sel0
           && elt->selector.sel1 == key->sel1)
        {
            *elt_out = elt;
            return noErr;
        }
    }
    return errAEKeyNotFound;
}

static OSErr hdlr_table_append(AE_hdlr_table_h table,
                               const AE_hdlr_selector_t *key,
                               AE_hdlr_table_elt_t **elt_out)
{
    AE_hdlr_table_elt_t *elt;
    int n;

    if(!table)
        return paramErr;

    n = hdlr_table_n_elts(table);
    SetHandleSize((Handle)table,
                  sizeof(AE_hdlr_table_t)
                      + (n + 1) * sizeof(AE_hdlr_table_elt_t));
    if(LM(MemErr) != noErr)
        return LM(MemErr);

    (*table)->n_allocated_bytes
        = sizeof(AE_hdlr_table_t)
          + (n + 1) * sizeof(AE_hdlr_table_elt_t);
    (*table)->n_elts = n + 1;

    elt = hdlr_table_elt_ptr(table, n);
    memset(elt, 0, sizeof *elt);
    elt->selector = *key;
    *elt_out = elt;
    return noErr;
}

OSErr Executor::C__AE_hdlr_install(AE_hdlr_t *hdlr,
                                   AE_hdlr_selector_t *key,
                                   const void *imports,
                                   AE_hdlr_table_h table)
{
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    (void)imports;

    if(!hdlr || !key)
        return paramErr;

    err = hdlr_table_find(table, key, &elt);
    if(err == noErr)
        elt->hdlr = *hdlr;
    else
    {
        err = hdlr_table_append(table, key, &elt);
        if(err != noErr)
            return err;
        elt->hdlr = *hdlr;
    }
    return noErr;
}

OSErr Executor::C__AE_hdlr_install_unique(AE_hdlr_t *hdlr,
                                          AE_hdlr_selector_t *key,
                                          const void *imports,
                                          AE_hdlr_table_h table)
{
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    (void)imports;

    if(!hdlr || !key)
        return paramErr;

    err = hdlr_table_find(table, key, &elt);
    if(err == noErr)
        return errAEDuplicateKey;

    err = hdlr_table_append(table, key, &elt);
    if(err != noErr)
        return err;
    elt->hdlr = *hdlr;
    return noErr;
}

OSErr Executor::C__AE_hdlr_delete(AE_hdlr_selector_t *key,
                                  const void *imports,
                                  AE_hdlr_table_h table)
{
    AE_hdlr_table_elt_t *elt;
    int i, n;
    OSErr err;

    (void)imports;

    if(!key)
        return paramErr;

    err = hdlr_table_find(table, key, &elt);
    if(err != noErr)
        return noErr; /* deleting an absent key succeeds */

    i = elt - hdlr_table_elt_ptr(table, 0);
    n = hdlr_table_n_elts(table);
    if(i + 1 < n)
        memmove(elt, elt + 1, (n - i - 1) * sizeof *elt);
    (*table)->n_elts = n - 1;
    return noErr;
}

OSErr Executor::C__AE_hdlr_lookup(AE_hdlr_t *result,
                                  AE_hdlr_selector_t *key,
                                  const void *imports,
                                  AE_hdlr_table_h table)
{
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    (void)imports;

    if(!result || !key)
        return paramErr;

    err = hdlr_table_find(table, key, &elt);
    if(err != noErr)
        return err;
    *result = elt->hdlr;
    return noErr;
}

OSErr Executor::C__AE_hdlr_get_key(AE_hdlr_selector_t *key, int32_t index,
                                   const void *imports,
                                   AE_hdlr_table_h table)
{
    (void)imports;

    if(!key)
        return paramErr;

    if(index < 1 || index > hdlr_table_n_elts(table))
        return errAEIndexOutOfRange;

    *key = hdlr_table_elt_ptr(table, index - 1)->selector;
    return noErr;
}

/* -------------------------- zone initialisation -------------------------- */

void Executor::AE_init(void)
{
    AE_info_t *info;
    AE_zone_tables_h zone_tables;

    info = (AE_info_t *)NewPtrSysClear(sizeof *info);
    if(info == nullptr)
        return;

    TheZoneGuard guard(LM(SysZone));

    zone_tables = (AE_zone_tables_h)NewHandleClear(sizeof(AE_zone_tables_t));

    _AE_hdlr_table_alloc(&(*zone_tables)->event_hdlr_table, 1, nullptr,
                         0x00080008, 8);
    _AE_hdlr_table_alloc(&(*zone_tables)->coercion_hdlr_table, 1, nullptr,
                         0x00080008, 8);
    _AE_hdlr_table_alloc(&(*zone_tables)->special_hdlr_table, 1, nullptr,
                         0x00080008, 8);
    info->system_zone_tables = zone_tables;

    LM(AE_info) = info;
}

void Executor::AE_reinit(void)
{
    AE_info_t *info;
    AE_zone_tables_h zone_tables;

    info = LM(AE_info);
    if(info == nullptr)
    {
        AE_init();
        return;
    }

    TheZoneGuard guard(LM(ApplZone));

    zone_tables = (AE_zone_tables_h)NewHandleClear(sizeof(AE_zone_tables_t));

    _AE_hdlr_table_alloc(&(*zone_tables)->event_hdlr_table, 0, nullptr,
                         0x00080008, 8);
    _AE_hdlr_table_alloc(&(*zone_tables)->coercion_hdlr_table, 0, nullptr,
                         0x00080008, 8);
    _AE_hdlr_table_alloc(&(*zone_tables)->special_hdlr_table, 0, nullptr,
                         0x00080008, 8);
    info->appl_zone_tables = zone_tables;
}

/* ----------------------------- event handlers ---------------------------- */

bool Executor::application_accepts_open_app_aevt_p;

static bool key_matches(int32_t installed, int32_t wanted)
{
    return installed == wanted || installed == typeWildCard;
}

static int key_specificity(int32_t installed_class, int32_t installed_id)
{
    return (installed_class == typeWildCard ? 0 : 2)
           + (installed_id == typeWildCard ? 0 : 1);
}

/* Find the most specific entry matching class/id.  Exact entries win over
 * class-wildcard entries, which win over id-wildcard entries, which win over
 * the both-wildcard entry. */
static bool lookup_event_handler(AEEventClass event_class, AEEventID event_id,
                                 Boolean system_p,
                                 GUEST<AEEventHandlerUPP> *handler_out,
                                 GUEST<int32_t> *refcon_out)
{
    AE_hdlr_table_h table = hdlr_table(system_p, event);
    AE_hdlr_table_elt_t *best = nullptr;
    int best_score = -1;
    int i, n;

    if(!table)
        return false;

    n = hdlr_table_n_elts(table);
    for(i = 0; i < n; i++)
    {
        AE_hdlr_table_elt_t *elt = hdlr_table_elt_ptr(table, i);
        int score;

        if(!key_matches(elt->selector.sel0, event_class)
           || !key_matches(elt->selector.sel1, event_id))
            continue;

        score = key_specificity(elt->selector.sel0, elt->selector.sel1);
        if(score > best_score)
        {
            best_score = score;
            best = elt;
        }
    }

    if(best == nullptr)
        return false;

    *handler_out = guest_cast<AEEventHandlerUPP>(best->hdlr.fn);
    *refcon_out = best->hdlr.refcon;
    return true;
}

bool Executor::ae_lookup_event_handler(AEEventClass event_class,
                                       AEEventID event_id, bool system_p,
                                       GUEST<AEEventHandlerUPP> *handler_out,
                                       GUEST<int32_t> *refcon_out)
{
    return lookup_event_handler(event_class, event_id, system_p,
                                handler_out, refcon_out);
}

OSErr Executor::C_AEInstallEventHandler(AEEventClass event_class,
                                        AEEventID event_id,
                                        AEEventHandlerUPP hdlr_fn,
                                        int32_t refcon,
                                        Boolean system_handler_p)
{
    AE_hdlr_table_h table;
    AE_hdlr_selector_t selector;
    AE_hdlr_t hdlr;
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    if(!hdlr_fn || (ptr_to_longint(hdlr_fn) & 1))
        return paramErr;

    table = hdlr_table(system_handler_p, event);

    selector.sel0 = event_class;
    selector.sel1 = event_id;

    hdlr.fn = (void *)hdlr_fn;
    hdlr.refcon = refcon;

    err = hdlr_table_find(table, &selector, &elt);
    if(err == noErr)
        elt->hdlr = hdlr;
    else
    {
        err = hdlr_table_append(table, &selector, &elt);
        if(err != noErr)
            return err;
        elt->hdlr = hdlr;
    }
    AE_TABLE_ELT_FLAGS(*elt) = 0;

    /* Some applications complain if they are sent an open-application event
     * but have not installed a handler for it. */
    application_accepts_open_app_aevt_p = true;

    return noErr;
}

static void dummy(void)
{
}

OSErr Executor::C_AEGetEventHandler(AEEventClass event_class,
                                    AEEventID event_id,
                                    GUEST<AEEventHandlerUPP> *hdlr,
                                    GUEST<int32_t> *refcon,
                                    Boolean system_handler_p)
{
    AE_hdlr_table_h table;
    AE_hdlr_selector_t selector;
    AE_hdlr_table_elt_t *elt;
    OSErr err;

#if 1
    if(event_class == "go b"_4 && event_id == "ears"_4)
        system_error("This application appears to be using AppleEvents in "
                     "a way that is not currently supported under Executor.  "
                     "You may try to continue, but in all likelihood Executor "
                     "will soon crash because of this incompatibility.",
                     0, "Exit", "Continue",
                     nullptr, C_ExitToShell, dummy, nullptr);
#endif

    table = hdlr_table(system_handler_p, event);

    selector.sel0 = event_class;
    selector.sel1 = event_id;

    /* AEGetEventHandler requires an entry with exactly these values: unlike
     * dispatching, it does not fall back to wildcard entries. */
    err = hdlr_table_find(table, &selector, &elt);
    if(err != noErr)
        return errAEHandlerNotFound;

    *hdlr = guest_cast<AEEventHandlerUPP>(elt->hdlr.fn);
    *refcon = elt->hdlr.refcon;

    application_accepts_open_app_aevt_p = true;

    return noErr;
}

OSErr Executor::C_AERemoveEventHandler(AEEventClass event_class,
                                       AEEventID event_id,
                                       AEEventHandlerUPP hdlr,
                                       Boolean system_handler_p)
{
    AE_hdlr_table_h table;
    AE_hdlr_selector_t selector;
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    table = hdlr_table(system_handler_p, event);

    selector.sel0 = event_class;
    selector.sel1 = event_id;

    err = hdlr_table_find(table, &selector, &elt);
    if(err != noErr)
        return errAEHandlerNotFound;

    if((void *)hdlr != elt->hdlr.fn)
        return errAEHandlerNotFound;

    return _AE_hdlr_delete(&selector, nullptr, table);
}

/* --------------------------- coercion handlers --------------------------- */

OSErr Executor::C_AEInstallCoercionHandler(
    DescType from_type, DescType to_type, AECoerceDescUPP hdlr_fn,
    int32_t refcon, Boolean from_type_is_desc_p, Boolean system_handler_p)
{
    AE_hdlr_table_h table;
    AE_hdlr_selector_t selector;
    AE_hdlr_t hdlr;
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    if(hdlr_fn == nullptr || (ptr_to_longint(hdlr_fn) & 1))
        return paramErr;

    table = hdlr_table(system_handler_p, coercion);

    selector.sel0 = from_type;
    selector.sel1 = to_type;

    hdlr.fn = (void *)hdlr_fn;
    hdlr.refcon = refcon;

    err = hdlr_table_find(table, &selector, &elt);
    if(err == noErr)
        elt->hdlr = hdlr;
    else
    {
        err = hdlr_table_append(table, &selector, &elt);
        if(err != noErr)
            return err;
        elt->hdlr = hdlr;
    }
    AE_TABLE_ELT_FLAGS(*elt)
        = from_type_is_desc_p ? AE_HDLRTABLE_ELT_FLAG_FROM_TYPE_IS_DESC : 0;

    return noErr;
}

OSErr Executor::C_AEGetCoercionHandler(DescType from_type, DescType to_type,
                                       GUEST<AECoerceDescUPP> *hdlr_out,
                                       GUEST<int32_t> *refcon_out,
                                       GUEST<Boolean> *from_type_is_desc_p_out,
                                       Boolean system_handler_p)
{
    AE_hdlr_table_h table;
    AE_hdlr_selector_t selector;
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    table = hdlr_table(system_handler_p, coercion);

    selector.sel0 = from_type;
    selector.sel1 = to_type;

    err = hdlr_table_find(table, &selector, &elt);
    if(err != noErr)
        return err;

    *hdlr_out = guest_cast<AECoerceDescUPP>(elt->hdlr.fn);
    *refcon_out = elt->hdlr.refcon;
    if(from_type_is_desc_p_out)
        *from_type_is_desc_p_out
            = (AE_TABLE_ELT_FLAGS(*elt)
               & AE_HDLRTABLE_ELT_FLAG_FROM_TYPE_IS_DESC)
                  ? true
                  : false;

    return noErr;
}

OSErr Executor::C_AERemoveCoercionHandler(
    DescType from_type, DescType to_type, AECoerceDescUPP hdlr,
    Boolean system_handler_p)
{
    AE_hdlr_table_h table;
    AE_hdlr_selector_t selector;
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    table = hdlr_table(system_handler_p, coercion);

    selector.sel0 = from_type;
    selector.sel1 = to_type;

    err = hdlr_table_find(table, &selector, &elt);
    if(err != noErr)
        return errAEHandlerNotFound;

    if((void *)hdlr != elt->hdlr.fn)
        return errAEHandlerNotFound;

    return _AE_hdlr_delete(&selector, nullptr, table);
}

/* ---------------------------- special handlers --------------------------- */

#define k_special_sel1 ("\000\000\000\000"_4)

static bool valid_special_keyword(AEKeyword function_class)
{
    switch(function_class)
    {
        case keyPreDispatch:
        case keySelectProc:
        case keyAECompareProc:
        case keyAECountProc:
        case keyDisposeTokenProc:
        case keyAEMarkTokenProc:
        case keyAEMarkProc:
        case keyAEAdjustMarksProc:
        case keyAEGetErrDescProc:
        case keyAEBlock:
        case keyAEUnblock:
        case keyAENavigate:
            return true;
        default:
            return false;
    }
}

OSErr Executor::C_AEInstallSpecialHandler(AEKeyword function_class,
                                          AEEventHandlerUPP hdlr_fn,
                                          Boolean system_handler_p)
{
    AE_hdlr_table_h table;
    AE_hdlr_selector_t selector;
    AE_hdlr_t hdlr;
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    if(!hdlr_fn || (ptr_to_longint(hdlr_fn) & 1))
        return paramErr;

    if(!valid_special_keyword(function_class))
        return errAENotASpecialFunction;

    table = hdlr_table(system_handler_p, special);

    selector.sel0 = function_class;
    selector.sel1 = k_special_sel1;

    hdlr.fn = (void *)hdlr_fn;
    hdlr.refcon = 0;

    err = hdlr_table_find(table, &selector, &elt);
    if(err == noErr)
        elt->hdlr = hdlr;
    else
    {
        err = hdlr_table_append(table, &selector, &elt);
        if(err != noErr)
            return err;
        elt->hdlr = hdlr;
    }
    AE_TABLE_ELT_FLAGS(*elt) = 0;

    return noErr;
}

OSErr Executor::C_AEGetSpecialHandler(AEKeyword function_class,
                                      GUEST<AEEventHandlerUPP> *hdlr_out,
                                      Boolean system_handler_p)
{
    AE_hdlr_table_h table;
    AE_hdlr_selector_t selector;
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    if(!valid_special_keyword(function_class))
        return errAENotASpecialFunction;

    table = hdlr_table(system_handler_p, special);

    selector.sel0 = function_class;
    selector.sel1 = k_special_sel1;

    err = hdlr_table_find(table, &selector, &elt);
    if(err != noErr)
        return errAEHandlerNotFound;

    *hdlr_out = guest_cast<AEEventHandlerUPP>(elt->hdlr.fn);
    return noErr;
}

OSErr Executor::C_AERemoveSpecialHandler(AEKeyword function_class,
                                         AEEventHandlerUPP hdlr,
                                         Boolean system_handler_p)
{
    AE_hdlr_table_h table;
    AE_hdlr_selector_t selector;
    AE_hdlr_table_elt_t *elt;
    OSErr err;

    if(!valid_special_keyword(function_class))
        return errAENotASpecialFunction;

    table = hdlr_table(system_handler_p, special);

    selector.sel0 = function_class;
    selector.sel1 = k_special_sel1;

    err = hdlr_table_find(table, &selector, &elt);
    if(err != noErr)
        return errAEHandlerNotFound;

    if((void *)hdlr != elt->hdlr.fn)
        return errAEHandlerNotFound;

    return _AE_hdlr_delete(&selector, nullptr, table);
}

bool Executor::ae_lookup_special_handler(AEKeyword function_class,
                                         bool system_p,
                                         GUEST<AEEventHandlerUPP> *handler_out,
                                         GUEST<int32_t> *refcon_out)
{
    AE_hdlr_table_h table = hdlr_table(system_p, special);
    AE_hdlr_selector_t selector;
    AE_hdlr_table_elt_t *elt;

    if(!table)
        return false;

    selector.sel0 = function_class;
    selector.sel1 = k_special_sel1;

    if(hdlr_table_find(table, &selector, &elt) != noErr)
        return false;

    *handler_out = guest_cast<AEEventHandlerUPP>(elt->hdlr.fn);
    *refcon_out = elt->hdlr.refcon;
    return true;
}
