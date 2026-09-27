#pragma once
#include <syn68k_public.h>

class PowerCore;

namespace Executor
{
    enum class CPUMode
    {
        none,
        m68k,
        ppc
    };

    PowerCore& getPowerCore();

    extern CPUMode currentCPUMode;
    extern syn68k_addr_t currentM68KPC;

    // Guest address of the A-line trap instruction currently being dispatched
    // through alinehandler().  This is the only reliable source for the trap
    // call site: a trap callback is entered with MAGIC_EXIT_EMULATOR_ADDRESS on
    // top of the stack, not the guest return address.
    extern uint32_t currentTrapPC;

    void execute68K(syn68k_addr_t addr);
    void executePPC(syn68k_addr_t addr);

    unsigned long ROMlib_destroy_blocks(syn68k_addr_t start, uint32_t count,
                                        bool flush_only_faulty_checksums);
}