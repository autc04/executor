#pragma once

#include <base/traps.h>
#include <syn68k_public.h>

#include <stdint.h>

class PowerCore;

namespace Executor
{
namespace traps
{

// The straight-line entrypoints emitted by multiversal.
using Entry68KFn = syn68k_addr_t (*)(syn68k_addr_t);
using EntryPPCFn = uint32_t (*)(PowerCore&);

// Non-template replacement for WrappedFunction/TrapFunction.  It holds plain
// function pointers to generated marshalling code instead of instantiating the
// callfrom68K/callfromPPC/Register<> templates.  The generated per-trap
// callable object derives from this and adds the typed operator()/operator&.
//
// This is the Phase-0 skeleton: it exists to pin down the runtime shape and is
// not wired up by itself yet.
class GeneratedEntrypoint : public Entrypoint
{
public:
    GeneratedEntrypoint(const char* name, const char* exportToLib, int trapno,
                        Entry68KFn fn68k, EntryPPCFn fnppc,
                        GenericDispatcherTrap* dispatcher = nullptr,
                        uint32_t selector = 0)
        : Entrypoint(name, exportToLib), trapno(trapno), fn68k(fn68k), fnppc(fnppc),
          dispatcher(dispatcher), selector(selector)
    {
    }

    virtual void init() override;

    bool isPatched() const
    {
        return trapno != 0 && tableEntry() != originalFunction;
    }

    ProcPtr guestFunction() const { return guestFP; }

protected:
    syn68k_addr_t& tableEntry() const
    {
        if(trapno & TOOLBIT)
            return tooltraptable[trapno & 0x3FF];
        else
            return ostraptable[trapno & 0xFF];
    }

private:
    static syn68k_addr_t handle68K(syn68k_addr_t addr, void* ctx);

    int trapno;
    Entry68KFn fn68k;
    EntryPPCFn fnppc;
    // Set for dispatcher subtraps: registering `fn68k` under `selector` in
    // `dispatcher` instead of (or in addition to) a trap-table entry.
    GenericDispatcherTrap* dispatcher;
    uint32_t selector;
    ProcPtr guestFP = nullptr;
    syn68k_addr_t originalFunction = 0;
};

}
}
