---
name: extract-disassemble-mac68k
description: Use when you need to pull 68K Mac code or resources out of a classic-Mac disk image, resource fork, or MacBinary file and disassemble them — e.g. extracting a PACK/CODE/DRVR resource, exploring an HFS image, or decoding a raw 68K blob into mnemonic assembly.
---

# Extract & disassemble Mac 68K code

Use **`executor-tool`** (`tools/`, built as `build/tools/executor-tool`). It
drives Executor's own File Manager + Resource Manager and cxmon's
disassembler, so resources are read exactly the way Executor reads them and
disassembly uses familiar MacOS syntax with A-line trap numbers and
low-memory globals shown by name.

Build with the normal build step (`cmake --build build`, from inside the Nix
dev shell / `direnv exec .`). See `tools/README.md` for the full interface.

## Commands

`<file>`/`<path>` are host paths or Mac paths (`Volume:Folder:File`); the host
filesystem is the volume named `/`. `<type>` is 4 chars or `0xRRGGBBAA`;
addresses are hex.

```sh
# explore a disk image
executor-tool --volume image.dsk ls 'System:'
executor-tool --volume image.dsk ls 'System:System Folder'

# resources (works inside an image, or on a host MacBinary/AppleDouble file)
executor-tool --volume image.dsk types 'System:System Folder:System'
executor-tool --volume image.dsk res   'System:System Folder:System' PACK
executor-tool --volume image.dsk dump  'System:System Folder:System' PACK 8 out.bin

# disassemble a host file or a resource
executor-tool dis out.bin 0x8c 0x120
executor-tool --volume image.dsk disres 'System:System Folder:System' PACK 8 0 0x60
```

`--volume` mounts a disk image (repeatable; `MacVolumes` is also honoured).
`--verbose` keeps Executor's start-up chatter, which is otherwise silenced so
it can't corrupt parseable output.

## Notes

- `dump` output is the resource's exact bytes, so it can be diffed/hashed.
- Disassembly names A-line traps. `--lowmem` additionally names low-memory
  globals in operands; it is off by default because at offset 0 it also
  mislabels branch targets (see `tools/README.md`).
- `dis`/`disres` take optional hex start/end offsets; without them the whole
  blob is disassembled. Linear disassembly desyncs at embedded data — pass
  ranges to disassemble just the code.

