#pragma once

#include <chrono>
#include <vector>
#include <functional>
#include <syn68k_public.h>

class PowerCore;

namespace Executor
{
extern void syncint_init();
extern void syncint_shutdown();
extern void syncint_post(std::chrono::microseconds usecs, bool fromLast = false);
extern void syncint_wait_interrupt();
extern void syncint_check_interrupt();


class Interrupt
{
    int index = -1;

    static void handleCommon();

public:
    Interrupt() = default;
    explicit Interrupt(std::function<void ()> f);

    void trigger();


    static syn68k_addr_t handle68K(syn68k_addr_t interrupt_pc, void *unused);
    static void handlePowerPC(PowerCore& cpu);
};

}
