#include "gtest/gtest.h"
#include <base/cpu.h>
#include <base/mactype.h>
#include <PowerCore.h>

#define ABSOLUTE_ADDR 0

#if ABSOLUTE_ADDR
#define BASE 0x1000
#else
#include <MemoryMgr.h>

uint32_t BASE;
#endif
#define WORDS (ptr_from_longint<GUEST<uint16_t>*>(BASE))
#define LONGS (ptr_from_longint<GUEST<uint32_t>*>(BASE))

using namespace Executor;

thread_local int breakHit;
thread_local syn68k_addr_t stoppedAddr;

class CPU : public testing::Test
{
public:
    void m68k(std::initializer_list<uint16_t> code)
    {
        auto p = ptr_from_longint<GUEST<uint16_t>*>(BASE);
        for(auto w : code)
            *p++ = w;
        
        m68k();
    }
    void m68k()
    {
        destroy_blocks(0, ~0);
        execute68K(BASE);
    }

    void ppc(std::initializer_list<uint32_t> code)
    {
        auto p = ptr_from_longint<GUEST<uint32_t>*>(BASE);
        for(auto w : code)
            *p++ = w;

        ppc();
    }

    void ppc()
    {
        getPowerCore().flushCache();
        executePPC(BASE);
    }

#if !ABSOLUTE_ADDR
    Ptr basePtr;

    CPU()
    {
        basePtr = NewPtrClear(4096);
        BASE = ptr_to_longint(basePtr);
        EM_A7 = getPowerCore().r[1] = ptr_to_longint(LM(MemTop)) - 4;
    }
    ~CPU()
    {
        DisposePtr(basePtr);
    }
#endif
};

class CPUSingleStep : public CPU
{
public:
    CPUSingleStep()
    {
        breakHit = 0;
        stoppedAddr = 0;

        syn68k_debugger_callbacks.getNextBreakpoint = [](syn68k_addr_t addr) -> syn68k_addr_t {
            if(stoppedAddr == addr)
                return addr + 1;
            else
                return addr;
        };
        syn68k_debugger_callbacks.debugger = [](syn68k_addr_t addr) -> syn68k_addr_t {
            breakHit++;
            stoppedAddr = addr;
            destroy_blocks(0, ~0);
            return addr;
        };
    }

    ~CPUSingleStep()
    {
        syn68k_debugger_callbacks.getNextBreakpoint = nullptr;
        syn68k_debugger_callbacks.debugger = nullptr;
    }
};

TEST_F(CPU, execute68K)
{
    EM_D0 = 0;

    m68k({
        0x4E71,  //  NOP
        0x303C,  //  MOVE.W ___, d0
        0x002A,  //         #42
        0x4E75   //  RTS
    });

    EXPECT_EQ(42, EM_D0);
}

TEST_F(CPU, executePPC)
{
    getPowerCore().r[3] = 0;
    ppc({
        0x60000000,  //  nop
        0x38600036,  //  li r3, 54
        0x3860002a,  //  li r4, 42
        0x4e800020   //  blr
    });

    EXPECT_EQ(42, getPowerCore().r[3]);
}


TEST_F(CPU, breakpoint68K)
{
    thread_local syn68k_addr_t stoppedD0;
    breakHit = 0;
    stoppedAddr = stoppedD0 = 0;

    syn68k_debugger_callbacks.getNextBreakpoint = [](syn68k_addr_t addr) -> syn68k_addr_t {
        if(breakHit == 0 && addr <= BASE + 6)
            return BASE + 6;
        else
            return 0xFFFFFFFF;
    };
    syn68k_debugger_callbacks.debugger = [](syn68k_addr_t addr) -> syn68k_addr_t {
        breakHit++;
        stoppedAddr = addr;
        stoppedD0 = EM_D0;
        destroy_blocks(0, ~0);
        return addr;
    };

    EM_D0 = 0;
    m68k({
        0x4E71,  //  NOP
        0x303C,  //  MOVE.W ___, d0
        0x0036,  //         #54
        0x303C,  //  MOVE.W ___, d0
        0x002A,  //         #42
        0x4E75   //  RTS
    });

    EXPECT_EQ(42, EM_D0);
    EXPECT_EQ(1, breakHit);
    EXPECT_EQ(54, stoppedD0);
    EXPECT_EQ(BASE + 6, stoppedAddr);
}

TEST_F(CPU, breakpointPPC)
{
    auto& cpu = getPowerCore();
    cpu.r[3] = 0;

    thread_local int breakHit;
    thread_local syn68k_addr_t stoppedAddr, stoppedR3;
    breakHit = 0;
    stoppedAddr = stoppedR3 = 0;

    cpu.getNextBreakpoint = [](syn68k_addr_t addr) -> syn68k_addr_t {
        if(breakHit == 0 && addr <= BASE + 8)
            return BASE + 8;
        else
            return 0xFFFFFFFF;
    };
    cpu.debugger = [](PowerCore& cpu) {
        breakHit++;
        stoppedAddr = cpu.CIA;
        stoppedR3 = cpu.r[3];
        cpu.flushCache();
    };

    ppc({
        0x60000000,  //  nop
        0x38600036,  //  li r3, 54
        0x3860002a,  //  li r4, 42
        0x4e800020   //  blr
    });

    EXPECT_EQ(42, cpu.r[3]);
    EXPECT_EQ(1, breakHit);
    EXPECT_EQ(54, stoppedR3);
    EXPECT_EQ(BASE + 8, stoppedAddr);
}

TEST_F(CPUSingleStep, basic68K)
{
    m68k({
        0x702a,  //          moveq #42, %d0
        0x7215,  //          moveq #21, %d1
        0x4e75   //          rts
    });
    EXPECT_EQ(42, EM_D0);
    EXPECT_EQ(21, EM_D1);
    EXPECT_EQ(3, breakHit);
}

TEST_F(CPUSingleStep, braSinglestep68K)
{
    m68k({
        0x6002,  //         bra.s label
        0x4e71,  //         nop
        0x4e75   // label:  rts
    });
    EXPECT_EQ(2, breakHit);
}

TEST_F(CPUSingleStep, beqSinglestep68K)
{
    EM_D0 = 0;
    m68k({
        0x4a00,  //         tst.b %d0
        0x6702,  //         beq.s label
        0x4e71,  //         nop
        0x4e75   // label:  rts
    });
    EXPECT_EQ(3, breakHit);

    EM_D0 = 1;
    breakHit = 0;
    m68k();
    EXPECT_EQ(4, breakHit);
}

TEST_F(CPUSingleStep, fib68K)
{
    EM_D0 = 7;

    m68k({
        0x7200,  //         moveq #0, %d1
        0x7401,  //         moveq #1, %d2
        0x4a00,  // loop:   tst.b %d0
        0x6708,  //         beq.s end
        0xd282,  //         add.l %d2, %d1
        0xc541,  //         exg %d2, %d1
        0x5340,  //         subq #1, %d0
        0x60f4,  //         bra.s loop
        0x4e75   // end:    rts
    });

    EXPECT_EQ(13, EM_D1);
}
