# QuickDraw Graphics

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

QuickDraw is the Mac OS 2D graphics library. Executor reimplements it entirely in C++,
covering monochrome and colour QuickDraw, offscreen GWorlds, regions, polygons, arcs,
bitmaps, text rendering, colour palettes, and picture recording/playback. QuickDraw is
the largest single subsystem in the codebase.

## Key Concepts

**GrafPort / CGrafPort**: all drawing is done into a graphics port (`GrafPtr` / 
`CGrafPtr`). The current port is `LM(thePort)`. A colour port (`CGrafPort`) adds a
`PixMap` in place of the monochrome `BitMap`, plus a `ColorTable` and palette
information. The `cquick.h` header provides inline accessors that work with both port
types.

**BitMap / PixMap**: a `BitMap` is a contiguous rectangular array of pixels with a
`rowBytes` stride and a `bounds` rectangle. A `PixMap` extends this to colour depths
(1, 2, 4, 8, 16, 32 bpp). All pixel data lives in the guest address space as a
`GUEST<Ptr>`.

**Grafverb / transfer modes**: drawing operations are parameterised by a `GrafVerb`
(frame, paint, erase, invert, fill) and a transfer mode (`srcCopy`, `srcOr`, `srcXor`,
etc.). The inner-loop pixel blitters in `xdblt.cpp` and `srcblt.cpp` implement all
combinations.

**Regions**: a `Region` is a run-length encoded description of an arbitrary shape.
The region code (`qRegion.cpp`, `region.h`) uses a compact band representation. Regions
are fundamental to the Window Manager, clipping, and update tracking.

**Standard drawing routines**: each shape type (rect, oval, round-rect, arc, poly,
region, text, bits) has a "standard" implementation (`qStdRect.cpp`, etc.) and an
optional hook (`grafProcs` pointer in the port). When `grafProcs` is non-null, Executor
calls through the hook table, enabling applications to override drawing.

**Depth conversion**: `dcconvert.cpp` and `qPixMapConv.cpp` handle conversion between
different pixel depths when blitting to a port with a different depth than the source.

**Fonts**: `font.cpp` and `qText.cpp` implement font rendering. Executor ships a set of
bitmap fonts in the bundled System resource file. `fontIMVI.cpp` handles the Font
Manager IM VI interface.

**Picture recording**: `qPicture.cpp` and `qPicstuff.cpp` implement `OpenPicture` /
`ClosePicture` / `DrawPicture`. Pictures are stored as a stream of QuickDraw opcodes
in a relocatable handle.

## Source Files

| Path | Description |
|------|-------------|
| `src/quickdraw/qRect.cpp` | Rectangle drawing |
| `src/quickdraw/qOval.cpp` (via qStdOval) | Oval drawing |
| `src/quickdraw/qBit.cpp` | `CopyBits`, `CopyMask` |
| `src/quickdraw/qRegion.cpp` | Region operations: union, intersection, difference |
| `src/quickdraw/xdblt.cpp` / `xdblt.h` | Core pixel blitter (depth-independent) |
| `src/quickdraw/srcblt.cpp` | Source blitter for same-depth copies |
| `src/quickdraw/qColor.cpp` | Colour QuickDraw traps |
| `src/quickdraw/qGWorld.cpp` | Offscreen GWorld management |
| `src/quickdraw/font.cpp` | Font metrics and rendering |
| `src/quickdraw/qPicture.cpp` | Picture record / playback |
| `src/quickdraw/quick.h` | Internal prototypes, hook call macros |
| `src/quickdraw/cquick.h` | Port type detection, colour accessor macros |
| `src/quickdraw/image.cpp` | Pixel-map image utilities |

## Important Data Structures

- **`GrafPort` / `CGrafPort`** (Mac ABI structs, `GUEST_STRUCT`): port state including
  current pen, clip region, cursor position, and pixel buffer.
- **`BitMap` / `PixMap`**: pixel buffer descriptor; `PixMap` has `pmTable` (colour
  table handle) and `pixelSize` in addition to `BitMap` fields.
- **`Region`**: variable-length run-length-encoded region. `RGN_SMALL_SIZE` is the
  minimum (rectangular) size.
- **`xdata_t`** (`xdata.h`): intermediate representation used by the blitters to
  describe a blit operation; decouples the blitter from the caller.

## Key Functions / Traps

| Symbol | Description |
|--------|-------------|
| `C_CopyBits` | Blit between any two bitmaps/pixmaps |
| `C_PaintRect` / `C_FrameRect` | Rectangle fill / outline |
| `C_DrawText` / `C_TextWidth` | Text rendering and measurement |
| `C_OpenRgn` / `C_CloseRgn` | Region capture |
| `C_UnionRgn` / `C_SectRgn` / `C_DiffRgn` | Region boolean operations |
| `C_NewGWorld` / `C_DisposeGWorld` | Offscreen graphics world management |
| `C_OpenPicture` / `C_DrawPicture` | Picture record/playback |
| `ROMlib_bltrgn` | Internal region-masked blit |

## Design Notes / Gotchas

- **Blitter generation**: `src/quickdraw/makerawblt.pl` generates `rawblt.h`, which
  contains unrolled inner loops for common depth/mode combinations. Do not hand-edit
  this generated file.
- **Colour shim**: `REALMODE(x)` adds 16 to a transfer mode to indicate a colour
  (rather than pattern) blit; `WHITEMODE` and `BLACKMODE` are sentinels above the
  normal mode range.
- **Rootless mode**: when `Framebuffer::rootless` is true, QuickDraw windows are
  composited directly onto the host desktop. `windRootless.cpp` and
  `qGrafport.cpp` contain the integration code for this mode.
- Regions use a custom `RGN_STOP` sentinel (32767) to mark the end of a band. Code
  that walks region internals must respect this sentinel to avoid buffer overruns.
