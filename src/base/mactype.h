#if !defined(_MACTYPE_H_)
#define _MACTYPE_H_

/*
 * Copyright 1986, 1989, 1990, 1995 by Abacus Research and Development, Inc.
 * All rights reserved.
 *

 */
#include <stdint.h>
#include <cstring>
#include <iosfwd>
#include <stdexcept>
#include <type_traits>
#include <syn68k_public.h>

#include <base/byteswap.h>
#include <base/functions.h>
#include <base/traps.h>


namespace Executor
{

#define BEGIN_EXECUTOR_ONLY /* marker for parsing executor header files */
#define END_EXECUTOR_ONLY   /* marker for parsing executor header files */

struct Point
{
    int16_t v;
    int16_t h;

    inline bool operator==(Point other) const { return v == other.v && h == other.h; }
};

// Point is hand-written (MacTypes.yaml marks it `not-for: executor`), so unlike
// the generated structs it has no generated describeStruct.  Defined in
// base/logging.cpp.
void describeStruct(const Point&, std::ostream&);

#if defined(BIGENDIAN)
template<typename T>
T SwapTyped(T x) { return x; }
#else
template<typename T>
auto SwapTyped(T x) -> std::enable_if_t<std::is_integral_v<T>, T>
{
    if constexpr(sizeof(T) == 1)
        return x;
    else if constexpr(sizeof(T) == 2)
        return swap16((uint16_t)x);
    else if constexpr(sizeof(T) == 4)
        return swap32((uint32_t)x);
    else if constexpr(sizeof(T) == 8)
        return swap64((uint64_t)x);
}
#endif


inline uint16_t *SYN68K_TO_US_CHECK0_CHECKNEG1(syn68k_addr_t addr)
{
    if(addr == (syn68k_addr_t)-1)
        return (uint16_t*) -1;
    else
        return SYN68K_TO_US_CHECK0(addr);
}

inline syn68k_addr_t US_TO_SYN68K_CHECK0_CHECKNEG1(const void* addr)
{
    if(addr == (void*)-1)
        return (syn68k_addr_t) -1;
    else
        return US_TO_SYN68K_CHECK0(addr);
}

template<typename T, typename... Args>
inline syn68k_addr_t US_TO_SYN68K_CHECK0_CHECKNEG1(T (*addr)(Args...))
{
    return US_TO_SYN68K_CHECK0_CHECKNEG1((const void*) addr);
}


namespace guestvalues
{
template<typename T, typename = void>
struct GuestTypeTraits;

template<typename T>
struct GuestTypeTraits<T, std::enable_if_t<
    (std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) <= 4>>
{
    using HostType = T;
    using GuestType = T;
    static constexpr bool fitsInRegister = true;

    static GuestType host_to_guest(HostType x) { return SwapTyped(x); }
    static HostType guest_to_host(GuestType x) { return SwapTyped(x); }

    static uint32_t host_to_reg(HostType x) { return (uint32_t)x; }
    static HostType reg_to_host(std::enable_if_t<sizeof(T) <= 4, uint32_t> x) { return (HostType)x; }
};

template<typename T>
struct GuestTypeTraits<T, std::enable_if_t<std::is_integral_v<T> && 4 < sizeof(T)>>
{
    using HostType = T;
    using GuestType = T;
    static constexpr bool fitsInRegister = false;

    static GuestType host_to_guest(HostType x) { return SwapTyped(x); }
    static HostType guest_to_host(GuestType x) { return SwapTyped(x); }
};

template<>
struct GuestTypeTraits<std::nullptr_t>
{
    using HostType = std::nullptr_t;
    using GuestType = uint32_t;
    static constexpr bool fitsInRegister = true;

    static GuestType host_to_guest(HostType x) { return 0; }

    static uint32_t host_to_reg(HostType x) { return 0; }
};

template<typename T>
struct GuestTypeTraits<T*>
{
    using HostType = T*;
    using GuestType = uint32_t;
    static constexpr bool fitsInRegister = true;

    static GuestType host_to_guest(HostType x) { return SwapTyped(US_TO_SYN68K_CHECK0_CHECKNEG1(x)); }
    static HostType guest_to_host(GuestType x) { return (HostType)SYN68K_TO_US_CHECK0_CHECKNEG1(SwapTyped(x)); }

