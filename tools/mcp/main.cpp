/* executor-mcp -- a small MCP (Model Context Protocol) server that lets an AI
 * agent drive a running Executor session.
 *
 * It is the client of Executor's control socket (--control), not of the guest.
 * See docs/ai/2026-11-06-agent-control-design.md.
 *
 *   executor-mcp --attach unix:/run/user/1000/executor/1234.sock
 *   executor-mcp --spawn build/executor --size 1024x768 --bpp 8 -- tests.ad
 *
 * MCP messages are newline-delimited JSON-RPC 2.0 on stdin/stdout.  Nothing
 * else may be written to stdout; the spawned Executor's stdout is redirected to
 * stderr.
 */

#ifndef _WIN32

#include "session.h"
#include "tools.h"

#include <boost/json.hpp>

#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

namespace json = boost::json;

namespace Executor::mcp
{

namespace
{

void writeMessage(const json::object &message)
{
    std::cout << json::serialize(message) << '\n' << std::flush;
}

void writeResult(const json::value &id, json::object result)
{
    json::object message;
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["result"] = std::move(result);
    writeMessage(message);
}

void writeError(const json::value &id, int code, const std::string &text)
{
    json::object error;
    error["code"] = code;
    error["message"] = text;

    json::object message;
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["error"] = std::move(error);
    writeMessage(message);
}

int runMcp(Session &session)
{
    std::string line;

    while(std::getline(std::cin, line))
    {
        if(line.empty())
            continue;

        boost::system::error_code ec;
        json::value value = json::parse(line, ec);

        if(ec || !value.is_object())
        {
            writeError(nullptr, -32700, "parse error");
            continue;
        }

        json::object request = value.as_object();
        json::value id = request.contains("id") ? request["id"] : json::value();

        std::string method;
        if(request.contains("method") && request["method"].is_string())
            method = std::string(request["method"].as_string());

        bool isNotification = !request.contains("id");

        if(method == "initialize")
        {
            json::object result;

            json::value requestedVersion;
            if(request.contains("params") && request["params"].is_object())
            {
                const json::object &params = request["params"].as_object();
                if(const json::value *v = params.if_contains("protocolVersion");
                   v && v->is_string())
                    requestedVersion = *v;
            }

            result["protocolVersion"] = requestedVersion.is_string()
                ? requestedVersion
                : json::value("2025-06-18");

            json::object capabilities;
            capabilities["tools"] = json::object();
            result["capabilities"] = std::move(capabilities);

            json::object info;
            info["name"] = "executor-mcp";
            info["version"] = "1.0";
            result["serverInfo"] = std::move(info);

            writeResult(id, std::move(result));
            continue;
        }

        if(method.rfind("notifications/", 0) == 0)
            continue;

        if(method == "ping")
        {
            writeResult(id, json::object());
            continue;
        }

        if(method == "tools/list")
        {
            json::object result;
            result["tools"] = toolList();
            writeResult(id, std::move(result));
            continue;
        }

        if(method == "tools/call")
        {
            if(!request.contains("params") || !request["params"].is_object())
            {
                writeError(id, -32602, "invalid params");
                continue;
            }

            const json::object &params = request["params"].as_object();

            std::string name;
            if(const json::value *v = params.if_contains("name");
               v && v->is_string())
                name = std::string(v->as_string());

            json::object args;
            if(const json::value *v = params.if_contains("arguments");
               v && v->is_object())
                args = v->as_object();

            if(name.empty())
            {
                writeError(id, -32602, "missing tool name");
                continue;
            }

            try
            {
                writeResult(id, callTool(session, name, args));
            }
            catch(const std::exception &e)
            {
                json::object result;
                json::array content;
                content.push_back(json::object{ { "type", "text" },
                                               { "text", e.what() } });
                result["content"] = std::move(content);
                result["isError"] = true;
                writeResult(id, std::move(result));
            }

            continue;
        }

        if(!isNotification)
            writeError(id, -32601, "unknown method: " + method);
    }

    return 0;
}

void usage()
{
    std::fprintf(stderr,
        "usage: executor-mcp --attach <endpoint>\n"
        "       executor-mcp --spawn <executor> [--size WxH] [--bpp N] [-- app args...]\n"
        "  endpoint: unix:<path> or tcp:<host>:<port>\n");
}

}

}

int main(int argc, char **argv)
{
    std::string attach;
    Executor::mcp::SpawnOptions spawnOptions;

    for(int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        if(arg == "--attach" && i + 1 < argc)
            attach = argv[++i];
        else if(arg == "--spawn" && i + 1 < argc)
            spawnOptions.executor = argv[++i];
        else if(arg == "--size" && i + 1 < argc)
            spawnOptions.size = argv[++i];
        else if(arg == "--bpp" && i + 1 < argc)
            spawnOptions.bpp = argv[++i];
        else if(arg == "--")
        {
            for(int j = i + 1; j < argc; j++)
                spawnOptions.appArgs.push_back(argv[j]);
            break;
        }
        else
        {
            Executor::mcp::usage();
            return 2;
        }
    }

    try
    {
        if(!attach.empty())
        {
            Executor::mcp::Session session;
            if(!session.connect(attach))
            {
                std::fprintf(stderr, "executor-mcp: cannot connect to %s\n",
                             attach.c_str());
                return 1;
            }
            return Executor::mcp::runMcp(session);
        }

        if(!spawnOptions.executor.empty())
        {
            auto session = Executor::mcp::Session::spawn(spawnOptions);
            if(!session)
                return 1;
            return Executor::mcp::runMcp(*session);
        }

        Executor::mcp::usage();
        return 2;
    }
    catch(const std::exception &e)
    {
        std::fprintf(stderr, "executor-mcp: %s\n", e.what());
        return 1;
    }
}

#else /* _WIN32 */

int main(int, char **)
{
    std::fprintf(stderr, "executor-mcp is not supported on Windows yet\n");
    return 1;
}

#endif
