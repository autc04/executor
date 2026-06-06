# Sound Manager

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

The Sound Manager implements the classic Mac OS audio API: `SndNewChannel`, `SndPlay`,
`SndDoCommand`, `SndDoImmediate`, and the supporting sound format parsers. It drives a
platform-specific `SoundDriver` that feeds PCM audio to the host audio system.
An optional "fake" driver silently discards audio when no host sound device is
available.

## Key Concepts

**`SndChannel`**: the in-guest-memory structure representing one sound channel. Each
channel has a command queue (`queue[]`, length `qLength`), head and tail indices
(`qHead`, `qTail`), a callback proc pointer, and a set of modifier links for
sound-synthesis hardware simulation.

**Command queue model**: applications enqueue `SndCommand` records (command word +
two parameters). The synthesiser thread dequeues and executes commands. Queue-full
(`qfull_p`) and queue-empty (`qempty_p`) are tested before enqueue and dequeue
respectively. A zero-length queue is treated as empty to avoid division-by-zero.

**Synthesisers** (`snth5.cpp`): sound synthesiser unit 5 (sampled sound). Converts
Mac `SndListHandle` (sampled sound resources) into PCM samples for the driver.

**`SoundDriver`**: the abstract C++ interface to the host audio backend:
- `sound_init()` / `sound_shutdown()`: lifecycle.
- `HungerStart()` → `HungerInfo`: the driver fills in the time window and buffer
  pointer for which it needs samples. `HungerFinish()` signals that the buffer has
  been consumed.
- `sound_go()` / `sound_stop()`: start/stop the audio stream.
- `sound_clear_pending()`: discard buffered audio.

**`HungerInfo`**: the buffer request from the driver to the synthesiser:
- `t2`, `t3`: the time range (in samples) for which data is requested.
- `buf`: host pointer to the output buffer (or null if the samples should be computed
  but not stored).
- `bufsize`, `rate`: buffer byte count and sample rate.

**Fake driver** (`soundfake.cpp`): implements `SoundDriver` with no-ops. Used when the
host has no audio or when the user disables sound via `-nosound`. `sound_silent()`
returns `true` in this case.

**Linux driver** (`src/config/sound/linux/`): ALSA-based PCM output using the
hunger/finish model.

**`sound_disabled_p`**: global bool; set via the `-nosound` command-line flag. When
true, `SndNewChannel` and all playback traps return without doing anything.

## Source Files

| Path | Description |
|------|-------------|
| `src/sound/sound.cpp` | `SndNewChannel`, `SndPlay`, `SndDoCommand`, queue management |
| `src/sound/sounddriver.h` | `SoundDriver` abstract interface, `HungerInfo` |
| `src/sound/sounddriver.cpp` | `SoundDriver` vtable and `sound_driver` global |
| `src/sound/soundfake.cpp` | Silent no-op driver |
| `src/sound/snth5.cpp` | Sampled sound synthesiser (unit 5) |
| `src/sound/soundIMVI.cpp` | Sound Manager IM VI additions |
| `src/sound/soundopts.h` | `snd_time` typedef, sample rate types |
| `src/config/sound/linux/` | ALSA back-end |

## Important Data Structures

- **`SndChannel`** (Mac ABI, guest): `queue[]` of `SndCommand`; `qHead`, `qTail`,
  `qLength`; `userInfo` for application data; `callBack` proc pointer.
- **`SndCommand`** (Mac ABI, guest): `cmd` (command code), `param1`, `param2`.
- **`HungerInfo`** (host): `{t2, t3, buf, bufsize, rate}`.
- **`SoundDriver`** (abstract C++ class): one instance at `sound_driver`.

## Key Functions / Traps

| Symbol | Trap | Description |
|--------|------|-------------|
| `C_SndNewChannel(chanp, synth, init, proc)` | `_SndNewChannel` | Allocate a sound channel |
| `C_SndDisposeChannel(chan, quiet)` | `_SndDisposeChannel` | Release a channel |
| `C_SndDoCommand(chan, cmd, noWait)` | `_SndDoCommand` | Enqueue one sound command |
| `C_SndDoImmediate(chan, cmd)` | `_SndDoImmediate` | Execute a command immediately |
| `C_SndPlay(chan, sndHandle, async)` | `_SndPlay` | Play a `SndListHandle` |
| `qfull_p(chan)` / `qempty_p(chan)` | — | Queue state predicates |

## Design Notes / Gotchas

- **Queue overflow**: `SndDoCommand` with `noWait = false` should block until there is
  space in the channel queue. Executor's implementation may not fully honour the
  blocking semantics; applications that fill the queue rapidly may lose commands.
- **Zero-length queue guard**: `qempty_p` explicitly checks for `qLength <= 0` to
  avoid a modulo by zero in the dequeue path. This defensive check was added after a
  real compatibility problem with TetrisMax.
- **Callback proc**: the `callBack` field in `SndChannel` is a guest function pointer
  that may be called at interrupt time on real hardware. Executor calls it
  synchronously from the synthesiser rather than at interrupt level.
- **`sound_disabled_p`**: checked early in every Sound Manager trap. When true the
  function returns immediately with `noErr` rather than attempting any audio operation.
