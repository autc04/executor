# Window, Dialog & Menu Managers

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

These three managers implement the classic Mac OS user-interface widgets. The Window
Manager owns window creation, sizing, dragging, z-order, and painting. The Dialog
Manager builds modal and modeless dialogs from `DLOG` and `DITL` resources and handles
item tracking. The Menu Manager draws the menu bar, handles pull-down menus, and
dispatches menu selections.

## Window Manager

### Key Concepts

**`WindRecord` / `WindowPeek`**: the in-memory representation of a window. `WindowPeek`
(a pointer to the `WindowRecord` alias) exposes the full private state; `WindowPtr` is
the public opaque pointer. The `windrestype` struct (`wind.h`) mirrors the `WIND`
resource layout used to create windows.

**WDEF** (Window Definition Function): each window style (document window = WDEF 0,
dialog box = WDEF 16, etc.) is implemented by a C++ function called via
`WINDCALL(w, message, param)` → `ROMlib_windcall`. `C_wdef0` and `C_wdef16` implement
the two standard WDEFs.

**Rootless mode**: when `Framebuffer::rootless` is true, windows are not drawn onto the
emulator framebuffer. Instead, `windRootless.cpp` delegates the actual window rectangle
to the host compositor. `ROMlib_rootless_update`, `ROMlib_rootless_openmenu`, and
`ROMlib_rootless_closemenu` manage compositing state.

**Update region**: `LM(VisRgn)` and per-window update regions control which portions of
a window need repainting after being obscured or revealed.

### Source Files

`src/wind/windDisplay.cpp`, `windMisc.cpp`, `windMouse.cpp`, `windDocdef.cpp`,
`windSize.cpp`, `windUpdate.cpp`, `windColor.cpp`, `windRootless.cpp`, `windInit.cpp`

### Key Traps

`NewWindow`, `DisposeWindow`, `ShowWindow`, `HideWindow`, `SelectWindow`,
`BringToFront`, `SendBehind`, `DrawGrowIcon`, `DragWindow`, `GrowWindow`,
`ZoomWindow`, `SetWTitle`, `GetWTitle`

---

## Dialog Manager

### Key Concepts

**`DialogRecord`**: extends `WindowRecord` with a `DITLHandle` (dialog item list), an
`editField` index, and a filter proc. Dialog items (`DITL` resource) include buttons,
check boxes, radio buttons, static text, editable text, icons, pictures, and user items.

**`itm.h`**: defines the internal dialog item layout — type byte and item rectangle.

**`ModalDialog`**: the blocking event loop that runs while a modal dialog is open. It
calls the application's filter proc on each event and checks for button hits.

**Alert dialogs**: `StopAlert`, `CautionAlert`, `NoteAlert`, and `PlainAlert` read
`ALRT` resources to determine dialog geometry and item layout before calling into the
dialog machinery.

### Source Files

`src/dial/dialCreate.cpp`, `dialHandle.cpp`, `dialItem.cpp`, `dialAlert.cpp`,
`dialDispatch.cpp`, `dialManip.cpp`, `dialInit.cpp`

### Key Traps

`NewDialog`, `DisposeDialog`, `ModalDialog`, `DialogSelect`, `GetDItem`, `SetDItem`,
`GetIText`, `SetIText`, `StopAlert`, `CautionAlert`, `NoteAlert`, `FindDItem`

---

## Menu Manager

### Key Concepts

**`MenuRecord`**: the in-memory representation of a menu. `LM(MenuList)` is the linked
list of all installed menus. The menu bar is drawn from this list by `DrawMenuBar`.

**MDEF** (Menu Definition Function): `stdmdef.cpp` and `stdmbdf.cpp` implement the
standard pull-down and menu bar DragRect procedures.

**Menu tracking**: `MenuSelect` runs the pull-down tracking loop, highlighting items and
returning the selected `(menuID, itemIndex)` packed into a `LONGINT`.

**Apple menu** (`apple.map`): the apple menu bitmap and Desk Accessory enumeration.

### Source Files

`src/menu/menu.cpp`, `menuColor.cpp`, `menuV.cpp`, `stdmdef.cpp`, `stdmbdf.cpp`

### Key Traps

`NewMenu`, `DisposeMenu`, `InsertMenu`, `DeleteMenu`, `DrawMenuBar`, `MenuSelect`,
`MenuKey`, `AppendMenu`, `AddResMenu`, `SetItem`, `GetItem`, `EnableItem`, `DisableItem`,
`CheckItem`, `HiliteMenu`

---

## Design Notes / Gotchas

- **`WINDCALL` macro**: always use the macro rather than calling `ROMlib_windcall`
  directly, so that future changes to the dispatch path are automatically applied.
- **`wmgr_port`**: defined as `guest_cast<GrafPtr>(LM(WMgrCPort))`. All Window Manager
  drawing must go into this port, not the application port.
- **Dialog item types**: item types `0x00`–`0x07` are the standard types; the
  `userItem` type (`0x00` with the control bit clear) requires the application to
  provide a custom draw proc. Executor does not pre-fill these.
- **`ROMlib_firstvisible(w)`**: skips invisible windows at the front of the window list
  to find the topmost visible window; used internally by `SelectWindow`.
- Menu color tables (`menuColor.cpp`) are only populated for Color QuickDraw
  environments. In monochrome mode they are ignored.
