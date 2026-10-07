#include <base/trap-entry.h>
#include <base/functions.impl.h>
#include <base/builtinlibs.h>
#include <base/cpu.h>
#include <base/debugger.h>

#include <cassert>
#include <cstdlib>
#include <iostream>

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

    if(dispatcher)
        dispatcher->addSelector(selector, this, fn68k);
}

uint32_t GeneratedDispatcherTrap::getSelector() const
{
    switch(kind)
    {
        case SelectorKind::D0:
            return EM_D0 & mask;
        case SelectorKind::D1:
            return EM_D1 & mask;
        case SelectorKind::StackWMasked:
        case SelectorKind::StackWLookahead:
            return READUW(EM_A7 + 4) & mask;
        case SelectorKind::StackLMasked:
            return READUL(EM_A7 + 4) & mask;
    }
    return 0;
}

void GeneratedDispatcherTrap::commitSelector() const
{
    switch(kind)
    {
        case SelectorKind::StackWMasked:
        {
            auto ret = POPADDR();
            EM_A7 += 2;
            PUSHADDR(ret);
            break;
        }
        case SelectorKind::StackLMasked:
        {
            auto ret = POPADDR();
            EM_A7 += 4;
            PUSHADDR(ret);
            break;
        }
        default:
            break;
    }
}

syn68k_addr_t GeneratedDispatcherTrap::invokeFrom68K(syn68k_addr_t addr, void* extra)
{
    auto* self = static_cast<GeneratedDispatcherTrap*>(extra);

    if(auto ret = self->checkBreak68K(addr); ~ret)
        return ret;

    uint32_t sel = self->getSelector();
    auto it = self->selectors.find(sel);
    if(it != self->selectors.end())
    {
        if(auto ret = it->second.entrypoint->checkBreak68K(addr); ~ret)
            return ret;

        self->commitSelector();
        return it->second.invoke(addr);
    }
    else
    {
        std::cerr << "Unknown selector 0x" << std::hex << sel << " for trap " << self->name << std::endl;
        if(base::Debugger::instance)
        {
            if(auto ret = base::Debugger::instance->trapBreak68K(addr, "Unimplemented selector"); ~ret)
                return ret;
            return POPADDR();
        }
        else
            std::abort();
    }
}

void GeneratedDispatcherTrap::init()
{
    GenericDispatcherTrap::init();
    if(trapno)
    {
        ProcPtr guestFP = (ProcPtr)SYN68K_TO_US(::callback_install(&invokeFrom68K, this));
        if(trapno & TOOLBIT)
            tooltraptable[trapno & 0x3FF] = US_TO_SYN68K(((void*)guestFP));
        else
            ostraptable[trapno & 0xFF] = US_TO_SYN68K(((void*)guestFP));
    }
}

void GeneratedDispatcherTrap::addSelector(uint32_t sel, Entrypoint* entrypoint,
                                          std::function<syn68k_addr_t(syn68k_addr_t)> handler)
{
    selectors[sel & mask] = { entrypoint, handler };
}

}
}