    static uint32_t host_to_reg(HostType x) { return US_TO_SYN68K_CHECK0_CHECKNEG1(x); }
    static HostType reg_to_host(uint32_t x) { return (HostType)SYN68K_TO_US_CHECK0_CHECKNEG1(x); }
};

template<typename T, typename CallConv>
struct GuestTypeTraits<UPP<T, CallConv>>
{
    using HostType = UPP<T, CallConv>;
    using GuestType = uint32_t;
    static constexpr bool fitsInRegister = true;

    static GuestType host_to_guest(HostType x) { return SwapTyped(US_TO_SYN68K_CHECK0_CHECKNEG1((void*)x)); }
    static HostType guest_to_host(GuestType x) { return (HostType)SYN68K_TO_US_CHECK0_CHECKNEG1(SwapTyped(x)); }

    static uint32_t host_to_reg(HostType x) { return US_TO_SYN68K_CHECK0_CHECKNEG1((void*)x); }
    static HostType reg_to_host(uint32_t x) { return (HostType)SYN68K_TO_US_CHECK0_CHECKNEG1(x); }
};

template<typename To, typename From>
To reinterpret_bits_cast(From x)
{
    To y;
    static_assert(sizeof(x) == sizeof(y), "illegal reinterpret_bits_cast");
    std::memcpy(&y, &x, sizeof(x));
    return y;
}

template<>
struct GuestTypeTraits<float>
{
    using HostType = float;
    using GuestType = uint32_t;
    static constexpr bool fitsInRegister = true;

    static GuestType host_to_guest(HostType x) { return SwapTyped(reinterpret_bits_cast<GuestType>(x)); }
    static HostType guest_to_host(GuestType x) { return reinterpret_bits_cast<HostType>(SwapTyped(x)); }

    static uint32_t host_to_reg(HostType x) { return reinterpret_bits_cast<GuestType>(x); }
    static HostType reg_to_host(uint32_t x) { return (HostType)reinterpret_bits_cast<HostType>(x); }
};

template<>
struct GuestTypeTraits<double>
{
    using HostType = double;
    using GuestType = uint64_t;
    static constexpr bool fitsInRegister = false;

    static GuestType host_to_guest(HostType x) { return SwapTyped(reinterpret_bits_cast<GuestType>(x)); }
    static HostType guest_to_host(GuestType x) { return reinterpret_bits_cast<HostType>(SwapTyped(x)); }
};
template<>
struct GuestTypeTraits<Point>
{
    using HostType = Point;
    using GuestType = uint32_t;
    static constexpr bool fitsInRegister = true;

    static_assert(sizeof(Point) == 4);

    static GuestType host_to_guest(HostType x) { return reinterpret_bits_cast<uint32_t>(SwapPoint(x)); }
    static HostType guest_to_host(GuestType x) { return SwapPoint(reinterpret_bits_cast<Point>(x)); }
    
    static uint32_t host_to_reg(HostType x) { return ((uint32_t)x.v << 16) | (x.h & 0xFFFFU); }
    static HostType reg_to_host(uint32_t x) { return Point{ int16_t(x >> 16), int16_t(x & 0xFFFF) }; }

private:
    static Point SwapPoint(Point x) { return Point { SwapTyped(x.v), SwapTyped(x.h) }; }
};

template<typename T>
struct GuestWrapperStorage
{
    using HostType = T;
    using GuestType = typename GuestTypeTraits<T>::GuestType;
    
    static_assert(sizeof(GuestType) >= 2, "GuestWrapper<> should not be used for single-byte values");

    union
    {
        uint8_t data[sizeof(GuestType)];
        uint16_t align;
    };

    GuestType raw() const
    {
        GuestType tmp;
        std::memcpy(&tmp, data, sizeof(GuestType));
        return tmp;
    }

    void raw(GuestType x)
    {
        std::memcpy(data, &x, sizeof(GuestType));
    }
};

template<typename T>
struct GuestWrapperGetSet : GuestWrapperStorage<T>
{
    using GuestWrapperStorage<T>::GuestWrapperStorage;

