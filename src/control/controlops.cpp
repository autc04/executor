#include <control/controlops.h>

#include <control/jobqueue.h>
#include <control/keymap.h>

#include <base/common.h>
#include <base/emulator_thread.h>
#include <base/mactype.h>
#include <EventMgr.h>
#include <OSEvent.h>
#include <SegmentLdr.h>
#include <SysErr.h>
#include <ToolboxEvent.h>
#include <WindowMgr.h>
#include <wind/wind.h>
#include <rsys/keyboard.h>
#include <vdriver/responsiveness.h>
#include <vdriver/vdriver.h>
#include <osevent/osevent.h>
#include <rsys/toolevent.h>
#include <time/time.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace Executor;
using namespace Executor::control;

namespace json = boost::json;
namespace asio = boost::asio;

/* The arguments the emulated application was launched with; defined in
 * mpw/mpw.cpp. */
extern std::vector<std::string> argsHack;

namespace
{

constexpr long kDefaultClickGapMs = 50;
constexpr long kDefaultClickIntervalMs = 30;
constexpr long kDefaultKeyGapMs = 30;
constexpr long kDefaultDragMs = 300;
constexpr long kDefaultDragSteps = 10;

json::object errorObject(const char *code, std::string text)
{
    json::object o;
    o["ok"] = false;
    o["code"] = code;
    o["error"] = std::move(text);
    return o;
}

bool getInt(const json::object &o, const char *key, long &out)
{
    const json::value *v = o.if_contains(key);

    if(!v || !v->is_int64())
        return false;

    out = static_cast<long>(v->as_int64());
    return true;
}

bool getInt(const json::object &o, const char *key, int &out)
{
    long v;

    if(!getInt(o, key, v))
        return false;

    out = static_cast<int>(v);
    return true;
}

bool getString(const json::object &o, const char *key, std::string &out)
{
    const json::value *v = o.if_contains(key);

    if(!v || !v->is_string())
        return false;

    out = std::string(v->as_string());
    return true;
}

bool getPair(const json::object &o, const char *key, int &a, int &b)
{
    const json::value *v = o.if_contains(key);

    if(!v || !v->is_array())
        return false;

    const json::array &arr = v->as_array();

    if(arr.size() != 2 || !arr[0].is_int64() || !arr[1].is_int64())
        return false;

    a = static_cast<int>(arr[0].as_int64());
    b = static_cast<int>(arr[1].as_int64());
    return true;
}

bool getQuad(const json::object &o, const char *key,
             int &a, int &b, int &c, int &d)
{
    const json::value *v = o.if_contains(key);

    if(!v || !v->is_array())
        return false;

    const json::array &arr = v->as_array();

    if(arr.size() != 4
       || !arr[0].is_int64() || !arr[1].is_int64()
       || !arr[2].is_int64() || !arr[3].is_int64())
        return false;

    a = static_cast<int>(arr[0].as_int64());
    b = static_cast<int>(arr[1].as_int64());
    c = static_cast<int>(arr[2].as_int64());
    d = static_cast<int>(arr[3].as_int64());
    return true;
}

/* Emulator-thread work, as an awaitable.  The work runs inside
 * Interrupt::handleCommon and its result is posted back to the caller's
 * executor. */
struct EmulatorResult
{
    json::object json;
    std::vector<uint32_t> pixels;
};

template<typename F>
asio::awaitable<EmulatorResult> onEmulator(F f)
{
    auto token = asio::use_awaitable;

    co_return co_await asio::async_initiate<decltype(token), void(EmulatorResult)>(
        [f = std::move(f)](auto handler) mutable
        {
            /* The handler is move-only; keep it in a shared_ptr so the task
             * stays copy-constructible for std::function. */
            auto shared = std::make_shared<std::decay_t<decltype(handler)>>(
                std::move(handler));

            Executor::runOnEmulatorThread(
                [f = std::move(f), shared]() mutable
                {
                    EmulatorResult result;

                    try
                    {
                        result = f();
                    }
                    catch(const std::exception &e)
                    {
                        result.json = errorObject("internal", e.what());
                    }
                    catch(...)
                    {
                        result.json = errorObject("internal", "unknown exception");
                    }

                    auto executor = shared->get_executor();
                    asio::post(executor,
                               [shared, result = std::move(result)]() mutable
                               {
                                   (*shared)(std::move(result));
                               });
                });
        },
        token);
}

asio::awaitable<json::object> submitJob(std::shared_ptr<Job> job)
{
    auto token = asio::use_awaitable;

    co_return co_await asio::async_initiate<decltype(token), void(json::object)>(
        [job = std::move(job)](auto handler) mutable
        {
            auto shared = std::make_shared<std::decay_t<decltype(handler)>>(
                std::move(handler));

            JobQueue::instance().submit(
                std::move(job),
                [shared](json::object result)
                {
                    auto executor = shared->get_executor();
                    asio::post(executor,
                               [shared, result = std::move(result)]() mutable
                               {
                                   (*shared)(std::move(result));
                               });
                });
        },
        token);
}

class SequenceJob : public Job
{
public:
    struct Step
    {
        std::chrono::milliseconds delayBefore{ 0 };
        std::function<void()> action;
    };

