#include <vector>
#include <syn68k_public.h>

#include <base/functions.h>
#include <base/mactype.h>
#include <base/byteswap.h>
#include <base/functions.impl.h>
#include <base/logging.h>
#include <base/dispatcher.h>

#include <assert.h>

using namespace Executor;

syn68k_addr_t Executor::tooltraptable[NTOOLENTRIES]; /* Gets filled in at run time */
syn68k_addr_t Executor::ostraptable[NOSENTRIES]; /* Gets filled in at run time */

namespace Executor
{
RAW_68K_TRAP(Unimplemented, 0xA89F);
void ReferenceAllTraps();
}

Executor::traps::internal::DeferredInit *Executor::traps::internal::DeferredInit::first = nullptr;
Executor::traps::internal::DeferredInit *Executor::traps::internal::DeferredInit::last = nullptr;

std::unordered_map<std::string, traps::Entrypoint*> Executor::traps::entrypoints;

traps::internal::DeferredInit::DeferredInit()
    : next(nullptr)
{
    if(!first)
        first = last = this;
    else
    {
        last->next = this;
        last = this;
    }
}

void traps::internal::DeferredInit::initAll()
{
    for(DeferredInit *p = first; p; p = p->next)
        p->init();
}

void traps::Entrypoint::init()
{
    entrypoints[name] = this;
}

void traps::init(bool log)
{
    logging::setEnabled(log);
    ReferenceAllTraps();
    internal::DeferredInit::initAll();
    for(int i = 0; i < NTOOLENTRIES; i++)
    {
        if(tooltraptable[i] == 0)
            tooltraptable[i] = US_TO_SYN68K((void*)&stub_Unimplemented);
    }
    for(int i = 0; i < NOSENTRIES; i++)
    {
        if(ostraptable[i] == 0)
            ostraptable[i] = US_TO_SYN68K((void*)&stub_Unimplemented);
    }

    trap_install_handler(0xA, &alinehandler, (void *)0);
}
