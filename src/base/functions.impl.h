
#pragma once

#include <base/functions.h>
#include <base/mactype.h>

#include <string.h>
#include <tuple>

#include <base/cpu.h>
#include <PowerCore.h>

namespace Executor
{

namespace callconv
{

template<int n> struct D
{
    template<typename T>
    operator T() { return cpu_state.regs[n].ul.n; }
    template<typename T>
    operator T*() { return ptr_from_longint<T*>(cpu_state.regs[n].ul.n); }
    template<typename T>
    operator UPP<T>() { return UPP<T>(ptr_from_longint<ProcPtr>(cpu_state.regs[n].ul.n)); }

    static void set(uint32_t x) { cpu_state.regs[n].ul.n = x; }
    static void set(const void *x) { cpu_state.regs[n].ul.n = ptr_to_longint(x); }
    template<class T, class CC>
    static void set(UPP<T,CC> x) { cpu_state.regs[n].ul.n = ptr_to_longint((void*)x); }

    template<class T>
    static void afterCall(T) {}
};

template<int n> struct A : D<n+8> {};

struct D0HighWord
{
    operator uint16_t() { return EM_D0 >> 16; }

    static void set(uint16_t x)
    {
        EM_D0 = (EM_D0 & 0xFFFF) | (x << 16);
    }

    template<class T>
    static void afterCall(T) {}    
};
struct D0LowWord
{
    operator uint16_t() { return EM_D0 & 0xFFFF; }

    static void set(uint16_t x)
    {
        EM_D0 = (EM_D0 & 0xFFFF0000) | x;
    }

    template<class T>
    static void afterCall(T) {}    
};

template<typename T, typename Loc> struct Out
{
    GUEST<T> temp;

    operator GUEST<T>*() { return &temp; }

    ~Out() { Loc::set(toRawHostOrder<T>(temp)); }

    static void set(GUEST<T>* p) {}
    static void afterCall(GUEST<T>* p) { setRawHostOrder<T>(*p,Loc()); }
};

template<typename T, typename InLoc, typename OutLoc> struct InOut
{
    GUEST<T> temp;

    operator GUEST<T>*() { return &temp; }

    InOut() { setRawHostOrder<T>(temp,InLoc()); }
    ~InOut() { OutLoc::set(toRawHostOrder<T>(temp)); }

    static void set(GUEST<T>* p) { InLoc::set(toRawHostOrder<T>(*p)); }
    static void afterCall(GUEST<T>* p) { setRawHostOrder<T>(*p,OutLoc()); }
};

template<int mask> struct TrapBit
{
    operator bool() { return !!(EM_D1 & mask); }

    static void set(bool b)
    {
        if(b)
            EM_D1 |= mask;
        else
            EM_D1 &= ~mask;
    }
    template<class T>
    static void afterCall(T) {}
};

struct CCFromD0
{
    syn68k_addr_t afterwards(syn68k_addr_t ret)
    {
        cpu_state.ccc = 0;
        cpu_state.ccn = (cpu_state.regs[0].sw.n < 0);
        cpu_state.ccv = 0;
        cpu_state.ccx = cpu_state.ccx; /* unchanged */
        cpu_state.ccnz = (cpu_state.regs[0].sw.n != 0);
        return ret;
    }
};

struct ClearD0
{
    syn68k_addr_t afterwards(syn68k_addr_t ret)
    {
        EM_D0 = 0;
        return ret;
    }  
};

struct SaveA1D1D2
{
    syn68k_addr_t a1, d1, d2;
    SaveA1D1D2()
    {
        a1 = EM_A1;
        d1 = EM_D1;
        d2 = EM_D2;
    }
    syn68k_addr_t afterwards(syn68k_addr_t ret)
    {
        EM_A1 = a1;
        EM_D1 = d1;
        EM_D2 = d2;
        return ret;
    }  
};

struct MoveA1ToA0
{
    syn68k_addr_t a1;
    MoveA1ToA0()
    {
        a1 = EM_A1;
    }
    syn68k_addr_t afterwards(syn68k_addr_t ret)
    {
        EM_A0 = a1;
        return ret;
    }
};

namespace stack
{
    template<class T>
    void push(T x)
    {
        EM_A7 -= (sizeof(GUEST<T>) + 1) & ~1;
        *(ptr_from_longint<GUEST<T>*>(EM_A7)) = x;
    }
    template<class T>
    T pop()
    {
        T retval = *ptr_from_longint<GUEST<T>*>(EM_A7);
        EM_A7 += (sizeof(GUEST<T>) + 1) & ~1;
        return retval;
    }
}
}

namespace callfrom68K
{
    template<typename F, typename CallConv>
    struct Invoker;

    template<typename... Xs> struct List;

    template<typename F, typename Args, typename ToDoArgs>
    struct PascalInvoker;

