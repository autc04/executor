# Plan: Implicit mouse capture (and "not pumping events" detection)

> **AI-generated.** This document was produced with the assistance of an AI
> language model and may contain inaccuracies. It is a **plan**, not a record of
> finished work.

- **Date:** 2026-10-07
- **Status:** planned / not implemented
- **Area:** `src/vdriver/`, `src/desk.cpp`, `src/toolevent.cpp`,
  `src/dial/dialHandle.cpp`, `src/config/front-ends/{qt,wayland}/`,
  `src/prefs/`, `src/prefpanel.cpp`, `res/System.ad`

## Summary

We want to detect when the running Mac application is no longer pumping its
event loop — it has not called `SystemTask` or `WaitNextEvent` for more than a
configurable number of ticks (default **60 ticks = 1 s**) — and reproduce the
classic-Mac "implicit mouse capture" that such applications rely on, plus give
the user feedback about it, in rootless mode:

1. **Detect** the condition (iteration 1).
2. **Capture mouse clicks** that would otherwise fall through the rootless
   "holes" onto the host desktop, so they keep going to the emulated app instead
   of interrupting it (iteration 2).
3. **Give visual feedback** by dimming the desktop to **50 % transparent black**
   while capture is active (iteration 3).
4. Make that dim a smooth **fade** (iteration 4).
5. Add an on/off switch to the **Command-Shift-5 "Emulation Settings"** dialog
   (iteration 5).

## Motivation: the classic-game implicit capture

Classic Mac OS had no pre-emptive multitasking, so a game could simply stop
calling `SystemTask`/`WaitNextEvent` and instead poll `GetOSEvent` directly.
Because it never yielded to the Window Manager, clicking the desktop or another
application did **not** switch away or steal focus: the game kept the mouse and
kept running. That was *implicit mouse capture*, and games came to depend on it.

Under Executor in rootless mode there is no Finder and no Window Manager to
switch to, and the rootless "holes" are click-through — so a click outside the
emulated windows leaks to whatever host window is behind and **interrupts
gameplay**. To reproduce the classic behaviour we must capture those clicks in
the holes while the app is in this state.

The same detected state also lets us show feedback that the emulator is holding
the mouse for the app.

> Note: this is deliberately **not** a "the app has crashed" detector. An app
> that polls `GetOSEvent` instead of `SystemTask`/`WaitNextEvent` is behaving
> exactly as intended here.

### Terminology: what "unresponsive" means in this plan

For the rest of this document, **captured state** means:

> the application has not called `SystemTask` or `WaitNextEvent` within the
> timeout **and** the mouse button is not currently held.

The mouse-button qualifier is important (see the next section) and is the main
difference from the earlier draft of this plan. The word "unresponsive" is
avoided below except in code identifiers, to prevent confusion with "crashed".

## Response semantics

### What counts as proof the app is alive

| Source | File | Counts? |
|--------|------|---------|
| `_SystemTask` → `C_SystemTask` | `src/desk.cpp:159` | **yes** |
| `_WaitNextEvent` → `C_WaitNextEvent` | `src/toolevent.cpp:464` | **yes** |
| `_ModalDialog` → `C_ModalDialog` | `src/dial/dialHandle.cpp:115`, `:188` | **yes** — runs its own `GetNextEvent` loop; on a real Mac it calls `SystemTask` internally |
| `_GetNextEvent` → `C_GetNextEvent` | `src/toolevent.cpp:446` | **no** — a Mac `GetNextEvent` does not yield either |
| `_GetOSEvent` (games poll this) | `src/osevent/osevent.cpp` | **no** — this is the whole point |

### Mouse-down tracking loops

A mouse-down tracking loop (Executor's `WaitMouseUp`, `TrackControl`,
`DragWindow`, `MenuSelect`, `TrackGoAway`, `GrowWindow`, `TrackBox`, …, or a
loop an application writes itself around `Button`/`StillDown`/`GetOSEvent`)
must **not** be treated as captured and must **not** produce visual feedback.
Two reasons:

- Click-through is irrelevant while the button is held: no new click can be
  initiated, so there is nothing to capture.
