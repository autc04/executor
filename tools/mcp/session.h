#pragma once

#include <boost/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Executor::mcp
{

struct SpawnOptions
{
    std::string executor;
    std::string size;
    std::string bpp;
    std::vector<std::string> appArgs;
};

/* One control-socket connection to an Executor session. */
class Session
{
public:
    Session() = default;
    ~Session();

    Session(Session &&other) noexcept;
    Session &operator=(Session &&other) noexcept;

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    /* Connects to an existing session; false on failure. */
    bool connect(const std::string &endpoint);

    /* Spawns Executor and connects to its control socket. */
    static std::optional<Session> spawn(const SpawnOptions &options);

    /* Sends one control request and returns the reply. */
    boost::json::object request(boost::json::object command);

private:
    std::string readLine();
    void writeAll(const std::string &data);
    void shutdownChild();

    int fd_ = -1;
    int child_ = -1;
    std::string buffer_;
    int64_t nextId_ = 1;
};

}
