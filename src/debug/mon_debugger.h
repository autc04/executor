#pragma once

#include <string>
#include <vector>

namespace Executor
{
    void InitMonDebugger();

    // Queue debugger commands to run once, at process initialization (see
    // mon_debugger.cpp).  Used for the --debug-cmd command-line option; the
    // EXECUTOR_DBG_INIT environment variable is handled by InitMonDebugger().
    void RunDebuggerCommands(const std::vector<std::string>& commands);
}