- The in-progress press/release is kept by the emulator window through the
  window system's implicit grab, so turning capture off mid-press cannot leak
  the release.

**Recommended mechanism — a global "button held" rule.** Rather than hooking
every tracking trap, gate the captured state on the mouse button being up:

```
responsive  ==  (SystemTask/WaitNextEvent/ModalDialog within timeout)  OR  (button held)
captured    ==  !responsive
```

Because *every* tracking loop (Executor's and application-supplied) runs with
the button held, this single rule covers them all — including the
application-supplied loops that we cannot hook. It also automatically suppresses
feedback during drags, so a long window drag does not strobe the desktop dim.

Optional defensive hooks on Executor's own tracking traps (they call
`noteResponse()` when entered) are harmless but should be largely redundant
under the global rule; add them only if testing shows a gap.

On the button state: the guest-visible flag is `LM(MBState)` (0 = down, set on
the emulator thread in `EventSink::mouseButtonEvent`, `eventsink.cpp:24`), which
is **not** safe to read from the GUI thread. Track the host-side button state in
`EventSink` instead — it is the single funnel for host input (including
`EventRecorder`/`EventPlayback`, which both delegate to `EventSink`) — and have
it push the state into the tracker (see below).

```mermaid
sequenceDiagram
    participant Game as Game (guest)
    participant Trk as Responsiveness
    participant FE as Front-end (GUI)
    Note over Game: gameplay loop: GetOSEvent, no SystemTask
    Game->>Trk: (silence) 1 s elapses
    Trk->>FE: captured = true
    FE->>FE: capture clicks + fade desktop to 50% black
    Game->>Game: user clicks in a hole (captured, button now down)
    Trk->>FE: button held => not captured
    FE->>FE: fade dim back out (capture stays safe via implicit grab)
    Game->>Game: button released; still polling GetOSEvent
    Trk->>FE: captured = true again
```

## Background: the pieces this builds on

### Threading model

The guest runs on a dedicated `executorThread` (`src/main.cpp:432`); the GUI
event loop (Qt `qapp->exec()`, Wayland `display_.dispatch()`) runs on the main
thread. Host input is funnelled through `EventSink`
(`src/vdriver/eventsink.cpp`), which marshals work onto the emulator thread.

**Consequence:** the captured state can be evaluated on the *GUI thread* using
the host clock, independently of whether the guest is making progress — essential
for a game that never yields.

### Time and ticks

- `Executor::msecs_elapsed()` (`src/time/time.cpp:37`) is a monotonic host
  clock (scaled by `timewarp`).
- Ticks are defined by `C_TickCount()` (`src/toolevent.cpp:573`):
  `ticks = msecs_elapsed() * 3 / 50`, i.e. **60 ticks = 1000 ms**
  (`ms = ticks * 50 / 3`).

### Rootless compositing and click-through

In rootless mode the host window is clipped to the union of the emulated windows
(plus the menu bar and any open menu). Everything outside is a "hole" through
which the host desktop is visible **and through which clicks pass to the host**.

- `ROMlib_rootless_update()` (`src/wind/windRootless.cpp:14`) builds that region
  and hands it to `VideoDriver::setRootlessRegion()` (`vdriver.cpp:156`);
  `commitRootlessRegion()` (`vdriver.cpp:181`) turns it into a dirty-rect delta.
- **Qt** applies it as a binary window mask, `window->setMask(qtRgn)`
  (`src/config/front-ends/qt/qt.cpp:256`). A `QWindow` mask is simultaneously a
  **paint clip** *and* an **input shape**: holes are click-through.
- **Wayland** applies it as a separate **input region**,
  `surface_.set_input_region(waylandRgn)`
  (`src/config/front-ends/wayland/wayland.cpp:448`). The surface is drawn with
  alpha, so the desktop stays transparent without a paint clip.

Only Qt and Wayland implement rootless; X/SDL/win32/headless fill the whole
window with the framebuffer, so iterations 2–4 are no-ops there.

## Architecture overview (shared by all iterations)

