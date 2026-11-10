---
name: executor-mcp-agent
description: Use when driving a running Executor session through the executor-mcp tools — launching an application in the emulated Mac, operating menus and dialogs, typing into a document, or working out why injected input has no visible effect.
---

# Driving an emulated Mac through `executor-mcp`

The MCP server (`tools/executor-mcp`) is a thin client of Executor's control
socket (`executor --control auto|unix:<path>|tcp:<host>:<port>`). It injects
input through the same `EventSink` a real user goes through, so it can drive any
classic application; it cannot start or restart the emulator, and it does not
know which session you mean until `attach` is called.

For a session you drive interactively, start Executor with
`--control … --no-suspend-events`: otherwise switching the window to the
background tells the application it is suspended and it deactivates its front
window, so the next injected click only reactivates it. The `attach` reply's
`suspend_events` says which kind of session this is.

Protocol and design: `docs/ai/2026-11-06-agent-control-design.md`. Findings
from the first agent-driven run: `docs/ai/2026-11-07-executor-mcp-test-findings.md`.

## The loop

```
attach("unix:/run/user/<uid>/executor/<pid>.sock")
screenshot            → decide what to do
click / press_key / type_text
wait(300…800)         → let the guest run
screenshot            → confirm the *result*, not the tool's "ok"
```

Every input tool reports only that the events were **posted**. The control
protocol's `delivered`/`suppressed` describe the guest's event mask, not what
the application did with the events. The screenshot is the only ground truth.

## One application at a time

Executor runs **a single Mac application**. There is no application switcher,
no application menu, and nothing running in the background: the desktop and its
folder windows are Executor's own Browser, which is not running while another
application is. So clicking the desktop or a Finder window while ClarisWorks is
front does nothing at all — it cannot bring anything forward. Switching
applications means quitting the current one back to the Browser and launching
another.

"Activate a window" therefore only ever means *make one of the running
application's own windows front*. `status.front` names that window.

## Coordinates and screenshots

- Coordinates are **guest pixels**, 1:1 with the top-level screenshot. The
  metadata block gives `screen_width`/`screen_height`; when you crop or
  downscale, `width`/`height` describe the image you got.
- The Mac's pointer is a host cursor and is **never drawn** into the image. The
  metadata's `mouse: [x, y]` (and `status.mouse`) is how you know where it is.
- To read small UI text, **crop** with `region: [left, top, right, bottom]`.
  9-pixel Chicago is legible in a crop and mush after `max_width` resampling,
  so use `max_width` only for a whole-screen overview.
- The screen is often 8 bpp; dithered greys are normal.

## Mouse

- `click` posts down and up at the same point; `count: 2` is a double click.
- **A click that only activates a window is consumed.** In a window that is not
  in front, the first click activates it, so selecting an icon there needs a
  second click and opening it a double click after that (`click` → `click` →
  `click count: 2`). `status.front` tells you which window is in front.
- **`click` cannot use a menu.** Down opens the menu and the up at the same
  point closes it again. Instead:
  `button_down(x=menu title, y≈10)` → `move(item)` → `button_up`, or a single
  `drag` from the title to the item. `drag` is also what window moving,
  resizing and rubber-band selection need.
- Aim at icon *graphics*, not at the label: clicking a selected item's label
  starts a rename.

## Keyboard

- `type_text` is printable ASCII only and goes to **whoever has focus**. A
  freshly opened document has no insertion point — click into the canvas or
  field first.
- `press_key` takes one character or a name (`Return`, `Tab`, `Escape`,
  `LeftArrow`, `F5`, …) plus modifiers.
- A modifier can be named as the key: `key_down "cmd"` holds it across further
  requests (real chords), `key_up "cmd"` releases it. Use that to let go of a
  modifier that got stuck; do not send a fake ⌘-keypress as a workaround.
  `caps` is not a name: its bit is the caps *lock* state.
- Classic Mac OS never queues key-up events and Executor masks them for the
  whole session, so a key reply normally reads
  `delivered: true, suppressed: ["keyup"]`. That is normal — do not "fix" it by
  retrying.
- Keys go to the **front window** of the running application; a window that is
  merely visible receives nothing, so click into it first.

## Reading `status`

| Field | Meaning and what to do |
|---|---|
| `app`, `front` | The one application Executor is running, and the title of its front window. During a launch `front` still names a window of the previous application. |
| `pumping`, `in_event_wait` | `pumping: false` means the application has neither returned to its event loop nor been inside `WaitNextEvent`/`ModalDialog` for a second: it is stuck and **will ignore input**. Stop clicking, `wait`, look again. |
| `events_pending` | Events queued and not yet consumed. A number that keeps growing while `pumping` is false is a stuck guest, not slow input. |
| `busy` | An injected sequence (click/drag/type) is still running. |
| `mouse`, `button`, `modifiers` | Current pointer, button state, modifier keys. `button: true` while a drag or menu is in progress. |
| `dbltime` | Double-click threshold in ticks (60/s) — how close `count: 2`'s clicks must be. |

## Recipes

**Launch an application from the Browser.** Screenshot → the icon is in a
folder window: bring that window to the front (`click` on its title bar), then
`click count: 2` on the icon → `wait(5000)` → screenshot. Classic apps show
splash screens and modal alerts before any window; read them from the
screenshot instead of assuming the launch failed. ClarisWorks 3.0, for example,
opens a "This copy of ClarisWorks has been modified by another program or
virus" alert that has to be dismissed with **Continue**.

**Dismiss an alert.** The default button has the thick outline and usually has
Return as its keyboard equivalent, but check the two labels: choosing the
default is only right when the default is what you want.

**Choose a menu item.** `button_down` on the menu title, `move` to the item
(the menu stays open; screenshot if unsure of the item's y), `button_up` on the
item. Do not let the release land on the menu's title bar — that cancels.

**Open a new document.** `File ▸ New…` → choose the document kind in the dialog
→ OK. Verify by reading the title bar (`Untitled 1`) and the appearance of the
document's tool palette.

## When nothing happens

1. `status.modifiers`: if any of `shift`/`cmd`/`option`/`control` is true while
   nobody is holding it, every click is a *modified* click and the application
   will ignore it. Release it with `key_up` naming that modifier (`cmd`,
   `shift`, `opt`, `ctrl`) and try again. A modifier can be left down when the
   session loses the foreground while it is held; Executor releases held keys on
   focus loss, but a session that was already running before that fix, or a
   client that used `key_down` without `key_up`, can still strand one.
2. `status`: is `pumping` true? Is `busy` stuck non-zero? Is `events_pending`
   growing?
3. Screenshot: is there an alert or a modal dialog you missed, possibly under
   another window?
4. `status.app`: is that the application you meant? Exactly one runs at a time,
   so a click on the desktop or on a Finder window cannot bring another one
   forward — if the wrong application is running, quit it back to the Browser
   and launch the right one.
5. Is the click landing where you think? The pointer is invisible in
   screenshots — compare `mouse` with the intended target, and crop the target
   area to check.
6. `session ended: executor closed the control connection` means the emulator
   is gone; the server is unattached now and needs a new `attach` (with the new
   socket path, since it contains the pid).
