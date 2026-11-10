#include <control/controlserver.h>

#include <control/controlops.h>
#include <control/jobqueue.h>
#include <control/thread_util.h>

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <istream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace Executor::control;

namespace
{

namespace asio = boost::asio;
namespace json = boost::json;
namespace fs = std::filesystem;

constexpr std::size_t kMaxRequestLine = 64 * 1024;

using UnixAcceptor = asio::local::stream_protocol::acceptor;
using UnixSocket = asio::local::stream_protocol::socket;
using UnixEndpoint = asio::local::stream_protocol::endpoint;
using TcpAcceptor = asio::ip::tcp::acceptor;
using TcpSocket = asio::ip::tcp::socket;

class ControlServer
{
public:
    explicit ControlServer(std::string endpoint);
    ~ControlServer();

    bool start();
    void requestStop();
    void join();

private:
    enum class Kind
    {
        None,
        Unix,
        Tcp,
    };

    void parseEndpoint(const std::string &endpoint);
    bool startUnix();
    bool startTcp();

    asio::awaitable<void> acceptUnix();
    asio::awaitable<void> acceptTcp();

    template<typename Socket>
    asio::awaitable<void> serve(Socket sock);

    Kind kind_ = Kind::None;
    std::string unixPath_;
    std::string tcpHost_;
    unsigned short tcpPort_ = 0;

