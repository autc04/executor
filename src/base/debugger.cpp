#include "debugger.h"
#include <base/cpu.h>
#include <base/mactype.h>
#include <PowerCore.h>
#include <cassert>

using namespace Executor;
using namespace Executor::base;

Debugger *Debugger::instance = nullptr;

Debugger::Debugger()
{
    PowerCore& cpu = getPowerCore();
    cpu.debugger = [](PowerCore& cpu) {
        cpu.CIA = instance->interact1({ Reason::breakpoint, nullptr, CPUMode::ppc, cpu.CIA });
    };
    cpu.getNextBreakpoint = [](uint32_t addr) -> uint32_t { 
        return instance->getNextBreakpoint(addr, 4);
    };
    
    syn68k_debugger_callbacks.debugger = [](uint32_t addr) -> uint32_t {
        return instance->interact1({ Reason::breakpoint, nullptr, CPUMode::m68k, addr });
    };
    syn68k_debugger_callbacks.getNextBreakpoint = [](uint32_t addr) -> uint32_t {
        return instance->getNextBreakpoint(addr, 1); 
    };

    traps::entrypoints["Debugger"]->breakpoint = true;
}

Debugger::~Debugger()
{
    PowerCore& cpu = getPowerCore();
    cpu.debugger = nullptr;
    cpu.getNextBreakpoint = nullptr;
    syn68k_debugger_callbacks.debugger = nullptr;
    syn68k_debugger_callbacks.getNextBreakpoint = nullptr;
}

uint32_t Debugger::interact1(DebuggerEntry entry)
{
    DebuggerExit exit = interact(entry);
    singlestep = exit.singlestep;
    singlestepFrom = exit.addr;
    getPowerCore().flushCache();
    ROMlib_destroy_blocks(0, ~0, false);
    return exit.addr;
}

uint32_t Debugger::nmi68K(uint32_t /*interruptCallbackAddr*/)
{
    uint16_t frameType [[maybe_unused]] = (*ptr_from_longint<GUEST<uint16_t>*>(EM_A7 + 6)) >> 12;
    assert(frameType == 0);

    uint16_t sr = *ptr_from_longint<GUEST<uint16_t>*>(EM_A7);
	uint32_t addr = *ptr_from_longint<GUEST<uint32_t>*>(EM_A7 + 2);
    EM_A7 += 8;

    assert((cpu_state.sr & 0x3000) == (sr & 0x3000));    // we should not need to switch stacks
    cpu_state.sr = sr & ~31;
    cpu_state.ccc = sr & 1;
    cpu_state.ccv = sr & 2;
    cpu_state.ccnz = ~sr & 4;
    cpu_state.ccn = sr & 8;
    cpu_state.ccx = sr & 16;
    interrupt_note_if_present();

    return interact1({ Reason::nmi, nullptr, CPUMode::m68k, addr });
}

void Debugger::nmiPPC()
{
    PowerCore& cpu = getPowerCore();
    cpu.CIA = interact1({ Reason::nmi, nullptr, CPUMode::ppc, cpu.CIA });
}

uint32_t Debugger::trapBreak68K(uint32_t addr, const char *name)
{
    if(continuingFromEntrypoint)
    {
        continuingFromEntrypoint = false;
        return ~0;
    }

        // Work out the guest address of the trapping instruction.  A trap
        // callback is entered through execute68K() with
        // MAGIC_EXIT_EMULATOR_ADDRESS on top of the stack, so the word at
        // EM_A7 is not a return address; the A-line dispatcher records the
        // real call site in currentTrapPC.  Only fall back to the stack-walk
        // (for a direct jsr to a trap-table entry) if that is unavailable.
    uint32_t trapaddr = currentTrapPC;
    if(trapaddr == 0)
    {
        uint32_t retaddr = *ptr_from_longint<GUEST<uint32_t>*>(EM_A7);
        uint16_t potentialTrap = *ptr_from_longint<GUEST<uint16_t>*>(retaddr - 2);
        if((potentialTrap & 0xF000) == 0xA000)
        {
            uint32_t entry;
            if(potentialTrap & TOOLBIT)
                entry = tooltraptable[potentialTrap & 0x3FF];
            else
                entry = ostraptable[potentialTrap & 0xFF];
            if(entry == addr)
                trapaddr = retaddr - 2;
        }
    }

    if(trapaddr != 0)
        addr = trapaddr;

    uint32_t newAddr = interact1({ Reason::entrypoint, name, CPUMode::m68k, addr });

    if(addr == newAddr)
        continuingFromEntrypoint = true;

    return newAddr;
}

uint32_t Debugger::trapBreakPPC(PowerCore& cpu, const char *name)
{
    if(continuingFromEntrypoint)
    {
        continuingFromEntrypoint = false;
        return ~0;
    }

        // pop one stack frame so we are not in the emulator callback stub
    cpu.CIA = cpu.lr -4;
    cpu.r[2] = *ptr_from_longint<GUEST<uint32>*>(cpu.r[1]+0x14);
    cpu.lr = *ptr_from_longint<GUEST<uint32>*>(cpu.r[1]+0x8);

    uint32_t addr = cpu.CIA;
    uint32_t newAddr = interact1({ Reason::entrypoint, name, CPUMode::ppc, addr });

    if(addr == newAddr)
        continuingFromEntrypoint = true;

    return newAddr;
}

uint32_t Debugger::getNextBreakpoint(uint32_t addr, uint32_t nextOffset)
{
    if(singlestep)
    {
        if(addr == singlestepFrom)
            return addr + nextOffset;
        else
            return addr;
    }
    
    auto breakpoint_it = breakpoints.lower_bound(addr);
    if(breakpoint_it == breakpoints.end())
        return 0xFFFFFFFF;
    else
        return *breakpoint_it;
}

void Debugger::initProcess(uint32_t entrypoint)
{
    // A new process means new code at new addresses, so the live breakpoint set
    // is rebuilt for it.  The derived debugger may add its own (see
    // MonDebugger::initProcess).
    breakpoints.clear();
    if(breakOnProcessEntry)
        breakpoints.insert(entrypoint);
}
