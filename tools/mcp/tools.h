#pragma once

#include <boost/json.hpp>

#include <string>

namespace Executor::mcp
{

class Session;

/* The tools/list result. */
boost::json::array toolList();

/* Executes one tools/call.  Throws on protocol errors. */
boost::json::object callTool(Session &session, const std::string &name,
                             const boost::json::object &args);

}