```mermaid
flowchart TD
    subgraph Emulator thread
        ST[C_SystemTask] --> NOTE[Responsiveness::noteResponse]
        WNE[C_WaitNextEvent] --> NOTE
        MD[C_ModalDialog] --> NOTE
    end
    subgraph GUI thread
        MB[EventSink::mouseButtonEvent] --> BTN[setButtonDown]
        TIMER[host periodic timer] --> POLL[VideoDriver::updateResponsivenessFeedback]
        POLL --> ST_[(last response, host ms)]
        POLL --> POLL2
        POLL2{responsive =<br/>recent pump OR button held?} -->|captured| CAP[setClickCapture true]
        POLL2 -->|animate| DIM[setDesktopDim alpha]
        CAP --> FE[front-end mask / input region]
        DIM --> FE
    end
    NOTE --> ST_
    BTN --> POLL2
```

### 1. `Responsiveness` tracker (new: `src/vdriver/responsiveness.{h,cpp}`)

A tiny thread-safe singleton. No guest memory, no `GUEST<>` types.

```cpp
namespace Executor {
class Responsiveness {
public:
    static Responsiveness& instance();

    // Emulator thread: guest just called SystemTask/WaitNextEvent/ModalDialog.
    void noteResponse();

    // GUI thread: host mouse button press/release (from EventSink).
    void setButtonDown(bool down);

    // GUI thread: is the app in the captured state?
    //   captured == !(recentlyPumped || buttonDown) && enabled
    bool isCaptured() const;

    int  timeoutTicks() const;   void setTimeoutTicks(int ticks); // default 60
    bool enabled() const;        void setEnabled(bool);          // prefs toggle

    void reset();                // boot and app launch

    // Test seam: evaluate against explicit host time / button state.
    bool isCapturedAt(unsigned long nowMs, bool buttonDown) const;
private:
    std::atomic<unsigned long> lastResponseMs_{0};
    std::atomic<bool> buttonDown_{false};
    std::atomic<int> timeoutTicks_{60};
    std::atomic<bool> enabled_{true};
};
}
```

- `noteResponse()` stores `msecs_elapsed()`.
- `isCaptured()` is `enabled_ && !buttonDown_ && msecs_elapsed() - lastResponseMs_
  >= timeoutTicks_ * 50 / 3`.
- `reset()` stores `msecs_elapsed()` so a freshly launched app is not instantly
  flagged before it has had a chance to run.

### 2. Feedback state machine + front-end hooks (in `VideoDriver`)

Add to the base `VideoDriver` (`src/vdriver/vdriver.h:103`):

```cpp
protected:
    // Evaluated on the GUI thread from the front-end's frame/timer.
    // Advances the fade, toggles click capture, and returns true while a
    // repaint is still needed.
    bool updateResponsivenessFeedback();

    // Rootless front-ends override these; defaults are no-ops (headless).
    virtual void setClickCapture(bool capture) {}   // capture clicks in the holes
    virtual void setDesktopDim(float alpha) {}      // 0 = transparent, 1 = opaque black
```

State: `dimCurrent_`, `dimTarget_` (0 or `kCaptureDim = 0.5f`), the fade start
time, and the last capture value. `updateResponsivenessFeedback()`:

1. `bool captured = Responsiveness::instance().isCaptured();`
2. set `dimTarget_ = captured ? kCaptureDim : 0.0f`.
3. ease `dimCurrent_` toward `dimTarget_` over `kFadeDurationMs` (iteration 4).
4. call `setClickCapture(captured)` on change and `setDesktopDim(dimCurrent_)`
   when it changes.
5. return `true` while the fade is still running.

**Who calls `updateResponsivenessFeedback()`?** The GUI thread, on a periodic
timer, because the guest may never yield. Recommended per front-end:

- **Qt:** a `QTimer` (~30–60 Hz) created in `QtVideoDriver::setMode`
  (`qt.cpp:191`); its slot calls `updateResponsivenessFeedback()` and, when it
  returns true, `window->requestUpdate()` (repaint).
- **Wayland:** reuse/extend the front-end's update scheduling (`updateTimeout_`,
  `wayland.cpp:367`) so a timeout wakes the loop and calls the same method.
- **Headless:** never called (no-ops).

This keeps polling on the thread that owns the graphics and avoids cross-thread
`requestUpdate`.

---

## Iteration 1 — Detection

