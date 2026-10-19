/* Copyright 1994, 1995, 1996 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* AppleEvents implementation */

#include <base/common.h>

#include <EventMgr.h>
#include <NotifyMgr.h>
#include <AppleEvents.h>
#include <MemoryMgr.h>
#include <ProcessMgr.h>
#include <OSEvent.h>
#include <OSUtil.h>
#include <ProcessMgr.h>
#include <AliasMgr.h>

#include <mman/mman.h>
#include <commandline/flags.h>
#include <base/functions.impl.h>
#include <appleevent/apple_events.h>

namespace Executor
{
bool send_application_open_aevt_p;
}
using namespace Executor;

int16_t Executor::AE_generate_return_id(void)
{
    static uint16_t next_return_id = 0;

    do
        next_return_id++;
    while(next_return_id == 0 || next_return_id == 0xFFFF);
    return (int16_t)next_return_id;
}

/* --------------------------- current event state -------------------------- */

namespace
{
struct current_event_state_t
{
    AppleEvent event;
    bool valid;
    AEEventSource source;
};

AppleEvent current_event;
bool current_event_valid;
AEEventSource current_event_source = kAEUnknownSource;

AEInteractionAllowed interaction_level = kAEInteractWithSelf;

void dispose_current_event(void)
{
    if(current_event_valid)
    {
        AEDisposeDesc(&current_event);
        current_event_valid = false;
    }
    current_event_source = kAEUnknownSource;
}

void set_current_event(const AppleEvent *evt, AEEventSource source)
{
    dispose_current_event();

    if(evt && evt->descriptorType == typeAppleEvent)
    {
        if(AEDuplicateDesc((AEDesc *)evt, &current_event) == noErr)
        {
            current_event_valid = true;
            current_event_source = source;
        }
    }
}

void save_current_event(current_event_state_t *saved)
{
    saved->valid = false;
    saved->source = current_event_source;
    saved->event.descriptorType = typeNull;
    saved->event.dataHandle = nullptr;
    if(current_event_valid
       && AEDuplicateDesc(&current_event, &saved->event) == noErr)
        saved->valid = true;
}

void restore_current_event(const current_event_state_t *saved)
{
    dispose_current_event();
    if(saved->valid)
    {
        current_event = saved->event;
        current_event_valid = true;
        current_event_source = saved->source;
    }
}

int16_t new_return_id(void)
{
    return AE_generate_return_id();
}

OSErr put_int16_attribute(AppleEvent *evt, AEKeyword keyword, int16_t value)
{
    GUEST<int16_t> v = value;

    return AEPutAttributePtr(evt, keyword, typeSInt16, &v, sizeof v);
}

bool has_param(AERecord *rec, AEKeyword keyword)
{
    GUEST<DescType> type;
    GUEST<Size> size;

    return AESizeOfParam(rec, keyword, &type, &size) == noErr;
}

OSErr put_error_number(AppleEvent *evt, OSErr err)
{
    GUEST<int32_t> v = err;

    if(has_param(evt, keyErrorNumber))
        return noErr;

    return AEPutParamPtr(evt, keyErrorNumber, typeSInt32, &v, sizeof v);
}

void get_reply_ids(const AppleEvent *evt, int16_t *return_id_out,
                   int32_t *transaction_id_out)
{
    GUEST<DescType> type;
    GUEST<Size> size;
    GUEST<int16_t> rid;
    GUEST<int32_t> tid;

    *return_id_out = kAutoGenerateReturnID;
    *transaction_id_out = kAnyTransactionID;

    if(AEGetAttributePtr((AppleEvent *)evt, keyReturnIDAttr, typeSInt16,
                         &type, &rid, sizeof rid, &size)
       == noErr)
        *return_id_out = rid;

    if(AEGetAttributePtr((AppleEvent *)evt, keyTransactionIDAttr, typeSInt32,
                         &type, &tid, sizeof tid, &size)
       == noErr)
        *transaction_id_out = tid;
}

OSErr ensure_return_id(AppleEvent *evt)
{
    GUEST<DescType> type;
    GUEST<Size> size;
    GUEST<int16_t> rid;

    if(AEGetAttributePtr(evt, keyReturnIDAttr, typeSInt16, &type,
                         &rid, sizeof rid, &size)
           == noErr
       && rid != 0 && rid != kAutoGenerateReturnID)
        return noErr;

    return put_int16_attribute(evt, keyReturnIDAttr, new_return_id());
}

OSErr create_reply_event(const AppleEvent *evt, AppleEvent *reply)
{
    ProcessSerialNumber psn;
    AEDesc target;
    int16_t return_id;
    int32_t transaction_id;
    OSErr err;

    GetCurrentProcess(&psn);
    err = AECreateDesc(typeProcessSerialNumber, (Ptr)&psn, sizeof psn,
                       &target);
    if(err != noErr)
        return err;

    get_reply_ids(evt, &return_id, &transaction_id);
    err = AECreateAppleEvent(kCoreEventClass, kAEAnswer, &target,
                             return_id, transaction_id, reply);
    AEDisposeDesc(&target);
    return err;
}

OSErr post_apple_event(const AppleEvent *evt, AEEventClass event_class,
                       AEEventID event_id)
{
    EventRecord evt_rec;
    Handle data;
    int size;

    data = evt->dataHandle;
    if(data == nullptr)
        return noErr;

    size = GetHandleSize(data);

    evt_rec.what = kHighLevelEvent;
    evt_rec.message = event_class;
    evt_rec.when = 0;
    evt_rec.where.v = (int16_t)(event_id >> 16);
    evt_rec.where.h = (int16_t)(event_id & 0xFFFF);
    evt_rec.modifiers = 0;

    {
        HLockGuard guard(data);
        return PostHighLevelEvent(&evt_rec, nullptr, 0, *data, size, -1);
    }
}

OSErr call_special_handler(bool system_p, const AppleEvent *evt,
                           AppleEvent *reply)
{
    GUEST<AEEventHandlerUPP> hdlr_s;
    GUEST<int32_t> refcon_s;

    if(!ae_lookup_special_handler(keyPreDispatch, system_p,
                                  &hdlr_s, &refcon_s))
        return errAEEventNotHandled;

    AEEventHandlerUPP hdlr = hdlr_s;
    return hdlr(evt, reply, refcon_s);
}

OSErr call_event_handler(bool system_p, AEEventClass event_class,
                         AEEventID event_id, const AppleEvent *evt,
                         AppleEvent *reply)
{
    GUEST<AEEventHandlerUPP> hdlr_s;
    GUEST<int32_t> refcon_s;

    if(!ae_lookup_event_handler(event_class, event_id, system_p,
                                &hdlr_s, &refcon_s))
        return errAEEventNotHandled;

    AEEventHandlerUPP hdlr = hdlr_s;
    return hdlr(evt, reply, refcon_s);
}

/* The dispatch algorithm of Inside Macintosh: Interapplication
 * Communication, "Responding to Apple Events". */
OSErr dispatch_apple_event(const AppleEvent *evt, AEEventClass event_class,
                           AEEventID event_id, AppleEvent *reply,
                           AEEventSource source)
{
    current_event_state_t saved;
    OSErr err;

    save_current_event(&saved);
    set_current_event(evt, source);

    err = call_special_handler(false, evt, reply);
    if(err == errAEEventNotHandled)
        err = call_event_handler(false, event_class, event_id, evt, reply);
    if(err == errAEEventNotHandled)
        err = call_special_handler(true, evt, reply);
    if(err == errAEEventNotHandled)
        err = call_event_handler(true, event_class, event_id, evt, reply);
    if(err == errAEEventNotHandled)
        err = errAEEventNotHandled;

    restore_current_event(&saved);
    return err;
}

bool get_event_class_and_id(const EventRecord *evt_rec,
                            AEEventClass *class_out, AEEventID *id_out)
{
    *class_out = evt_rec->message;
    *id_out = ((uint32_t)(uint16_t)evt_rec->where.v << 16)
              | (uint16_t)evt_rec->where.h;
    return *class_out != 0 && *id_out != 0;
}
} /* namespace */