    using Traits = GuestTypeTraits<T>;
    using HostType = typename Traits::HostType;

    HostType get() const
    {
        return Traits::guest_to_host(this->raw());
    }

    void set(HostType x)
    {
        this->raw(Traits::host_to_guest(x));
    }
};

template<typename T, typename Enable = void>
struct GuestWrapperOps : GuestWrapperGetSet<T>
{
    using GuestWrapperGetSet<T>::GuestWrapperGetSet;
};

template<typename T>
struct GuestWrapper : GuestWrapperOps<T>
{
    using Traits = GuestTypeTraits<T>;
    using HostType = typename Traits::HostType;
    using GuestType = typename Traits::GuestType;

    uint32_t raw_host_order() const
    {
        static_assert(Traits::fitsInRegister);
        if constexpr(Traits::fitsInRegister)
            return SwapTyped(this->raw());
        else
            return 0;
    }

    void raw_host_order(uint32_t x)
    {
        static_assert(Traits::fitsInRegister);
        if constexpr(Traits::fitsInRegister)
            this->raw(SwapTyped(static_cast<GuestType>(x)));
    }

    GuestWrapper() = default;
    GuestWrapper(const GuestWrapper<T>& y) = default;
    GuestWrapper<T> &operator=(const GuestWrapper<T> &y) = default;
    using GuestWrapperOps<T>::GuestWrapperOps;

    template<typename T2, typename = typename std::enable_if_t<std::is_convertible_v<T2, T>>>
    GuestWrapper(const GuestWrapper<T2> &y)
    {
        if constexpr(sizeof(T) == sizeof(T2))
            this->raw(y.raw());
        else
            this->set(y.get());
    }

    template<typename T2, typename = typename std::enable_if_t<std::is_convertible_v<T2, T>>>
    GuestWrapper<T> &operator=(const GuestWrapper<T2> &y)
    {
        if constexpr(sizeof(T) == sizeof(T2))
            this->raw(y.raw());
        else
            this->set(y.get());
        return *this;
    }


    GuestWrapper(const T &y)
    {
        this->set(y);
    }

    GuestWrapper<T> &operator=(const T &y)
    {
        this->set(y);
        return *this;
    }

    operator T() const { return this->get(); }

    template<typename T2>
    explicit operator T2() const { return (T2)this->get(); }

    static GuestWrapper<T> fromRaw(GuestType r)
    {
        GuestWrapper<T> w;
        w.raw(r);
        return w;
    }
    static GuestWrapper<T> fromHost(HostType x)
    {
        GuestWrapper<T> w;
        w.set(x);
        return w;
    }

    explicit operator bool()
    {
        return this->raw() != 0;
    }
};


template<typename T>
struct GuestWrapperOps<T*> : GuestWrapperGetSet<T*>
{
    T& operator*() const { return *this->get(); }
    T* operator->() const { return this->get(); }
};

template<>
struct GuestWrapperOps<void*> : GuestWrapperGetSet<void*>
{
};
template<>
struct GuestWrapperOps<const void*> : GuestWrapperGetSet<const void*>
{
};

template<typename Ret, typename... Args, typename CallConv>
struct GuestWrapperOps<UPP<Ret(Args...), CallConv>> : GuestWrapperGetSet<UPP<Ret(Args...), CallConv>>
{
    Ret operator()(Args... args)
    {
        return (this->get())(args...);
    }
};

template<typename T>
struct GuestWrapperOps<T, std::enable_if_t<std::is_integral_v<T>>> : GuestWrapperGetSet<T>
{
    GuestWrapper<T> operator~() const
    {
        return GuestWrapper<T>::fromRaw(~this->raw());
    }
};


template<>
struct GuestWrapperStorage<Point>
{
    struct is_guest_struct {};
    GuestWrapper<int16_t> v, h;

    using HostType = Point;
    using GuestType = uint32_t;

    GuestType raw() const
    {
        GuestType tmp;
        std::memcpy(&tmp, &v, sizeof(GuestType));
        return tmp;
    }

    void raw(GuestType x)
    {
        std::memcpy(&v, &x, sizeof(GuestType));
    }

