#pragma once

#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif

namespace Executor::control
{

/* Threads created from the emulator thread inherit the *unblocked* SIGALRM
 * mask that SyncintTimer::start() installs there.  Threads that must not
 * receive the emulator's timer signal block it explicitly; syncint.cpp assumes
 * only the emulator thread is eligible. */
inline void blockEmulatorSignal()
{
#ifndef _WIN32
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
#endif
}

}
