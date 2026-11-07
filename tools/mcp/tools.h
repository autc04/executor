#pragma once

#include "session.h"

#include <boost/json.hpp>

#include <string>

namespace Executor::mcp
{

/* The MCP server's session state: at most one attached Executor. */
struct ToolState
{
    Session session;
    std::string endpoint; /* empty when not attached */

    bool attached() const { return !endpoint.empty() && session.isOpen(); }
};

/* The tools/list result. */
boost::json::array toolList();

/* Executes one tools/call.  Throws on protocol errors. */
boost::json::object callTool(ToolState &state, const std::string &name,
                             const boost::json::object &args);

}
