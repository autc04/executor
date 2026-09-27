#include <base/dispatcher.h>
#include <base/common.h>
#include <base/trapname.h>
#include <base/cpu.h>

/* #define MEMORY_WATCH */

#ifdef MEMORY_WATCH
#include <mman/mman_private.h>
#endif

using namespace Executor;

LONGINT debugnumber, debugtable[1 << 12], cutoff = 20;

#if !defined(NDEBUG)
typedef struct
{
    unsigned trapno;
    int32_t when;
} trap_when_t;

static int
compare_trap_when(const void *p1, const void *p2)
{
    return ((const trap_when_t *)p1)->when - ((const trap_when_t *)p2)->when;
}

/* Prints out the traps executed within the last NUM_TRAPS_BACK traps. */
void dump_recent_traps(int num_traps_back)
{
    trap_when_t traps[0x1000];
    int i, num_interesting_traps;

    /* Record all the traps that are recent enough. */
    for(i = 0, num_interesting_traps = 0; i < (int)std::size(traps); i++)
    {
        int32_t when;

        when = debugtable[i];
        if(when != 0 && when >= debugnumber - num_traps_back)
        {
            traps[num_interesting_traps].trapno = 0xA000 + i;
            traps[num_interesting_traps].when = when;
            num_interesting_traps++;
        }
    }

    /* Sort them by time, oldest first. */
    qsort(traps, num_interesting_traps, sizeof traps[0], compare_trap_when);

    /* Print them out. */
    for(i = 0; i < num_interesting_traps; i++)
        printf("0x%04X\t%ld\t%s\n", traps[i].trapno, (long)traps[i].when,
               trap_name(traps[i].trapno));
}
#endif /* !NDEBUG */

#if !defined(NDEBUG)

char trap_watchpoint_data[1024];
char *trap_watchpoint_name[1024];
struct
{
    /* pointer to actual memory */
    int handle_p;
    char *x;
    /* pointer to our `orignal' copy */
    char *orig;
    int size;
} trap_watchpoints[1024];
int trap_watchpoint_next_data;
int trap_watchpoint_next;

/* make a per-trap watchpoint of the data at `x', of size `size' */
void trap_watch(char *name, void *x, int size)
{
    char *trap_data;

    if(trap_watchpoint_next == 1024)
    {
        fprintf(stderr, "all available trap watchpoints used\n");
        return;
    }
    trap_watchpoints[trap_watchpoint_next].size = size;
    trap_watchpoints[trap_watchpoint_next].handle_p = false;
    trap_watchpoints[trap_watchpoint_next].x = (char *)x;
    trap_data
        = trap_watchpoints[trap_watchpoint_next].orig
        = &trap_watchpoint_data[trap_watchpoint_next_data];
    trap_watchpoint_next++;
    trap_watchpoint_next_data += size;

    memcpy(trap_data, x, size);
}

void trap_watch_handle(char *name, Handle x, int size)
{
    char *trap_data;

    if(trap_watchpoint_next == 1024)
    {
        fprintf(stderr, "all available trap watchpoints used\n");
        return;
    }
    trap_watchpoints[trap_watchpoint_next].size = size;
    trap_watchpoints[trap_watchpoint_next].handle_p = true;
    trap_watchpoints[trap_watchpoint_next].x = (char *)x;
    trap_data
        = trap_watchpoints[trap_watchpoint_next].orig
        = &trap_watchpoint_data[trap_watchpoint_next_data];
    trap_watchpoint_next++;
    trap_watchpoint_next_data += size;

    memcpy(trap_data, *(Handle)x, size);
}

void trap_break_me(void)
{
}

void trap_dump_bits(const char *msg, char *data, int size)
{
    int i;

    fprintf(stderr, "%s: ", msg);
    for(i = 0; i < size; i++)
        fprintf(stderr, "%x%x",
                (int)((data[i] >> 4) & 0xF),
                (int)(data[i] & 0xF));
    fprintf(stderr, "\n");
}

int check_trap_watchpoints_p = false;

void check_trap_watchpoints(const char *msg)
{
    int size, i;

    for(i = 0; i < 1024; i++)
    {
        if(trap_watchpoints[i].x)
        {
            void *x;

            x = (trap_watchpoints[i].handle_p
                     ? (void *)*(Handle)trap_watchpoints[i].x
                     : trap_watchpoints[i].x);

            size = trap_watchpoints[i].size;

            if(memcmp(x, trap_watchpoints[i].orig,
                      size))
            {
                /* differs */
                fprintf(stderr, "%s", msg);
                trap_dump_bits("old", trap_watchpoints[i].orig, size);
                trap_dump_bits("new", (char *)x, size);

                memcpy(trap_watchpoints[i].orig, x, size);

                trap_break_me();
            }
        }
    }
}
#endif /* !NDEBUG */

#define TOOLMASK (0x3FF)
#define OSMASK (0xFF)
#define DONTSAVEA0BIT (0x100)
#define POPBIT (0x400)