    GuestWrapperStorage() = default;
    GuestWrapperStorage(GuestWrapper<int16_t> v, GuestWrapper<int16_t> h)
        : v(v), h(h) {}
};




template<class T1>
auto operator==(GuestWrapper<T1*> a, std::nullptr_t)
{
    return !a;
}

template<class T1>
auto operator==(std::nullptr_t, GuestWrapper<T1*> a)
{
    return !a;
}
template<class T1>
auto operator!=(GuestWrapper<T1*> a, std::nullptr_t)
{
    return (bool)a;
}

template<class T1>
auto operator!=(std::nullptr_t, GuestWrapper<T1*> a)
{
    return (bool)a;
}

#define DECLARE_OPERATOR_1(OP, TYPE_A, TYPE_B, USE_A, USE_B)                                             \
    template<class T1, class T2>                                                                         \
    auto operator OP(TYPE_A a, TYPE_B b)                                                                 \
        ->decltype(typename GuestTypeTraits<T1>::HostType() OP typename GuestTypeTraits<T2>::HostType()) \
    {                                                                                                    \
        return USE_A OP USE_B;                                                                           \
    }

#define DECLARE_OPERATOR(OP)                                 \
    DECLARE_OPERATOR_1(OP, GuestWrapper<T1>, T2, a.get(), b) \
    DECLARE_OPERATOR_1(OP, T1, GuestWrapper<T2>, a, b.get()) \
    DECLARE_OPERATOR_1(OP, GuestWrapper<T1>, GuestWrapper<T2>, a.get(), b.get())

#define DECLARE_ASSIGNMENT_OPERATOR_1(OP, TYPE_B)                     \
    template<class T1, class T2>                                      \
    auto operator OP##=(GuestWrapper<T1> &a, TYPE_B b)                \
        ->decltype(a = a OP typename GuestTypeTraits<T2>::HostType()) \
    {                                                                 \
        return a = a OP b;                                            \
    }

#define DECLARE_ASSIGNMENT_OPERATOR(OP)                 \
    DECLARE_ASSIGNMENT_OPERATOR_1(OP, GuestWrapper<T2>) \
    DECLARE_ASSIGNMENT_OPERATOR_1(OP, T2)

#define DECLARE_OPERATOR_AND_ASSIGNMENT(OP) \
    DECLARE_OPERATOR(OP)                    \
    DECLARE_ASSIGNMENT_OPERATOR(OP)

DECLARE_OPERATOR(==)
DECLARE_OPERATOR(!=)
DECLARE_OPERATOR(>)
DECLARE_OPERATOR(<)
DECLARE_OPERATOR(>=)
DECLARE_OPERATOR(<=)
DECLARE_OPERATOR_AND_ASSIGNMENT(+)
DECLARE_OPERATOR_AND_ASSIGNMENT(-)
DECLARE_OPERATOR_AND_ASSIGNMENT(*)
DECLARE_OPERATOR_AND_ASSIGNMENT(/)
DECLARE_OPERATOR_AND_ASSIGNMENT(>>)
DECLARE_OPERATOR_AND_ASSIGNMENT(<<)
DECLARE_OPERATOR_AND_ASSIGNMENT(&)
DECLARE_OPERATOR_AND_ASSIGNMENT(|)
DECLARE_OPERATOR_AND_ASSIGNMENT(^)


#define DECLARE_UNARY_OP(OP)                                    \
    template<class T1>                                          \
    auto operator OP(GuestWrapper<T1> a)                        \
        ->decltype(OP typename GuestTypeTraits<T1>::HostType()) \
    {                                                           \
        return OP a.get();                                      \
    }

DECLARE_UNARY_OP(-)
DECLARE_UNARY_OP(~)

template<class T>
T& declref();

#define DECLARE_PREFIX_POSTFIX(OP, IMPL)                                                       \
    template<class T1>                                                                   \
    auto operator OP(GuestWrapper<T1> &a)                                                \
        ->GuestWrapper<decltype(declref<typename GuestTypeTraits<T1>::HostType>() OP)> & \
    {                                                                                    \
        return a IMPL;                                                                   \
    }                                                                                    \
    template<class T1>                                                                   \
    auto operator OP(GuestWrapper<T1> &a, int)                                           \
        ->decltype(declref<typename GuestTypeTraits<T1>::HostType>() OP)                 \
    {                                                                                    \
        auto tmp = a;                                                                    \
        a IMPL;                                                                          \
        return tmp.get();                                                                \
    }

DECLARE_PREFIX_POSTFIX(++, += 1)
DECLARE_PREFIX_POSTFIX(--, -= 1)

#undef DECLARE_OPERATOR
#undef DECLARE_OPERATOR_1
#undef DECLARE_ASSIGNMENT_OPERATOR
#undef DECLARE_ASSIGNMENT_OPERATOR_1
#undef DECLARE_OPERATOR_AND_ASSIGNMENT
#undef DECLARE_UNARY_OP
#undef DECLARE_PREFIX_POSTFIX

#define GUEST_STRUCT    struct is_guest_struct {}

template<typename TT, typename SFINAE = void>
struct GuestType
{
    using type = GuestWrapper<TT>;
};

template<typename TT>
struct GuestType<GuestWrapper<TT>>
{
    using type = GuestWrapper<TT>;
};

template<typename TT>
struct GuestType<TT, std::void_t<typename TT::is_guest_struct>>
{
    using type = TT;
};


template<>
struct GuestType<char>
{
    using type = char;
};

template<>
struct GuestType<bool>
{
    using type = int8_t;
};

template<>
struct GuestType<signed char>
{
    using type = signed char;
};

template<>
struct GuestType<unsigned char>
{
    using type = unsigned char;
};

template<typename TT, int n>
struct GuestType<TT[n]>
{
    using type = typename GuestType<TT>::type[n];
};

template<typename TT>
struct GuestType<TT[0]>
{
    using type = typename GuestType<TT>::type[0];
};



}

template<typename TT>
using GUEST = typename guestvalues::GuestType<TT>::type;


template<typename T>
GUEST<T> toGuest(T x)
{
    return GUEST<T>(x);
}

template<typename T>
T toHost(guestvalues::GuestWrapper<T> x)
{
    return x;
}

template<typename T>
uint32_t toRawHostOrder(GUEST<T> x)
{
    if constexpr(sizeof(x) == 1)
        return x;
    else
        return x.raw_host_order();
}
template<typename T>
void setRawHostOrder(GUEST<T>& x, uint32_t y)
{
    if constexpr(sizeof(x) == 1)
        x = y;
    else
        x.raw_host_order(y);
}


template<typename TO, typename FROM, 
    typename = typename guestvalues::GuestTypeTraits<FROM>::HostType,
    typename = std::enable_if<sizeof(GUEST<TO>) == sizeof(GUEST<FROM>)>>
GUEST<TO> guest_cast(guestvalues::GuestWrapper<FROM> p)
{
    //return GUEST<TO>((TO)(FROM)p);
    GUEST<TO> result;
    result.raw(p.raw());
    return result;
}


template<typename TO, typename FROM,
    typename = typename guestvalues::GuestTypeTraits<FROM>::HostType>
GUEST<TO> guest_cast(FROM in)
{
    //return GUEST<TO>((TO)(FROM)p);
    GUEST<FROM> p(in);
    GUEST<TO> result;
    result.raw_host_order(p.raw_host_order());
    return result;
}

template<typename TT>
TT ptr_from_longint(int32_t l)
{
    GUEST<TT> g;
    g.raw_host_order(l);
    return g;
}

template<typename TT>
int32_t ptr_to_longint(TT p)
{
    return GUEST<TT>(p).raw_host_order();
}

template<class T, bool inout = true>
class GuestRef
{
    T& native;
    GUEST<T> guest;
public:
    GuestRef(T& x)
        : native(x)
    {
        if(inout)
            guest = x;
    }
    GuestRef(const GuestRef<T>&) = delete;

    operator GUEST<T>*() { return &guest; }
    operator GUEST<T>&() { return guest; }

    GUEST<T>* operator&() { return &guest; }

    ~GuestRef()
    {
        native = guest;
    }
};
template<class T>
GuestRef<T,true> inout(T& x) { return GuestRef<T, true>(x); }

template<class T>
GuestRef<T,false> out(T& x) { return GuestRef<T, false>(x); }


}

#endif /* _MACTYPE_H_ */
