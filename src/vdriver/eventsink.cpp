#include "vdriver.h"

#include <ToolboxEvent.h>

#include <base/emulator_thread.h>
#include <rsys/adb.h>
#include <rsys/toolevent.h>
#include <osevent/osevent.h>
#include <rsys/keyboard.h>
#include <time/syncint.h>
#include "responsiveness.h"

#include <cstdio>
#include <cstdlib>

using namespace Executor;

namespace
{
bool wheelDebug()
{
    static const bool enabled = std::getenv("EXECUTOR_WHEEL_DEBUG") != nullptr;
    return enabled;
}
}

std::unique_ptr<EventSink> EventSink::instance;

bool Executor::send_suspend_resume_events = true;

void EventSink::mouseButtonEvent(bool down)
{
    // The responsiveness tracker needs the host button state to tell a
    // mouse-down tracking loopapart from a stuck/modal application
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

void EventSink::mouseWheelEvent(int deltaX, int deltaY, int h, int v)
{
    if(wheelDebug())
        fprintf(stderr, "[wheel] EventSink dx=%d dy=%d h=%d v=%d\n", deltaX,
                deltaY, h, v);

    runOnEmulatorThread([=]() {
        int hh = h;
        int vv = v;

        if(hh < 0 || vv < 0)
        {
            hh = LM(MouseLocation2).h;
            vv = LM(MouseLocation2).v;
        }

        if(wheelDebug())
            fprintf(stderr, "[wheel] emulator thread h=%d v=%d\n", hh, vv);

        ROMlib_handleWheelEvent(deltaX, deltaY, hh, vv);
    });
}

void EventSink::keyboardEvent(bool down, unsigned char rawMkvkey)
{
    runOnEmulatorThread([=, this]() {
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
    runOnEmulatorThread([this] {
        bool synthesizeEvents = !send_suspend_resume_events;

        for(unsigned key = 0; key < 128; key++)
        {
            if(key == MKV_CAPS || !ROMlib_GetKey(key))
                continue;

            /* keyboardEvent() maps right-hand modifiers onto their left twins,
             * so it cannot clear those; take them out directly. */
            if(synthesizeEvents && ROMlib_right_to_left_key_map(key) == key)
                keyboardEvent(false, key);
            else
                ROMlib_SetKey(key, false);
        }

        if(LM(MBState) == 0)
        {
            if(synthesizeEvents)
                mouseButtonEvent(false);
            else
            {
                LM(MBState) = 0xFF;
                Responsiveness::instance().setButtonDown(false);
            }
        }
        if(send_suspend_resume_events)
            sendsuspendevent();
    });
}

void EventSink::resumeEvent(bool updateClipboard /* TODO: does this really make sense? */)
{
    if(send_suspend_resume_events)
        runOnEmulatorThread([=]() { sendresumeevent(updateClipboard); });
}

void EventSink::requestQuit()
{
    runOnEmulatorThread(&ROMlib_send_quit);
}

void EventSink::wake()
{
    wakeEmulatorThread();
}