    asio::io_context ioc_;
    std::unique_ptr<UnixAcceptor> unixAcceptor_;
    std::unique_ptr<TcpAcceptor> tcpAcceptor_;
    std::thread thread_;
    bool started_ = false;
    bool joined_ = false;
};

ControlServer::ControlServer(std::string endpoint)
{
    parseEndpoint(endpoint);
}

ControlServer::~ControlServer()
{
    requestStop();
    join();
}

void ControlServer::parseEndpoint(const std::string &endpoint)
{
    if(endpoint.empty() || endpoint == "off" || endpoint == "none")
    {
        kind_ = Kind::None;
        return;
    }

    if(endpoint == "auto")
    {
        const char *dir = std::getenv("XDG_RUNTIME_DIR");
        if(!dir || !*dir)
            dir = std::getenv("TMPDIR");
        if(!dir || !*dir)
            dir = "/tmp";

        std::string pid = "0";
#ifndef _WIN32
        pid = std::to_string((long)getpid());
#endif
        unixPath_ = std::string(dir) + "/executor/" + pid + ".sock";
        kind_ = Kind::Unix;
        return;
    }

    if(endpoint.rfind("unix:", 0) == 0)
    {
        unixPath_ = endpoint.substr(5);
        kind_ = Kind::Unix;
        return;
    }

    if(endpoint.rfind("tcp:", 0) == 0)
    {
        std::string rest = endpoint.substr(4);
        auto colon = rest.rfind(':');

        if(colon == std::string::npos)
        {
            tcpHost_ = "127.0.0.1";
            tcpPort_ = (unsigned short)std::atoi(rest.c_str());
        }
        else
        {
            tcpHost_ = rest.substr(0, colon);
            tcpPort_ = (unsigned short)std::atoi(rest.c_str() + colon + 1);
            if(tcpHost_.empty())
                tcpHost_ = "127.0.0.1";
        }

        kind_ = Kind::Tcp;
        return;
    }

    /* Anything else is a socket path. */
    unixPath_ = endpoint;
    kind_ = Kind::Unix;
}

bool ControlServer::startUnix()
{
    boost::system::error_code ec;
    std::error_code ignored;

    if(unixPath_.empty())
        return false;

    fs::create_directories(fs::path(unixPath_).parent_path(), ignored);
    fs::remove(unixPath_, ignored);

    unixAcceptor_ = std::make_unique<UnixAcceptor>(ioc_);
    unixAcceptor_->open(UnixAcceptor::protocol_type(), ec);
    if(ec)
    {
        fprintf(stderr, "control: cannot open %s: %s\n",
                unixPath_.c_str(), ec.message().c_str());
        return false;
    }

    unixAcceptor_->bind(UnixEndpoint(unixPath_), ec);
    if(ec)
    {
        fprintf(stderr, "control: cannot bind %s: %s\n",
                unixPath_.c_str(), ec.message().c_str());
        return false;
    }

    unixAcceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if(ec)
    {
        fprintf(stderr, "control: cannot listen on %s: %s\n",
                unixPath_.c_str(), ec.message().c_str());
        return false;
    }

#ifndef _WIN32
    ::chmod(unixPath_.c_str(), 0600);
#endif

    asio::co_spawn(ioc_, acceptUnix(), asio::detached);
    fprintf(stderr, "control endpoint: unix:%s\n", unixPath_.c_str());
    return true;
}

bool ControlServer::startTcp()
{
    boost::system::error_code ec;

    auto address = asio::ip::make_address(tcpHost_, ec);
    if(ec)
    {
        fprintf(stderr, "control: bad address %s: %s\n",
                tcpHost_.c_str(), ec.message().c_str());
        return false;
    }

    tcpAcceptor_ = std::make_unique<TcpAcceptor>(ioc_);
    tcpAcceptor_->open(asio::ip::tcp::v4(), ec);
    if(ec)
    {
        fprintf(stderr, "control: cannot open tcp socket: %s\n", ec.message().c_str());
        return false;
    }

    tcpAcceptor_->set_option(asio::socket_base::reuse_address(true), ec);

    tcpAcceptor_->bind(asio::ip::tcp::endpoint(address, tcpPort_), ec);
    if(ec)
    {
        fprintf(stderr, "control: cannot bind %s:%u: %s\n",
                tcpHost_.c_str(), (unsigned)tcpPort_, ec.message().c_str());
        return false;
    }

    tcpAcceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if(ec)
    {
        fprintf(stderr, "control: cannot listen: %s\n", ec.message().c_str());
        return false;
    }

    asio::co_spawn(ioc_, acceptTcp(), asio::detached);
    fprintf(stderr, "control endpoint: tcp:%s:%u\n",
            tcpHost_.c_str(), (unsigned)tcpPort_);
    return true;
}

bool ControlServer::start()
{
    if(kind_ == Kind::None)
        return false;

    bool ok = kind_ == Kind::Unix ? startUnix() : startTcp();

    if(!ok)
        return false;

    thread_ = std::thread([this] {
        blockEmulatorSignal();
        ioc_.run();
    });

    started_ = true;
    return true;
}

void ControlServer::requestStop()
{
    if(!started_)
        return;

    asio::post(ioc_, [this] {
        boost::system::error_code ec;

        if(unixAcceptor_)
            unixAcceptor_->close(ec);
        if(tcpAcceptor_)
            tcpAcceptor_->close(ec);

        ioc_.stop();
    });
}

void ControlServer::join()
{
    if(joined_)
        return;

    joined_ = true;

    if(thread_.joinable())
        thread_.join();

#ifndef _WIN32
    if(kind_ == Kind::Unix && !unixPath_.empty())
        ::unlink(unixPath_.c_str());
#endif
}

asio::awaitable<void> ControlServer::acceptUnix()
{
    for(;;)
    {
        boost::system::error_code ec;
        UnixSocket sock(ioc_);

        co_await unixAcceptor_->async_accept(
            sock, asio::redirect_error(asio::use_awaitable, ec));

        if(ec)
            co_return;

        asio::co_spawn(ioc_, serve(std::move(sock)), asio::detached);
    }
}

asio::awaitable<void> ControlServer::acceptTcp()
{
    for(;;)
    {
        boost::system::error_code ec;
        TcpSocket sock(ioc_);

        co_await tcpAcceptor_->async_accept(
            sock, asio::redirect_error(asio::use_awaitable, ec));

        if(ec)
            co_return;

        asio::co_spawn(ioc_, serve(std::move(sock)), asio::detached);
    }
}

template<typename Socket>
asio::awaitable<void> ControlServer::serve(Socket sock)
{
    asio::streambuf buf(kMaxRequestLine);

    for(;;)
    {
        boost::system::error_code ec;

        co_await asio::async_read_until(
            sock, buf, '\n', asio::redirect_error(asio::use_awaitable, ec));

        if(ec)
        {
            if(ec == asio::error::not_found)
            {
                static const char tooLong[] =
                    "{\"ok\":false,\"code\":\"bad-request\","
                    "\"error\":\"request line too long\"}\n";
                boost::system::error_code ignored;

                co_await asio::async_write(
                    sock, asio::buffer(tooLong, sizeof(tooLong) - 1),
                    asio::redirect_error(asio::use_awaitable, ignored));
            }

            co_return;
        }

        std::string line;
        {
            std::istream is(&buf);
            std::getline(is, line);
        }

        Reply reply = co_await runVerb(line);

        std::string text = json::serialize(reply.json);
        text += '\n';

        co_await asio::async_write(
            sock, asio::buffer(text),
            asio::redirect_error(asio::use_awaitable, ec));

        if(ec)
            co_return;

        if(reply.terminate)
            std::_Exit(0);
    }
}

std::unique_ptr<ControlServer> server;

}

void Executor::control::startServer(const std::string &endpoint)
{
    if(endpoint.empty() || endpoint == "off" || endpoint == "none")
        return;

    auto srv = std::make_unique<ControlServer>(endpoint);

    if(!srv->start())
        return;

    server = std::move(srv);

    JobQueue::instance().start();
}

void Executor::control::requestStop()
{
    if(server)
        server->requestStop();
}

void Executor::control::join()
{
    if(server)
    {
        server->requestStop();
        server->join();
        server.reset();
    }

    JobQueue::instance().stop();
}