    template<typename Ret, typename... Args>
    struct PascalInvoker<Ret (Args...), List<Args...>, List<>>
    {
        template<typename F>
        static void invokeFrom68K(const F& fptr, Args... args)
        {
            Ret retval = fptr(args...);
            *ptr_from_longint<GUEST<Ret>*>(EM_A7) = retval;
        }
    };

    template<typename... Args>
    struct PascalInvoker<void (Args...), List<Args...>, List<>>
    {
        template<typename F>
        static void invokeFrom68K(const F& fptr, Args... args)
        {
            fptr(args...);
        }
    };

    template<typename F, typename... Args, typename Arg, typename... ToDoArgs>
    struct PascalInvoker<F, List<Args...>, List<Arg, ToDoArgs...>>
    {
        template<typename F1>
        static void invokeFrom68K(const F1& fptr, Args... args)
        {
            Arg arg = callconv::stack::pop<Arg>();
            PascalInvoker<F, List<Arg, Args...>, List<ToDoArgs...>>::invokeFrom68K(fptr, arg, args...);
        }
    };

    template<typename SrcList, typename DstList = List<>>
    struct Reverse;

    template<typename... Xs, typename X, typename... Ys>
    struct Reverse<List<X, Xs...>, List<Ys...>>
    {
        using type = typename Reverse<List<Xs...>, List<X,Ys...>>::type;
    };

    template<typename... Ys>
    struct Reverse<List<>, List<Ys...>>
    {
        using type = List<Ys...>;
    };
    
    template<typename Ret, typename... Args>
    struct Invoker<Ret (Args...), callconv::Pascal>
    {
        template<typename F>
        static syn68k_addr_t invokeFrom68K(syn68k_addr_t addr, const F& fptr)
        {
            syn68k_addr_t retaddr = callconv::stack::pop<syn68k_addr_t>();
            PascalInvoker<Ret (Args...), List<>, typename Reverse<List<Args...>>::type>::invokeFrom68K(fptr);
            return retaddr;
        }
    };

    template<typename Arg>
    void readCArg(Arg& arg, syn68k_addr_t& ptr)
    {
        arg = *ptr_from_longint<GUEST<Arg>*>(ptr);
        ptr += (sizeof(GUEST<Arg>) + 1) & ~1;
    }

    template<typename F, typename... Args, std::size_t... Indices>
    auto readCArgsAndCall(std::tuple<Args...>& args, const F& fptr, syn68k_addr_t ptr, std::index_sequence<Indices...>)
    {
        (readCArg(std::get<Indices>(args), ptr),...);
        return fptr(std::get<Indices>(args)...);
    }

    template<typename Ret, typename... Args>
    struct Invoker<Ret (Args...), callconv::CCall>
    {
        template<typename F>
        static syn68k_addr_t invokeFrom68K(syn68k_addr_t addr, const F& fptr)
        {
            std::tuple<Args...> args;
            EM_D0 = readCArgsAndCall(args, fptr, EM_A7 + 4, std::index_sequence_for<Args...>());
            return callconv::stack::pop<syn68k_addr_t>();
        }
    };

    template<typename... Args>
    struct Invoker<void (Args...), callconv::CCall>
    {
        template<typename F>
        static syn68k_addr_t invokeFrom68K(syn68k_addr_t addr, const F& fptr)
        {
            std::tuple<Args...> args;
            readCArgsAndCall(args, fptr, EM_A7 + 4, std::index_sequence_for<Args...>());
            return callconv::stack::pop<syn68k_addr_t>();
        }
    };

    template<>
    struct Invoker<syn68k_addr_t (syn68k_addr_t addr), callconv::Raw>
    {
        template<typename F>
        static syn68k_addr_t invokeFrom68K(syn68k_addr_t addr, const F& fptr)
        {
            return fptr(addr);
        }
    };

    template<typename F, typename DoneArgs, typename ToDoArgs, typename ToDoCC>
    struct RegInvoker;

    template<typename Ret, typename... Args, typename... DoneArgs>
    struct RegInvoker<Ret (Args...), List<DoneArgs...>, List<>, List<>>
    {
        template<typename F>
        static Ret invokeFrom68K(syn68k_addr_t addr, const F& fptr, DoneArgs... args)
        {
            return fptr(args...);
        }
    };

    template<typename Ret, typename... Args, typename... DoneArgs, typename Arg, typename... TodoArgs, typename CC, typename... TodoCC>
    struct RegInvoker<Ret (Args...), List<DoneArgs...>, List<Arg, TodoArgs...>, List<CC, TodoCC...>>
    {
        template<typename F>
        static Ret invokeFrom68K(syn68k_addr_t addr, const F& fptr, DoneArgs... args)
        {
            CC newarg;
            return RegInvoker<Ret (Args...),List<DoneArgs...,Arg>,List<TodoArgs...>,List<TodoCC...>>
                ::invokeFrom68K(addr, fptr, args..., newarg);
        }
    };

