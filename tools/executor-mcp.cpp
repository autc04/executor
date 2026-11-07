/* executor-mcp -- a small MCP (Model Context Protocol) server that lets an AI
 * agent drive a running Executor session.
 *
 * It is the client of Executor's control socket (--control), not of the
 * guest.  See docs/ai/2026-11-06-agent-control-design.md.
 *
 *   executor-mcp --attach unix:/run/user/1000/executor/1234.sock
 *   executor-mcp --spawn build/executor --size 1024x768 --bpp 8 -- tests.ad
 *
 * MCP messages are newline-delimited JSON-RPC 2.0 on stdin/stdout.  Nothing
 * else may be written to stdout; the spawned Executor's stdout is redirected to
 * stderr.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

namespace json = boost::json;

#ifndef _WIN32

namespace
{

/* ------------------------------------------------------------------ base64 */

const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string &bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);

    size_t i = 0;
    for(; i + 2 < bytes.size(); i += 3)
    {
        uint32_t v = (uint8_t)bytes[i] << 16 | (uint8_t)bytes[i + 1] << 8
                   | (uint8_t)bytes[i + 2];
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += kBase64[(v >> 6) & 63];
        out += kBase64[v & 63];
    }

    if(i + 1 == bytes.size())
    {
        uint32_t v = (uint8_t)bytes[i] << 16;
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += "==";
    }
    else if(i + 2 == bytes.size())
    {
        uint32_t v = (uint8_t)bytes[i] << 16 | (uint8_t)bytes[i + 1] << 8;
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += kBase64[(v >> 6) & 63];
        out += '=';
    }

    return out;
}

