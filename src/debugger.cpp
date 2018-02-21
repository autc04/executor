#include <OSUtil.h>

#include <mon.h>
#include <rsys/cpu.h>
#include <PowerCore.h>
#include <syn68k_public.h>
#include <SegmentLdr.h>

using namespace Executor;

namespace
{
bool mon_inited = false;
bool singlestep = false;

void InitDebugger();
void EnterDebugger();

void InitDebugger()
{
    if(mon_inited)
        return;

    mon_init();
    mon_read_byte = [](mon_addr_t addr) { return (uint32_t) *(uint8_t*)SYN68K_TO_US(addr); };
    mon_write_byte = [](mon_addr_t addr, uint32_t b) { *(uint8_t*)SYN68K_TO_US(addr) = (uint8_t) b; };

    PowerCore& cpu = getPowerCore();
    cpu.debugger = [](PowerCore& cpu) { EnterDebugger(); };
    cpu.getNextBreakpoint = [](uint32_t addr) {
        if(singlestep)
            return addr;
        
        auto breakpoint_it = active_break_points.lower_bound(addr);
        if(breakpoint_it == active_break_points.end())
            return 0xFFFFFFFF;
        else
            return *breakpoint_it;
    };

    mon_add_command("es", [] { ExitToShell(); }, "Exit To Shell");

    mon_add_command("r", [] {
        PowerCore& cpu = getPowerCore();
        fprintf(monout, "CIA = %08x   cr = %08x  ctr = %08x   lr = %08x\n", cpu.CIA, cpu.cr, cpu.ctr, cpu.lr);
        fprintf(monout, "\n");
        for(int i = 0; i < 32; i++)
        {
            fprintf(monout, "%sr%d =   %08x", i < 10 ? " " : "", i, cpu.r[i]);
            if(i % 8 == 7)
                fprintf(monout, "\n");
            else
                fprintf(monout, "  ");
        }
        fprintf(monout, "\n");
        for(int i = 0; i < 32; i++)
        {
            fprintf(monout, "%sf%d = %10f", i < 10 ? " " : "", i, cpu.f[i]);
            if(i % 8 == 7)
                fprintf(monout, "\n");
            else
                fprintf(monout, "  ");
        }
        
    }, "show ppc registers");

    mon_add_command("s", [] {
        mon_exit_requested = true;
        singlestep = true;
    }, "single step");

    mon_inited = true;
}

void EnterDebugger()
{
    InitDebugger();
    mon_dot_address = getPowerCore().CIA;
    singlestep = false;
    const char *args[] = {"mon", "-m", "-r", nullptr};
    mon(3,args);
}

}

void Executor::C_DebugStr(StringPtr p)
{
    int i;

    fprintf(stderr, "debugstr: ");
    for(i = *p++; i-- > 0; fprintf(stderr, "%c", (LONGINT)*p++))
        ;
    fprintf(stderr, "\n");
}

void Executor::C_Debugger()
{
    EnterDebugger();
}