    explicit SequenceJob(std::vector<Step> steps,
                         std::function<void(json::object &)> finalize = {})
        : steps_(std::move(steps)), finalize_(std::move(finalize))
    {
    }

    std::chrono::milliseconds delay() const override
    {
        return next_ < steps_.size() ? steps_[next_].delayBefore
                                     : std::chrono::milliseconds(0);
    }

    bool run(json::object &result) override
    {
        if(next_ >= steps_.size())
        {
            finish(result);
            return true;
        }

        if(steps_[next_].action)
            steps_[next_].action();

        if(++next_ < steps_.size())
            return false;

        finish(result);
        return true;
    }

private:
    void finish(json::object &result)
    {
        result["ok"] = true;

        if(finalize_)
            finalize_(result);
    }

    std::vector<Step> steps_;
    std::function<void(json::object &)> finalize_;
    size_t next_ = 0;
};

std::string base64(const std::string &bytes)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);

    size_t i = 0;
    for(; i + 2 < bytes.size(); i += 3)
    {
        uint32_t v = (uint8_t)bytes[i] << 16 | (uint8_t)bytes[i + 1] << 8 | (uint8_t)bytes[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }

    if(i + 1 == bytes.size())
    {
        uint32_t v = (uint8_t)bytes[i] << 16;
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += "==";
    }
    else if(i + 2 == bytes.size())
    {
        uint32_t v = (uint8_t)bytes[i] << 16 | (uint8_t)bytes[i + 1] << 8;
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += '=';
    }

    return out;
}

/* The wire format: width*height bytes in R, G, B, A order. */
std::string base64RGBA(const std::vector<uint32_t> &pixels)
{
    std::string bytes;
    bytes.resize(pixels.size() * 4);

    size_t i = 0;
    for(uint32_t p : pixels)
    {
        bytes[i++] = (char)((p >> 16) & 0xFF);
        bytes[i++] = (char)((p >> 8) & 0xFF);
        bytes[i++] = (char)(p & 0xFF);
        bytes[i++] = (char)((p >> 24) & 0xFF);
    }

    return base64(bytes);
}

/* Whether PostEvent() would refuse this event class.  keyUp is excluded:
 * classic Mac OS never queues key-up events, and Executor masks it for the
 * whole session (`LM(SysEvtMask) = ~(1L << keyUp)`, init.cpp), so counting it
 * would report every keystroke as undelivered. */
bool eventSuppressed(int eventCode)
{
    return eventCode != keyUp && (LM(SysEvtMask) & (1 << eventCode)) == 0;
}

/* Collects the event classes a command found masked, for the reply's
 * "suppressed" array.  Shared by the job's steps and its finalizer. */
struct Delivery
{
    std::shared_ptr<std::vector<std::string>> suppressed
        = std::make_shared<std::vector<std::string>>();

    void check(int eventCode, const char *name) const
    {
        if(eventSuppressed(eventCode))
            suppressed->push_back(name);
    }

    void finish(json::object &o) const
    {
        json::array names;
        for(const std::string &name : *suppressed)
            names.push_back(json::string_view(name));

        o["delivered"] = suppressed->empty();
        o["suppressed"] = std::move(names);
    }
};

