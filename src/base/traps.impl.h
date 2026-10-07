#pragma once

#include <base/traps.h>
#include <base/functions.impl.h>
#include <base/logging.h>
#include <base/debugger.h>

#include <cassert>
#include <iostream>
#include <functional>

namespace Executor
{
namespace builtinlibs
{
    void addPPCEntrypoint(const char *library, const char *function, std::function<uint32_t (PowerCore&)> code);
}

namespace traps
{


template<typename F>
syn68k_addr_t callback_install (const F& func)
{
    return ::callback_install(
        [](syn68k_addr_t a, void * b) -> syn68k_addr_t
        {
            currentCPUMode = CPUMode::m68k;
            currentM68KPC = *ptr_from_longint<GUEST<uint32>*>(EM_A7);
            const F& f = *(const F*)b;
            return f(a);
        },
        (void*)new F(func)
    );
}

template<typename Ret, typename... Args, Ret (*fptr)(Args...), typename CallConv>
void WrappedFunction<Ret (Args...), fptr, CallConv>::init()
{
    Entrypoint::init();
#ifdef EXECUTOR_ENABLE_LOGGING
    // LoggedFunction gates on logging::enabled() at call time, so a single
    // instantiation serves both the logged and the unlogged case.
    guestFP = (UPP<Ret (Args...),CallConv>)SYN68K_TO_US(callback_install(
            [this](syn68k_addr_t addr)
            {
                if(auto ret = this->checkBreak68K(addr); ~ret)
                    return ret;

                return callfrom68K::Invoker<Ret (Args...), CallConv>
                    ::invokeFrom68K(addr, logging::makeLoggedFunction<CallConv>(name, fptr));
            }
        ));    
#else
    guestFP = (UPP<Ret (Args...),CallConv>)SYN68K_TO_US(callback_install(
            [this](syn68k_addr_t addr)
            {
                if(auto ret = this->checkBreak68K(addr); ~ret)
                    return ret;

                return callfrom68K::Invoker<Ret (Args...), CallConv>
                    ::invokeFrom68K(addr, fptr);
            }
        ));    
#endif

    if(libname)
    {
#ifdef EXECUTOR_ENABLE_LOGGING
        builtinlibs::addPPCEntrypoint(libname, name,
            [this](PowerCore& cpu) { 
                if(auto ret = this->checkBreakPPC(cpu); ~ret)
                    return ret;

                return callfromPPC::Invoker<Ret (Args...)>::invokeFromPPC(cpu, logging::makeLoggedFunction(name, fptr)); 
            }
        );
#else
        builtinlibs::addPPCEntrypoint(libname, name,
            [this](PowerCore& cpu) { 
                if(auto ret = this->checkBreakPPC(cpu); ~ret)
                    return ret;
                
                return callfromPPC::Invoker<Ret (Args...)>::invokeFromPPC(cpu, fptr);
            }
        );
#endif
    }
}

template<typename Ret, typename... Args, Ret (*fptr)(Args...), int trapno, typename CallConv>
Ret TrapFunction<Ret (Args...), fptr, trapno, CallConv>::invokeViaTrapTable(Args... args) const
{
    return (UPP<Ret (Args...),CallConv>(SYN68K_TO_US(tableEntry())))(args...);
}

template<typename Ret, typename... Args, Ret (*fptr)(Args...), int trapno, typename CallConv>
void TrapFunction<Ret (Args...), fptr, trapno, CallConv>::init()
{
    WrappedFunction<Ret (Args...), fptr, CallConv>::init();
    originalFunction = US_TO_SYN68K(((void*)this->guestFP));
    assert(trapno);
    assert(!tableEntry());
    tableEntry() = originalFunction;
}

template<typename Ret, typename... Args, Ret (*fptr)(Args...), int trapno, uint32_t selector, typename CallConv>
SubTrapFunction<Ret (Args...), fptr, trapno, selector, CallConv>::SubTrapFunction(
    const char* name, GenericDispatcherTrap& dispatcher, const char *exportToLib)
    : WrappedFunction<Ret(Args...),fptr,CallConv>(name, exportToLib), dispatcher(dispatcher)
{
}

template<typename Ret, typename... Args, Ret (*fptr)(Args...), int trapno, uint32_t selector, typename CallConv>
void SubTrapFunction<Ret (Args...), fptr, trapno, selector, CallConv>::init()
{
    WrappedFunction<Ret(Args...),fptr,CallConv>::init();
#ifdef EXECUTOR_ENABLE_LOGGING
    dispatcher.addSelector(selector, this,
        [this](syn68k_addr_t addr)
        {
            return callfrom68K::Invoker<Ret (Args...), CallConv>
                ::invokeFrom68K(addr, logging::makeLoggedFunction<CallConv>(this->name, fptr));
        }
    );
#else
    dispatcher.addSelector(selector, this,
        [](syn68k_addr_t addr)
        {
            return callfrom68K::Invoker<Ret (Args...), CallConv>
                ::invokeFrom68K(addr, fptr); 
        }
    );
#endif
}


}
}
