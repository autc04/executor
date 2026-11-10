#include "tools.h"

#include "base64.h"
#include "png.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

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

/* A control reply with ok:false is a failed call, not a result whose text
 * happens to mention an error: keep isError honest so a client can branch on
 * it without parsing. */
json::object resultOrError(const json::object &controlReply)
{
    const json::value *ok = controlReply.if_contains("ok");

    if(ok && ok->is_bool() && ok->as_bool())
        return toolReply(controlReply);

    std::string text = "executor: ";

    if(const json::value *code = controlReply.if_contains("code");
       code && code->is_string())
        text += std::string(code->as_string()) + ": ";

    if(const json::value *error = controlReply.if_contains("error");
       error && error->is_string())
        text += std::string(error->as_string());
    else
        text += "the request failed without an error message";

    return toolError(text);
}

bool intArgument(const json::object &args, const char *name, long &out)
{
    const json::value *v = args.if_contains(name);

    if(!v || !v->is_int64())
        return false;

    out = (long)v->as_int64();
    return true;
}

bool pairArgument(const json::object &args, const char *name, long &a, long &b)
{
    const json::value *v = args.if_contains(name);

    if(!v || !v->is_array())
        return false;

    const json::array &arr = v->as_array();

    if(arr.size() != 2 || !arr[0].is_int64() || !arr[1].is_int64())
        return false;

    a = (long)arr[0].as_int64();
    b = (long)arr[1].as_int64();
    return true;
}

