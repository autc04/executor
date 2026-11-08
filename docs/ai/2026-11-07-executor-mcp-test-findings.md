# executor-mcp: findings from the first agent-driven test run

Test session: an agent attached to a running Executor
(`unix:/run/user/1026/executor/2382901.sock`, 1024×768 at 8 bpp), launched
ClarisWorks 3.0 from the Finder and created an untitled Drawing document. This
note records what broke and what the agent had to discover on its own; the
corresponding fixes are in the same change.

## What worked

`attach`, `status`, `screenshot` (full screen), `move`, `button_down`/`button_up`,
`click` (including `count: 2`), and the recovery behaviour of a dropped session
(`session ended: executor closed the control connection`) all behaved as
designed.

## Bugs

### B1. Every keyboard result reports `delivered: false`

`delivered` ANDs the mask check for *every* event a verb posts, and `key`/`type`
post `keyDown` **and** `keyUp`. Executor masks `keyUp` permanently —
`LM(SysEvtMask) = ~(1L << keyUp)` in `src/init.cpp` and
`src/osevent/osevent.cpp`, because classic Mac OS never queues key-up events.
So the field is always `false` for keyboard input no matter what the guest does.
The run's two `press_key` calls (Cmd-O, Escape) both reported `false`;
`keyDown` had in fact been posted and the mask allowed it.

Fix: `delivered` plus a `suppressed` array naming the event classes the guest
masked, with `keyUp` excluded — it can never be delivered, so counting it
destroys the signal.

### B2. `screenshot`'s `region` argument cannot be passed

The schema was built with `pointProperty()`, so it advertised
`minItems: 2, maxItems: 2` while the verb needs `[left, top, right, bottom]`.
The client rejected the call before it ever reached the server
(*"must NOT have more than 2 items"*). Fix: a four-element schema.

### B3. Control-side failures look like successful tool calls

`screenshot` returned `toolReply(reply)` even when the control reply was
`ok: false`, so the failure arrived as a normal result with the error buried in
a text block; only `isError` was missing. Fix: one conversion point that turns
`ok: false` into a tool error for every verb.

### B4. `wait` is a round trip to the guest

The design says `wait` is pure host timing, but the tool forwarded it to the
guest. That is exactly backwards: `wait` is what an agent needs when the guest
is wedged, and then it fails or burns the 120 s request timeout. Fix: sleep in
the MCP process.

### B5. `status.modifiers` is a raw bitmask containing the button state

Every sample reported `128`, which is `btnState` — not a modifier — while the
same reply said `button: false`. Fix: an object of booleans, with the raw value
kept as `modifiers_raw`.

### B6. `status` cannot name the running application

`front` is a window *title*, so "no window", "the Finder is front" and "an app
is front but has no titled window" are indistinguishable, and during a launch
the title still belongs to the previous process. Fix: `app`, from
`LM(CurApName)`.

### B7. `pumping: false` is ambiguous

`pumping` means "the guest called SystemTask/WaitNextEvent/ModalDialog within
the last second". A guest that is *inside* a blocking `WaitNextEvent`, or in a
menu tracking loop, legitimately does not call them and looks hung.
`Responsiveness` already tracks that state (`activeDepth_`); `status` ignored
it. There was also no way to tell whether injected events were piling up. Fix:
`in_event_wait`, `pumping` honouring it, and `events_pending` (length of the
guest event queue).

### B8. The pointer is invisible and its position is not reported

The guest cursor is a host cursor and is never composited into the framebuffer
(deliberate, see the design document), but the `screenshot` result omitted the
pointer position that the design document promises in its text block. Fix:
`frame` reports `mouse` and `button`; the tool puts them in the metadata block.

### B9. Bad arguments surface as raw C++ exceptions

`button_down` with `x` but no `y` throws `std::out_of_range` from
`args.at("y")`, which the agent sees as a `boost::json` message. Fix: validate
arguments and return a normal tool error.

### B10. Unexplained: the guest stopped pumping for ~30 s

