#if !defined(_RESPONSIVENESS_H_)
#define _RESPONSIVENESS_H_

#include <atomic>

namespace Executor
{

/* Tracks whether the running Mac application has recently pumped its event
 * loop, and whether the emulator should therefore hold onto the mouse.
 * See docs/ai/2026-10-07-unresponsive-app-detection-plan.md.
 *
 * "captured" is a latched state.  It is *entered* when the application has gone
 * quiet (no SystemTask/WaitNextEvent/ModalDialog) for the timeout while the
 * mouse button is up, and *left* when the application pumps again (or detection
 * is disabled).
 *
 * A mouse-down tracking loop is neutral: it must not enter capture, must not
 * leave it, and -- importantly -- must not count as quiet time.  Otherwise a
 * long tracking loop would make the application look as if it had been stuck
 * for the whole loop, and capture would begin the instant the button came up.
 * The quiet clock therefore starts at the later of the last pump and the last
 * button *release* (see pollAt()).
 *
 * All methods are safe to call from both the emulator thread and the GUI
 * thread; the state is kept in atomics and host time is compared with the same
 * monotonic clock that drives TickCount().  The `...At(nowMs)` methods are
 * deterministic seams for tests; the plain ones use msecs_elapsed().
 */
class Responsiveness
{
public:
    static Responsiveness& instance();

    /* Emulator thread: the guest just called SystemTask / WaitNextEvent /
     * ModalDialog, i.e. it pumped its event loop. */
    void noteResponse();
    void noteResponseAt(unsigned long nowMs);

    /* Emulator thread: the application is waiting for events (WaitNextEvent may
     * block until the next event, e.g. a click).  The whole time spent inside
     * such a call is the application behaving correctly, so it must not be
     * mistaken for a stuck application -- neither may it count towards entering
     * capture.  ActiveScope is the RAII helper; enter/exit maintain a nesting
     * counter and refresh the quiet clock. */
    void enterActive();
    void exitActive();

    class ActiveScope
    {
    public:
        ActiveScope() { instance().enterActive(); }
        ~ActiveScope() { instance().exitActive(); }
        ActiveScope(const ActiveScope&) = delete;
        ActiveScope& operator=(const ActiveScope&) = delete;
    };

    /* GUI thread: host mouse button press/release.  A release restarts the
     * quiet clock. */
    void setButtonDown(bool down);
    void setButtonDownAt(bool down, unsigned long nowMs);

    /* GUI thread: re-evaluate and latch the captured state, returning it.
     * Call this periodically (and from the front-end frame loop). */
    bool poll();
    bool pollAt(unsigned long nowMs);

    /* The state latched by the last poll(); does not re-evaluate. */
    bool isCaptured() const;

    /* Timeout in ticks (60 ticks == 1 s). */
    int timeoutTicks() const;
    void setTimeoutTicks(int ticks);

    bool enabled() const;
    void setEnabled(bool on);

    /* Debug/testing: force the captured state regardless of timeout, button or
     * enabled flag.  Used to exercise the capture feedback without a game. */
    bool forceCaptured() const;
    void setForceCaptured(bool on);

    /* At boot and at each app launch. */
    void reset();
    void resetAt(unsigned long nowMs);

    /* The host time recorded by the last noteResponse()/reset(). */
    unsigned long lastResponseMs() const;

private:
    Responsiveness();

    unsigned long timeoutMs() const;

    std::atomic<unsigned long> lastResponseMs_; /* last pump */
    std::atomic<unsigned long> lastReleaseMs_;  /* last mouse-button release */
    std::atomic<int> activeDepth_;              /* inside an event-wait call */
    std::atomic<bool> buttonDown_;
    std::atomic<int> timeoutTicks_;
    std::atomic<bool> enabled_;
    std::atomic<bool> forceCaptured_;
    std::atomic<bool> captured_;                /* latched */
};

}

#endif /* !_RESPONSIVENESS_H_ */
