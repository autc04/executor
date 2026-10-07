#include "responsiveness.h"

#include <time/time.h>

using namespace Executor;

Responsiveness& Responsiveness::instance()
{
    static Responsiveness theInstance;
    return theInstance;
}

Responsiveness::Responsiveness()
    : lastResponseMs_(msecs_elapsed()), lastReleaseMs_(msecs_elapsed()),
      buttonDown_(false), timeoutTicks_(60), enabled_(true),
      forceCaptured_(false), captured_(false)
{
}

void Responsiveness::noteResponse()
{
    noteResponseAt(msecs_elapsed());
}

void Responsiveness::noteResponseAt(unsigned long nowMs)
{
    lastResponseMs_ = nowMs;
}

void Responsiveness::setButtonDown(bool down)
{
    setButtonDownAt(down, msecs_elapsed());
}

void Responsiveness::setButtonDownAt(bool down, unsigned long nowMs)
{
    if(down)
        buttonDown_ = true;
    else
    {
        buttonDown_ = false;
        /* The quiet clock starts at the release, so the time spent in a
         * tracking loop does not count towards entering capture. */
        lastReleaseMs_ = nowMs;
    }
}

unsigned long Responsiveness::timeoutMs() const
{
    /* 60 ticks == 1000 ms, i.e. ms == ticks * 50 / 3. */
    return (unsigned long)timeoutTicks_ * 50 / 3;
}

bool Responsiveness::poll()
{
    if(forceCaptured_)
    {
        captured_ = true;
        return true;
    }

    return pollAt(msecs_elapsed());
}

bool Responsiveness::pollAt(unsigned long nowMs)
{
    bool captured = captured_;

    unsigned long lastPump = lastResponseMs_;
    unsigned long lastRelease = lastReleaseMs_;

    if(!enabled_)
        captured = false;                        /* feature off */
    else if(nowMs < lastPump || nowMs - lastPump < timeoutMs())
        captured = false;                        /* the app pumped recently */
    else
    {
        /* Quiet enough to enter, but only if the button has also been up (no
         * tracking loop) for the whole timeout.  The clock starts at the later
         * of the last pump and the last release, so a tracking loop neither
         * enters capture nor counts as quiet time -- capture can only begin a
         * full timeout after the release.  When the button is held we keep the
         * previous (latched) value: a press must not leave capture either. */
        unsigned long base = lastPump > lastRelease ? lastPump : lastRelease;

        if(!buttonDown_ && nowMs >= base && nowMs - base >= timeoutMs())
            captured = true;
    }

    captured_ = captured;
    return captured;
}

bool Responsiveness::isCaptured() const
{
    return captured_;
}

int Responsiveness::timeoutTicks() const
{
    return timeoutTicks_;
}

void Responsiveness::setTimeoutTicks(int ticks)
{
    if(ticks < 0)
        ticks = 0;
    timeoutTicks_ = ticks;
}

bool Responsiveness::enabled() const
{
    return enabled_;
}

void Responsiveness::setEnabled(bool on)
{
    enabled_ = on;
}

bool Responsiveness::forceCaptured() const
{
    return forceCaptured_;
}

void Responsiveness::setForceCaptured(bool on)
{
    forceCaptured_ = on;
}

void Responsiveness::reset()
{
    resetAt(msecs_elapsed());
}

void Responsiveness::resetAt(unsigned long nowMs)
{
    lastResponseMs_ = nowMs;
    lastReleaseMs_ = nowMs;
    buttonDown_ = false;
    captured_ = false;
}

unsigned long Responsiveness::lastResponseMs() const
{
    return lastResponseMs_;
}