/* dispatching apple events */

OSErr Executor::C_AEProcessAppleEvent(EventRecord *evtrec)
{
    GUEST<TargetID> dummy_target_id;
    GUEST<int32_t> dummy_refcon;
    GUEST<int32_t> evt_data_size;
    GUEST<DescType> dummy_type;
    GUEST<Size> dummy_size;
    Handle evt_data;
    AppleEvent evt;
    AppleEvent reply;
    AEEventClass event_class;
    AEEventID event_id;
    int16_t return_id;
    int32_t transaction_id;
    OSErr err;

    if(evtrec == nullptr)
        return paramErr;

    evt_data_size = 0;
    err = AcceptHighLevelEvent(&dummy_target_id, &dummy_refcon,
                               nullptr, &evt_data_size);
    if(err != bufferIsSmall && err != noErr)
        return errAEEventNotHandled;

    evt_data = NewHandle(evt_data_size);
    if(MemError() != noErr)
        return MemError();

    {
        HLockGuard guard(evt_data);
        err = AcceptHighLevelEvent(&dummy_target_id, &dummy_refcon,
                                   *evt_data, &evt_data_size);
    }

    if(err != noErr)
    {
        DisposeHandle(evt_data);
        return errAEEventNotHandled;
    }

    evt.descriptorType = typeAppleEvent;
    evt.dataHandle = evt_data;

    if(!get_event_class_and_id(evtrec, &event_class, &event_id))
    {
        /* Fall back to the attributes if the event record was not filled in. */
        err = AEGetAttributePtr(&evt, keyEventClassAttr, typeType,
                                &dummy_type, &event_class,
                                sizeof event_class, &dummy_size);
        if(err == noErr)
            err = AEGetAttributePtr(&evt, keyEventIDAttr, typeType,
                                    &dummy_type, &event_id,
                                    sizeof event_id, &dummy_size);
        if(err != noErr)
        {
            AEDisposeDesc(&evt);
            return errAEEventNotHandled;
        }
    }

    /* Record where the event came from.  Executor has only one process, so
     * every event it receives is a same-process event. */
    {
        GUEST<AEEventSource> source;

        err = AEGetAttributePtr(&evt, keyEventSourceAttr, typeSInt16,
                                &dummy_type, &source, sizeof source,
                                &dummy_size);
        if(err != noErr)
        {
            GUEST<int16_t> source16 = kAESameProcess;

            AEPutAttributePtr(&evt, keyEventSourceAttr, typeSInt16,
                              &source16, sizeof source16);
        }
    }

    err = create_reply_event(&evt, &reply);
    if(err != noErr)
    {
        reply.descriptorType = typeNull;
        reply.dataHandle = nullptr;
    }

    get_reply_ids(&evt, &return_id, &transaction_id);

    err = dispatch_apple_event(&evt, event_class, event_id, &reply,
                               kAESameProcess);

    /* A handler that reports an error other than errAEEventNotHandled is
     * reporting a genuine failure; pass it back to the caller. */
    if(err != noErr && err != errAEEventNotHandled
       && reply.descriptorType == typeAppleEvent)
        put_error_number(&reply, err);

    /* Replies are not themselves answered. */
    if(event_id != kAEAnswer && return_id != 0
       && return_id != kAutoGenerateReturnID)
    {
        if(reply.descriptorType == typeAppleEvent)
        {
            if(err == errAEEventNotHandled)
                put_error_number(&reply, errAEEventNotHandled);

            post_apple_event(&reply, kCoreEventClass, kAEAnswer);
        }
    }

    if(reply.descriptorType == typeAppleEvent)
        AEDisposeDesc(&reply);
    AEDisposeDesc(&evt);

    return err;
}

