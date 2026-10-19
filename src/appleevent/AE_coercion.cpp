/* Copyright 1986, 1989, 1990 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <base/common.h>

#include <AppleEvents.h>
#include <AliasMgr.h>
#include <MemoryMgr.h>

#include <mman/mman.h>
#include <appleevent/apple_events.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace Executor;

/* Default coercion handling provided by the Apple Event Manager.
 * See Inside Macintosh: Interapplication Communication, table 4-1.
 */

namespace
{
template<typename T>
T guest_read(const void *p)
{
    GUEST<T> g;

    memcpy(&g, p, sizeof g);
    return g;
}

template<typename T>
OSErr make_desc(DescType type, T value, AEDesc *result)
{
    GUEST<T> g = value;

    return AECreateDesc(type, &g, sizeof g, result);
}

std::string text_from_data(const void *data, Size data_size)
{
    if(data == nullptr || data_size == 0)
        return std::string();

    return std::string((const char *)data, data_size);
}

bool text_to_ldouble(const void *data, Size data_size, long double *value_out)
{
    std::string s = text_from_data(data, data_size);
    char *end = nullptr;

    if(s.empty())
        return false;

    *value_out = strtold(s.c_str(), &end);
    return end != s.c_str();
}

bool type_is_numeric(DescType type)
{
    switch(type)
    {
        case typeSInt16:
        case typeSInt32:
        case typeUInt32:
        case typeSInt64:
        case typeIEEE32BitFloatingPoint:
        case typeIEEE64BitFloatingPoint:
            return true;
        default:
            return false;
    }
}

bool numeric_from_data(DescType type, const void *data, Size data_size,
                       long double *value_out)
{
    switch(type)
    {
        case typeSInt16:
            if(data_size < 2)
                return false;
            *value_out = guest_read<int16_t>(data);
            return true;
        case typeSInt32:
            if(data_size < 4)
                return false;
            *value_out = guest_read<int32_t>(data);
            return true;
        case typeUInt32:
            if(data_size < 4)
                return false;
            *value_out = (long double)guest_read<uint32_t>(data);
            return true;
        case typeSInt64:
            if(data_size < 8)
                return false;
            *value_out = (long double)guest_read<int64_t>(data);
            return true;
        case typeIEEE32BitFloatingPoint:
            if(data_size < 4)
                return false;
            *value_out = guest_read<float>(data);
            return true;
        case typeIEEE64BitFloatingPoint:
            if(data_size < 8)
                return false;
            *value_out = guest_read<double>(data);
            return true;
        default:
            return false;
    }
}

OSErr numeric_to_desc(DescType to_type, long double value, AEDesc *result)
{
    switch(to_type)
    {
        case typeSInt16:
            return make_desc<int16_t>(to_type, (int16_t)value, result);
        case typeSInt32:
            return make_desc<int32_t>(to_type, (int32_t)value, result);
        case typeUInt32:
            return make_desc<uint32_t>(to_type, (uint32_t)value, result);
        case typeSInt64:
            return make_desc<int64_t>(to_type, (int64_t)value, result);
        case typeIEEE32BitFloatingPoint:
            return make_desc<float>(to_type, (float)value, result);
        case typeIEEE64BitFloatingPoint:
            return make_desc<double>(to_type, (double)value, result);
        default:
            return errAECoercionFail;
    }
}

OSErr number_to_text(long double value, bool integral, bool unsigned_value,
                     DescType to_type, AEDesc *result)
{
    char buf[64];
    int n;

    (void)to_type;

    if(integral)
    {
        if(unsigned_value)
            n = snprintf(buf, sizeof buf, "%llu",
                         (unsigned long long)value);
        else
            n = snprintf(buf, sizeof buf, "%lld",
                         (long long)value);
    }
    else
        n = snprintf(buf, sizeof buf, "%.10Lg", value);

    return AECreateDesc(typeChar, buf, n, result);
}
} /* namespace */

