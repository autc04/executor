#include <time/syncint.h>
#include <time/time.h>
#include <base/cpu.h>
#include <PowerCore.h>
#include <base/debugger.h>
#include <base/profiling.h>

#include <chrono>
#include <array>
#include <optional>

#if defined(_WIN32)
#define USE_TIMER_THREAD 1
#define USE_SIGNALS 0
#else
#define USE_TIMER_THREAD 0
#define USE_SIGNALS 1
#endif


#if USE_TIMER_THREAD
#include <thread>
#include <mutex>
#include <condition_variable>
#else
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <pthread.h>
#endif

using namespace Executor;

namespace
{
    constexpr int maxInterrupts = 10;
        
        // pointer to std::function to avoid static constructor
        // (so Interrupt() can be constructed at static construction time)
    std::array<std::function<void ()>*, maxInterrupts> interruptFunctions;
    std::array<std::atomic_flag, maxInterrupts> interruptTriggered;
    int nInterrupts = 0;


    Interrupt timerInterrupt;

#if USE_TIMER_THREAD
    std::mutex mutex;
    std::condition_variable wake_cond;
#else
    std::optional<pthread_t> waitingThread;
#endif

    struct SyncintTimer
    {
#if USE_TIMER_THREAD
        using clock = std::chrono::steady_clock;
        std::thread thread;
        std::condition_variable cond;
        bool terminate = false;
        clock::time_point last_interrupt;
        clock::time_point scheduled_interrupt;
#endif

        SyncintTimer();
        ~SyncintTimer();

        void start();
        void post(std::chrono::microseconds usecs, bool fromLast);
        void wait();
    };

    SyncintTimer timer;
}

Interrupt::Interrupt(std::function<void ()> f)
{
    assert(nInterrupts < maxInterrupts);
    index = nInterrupts++;

    interruptFunctions[index] = new std::function<void()>(f);
    interruptTriggered[index].test_and_set();
}

void Interrupt::trigger()
{
    if(index < 0)
        return;

    interruptTriggered[index].clear();

    interrupt_generate(M68K_TIMER_PRIORITY);
    getPowerCore().requestInterrupt();


#if USE_TIMER_THREAD
    wake_cond.notify_all();
#else
    if(waitingThread && !pthread_equal(pthread_self(), *waitingThread))
        pthread_kill(*waitingThread, SIGALRM);
#endif
}

void Interrupt::handleCommon()
{
    cpu_state.interrupt_pending[M68K_TIMER_PRIORITY] = false;
    getPowerCore().cancelInterrupt();

    /* We save the 68k registers and cc bits away. */
    M68kReg saved_regs[16];
    CCRElement saved_ccnz, saved_ccn, saved_ccc, saved_ccv, saved_ccx;
    memcpy(saved_regs, &cpu_state.regs, sizeof saved_regs);
    saved_ccnz = cpu_state.ccnz;
    saved_ccn = cpu_state.ccn;
    saved_ccc = cpu_state.ccc;
    saved_ccv = cpu_state.ccv;
    saved_ccx = cpu_state.ccx;

    /* There's no reason to think we need to decrement A7 by 32;
     * it's just a paranoid thing to do.
     */
    EM_A7 = (EM_A7 - 32) & ~3; /* Might as well long-align it. */

    for(int i = 0; i < nInterrupts; i++)
    {
        if(!interruptTriggered[i].test_and_set())
            (*interruptFunctions[i])();
    }

    memcpy(&cpu_state.regs, saved_regs, sizeof saved_regs);
    cpu_state.ccnz = saved_ccnz;
    cpu_state.ccn = saved_ccn;
    cpu_state.ccc = saved_ccc;
    cpu_state.ccv = saved_ccv;
    cpu_state.ccx = saved_ccx;
}

syn68k_addr_t Interrupt::handle68K(syn68k_addr_t interrupt_pc, void *unused)
{
    currentCPUMode = CPUMode::m68k;
    currentM68KPC = *ptr_from_longint<GUEST<uint32_t>*>(EM_A7 + 2);

    handleCommon();

    if(base::Debugger::instance && base::Debugger::instance->interruptRequested())
        return base::Debugger::instance->nmi68K(interrupt_pc);

    return MAGIC_RTE_ADDRESS;
}