OSErr Executor::C_AESend(AppleEvent *evt, AppleEvent *reply,
                         AESendMode send_mode, AESendPriority send_priority,
                         int32_t timeout, IdleUPP idle_proc,
                         EventFilterUPP filter_proc)
{
    GUEST<DescType> target_type_s;
    GUEST<Size> target_size_s;
    char target_buf[32];
    AEEventClass event_class;
    AEEventID event_id;
    int reply_mode;
    GUEST<DescType> dummy_type;
    GUEST<Size> dummy_size;
    OSErr err;

    (void)send_priority;
    (void)timeout;
    (void)idle_proc;
    (void)filter_proc;

    if(evt == nullptr || reply == nullptr
       || evt->descriptorType != typeAppleEvent)
        return paramErr;

    reply->descriptorType = typeNull;
    reply->dataHandle = nullptr;

    err = AEGetAttributePtr(evt, keyAddressAttr, typeWildCard,
                            &target_type_s, target_buf, sizeof target_buf,
                            &target_size_s);
    if(err != noErr)
        return errAEUnknownAddressType;

    err = AEGetAttributePtr(evt, keyEventClassAttr, typeType,
                            &dummy_type, &event_class,
                            sizeof event_class, &dummy_size);
    if(err != noErr)
        return err;

    err = AEGetAttributePtr(evt, keyEventIDAttr, typeType,
                            &dummy_type, &event_id,
                            sizeof event_id, &dummy_size);
    if(err != noErr)
        return err;

    reply_mode = send_mode & _kAEReplyMask;
    if(reply_mode != kAENoReply && reply_mode != kAEQueueReply
       && reply_mode != kAEWaitReply)
        return errAEUnknownSendMode;

    /* Work out whether this event is addressed to us. */
    {
        bool addressed_to_self = false;

        if(target_type_s == typeProcessSerialNumber
           && target_size_s == sizeof(ProcessSerialNumber))
        {
            ProcessSerialNumber current_psn;
            Boolean same;

            GetCurrentProcess(&current_psn);
            if(SameProcess((ProcessSerialNumber *)target_buf, &current_psn,
                           &same)
                   == noErr
               && same)
                addressed_to_self = true;
        }
        else if(target_type_s == typeApplSignature
                && target_size_s == sizeof(OSType))
        {
            ProcessSerialNumber current_psn;
            ProcessInfoRec info;
            OSType signature;

            {
                GUEST<OSType> sig_s;

                memcpy(&sig_s, target_buf, sizeof sig_s);
                signature = sig_s;
            }
            GetCurrentProcess(&current_psn);
            info.processInfoLength = sizeof info;
            if(GetProcessInformation(&current_psn, &info) == noErr
               && info.processSignature == signature)
                addressed_to_self = true;
        }
        else
            return errAEUnknownAddressType;

        if(!addressed_to_self)
        {
            /* Only one process exists.  An event addressed elsewhere cannot
             * be delivered. */
            if(target_type_s == typeApplSignature)
                return invalidConnection;
            return reply_mode == kAEWaitReply ? errAEEventNotHandled : noErr;
        }
    }

    /* kAEDontExecute asks the manager to record (not execute) the event.
     * Recording is not implemented. */
    if(send_mode & 0x2000)
        return noErr;

    /* The manager records the client's interaction preference in the
     * keyInteractLevelAttr attribute. */
    {
        int16_t interact = send_mode & _kAEInteractMask;

        if(interact == 0)
            interact = kAECanInteract;
        put_int16_attribute(evt, keyInteractLevelAttr, interact);
    }

    if(reply_mode == kAENoReply)
    {
        /* No reply is expected; make sure AEProcessAppleEvent does not post
         * one. */
        put_int16_attribute(evt, keyReturnIDAttr, 0);
        return post_apple_event(evt, event_class, event_id);
    }

    err = ensure_return_id(evt);
    if(err != noErr)
        return err;

    if(reply_mode == kAEQueueReply)
        return post_apple_event(evt, event_class, event_id);

    /* kAEWaitReply: process the event synchronously.  There is no other
     * process to wait for. */
    {
        AppleEvent reply_event;

        err = create_reply_event(evt, &reply_event);
        if(err != noErr)
            return err;

        err = dispatch_apple_event(evt, event_class, event_id,
                                   &reply_event, kAESameProcess);
        if(err == errAEEventNotHandled)
        {
            AEDisposeDesc(&reply_event);
            return err;
        }

        *reply = reply_event;
        return err;
    }
}

