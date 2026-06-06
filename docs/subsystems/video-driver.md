# Video Driver & Screen Refresh

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

The video driver subsystem connects Executor's emulated framebuffer to a host display.
It defines an abstract `VideoDriver` interface, implements dirty-rect tracking to avoid
redrawing unchanged pixels, delivers host input events to the emulated keyboard and
mouse, and manages the screen refresh cycle. Platform-specific front-ends live under
`src/config/front-ends/` and subclass `VideoDriver`.

## Key Concepts

**`Framebuffer`**: a plain struct that owns the pixel buffer (`std::shared_ptr<uint8_t>`
for the pixel data) plus geometry (`width`, `height`, `bpp`, `rowBytes`) and metadata
(`grayscale`, `rootless`, `rgbSpec`). QuickDraw writes to this buffer via the low-
memory globals that point into it. The `Framebuffer` constructor allocates correctly-
sized storage.

**`VideoDriver`**: the abstract base class (declared in `vdriver.h`, not shown in full
above). Concrete subclasses implement:
- `setMode(w, h, bpp, grayscale)` — allocate or resize the framebuffer.
- `updateScreen(framebuffer, dirtyRects)` — copy dirty regions to the host window.
- `setCursor(data, mask, hotx, hoty)` — update the hardware cursor.
- `runEventLoop()` / `endEventLoop()` — platform event loop integration.
- `requestUpdate()` — called by the refresh timer to schedule a repaint.

**`DirtyRects`**: a small-capacity (`MAX_DIRTY_RECTS = 5`) fixed-size list of
rectangles that have been modified since the last `updateScreen`. The QuickDraw blitter
calls `dirty_rect_accrue(top, left, bottom, right)` whenever pixels are written.
`DirtyRects::getAndClear()` returns the accumulated rectangles and resets the list.
When more than 5 rects accumulate, they are coalesced into a single bounding rect to
avoid unbounded growth.

**`EventSink`**: implements `IEventListener` and translates host input events (mouse
clicks, key presses, suspend/resume) into entries on the Mac event queue. It uses an
internal mutex-protected work queue so that events posted from the host GUI thread are
picked up by `pumpEvents()` on the emulator thread.

**`IEventListener`**: interface consumed by each front-end. Calling
`mouseButtonEvent(down, h, v)` atomically calls `mouseMoved` then `mouseButtonEvent`,
ensuring the event position is always current.

**Refresh timer** (`src/vdriver/refresh.cpp`): a VBL-style periodic timer that fires at
the configured refresh rate (default ~60 Hz). On each tick it calls
`VideoDriver::requestUpdate()`, which schedules a `updateScreen` call on the GUI thread.

**Rootless mode**: when `Framebuffer::rootless == true`, emulated windows are not
drawn into the framebuffer and instead composited directly onto the host desktop by
the front-end. `windRootless.cpp` determines per-window compositing geometry.

## Source Files

| Path | Description |
|------|-------------|
| `src/vdriver/vdriver.h` | `Framebuffer`, `VideoDriver`, `IEventListener`, `EventSink` |
| `src/vdriver/vdriver.cpp` | Non-abstract VideoDriver helpers |
| `src/vdriver/dirtyrect.h` / `.cpp` | `DirtyRects` and `dirty_rect_accrue` |
| `src/vdriver/refresh.h` / `.cpp` | Refresh timer, `set_refresh_rate` |
| `src/vdriver/autorefresh.cpp` | Automatic screen refresh scheduling |
| `src/vdriver/eventsink.cpp` | `EventSink` implementation |
| `src/vdriver/eventrecorder.cpp` | Deterministic event recording/playback for tests |
| `src/config/front-ends/qt/qt.cpp` | Qt front-end (`QtVideoDriver`) |
| `src/config/front-ends/wayland/` | Wayland front-end |
| `src/config/front-ends/sdl2/` | SDL2 front-end |
| `src/config/front-ends/headless/` | Headless front-end (for CI/tests) |

## Important Data Structures

- **`Framebuffer`**: pixel buffer + geometry. Owned by `VideoDriver` and shared with
  the QuickDraw layer via low-memory globals that point into `Framebuffer::data`.
- **`DirtyRects::Rect`**: `{top, left, bottom, right}` in screen-pixel coordinates.
- **`vdriver_color_t`**: `{red, green, blue}` each 16-bit, for colour table entries
  passed to `updateScreen`.

## Key Functions / Methods

| Symbol | Description |
|--------|-------------|
| `dirty_rect_accrue(t,l,b,r)` | Record a modified rectangle |
| `DirtyRects::getAndClear()` | Retrieve and reset the dirty rect list |
| `EventSink::pumpEvents()` | Drain the pending-event queue on the emulator thread |
| `set_refresh_rate(hz)` | Change the screen refresh frequency |
| `QtVideoDriver::setMode()` | Resize the Qt window and reallocate the framebuffer |

## Design Notes / Gotchas

- **Thread safety**: `EventSink` is the only object in the video subsystem designed
  for cross-thread use. The `Framebuffer` pixel buffer is written exclusively on the
  emulator thread and read on the GUI thread; the dirty-rect list is the synchronisation
  mechanism between the two.
- **Minimum resolution**: `VDRIVER_MIN_SCREEN_WIDTH = 512`, `VDRIVER_MIN_SCREEN_HEIGHT = 342`
  (matching the original 512×342 Mac screen). Front-ends should reject smaller sizes.
- **`EventRecorder`**: records/replays raw input events for regression testing. When
  active it wraps `EventSink` and forwards events to the recorder before passing them
  on.
- Adding a new front-end requires: subclassing `VideoDriver`, implementing all pure
  virtual methods, and adding a CMake option in `src/config/front-ends/`.