std::string base64Decode(const std::string &text)
{
    static int table[256];
    static bool initialized = false;

    if(!initialized)
    {
        std::fill(std::begin(table), std::end(table), -1);
        for(int i = 0; i < 64; i++)
            table[(uint8_t)kBase64[i]] = i;
        initialized = true;
    }

    std::string out;
    out.reserve(text.size() / 4 * 3);

    uint32_t acc = 0;
    int bits = 0;

    for(char c : text)
    {
        int v = table[(uint8_t)c];

        if(v < 0)
            continue; /* padding and whitespace */

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;

        if(bits >= 8)
        {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }

    return out;
}

/* --------------------------------------------------------------------- PNG */

uint32_t crc32(const std::string &data, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool initialized = false;

    if(!initialized)
    {
        for(uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for(int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        initialized = true;
    }

    crc = ~crc;

    for(uint8_t b : data)
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);

    return ~crc;
}

uint32_t adler32(const std::string &data)
{
    uint32_t a = 1, b = 0;

    for(uint8_t byte : data)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }

    return (b << 16) | a;
}

void appendBE32(std::string &out, uint32_t v)
{
    out += (char)((v >> 24) & 0xFF);
    out += (char)((v >> 16) & 0xFF);
    out += (char)((v >> 8) & 0xFF);
    out += (char)(v & 0xFF);
}

void appendChunk(std::string &png, const char *type, const std::string &data)
{
    appendBE32(png, (uint32_t)data.size());

    std::string typed(type, 4);
    typed += data;

    png += typed;
    appendBE32(png, crc32(typed));
}

/* A zlib stream for the PNG IDAT chunk: real compression when zlib is
 * available, otherwise stored (uncompressed) deflate blocks, which is valid
 * and needs no dependency. */
std::string deflateZlib(const std::string &raw)
{
#ifdef HAVE_ZLIB
    uLongf bound = compressBound(raw.size());
    std::string out;
    out.resize(bound);

    if(compress2(reinterpret_cast<Bytef *>(out.data()), &bound,
                 reinterpret_cast<const Bytef *>(raw.data()), raw.size(), 6)
       == Z_OK)
    {
        out.resize(bound);
        return out;
    }
    /* fall through to the stored form */
#endif

    std::string z;
    z += (char)0x78; /* zlib header, no preset dictionary */
    z += (char)0x01;

    size_t offset = 0;
    while(offset < raw.size())
    {
        size_t n = std::min<size_t>(65535, raw.size() - offset);
        bool last = offset + n == raw.size();

        z += (char)(last ? 1 : 0);
        z += (char)(n & 0xFF);
        z += (char)((n >> 8) & 0xFF);
        z += (char)((~n) & 0xFF);
        z += (char)(((~n) >> 8) & 0xFF);
        z.append(raw, offset, n);
        offset += n;
    }

    appendBE32(z, adler32(raw));
    return z;
}

/* A valid PNG; the IDAT stream is zlib-compressed when zlib is present. */
std::string pngRGBA(int width, int height, const std::string &rgba)
{
    std::string raw;
    raw.reserve((size_t)height * (1 + (size_t)width * 4));

    for(int y = 0; y < height; y++)
    {
        raw += '\0'; /* filter type 0 */
        raw.append(rgba, (size_t)y * width * 4, (size_t)width * 4);
    }

    std::string z = deflateZlib(raw);

    std::string ihdr;
    appendBE32(ihdr, (uint32_t)width);
    appendBE32(ihdr, (uint32_t)height);
    ihdr += (char)8; /* bit depth */
    ihdr += (char)6; /* colour type: RGBA */
    ihdr += (char)0; /* compression */
    ihdr += (char)0; /* filter */
    ihdr += (char)0; /* interlace */

    std::string png = "\x89PNG\r\n\x1a\n";
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", z);
    appendChunk(png, "IEND", std::string());

    return png;
}

std::string downscaleRGBA(const std::string &rgba, int width, int height,
                          int maxWidth, int &outWidth, int &outHeight)
{
    if(maxWidth <= 0 || width <= maxWidth)
    {
        outWidth = width;
        outHeight = height;
        return rgba;
    }

    outWidth = maxWidth;
    outHeight = std::max(1, (int)((int64_t)height * maxWidth / width));

    std::string out;
    out.resize((size_t)outWidth * outHeight * 4);

    for(int y = 0; y < outHeight; y++)
    {
        int sy = (int)((int64_t)y * height / outHeight);

        for(int x = 0; x < outWidth; x++)
        {
            int sx = (int)((int64_t)x * width / outWidth);
            std::memcpy(&out[((size_t)y * outWidth + x) * 4],
                        &rgba[((size_t)sy * width + sx) * 4], 4);
        }
    }

    return out;
}

/* ----------------------------------------------------------------- session */

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

class Session
{
public:
    std::optional<int> connect(const std::string &endpoint);

    json::object request(json::object command);
    void setChild(pid_t pid) { child_ = pid; }
    void shutdownChild();
    bool isOpen() const { return fd_ >= 0; }

private:
    std::string readLine();
    void writeAll(const std::string &data);

    int fd_ = -1;
    pid_t child_ = -1;
    std::string buffer_;
    int64_t nextId_ = 1;
};

std::optional<int> Session::connect(const std::string &endpoint)
{
    fd_ = connectEndpoint(endpoint);

    if(fd_ < 0)
        return std::nullopt;

    /* Do not hang an agent forever if the emulator stops answering. */
    timeval timeout = {};
    timeout.tv_sec = 120;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    return fd_;
}

std::string Session::readLine()
{
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
}

void Session::writeAll(const std::string &data)
{
    size_t done = 0;

    while(done < data.size())
    {
        ssize_t n = ::send(fd_, data.data() + done, data.size() - done,
                           MSG_NOSIGNAL);
        if(n <= 0)
            throw std::runtime_error("cannot write to the control connection");
        done += (size_t)n;
    }
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
    if(child_ > 0)
    {
        ::kill(child_, SIGTERM);
        ::waitpid(child_, nullptr, 0);
        child_ = -1;
    }
}

/* ------------------------------------------------------------------- tools */

json::object textContent(const std::string &text)
{
    json::object o;
    o["type"] = "text";
    o["text"] = text;
    return o;
}

json::object toolReply(const json::object &controlReply)
{
    json::object reply = controlReply;
    reply.erase("id"); /* the control protocol's correlation id */

    json::array content;
    content.push_back(textContent(json::serialize(reply)));

    json::object result;
    result["content"] = std::move(content);
    return result;
}

json::object toolError(const std::string &message)
{
    json::object result;
    json::array content;
    content.push_back(textContent(message));
    result["content"] = std::move(content);
    result["isError"] = true;
    return result;
}

json::object callTool(Session &session, const std::string &name,
                      const json::object &args)
{
    auto forwarded = [&](json::object command) {
        return toolReply(session.request(std::move(command)));
    };

    if(name == "screenshot")
    {
        json::object command;
        command["cmd"] = "frame";

        if(const json::value *region = args.if_contains("region");
           region && region->is_array())
            command["rect"] = *region;

        json::object reply = session.request(command);

        if(!reply.contains("ok") || !reply.at("ok").as_bool())
            return toolReply(reply);

        int width = (int)reply["width"].as_int64();
        int height = (int)reply["height"].as_int64();
        std::string rgba = base64Decode(std::string(reply["data"].as_string()));

        int maxWidth = 0;
        if(const json::value *v = args.if_contains("max_width");
           v && v->is_int64())
            maxWidth = (int)v->as_int64();

        int outWidth, outHeight;
        rgba = downscaleRGBA(rgba, width, height, maxWidth, outWidth, outHeight);

        std::string png = pngRGBA(outWidth, outHeight, rgba);

        json::object meta;
        meta["width"] = outWidth;
        meta["height"] = outHeight;
        meta["screen_width"] = width;
        meta["screen_height"] = height;
        meta["format"] = "png";

        json::array content;
        content.push_back(textContent(json::serialize(meta)));

        json::object image;
        image["type"] = "image";
        image["data"] = base64Encode(png);
        image["mimeType"] = "image/png";
        content.push_back(std::move(image));

        json::object result;
        result["content"] = std::move(content);
        return result;
    }

    if(name == "status")
        return forwarded(json::object{ { "cmd", "status" } });

    if(name == "move")
        return forwarded(json::object{ { "cmd", "move" },
                                       { "x", args.at("x") },
                                       { "y", args.at("y") } });

    if(name == "click")
    {
        json::object command;
        command["cmd"] = "click";
        command["x"] = args.at("x");
        command["y"] = args.at("y");
        if(const json::value *v = args.if_contains("count"); v && v->is_int64())
            command["count"] = *v;
        return forwarded(std::move(command));
    }

    if(name == "button_down" || name == "button_up")
    {
        if(const json::value *x = args.if_contains("x"); x && x->is_int64())
        {
            json::object move;
            move["cmd"] = "move";
            move["x"] = *x;
            move["y"] = args.at("y");
            session.request(std::move(move));
        }

        json::object command;
        command["cmd"] = name == "button_down" ? "down" : "up";
        if(const json::value *b = args.if_contains("button"); b && b->is_string())
            command["button"] = *b;
        return forwarded(std::move(command));
    }

    if(name == "drag")
    {
        json::object command;
        command["cmd"] = "drag";
        command["from"] = args.at("from");
        command["to"] = args.at("to");
        if(const json::value *v = args.if_contains("duration_ms"); v && v->is_int64())
            command["duration_ms"] = *v;
        return forwarded(std::move(command));
    }

    if(name == "type_text")
    {
        json::object command;
        command["cmd"] = "type";
        command["text"] = args.at("text");
        return forwarded(std::move(command));
    }

    if(name == "press_key" || name == "key_down" || name == "key_up")
    {
        json::object command;
        command["cmd"] = name == "press_key" ? "key"
                       : name == "key_down" ? "keydown"
                                            : "keyup";
        command["name"] = args.at("name");
        if(const json::value *v = args.if_contains("modifiers"); v && v->is_array())
            command["modifiers"] = *v;
        return forwarded(std::move(command));
    }

    if(name == "wait")
        return forwarded(json::object{ { "cmd", "wait" }, { "ms", args.at("ms") } });

    if(name == "quit")
    {
        json::object command;
        command["cmd"] = "quit";
        if(const json::value *v = args.if_contains("force"); v && v->is_bool())
            command["force"] = *v;
        return forwarded(std::move(command));
    }

    return toolError("unknown tool: " + name);
}

/* -------------------------------------------------------------- MCP server */

json::object prop(const char *type)
{
    json::object o;
    o["type"] = type;
    return o;
}

json::object integerProperty(const char *description)
{
    json::object o = prop("integer");
    o["description"] = description;
    return o;
}

json::object pointProperty(const char *description)
{
    json::object o = prop("array");
    o["description"] = description;
    o["items"] = json::object{ { "type", "integer" } };
    o["minItems"] = 2;
    o["maxItems"] = 2;
    return o;
}

json::object stringArrayProperty(const char *description)
{
    json::object o = prop("array");
    o["description"] = description;
    o["items"] = json::object{ { "type", "string" } };
    return o;
}

json::object toolDefinition(const char *name, const char *description,
                            json::object properties,
                            json::array required = {})
{
    json::object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(properties);
    if(!required.empty())
        schema["required"] = std::move(required);

    json::object tool;
    tool["name"] = name;
    tool["description"] = description;
    tool["inputSchema"] = std::move(schema);
    return tool;
}

json::array toolList()
{
    json::array tools;

    tools.push_back(toolDefinition(
        "screenshot",
        "Capture the emulated Mac screen (or a rectangle of it) as a PNG.",
        { { "region", pointProperty("optional [left, top, right, bottom]") },
          { "max_width", integerProperty("downscale so the image is at most this wide") } }));

    tools.push_back(toolDefinition(
        "status",
        "Tick count, pointer position, button/modifier state, whether the guest "
        "recently pumped its event loop, front window title and whether input is busy.",
        {}));

    tools.push_back(toolDefinition(
        "click",
        "Move the pointer to (x, y) and click. Use count=2 for a double click.",
        { { "x", integerProperty("guest screen x") },
          { "y", integerProperty("guest screen y") },
          { "count", integerProperty("number of clicks (default 1)") } },
        { "x", "y" }));

    tools.push_back(toolDefinition(
        "move", "Move the pointer to (x, y) without pressing a button.",
        { { "x", integerProperty("guest screen x") },
          { "y", integerProperty("guest screen y") } },
        { "x", "y" }));

    tools.push_back(toolDefinition(
        "button_down", "Press and hold the mouse button, optionally at (x, y).",
        { { "x", integerProperty("optional guest screen x") },
          { "y", integerProperty("optional guest screen y") } }));

    tools.push_back(toolDefinition(
        "button_up", "Release the mouse button.",
        { { "x", integerProperty("optional guest screen x") },
          { "y", integerProperty("optional guest screen y") } }));

    tools.push_back(toolDefinition(
        "drag", "Press at `from`, move to `to` over duration_ms, then release.",
        { { "from", pointProperty("[x, y]") },
          { "to", pointProperty("[x, y]") },
          { "duration_ms", integerProperty("default 300") } },
        { "from", "to" }));

    tools.push_back(toolDefinition(
        "type_text", "Type printable ASCII text.",
        { { "text", prop("string") } }, { "text" }));

    tools.push_back(toolDefinition(
        "press_key", "Press and release a key by name (e.g. Return, Tab, F5) or character.",
        { { "name", prop("string") },
          { "modifiers", stringArrayProperty("shift, cmd, opt, ctrl, caps") } },
        { "name" }));

    tools.push_back(toolDefinition(
        "key_down", "Press and hold a key.",
        { { "name", prop("string") } }, { "name" }));

    tools.push_back(toolDefinition(
        "key_up", "Release a key.",
        { { "name", prop("string") } }, { "name" }));

    tools.push_back(toolDefinition(
        "wait", "Wait for the given number of milliseconds.",
        { { "ms", integerProperty("milliseconds") } }, { "ms" }));

    tools.push_back(toolDefinition(
        "quit", "Ask the running application to quit (Cmd-Q); force exits the emulator.",
        { { "force", prop("boolean") } }));

    return tools;
}

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
            if(request.contains("params")
               && request["params"].is_object())
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

        if(method == "notifications/initialized" || method.rfind("notifications/", 0) == 0)
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
                writeResult(id, toolError(e.what()));
            }

            continue;
        }

        if(!isNotification)
            writeError(id, -32601, "unknown method: " + method);
    }

    return 0;
}

