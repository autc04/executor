#pragma once

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <cstdint>
#include <string>

namespace Executor::mcp
{

/* One control-socket connection to a running Executor session. */
class Session
{
public:
    Session() = default;

    /* Connects to an existing session; false on failure. */
    bool connect(const std::string &endpoint);

    /* Sends one control request and returns the reply. */
    boost::json::object request(boost::json::object command);

private:
    boost::asio::awaitable<boost::json::object> requestAsync(boost::json::object command);

    boost::asio::io_context ioc_;
    boost::asio::generic::stream_protocol::socket socket_{ioc_};
    boost::asio::streambuf buffer_;
    int64_t nextId_ = 1;
};

}