**Goal:** a correct, testable answer to "should we capture the mouse right now?",
usable from the GUI thread and the prefs dialog.

### Design

- Add `src/vdriver/responsiveness.{h,cpp}` as above; register it in
  `vdriver_sources` in `src/CMakeLists.txt`.
- `noteResponse()` at the top of `C_SystemTask()` (`desk.cpp:159`),
  `C_WaitNextEvent()` (`toolevent.cpp:464`), and `C_ModalDialog()`
  (`dialHandle.cpp:115`, `:188`). Do **not** hook `GetNextEvent`/`GetOSEvent`.
- `setButtonDown(down)` from `EventSink::mouseButtonEvent` (`eventsink.cpp:21`),
  before marshalling to the emulator thread (this runs on the GUI thread).
- `reset()` at boot next to `syncint_init()`/`ROMlib_eventinit()`
  (`main.cpp:456`/`:460`) and at each app launch in `NewLaunch()`
  (`src/launch.cpp:567`) beside `hle_reinit()`/`AE_reinit()`.
- Expose the timeout and a disable switch as **command-line options** now, in
  `main.cpp`'s "Miscellaneous" group (`main.cpp:304`):
  `--unresponsive-timeout <ticks>` (default 60) and an inverted switch. This
  lets users disable the feature before iteration 5 lands and helps testing.

### Gotchas

- **`GetOSEvent` must stay "silent".** It is exactly the call the target games
  use; hooking it would defeat the feature.
- **Timewarp.** Use `msecs_elapsed()` so ticks stay consistent with
  `TickCount()`; do not mix in `steady_clock` outside the test seam.
- **Disabled state.** When disabled, `isCaptured()` is always false and any
  applied feedback must be released (dim cleared, capture off) — the state
  machine handles this by animating back to 0.

### Tests

- New native CTest source `tests/responsiveness.cpp`, registered in
  `tests/CMakeLists.txt` (`NATIVE_TEST_SOURCES`), using `isCapturedAt` so no real
  sleeps are needed. Cover: fresh instance is not captured; `noteResponse()`
  restarts the window; timeout-in-ticks converts to the right millisecond
  budget; **`setButtonDown(true)` forces not-captured and `false` re-arms**;
  `setEnabled(false)` forces not-captured; `reset()` restores not-captured.
- Optional: a `--debug`-style flag or env var (e.g.
  `EXECUTOR_FORCE_UNRESPONSIVE`) to force the captured state so iterations 2–4
  can be exercised without a game. The forced state should bypass the
  button-held rule so the visuals can be checked.

### Acceptance

- A program that polls `GetOSEvent` (never `SystemTask`/`WaitNextEvent`) becomes
  captured after ~1 s; calling `WaitNextEvent` clears it within ~1 s.
- Holding the mouse button suppresses capture immediately; releasing re-arms the
  ~1 s timer.
- No effect in non-rootless modes.

---

## Iteration 2 — Capture clicks while captured (rootless only)

**Goal:** while captured, clicks in the rootless "holes" are delivered to
Executor instead of falling through to the host, so they do not interrupt the
app.

### Design

- `VideoDriver::setClickCapture(bool)` (base no-op; front-end hooks below).
- **Qt** (`src/config/front-ends/qt/qt.cpp`): the rootless mask is the
  click-through mechanism, so capture means *widening the effective mask to the
  full screen* while captured, and restoring the rootless mask otherwise. Keep
  the two sources separate — store the rootless region and a
  `clickCaptureActive_` flag, and compute the effective mask in `render()`
  (`qt.cpp:240`) as `clickCaptureActive_ ? fullScreen : rootlessRegion`.
  - This couples iteration 2 with iteration 3 on Qt: a full-screen mask makes
    the whole window opaque, so the now-visible desktop pixels must be painted
    (the dim), not left as a hole. See iteration 3 (approach A).
- **Wayland** (`wayland.cpp`): `setClickCapture(true)` sets the input region to
  the full screen; `false` restores the region computed by
  `commitRootlessRegion()`. Wayland keeps paint and input separate, so nothing
  else changes.