/* ------------------------------------------------------------------- spawn */

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

struct SpawnOptions
{
    std::string executor;
    std::string size;
    std::string bpp;
    std::vector<std::string> appArgs;
};

Session spawn(const SpawnOptions &options)
{
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

    pid_t child = pid;

    for(int i = 0; i < 240; i++) /* up to 60 s */
    {
        int status = 0;
        if(::waitpid(child, &status, WNOHANG) == child)
            throw std::runtime_error("executor exited before the control socket appeared");

        int fd = connectEndpoint(endpoint);
        if(fd >= 0)
        {
            Session session;
            /* Reconnect through the same object so it owns the fd. */
            ::close(fd);
            if(session.connect(endpoint))
            {
                session.setChild(child);
                return session;
            }
        }

        ::usleep(250 * 1000);
    }

    ::kill(child, SIGTERM);
    throw std::runtime_error("timed out waiting for the control socket");
}

void usage()
{
    std::fprintf(stderr,
        "usage: executor-mcp --attach <endpoint>\n"
        "       executor-mcp --spawn <executor> [--size WxH] [--bpp N] [-- app args...]\n"
        "  endpoint: unix:<path> or tcp:<host>:<port>\n");
}

} /* namespace */

int main(int argc, char **argv)
{
    std::string attach;
    SpawnOptions spawnOptions;

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
            usage();
            return 2;
        }
    }

    try
    {
        if(!attach.empty())
        {
            Session session;
            if(!session.connect(attach))
            {
                std::fprintf(stderr, "executor-mcp: cannot connect to %s\n",
                             attach.c_str());
                return 1;
            }
            return runMcp(session);
        }

        if(!spawnOptions.executor.empty())
        {
            Session session = spawn(spawnOptions);
            int rc = runMcp(session);
            session.shutdownChild();
            return rc;
        }

        usage();
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
