#include "session.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <istream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace json = boost::json;
namespace asio = boost::asio;

namespace Executor::mcp
{

namespace
{

/* A wedged executor must not hang the MCP server forever. */
constexpr std::chrono::seconds kRequestTimeout{ 120 };

/* Runs an awaitable to completion on this thread and returns its value. */
template<typename T>
T runSync(asio::io_context &ioc, asio::awaitable<T> awaitable)
{
    std::optional<T> value;
    std::exception_ptr error;

    asio::co_spawn(ioc, std::move(awaitable),
                   [&](std::exception_ptr e, T result) {
                       error = e;
                       value = std::move(result);
                   });

    ioc.run();
    ioc.restart();

    if(error)
        std::rethrow_exception(error);

    return std::move(*value);
}

/* Parses "unix:<path>" or "tcp:<host>:<port>" into an address usable by a
 * generic stream socket.  Throws with a user-facing message. */
asio::generic::stream_protocol::endpoint parseEndpoint(const std::string &endpoint)
{
    if(endpoint.rfind("unix:", 0) == 0)
    {
        std::string path = endpoint.substr(5);

        if(path.empty())
            throw std::runtime_error("empty unix socket path");

        return asio::generic::stream_protocol::endpoint(
            asio::local::stream_protocol::endpoint(path));
    }

    if(endpoint.rfind("tcp:", 0) == 0)
    {
        std::string rest = endpoint.substr(4);
        auto colon = rest.rfind(':');
        std::string host = colon == std::string::npos ? rest : rest.substr(0, colon);
        std::string port = colon == std::string::npos ? "" : rest.substr(colon + 1);

        if(host.empty())
            host = "127.0.0.1";

        boost::system::error_code ec;
        auto address = asio::ip::make_address(host, ec);

        if(ec)
            throw std::runtime_error("invalid TCP address: " + host);

        if(!address.is_loopback())
            throw std::runtime_error("only loopback TCP endpoints are allowed");

        long portNumber = std::strtol(port.c_str(), nullptr, 10);

        if(portNumber <= 0 || portNumber > 65535)
            throw std::runtime_error("invalid TCP port: " + port);

        return asio::generic::stream_protocol::endpoint(
            asio::ip::tcp::endpoint(address, (unsigned short)portNumber));
    }

    throw std::runtime_error(
        "endpoint must be unix:<path> or tcp:<host>:<port>");
}

}

void Session::connect(const std::string &endpoint)
{
    auto address = parseEndpoint(endpoint);

    close();

    boost::system::error_code ec;

    socket_.open(address.protocol(), ec);
    if(ec)
        throw std::runtime_error("cannot open socket: " + ec.message());

    socket_.connect(address, ec);

    if(ec)
    {
        close();
        throw std::runtime_error("cannot connect to " + endpoint + ": "
                                 + ec.message());
    }
}

void Session::close()
{
    boost::system::error_code ec;
    socket_.close(ec);

    buffer_.consume(buffer_.size());
}

bool Session::isOpen() const
{
    return socket_.is_open();
}

json::object Session::request(json::object command)
{
    return runSync(ioc_, requestAsync(std::move(command)));
}

asio::awaitable<json::object> Session::requestAsync(json::object command)
{
    command["id"] = nextId_++;
    std::string text = json::serialize(command) + "\n";

    /* Cancel the pending I/O on timeout; this throws below. */
    auto timedOut = std::make_shared<bool>(false);

    asio::steady_timer deadline(co_await asio::this_coro::executor);
    deadline.expires_after(kRequestTimeout);
    deadline.async_wait([this, timedOut](const boost::system::error_code &ec) {
        if(!ec)
        {
            *timedOut = true;
            boost::system::error_code ignored;
            socket_.cancel(ignored);
        }
    });

    boost::system::error_code ec;

    co_await asio::async_write(socket_, asio::buffer(text),
                               asio::redirect_error(asio::use_awaitable, ec));

    if(!ec)
        co_await asio::async_read_until(socket_, buffer_, '\n',
                                        asio::redirect_error(asio::use_awaitable, ec));

    deadline.cancel();

    if(ec)
    {
        if(*timedOut)
            throw std::runtime_error("timed out waiting for executor");

        throw std::runtime_error("executor closed the control connection");
    }

    std::string line;
    {
        std::istream is(&buffer_);
        std::getline(is, line);
    }

    boost::system::error_code parseError;
    json::value value = json::parse(line, parseError);

    if(parseError || !value.is_object())
        throw std::runtime_error("executor sent a malformed reply");

    co_return value.as_object();
}

}