- **How captured clicks are handled:** they enter through the existing
  `IEventListener` path (`EventSink::mouseButtonEvent` / `mouseMoved`,
  `eventsink.cpp:21`), which posts ordinary `mouseDown`/`mouseUp`/mouse-moved
  events — queued and delivered when the app next services events. The
  in-progress press/release is kept by the window system's implicit grab, so
  turning capture off on button-down cannot leak the release.

> An alternative that avoids touching the click area at all — a real host
> pointer grab — is evaluated in *Alternative design: real mouse capture* below.
> It is deliberately **not** the default.

### Tests / validation

- No host-independent unit test is practical; validate by hand on Qt and Wayland
  with the forced-capture debug hook and a `GetOSEvent`-style test app: clicks in
  holes no longer reach the host window behind, and they reach the guest.
- Wayland: confirm a full-screen input region does not break menu tracking or
  window drags.

### Open questions

- Unbounded event-queue growth if a captured app never resumes. Consider capping
  or coalescing queued synthetic clicks.
- Should capture be latched for an in-progress press even if the button-up rule
  would turn it off? (The implicit-grab argument says no, but verify on Qt and
  Wayland.)

---

## Iteration 3 — Visual feedback: dim the desktop to 50 % transparent black

**Goal:** while captured, the desktop area is dimmed to black at 50 % opacity,
in rootless mode, without dimming the emulated windows.

### Design

- `VideoDriver::setDesktopDim(float alpha)` (base no-op), target
  `kCaptureDim = 0.5f`.
- **Wayland** is straightforward: the surface already carries per-pixel alpha.
  Paint the desktop pixels (the region *outside* `rootlessRegion_`) as
  `rgba(0, 0, 0, alpha)`, leaving emulated windows opaque. The input region is
  independent, so capture (iteration 2) is unaffected.
- **Qt — approach A (selected).** Use the binary rootless mask normally (desktop
  punched out, click-through). While captured, **drop the mask and render the
  window with per-pixel alpha**: switch the backing image to
  `QImage::Format_ARGB32_Premultiplied`, paint desktop pixels as black at
  `alpha`, and keep emulated window pixels opaque. A maskless window hit-tests as
  a rectangle, which *is* the click capture from iteration 2 — so iteration 2 and
  iteration 3 are the same Qt state change.
  - Must be verified per platform that a maskless, per-pixel-alpha `QWindow`
    still receives the click (needed for capture). On macOS the frameless window
    may need `setOpaque:NO`, and Windows/X11 may need an ARGB/composited
    surface; document each.
  - The fallback if a platform refuses translucency is a dedicated
    input-transparent overlay window below the main window (approach B) or, last
    resort, a full-screen opaque near-black desktop; note the limitation rather
    than silently degrading.

### Tests / validation

- Manual, on Qt and Wayland, with the forced-capture hook: the desktop visibly
  dims while captured, emulated windows keep their normal appearance, and the
  dim disappears when capture ends (including when the button goes down).
- Confirm normal (responsive) rendering is unchanged and that the rootless
  region delta computation is unaffected.

### Open questions

