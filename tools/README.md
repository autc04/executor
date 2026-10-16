# `executor-tool`

A host command-line utility for inspecting classic Mac files: list the
contents of HFS disk images, list and extract resources, and disassemble 68K
code. It is an ordinary host program (console/file I/O via C++), not a classic
Mac application.

It drives Executor's own File Manager / HFS + LocalVolume back ends and
Resource Manager, so it reads resources exactly the way Executor does; the
disassembler is cxmon's, so the output uses familiar MacOS syntax and A-line
trap numbers and low-memory globals are shown by name.

## Build

Part of the normal build (`cmake --build build`); the binary ends up at
`build/tools/executor-tool`. It needs a writable `~/.executor` (like Executor
itself) and, when run from the sandbox, a writable `$HOME`.

## Usage

```
executor-tool [--volume <image>]... [--verbose] <command> [args]
```

`<file>`/`<path>` arguments are either host paths or Mac paths
(`Volume:Folder:File`); host paths are reachable through the volume named `/`.
`<type>` is 4 characters (`PACK`) or `0xRRGGBBAA`; `<id>` is `-32768..65535`;
addresses are hex byte offsets.

| Command | What |
|---|---|
| `volumes` | list mounted volumes |
| `ls [<path-or-image>]` | list a directory (or all volumes). With a host *file*, mount it as a disk image and list the volume root |
| `types <file>` | list resource types and their counts |
| `res <file> [<type>]` | list resources (optionally of one type) |
| `dump <file> <type> <id> <out>` | write a resource's data to a host file |
| `dis <hostfile> [<start>] [<end>]` | disassemble a host file |
| `disres <file> <type> <id> [<start>] [<end>]` | disassemble a resource |

`--volume` mounts a disk image (repeatable); images/volumes named in the
`MacVolumes` environment variable are mounted too. `--verbose` keeps Executor's
own start-up chatter (otherwise it is silenced so it cannot corrupt output).

Disassembly names A-line traps (`a023` → `DisposeHandle`). `--lowmem` also
names low-memory globals in address operands (`ExpandMem`, …); it is **off** by
default because, when a blob is disassembled at offset 0, the same lookup would
mislabel branch targets that happen to land in 0x100–0x3000.

## Examples

```sh
# explore a disk image
executor-tool --volume ~/Emulators/System753.dsk ls 'System:'
executor-tool --volume ~/Emulators/System753.dsk ls 'System:System Folder'

# resources of a file (inside an image, or a host MacBinary/AppleDouble file)
executor-tool --volume ~/Emulators/System753.dsk types 'System:System Folder:System'
executor-tool --volume ~/Emulators/System753.dsk res   'System:System Folder:System' PACK
executor-tool --volume ~/Emulators/System753.dsk dump  'System:System Folder:System' PACK 8 out.bin

# disassemble (A-line traps and low-memory globals get names)
executor-tool dis out.bin 0x8c 0x120
executor-tool --volume ~/Emulators/System753.dsk disres 'System:System Folder:System' PACK 8 0 0x60
```
