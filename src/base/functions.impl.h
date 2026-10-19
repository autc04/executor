
#pragma once

#include <base/functions.h>
#include <base/mactype.h>

#include <cstring>
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

// Read one argument from the C-convention guest stack.
template<typename Arg>
void readCArg(Arg& arg, syn68k_addr_t& ptr)
{
    arg = *ptr_from_longint<GUEST<Arg>*>(ptr);
    ptr += (sizeof(GUEST<Arg>) + 1) & ~1;
}
}

namespace callfromPPC
{
    namespace detail
    {
        // True for types whose in-memory representation already is the guest
        // (big-endian) representation, i.e. GUEST_STRUCT types and GUEST<>
        // wrappers.  Those can be copied to and from the parameter area
        // verbatim.
        template<typename T, typename = void>
        struct IsGuestStruct : std::false_type {};

        template<typename T>
        struct IsGuestStruct<T, std::void_t<typename T::is_guest_struct>>
            : std::true_type {};

        template<typename T>
        inline constexpr bool isGuestStruct = IsGuestStruct<T>::value;

        template<typename T>
        inline constexpr bool isFloatingParameter =
            std::is_same_v<T, float> || std::is_same_v<T, double>;

        // Number of 4-byte words a parameter occupies in the PowerPC
        // parameter area.  Composite parameters are padded to a multiple of
        // 4 bytes (Mac OS Runtime Architectures, "Routine Calling
        // Conventions").
        template<typename T>
        constexpr int wordsFor()
        {
            return (int)((sizeof(GUEST<T>) + 3) / 4);
        }

        // A parameter-area word holds guest (big-endian) bytes.  Composite
        // parameters start at the most significant byte of their first word
        // and are followed by padding, so a partially used last word has its
        // bytes at the top.
        inline uint32_t wordFromBytes(const uint8_t *bytes, size_t len)
        {
            uint32_t word = 0;
            for(size_t i = 0; i < len; i++)
                word |= uint32_t(bytes[i]) << (24 - 8 * i);
            return word;
        }

        inline void bytesFromWord(uint8_t *bytes, size_t len, uint32_t word)
        {
            for(size_t i = 0; i < len; i++)
                bytes[i] = uint8_t(word >> (24 - 8 * i));
        }
    }

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
        // The parameter area contains every argument, whether or not it is
        // passed in a register, so both cursors advance by its full size.
        constexpr int words = detail::wordsFor<T>();
        return ParameterPasser { cpu, paramPtr + 4 * words,
                                  gprIndex + words,
                                  fprIndex + (detail::isFloatingParameter<T> ? 1 : 0) };
    }
 
    template<typename T>
    T ParameterPasser::get()
    {
        if constexpr(detail::isFloatingParameter<T>)
        {
            if(fprIndex <= 13)
                return T(cpu.f[fprIndex]);
            else
            {
                using Traits = guestvalues::GuestTypeTraits<T>;
                using GuestType = typename Traits::GuestType;
                GuestType raw;
                std::memcpy(&raw, paramPtr, sizeof raw);
                return Traits::guest_to_host(raw);
            }
        }
        else if constexpr(detail::isGuestStruct<T>)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "guest structures must be trivially copyable");
            T result;
            auto *bytes = reinterpret_cast<uint8_t *>(&result);
            for(int i = 0; i < detail::wordsFor<T>(); i++)
            {
                size_t offset = 4 * i;
                size_t len = sizeof(T) - offset;
                if(len > 4)
                    len = 4;
                if(gprIndex + i <= 10)
                    detail::bytesFromWord(bytes + offset, len, cpu.r[gprIndex + i]);
                else
                    std::memcpy(bytes + offset, paramPtr + offset, len);
            }
            return result;
        }
        else
        {
            using Traits = guestvalues::GuestTypeTraits<T>;
            using GuestType = typename Traits::GuestType;
            static_assert(sizeof(GuestType) <= 4,
                          "parameter does not fit in a PowerPC parameter word");
            if(gprIndex <= 10)
                return Traits::reg_to_host(cpu.r[gprIndex]);
            else
            {
                GuestType raw;
                std::memcpy(&raw, paramPtr + 4 - sizeof(GuestType), sizeof raw);
                return Traits::guest_to_host(raw);
            }
        }
    }
    template<typename T>
    void ParameterPasser::set(const T& arg)
    {
        if constexpr(detail::isFloatingParameter<T>)
        {
            if(fprIndex <= 13)
                cpu.f[fprIndex] = arg;
            else
            {
                using Traits = guestvalues::GuestTypeTraits<T>;
                auto raw = Traits::host_to_guest(arg);
                std::memcpy(paramPtr, &raw, sizeof raw);
            }
        }
        else if constexpr(detail::isGuestStruct<T>)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "guest structures must be trivially copyable");
            const auto *bytes = reinterpret_cast<const uint8_t *>(&arg);
            for(int i = 0; i < detail::wordsFor<T>(); i++)
            {
                size_t offset = 4 * i;
                size_t len = sizeof(T) - offset;
                if(len > 4)
                    len = 4;
                if(gprIndex + i <= 10)
                    cpu.r[gprIndex + i] = detail::wordFromBytes(bytes + offset, len);
                else
                    std::memcpy(paramPtr + offset, bytes + offset, len);
            }
        }
        else
        {
            using Traits = guestvalues::GuestTypeTraits<T>;
            using GuestType = typename Traits::GuestType;
            static_assert(sizeof(GuestType) <= 4,
                          "parameter does not fit in a PowerPC parameter word");
            if(gprIndex <= 10)
                cpu.r[gprIndex] = Traits::host_to_reg(arg);
            else
            {
                auto raw = Traits::host_to_guest(arg);
                std::memcpy(paramPtr + 4 - sizeof(GuestType), &raw, sizeof raw);
            }
        }
    }
}

template<typename Ret, typename... Args, typename CallConv>
Ret UPP<Ret(Args...), CallConv>::operator()(Args... args) const
{
    // Found by ADL at instantiation: the generated per-signature overload.
    return invokeUPP(*this, args...);
}

}
