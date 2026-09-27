#include <debug/mon_debugger.h>
#include <base/debugger.h>
#include <OSUtil.h>

#include <mon.h>
#include <mon_disass.h>
#include <base/cpu.h>
#include <base/mactype.h>
#include <PowerCore.h>
#include <syn68k_public.h>
#include <SegmentLdr.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Executor;


class MonDebugger;


namespace
{
bool mon_singlestep = false;
bool nmi = false;

MonDebugger *g_debugger = nullptr;

std::string trim(std::string s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if(b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void usageOn()
{
    fprintf(monout, "Usage: on * \"command\"\n");
}
void usageCount(const char *name)
{
    fprintf(monout, "Usage: %s * <n>\n", name);
}

// Parse the "<name> * <n>" form shared by skip/limit/steps.  mon_token is
// already set to the token after the command name by the command dispatcher.
bool parseCountSpec(const char *name, mon_addr_t *out)
{
    if(mon_token != T_MUL || mon_get_token() != T_NUMBER)
    {
        usageCount(name);
        return false;
    }
    *out = mon_number;
    mon_get_token();
    if(mon_token != T_END)
    {
        usageCount(name);
        return false;
    }
    return true;
}
}


class MonDebugger : public base::Debugger
{
    // Behaviour at a stop (a trap entrypoint or a code breakpoint) is configured
    // with commands supplied at startup via EXECUTOR_DBG_INIT (an env var holding
    // ';'/newline-separated commands) or --debug-cmd (both may be combined):
    //
    //   on * "command"   run `command` at every stop (repeatable)
    //   skip * n         ignore the first n stops
    //   limit * n        process at most n stops, then keep going
    //   steps * n        single-step (and disassemble) n instructions per stop
    //
    // `on * "x"` resumes at every stop, i.e. it turns the debugger into a
    // non-interactive tracer ("batch mode"); `x` is not passed to cxmon, it just
    // marks the stop as auto-continue.  Without any of these commands a stop
    // drops into the interactive cxmon monitor as before.
    //
    // Only the `*` spec is understood for now; the grammar is shared by all four
    // commands so per-trap/per-address specs can be added later.

    std::vector<std::string> stopCommands;
    bool resumeRequested = false;   // "on * x" was registered
    int skipHits = 0;               // stops to ignore
    int limitHits = -1;             // stops to process (-1 = unlimited)
    int stepCount = 0;              // instructions to single-step per stop
    bool specConfigured = false;    // any of the commands above was used

    // Commands supplied at startup (EXECUTOR_DBG_INIT + --debug-cmd).  They are
    // not run immediately: they run once, when the first process is initialized
    // (see initProcess), so that 'ba' breakpoints are armed into the fresh
    // breakpoint set rather than being cleared by it.
    std::vector<std::string> startupCommands;
    bool startupRan = false;

    int stopsSeen = 0;
    int stopsProcessed = 0;
    int stepRemaining = 0;

public:
    MonDebugger();
    ~MonDebugger();

    virtual bool interruptRequested() override;
    virtual DebuggerExit interact(DebuggerEntry e) override;
    virtual void initProcess(uint32_t entrypoint) override;

    void addStopCommand(const std::string& command);
    void addStartupCommands(const std::vector<std::string>& commands);
    void setSkip(int n) { skipHits = n; specConfigured = true; }
    void setLimit(int n) { limitHits = n; specConfigured = true; }
    void setSteps(int n) { stepCount = n; specConfigured = true; }

    // Run a list of debugger commands non-interactively (used for the startup
    // script and, at each stop, for the accumulated `on *` commands).
    void runCommands(const std::vector<std::string>& commands);

private:
    DebuggerExit stopExit(const DebuggerEntry& entry, bool singlestep) const;
    DebuggerExit interactive(const DebuggerEntry& entry);
    void traceInstruction(const DebuggerEntry& entry) const;
};


MonDebugger::MonDebugger()
{
    mon_init();
    g_debugger = this;

    mon_read_byte = [](mon_addr_t addr) { return (uint32_t) *(uint8_t*)SYN68K_TO_US(addr); };
    mon_write_byte = [](mon_addr_t addr, uint32_t b) { *(uint8_t*)SYN68K_TO_US(addr) = (uint8_t) b; };

    mon_add_command("es", [] { 
        ExitToShell(); 
    }, "es                       Exit To Shell\n");

    mon_add_command("r", [] {
        if(currentCPUMode == CPUMode::ppc)
        {
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
        }
        else if(currentCPUMode == CPUMode::m68k)
        {
            fprintf(monout, "PC = %08x\n", currentM68KPC);
            for(int i = 0; i < 8; i++)
                fprintf(monout, "D%d = %08x%s", i, EM_DREG(i), i == 7 ? "\n" : " ");
            for(int i = 0; i < 8; i++)
                fprintf(monout, "A%d = %08x%s", i, EM_AREG(i), i == 7 ? "\n" : " ");
        }
        
    }, "r                        show ppc registers\n");

    mon_add_command("atb", [] {
        if(mon_token != T_STRING)
            fprintf(monout, "Usage: atb \"entrypoint\"\n");
        else
        {
            std::string str = mon_string;
            mon_get_token();
            if(mon_token != T_END)
                fprintf(monout, "Usage: atb \"entrypoint\"\n");
            else if(auto p = traps::entrypoints.find(str); p != traps::entrypoints.end())
                p->second->breakpoint = true;
            else
                fprintf(monout, "No such entrypoint: %s\n", str.c_str());
        }
    }, "atb \"entrypoint\"         break on entry point\n");

    mon_add_command("atc", [] {
        if(mon_token != T_STRING)
            fprintf(monout, "Usage: atc \"entrypoint\"\n");
        else
        {
            std::string str = mon_string;
            mon_get_token();
            if(mon_token != T_END)
                fprintf(monout, "Usage: atc \"entrypoint\"\n");
            else if(auto p = traps::entrypoints.find(str); p != traps::entrypoints.end())
                p->second->breakpoint = false;
            else
                fprintf(monout, "No such entrypoint: %s\n", str.c_str());
        }
    }, "atc \"entrypoint\"         clear on entry point breakpoint\n");

    mon_add_command("s", [] {
        mon_exit_requested = true;
        mon_singlestep = true;
    }, "s                        single step\n");

    mon_add_command("on", [] {
        if(mon_token != T_MUL || mon_get_token() != T_STRING)
        {
            usageOn();
            return;
        }
        std::string command = mon_string;
        mon_get_token();
        if(mon_token != T_END)
            usageOn();
        else
            g_debugger->addStopCommand(command);
    }, "on * \"command\"           run command at every stop\n");

    mon_add_command("skip", [] {
        mon_addr_t n;
        if(parseCountSpec("skip", &n))
            g_debugger->setSkip((int)n);
    }, "skip * n                 ignore the first n stops\n");

    mon_add_command("limit", [] {
        mon_addr_t n;
        if(parseCountSpec("limit", &n))
            g_debugger->setLimit((int)n);
    }, "limit * n                process at most n stops\n");

    mon_add_command("steps", [] {
        mon_addr_t n;
        if(parseCountSpec("steps", &n))
            g_debugger->setSteps((int)n);
    }, "steps * n                single-step n instructions per stop\n");


#ifndef _WIN32
    struct sigaction act = {};
    act.sa_handler = [](int) {
        nmi = true;
    };
    sigaction(SIGINT, &act, nullptr);
#endif

    if(const char *init = getenv("EXECUTOR_DBG_INIT"))
    {
        std::vector<std::string> commands;
        std::string script(init);
        size_t pos = 0;
        while(pos <= script.size())
        {
            size_t end = script.find_first_of(";\n", pos);
            std::string command = trim(script.substr(pos, end - pos));
            if(!command.empty())
                commands.push_back(command);
            if(end == std::string::npos)
                break;
            pos = end + 1;
        }
        addStartupCommands(commands);
    }
}

MonDebugger::~MonDebugger()
{
    g_debugger = nullptr;
}

bool MonDebugger::interruptRequested()
{
    if(nmi)
    {
        nmi = false;
        return true;
    }
    else
        return false;
}

void MonDebugger::addStopCommand(const std::string& command)
{
    specConfigured = true;
    if(command == "x")
        resumeRequested = true;   // "on * x" == batch mode: never block
    else
        stopCommands.push_back(command);
}

void MonDebugger::addStartupCommands(const std::vector<std::string>& commands)
{
    startupCommands.insert(startupCommands.end(), commands.begin(), commands.end());
}

void MonDebugger::initProcess(uint32_t entrypoint)
{
    // Base clears the live breakpoint set and inserts the process-entry
    // breakpoint.  Run the startup script *after* that, so 'ba' breakpoints
    // land in the fresh set, then merge cxmon's breakpoints in; the union keeps
    // the process-entry breakpoint (it lives outside active_break_points).
    base::Debugger::initProcess(entrypoint);

    if(!startupRan)
    {
        startupRan = true;
        runCommands(startupCommands);
        breakpoints.insert(active_break_points.begin(), active_break_points.end());
    }
}

void MonDebugger::runCommands(const std::vector<std::string>& commands)
{
    if(commands.empty())
        return;

    std::vector<const char *> args = {"mon", "-m", "-r"};
    for(const auto& command : commands)
        args.push_back(command.c_str());
    mon((int)args.size(), args.data());
    if(monout)
        fflush(monout);
}

auto MonDebugger::stopExit(const DebuggerEntry& entry, bool singlestep) const -> DebuggerExit
{
    // For an entrypoint trap, return ~0 ("not handled") so the trap callback
    // falls through and actually executes the trap; returning the trap PC would
    // re-dispatch the A-line trap through execute68K and unbalance the stack.
    if(entry.reason == Reason::entrypoint)
        return { ~(uint32_t)0, singlestep };
    return { entry.addr, singlestep };
}

void MonDebugger::traceInstruction(const DebuggerEntry& entry) const
{
    fprintf(stderr, "[dbg] step %08x: ", entry.addr);
    disass_68k(stderr, entry.addr);
    fflush(stderr);
}

auto MonDebugger::interact(DebuggerEntry entry) -> DebuggerExit
{
    // Instruction single-stepping requested by "steps * n".
    if(stepRemaining > 0 && entry.reason == Reason::breakpoint && entry.mode == CPUMode::m68k)
    {
        traceInstruction(entry);
        --stepRemaining;
        return { entry.addr, stepRemaining > 0 };
    }

    bool isStop = entry.reason == Reason::entrypoint
        || entry.reason == Reason::breakpoint;

    if(isStop && specConfigured)
    {
        ++stopsSeen;
        if(stopsSeen <= skipHits)
            return stopExit(entry, false);
        if(limitHits >= 0 && stopsProcessed >= limitHits)
            return stopExit(entry, false);
        ++stopsProcessed;

        runCommands(stopCommands);
        breakpoints = active_break_points;

        if(stepCount > 0)
        {
            stepRemaining = stepCount;
            return stopExit(entry, true);
        }
        if(resumeRequested)
            return stopExit(entry, false);

        // No "on * x": drop into the interactive monitor below.
    }

    return interactive(entry);
}

auto MonDebugger::interactive(const DebuggerEntry& entry) -> DebuggerExit
{
    mon_dot_address = entry.addr;

    const char* const cpus[] = { "none", "m68k", "ppc" };
    const char* const reasons[] = { "Ctrl-C", "breakpoint", "singlestep", "api breakpoint", "Debugger call", "DebugStr call" };

    currentCPUMode = entry.mode;

    fprintf(stdout, "(%s) %s", cpus[(int)entry.mode], reasons[(int)entry.reason]);
    if(entry.str)
        fprintf(stdout, ": %s\n", entry.str);
    else
        fprintf(stdout, "\n");

    if(entry.mode == CPUMode::m68k)
    {
        fprintf(stdout, "%08x: ", entry.addr);
        disass_68k(stdout, entry.addr);
    }
    else if(entry.mode == CPUMode::ppc)
    {
        uint32_t w = mon_read_word(entry.addr);
        fprintf(stdout, "%08x: %08x\t", entry.addr, w);
        disass_ppc(stdout, entry.addr, w);
    }

    mon_singlestep = false;
    const char *args[] = {"mon", "-m", "-r", nullptr};
    mon(3,args);

    breakpoints = active_break_points;

    return { entry.addr, mon_singlestep };
}

void Executor::InitMonDebugger()
{
    base::Debugger::instance = new MonDebugger();
}

void Executor::RunDebuggerCommands(const std::vector<std::string>& commands)
{
    if(g_debugger)
        g_debugger->addStartupCommands(commands);
}