bool stringArgument(const json::object &args, const char *name, std::string &out)
{
    const json::value *v = args.if_contains(name);

    if(!v || !v->is_string())
        return false;

    out = std::string(v->as_string());
    return true;
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

json::object rectProperty(const char *description)
{
    json::object o = prop("array");
    o["description"] = description;
    o["items"] = json::object{ { "type", "integer" } };
    o["minItems"] = 4;
    o["maxItems"] = 4;
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
        "Attach to a running Executor session; nothing else works before this. "
        "`endpoint` is \"unix:/path/to/socket\" (Executor prints it at startup, "
        "usually $XDG_RUNTIME_DIR/executor/<pid>.sock, so it has to be passed "
        "in) or \"tcp:<host>:<port>\" on loopback. Replaces any previous "
        "session. The reply repeats the session's hello: proto, app (the "
        "argument Executor was launched with, often empty and not the "
        "application the guest is running), screen width/height/bpp, tick, and "
        "suspend_events (whether losing focus tells the application it has gone "
        "to the background - false when Executor was started with "
        "--no-suspend-events, which is what an agent driving an interactive "
        "session wants).",
        { { "endpoint", prop("string") } }, { "endpoint" }));

    tools.push_back(toolDefinition(
        "screenshot",
        "Capture the emulated Mac screen, or the rectangle `region` = "
        "[left, top, right, bottom] in guest pixels, as a PNG plus a JSON "
        "metadata block (width, height, screen_width, screen_height, mouse "
        "[x,y], button). The Mac's own pointer is not drawn into the image, so "
        "read `mouse` to know where it is. Coordinates are 1:1 with the screen: "
        "crop with `region` to inspect detail, because `max_width` resamples "
        "with nearest neighbour and makes 9-pixel Mac text unreadable. Take a "
        "screenshot after the guest has had time to redraw.",
        { { "region", rectProperty("optional [left, top, right, bottom] in guest "
                                   "pixels; defaults to the whole screen") },
          { "max_width", integerProperty("optional downscale so the image is at "
                                         "most this wide") } }));

    tools.push_back(toolDefinition(
        "status",
        "State of the emulated Mac, without capturing anything. Fields: tick "
        "(60 per second, guest clock); app (the one application Executor is "
        "running - there is no application switching, so this is what every "
        "click and key goes to); front (title of its front window, empty when "
        "there is none); "
        "mouse [x,y] and button (true while the button is held); modifiers "
        "{shift, cmd, option, control, caps}; modifiers_raw; dbltime (the "
        "double-click threshold in ticks); busy (an injected input sequence is "
        "still running); events_pending (events queued but not yet consumed by "
        "the application); pumping and in_event_wait. pumping is false when the "
        "application has neither returned to its event loop nor been inside "
        "WaitNextEvent/ModalDialog for a second: such an application ignores "
        "input, so wait and look again rather than sending more.",
        {}));

    tools.push_back(toolDefinition(
        "click",
        "Move the pointer to (x, y) and click; count=2 is a double click. The "
        "reply's delivered/suppressed report whether the guest's event mask "
        "accepted the events, not whether the application acted on them - "
        "verify with a screenshot. Classic Mac OS consumes the click that "
        "only activates a window, so in a window that is not in front use "
        "count 1 to activate it first and click or double-click afterwards. "
        "Clicking a menu-bar title only opens and "
        "closes the menu; use button_down + move + button_up for menus.",
        { { "x", integerProperty("guest screen x in pixels") },
          { "y", integerProperty("guest screen y in pixels") },
          { "count", integerProperty("number of clicks: 1 (default) or 2 for a "
                                     "double click") } },
        { "x", "y" }));

    tools.push_back(toolDefinition(
        "move",
        "Move the pointer to (x, y) without pressing a button. Reports only "
        "that the move was posted; the guest may not redraw until it gets "
        "around to it.",
        { { "x", integerProperty("guest screen x in pixels") },
          { "y", integerProperty("guest screen y in pixels") } },
        { "x", "y" }));

    tools.push_back(toolDefinition(
        "button_down",
        "Press and hold the mouse button, optionally moving to (x, y) first "
        "(x and y must be given together). Combine with move and button_up to "
        "operate menus and to rubber-band select.",
        { { "x", integerProperty("optional guest screen x") },
          { "y", integerProperty("optional guest screen y") } }));

    tools.push_back(toolDefinition(
        "button_up",
        "Release the mouse button, optionally at (x, y). Where the release "
        "lands decides what a menu or a tracking loop does.",
        { { "x", integerProperty("optional guest screen x") },
          { "y", integerProperty("optional guest screen y") } }));

    tools.push_back(toolDefinition(
        "drag",
        "Press at `from`, move to `to` over duration_ms, then release. The "
        "reliable way to choose a menu item, move or resize a window, and drag "
        "a selection.",
        { { "from", pointProperty("[x, y] to press at") },
          { "to", pointProperty("[x, y] to release at") },
          { "duration_ms", integerProperty("default 300") } },
        { "from", "to" }));

    tools.push_back(toolDefinition(
        "type_text",
        "Type printable ASCII text into whatever has keyboard focus, as "
        "individual key presses. Click into a text field first: a document "
        "that was just opened has no insertion point. Characters that cannot "
        "be produced by the current keyboard layout are rejected, so replace "
        "non-ASCII text with press_key.",
        { { "text", prop("string") } }, { "text" }));

    tools.push_back(toolDefinition(
        "press_key",
        "Press and release one key: a single printable character, a name - "
        "Return, Enter, Tab, Space, Escape, Backspace, ForwardDelete, "
        "LeftArrow, RightArrow, UpArrow, DownArrow, Home, End, PageUp, "
        "PageDown, Help, Clear, F1..F15 - or a modifier (shift, cmd, opt, "
        "ctrl), which is then tapped. Modifiers listed in `modifiers` are "
        "pressed and released around the key, as real modifier events; do not "
        "also name the same modifier as the key. Classic Mac OS never queues "
        "key-up events, so the reply normally reads delivered=true with "
        "suppressed=[\"keyup\"].",
        { { "name", prop("string") },
          { "modifiers", stringArrayProperty("optional shift, cmd, opt, ctrl, caps") } },
        { "name" }));

    tools.push_back(toolDefinition(
        "key_down",
        "Press and hold a key by name, character, or modifier (shift, cmd, opt, "
        "ctrl); release it with key_up. Holding a modifier across several "
        "requests is how you build a chord that press_key cannot express. "
        "Check status.modifiers afterwards: a modifier that stays true after "
        "the session went to the background is a stuck key, and key_up <name> "
        "is how you let go of it.",
        { { "name", prop("string") } }, { "name" }));

    tools.push_back(toolDefinition(
        "key_up",
        "Release a key previously held with key_down; accepts the same names, "
        "including modifiers.",
        { { "name", prop("string") } }, { "name" }));

    tools.push_back(toolDefinition(
        "wait",
        "Sleep for ms in the MCP server; no round trip to the emulator, so it "
        "still works when the guest is stuck. Put it between an input and the "
        "screenshot that checks the result: a few hundred milliseconds for a "
        "click, seconds after launching an application or opening a document.",
        { { "ms", integerProperty("milliseconds") } }, { "ms" }));

    tools.push_back(toolDefinition(
        "quit",
        "Ask the guest application to quit (as Cmd-Q does); that returns to "
        "Executor's Browser, which is the only way to reach another "
        "application. force=true exits the emulator process instead, which ends "
        "the session.",
        { { "force", prop("boolean") } }));

    return tools;
}