    template<typename Ret, typename... Args, typename RetConv, typename... ArgConvs>
    struct Invoker<Ret (Args...), callconv::Register<RetConv (ArgConvs...)>>
    {
        template<typename F>
        static syn68k_addr_t invokeFrom68K(syn68k_addr_t addr, const F& fptr)
        {
            syn68k_addr_t retaddr = POPADDR();
            Ret retval = RegInvoker<Ret (Args...), List<>, List<Args...>, List<ArgConvs...>>::invokeFrom68K(addr, fptr);
            RetConv::set(retval);  // ### double conversion?
            return retaddr;
        }
    };
    template<typename... Args, typename RetConv, typename... ArgConvs>
    struct Invoker<void (Args...), callconv::Register<RetConv (ArgConvs...)>>
    {
        template<typename F>
        static syn68k_addr_t invokeFrom68K(syn68k_addr_t addr, const F& fptr)
        {
            syn68k_addr_t retaddr = POPADDR();
            RegInvoker<void (Args...), List<>, List<Args...>, List<ArgConvs...>>::invokeFrom68K(addr, fptr);
            return retaddr;
        }
    };

    template<typename Ret, typename... Args, typename RetArgConv, typename Extra1, typename... Extras>
    struct Invoker<Ret (Args...), callconv::Register<RetArgConv, Extra1, Extras...>>
    {
        template<typename F>
        static syn68k_addr_t invokeFrom68K(syn68k_addr_t addr, const F& fptr)
        {
            Extra1 state;
            syn68k_addr_t retval = Invoker<Ret(Args...), callconv::Register<RetArgConv, Extras...>>::invokeFrom68K(addr,fptr);
            return state.afterwards(retval);
        }
    };
}

namespace callto68K
{
    template<typename F, typename CallConv>
    struct Invoker;

    template<typename F, typename... Args>
    struct InvokerRec;

    template<>
    struct InvokerRec<void ()>
    {
        static void invoke68K(void *ptr)
        {
            execute68K(US_TO_SYN68K(ptr));
        }
    };

    template<typename Arg, typename... Args, typename ArgConv, typename... ArgConvs>
    struct InvokerRec<void (Arg, Args...), ArgConv, ArgConvs...>
    {
        static void invoke68K(void *ptr, Arg arg, Args... args)
        {
            ArgConv::set(arg);
            InvokerRec<void (Args...), ArgConvs...>::invoke68K(ptr, args...);
            ArgConv::afterCall(arg);
        }
    };
   
    template<typename Ret, typename... Args, typename RetConv, typename... ArgConvs, typename... Extras>
    struct Invoker<Ret (Args...), callconv::Register<RetConv(ArgConvs...), Extras...>>
    {
        static Ret invoke68K(void *ptr, Args... args)
        {
            InvokerRec<void (Args...), ArgConvs...>::invoke68K(ptr, args...);
            return RetConv();
        }
    };

    template<typename... Args, typename... ArgConvs, typename... Extras>
    struct Invoker<void (Args...), callconv::Register<void(ArgConvs...), Extras...>>
    {
        static void invoke68K(void *ptr, Args... args)
        {
            InvokerRec<void (Args...), ArgConvs...>::invoke68K(ptr, args...);
        }
    };

    template<>
    struct Invoker<void (), callconv::Pascal>
    {
        static void invoke68K(void *ptr)
        {
            M68kReg saveregs[14];
            memcpy(saveregs, &EM_D1, sizeof(saveregs)); /* d1-d7/a0-a6 */
            execute68K(US_TO_SYN68K(ptr));
            memcpy(&EM_D1, saveregs, sizeof(saveregs)); /* d1-d7/a0-a6 */
        }
    };
    template<typename Arg, typename... Args>
    struct Invoker<void (Arg, Args...), callconv::Pascal>
    {
        static void invoke68K(void *ptr, Arg arg, Args... args)
        {
            callconv::stack::push(arg);
            Invoker<void (Args...), callconv::Pascal>::invoke68K(ptr, args...);
        }
    };
    template<typename Ret, typename... Args>
    struct Invoker<Ret (Args...), callconv::Pascal>
    {
        static Ret invoke68K(void *ptr, Args... args)
        {
            callconv::stack::push(Ret());
            Invoker<void (Args...), callconv::Pascal>::invoke68K(ptr, args...);
            return callconv::stack::pop<Ret>();
        }
    };

    template<>
    struct Invoker<syn68k_addr_t (syn68k_addr_t), callconv::Raw>
    {
        static syn68k_addr_t invoke68K(void *ptr, syn68k_addr_t)
        {
            execute68K(US_TO_SYN68K(ptr));
            return POPADDR();   // ###
        }
    };
}

namespace callfromPPC
{