OSErr Executor::AE_default_coerce(DescType from_type, const void *data,
                                  Size data_size, DescType to_type,
                                  AEDesc *result)
{
    long double value;

    if(from_type == to_type || to_type == typeWildCard)
        return AECreateDesc(from_type, data, data_size, result);

    /* any descriptor type -> a list containing that descriptor */
    if(to_type == typeAEList)
    {
        AEDesc item;
        AEDescList list;
        OSErr err;

        list.descriptorType = typeNull;
        list.dataHandle = nullptr;

        err = AECreateDesc(from_type, data, data_size, &item);
        if(err != noErr)
            return err;

        err = AECreateList(nullptr, 0, false, &list);
        if(err == noErr)
            err = AEPutDesc(&list, 0, &item);
        AEDisposeDesc(&item);
        if(err != noErr)
        {
            AEDisposeDesc(&list);
            return err;
        }
        *result = list;
        return noErr;
    }

    /* typeChar -> numeric */
    if(from_type == typeChar && type_is_numeric(to_type))
    {
        if(!text_to_ldouble(data, data_size, &value))
            return errAECoercionFail;
        return numeric_to_desc(to_type, value, result);
    }

    /* numeric -> typeChar */
    if(type_is_numeric(from_type) && to_type == typeChar)
    {
        bool integral = (from_type != typeIEEE32BitFloatingPoint
                         && from_type != typeIEEE64BitFloatingPoint);

        if(!numeric_from_data(from_type, data, data_size, &value))
            return errAECoercionFail;
        return number_to_text(value, integral, from_type == typeUInt32,
                              to_type, result);
    }

    /* numeric -> numeric */
    if(type_is_numeric(from_type) && type_is_numeric(to_type))
    {
        if(!numeric_from_data(from_type, data, data_size, &value))
            return errAECoercionFail;
        return numeric_to_desc(to_type, value, result);
    }

    /* typeChar -> typeType / typeEnumerated */
    if(from_type == typeChar
       && (to_type == typeType || to_type == typeEnumerated))
    {
        GUEST<DescType> v;

        if(data_size < 4)
            return errAECoercionFail;
        memcpy(&v, data, 4);
        return AECreateDesc(to_type, &v, sizeof v, result);
    }

    /* typeType / typeEnumerated -> typeChar */
    if((from_type == typeType || from_type == typeEnumerated)
       && to_type == typeChar)
    {
        if(data_size < 4)
            return errAECoercionFail;
        return AECreateDesc(typeChar, data, 4, result);
    }

    /* typeTrue / typeFalse -> typeBoolean */
    if(from_type == typeTrue && to_type == typeBoolean)
    {
        uint8_t b = 1;
        return AECreateDesc(typeBoolean, &b, 1, result);
    }
    if(from_type == typeFalse && to_type == typeBoolean)
    {
        uint8_t b = 0;
        return AECreateDesc(typeBoolean, &b, 1, result);
    }

    /* typeEnumerated -> typeBoolean */
    if(from_type == typeEnumerated && to_type == typeBoolean)
    {
        DescType v;
        uint8_t b;

        if(data_size < 4)
            return errAECoercionFail;
        v = guest_read<DescType>(data);
        if(v == "true"_4)
            b = 1;
        else if(v == "fals"_4)
            b = 0;
        else
            return errAECoercionFail;
        return AECreateDesc(typeBoolean, &b, 1, result);
    }

    /* typeBoolean -> typeEnumerated */
    if(from_type == typeBoolean && to_type == typeEnumerated)
    {
        GUEST<DescType> v;

        if(data_size < 1)
            return errAECoercionFail;
        v = ((const uint8_t *)data)[0] ? "true"_4 : "fals"_4;
        return AECreateDesc(typeEnumerated, &v, sizeof v, result);
    }

    /* integer -> typeBoolean */
    if((from_type == typeSInt16 || from_type == typeSInt32)
       && to_type == typeBoolean)
    {
        uint8_t b;

        if(!numeric_from_data(from_type, data, data_size, &value))
            return errAECoercionFail;
        b = value != 0;
        return AECreateDesc(typeBoolean, &b, 1, result);
    }

    /* typeBoolean -> integer */
    if(from_type == typeBoolean
       && (to_type == typeSInt16 || to_type == typeSInt32))
    {
        if(data_size < 1)
            return errAECoercionFail;
        return numeric_to_desc(to_type, ((const uint8_t *)data)[0] ? 1 : 0,
                               result);
    }

    /* typeIntlText -> typeChar */
    if(from_type == "itxt"_4 && to_type == typeChar)
    {
        if(data_size < 6)
            return errAECoercionFail;
        return AECreateDesc(typeChar, (const char *)data + 6,
                            data_size - 6, result);
    }

    /* typeAlias -> typeFSS */
    if(from_type == typeAlias && to_type == typeFSS)
    {
        AliasHandle alias;
        FSSpec fss;
        Boolean changed;
        OSErr err;

        alias = (AliasHandle)NewHandle(data_size);
        if(alias == nullptr)
            return memFullErr;
        memcpy(*alias, data, data_size);

        err = ResolveAlias(nullptr, alias, &fss, &changed);
        DisposeHandle((Handle)alias);
        if(err != noErr)
            return errAECoercionFail;

        return AECreateDesc(typeFSS, &fss, sizeof fss, result);
    }

    return errAECoercionFail;
}

OSErr Executor::AE_default_coerce_desc(const AEDesc *desc, DescType to_type,
                                       AEDesc *result)
{
    OSErr err;

    if(desc->descriptorType == to_type || to_type == typeWildCard)
        return AEDuplicateDesc((AEDesc *)desc, result);

    /* a list with exactly one item can be coerced to that item's type */
    if(desc->descriptorType == typeAEList)
    {
        GUEST<int32_t> count;

        err = AECountItems((AEDescList *)desc, &count);
        if(err != noErr)
            return err;
        if(count != 1)
            return errAECoercionFail;

        return AEGetNthDesc((AEDescList *)desc, 1, to_type, nullptr, result);
    }

    {
        Handle h = desc->dataHandle;
        OSErr retval;

        if(h == nullptr)
            return AE_default_coerce(desc->descriptorType, nullptr, 0,
                                     to_type, result);

        HLockGuard guard(h);
        retval = AE_default_coerce(desc->descriptorType, *h,
                                   GetHandleSize(h), to_type, result);
        return retval;
    }
}