Sequence: `move` to the Apple menu, `button_down` (menu opened), screenshot,
`button_up` at `(19, 10)` — the menu's title bar — which closed the menu without
selecting anything. From then on `pumping` was `false` and `since_response_ms`
climbed past 30 s; injected `move`/`click`/`key` were accepted by the mask but
nothing changed on screen and `front` never changed. A real host mouse event
revived the session, after which the Finder redrew with its folder window
re-laid out.

**Status: open, not reproducible.** The sequence was replayed later, on the same
live session and on a fresh headless one: the menu opens on the button-down,
closes on the button-up on its title bar, and menus keep working afterwards;
`pumping` stayed true throughout a 15 s poll. Whatever produced the 30 s without
a single `SystemTask`/`WaitNextEvent`/`ModalDialog` was not the menu release.
`events_pending` and `in_event_wait` exist to make the next occurrence
diagnosable; the next step is a run under `--logtraps` (see the
`executor-guest-debugging` skill).

### B11. Input has no effect because a modifier is stuck (found while chasing B10)

The live session was in a state where every injected click was accepted by the
event mask, the pointer moved, the guest pumped every ~60 ms — and nothing
reacted: icons never highlighted, windows never activated, the screen did not
change. `status` reported `modifiers: 384`, i.e. `cmdKey|btnState`: the guest
believed Command was held, so every click was a ⌘-click. One command key
press/release cleared the bit and input worked again (Apple menu opens and
closes, a dialog's Cancel button closes it).

Nobody in that session had pressed Command through the API, and a *completed*
`key`/`type` command cannot leave a modifier down — it presses and releases it
symmetrically. The plausible origin is the host: no front-end released held
keys when the window lost the foreground, and the Wayland front-end learns about
focus only through the toplevel `activated` state, after which the key-up for a
held modifier is never delivered.

Both halves are fixed:

- `EventSink::suspendEvent()` — which every front-end calls when the window
  loses the foreground — now clears every bit still set in `LM(KeyMap)` (caps
  lock excepted, it is a lock) and releases a held mouse button, before the
  suspend event reaches the guest.
- `key`/`keydown`/`keyup` accept modifier names (`shift`, `cmd`, `opt`, `ctrl`),
  so a modifier can be held across requests and a stuck one can be released with
  `key_up "cmd"` instead of the ⌘-O press/release that was used here as a
  workaround.

The raw `modifiers` bitmask is what made this hard: `384` is not readable at a
glance, and `btnState` is set while the button is *up* (`ROMlib_GetModifiers()`
inverts it), so every sample carries `0x80` and "no modifiers" reads as `128`.
That is why `status` now also reports a `modifiers` object (B5).

## Workflow traps (documentation, not bugs)

- **A plain `click` cannot use a menu.** It posts down and up at the same point,
  so a menu opens and closes immediately. Menus need `button_down` on the title,
  `move` to the item, then `button_up` (or `drag`).
- **The first click on a window that is not in front is consumed by
  activation.** A double click on an icon in such a window therefore needs three
  clicks: activate, then `click count: 2`. There is no application-level
  version of this: Executor runs one application at a time, so clicking the
  desktop can never bring another program forward.
- **`click` says nothing about the effect.** Every input needs a `wait` and a
  screenshot; the tool replies only report that the events were posted.
- **Downscaling destroys classic-Mac text.** `max_width` is a nearest-neighbour
  resample; 9-point Chicago survives cropping (`region`) but not shrinking.
- **`attach`'s `app` field is usually empty.** It is the argument Executor was
  launched with, not the app the guest is running (see B6).

## Where the knowledge went

| Knowledge | Home |
|---|---|
| Coordinate space, cursor not composited, `region` = `[l,t,r,b]` | `screenshot` description |
| `delivered`/`suppressed`, `keyUp` is normal | `key`-family descriptions |
| Meaning of `pumping`, `in_event_wait`, `events_pending`, `app`, `front`, `modifiers` | `status` description |
| `count: 2` is a double click; the first click may only activate | `click` description |
| A stuck modifier makes every click a modified click; `key_up <modifier>` clears it | `status` description, `key_down`/`key_up` descriptions, skill triage |
| "input → wait → screenshot, `ok` is not proof" | `initialize.instructions` |
| Menus, activation clicks, app launching, modal alerts, wedged-guest triage | `executor-mcp-agent` skill |
