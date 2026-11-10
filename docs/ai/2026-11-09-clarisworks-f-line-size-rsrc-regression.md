# ClarisWorks 3.0 "unhandled trap 11 (FPU)" — SizeSrsrc regression

## Reproduction

In a word-processing document, select a line of text, make it bold and 18 point,
then click the centre-alignment button in the toolbar. The click kills the
application; the selection and the size change are needed — on an empty document
the same click is harmless.

## Symptom

After the last click the emulator dies with

```
init.cpp:123; fatal error in `unhandled_trap': unhandled trap 11 at pc 0x1221D2 (FPU)
```

(or, with different playback timing, trap 4 at pc 0x0). It looks like an FPU
problem, but the F-line vector is only the last step of a wild jump.

## What actually happens

* The faulting PC `0x001221D2` is **not code**. It lies in a *free* Memory
  Manager block that still holds stale QuickDraw-style dither patterns
  (`ffff 6666 ffff ...`); the word at the PC is `0xFFFF`, which is an F-line
  opcode. `0x0000`/garbage there explains the alternating trap 4/F-line
  reports.
* The executing code at `0x001216xx` is in ClarisWorks' decompressed `CODE 17`
  segment: a locked, purgeable resource handle whose data starts at
  `0x0011E0BC`. The block ends at `0x0012212C` in the middle of a function.
* `CODE 17`'s compression header declares an uncompressed size of `0x4224`,
  and the decompressor does call `SetHandleSize(h, 0x4224)` — but a second
  `SetHandleSize(h, 0x4070)` from guest `0x0092BCA` shrinks the same handle
  later, truncating the last `0x1B4` bytes of the segment.
* The switch dispatcher at `0x00121632` (selector 4 → case at `0x001216FA`)
  then executes `JSR d16(PC),0x1221CE` — a call into the truncated-away part.
  The target is now in freed memory, hence the wild jump.

## Root cause

`0x0092BCA` is ClarisWorks' segment loader. To size a segment handle it calls
`SizeSrsrc(GetResource('CODE', 42))` (trap `0xA9A5`). Commit `631a7b08`
("GetResourceSizeOnDisk: the size on disk, not the loaded size", 2026-10-21)
changed `C_GetResourceSizeOnDisk` from

```c++
return ROMlib_SizeResource(res, true);
```

to

```c++
return ROMlib_SizeResource(res, false);
```

`0xA9A5` is the classic `SizeRsrc`/`SizeResource` trap (see
`docs/outdated/universal_header_traps:653`).

ClarisWorks handles the unloaded case itself (its patched
`GetResourceSizeOnDisk` returns the uncompressed size from a private table and
falls back to the original trap whenever `*h != 0`), and relies on the system
trap for the loaded case. `CODE 42` was already loaded (and decompressed to
`0x6632` bytes) at this point. With the changed implementation Executor
returned the on-disk (compressed) size `0x4070`, so ClarisWorks resized the
`CODE 17` handle to `0x4070` instead of `0x6632` and cut the segment short.

## What the trap is required to return

`Inside Macintosh I`, chapter 5 ("The Resource Manager"), p. 1-121:

> Given a handle to a resource, SizeResource returns the size in bytes of the
> resource in the resource file. If the given handle isn't a handle to a
> resource, SizeResource will return -1 and the ResError function will return
> the result code resNotFound. It's a good idea to call SizeResource and
> ensure that sufficient space is available before reading a resource into
> memory.

`Inside Macintosh: More Macintosh Toolbox`, chapter 1, p. 1-104 and 1-105 —
the same trap under its later name:

> The GetResourceSizeOnDisk and GetMaxResourceSize functions get the exact size
> and maximum size, respectively, of a resource.
>
> Given a handle to a resource, the GetResourceSizeOnDisk function checks the
> resource on disk (not in memory) and returns its exact size, in bytes. […]
> You can call GetResourceSizeOnDisk before reading a resource into memory to
> make sure there's enough memory available to do so successfully.

and p. 1-106, the contrast with `GetMaxResourceSize` (`0xA821`):

> GetMaxResourceSize does not check the resource on disk; instead, it either
> checks the resource size in memory or, if the resource is not in memory,
> calculates its size, in bytes, on the basis of information in the resource
> map in memory. This gives you an approximate size for the resource that you
> can count on as the resource's maximum size. It's possible that the resource
> is actually smaller than the offsets in the resource map indicate because the
> file has not yet been compacted.

"On disk (not in memory)" says where the number comes from when the resource is
*not* in memory; it is not a licence to report the physical footprint of the
storage container. The routine promises the **exact** size of the resource, in
contrast to `GetMaxResourceSize`'s *approximate maximum* derived from map
offsets, and the documented use case — sizing an allocation *before* loading —
only makes sense if the answer is the size the resource has once it exists.
Hence:

| resource state | required result |
|---|---|
| handle is not a resource handle | `-1`, `ResError` = `resNotFound` |
| loaded (`*h != 0`) | `GetHandleSize(h)` |
| not loaded, `resCompressed` set | uncompressed size from the compression header in the fork |
| not loaded, plain | the length word in the fork at `rdatoff + doff` |

Both of the cases the `false` implementation gets wrong follow from that:

* **Compressed resources.** The fork holds
  `[length][0xA89F6572][0x120801][uncompressedSize][payload]`, so the length
  word is the container — 16 bytes plus the compressed payload, which is not
  "the size in bytes of the resource". `ROMlib_SizeResource`'s compression
  branch is gated on `usehandle`, so with `false` it is dead code and every
  compressed resource reports the container size. IM:I p. 1-111 shows the
  `resCompressed` attribute as the bit "reserved for use by the Resource
  Manager": the map says that the stored bytes are an encoding, so that this
  query can be answered without decompressing. Reporting the encoded length
  would also make the documented idiom — call the trap before loading to check
  there is memory for the resource — under-allocate.
* **Resources changed in memory.** IM:I p. 1-123 states that the modifying
  routines "change only the resource map in memory", and p. 1-126 that an
  enlarged resource is written at the end of the file with the space occupied
  by the original compacted later; MMT p. 1-106 adds that the file may not have
  been compacted at all. The fork's length word is therefore stale for a
  loaded, changed resource, and meaningless for one that was added and never
  written (`doff == 0xFFFFFF`, no on-disk location at all). The handle is the
  only place where an exact size still exists.

`ROMlib_SizeResource(res, false)` stays correct for its two internal callers,
`C_ChangedResource` and `C_ReadPartialResource`, which genuinely want the raw
stored length. The correction belongs at the trap, not in the helper.

## Resolution

`631a7b08` reverted on 2026-11-10: `C_GetResourceSizeOnDisk` calls
`ROMlib_SizeResource(res, true)` again.

## Open question: the Finder 7.5 rationale

`true` and `false` differ in exactly two situations — loaded handles, and
unloaded *compressed* resources — so the Finder problem that prompted the
commit was necessarily one of them, and the commit's premise must be
re-examined.
With `true`, an unloaded compressed resource returns the uncompressed
size. If the Finder was comparing against the stored length, that case will
resurface, and the fix belongs in the compressed branch (or in the Finder's own
header read), not in reverting the whole trap to `false`.