    struct ParameterPasser
    {
        PowerCore& cpu;
        uint8_t *paramPtr;
        int gprIndex = 3;
        int fprIndex = 1;

        template<typename T>
        T get();
        template<typename T>
        void set(const T&);
        
        template<typename T>
        ParameterPasser operator<<(const T& arg);

        template<typename T>
        ParameterPasser operator>>(T& arg);

        template<typename T>
        constexpr ParameterPasser operator+(const T& arg);
    };

    template<typename T>
    ParameterPasser ParameterPasser::operator<<(const T& arg)
    {
        set(arg);
        return *this + arg;
    }

    template<typename T>
    ParameterPasser ParameterPasser::operator>>(T& arg)
    {
        arg = get<T>();
        return *this + arg;
    }

    template<typename T>
    constexpr ParameterPasser ParameterPasser::operator+(const T&)
    {
        static_assert(sizeof(GUEST<T>) <= 4, "parameter too large");
        return ParameterPasser { cpu, paramPtr + 4, gprIndex + 1, fprIndex };
    }
    template<>
    constexpr ParameterPasser ParameterPasser::operator+ <float>(const float&)
    {
        static_assert(sizeof(float) == 4, "unexpected sizeof(float)");
        return ParameterPasser { cpu, paramPtr + 4, gprIndex + 1, fprIndex + 1 };
    }
    template<>
    constexpr ParameterPasser ParameterPasser::operator+ <double>(const double&)
    {
        static_assert(sizeof(double) == 8, "unexpected sizeof(double)");
        return ParameterPasser { cpu, paramPtr + 1, gprIndex + 2, fprIndex + 1 };
    }
 
    template<typename T>
    T ParameterPasser::get()
    {
        static_assert(sizeof(GUEST<T>) <= 4, "parameter too large");
        if(gprIndex <= 10)
            return guestvalues::GuestTypeTraits<T>::reg_to_host(cpu.r[gprIndex]);
        else
        {
            using GuestType = typename guestvalues::GuestTypeTraits<T>::GuestType;
            auto p = reinterpret_cast<GuestType *>
                (paramPtr + 4-sizeof(GuestType));
            return guestvalues::GuestTypeTraits<T>::guest_to_host(*p);
        }
    }
    template<typename T>
    void ParameterPasser::set(const T& arg)
    {
        static_assert(sizeof(GUEST<T>) <= 4, "parameter too large");
        if(gprIndex <= 10)
            cpu.r[gprIndex] = guestvalues::GuestTypeTraits<T>::host_to_reg(arg);
        else
        {
            using GuestType = typename guestvalues::GuestTypeTraits<T>::GuestType;
            auto p = reinterpret_cast<GuestType *>
                (paramPtr + 4-sizeof(GuestType));
            *p = guestvalues::GuestTypeTraits<T>::host_to_guest(arg);
        }
    }
    template<class Ret>
    struct InvokerRet;

    template<typename Ret, typename... Args>
    struct InvokerRet<Ret (Args...)>
    {
        template<typename F>
        static void invokeRet(PowerCore& cpu, const F& fptr, Args... args)
        {
            Ret ret = fptr(args...);
            ParameterPasser{cpu,nullptr}.set(ret);
        }
    };

    template<typename... Args>
    struct InvokerRet<void (Args...)>
    {
        template<typename F>
        static void invokeRet(PowerCore& cpu, const F& fptr, Args... args)
        {
            fptr(args...);
        }
    };

    template<typename F>
    struct Invoker;

    template<typename Ret, typename... Args>
    struct Invoker<Ret (Args...)>
    {
        template<typename F, size_t... Is>
        static void invokeFromPPCHelper(PowerCore& cpu, const F& fptr, std::index_sequence<Is...>)
        {
            std::tuple<Args...> args;
            ParameterPasser argGetter{cpu, guestvalues::GuestTypeTraits<uint8_t*>::reg_to_host(cpu.r[1]+24)};

            (void)((argGetter >> ... >> std::get<Is>(args)));
            InvokerRet<Ret (Args...)>::invokeRet(cpu, fptr, std::get<Is>(args)...);
        }
        
        template<typename F>
        static uint32_t invokeFromPPC(PowerCore& cpu, const F& fptr)
        {
            uint32_t saveLR = cpu.lr;
            EM_A7 = cpu.r[1];
            invokeFromPPCHelper(cpu, fptr, std::index_sequence_for<Args...>());
            cpu.r[1] = EM_A7;
            return saveLR; //cpu.lr;
        }
    };


}

template<typename Ret, typename... Args, typename CallConv>
Ret UPP<Ret(Args...), CallConv>::operator()(Args... args) const
{
    return callto68K::Invoker<Ret(Args...), CallConv>::invoke68K(ptr, args...);
}

}