std::string frontWindowTitle()
{
    if(LM(WWExist) != EXIST_YES)
        return {};

    WindowPeek wp = LM(WindowList);

    for(int guard = 0; wp && guard < 64;
        wp = WINDOW_NEXT_WINDOW(wp), guard++)
    {
        if(!WINDOW_VISIBLE(wp))
            continue;

        StringHandle title = WINDOW_TITLE(wp);
        if(!title)
            return {};

        StringPtr s = toHost(*title);
        if(!s)
            return {};

        unsigned len = s[0];
        if(len > 255)
            len = 255;

        return std::string(reinterpret_cast<const char *>(s + 1), len);
    }

    return {};
}

/* The application the guest is running, as Executor tracks it in CurApName. */
std::string currentAppName()
{
    const Byte *name = LM(CurApName);

    unsigned len = name[0];
    if(len > 33)
        len = 33;

    return std::string(reinterpret_cast<const char *>(name + 1), len);
}

/* Events sitting in the guest's event queue.  A queue that keeps growing while
 * the application does not pump is the signature of a stuck guest. */
int pendingEventCount()
{
    int count = 0;

    for(EvQEl *qp = (EvQEl *)LM(EventQueue).qHead; qp && count < 256;
        qp = (EvQEl *)qp->qLink)
        count++;

    return count;
}

std::string launchArgument()
{
    return argsHack.empty() ? std::string() : argsHack[0];
}

/* Modifier keys, in the order they are pressed. */
struct ModifierKey
{
    uint16_t flag;
    uint8_t key;
};

const ModifierKey modifierKeys[] = {
    { shiftKey, MKV_LEFTSHIFT },
    { cmdKey, MKV_CLOVER },
    { optionKey, MKV_LEFTOPTION },
    { ControlKey, MKV_LEFTCNTL },
    { alphaLock, MKV_CAPS },
};

uint16_t parseModifiers(const json::object &o)
{
    const json::value *v = o.if_contains("modifiers");
    uint16_t flags = 0;

    if(!v || !v->is_array())
        return 0;

    for(const json::value &m : v->as_array())
    {
        if(!m.is_string())
            continue;

        std::string name(m.as_string());

        if(name == "shift")
            flags |= shiftKey;
        else if(name == "cmd" || name == "command" || name == "clover")
            flags |= cmdKey;
        else if(name == "opt" || name == "option")
            flags |= optionKey;
        else if(name == "ctrl" || name == "control")
            flags |= ControlKey;
        else if(name == "caps")
            flags |= alphaLock;
    }

    return flags;
}

/* Enqueues modifier key transitions.  They are ordered relative to the events
 * posted by EventSink because both use the same emulator-thread queue. */
void setModifiers(uint16_t flags, bool down)
{
    for(const ModifierKey &m : modifierKeys)
        if(flags & m.flag)
        {
            uint8_t key = m.key;
            Executor::runOnEmulatorThread([key, down] { ROMlib_SetKey(key, down); });
        }
}

struct NamedKey
{
    const char *name;
    uint8_t key;
};

const NamedKey namedKeys[] = {
    { "Return", MKV_RETURN },
    { "Enter", MKV_NUMENTER },
    { "Tab", MKV_TAB },
    { "Space", MKV_SPACE },
    { "Escape", MKV_ESCAPE },
    { "Backspace", MKV_BACKSPACE },
    { "Delete", MKV_BACKSPACE },
    { "ForwardDelete", MKV_DELFORWARD },
    { "LeftArrow", MKV_LEFTARROW },
    { "RightArrow", MKV_RIGHTARROW },
    { "UpArrow", MKV_UPARROW },
    { "DownArrow", MKV_DOWNARROW },
    { "Home", MKV_HOME },
    { "End", MKV_END },
    { "PageUp", MKV_PAGEUP },
    { "PageDown", MKV_PAGEDOWN },
    { "Help", MKV_HELP },
    { "Clear", MKV_NUMCLEAR },
    { "F1", MKV_F1 }, { "F2", MKV_F2 }, { "F3", MKV_F3 }, { "F4", MKV_F4 },
    { "F5", MKV_F5 }, { "F6", MKV_F6 }, { "F7", MKV_F7 }, { "F8", MKV_F8 },
    { "F9", MKV_F9 }, { "F10", MKV_F10 }, { "F11", MKV_F11 }, { "F12", MKV_F12 },
    { "F13", MKV_F13 }, { "F14", MKV_F14 }, { "F15", MKV_F15 },
};

