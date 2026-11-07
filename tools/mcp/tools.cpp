#include "tools.h"

#include "base64.h"
#include "png.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace json = boost::json;

namespace Executor::mcp
{

namespace
{

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

json::object notAttached()
{
    return toolError("not attached to an Executor session: call the attach tool "
                     "with an endpoint first");
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

/* Attaches to `endpoint`, validates it with hello and replaces the current
 * session.  A failed attach leaves the server unattached. */
json::object attachSession(ToolState &state, const json::object &args)
{
    if(!args.contains("endpoint") || !args.at("endpoint").is_string())
        return toolError("attach needs an \"endpoint\" string");

    std::string endpoint(args.at("endpoint").as_string());

    try
    {
        state.session.connect(endpoint);

        json::object hello = state.session.request(json::object{ { "cmd", "hello" } });

        if(!hello.contains("ok") || !hello.at("ok").as_bool())
        {
            state.session.close();
            return toolError("executor rejected the hello request");
        }

        state.endpoint = endpoint;
        hello["endpoint"] = endpoint;

        return toolReply(hello);
    }
    catch(const std::exception &e)
    {
        state.session.close();
        state.endpoint.clear();
        return toolError(std::string("cannot attach: ") + e.what());
    }
}

}

json::array toolList()
{
    json::array tools;

    tools.push_back(toolDefinition(
        "attach",
        "Attach to a running Executor session. Must be called before any other "
        "tool. endpoint is \"unix:<path>\" or \"tcp:<host>:<port>\" (loopback "
        "only); the reply describes the attached session.",
        { { "endpoint", prop("string") } }, { "endpoint" }));

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

json::object callTool(ToolState &state, const std::string &name,
                      const json::object &args)
{
    if(name == "attach")
        return attachSession(state, args);

    if(!state.attached())
        return notAttached();

    /* Any I/O failure means the attached session is gone; drop it so the agent
     * can attach again rather than reusing a dead socket. */
    auto request = [&state](json::object command) {
        try
        {
            return state.session.request(std::move(command));
        }
        catch(const std::exception &e)
        {
            state.session.close();
            state.endpoint.clear();
            throw std::runtime_error(std::string("session ended: ") + e.what());
        }
    };

    auto forwarded = [&](json::object command) {
        return toolReply(request(std::move(command)));
    };

    if(name == "screenshot")
    {
        json::object command;
        command["cmd"] = "frame";

        if(const json::value *region = args.if_contains("region");
           region && region->is_array())
            command["rect"] = *region;

        json::object reply = request(command);

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

        std::string png = encodePngRGBA(outWidth, outHeight, rgba);

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
    {
        json::object reply = request(json::object{ { "cmd", "status" } });
        reply["endpoint"] = state.endpoint;
        return toolReply(reply);
    }

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
            request(std::move(move));
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

}
