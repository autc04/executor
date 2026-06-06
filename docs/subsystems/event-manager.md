# Event & OS Event Manager

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

The Event Manager delivers keyboard, mouse, update, activate, disk, and application
events to Mac applications via `GetNextEvent`, `WaitNextEvent`, and `PostEvent`. The
OS Event Manager manages the low-level event queue (OS queue `EvQHdr`) and maps host
input events (delivered by the video driver) into Mac event records. Apple Events are
handled by a separate layer built on top of the basic event infrastructure.

## Key Concepts

**High-Level Event (HLE) queue** (`hle.cpp`): `hle_get_event` is the core event-
fetching function. It checks, in priority order: pending Apple Events, mouse events,
keyboard auto-repeat, and then reads the OS event queue. The result is written into
the `EventRecord` passed by the application.

**OS Event queue** (`LM(EvQHdr)`): a Mac OS queue of `EvQEl` (event queue elements).
`ROMlib_PPostEvent` enqueues a new element. The queue is in the guest address space and
elements are `GUEST_STRUCT`-wrapped.

**`EventSink`**: the video driver delivers input events by calling methods on
`EventSink` (see `video-driver.md`). `EventSink::pumpEvents()` drains the event work
queue and posts entries to the OS queue or directly to the HLE mechanism.

**Keyboard translation** (`osevent.cpp`):
- `ROMlib_kchr_ptr()` returns a pointer to the KCHR keyboard layout resource, used to
  map virtual key codes to Mac character codes.
- `ROMlib_set_keyboard(name)` selects a keyboard layout by name.
- `ROMlib_right_to_left_key_map` handles RTL keyboard remapping for Arabic/Hebrew.

**Key state** (`ROMlib_GetKey`, `ROMlib_SetKey`): the emulated key state bitmap. Each
virtual key code (MKVKey) has a corresponding bit. The low-memory `LM(KeyMap)` 16-byte
bitmap is updated from this state.

**Auto-key** (`ROMlib_SetAutokey`): posts repeated keyboard events when a key is held
down, implementing the Mac auto-repeat mechanism.

**Modifier keys**: `ROMlib_GetModifiers()` returns the current modifier flag word
(Shift, Command, Option, Control, Caps Lock) by reading the key state bitmap.

**`hle_init` / `hle_reinit` / `hle_reset`**: lifecycle functions for the HLE queue.
`hle_reinit` clears pending events when a new application launches.

## Apple Events (`src/appleevent/`)

Apple Events are high-level inter-application messages. Executor's implementation
provides enough compatibility to receive `aevt` events from the Finder (open
documents, quit) and dispatch them to the application's Apple Event handler.

**`AE_desc`**: the internal representation of an `AEDesc`. Executor stores AE
descriptors inline in guest memory using `inline_desc_t` (type + size + data[0]).

**`list_header_t`** and **`ae_header_t`**: the internal on-heap layout of an
`AEDescList` and `AppleEvent` respectively. The `param_offset` field separates
attributes from parameters in the event record.

**`AE_init` / `AE_reinit`**: initialise the Apple Event dispatch table. `AE_reinit`
clears handlers registered by the previous application.

## Source Files

| Path | Description |
|------|-------------|
| `src/osevent/osevent.cpp` | `GetNextEvent`, `WaitNextEvent`, queue management |
| `src/osevent/hle.cpp` | HLE queue: `hle_get_event`, keyboard auto-repeat |
| `src/osevent/osevent.h` | Event Manager internal API |
| `src/osevent/ibm_keycodes.cpp` | PC→Mac virtual key code mapping |
| `src/appleevent/AE.cpp` | `AESend`, `AEProcessAppleEvent`, `AEInstallEventHandler` |
| `src/appleevent/AE_desc.cpp` | `AECreateDesc`, `AEGetDescData`, `AEDisposeDesc` |
| `src/appleevent/AE_coercion.cpp` | Apple Event type coercion |
| `src/appleevent/AE_hdlr.cpp` | Apple Event handler dispatch table |
| `src/appleevent/apple_events.h` | Internal types: `inline_desc_t`, `ae_header_t` |

## Important Data Structures

- **`EvQEl`** (Mac ABI, guest): event queue element: `evtQWhat`, `evtQMessage`,
  `evtQWhen`, `evtQWhere`, `evtQModifiers`.
- **`EventRecord`** (Mac ABI, guest): the structure filled in by `GetNextEvent`.
- **`inline_desc_t`** (guest struct): embedded Apple Event descriptor: type, size,
  data bytes inline.
- **`ae_header_t`** (guest struct): Apple Event container header with attribute count,
  parameter count, event class, and event ID.

## Key Functions / Traps

| Symbol | Trap | Description |
|--------|------|-------------|
| `C_GetNextEvent(mask, event)` | `_GetNextEvent` | Fetch the next matching event |
| `C_WaitNextEvent(mask, event, sleep, rgn)` | `_WaitNextEvent` | Sleep until an event arrives |
| `C_PostEvent(what, msg)` | `_PostEvent` | Post a synthetic event |
| `ROMlib_PPostEvent(code, msg, qelp, when, where, mods)` | — | Internal queue post |
| `hle_get_event(evt, remove)` | — | Core event fetch (HLE queue) |
| `C_AEInstallEventHandler` | — | Register an Apple Event handler |
| `C_AEProcessAppleEvent(event)` | — | Dispatch a received Apple Event |

## Design Notes / Gotchas

- **Thread safety**: events are delivered from the GUI thread via `EventSink`. They
  must be consumed on the emulator thread via `pumpEvents()`. Never post directly to
  the OS queue from the GUI thread.
- **`ROMlib_bewaremovement`**: a flag set when the mouse is near the edge of the screen
  in rootless mode, used to suppress accidental window drags.
- **Key map consistency**: `LM(KeyMap)` is a 16-byte guest bitmap; `ROMlib_GetKey`
  consults the host-side state, which is kept in sync with `LM(KeyMap)` on every key
  event.
