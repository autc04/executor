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
    // The per-signature generated overload installs the callback, leaving the
    // breakpoint check and the logged name to this object.
    createCallbackUPP(fptr, guestFP, this);

    if(libname)
    {
        builtinlibs::addPPCEntrypoint(libname, name,
            [this](PowerCore& cpu) { 
                if(auto ret = this->checkBreakPPC(cpu); ~ret)
                    return ret;

                return invokeFromPPC(cpu, fptr, this); 
            }
        );
    }
}


}
}
