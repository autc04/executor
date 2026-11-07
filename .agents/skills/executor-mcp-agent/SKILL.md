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
- **The click that activates a background window is consumed.** Clicking an
  icon in an inactive window needs: `click` (activate) → `click` (select) →
  `click count: 2` (open). Watch the title bar for the active-window look.
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
- Classic Mac OS never queues key-up events and Executor masks them for the
  whole session, so a key reply normally reads
  `delivered: true, suppressed: ["keyup"]`. That is normal — do not "fix" it by
  retrying.
- A real key press needs the application to be frontmost; a document window
  that is merely visible receives nothing.

## Reading `status`

| Field | Meaning and what to do |
|---|---|
| `app`, `front` | Application the guest runs and the title of its front window. During a launch `front` still belongs to the previous process. |
| `pumping`, `in_event_wait` | `pumping: false` means the application has neither returned to its event loop nor been inside `WaitNextEvent`/`ModalDialog` for a second: it is stuck and **will ignore input**. Stop clicking, `wait`, look again. |
| `events_pending` | Events queued and not yet consumed. A number that keeps growing while `pumping` is false is a stuck guest, not slow input. |
| `busy` | An injected sequence (click/drag/type) is still running. |
| `mouse`, `button`, `modifiers` | Current pointer, button state, modifier keys. `button: true` while a drag or menu is in progress. |
| `dbltime` | Double-click threshold in ticks (60/s) — how close `count: 2`'s clicks must be. |

## Recipes

**Launch an application from the Finder.** Screenshot → the icon is in a
folder window: activate that window (`click` on its title bar), then
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

1. `status`: is `pumping` true? Is `busy` stuck non-zero? Is `events_pending`
   growing?
2. Screenshot: is there an alert or a modal dialog you missed, possibly under
   another window?
3. Is the application actually frontmost (`app`, and the Application menu at the
   right end of the menu bar)?
4. Is the click landing where you think? The pointer is invisible in
   screenshots — compare `mouse` with the intended target, and crop the target
   area to check.
5. `session ended: executor closed the control connection` means the emulator
   is gone; the server is unattached now and needs a new `attach` (with the new
   socket path, since it contains the pid).