/* ------------------------- current event accessors ------------------------ */

OSErr Executor::C_AESuspendTheCurrentEvent(AppleEvent *evt)
{
    (void)evt;
    return noErr;
}

OSErr Executor::C_AEResumeTheCurrentEvent(
    AppleEvent *evt, AppleEvent *reply, AEEventHandlerUPP dispatcher,
    int32_t refcon)
{
    OSErr err = noErr;

    if(evt)
        set_current_event(evt, kAESameProcess);

    if(dispatcher)
        err = dispatcher(evt, reply, refcon);

    return err;
}

OSErr Executor::C_AEGetTheCurrentEvent(AppleEvent *return_evt)
{
    if(return_evt == nullptr)
        return paramErr;

    if(!current_event_valid)
    {
        return_evt->descriptorType = typeNull;
        return_evt->dataHandle = nullptr;
        return noErr;
    }

    return AEDuplicateDesc(&current_event, return_evt);
}

OSErr Executor::C_AESetTheCurrentEvent(AppleEvent *evt)
{
    if(evt == nullptr)
        return paramErr;

    set_current_event(evt, kAESameProcess);
    return noErr;
}

/* ---------------------------- user interaction ---------------------------- */

OSErr Executor::C_AESetInteractionAllowed(AEInteractionAllowed level)
{
    if(level > kAEInteractWithAll)
        return paramErr;

    interaction_level = level;
    return noErr;
}

