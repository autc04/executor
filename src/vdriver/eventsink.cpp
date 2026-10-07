#include "vdriver.h"

#include <ToolboxEvent.h>

#include <rsys/adb.h>
#include <rsys/toolevent.h>
#include <osevent/osevent.h>
#include <rsys/keyboard.h>
#include <time/syncint.h>
#include "responsiveness.h"

using namespace Executor;

std::unique_ptr<EventSink> EventSink::instance;

Interrupt eventInterrupt {
    [] {
        EventSink::instance->pumpEvents();
    }
};

void EventSink::mouseButtonEvent(bool down)
{
    /* This runs on the GUI thread, before the work item is marshalled to the
     * emulator thread.  The tracker needs the host button state to tell a
     * mouse-down tracking loop (button held, so click-through is irrelevant)
     * apart from an idle application. */
    Responsiveness::instance().setButtonDown(down);

    runOnEmulatorThread([=]() {
        if(down)
            LM(MBState) = 0;
        else
            LM(MBState) = 0xFF;

        PostEvent(down ? mouseDown : mouseUp, 0);
    });
}

void EventSink::mouseMoved(int h, int v)
{
    runOnEmulatorThread([=]() {
        LM(MouseLocation2) = LM(MTemp) = LM(MouseLocation) = Point{(int16_t)v, (int16_t)h};

        adb_apeiron_hack();
    });
}

void EventSink::keyboardEvent(bool down, unsigned char rawMkvkey)
{
    runOnEmulatorThread([=]() {
        auto mkvkey = ROMlib_right_to_left_key_map(rawMkvkey);

        ROMlib_SetKey(mkvkey, down);

        auto kchr = ROMlib_kchr_ptr();

        uint32_t translated = KeyTranslate(
            kchr, 
            (ROMlib_GetModifiers() & 0xFF00) | mkvkey | (down ? 0 : 0x80),
            &keytransState);

        auto evcode = down ? keyDown : keyUp;

        if(auto first_key = (translated >> 16) & 0xFF)
            PostEvent(evcode, (mkvkey << 8) | first_key);
        
        if(auto second_key = translated & 0xFF)
        {
            uint32_t msg = (mkvkey << 8) | second_key;
            PostEvent(evcode, msg);
            ROMlib_SetAutokey(down ? msg : -1);
        }
    });
}

void EventSink::suspendEvent()
{
    runOnEmulatorThread(&sendsuspendevent);
}

void EventSink::resumeEvent(bool updateClipboard /* TODO: does this really make sense? */)
{
    runOnEmulatorThread([=]() { sendresumeevent(updateClipboard); });
}

void EventSink::requestQuit()
{
    runOnEmulatorThread(&ROMlib_send_quit);
}

void EventSink::wake()
{
    eventInterrupt.trigger();
}

void EventSink::runOnEmulatorThread(std::function<void ()> f)
{
    std::lock_guard lk(mutex_);
    todo_.push_back(f);
    eventInterrupt.trigger();
}

void EventSink::pumpEvents()
{
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard lk(mutex_);
        todo = std::move(todo_);
    }
    for(const auto& f : todo)
        f();
}
