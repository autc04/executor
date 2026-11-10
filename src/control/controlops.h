#pragma once

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <string_view>

namespace Executor::control
{

struct Reply
{
    boost::json::object json; /* at least "ok"; the request's "id" is echoed */
    bool terminate = false;   /* quit force: exit after the reply is written */
};

/* Executes one control request (a single line of JSON).  Must be called from
 * the io thread. */
boost::asio::awaitable<Reply> runVerb(std::string_view requestLine);

}
