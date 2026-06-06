# TextEdit, Control & List Managers

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

TextEdit provides editable single- and multi-line text fields with selection, cut/copy/
paste, and caret management. The Control Manager handles buttons, check boxes, radio
buttons, scroll bars, and pop-up menus. The List Manager implements scrollable item
lists. All three are high-level toolbox widgets built on top of QuickDraw and the
Window Manager.

## TextEdit

### Key Concepts

**`TERec`**: the core TextEdit record. Fields include `hText` (Handle to the text
data), `selStart`, `selEnd`, `destRect` (typesetting rectangle), `viewRect` (visible
rectangle), `lineStarts` (array of line-start offsets), `nLines`, and style
information (`txFont`, `txFace`, `txSize`, `txMode`).

**`TEHIDDENH`**: a hidden extension handle attached to every `TERec`. Accessed via
`TE_FLAGS(te)`. Carries implementation-private state not part of the public `TERec`
layout.

**Stylised TextEdit** (`teIMIV.cpp`, `teIMV.cpp`): when `txSize == -1` the record is
stylised; a separate style table handle carries per-run font/style information. The
`TE_STYLIZED_P(te)` macro detects this mode.

**`ROMlib_call_TEDoText`**: the internal dispatcher that calls the application's
`doText` hook (if set) or Executor's own text drawing/hit-testing/caret-drawing code.

**Line layout**: `teInsert.cpp` rewraps lines after any text change. `teDisplay.cpp`
handles caret blinking and selection highlighting.

### Source Files

`src/textedit/teInit.cpp`, `teAccess.cpp`, `teDisplay.cpp`, `teEdit.cpp`,
`teInsert.cpp`, `teMisc.cpp`, `teScrap.cpp`, `teIMIV.cpp`, `teIMV.cpp`

### Key Traps

`TENew`, `TEDispose`, `TEActivate`, `TEDeactivate`, `TEIdle` (caret blink),
`TEKey`, `TEClick`, `TESetText`, `TEGetText`, `TEInsert`, `TEDelete`,
`TESetSelect`, `TECopy`, `TECut`, `TEPaste`, `TEScroll`, `TECalText`

---

## Control Manager

### Key Concepts

**`ControlRecord`**: the per-control state. `contrlVis` (visibility), `contrlHilite`
(highlight / disabled state), `contrlValue`, `contrlMin`, `contrlMax`, and `contrlProc`
(CDEF handle).

**CDEF** (Control Definition Function): `ctlStddef.cpp` and `ctlArrows.cpp` implement
the standard push-button, check-box, radio-button, and scroll-bar CDEFs.
`ctlPopup.cpp` implements the pop-up menu control.

**`TrackControl`**: the synchronous mouse-tracking loop. It calls `FindControl` to
hit-test the click, then loops calling the CDEF's track message until the mouse button
is released, updating the control's highlight state.

**Arrow bitmaps**: `.map` files in `src/ctl/` contain raw bitmap data for scroll-bar
arrows and thumb; these are compiled into the binary as byte arrays.

### Source Files

`src/ctl/ctlInit.cpp`, `ctlDisplay.cpp`, `ctlMisc.cpp`, `ctlMouse.cpp`,
`ctlSet.cpp`, `ctlSize.cpp`, `ctlArrows.cpp`, `ctlPopup.cpp`, `ctlIMIV.cpp`,
`ctlStddef.cpp`

### Key Traps

`NewControl`, `DisposeControl`, `ShowControl`, `HideControl`, `DrawControls`,
`FindControl`, `TrackControl`, `GetControlValue`, `SetControlValue`,
`GetControlMinimum`, `SetControlMinimum`, `GetControlMaximum`, `SetControlMaximum`,
`HiliteControl`, `MoveControl`, `SizeControl`

---

## List Manager

### Key Concepts

The List Manager (`src/list/`) implements a scrollable, selectable list of text or
custom cells. A `ListHandle` (handle to a `ListRec`) tracks the data handle, selection
state, cell size, and associated scroll bars. The LDEF (List Definition Function)
renders individual cells.

---

## Design Notes / Gotchas

- **Handle locking in TextEdit**: `TERec` and `hText` must be locked before any
  operation that might move memory (e.g., `ROMlib_call_TEDoText`). The `TE_DO_TEXT`
  macro wraps this with an `HLockGuard`. Failing to lock is the most common source of
  heap corruption in TextEdit code.
- **`TEP_*` vs `TE_*` macros**: `TE_*` macros take a `TEHandle` (handle to `TERec`);
  `TEP_*` macros take a `TERec*` (already-dereferenced). In debug builds, `TEP_*`
  asserts that the handle is locked.
- **Control enable/disable**: `contrlHilite == 255` means the control is disabled
  (dimmed). `HiliteControl(c, 255)` disables; `HiliteControl(c, 0)` re-enables.
- **Pop-up menus** (`ctlPopup.cpp`): the pop-up menu control is an extension not
  present in early Mac OS versions. It stores a `MenuHandle` in `contrlData` and
  uses `MenuSelect` for tracking.
- **`TE_SLAM`**: a debug assertion that verifies the internal consistency of a `TERec`.
  Only active when `ERROR_TEXT_EDIT_SLAM` is enabled at compile time.