/* A modifier key has no character of its own, so it can only be named.  Caps
 * lock is left out on purpose: its KeyMap bit is the lock state, so "releasing"
 * it would switch caps off. */
const NamedKey modifierNames[] = {
    { "shift", MKV_LEFTSHIFT },
    { "cmd", MKV_CLOVER }, { "command", MKV_CLOVER }, { "clover", MKV_CLOVER },
    { "opt", MKV_LEFTOPTION }, { "option", MKV_LEFTOPTION },
    { "ctrl", MKV_LEFTCNTL }, { "control", MKV_LEFTCNTL },
};

bool isModifierName(const std::string &name)
{
    for(const NamedKey &m : modifierNames)
        if(name == m.name)
            return true;

    return false;
}

struct Stroke
{
    uint8_t key = 0;
    uint16_t modifiers = 0;
};

std::optional<Stroke> resolveKey(const std::string &name, uint16_t extraModifiers)
{
    for(const NamedKey &n : namedKeys)
        if(name == n.name)
            return Stroke{ n.key, extraModifiers };

    /* Naming a modifier presses or releases it on its own, which is the only
     * way to hold one across several requests - or to let go of one that a
     * lost key-up left down. */
    for(const NamedKey &m : modifierNames)
        if(name == m.name)
            return Stroke{ m.key, extraModifiers };

    if(name.size() == 1)
    {
        char c = name[0];

        if(c == '\n' || c == '\r')
            return Stroke{ MKV_RETURN, extraModifiers };
        if(c == '\t')
            return Stroke{ MKV_TAB, extraModifiers };

        if(auto stroke = lookupChar(c))
            return Stroke{ stroke->key,
                           (uint16_t)(stroke->modifiers | extraModifiers) };
    }

    return std::nullopt;
}

json::object okResult()
{
    json::object o;
    o["ok"] = true;
    return o;
}

} /* namespace */

