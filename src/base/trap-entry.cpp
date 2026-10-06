#include <base/trap-entry.h>
#include <base/functions.impl.h>
#include <base/builtinlibs.h>
#include <base/cpu.h>

#include <cassert>

using namespace Executor;

namespace Executor
{
namespace traps
{

syn68k_addr_t GeneratedEntrypoint::handle68K(syn68k_addr_t addr, void* ctx)
{
    currentCPUMode = CPUMode::m68k;
    currentM68KPC = *ptr_from_longint<GUEST<uint32>*>(EM_A7);

    auto* self = static_cast<GeneratedEntrypoint*>(ctx);

    if(auto ret = self->checkBreak68K(addr); ~ret)
        return ret;

    return self->fn68k(addr);
}

void GeneratedEntrypoint::init()
{
    Entrypoint::init();

    if(fn68k)
    {
        guestFP = (ProcPtr)SYN68K_TO_US(::callback_install(&handle68K, this));
        originalFunction = US_TO_SYN68K((void*)guestFP);

        if(trapno)
        {
            assert(!tableEntry());
            tableEntry() = originalFunction;
        }
    }

    if(fnppc && libname)
    {
        builtinlibs::addPPCEntrypoint(libname, name,
            [this](PowerCore& cpu)
            {
                if(auto ret = this->checkBreakPPC(cpu); ~ret)
                    return ret;

                return fnppc(cpu);
            });
    }
}

}
}