OSErr Executor::C_AEGetInteractionAllowed(AEInteractionAllowed *level_out)
{
    if(level_out == nullptr)
        return paramErr;

    *level_out = interaction_level;
    return noErr;
}

OSErr Executor::C_AEInteractWithUser(int32_t timeout, NMRecPtr nm_req,
                                     IdleUPP idle_proc)
{
    (void)timeout;
    (void)nm_req;
    (void)idle_proc;

    if(current_event_valid)
    {
        if(interaction_level == kAEInteractWithSelf
           && current_event_source != kAESameProcess)
            return errAENoUserInteraction;

        if(interaction_level == kAEInteractWithLocal
           && current_event_source > kAELocalProcess)
            return errAENoUserInteraction;
    }

    return noErr;
}

OSErr Executor::C_AEResetTimer(AppleEvent *evt)
{
    (void)evt;
    return noErr;
}

/* ------------------------------- coercion -------------------------------- */

OSErr Executor::C_AECoercePtr(DescType data_type, Ptr data, Size data_size,
                              DescType result_type, AEDesc *desc_out)
{
    GUEST<AECoerceDescUPP> coercion_hdlr_s;
    GUEST<int32_t> refcon_s;
    GUEST<Boolean> is_desc_hdlr_s;
    Boolean is_desc_hdlr_p = false;
    OSErr err;

    if(desc_out == nullptr)
        return paramErr;

    desc_out->descriptorType = typeNull;
    desc_out->dataHandle = nullptr;

    if(result_type == typeWildCard || result_type == data_type)
        return AECreateDesc(data_type, data, data_size, desc_out);

    err = AEGetCoercionHandler(data_type, result_type,
                               &coercion_hdlr_s, &refcon_s,
                               &is_desc_hdlr_s, false);
    if(err == noErr)
        is_desc_hdlr_p = is_desc_hdlr_s;
    else
    {
        err = AEGetCoercionHandler(data_type, result_type,
                                   &coercion_hdlr_s, &refcon_s,
                                   &is_desc_hdlr_s, true);
        if(err == noErr)
            is_desc_hdlr_p = is_desc_hdlr_s;
        else
        {
            err = AE_default_coerce(data_type, data, data_size,
                                    result_type, desc_out);
            if(err == noErr || err == memFullErr)
                return err;
            return errAECoercionFail;
        }
    }

    {
        AECoerceDescUPP coercion_hdlr = coercion_hdlr_s;
        int32_t refcon = refcon_s;

        if(is_desc_hdlr_p)
        {
            descriptor_t *desc = (descriptor_t *)alloca(sizeof *desc);

            err = AECreateDesc(data_type, data, data_size, desc);
            if(err != noErr)
                return err;

            err = coercion_hdlr(desc, result_type, refcon, desc_out);
            AEDisposeDesc(desc);
        }
        else
        {
            AECoercePtrUPP ptr_coercion_hdlr((void *)coercion_hdlr);
            err = ptr_coercion_hdlr(data_type, data, data_size, result_type,
                                    refcon, desc_out);
        }
    }

    if(err != noErr)
    {
        desc_out->descriptorType = typeNull;
        desc_out->dataHandle = nullptr;
        return errAECoercionFail;
    }

    return noErr;
}

