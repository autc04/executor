#pragma once

#include <string>
#include <vector>

namespace Executor
{
    void InitMonDebugger();

    // Run debugger commands non-interactively (see mon_debugger.cpp for the
    // command set).  Used for the --debug-cmd command-line option; the
    // EXECUTOR_DBG_INIT environment variable is handled by InitMonDebugger().
    void RunDebuggerCommands(const std::vector<std::string>& commands);
}
