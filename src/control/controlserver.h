#pragma once

#include <string>

namespace Executor::control
{

/* Starts the agent control server.  Called once from the emulator thread,
 * after the guest is initialised.  `endpoint` is "auto", "unix:<path>",
 * "tcp:<host>:<port>", or empty/"off".  Failure is reported on stderr and is
 * not fatal. */
void startServer(const std::string &endpoint);

/* Requests shutdown of the control server.  Thread-safe and non-blocking;
 * called by the emulator thread as it unwinds. */
void requestStop();

/* Joins the io thread (and the job queue's timer thread).  Called by main
 * after the emulator thread has exited and before vdriver is destroyed. */
void join();

}