#define AEGetParamDesc AEGetParamDesc

static OSErr
parse_evt(const AppleEvent *evtp, AEDesc *desc_out)
{
    OSErr retval;
    AEDesc d;

    d.descriptorType = typeNull;
    d.dataHandle = nullptr;

    /*
   * The cast to AppleEvent * of evtp is so the compiler won't complain
   * about our discarding const.  The problem is AEGetParamDesc doesn't the
   * thing pointed to by its first argument, but the prototype doesn't show
   * this.
   */

    retval = AEGetParamDesc((AppleEvent *)evtp,
                            keyDirectObject, typeAEList, &d);
    if(retval == noErr)
    {
        LONGINT n;
        GUEST<LONGINT> n_s;

        retval = AECountItems(&d, &n_s);
        n = n_s;
        if(retval == noErr)
        {
            Handle h;
            AppParametersPtr p;

            h = NewHandle(sizeof *p + n * sizeof(FSSpec));
            if(!h || !(p = (decltype(p))*h))
                retval = MemError();
            else
            {
                LONGINT l;

                p->magic = APP_PARAMS_MAGIC;
                p->n_fsspec = n;
                for(l = 1; retval == noErr && l <= n; ++l)
                {
                    AEDesc d2;
                    GUEST<AEKeyword> keyword;

                    d2.descriptorType = typeNull;
                    d2.dataHandle = nullptr;

                    retval = AEGetNthDesc(&d, l, typeAlias, &keyword, &d2);
                    if(retval == noErr)
                    {
                        AliasHandle ah;
                        Boolean wasChanged;

                        ah = (AliasHandle)d2.dataHandle;
                        retval = ResolveAlias(nullptr, ah, &p->fsspec[l - 1],
                                              &wasChanged);
                        AEDisposeDesc(&d2);
                    }
                }
                if(retval != noErr)
                    DisposeHandle(h);
                else
                {
                    desc_out->descriptorType = "appa"_4;
                    desc_out->dataHandle = h;
                }
            }
        }
        AEDisposeDesc(&d);
    }
    return retval;
}

OSErr Executor::C_AECoerceDesc(AEDesc *desc, DescType result_type,
                               AEDesc *desc_out)
{
    GUEST<AECoerceDescUPP> coercion_hdlr_s;
    GUEST<int32_t> refcon_s;
    GUEST<Boolean> is_desc_hdlr_s;
    Boolean is_desc_hdlr_p = false;
    AECoerceDescUPP coercion_hdlr;
    int32_t refcon;
    DescType desc_type;
    OSErr err;

    if(desc == nullptr || desc_out == nullptr)
        return paramErr;

    desc_out->descriptorType = typeNull;
    desc_out->dataHandle = nullptr;

    desc_type = DESC_TYPE(desc);

    if(desc_type == "aevt"_4 && result_type == "appa"_4)
        return parse_evt(desc, desc_out);

    if(result_type == typeWildCard || result_type == desc_type)
        return AEDuplicateDesc(desc, desc_out);

    err = AEGetCoercionHandler(desc_type, result_type,
                               &coercion_hdlr_s, &refcon_s,
                               &is_desc_hdlr_s, false);
    if(err == noErr)
        is_desc_hdlr_p = is_desc_hdlr_s;
    else
    {
        err = AEGetCoercionHandler(desc_type, result_type,
                                   &coercion_hdlr_s, &refcon_s,
                                   &is_desc_hdlr_s, true);
        if(err == noErr)
            is_desc_hdlr_p = is_desc_hdlr_s;
        else
        {
            err = AE_default_coerce_desc(desc, result_type, desc_out);
            if(err == noErr || err == memFullErr)
                return err;
            return errAECoercionFail;
        }
    }

    /* swap things to a normal state */
    coercion_hdlr = coercion_hdlr_s;
    refcon = refcon_s;

    if(is_desc_hdlr_p)
        err = coercion_hdlr(desc, result_type, refcon, desc_out);
    else
    {
        Handle desc_data = DESC_DATA(desc);

        if(desc_data == nullptr)
            err = AE_default_coerce(desc_type, nullptr, 0, result_type,
                                    desc_out);
        else
        {
            HLockGuard guard(desc_data);
            AECoercePtrUPP ptr_coercion_hdlr((void *)coercion_hdlr);
            err = ptr_coercion_hdlr(desc_type, *desc_data,
                                    GetHandleSize(desc_data),
                                    result_type, refcon, desc_out);
        }
    }

    if(err != noErr)
    {
        desc_out->descriptorType = typeNull;
        desc_out->dataHandle = nullptr;
        return errAECoercionFail;
    }

    return noErr;
}