- "The desktop" here means the host desktop showing through the holes (this
  plan's reading). Executor's own emulated desktop pattern would be a
  framebuffer-only change and invisible in rootless mode.

---

## Iteration 4 — Fade effect

**Goal:** animate the transition between transparent and 50 % black instead of
snapping.

### Design

- The state machine owns `dimCurrent_`/`dimTarget_`; add `kFadeDurationMs`
  (proposed 300 ms) and an easing function (proposed ease-out,
  `1 - (1 - t)^3`), evaluated against `steady_clock` on the GUI thread.
- `updateResponsivenessFeedback()` returns `true` while
  `|dimCurrent_ - dimTarget_| > epsilon`, so the front-end keeps scheduling
  frames (Qt: the `QTimer` slot / repeated `requestUpdate()`; Wayland: keep
  calling `requestFrame()`), stopping once settled.
- Fade **out** with the same timing when capture ends.
- Capture itself toggles at detection time, *not* tied to the fade, so mouse
  capture is protected immediately even while the fade runs.
- Because the button-held rule suppresses capture during tracking loops, the
  dim does not strobe during drags. Still consider a small **hysteresis/debounce**
  so an app that flaps around the timeout does not flicker (e.g. require the
  captured state to hold for a moment before fading in).

### Tests / validation

- Manual: smooth fade in/out on Qt and Wayland; no busy repaint once settled; a
  click in a hole (button down then up) does not visibly thrash the dim.

---

## Iteration 5 — On/off switch in the Command-Shift-5 dialog

**Goal:** let the user disable the feature from the "Emulation Settings"
preferences dialog (opened via Command-Shift-5 → `dopreferences()`,
`src/prefpanel.cpp:421`; dispatched at `src/toolevent.cpp:349`).

### Design (mirrors the existing `PretendAlias`/`NoWarn32` booleans)

1. **State variable:** add `bool ROMlib_capture_when_unresponsive = true;` to
   `src/prefs/options.cpp` (definition) and `src/prefs/prefs.h` (declaration).
   Wire it to `Responsiveness::instance().setEnabled(...)`.
2. **Config parsing** (persist in `.ecf` files):
   - `src/prefs/parse.h`: add `ROMLIB_CAPTURE_UNRESPONSIVE_BIT = (1 << 22);`
     (the highest existing bit is `ROMLIB_TEXT_DISABLE_BIT`, `1 << 21`).
   - `src/prefs/parse.ypp`: add
     `{ "CaptureUnresponsive", OPTIONSCONSTANT, ROMLIB_CAPTURE_UNRESPONSIVE_BIT }`
     to the keyword table (`~line 314`).
   - `src/prefs/prefs.cpp`: honour the bit in `ParseConfigFile()` and emit
     `, CaptureUnresponsive` in `SaveConfigFile()`.
3. **Dialog wiring** (`src/prefpanel.cpp`):
   - `src/include/rsys/prefpanel.h`: add `PREF_CAPTURE_UNRESPONSIVE = 38` (next
     free id; `PREF_APPNAME` is 37).
   - `setupprefvalues()`: `modstate(dp, PREF_CAPTURE_UNRESPONSIVE,
     ROMlib_capture_when_unresponsive ? SETSTATE : CLEARSTATE);`
   - `readprefvalues()`: read the checkbox and
     `Responsiveness::instance().setEnabled(...)`.
   - `enable_disable_pref_items()`: add it to `to_enable`.
   - `dopreferences()`: add the item to the checkbox `FLIPSTATE` case list
     (`prefpanel.cpp:469`).
4. **Dialog resource (the hard part).** The dialog is `DLOG`/`DITL` id `-4063`,
   stored in the built-in System file's resource fork: `res/System` (empty data
   fork) + `res/System.ad` (AppleDouble resource fork), unpacked to `~/.executor/`
   at first run (`src/file/fileMisc.cpp:435`). Adding a checkbox + label means
   editing that **binary AppleDouble resource fork** with a resource editor
   (`Rez`/`DeRez` — e.g. from Retro68 — or a small script), then re-checking-in
   `res/System.ad`. There is no text `.r` source for it in the tree.
   - Suggested label: "Capture mouse when app doesn't respond" (or shorter
     "Dim desktop if app is stuck").
5. **Disabling must clean up:** `setEnabled(false)` must clear any active
   dim/capture (handled by the state machine animating back to 0).

### Tests / validation

- Native: assert the new constant round-trips through
  `ParseConfigFile`/`SaveConfigFile` (and the parse grammar accepts the keyword).
- Manual: toggle in Command-Shift-5, Save, verify the `.ecf` contains
  `CaptureUnresponsive`, relaunch honours it, and the feature is fully off (no
  dim, no capture) when unchecked.

---

## Alternative design: real mouse capture (not the default)

Instead of widening the click area (iteration 2), we could ask the host window
system to **grab the pointer** while the app is captured, so *all* pointer events
are redirected to the emulator window until the grab is released. This section
evaluates that alternative. It is **not** the proposed default; the input-region
approach stays the plan for now.

### What "real capture" means per front-end

There is no single cross-platform mechanism:

| Front-end / OS | Mechanism | Fidelity | Cost |
|---|---|---|---|
| X11 (`x`) | `XGrabPointer` / `XUngrabPointer`, optional `confine_to`, `XWarpPointer` | true global grab; all pointer events redirected, not only the holes | none |
| Qt (any platform) | `QWindow::setMouseGrabEnabled(true)` | forwarded to the platform plugin; support/behaviour varies | none |
| Wayland | only `zwp_pointer_constraints_v1` (`zwp_confined_pointer_v1` / `zwp_locked_pointer_v1`) + `zwp_relative_pointer_v1` | **no global grab by design**; confine/lock the pointer and get raw deltas, but only while the pointer is over the surface | none, but limited |
| macOS | `CGAssociateMouseAndMouseCursorPosition(false)`, `CGWarpMouseCursorPosition`, or a `CGEventTap` | full capture only via an event tap | event taps require Accessibility / Input Monitoring permission |
| Win32 | `SetCapture` (button-scoped) / `ClipCursor` | `SetCapture` only holds while a button is down, i.e. the wrong phase for us | none |
| SDL / SDL2 | `SDL_SetRelativeMouseMode` / `SDL_SetWindowGrab` | full relative capture | only in the non-rootless SDL front-ends |

### Mapping onto the state machine

Our captured state is active precisely while the button is **up**, but pointer
grabs are conventionally armed on button-down (`SetCapture`) or are long-lived
(`XGrabPointer`). So under this design the state machine would **arm the grab
when it enters the captured state and release it when the app becomes
responsive or the button goes down** — the same edges that toggle
`setClickCapture()` today. Release must additionally be forced on focus loss,
window close, and quit, or the user can be locked out of their desktop; an
unconditional escape hatch (hotkey, or auto-release on focus-out) is mandatory.

### Pros

- **Decouples input from paint.** This is the strongest argument given iteration
  3: approach A assumes a maskless, per-pixel-alpha `QWindow` still receives
  clicks everywhere, which is platform-dependent. A real grab *guarantees* the
  input side regardless of per-pixel alpha, so the mask can be dropped purely for
  the translucent dim and capture still works. The two concerns stop fighting
  over the single Qt mask.
- **Catches more than the holes.** Even a click that lands on a host window
  geometrically above the emulator (multi-monitor, always-on-top helpers) is
  redirected, which the input-region approach cannot do.
- **Enables true relative-mouse gameplay** (cursor hidden, raw deltas fed into
  `LM(MouseLocation)`), which is what many of these games actually want and is a
  valuable feature independent of responsivity detection.
- Uses the mechanism the window system provides for exactly this purpose.

### Cons / obstacles

- **Not portable.** Wayland deliberately offers no global grab, so capture there
  would be best-effort (confinement/relative-pointer) or fall back to the
  input-region path; Win32's `SetCapture` is button-scoped and unusable in the
  button-up phase; macOS may need a permission the user can refuse. So this
  becomes a per-front-end feature with uneven behaviour rather than one shared
  hook.
- **Invasive and risky.** A stuck grab traps the user's pointer; the release
  logic has to be bullet-proof across focus changes, crashes, and the debugger.
- **Permissions.** macOS event taps trigger TCC prompts; some users will refuse,
  which must degrade gracefully.
- **Changes mouse semantics if done as relative mode.** Synthesising
  `LM(MouseLocation)`/`GetMouse`/`Button` from deltas risks breaking apps that
  rely on absolute positions, and interacts with the existing cursor machinery
  (`ROMlib_showhidecursor`, `ROMlib_bewaremovement`, `adb_apeiron_hack`).
- **Does not replace iteration 3.** The dim still needs per-pixel alpha or an
  overlay window; real capture only removes the *need to widen the mask for
  input*.
- **Harder to test/automate** than setting an input region.

### Recommendation

- Keep the **input-region/mask approach as the default** (iteration 2): it uses
  each platform's native hit-testing, needs no permissions, behaves the same
  across front-ends, and matches the stated goal (clicks that would leak into the
  holes).
- Track **real mouse capture, presented as an opt-in "capture mouse" / relative
  mouse mode**, as a separate later feature, deliberately decoupled from the
  responsivity detection (an explicit user toggle, not an automatic reaction).
  That avoids the button-up/button-down phase mismatch and the lock-out risk.