asio::awaitable<Reply> Executor::control::runVerb(std::string_view requestLine)
{
    Reply reply;

    boost::system::error_code ec;
    json::value value = json::parse(requestLine, ec);

    if(ec || !value.is_object())
    {
        reply.json = errorObject("bad-request", "malformed request");
        co_return reply;
    }

    const json::object &req = value.as_object();

    std::string cmd;
    if(!getString(req, "cmd", cmd))
    {
        reply.json = errorObject("bad-request", "missing \"cmd\"");
        co_return reply;
    }

    if(cmd == "hello")
    {
        auto result = co_await onEmulator([] {
            EmulatorResult r;
            r.json["ok"] = true;
            r.json["proto"] = 1;
            r.json["app"] = launchArgument();
            r.json["width"] = vdriver->width();
            r.json["height"] = vdriver->height();
            r.json["bpp"] = vdriver->bpp();
            r.json["tick"] = (int64_t)C_TickCount();
            return r;
        });
        reply.json = std::move(result.json);
    }
    else if(cmd == "screen")
    {
        auto result = co_await onEmulator([] {
            EmulatorResult r;
            r.json["ok"] = true;
            r.json["width"] = vdriver->width();
            r.json["height"] = vdriver->height();
            r.json["bpp"] = vdriver->bpp();
            r.json["rowBytes"] = vdriver->rowBytes();
            r.json["rootless"] = vdriver->isRootless();
            return r;
        });
        reply.json = std::move(result.json);
    }
    else if(cmd == "status")
    {
        auto result = co_await onEmulator([] {
            EmulatorResult r;
            json::object &o = r.json;

            unsigned long now = msecs_elapsed();
            unsigned long last = Responsiveness::instance().lastResponseMs();
            unsigned long since = now > last ? now - last : 0;
            bool waiting = Responsiveness::instance().active();

            short mods = ROMlib_GetModifiers();
            json::object modifiers;
            modifiers["shift"] = (mods & shiftKey) != 0;
            modifiers["cmd"] = (mods & cmdKey) != 0;
            modifiers["option"] = (mods & optionKey) != 0;
            modifiers["control"] = (mods & ControlKey) != 0;
            modifiers["caps"] = ROMlib_GetKey(MKV_CAPS);

            o["ok"] = true;
            o["tick"] = (int64_t)C_TickCount();
            /* Inside WaitNextEvent/ModalDialog/tracking the application is
             * behaving, even though it does not call back into the emulator. */
            o["pumping"] = waiting || since <= 1000;
            o["in_event_wait"] = waiting;
            o["since_response_ms"] = (int64_t)since;
            o["button"] = (LM(MBState) == 0);
            o["mouse"] = json::array{ (int64_t)LM(MouseLocation).h.get(),
                                      (int64_t)LM(MouseLocation).v.get() };
            o["modifiers"] = std::move(modifiers);
            o["modifiers_raw"] = (int64_t)mods;
            o["dbltime"] = (int64_t)LM(DoubleTime);
            o["app"] = currentAppName();
            o["front"] = frontWindowTitle();
            o["events_pending"] = (int64_t)pendingEventCount();
            o["busy"] = JobQueue::instance().busy();
            return r;
        });
        reply.json = std::move(result.json);
    }
    else if(cmd == "frame")
    {
        int left = 0, top = 0, right = 0, bottom = 0;
        bool hasRect = getQuad(req, "rect", left, top, right, bottom);

        if(req.if_contains("rect") && !hasRect)
        {
            reply.json = errorObject("bad-request", "rect must be [left, top, right, bottom]");
            co_return reply;
        }

        auto result = co_await onEmulator([&] {
            EmulatorResult r;

            int w = vdriver->width();
            int h = vdriver->height();

            if(!hasRect)
            {
                left = 0;
                top = 0;
                right = w;
                bottom = h;
            }

            left = std::clamp(left, 0, w);
            top = std::clamp(top, 0, h);
            right = std::clamp(right, left, w);
            bottom = std::clamp(bottom, top, h);

            Rect rect;
            rect.left = (int16_t)left;
            rect.top = (int16_t)top;
            rect.right = (int16_t)right;
            rect.bottom = (int16_t)bottom;

            vdriver->captureARGB(rect, r.pixels);

            /* The guest cursor is a host cursor and is never composited into
             * the pixels, so the pointer has to be reported alongside. */
            r.json["mouse"] = json::array{ (int64_t)LM(MouseLocation).h.get(),
                                           (int64_t)LM(MouseLocation).v.get() };
            r.json["button"] = (LM(MBState) == 0);

            r.json["ok"] = true;
            r.json["width"] = right - left;
            r.json["height"] = bottom - top;
            r.json["format"] = "rgba8888";
            return r;
        });

        reply.json = std::move(result.json);

        if(reply.json["ok"].as_bool())
            reply.json["data"] = base64RGBA(result.pixels);
    }
    else if(cmd == "move")
    {
        int x = 0, y = 0;
        if(!getInt(req, "x", x) || !getInt(req, "y", y))
        {
            reply.json = errorObject("bad-request", "move needs x and y");
            co_return reply;
        }

        auto job = std::make_shared<SequenceJob>(
            std::vector<SequenceJob::Step>{
                { std::chrono::milliseconds(0),
                  [x, y] { EventSink::instance->mouseMoved(x, y); } },
            });

        reply.json = co_await submitJob(job);
    }
    else if(cmd == "down" || cmd == "up")
    {
        std::string button = "left";
        getString(req, "button", button);

        if(button != "left")
        {
            reply.json = errorObject("unsupported", "only the left button is supported");
            co_return reply;
        }

        bool down = cmd == "down";
        Delivery delivery;

        auto job = std::make_shared<SequenceJob>(
            std::vector<SequenceJob::Step>{
                { std::chrono::milliseconds(0),
                  [down, delivery]
                  {
                      delivery.check(down ? mouseDown : mouseUp,
                                     down ? "mousedown" : "mouseup");
                      EventSink::instance->mouseButtonEvent(down);
                  } },
            },
            [delivery](json::object &o) { delivery.finish(o); });

        reply.json = co_await submitJob(job);
    }
    else if(cmd == "click")
    {
        int x = 0, y = 0;
        long count = 1;
        long gap = kDefaultClickGapMs;
        long interval = kDefaultClickIntervalMs;

        getInt(req, "count", count);
        getInt(req, "gap_ms", gap);
        getInt(req, "interval_ms", interval);

        if(!getInt(req, "x", x) || !getInt(req, "y", y))
        {
            reply.json = errorObject("bad-request", "click needs x and y");
            co_return reply;
        }

        count = std::clamp(count, 1L, 3L);
        gap = std::clamp(gap, 0L, 10000L);
        interval = std::clamp(interval, 0L, 10000L);

        Delivery delivery;
        std::vector<SequenceJob::Step> steps;

        steps.push_back({ std::chrono::milliseconds(0),
                          [x, y] { EventSink::instance->mouseMoved(x, y); } });

        for(long i = 0; i < count; i++)
        {
            steps.push_back(
                { std::chrono::milliseconds(i == 0 ? 0 : interval),
                  [delivery]
                  {
                      delivery.check(mouseDown, "mousedown");
                      EventSink::instance->mouseButtonEvent(true);
                  } });
            steps.push_back(
                { std::chrono::milliseconds(gap),
                  [delivery]
                  {
                      delivery.check(mouseUp, "mouseup");
                      EventSink::instance->mouseButtonEvent(false);
                  } });
        }

        auto job = std::make_shared<SequenceJob>(
            std::move(steps),
            [delivery](json::object &o) { delivery.finish(o); });

        reply.json = co_await submitJob(job);
    }
    else if(cmd == "drag")
    {
        int x1, y1, x2, y2;
        long duration = kDefaultDragMs;
        long steps = kDefaultDragSteps;

        if(!getPair(req, "from", x1, y1) || !getPair(req, "to", x2, y2))
        {
            reply.json = errorObject("bad-request", "drag needs from and to");
            co_return reply;
        }

        getInt(req, "duration_ms", duration);
        getInt(req, "steps", steps);
        duration = std::clamp(duration, 0L, 60000L);
        steps = std::clamp(steps, 1L, 100L);

        Delivery delivery;
        std::vector<SequenceJob::Step> stepsList;

        stepsList.push_back({ std::chrono::milliseconds(0),
                              [x1, y1] { EventSink::instance->mouseMoved(x1, y1); } });
        stepsList.push_back({ std::chrono::milliseconds(0),
                              [delivery]
                              {
                                  delivery.check(mouseDown, "mousedown");
                                  EventSink::instance->mouseButtonEvent(true);
                              } });

        auto perStep = std::chrono::milliseconds(duration / steps);

        for(long i = 1; i <= steps; i++)
        {
            int x = x1 + (int)((long)(x2 - x1) * i / steps);
            int y = y1 + (int)((long)(y2 - y1) * i / steps);

            stepsList.push_back({ perStep,
                                  [x, y] { EventSink::instance->mouseMoved(x, y); } });
        }

        stepsList.push_back({ std::chrono::milliseconds(0),
                              [delivery]
                              {
                                  delivery.check(mouseUp, "mouseup");
                                  EventSink::instance->mouseButtonEvent(false);
                              } });

        auto job = std::make_shared<SequenceJob>(
            std::move(stepsList),
            [delivery](json::object &o) { delivery.finish(o); });

        reply.json = co_await submitJob(job);
    }
    else if(cmd == "key" || cmd == "keydown" || cmd == "keyup")
    {
        std::string name;
        if(!getString(req, "name", name))
        {
            reply.json = errorObject("bad-request", "key needs name");
            co_return reply;
        }

        /* The key would be pressed twice, and which one the "modifiers" entry
         * was meant to release would be ambiguous. */
        if(isModifierName(name) && req.if_contains("modifiers"))
        {
            reply.json = errorObject(
                "bad-request",
                "a modifier key cannot also be listed in \"modifiers\"");
            co_return reply;
        }

        auto stroke = resolveKey(name, parseModifiers(req));
        if(!stroke)
        {
            reply.json = errorObject("unsupported", "unknown key: " + name);
            co_return reply;
        }

        Delivery delivery;
        std::vector<SequenceJob::Step> steps;

        auto press = [stroke, delivery] {
            setModifiers(stroke->modifiers, true);
            delivery.check(keyDown, "keydown");
            EventSink::instance->keyboardEvent(true, stroke->key);
        };
        auto release = [stroke, delivery] {
            delivery.check(keyUp, "keyup");
            EventSink::instance->keyboardEvent(false, stroke->key);
            setModifiers(stroke->modifiers, false);
        };

        if(cmd == "keydown")
        {
            steps.push_back({ std::chrono::milliseconds(0), press });
        }
        else if(cmd == "keyup")
        {
            steps.push_back({ std::chrono::milliseconds(0), release });
        }
        else
        {
            long gap = kDefaultKeyGapMs;
            getInt(req, "gap_ms", gap);

            steps.push_back({ std::chrono::milliseconds(0), press });
            steps.push_back({ std::chrono::milliseconds(std::clamp(gap, 0L, 10000L)),
                              release });
        }

        auto job = std::make_shared<SequenceJob>(
            std::move(steps),
            [delivery](json::object &o) { delivery.finish(o); });

        reply.json = co_await submitJob(job);
    }
    else if(cmd == "type")
    {
        std::string text;
        long gap = kDefaultKeyGapMs;

        if(!getString(req, "text", text))
        {
            reply.json = errorObject("bad-request", "type needs text");
            co_return reply;
        }

        getInt(req, "gap_ms", gap);
        gap = std::clamp(gap, 0L, 10000L);

        Delivery delivery;
        std::vector<SequenceJob::Step> steps;

        for(size_t i = 0; i < text.size(); i++)
        {
            char c = text[i];
            std::optional<KeyStroke> stroke =
                (c == '\n' || c == '\r') ? std::optional<KeyStroke>(KeyStroke{ MKV_RETURN, 0 })
              : (c == '\t')             ? std::optional<KeyStroke>(KeyStroke{ MKV_TAB, 0 })
                                        : lookupChar(c);

            if(!stroke)
            {
                reply.json = errorObject("unsupported",
                                         std::string("cannot type character 0x")
                                             + "0123456789abcdef"[(c >> 4) & 15]
                                             + "0123456789abcdef"[c & 15]);
                co_return reply;
            }

            uint8_t key = stroke->key;
            uint16_t mods = stroke->modifiers;

            steps.push_back(
                { std::chrono::milliseconds(i == 0 ? 0 : gap),
                  [key, mods, delivery]
                  {
                      setModifiers(mods, true);
                      delivery.check(keyDown, "keydown");
                      EventSink::instance->keyboardEvent(true, key);
                  } });
            steps.push_back(
                { std::chrono::milliseconds(gap),
                  [key, mods, delivery]
                  {
                      delivery.check(keyUp, "keyup");
                      EventSink::instance->keyboardEvent(false, key);
                      setModifiers(mods, false);
                  } });
        }

        if(steps.empty())
        {
            reply.json = okResult();
            reply.json["chars"] = 0;
            co_return reply;
        }

        auto job = std::make_shared<SequenceJob>(
            std::move(steps),
            [delivery](json::object &o) { delivery.finish(o); });

        reply.json = co_await submitJob(job);
        reply.json["chars"] = (int64_t)text.size();
    }
    else if(cmd == "wait")
    {
        long ms = 0;
        getInt(req, "ms", ms);
        ms = std::clamp(ms, 0L, 600000L);

        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(ms));
        co_await timer.async_wait(asio::use_awaitable);

        reply.json = okResult();
    }
    else if(cmd == "quit")
    {
        bool force = false;

        if(const json::value *v = req.if_contains("force"); v && v->is_bool())
            force = v->as_bool();

        if(force)
        {
            /* The server exits after writing this reply. */
            reply.json = okResult();
            reply.terminate = true;
        }
        else
        {
            auto result = co_await onEmulator([] {
                EmulatorResult r;
                ROMlib_send_quit();
                r.json["ok"] = true;
                return r;
            });
            reply.json = std::move(result.json);
        }
    }
    else
    {
        reply.json = errorObject("unsupported", "unknown command: " + cmd);
    }

    if(const json::value *id = req.if_contains("id"))
        reply.json["id"] = *id;

    co_return reply;
}