OSErr Executor::C_AEManagerInfo(AEKeyword keyword, GUEST<LONGINT> *resultp)
{
    if(resultp == nullptr)
        return paramErr;

    switch(keyword)
    {
        case keyAEVersion:
            *resultp = 0x0101; /* version 1.0.1, in 'vers' format */
            break;
        case keyAERecorderCount:
            *resultp = 0;
            break;
        default:
            *resultp = 0;
            break;
    }

    return noErr;
}

/* ------------------------- object support (no OSL) ------------------------ */

/* Executor does not include the Object Support Library.  These selectors
 * therefore report paramErr, which is what the manager's catch-all path
 * returns when no catch-all handler is installed. */

OSErr Executor::C_AEDisposeToken(AEDesc *theToken)
{
    (void)theToken;
    return paramErr;
}

OSErr Executor::C_AEResolve(AEDesc *objectSpecifier, INTEGER callbackFlags,
                            AEDesc *theToken)
{
    (void)objectSpecifier;
    (void)callbackFlags;
    (void)theToken;
    return paramErr;
}

OSErr Executor::C_AERemoveObjectAccessor(
    DescType desiredClass, DescType containerType, ProcPtr theAccessor,
    Boolean isSysHandler)
{
    (void)desiredClass;
    (void)containerType;
    (void)theAccessor;
    (void)isSysHandler;
    return paramErr;
}

OSErr Executor::C_AEInstallObjectAccessor(
    DescType desiredClass, DescType containerType, ProcPtr theAccessor,
    LONGINT refcon, Boolean isSysHandler)
{
    (void)desiredClass;
    (void)containerType;
    (void)theAccessor;
    (void)refcon;
    (void)isSysHandler;
    return paramErr;
}

OSErr Executor::C_AEGetObjectAccessor(DescType desiredClass,
                                      DescType containerType,
                                      GUEST<ProcPtr> *theAccessor,
                                      GUEST<LONGINT> *accessorRefcon,
                                      Boolean isSysHandler)
{
    (void)desiredClass;
    (void)containerType;
    (void)theAccessor;
    (void)accessorRefcon;
    (void)isSysHandler;
    return paramErr;
}

OSErr Executor::C_AECallObjectAccessor(DescType desiredClass,
                                       AEDesc *containerToken,
                                       DescType containerClass,
                                       DescType keyForm, AEDesc *keyData,
                                       AEDesc *theToken)
{
    (void)desiredClass;
    (void)containerToken;
    (void)containerClass;
    (void)keyForm;
    (void)keyData;
    (void)theToken;
    return paramErr;
}

OSErr Executor::C_AESetObjectCallbacks(ProcPtr myCompareProc,
                                       ProcPtr myCountProc,
                                       ProcPtr myDisposeTokenProc,
                                       ProcPtr myGetMarkTokenProc,
                                       ProcPtr myMarkProc,
                                       ProcPtr myAdjustMarksProc,
                                       ProcPtr myGetErrDescProc)
{
    (void)myCompareProc;
    (void)myCountProc;
    (void)myDisposeTokenProc;
    (void)myGetMarkTokenProc;
    (void)myMarkProc;
    (void)myAdjustMarksProc;
    (void)myGetErrDescProc;
    return paramErr;
}
