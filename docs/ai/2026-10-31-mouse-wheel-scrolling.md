# Mouse-wheel scrolling

> **AI-generated.** This document was produced with the assistance of an AI language
> model and may contain inaccuracies.

## Problem

Classic Mac applications have no mouse-wheel event. The emulator receives wheel
input from its front-ends, but until now it either ignored it (SDL2, Wayland) or
mistook it for button clicks (X11/SDL1, where wheels arrive as buttons 4/5).

## Approach

Host wheel input goes through the normal `IEventListener::mouseWheelEvent()` path
and is marshalled to the emulator thread like a mouse event. Instead of inventing
an event, `ROMlib_handleWheelEvent()` (`src/osevent/wheel.cpp`) posts an ordinary
`mouseDown`/`mouseUp` pair on a scroll bar. The guest's own
`FindWindow`/`FindControl`/`TrackControl`/`DialogSelect` code does the rest.

Key points:

- **Direct invocation.** There is no pending-wheel queue and no
  `OSEventCommon` integration: `EventSink` calls `ROMlib_handleWheelEvent()`
  directly from the emulator-thread work item. The posted clicks are therefore
  indistinguishable from real ones.
- **Interrupt safety.** The handler runs from `Interrupt::handleCommon()`, so it
  avoids `FindWindow`, `FindControl`, `TestControl` and changes to
  `qdGlobals().thePort`, all of which can call guest code. It reads only
  `WindowRecord`/`ControlRecord` fields and region data, does coordinate
  conversion with `PORT_BOUNDS()`, calls the native `C_PtInRgn`/`C_PtInRect`/
  `C_TickCount` implementations rather than the trap wrappers (which may invoke
  patched guest code), and posts through `ROMlib_PPostEvent()` (explicit
  `where`/`when`/modifiers).
- **Initialization guards.** It bails unless `LM(WWExist) == EXIST_YES`
  (`InitWindows` has completed) and `default_aux_ctl` is set (Control Manager
  colour init ran). Window and control lists are null-checked and the walks have
  iteration caps.
- **Scroll-bar heuristics.** `FindControl()` is not used, because a document's
  scroll bar is responsible for the whole document body, not just the area under
  the pointer. A candidate is a visible, enabled control with `min < max` and a
  clearly elongated rect. Selection is:
  - vertical: the bar under the pointer, otherwise the nearest bar whose left
    edge is at/right of the pointer **and whose vertical span contains the
    pointer**;
  - horizontal: the bar under the pointer, otherwise the nearest bar below it
    whose horizontal span contains the pointer.
  The synthetic click goes to the top/bottom (or left/right) arrow centre. The
  span requirement keeps a split-pane bar from stealing wheel events when the
  pointer is above or below it.
- **Guards.** No clicks while a physical button is held (`LM(MBState) == 0`) or
  while `ROMlib_menuhelper()` is tracking a menu (pull-down, popup, hierarchical
  or sticky). The menu guard is a depth counter so nested/hierarchical tracking
  works.
- **Action timing.** `C_TrackControl()` now invokes the control action proc once
  for the initial part before entering its tracking loop. Wheel-generated clicks
  arrive with the matching `mouseUp` already in the queue, so the loop would
  otherwise exit without ever calling the action; the upfront call makes such a
  click scroll. See `docs/subsystems/textedit-controls.md`.
- **`visRgn` check.** Before posting, the arrow point (window-local coordinates)
  must be inside `PORT_VIS_REGION(wp)`. This rejects a scroll bar that is covered
  by a window in front even though the pointer itself is on a visible part of
  the window.

## Debugging

Set `EXECUTOR_WHEEL_DEBUG` to print the host event, the GUI→emulator-thread hop,
and the complete window/control lookup on stderr. Every early return has a
`bail:` line naming the failed gate (initialization, menu tracking, button down,
no window, no bar, `visRgn`, …), so the last line printed identifies where the
translation stopped.

## Known limitations

- Custom scroll bars with non-standard arrow geometry are handled best-effort;
  candidates are not validated via the CDEF because that would call guest code
  from an interrupt.
- A control that happens to be tall/narrow with `min < max` could be mistaken
  for a scroll bar. Requiring the CDEF resource type/id is a possible later
  refinement.
- The vertical-span requirement means a wheel over a header row above a bar that
  spans only the body (or over a split pane's non-overlapping region) does not
  scroll; that is deliberate for the first experiment and may be refined with
  dialog-manager information later.
- In the standard file dialog's sticky tracking loop after the button is
  released, a wheel over an unrelated scroll bar could still be injected.
- Wayland emits one notch per `wl_pointer.axis` event; high-resolution devices
  may produce many events per gesture.
