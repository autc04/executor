#include "session.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace json = boost::json;

namespace Executor::mcp
{

#ifndef _WIN32

namespace
{

int connectEndpoint(const std::string &endpoint)
{
    if(endpoint.rfind("unix:", 0) == 0)
    {
        std::string path = endpoint.substr(5);

        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if(fd < 0)
            return -1;

        sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());

        if(::connect(fd, (sockaddr *)&addr, sizeof addr) != 0)
        {
            ::close(fd);
            return -1;
        }

        return fd;
    }

    if(endpoint.rfind("tcp:", 0) == 0)
    {
        std::string rest = endpoint.substr(4);
        auto colon = rest.rfind(':');
        std::string host = colon == std::string::npos ? rest : rest.substr(0, colon);
        std::string port = colon == std::string::npos ? "0" : rest.substr(colon + 1);

        if(host.empty())
            host = "127.0.0.1";

        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *results = nullptr;
        if(::getaddrinfo(host.c_str(), port.c_str(), &hints, &results) != 0)
            return -1;

        int fd = -1;
        for(addrinfo *it = results; it; it = it->ai_next)
        {
            fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if(fd < 0)
                continue;

            if(::connect(fd, it->ai_addr, it->ai_addrlen) == 0)
                break;

            ::close(fd);
            fd = -1;
        }

        ::freeaddrinfo(results);
        return fd;
    }

    return -1;
}

int freeTcpPort()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        return 0;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if(::bind(fd, (sockaddr *)&addr, sizeof addr) != 0)
    {
        ::close(fd);
        return 0;
    }

    socklen_t len = sizeof addr;
    ::getsockname(fd, (sockaddr *)&addr, &len);
    ::close(fd);

    return ntohs(addr.sin_port);
}

}

#endif /* !_WIN32 */

Session::~Session()
{
    shutdownChild();

#ifndef _WIN32
    if(fd_ >= 0)
        ::close(fd_);
#endif
}

Session::Session(Session &&other) noexcept
    : fd_(other.fd_), child_(other.child_),
      buffer_(std::move(other.buffer_)), nextId_(other.nextId_)
{
    other.fd_ = -1;
    other.child_ = -1;
}

Session &Session::operator=(Session &&other) noexcept
{
    if(this != &other)
    {
        shutdownChild();

#ifndef _WIN32
        if(fd_ >= 0)
            ::close(fd_);
#endif

        fd_ = other.fd_;
        child_ = other.child_;
        buffer_ = std::move(other.buffer_);
        nextId_ = other.nextId_;

        other.fd_ = -1;
        other.child_ = -1;
    }

    return *this;
}

bool Session::connect(const std::string &endpoint)
{
#ifndef _WIN32
    fd_ = connectEndpoint(endpoint);

    if(fd_ < 0)
        return false;

    /* Do not hang an agent forever if the emulator stops answering. */
    timeval timeout = {};
    timeout.tv_sec = 120;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    return true;
#else
    (void)endpoint;
    return false;
#endif
}

std::optional<Session> Session::spawn(const SpawnOptions &options)
{
#ifndef _WIN32
    int port = freeTcpPort();
    if(port <= 0)
        throw std::runtime_error("cannot allocate a local port");

    std::string endpoint = "tcp:127.0.0.1:" + std::to_string(port);

    std::vector<std::string> args;
    args.push_back(options.executor);
    args.push_back("--headless");
    args.push_back("--control");
    args.push_back(endpoint);

    if(!options.size.empty())
    {
        args.push_back("--size");
        args.push_back(options.size);
    }

    if(!options.bpp.empty())
    {
        args.push_back("--bpp");
        args.push_back(options.bpp);
    }

    for(const std::string &arg : options.appArgs)
        args.push_back(arg);

    pid_t pid = ::fork();

    if(pid < 0)
        throw std::runtime_error("cannot fork");

    if(pid == 0)
    {
        /* The MCP protocol owns stdout; send the emulator's chatter to stderr. */
        ::dup2(STDERR_FILENO, STDOUT_FILENO);

        std::vector<char *> argv;
        for(std::string &arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);

        ::execv(options.executor.c_str(), argv.data());
        ::_exit(127);
    }

    Session session;
    session.child_ = pid;

    for(int i = 0; i < 240; i++) /* up to 60 s */
    {
        int status = 0;
        if(::waitpid(pid, &status, WNOHANG) == pid)
            throw std::runtime_error("executor exited before the control socket appeared");

        if(session.connect(endpoint))
            return session;

        ::usleep(250 * 1000);
    }

    ::kill(pid, SIGTERM);
    throw std::runtime_error("timed out waiting for the control socket");
#else
    (void)options;
    throw std::runtime_error("spawning Executor is not supported on Windows");
#endif
}

std::string Session::readLine()
{
#ifndef _WIN32
    for(;;)
    {
        auto pos = buffer_.find('\n');
        if(pos != std::string::npos)
        {
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);
            return line;
        }

        char chunk[65536];
        ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);

        if(n <= 0)
            throw std::runtime_error("executor closed the control connection");

        buffer_.append(chunk, (size_t)n);
    }
#else
    throw std::runtime_error("no control connection");
#endif
}

void Session::writeAll(const std::string &data)
{
#ifndef _WIN32
    size_t done = 0;

    while(done < data.size())
    {
        ssize_t n = ::send(fd_, data.data() + done, data.size() - done,
                           MSG_NOSIGNAL);
        if(n <= 0)
            throw std::runtime_error("cannot write to the control connection");
        done += (size_t)n;
    }
#else
    (void)data;
    throw std::runtime_error("no control connection");
#endif
}

json::object Session::request(json::object command)
{
    command["id"] = nextId_++;
    writeAll(json::serialize(command) + "\n");

    std::string line = readLine();

    boost::system::error_code ec;
    json::value value = json::parse(line, ec);

    if(ec || !value.is_object())
        throw std::runtime_error("executor sent a malformed reply");

    return value.as_object();
}

void Session::shutdownChild()
{
#ifndef _WIN32
    if(child_ > 0)
    {
        ::kill((pid_t)child_, SIGTERM);
        ::waitpid((pid_t)child_, nullptr, 0);
        child_ = -1;
    }
#endif
}

}