#define LOADSEGTRAPWORD (0xA9F0)
#define EXITTOSHELLTRAPWORD (0xA9F4)
enum
{
    MODESWITCHTRAPWORD = 0xaafe
};

#define ALINEEXCEPTIONFRAMESIZE 8
#define WEIRDMAGIC (0x1F52)


unsigned short Executor::mostrecenttrap;


#if defined(MEMORY_WATCH)

int memory_watch = 0;

static void
dump_difference(uint16_t trapn, int i,
                zone_info_t *currentp, const zone_info_t *newp)
{
    fprintf(stderr, "%d %s(%d): D#rel = %d, D#nrel = %d, D#free = %d, Dtotal = %d\n",
            debugnumber, trap_name(trapn), i, newp->n_rel - currentp->n_rel,
            newp->n_nrel - currentp->n_nrel, newp->n_free - currentp->n_free,
            newp->total_free - currentp->total_free);
    *currentp = *newp;
}

static void
compare_zone_infos(uint16_t trapn, zone_info_t current[3], zone_info_t new[3])
{
    int i;

    for(i = 0; i < 2; ++i)
    {
        if((std::abs(current[i].n_rel - new[i].n_rel) > 2) || (std::abs(current[i].n_nrel - new[i].n_nrel) > 2) || (std::abs(current[i].total_free - new[i].total_free) > 12 * 1024))
            dump_difference(trapn, i, &current[i], &new[i]);
    }
}
#endif

syn68k_addr_t Executor::alinehandler(syn68k_addr_t pc, void *ignored)
{
    syn68k_addr_t retval;
    unsigned short trapno, status;
    unsigned short trapword;
    syn68k_addr_t togoto;
#if defined(MEMORY_WATCH)
    zone_info_t current_zone_infos[3];
    zone_info_t new_zone_infos[3];
#endif

#if defined(MEMORY_WATCH)
    if(memory_watch)
    {
        ROMlib_sledgehammer_zones(__PRETTY_FUNCTION__, __FILE__, __LINE__,
                                  "after aline", current_zone_infos);
    }
#endif

    mostrecenttrap = trapword = READUW(pc);
    retval = pc + 2;

    // Record the exact guest address of the trapping instruction so that the
    // debugger can report a real call site (see Debugger::trapBreak68K).
    currentTrapPC = pc;

#if !defined(NDEBUG)
    if(check_trap_watchpoints_p)
        check_trap_watchpoints("entering `alinehandler ()'\n");
#endif

/*
 * Code for debugging
 */

#if !defined(NDEBUG)
    debugtable[trapword & 0xFFF] = ++debugnumber;
#endif /* !NDEBUG */

#if 0
    warning_trace_info ("in trapword = 0x%x, pc = 0x%x", trapword, pc);
#endif

    status = READUW(EM_A7);
    EM_A7 += ALINEEXCEPTIONFRAMESIZE;
    if(trapword & TOOLBIT)
    {
        trapno = trapword & TOOLMASK;
        togoto = tooltraptable[trapno];

        if(trapword & POPBIT)
            retval = POPADDR();

        PUSHADDR(retval); /* Where they'll return to */
        retval = (syn68k_addr_t)togoto; /* Where they have patched */
        /* us to go to */
    }
    else
    {
        trapno = trapword & OSMASK;
        togoto = ostraptable[trapno];

        PUSHADDR(retval);
        PUSHUW(status);
        PUSHUW(WEIRDMAGIC);
        PUSHUL(EM_A2);
        PUSHUL(EM_D2);
        PUSHUL(EM_D1);
        PUSHUL(EM_A1);
        EM_D1 = trapword;
        EM_D2 = (trapword & 0xFF) << 2;
        EM_A2 = (LONGINT)togoto;
        if(!(trapword & DONTSAVEA0BIT))
            PUSHUL(EM_A0);
        execute68K((syn68k_addr_t)togoto);
        if(!(trapword & DONTSAVEA0BIT))
            EM_A0 = POPUL();
        EM_A1 = POPUL();
        EM_D1 = POPUL();
        EM_D2 = POPUL();
        EM_A2 = POPUL();
        EM_A7 += 4;
        retval = POPADDR();

        cpu_state.ccc = 0;
        cpu_state.ccn = (cpu_state.regs[0].sw.n < 0);
        cpu_state.ccv = 0;
        cpu_state.ccx = cpu_state.ccx; /* unchanged */
        cpu_state.ccnz = (cpu_state.regs[0].sw.n != 0);
    }

#if defined(MEMORY_WATCH)
    if(memory_watch)
    {
        ROMlib_sledgehammer_zones(__PRETTY_FUNCTION__, __FILE__, __LINE__,
                                  "after aline", new_zone_infos);
        compare_zone_infos(trapword, current_zone_infos, new_zone_infos);
    }
#endif

#if !defined(NDEBUG)
    if(check_trap_watchpoints_p)
        check_trap_watchpoints("exiting `alinehandler ()'\n");
#endif

#if 0
    warning_trace_info ("out retval = 0x%x", retval);
#endif

    return retval;
}