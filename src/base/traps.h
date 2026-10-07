#pragma once

#include <base/functions.h>
#include <syn68k_public.h>

#include <stdint.h>
#include <unordered_map>
#include <functional>
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

class GenericDispatcherTrap : public Entrypoint
{
public:
    virtual void addSelector(uint32_t sel, Entrypoint* entrypoint, std::function<syn68k_addr_t(syn68k_addr_t)> handler) = 0;
    GenericDispatcherTrap(const char* name, uint16_t trapno) : Entrypoint(name), trapno(trapno) {}
protected:
    struct SelectorEntry
    {
        Entrypoint* entrypoint;
        std::function<syn68k_addr_t(syn68k_addr_t)> invoke;
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
    virtual void addSelector(uint32_t sel, Entrypoint* entrypoint, std::function<syn68k_addr_t(syn68k_addr_t)> handler) override;

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

template<typename F, F* fptr, int trapno, typename CallConv = callconv::Pascal>
class TrapFunction {};

template<typename Ret, typename... Args, Ret (*fptr)(Args...), int trapno, typename CallConv>
class TrapFunction<Ret (Args...), fptr, trapno, CallConv> : public WrappedFunction<Ret (Args...), fptr, CallConv>
{
public:
    using UPPType = typename WrappedFunction<Ret (Args...), fptr, CallConv>::UPPType;

    Ret operator()(Args... args) const
    {
        if(isPatched())
            return invokeViaTrapTable(args...);
        else
            return fptr(args...);
    }
    
    virtual void init() override;

    TrapFunction(const char* name, const char* exportToLib = nullptr) : WrappedFunction<Ret(Args...),fptr,CallConv>(name, exportToLib) {}

    bool isPatched() const { return tableEntry() != originalFunction; }
    Ret invokeViaTrapTable(Args...) const;
private:
    syn68k_addr_t originalFunction;

    syn68k_addr_t& tableEntry() const
    {
        if(trapno & TOOLBIT)
            return tooltraptable[trapno & 0x3FF];
        else
            return ostraptable[trapno & 0xFF];
    }
};

template<typename F, F* fptr, int trapno, uint32_t selector, typename CallConv = callconv::Pascal>
class SubTrapFunction {};

template<typename Ret, typename... Args, Ret (*fptr)(Args...), int trapno, uint32_t selector, typename CallConv>
class SubTrapFunction<Ret (Args...), fptr, trapno, selector, CallConv> : public WrappedFunction<Ret (Args...), fptr, CallConv>
{
public:
    Ret operator()(Args... args) const { return fptr(args...); }
    SubTrapFunction(const char* name, GenericDispatcherTrap& dispatcher, const char* exportToLib = nullptr);
    virtual void init() override;
private:
    GenericDispatcherTrap& dispatcher;
};

#define EXTERN_FUNCTION_WRAPPER(NAME, FPTR, INIT, ...) \
    extern Executor::traps::__VA_ARGS__ NAME
#define DEFINE_FUNCTION_WRAPPER(NAME, FPTR, INIT, ...) \
    Executor::traps::__VA_ARGS__ NAME INIT;   \
    template class Executor::traps::__VA_ARGS__;

#ifndef TRAP_INSTANTIATION
#define TRAP_INSTANTIATION EXTERN
#endif

#define PREPROCESSOR_CONCAT1(A,B) A##B
#define PREPROCESSOR_CONCAT(A,B) PREPROCESSOR_CONCAT1(A,B)
#define CREATE_FUNCTION_WRAPPER PREPROCESSOR_CONCAT(TRAP_INSTANTIATION, _FUNCTION_WRAPPER)

#define COMMA ,

// The hand-written macro surface.  Since the generator converts every trap in
// defs/*.yaml itself, these are only the fallbacks it uses for anything it has
// not (yet) converted, plus the hand-written uses in emustubs.h and the
// verbatim blocks in the YAML (NOTRAP_FUNCTION2, PASCAL_SUBTRAP, RAW_68K_*).
#define PASCAL_TRAP(NAME, TRAP) \
    CREATE_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME, "InterfaceLib"), TrapFunction<decltype(C_##NAME) COMMA &C_##NAME COMMA TRAP>)
#define REGISTER_TRAP(NAME, TRAP, ...) \
    CREATE_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME, "InterfaceLib"), TrapFunction<decltype(C_##NAME) COMMA &C_##NAME COMMA TRAP COMMA callconv::Register<__VA_ARGS__>>)
#define REGISTER_TRAP2(NAME, TRAP, ...) \
    CREATE_FUNCTION_WRAPPER(stub_##NAME, &NAME, (#NAME, "InterfaceLib"), TrapFunction<decltype(NAME) COMMA &NAME COMMA TRAP COMMA callconv::Register<__VA_ARGS__>>)

#define PASCAL_SUBTRAP(NAME, TRAP, SELECTOR, TRAPNAME) \
    CREATE_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME, TRAPNAME, "InterfaceLib"), SubTrapFunction<decltype(C_##NAME) COMMA &C_##NAME COMMA TRAP COMMA SELECTOR>)

#define NOTRAP_FUNCTION(NAME) \
    CREATE_FUNCTION_WRAPPER(NAME, &C_##NAME, (#NAME, "InterfaceLib"), WrappedFunction<decltype(C_##NAME) COMMA &C_##NAME>)
#define NOTRAP_FUNCTION2(NAME) \
    CREATE_FUNCTION_WRAPPER(stub_##NAME, &NAME, (#NAME, "InterfaceLib"), WrappedFunction<decltype(NAME) COMMA &NAME>)

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

#define RAW_68K_FUNCTION(NAME) \
    syn68k_addr_t RAW_##NAME(syn68k_addr_t, void *); \
    CREATE_FUNCTION_WRAPPER(stub_##NAME, &RAW_##NAME, (#NAME), WrappedFunction<decltype(RAW_##NAME) COMMA &RAW_##NAME COMMA callconv::Raw>)
#define RAW_68K_TRAP(NAME, TRAP) \
    syn68k_addr_t RAW_##NAME(syn68k_addr_t, void *); \
    CREATE_FUNCTION_WRAPPER(stub_##NAME, &RAW_##NAME, (#NAME), TrapFunction<decltype(RAW_##NAME) COMMA &RAW_##NAME COMMA TRAP COMMA callconv::Raw>)

#define RAW_68K_IMPLEMENTATION(NAME) \
        syn68k_addr_t Executor::RAW_##NAME(syn68k_addr_t trap_address [[maybe_unused]], void *)

// Bits read out of the register operand lists of the file-manager traps; the
// generated entrypoints use these when constructing their descriptors.
#define ASYNCBIT (1 << 10)
#define HFSBIT (1 << 9)

void init(bool enableLogging, const std::string& trapFilter = std::string());
extern std::unordered_map<std::string, traps::Entrypoint*> entrypoints;

}
}