void Interrupt::handlePowerPC(PowerCore& cpu)
{
    currentCPUMode = CPUMode::ppc;

    PowerCoreState saveState = (PowerCoreState&)cpu;

    cpu.r[1] -= 128;
    EM_A7 = cpu.r[1];
    handleCommon();
    cpu.r[1] += 128;
    EM_A7 = cpu.r[1];

    (PowerCoreState&)cpu = saveState;

    if(base::Debugger::instance && base::Debugger::instance->interruptRequested())
        base::Debugger::instance->nmiPPC();
}

void Executor::syncint_check_interrupt()
{
    if(INTERRUPT_PENDING())
    {
        syn68k_addr_t pc;

        pc = interrupt_process_any_pending(MAGIC_EXIT_EMULATOR_ADDRESS);
        if(pc != MAGIC_EXIT_EMULATOR_ADDRESS)
        {
            interpret_code(hash_lookup_code_and_create_if_needed(pc));
        }
    }
}

SyncintTimer::SyncintTimer()
{
#if USE_TIMER_THREAD
    last_interrupt = scheduled_interrupt = clock::now();
#else
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
#endif
}

void SyncintTimer::start()
{
#if USE_TIMER_THREAD
    thread = std::thread([this] {
        tracy::SetThreadName("timer");
        std::unique_lock<std::mutex> lock(mutex);
        while(!terminate)
        {
            if(scheduled_interrupt > last_interrupt)
            {
                auto status = cond.wait_until(lock, scheduled_interrupt);
                if(status == std::cv_status::timeout)
                {
                    last_interrupt = scheduled_interrupt;

                    timerInterrupt.trigger();
                }
            }
            else
            {
                cond.wait(lock);
            }
        }
    });
#else
    struct sigaction s;

    s.sa_handler = [](int n) {
        timerInterrupt.trigger();
    };
    sigemptyset(&s.sa_mask);
    s.sa_flags = 0;
    sigaction(SIGALRM, &s, nullptr);

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &sigs, NULL);

    waitingThread = pthread_self();
#endif
}

SyncintTimer::~SyncintTimer()
{
#if USE_TIMER_THREAD
    if(thread.joinable())
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            terminate = true;
        }
        thread.join();
    }
#endif
}

void SyncintTimer::post(std::chrono::microseconds usecs, bool fromLast)
{
#if USE_TIMER_THREAD
    std::unique_lock<std::mutex> lock(mutex);

    auto time = (fromLast ? last_interrupt : clock::now()) + usecs;

    if(time <= last_interrupt)
        last_interrupt = clock::time_point();
    if(scheduled_interrupt <= last_interrupt || time < scheduled_interrupt)
        scheduled_interrupt = time;
    
    cond.notify_one();
#else
    struct itimerval t;

    t.it_value.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(usecs).count();
    t.it_value.tv_usec = usecs.count() % 1000000;
    t.it_interval.tv_sec = 0;
    t.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &t, nullptr);
#endif
}

void SyncintTimer::wait()
{
#if USE_TIMER_THREAD
    using namespace std::literals::chrono_literals;

    std::unique_lock<std::mutex> lock(mutex);
    wake_cond.wait(lock, []() { return INTERRUPT_PENDING(); });

    // unlock here, in case the subsequent call to syncint_check_interrupt()
    // triggers a call to syncint_post(), which will need to lock 'mutex'.
    lock.unlock();
#else
    sigset_t zero_mask;
    sigemptyset(&zero_mask);
    sigsuspend(&zero_mask);
#endif
}

void Executor::syncint_init()
{
    /* Set up the trap vector for the timer interrupt. */
    *(GUEST<syn68k_addr_t> *)SYN68K_TO_US(M68K_TIMER_VECTOR * 4) = 
        callback_install(&Interrupt::handle68K, nullptr);

    getPowerCore().handleInterrupt = &Interrupt::handlePowerPC;

    for(auto& flag : interruptTriggered)
        flag.test_and_set();

    timerInterrupt = Interrupt(&timeInterruptHandler);
    timer.start();
}

void Executor::syncint_wait_interrupt()
{
    timer.wait();
    syncint_check_interrupt();
}

void Executor::syncint_post(std::chrono::microseconds usecs, bool fromLast)
{
    timer.post(usecs, fromLast);
}