json::object callTool(ToolState &state, const std::string &name,
                      const json::object &args)
{
    if(name == "attach")
        return attachSession(state, args);

    /* Pure timing, so it stays out of the guest round trip: waiting has to work
     * even when the emulator is wedged or the session is gone. */
    if(name == "wait")
    {
        long ms = 0;

        if(!intArgument(args, "ms", ms))
            return toolError("wait needs an integer ms");

        ms = std::clamp(ms, 0L, 600000L);

        if(ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));

        json::object reply;
        reply["ok"] = true;
        reply["waited_ms"] = ms;
        return toolReply(reply);
    }

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
        return resultOrError(request(std::move(command)));
    };

    if(name == "screenshot")
    {
        json::object command;
        command["cmd"] = "frame";

        if(const json::value *region = args.if_contains("region"); region)
        {
            if(!region->is_array() || region->as_array().size() != 4)
                return toolError("region must be [left, top, right, bottom]");

            for(const json::value &v : region->as_array())
                if(!v.is_int64())
                    return toolError("region must be [left, top, right, bottom]");

            command["rect"] = *region;
        }

        json::object reply = request(command);

        if(!reply.contains("ok") || !reply.at("ok").is_bool() || !reply.at("ok").as_bool())
            return resultOrError(reply);

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

        /* The guest cursor is a host cursor and never appears in the pixels. */
        if(const json::value *mouse = reply.if_contains("mouse"))
            meta["mouse"] = *mouse;
        if(const json::value *button = reply.if_contains("button"))
            meta["button"] = *button;

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
        return resultOrError(reply);
    }

    if(name == "move")
    {
        long x, y;

        if(!intArgument(args, "x", x) || !intArgument(args, "y", y))
            return toolError("move needs integer x and y");

        return forwarded(json::object{ { "cmd", "move" }, { "x", x }, { "y", y } });
    }

    if(name == "click")
    {
        long x, y;
        long count = 1;

        if(!intArgument(args, "x", x) || !intArgument(args, "y", y))
            return toolError("click needs integer x and y");

        intArgument(args, "count", count);

        json::object command;
        command["cmd"] = "click";
        command["x"] = x;
        command["y"] = y;
        command["count"] = count;
        return forwarded(std::move(command));
    }

    if(name == "button_down" || name == "button_up")
    {
        long x, y;

        if(args.if_contains("x") || args.if_contains("y"))
        {
            if(!intArgument(args, "x", x) || !intArgument(args, "y", y))
                return toolError("x and y must be given together, as integers");

            request(json::object{ { "cmd", "move" }, { "x", x }, { "y", y } });
        }

        json::object command;
        command["cmd"] = name == "button_down" ? "down" : "up";
        if(const json::value *b = args.if_contains("button"); b && b->is_string())
            command["button"] = *b;
        return forwarded(std::move(command));
    }

    if(name == "drag")
    {
        long x1, y1, x2, y2;

        if(!pairArgument(args, "from", x1, y1)
           || !pairArgument(args, "to", x2, y2))
            return toolError("drag needs integer from and to pairs");

        json::object command;
        command["cmd"] = "drag";
        command["from"] = json::array{ x1, y1 };
        command["to"] = json::array{ x2, y2 };
        if(const json::value *v = args.if_contains("duration_ms"); v && v->is_int64())
            command["duration_ms"] = *v;
        return forwarded(std::move(command));
    }

    if(name == "type_text")
    {
        std::string text;

        if(!stringArgument(args, "text", text))
            return toolError("type_text needs a text string");

        return forwarded(json::object{ { "cmd", "type" }, { "text", text } });
    }

    if(name == "press_key" || name == "key_down" || name == "key_up")
    {
        std::string keyName;

        if(!stringArgument(args, "name", keyName))
            return toolError("a key needs a name");

        json::object command;
        command["cmd"] = name == "press_key" ? "key"
                       : name == "key_down" ? "keydown"
                                            : "keyup";
        command["name"] = keyName;
        if(const json::value *v = args.if_contains("modifiers"); v && v->is_array())
            command["modifiers"] = *v;
        return forwarded(std::move(command));
    }

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
