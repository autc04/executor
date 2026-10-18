#pragma once

#include <base/functions.h>
#include <syn68k_public.h>

#include <stdint.h>
#include <unordered_map>
#include <string>

class PowerCore;

namespace Executor
{

namespace traps
{
#define TOOLBIT (0x0800)

namespace internal
{
    class DeferredInit
    {
    public:
        DeferredInit();
        virtual void init() = 0;
        static void initAll();
    private:
        DeferredInit *next;
        static DeferredInit *first, *last;
    };
}

class Entrypoint : public internal::DeferredInit
{
public:
    Entrypoint(const char* name, const char* exportToLib = nullptr)
        : name(name), libname(exportToLib) {}
    virtual void init() override;

    const char *name;
    const char *libname;
    bool breakpoint = false;

    uint32_t checkBreak68K(uint32_t addr)
    {
        if(breakpoint)
            return break68K(addr);
        else
            return ~(uint32_t)0;
    }

    uint32_t checkBreakPPC(PowerCore& cpu)
    {
        if(breakpoint)
            return breakPPC(cpu);
        else
            return ~(uint32_t)0;
    }
private:
    uint32_t break68K(uint32_t addr);
    uint32_t breakPPC(PowerCore& cpu);
};

// The uniform entry shape stored by GeneratedEntrypoint.  The marshaller is
// selected per signature; the Entrypoint* carries the name and the breakpoint
// check.
using Entry68KFn = syn68k_addr_t (*)(syn68k_addr_t, Entrypoint*);
using EntryPPCFn = uint32_t (*)(PowerCore&, Entrypoint*);

class GenericDispatcherTrap : public Entrypoint
{
public:
    virtual void addSelector(uint32_t sel, Entrypoint* entrypoint, Entry68KFn handler) = 0;
    GenericDispatcherTrap(const char* name, uint16_t trapno) : Entrypoint(name), trapno(trapno) {}
protected:
    struct SelectorEntry
    {
        Entrypoint* entrypoint;
        Entry68KFn invoke;
    };
    std::unordered_map<uint32_t, SelectorEntry> selectors;
    uint16_t trapno;
};

// Where a dispatcher reads its selector from, described as data rather than a
// template parameter so that generated code can construct a dispatcher without
// instantiating anything.
enum class SelectorKind
{
    D0,
    D1,
    StackWMasked,
    StackLMasked,
    StackWLookahead,
};

// Non-template replacement for the old templated dispatcher: the selector
// convention is a (kind, mask) pair passed to the constructor.
class GeneratedDispatcherTrap : public GenericDispatcherTrap
{
public:
    GeneratedDispatcherTrap(const char* name, uint16_t trapno, SelectorKind kind, uint32_t mask)
        : GenericDispatcherTrap(name, trapno), kind(kind), mask(mask) {}

    virtual void init() override;
    virtual void addSelector(uint32_t sel, Entrypoint* entrypoint, Entry68KFn handler) override;

private:
    static syn68k_addr_t invokeFrom68K(syn68k_addr_t addr, void* extra);
    uint32_t getSelector() const;
    void commitSelector() const;

    SelectorKind kind;
    uint32_t mask;
};


template<typename F, F* fptr, typename CallConv = callconv::Pascal>
class WrappedFunction {};

template<typename Ret, typename... Args, Ret (*fptr)(Args...), typename CallConv>
class WrappedFunction<Ret (Args...), fptr, CallConv> : public Entrypoint
{
public:
    Ret operator()(Args... args) const
    {
        return (*fptr)(args...);
    }

    UPP<Ret (Args...), CallConv> operator&() const
    {
        return guestFP;
    }

    virtual void init() override;

    using Entrypoint::Entrypoint;

    using UPPType = UPP<Ret (Args...), CallConv>;
protected:
    UPPType guestFP;
};

#define EXTERN_FUNCTION_WRAPPER(NAME, FPTR, INIT, ...) \
    extern Executor::traps::__VA_ARGS__ NAME
#define DEFINE_FUNCTION_WRAPPER(NAME, FPTR, INIT, ...) \
    Executor::traps::__VA_ARGS__ NAME INIT;   \
    template class Executor::traps::__VA_ARGS__;

#define COMMA ,

// The hand-written macro surface.  Everything the generator can express is
// emitted as straight-line entrypoints; only the hand-written `*_FUNCTION_PTR`
// objects (which publish a C++ function as a guest-callable pointer) still use
// these, plus `RAW_68K_IMPLEMENTATION` for defining a raw 68K handler.
#define PASCAL_FUNCTION_PTR(NAME) \
    DEFINE_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME), WrappedFunction<decltype(C_##NAME) COMMA &C_##NAME>)
#define REGISTER_FUNCTION_PTR(NAME, ...) \
    DEFINE_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME), WrappedFunction<decltype(C_##NAME) COMMA &C_##NAME COMMA callconv::Register<__VA_ARGS__>>)
#define CCALL_FUNCTION_PTR(NAME, ...) \
    DEFINE_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME), WrappedFunction<decltype(C_##NAME) COMMA &C_##NAME COMMA callconv::CCall>)
#define EXTERN_PASCAL_FUNCTION_PTR(NAME) \
    EXTERN_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME), WrappedFunction<decltype(C_##NAME) COMMA &C_##NAME>)
#define EXTERN_REGISTER_FUNCTION_PTR(NAME, ...) \
    EXTERN_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME), WrappedFunction<decltype(C_##NAME) COMMA &C_##NAME COMMA callconv::Register<__VA_ARGS__>>)
#define EXTERN_CCALL_FUNCTION_PTR(NAME) \
    EXTERN_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME), WrappedFunction<decltype(C_##NAME) COMMA &C_##NAME COMMA callconv::CCall>)

#define RAW_68K_IMPLEMENTATION(NAME) \
        syn68k_addr_t Executor::RAW_##NAME(syn68k_addr_t trap_address [[maybe_unused]])

// Bits read out of the register operand lists of the file-manager traps; the
// generated entrypoints use these when constructing their descriptors.
#define ASYNCBIT (1 << 10)
#define HFSBIT (1 << 9)

void init(bool enableLogging, const std::string& trapFilter = std::string());
extern std::unordered_map<std::string, traps::Entrypoint*> entrypoints;

}
}
