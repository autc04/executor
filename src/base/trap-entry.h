#pragma once

#include <base/traps.h>
#include <syn68k_public.h>

#include <stdint.h>

class PowerCore;

namespace Executor
{
namespace traps
{

// The straight-line entrypoints emitted by multiversal are stored as plain
// Entry68KFn / EntryPPCFn pointers.  The generated wrapper constructor passes
// a non-capturing lambda that names the trap's implementation and calls the
// signature's generated marshaller.
//
// Non-template replacement for the old templated wrapper objects.  The
// generated per-trap callable object derives from this and adds the typed
// operator()/operator&.
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

// Runs a raw 68K handler (RAW_<name>) with the untyped register/stack dump that
// --logtraps produced for the old templated wrappers.
syn68k_addr_t invokeFromRaw68K(syn68k_addr_t addr,
                               syn68k_addr_t (*f)(syn68k_addr_t),
                               traps::Entrypoint* ep);

}
