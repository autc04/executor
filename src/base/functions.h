#pragma once

#include <stdint.h>
#include <type_traits>
#include <utility>
#include <vector>
#include <unordered_map>
#include <base/trapglue.h>

namespace Executor
{

namespace callconv
{
class Pascal { };
class Raw { };
class CCall { };

template<typename T, typename... Extras>
class Register { };
template <int n> struct A;
template <int n> struct D;

using A0 = A<0>;
using A1 = A<1>;
using A2 = A<2>;
using A3 = A<3>;
using D0 = D<0>;
using D1 = D<1>;
using D2 = D<2>;
using D3 = D<3>;
using D4 = D<4>;
using D7 = D<7>;

struct D0HighWord;
struct D0LowWord;

template <int mask> struct TrapBit;
template<typename T, typename loc> struct Out;
template<typename T, typename loc, typename outloc = loc> struct InOut;

template<typename Loc> struct ReturnMemErr;
struct CCFromD0;

struct ClearD0;
struct SaveA1D1D2;
struct MoveA1ToA0;
}
using namespace callconv;


struct OpaqueUntyped68KProc;
typedef OpaqueUntyped68KProc* ProcPtr;

template<typename F, typename CallConv = callconv::Pascal>
struct UPP {};

namespace guestvalues
{
    template<typename T>
    struct GuestWrapper;
}

template<typename Ret, typename... Args, typename CallConv>
struct UPP<Ret(Args...), CallConv>
{
    void *ptr;

    constexpr UPP() = default;
    constexpr UPP(std::nullptr_t)
        : ptr(nullptr)
    {
    }
    explicit constexpr UPP(void *ptr)
        : ptr(ptr)
    {
    }
    explicit constexpr UPP(intptr_t ptr)
        : ptr((void*)ptr)
    {
    }
    
    template<typename T>
    explicit UPP(guestvalues::GuestWrapper<T> p)
        : ptr(p.get())
    {
    }

    template<class T>   
    explicit operator T *() const { return (T*)ptr; }
    explicit operator ProcPtr() const { return (ProcPtr)ptr; }

    explicit operator bool() const { return ptr != nullptr; }
    bool operator==(UPP<Ret(Args...)> other) const { return ptr == other.ptr; }
    bool operator!=(UPP<Ret(Args...)> other) const { return ptr != other.ptr; }

    Ret operator()(Args... args) const;
};

}
