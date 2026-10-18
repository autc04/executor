#pragma once
#include <base/mactype.h>
#include <syn68k_public.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace Executor
{
namespace logging
{

bool enabled();
void setEnabled(bool e);

// Restrict --logtraps output to trap names matching one of the comma-separated
// wildcard patterns (e.g. "PB*,FS*,HOpen*").  Empty (or never called) = all.
void setTrapFilter(const std::string& patterns);
bool trapLogEnabled(const char* name);

void resetNestingLevel();

bool loggingActive();
void indent();

// Data-driven trap logging: the generated entrypoints pass an array of
// per-type printers (one generated function per distinct argument/return
// type, shared by every trap that uses it) plus an array of pointers to the
// values.  Keeping the per-trap part to data means --logtraps instantiates no
// template per trap.
using LogArgFn = void (*)(const void*);
void logTrapCallData(const char* name, const LogArgFn* printers, const void* const* values, unsigned count);
void logTrapValReturnData(const char* name, LogArgFn printer, const void* value,
                          const LogArgFn* printers, const void* const* values, unsigned count);
void logTrapVoidReturnData(const char* name, const LogArgFn* printers, const void* const* values, unsigned count);

extern int nestingLevel;

// RAII nesting guard used by the generated trap entrypoints and marshallers so
// that --logtraps indentation and the "suppress nested traps" rule stay
// consistent.
struct LogNestingScope
{
    bool active;
    LogNestingScope() : active(enabled()) { if(active) nestingLevel++; }
    ~LogNestingScope() { if(active) nestingLevel--; }
};

void logUntypedArgs(const char *name);
void logUntypedReturn(const char *name);

void logEscapedChar(unsigned char c);
void logEscapedCharTo(std::ostream& os, unsigned char c);
bool canConvertBack(const void* p);
bool validAddress(const void* p);
bool validAddress(syn68k_addr_t p);
void dumpRegsAndStack();

// ostream-parameterised value formatting; logValue() below is the std::clog
// convenience wrapper used by --logtraps.
void logValueTo(std::ostream& os, char x);
void logValueTo(std::ostream& os, unsigned char x);
void logValueTo(std::ostream& os, signed char x);
void logValueTo(std::ostream& os, bool x);
void logValueTo(std::ostream& os, int16_t x);
void logValueTo(std::ostream& os, uint16_t x);
void logValueTo(std::ostream& os, int32_t x);
void logValueTo(std::ostream& os, uint32_t x);
void logValueTo(std::ostream& os, int64_t x);
void logValueTo(std::ostream& os, uint64_t x);
void logValueTo(std::ostream& os, float x);
void logValueTo(std::ostream& os, double x);
void logValueTo(std::ostream& os, unsigned char* p);
void logValueTo(std::ostream& os, const unsigned char* p);
void logValueTo(std::ostream& os, const void* p);
void logValueTo(std::ostream& os, void* p);
void logValueTo(std::ostream& os, ProcPtr p);

void logValue(char x);
void logValue(unsigned char x);
void logValue(signed char x);
void logValue(bool x);
void logValue(int16_t x);
void logValue(uint16_t x);
void logValue(int32_t x);
void logValue(uint32_t x);
void logValue(unsigned char* p);
void logValue(const void* p);
void logValue(void* p);
void logValue(ProcPtr p);

// Customization point: generated code declares, in the namespace of the
// described type (Executor for the generated Mac OS types),
//
//     void describeStruct(const T&, std::ostream&);
//
// which is found by ADL.  This generic overload is the fallback for any type
// that has no type-specific one.
template<typename T>
void describeStruct(const T&, std::ostream& os)
{
    os << "?";
}

// Print a guest address, never dereferencing.
template<typename T>
void logAddressValue(std::ostream& os, T* p)
{
    if(canConvertBack(p))
        os << "0x" << std::hex << US_TO_SYN68K_CHECK0_CHECKNEG1(p) << std::dec;
    else
        os << "?";
}

// Forward declarations so the generic and GuestWrapper overloads can call one
// another (their arguments' namespaces don't make them findable by ADL).
template<typename T>
void logValueTo(std::ostream& os, const T& x);
template<class T>
void logValueTo(std::ostream& os, const guestvalues::GuestWrapper<T>& p);

template<typename T>
void logValueTo(std::ostream& os, const T& x)
{
    if constexpr(std::is_enum_v<T>)
        os << (long long)x;
    else if constexpr(std::is_pointer_v<T>)
        logAddressValue(os, x);
    else if constexpr(std::is_array_v<T>)
    {
        // Byte arrays are usually Pascal strings (Str255, Str63, ...).
        if constexpr(std::is_same_v<std::remove_cv_t<std::remove_extent_t<T>>, unsigned char>)
            logValueTo(os, (const unsigned char*)x);
        else
        {
            os << "[";
            for(std::size_t i = 0; i < std::extent_v<T>; i++)
            {
                if(i)
                    os << ", ";
                logValueTo(os, x[i]);
            }
            os << "]";
        }
    }
    else
        describeStruct(x, os);
}

template<class T>
void logValueTo(std::ostream& os, const guestvalues::GuestWrapper<T>& p)
{
    logValueTo(os, p.get());
}

// A labelled field, as emitted by the generated describeStruct functions.
template<typename T>
void logField(std::ostream& os, const char* name, const T& value)
{
    os << name << "=";
    logValueTo(os, value);
    os << " ";
}

template<typename T>
void logValue(const T& arg)
{
    describeStruct(arg, std::clog);
}
template<class T>
void logValue(const guestvalues::GuestWrapper<T>& p)
{
    logValue(p.get());
}
template<class T>
void logValue(T* p)
{
    if(canConvertBack(p))
        std::clog << "0x" << std::hex << US_TO_SYN68K_CHECK0_CHECKNEG1(p) << std::dec;
    else
        std::clog << "?";
    if(validAddress(p))
    {
        std::clog << " => ";
        logValue(*p);
    }
}
template<class T>
void logValue(guestvalues::GuestWrapper<T*> p)
{
    std::clog << "0x" << std::hex << p.raw() << std::dec;
    if constexpr(!std::is_same_v<T, void>)
    {
        if(validAddress(p.raw_host_order()))
        {
            std::clog << " => ";
            logValue(*(p.get()));
        }
    }
}



template<class T>
bool validAddress(GUEST<T*> p)
{
    return validAddress(p.raw_host_order());
}


}
}
