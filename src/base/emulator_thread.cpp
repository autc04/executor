#include <base/emulator_thread.h>

#include <time/syncint.h>

#include <mutex>
#include <utility>
#include <vector>

using namespace Executor;

namespace
{
std::mutex todoMutex;
std::vector<std::function<void()>> todo;

/* Initialized lazily by workInterrupt. */
thread_local bool isEmulatorThread = false;

/* Drained at every interrupt safe point.  Registered at static construction
 * time, like the timer interrupt. */
Interrupt workInterrupt(
    [] {
        isEmulatorThread = true;
        std::vector<std::function<void()>> work;

        {
            std::lock_guard lk(todoMutex);
            work.swap(todo);
        }

        for(auto& f : work)
            f();
    });
}

void Executor::runOnEmulatorThread(std::function<void()> f)
{
    if(isEmulatorThread)
    {
        f();
        return;
    }

    {
        std::lock_guard lk(todoMutex);
        todo.push_back(std::move(f));
    }

    workInterrupt.trigger();
}

void Executor::wakeEmulatorThread()
{
    workInterrupt.trigger();
}