- If it is pursued, phase it: X11 and Qt-on-X11 first (real grab, no
  permissions), then macOS (with a graceful no-permission fallback), then
  Wayland (confinement/relative-pointer or fall back to the input region); Win32
  last.
- Where a real capture *is* available, the two designs compose well: grab the
  pointer for input and drop the mask for the translucent dim — avoiding
  approach A's reliance on masking-free hit-testing.

## Cross-cutting concerns

### Config / CLI summary

| Knob | Where | Default |
|------|-------|---------|
| enable/disable | prefs dialog (iteration 5) + CLI | enabled |
| timeout | CLI `--unresponsive-timeout <ticks>` (iteration 1); optional prefs field later | 60 ticks (1 s) |
| dim target / fade duration | constants (`kCaptureDim`, `kFadeDurationMs`) | 0.5, 300 ms |
| button-held rule | built in (not configurable) | on |

### Documentation updates

- `docs/subsystems/video-driver.md`: new `VideoDriver` hooks, rootless click
  capture, dim/fade feedback.
- `docs/subsystems/event-manager.md`: `SystemTask`/`WaitNextEvent`/`ModalDialog`
  feed the tracker; `GetOSEvent` deliberately does not.
- `docs/subsystems/window-dialog-menu.md`: rootless mask vs. input region, and
  the new feedback behaviour.
- This document.

### Risks / open questions

1. **Qt's conflated mask** (paint clip == input shape) is the crux of iterations
   2–3; approach A (toggle mask ↔ translucent ARGB window) is selected. Verify
   per platform that a maskless, per-pixel-alpha `QWindow` still receives clicks
   (needed for capture), with a documented fallback (approach B) where not.
2. **Implicit grab.** The button-held rule assumes the window system keeps the
   in-progress press/release with the emulator window even as capture turns off.
   Verify on Qt and Wayland.
3. **Host-side polling** is required because the guest may never yield. Ensure
   the periodic timer lives on the GUI thread and never blocks the guest.
4. **Button state source.** Use a host-side flag in `EventSink`, not
   `LM(MBState)` (which is written on the emulator thread and would race).
5. **Hysteresis.** Decide the debounce for a flapping app before implementing
   the fade, to avoid visible flicker.
6. **Non-rootless modes** must be unaffected; guard all feedback behind
   `vdriver->isRootless()`.
7. **Real mouse capture** (see the alternatives section) would decouple input
   from paint — the cleanest fix for the Qt mask conflict — but is not portable
   (Wayland has no global grab) and risks locking the user out. Revisit as an
   opt-in relative-mouse mode rather than folding it into the default path.

## Suggested implementation order

1. `Responsiveness` module + hooks + host button state + CLI + tests
   (iteration 1) — independently valuable and low-risk.
2. Wayland click capture + dim + fade (iterations 2–4 on the easy surface),
   using the forced-capture debug hook.
3. Qt click capture + dim + fade via approach A.
4. Prefs dialog wiring + resource edit (iteration 5).

## References

- `src/vdriver/vdriver.h`, `src/vdriver/vdriver.cpp` — `VideoDriver`,
  `Framebuffer`, rootless region plumbing.
- `src/vdriver/eventsink.cpp`, `eventrecorder.cpp` — host input funnel and the
  button-state source.
- `src/wind/windRootless.cpp` — `ROMlib_rootless_update`, open/close menu.
- `src/config/front-ends/qt/qt.cpp` — `setMode`, `render`, `setMask`.
- `src/config/front-ends/wayland/wayland.cpp` — `updateMode`, `frameCallback`,
  `set_input_region`.
- `src/desk.cpp`, `src/toolevent.cpp`, `src/dial/dialHandle.cpp` — trap entry
  points.
- `src/prefs/{prefs.h,options.cpp,parse.h,parse.ypp,prefs.cpp}`,
  `src/prefpanel.cpp`, `src/include/rsys/prefpanel.h` — preferences plumbing.
- `res/System.ad` — binary AppleDouble resource fork holding `DLOG`/`DITL` -4063.
- `docs/subsystems/video-driver.md`, `event-manager.md`,
  `window-dialog-menu.md`.
