#include <base/fault_handler.h>
#include <base/cpu.h>
#include <syn68k_public.h>

#include <cstdint>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

using namespace Executor;

namespace
{
#if !defined(_WIN32)

    char *appendStr(char *p, const char *s)
    {
        while(*s)
            *p++ = *s++;
        return p;
    }

    char *appendHex(char *p, uintptr_t v, int digits)
    {
        static const char hex[] = "0123456789abcdef";
        *p++ = '0';
        *p++ = 'x';
        for(int i = digits - 1; i >= 0; i--)
            *p++ = hex[(v >> (4 * i)) & 0xF];
        return p;
    }

    // Map a host address back to a guest address, if it is one.  The guest
    // address space is split into OFFSET_TABLE_SIZE windows; block i covers
    // guest addresses with high bits equal to i and maps to host_offset + guest.
    bool hostToGuest(uintptr_t host, uint32_t& guest)
    {
#if SIZEOF_CHAR_P == 4 && !defined(TWENTYFOUR_BIT_ADDRESSING)
        guest = (uint32_t)(host - ROMlib_offset);
        return true;
#else
        for(int i = 0; i < OFFSET_TABLE_SIZE; i++)
        {
            uintptr_t off = (uintptr_t)ROMlib_offsets[i];
            if(host >= off)
            {
                uint32_t cand = (uint32_t)(host - off);
                if((cand >> (ADDRESS_BITS - OFFSET_TABLE_BITS)) == (uint32_t)i)
                {
                    guest = cand;
                    return true;
                }
            }
        }
        return false;
#endif
    }

    void reportFault(int sig, siginfo_t *si)
    {
        uintptr_t fault = (uintptr_t)si->si_addr;
        uint32_t guestAddr = 0;
        bool isGuest = hostToGuest(fault, guestAddr);

        char buf[2048];
        char *p = buf;

        p = appendStr(p, "\n*** Executor: fault while executing guest code ***\n");
        p = appendStr(p, "signal:       ");
        p = appendStr(p, sig == SIGSEGV ? "SIGSEGV" : (sig == SIGBUS ? "SIGBUS" : "?"));
        p = appendStr(p, "   host address: ");
        p = appendHex(p, fault, 16);
        if(isGuest)
        {
            p = appendStr(p, "   guest address: ");
            p = appendHex(p, guestAddr, 8);
        }
        p = appendStr(p, "\n");

        uint32_t guestPC = syn68k_current_pc();
        p = appendStr(p, "guest PC:     ");
        p = appendHex(p, guestPC, 8);
        p = appendStr(p, syn68k_track_pc ? "   (from instruction tracking)\n"
                                         : "   (tracking off; enable with --debug segfault)\n");

        p = appendStr(p, "last trap:    ");
        p = appendHex(p, currentTrapPC, 8);
        p = appendStr(p, "   PC changed by trap: ");
        p = appendHex(p, currentM68KPC, 8);
        p = appendStr(p, "\n");

        p = appendStr(p, "amode_p:      ");
        p = appendHex(p, cpu_state.amode_p, 8);
        p = appendStr(p, "   reversed_amode_p: ");
        p = appendHex(p, cpu_state.reversed_amode_p, 8);
        p = appendStr(p, "\n");

        p = appendStr(p, "D0-D7:       ");
        for(int i = 0; i < 8; i++)
        {
            p = appendHex(p, EM_DREG(i), 8);
            *p++ = ' ';
        }
        p = appendStr(p, "\nA0-A7:       ");
        for(int i = 0; i < 8; i++)
        {
            p = appendHex(p, EM_AREG(i), 8);
            *p++ = ' ';
        }
        p = appendStr(p, "\n");

        write(2, buf, p - buf);
        _exit(128 + sig);
    }

    void faultHandler(int sig, siginfo_t *si, void * /*context*/)
    {
        // Only claim faults that happened while the interpreter was running and
        // that reference the emulated address space.  Anything else is assumed
        // to be a host bug, so restore the default disposition and re-raise.
        if(emulation_depth > 0)
        {
            uint32_t guestAddr;
            if(hostToGuest((uintptr_t)si->si_addr, guestAddr))
                reportFault(sig, si);
        }

        signal(sig, SIG_DFL);
        raise(sig);
    }
#endif
}

void Executor::installFaultHandler()
{
#if !defined(_WIN32)
    // Use an alternate stack so that a fault caused by a wild stack pointer can
    // still be reported.
    static char altStack[4 * 64 * 1024];
    stack_t ss;
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa = {};
    sa.sa_sigaction = faultHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
#endif
}
