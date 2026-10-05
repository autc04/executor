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

extern int nestingLevel;

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


template<typename Arg>
inline void logList(Arg a)
{
    logValue(a);
}
inline void logList()
{
}
template<typename Arg1, typename Arg2, typename... Args>
void logList(Arg1 a, Arg2 b, Args... args)
{
    logValue(a);
    std::clog << ", ";
    logList(b,args...);
}

template<class T>
bool validAddress(GUEST<T*> p)
{
    return validAddress(p.raw_host_order());
}

template<typename... Args>
void logTrapCall(const char* trapname, Args... args)
{
    if(!loggingActive() || !trapLogEnabled(trapname))
        return;
    std::clog.clear();
    indent();
    std::clog << trapname << "(";
    logList(args...);
    std::clog << ")\n" << std::flush;
}

template<typename Ret, typename... Args>
void logTrapValReturn(const char* trapname, Ret ret, Args... args)
{
    if(!loggingActive() || !trapLogEnabled(trapname))
        return;
    indent();
    std::clog << "returning: " << trapname << "(";
    logList(args...);
    std::clog << ") => ";
    logValue(ret);
    std::clog << std::endl << std::flush;
}
template<typename... Args>
void logTrapVoidReturn(const char* trapname, Args... args)
{
    if(!loggingActive() || !trapLogEnabled(trapname))
        return;
    indent();
    std::clog << "returning: " << trapname << "(";
    logList(args...);
    std::clog << ")\n" << std::flush;
}

template<typename T>
struct FunctionTypeForFunctor : FunctionTypeForFunctor<decltype(&T::operator())>
{ };

template<typename Cls, typename Ret, typename... Args>
struct FunctionTypeForFunctor<Ret (Cls::*)(Args...)> : FunctionTypeForFunctor<Ret (Args...)>
{ };

template<typename Ret, typename... Args>
struct FunctionTypeForFunctor<Ret (*)(Args...)> : FunctionTypeForFunctor<Ret (Args...)>
{ };
template<typename Ret, typename... Args>
struct FunctionTypeForFunctor<Ret (&)(Args...)> : FunctionTypeForFunctor<Ret (Args...)>
{ };
template<typename Ret, typename... Args>
struct FunctionTypeForFunctor<Ret (Args...)>
{
    using type = Ret (Args...);
};


template<typename CallConv, typename F,
         typename FunType = typename FunctionTypeForFunctor<F>::type>
class LoggedFunction;

template<typename CallConv, typename F, typename Ret, typename... Args>
class LoggedFunction<CallConv, F, Ret(Args...)>
{
    const char *name;
    F fun;

    struct LogLevelAdjuster
    {
        LogLevelAdjuster() { nestingLevel++; }
        ~LogLevelAdjuster() { nestingLevel--; }
    };
public:
    LoggedFunction(const char *name, const F& fun)
        : name(name), fun(fun)
    {
    }

    Ret operator() (Args... args) const
    {
        LogLevelAdjuster adj;

        if constexpr(std::is_same_v<CallConv, callconv::Raw>)
            logUntypedArgs(name);
        else
            logTrapCall(name, args...);

        if constexpr(std::is_same_v<Ret, void>)
        {
            fun(args...);
            logTrapVoidReturn(name, args...);
        }
        else
        {
            Ret ret = fun(args...);

            if constexpr(std::is_same_v<CallConv, callconv::Raw>)
                logUntypedReturn(name);
            else 
                logTrapValReturn(name, ret, args...);

            return ret;
        }
    }
};

template<class CallConv = callconv::Pascal, class F>
LoggedFunction<CallConv, F> makeLoggedFunction(const char *name, const F& f)
{
    return LoggedFunction<CallConv, F>(name, f);
}
template<class F1, class F>
LoggedFunction<callconv::Pascal, F, F1> makeLoggedFunction1(const char *name, const F& f)
{
    return LoggedFunction<callconv::Pascal, F, F1>(name, f);
}

}
}
