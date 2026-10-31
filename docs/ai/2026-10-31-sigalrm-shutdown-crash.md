# SIGALRM at shutdown: crash in `pthread_kill`

Date: 2026-10-31

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Symptom

A Tracy-enabled build (`-DENABLE_TRACY=ON`) of a GUI front-end crashed while
exiting the emulator. The signal was delivered to a `Tracy ...` worker thread
(e.g. `Tracy Profiler` in `poll`, or `Tracy DXT1` in `clock_nanosleep`), and the
backtrace had this shape:

```
#0 __pthread_kill_implementation
#1 <signal handler called>
#2 poll / clock_nanosleep
```

The profiler had never been attached; whether `capture` is running makes no
difference.

Reproduction without a GUI (deterministic, exit 139 before the fix):

```
QT_QPA_PLATFORM=offscreen build/executor tests/build/tests.ad
```

`build/executor --headless tests/build/tests.ad` exited cleanly, which is why
the bug only showed up with the Qt/Wayland front-ends: the crash needs a timer
signal to fire in the window between the emulator thread's exit and process
exit, and GUI teardown makes that window long enough.

## Root cause

Two independent problems in the POSIX timer path (`src/time/syncint.cpp`):

1. **The signal goes to the wrong thread.** `setitimer(ITIMER_REAL)` produces a
   *process-directed* SIGALRM, so the kernel picks any thread that does not
   block it. `SyncintTimer::SyncintTimer()` blocks SIGALRM on the main thread,
   but it is an ordinary static initializer. Tracy's `s_profiler` has
   `init_priority(105)` and starts its worker threads *before* that, so those
   threads inherit an unblocked SIGALRM mask. Verified on a running process
   before the fix (`SigBlk`, `0x2000` = SIGALRM):

   ```
   executor           SigBlk=0000000000002000
   emulator           SigBlk=0000000000000000
   Tracy Profiler     SigBlk=0000000000000000
   Tracy Sampling     SigBlk=0000000000000000
   ```

   The SIGALRM handler therefore ran on a Tracy worker rather than on the
   emulator thread.

2. **`waitingThread` is a dangling handle after shutdown.** The handler calls
   `Interrupt::trigger()`, which forwards the interrupt with
   `pthread_kill(*waitingThread, SIGALRM)`. `waitingThread` is the emulator
   thread's `pthread_t`, it is never cleared, and the timer is never disarmed.
   Once the emulator thread has exited (after `endEventLoop()` /
   `executorThread.join()`), glibc has freed its thread descriptor, so
   `pthread_kill` dereferences unmapped memory.

Under gdb the fault was `SEGV_MAPERR` in
`__pthread_kill_implementation`'s `lock cmpxchg` on the target thread
descriptor. `waitingThread` was `0x7fffe4bfe6c0` and the fault address
`0x7fffe4bfefc4` — exactly `waitingThread + 0x904`, i.e. a field of the freed
`struct pthread`. `pthread_kill` does not validate a user-supplied handle
(that is undefined behaviour), hence a segmentation fault instead of `ESRCH`.

## Fix

- `syncint.cpp` now blocks SIGALRM in a static initializer with
  `init_priority(101)`, before Tracy's priority-105 `Profiler` constructor can
  create threads. Every thread created after that inherits the block; only
  `SyncintTimer::start()` unblocks it, on the emulator thread. The SIGALRM
  handler can therefore only run on the emulator thread, and this also removes
  the pre-existing race where `interrupt_generate()` /
  `getPowerCore().requestInterrupt()` ran on an arbitrary thread.
- `waitingThreadActive` (an atomic flag) is set before unblocking SIGALRM and
  cleared by the new `SyncintTimer::stop()`. The handler does nothing while the
  flag is false, so a signal that was already pending cannot touch emulator
  state or forward to a dead thread.
- `stop()` also disarms the itimer (`setitimer(ITIMER_REAL, {0})`). It is
  called through the new `Executor::syncint_shutdown()` at the end of the
  emulator thread, right after `vdriver->endEventLoop()` in `src/main.cpp`.

The early block is defence in depth against the mask inheritance; the explicit
shutdown makes the timer's lifetime end with the emulator thread. Relying on
`init_priority` is somewhat brittle, so do not move thread creation in static
initializers with a priority below 105. A more fundamental alternative would be
to use the existing `USE_TIMER_THREAD` implementation (condition variable plus
timer thread, already used on Windows) on POSIX as well; that removes the
signal-driven timer entirely but changes interrupt delivery timing and would
need its own validation.

## Validation

- `QT_QPA_PLATFORM=offscreen build/executor tests/build/tests.ad`: exit 139
  (3/3) before, exit 0 (5/5) after.
- `build/executor --headless tests/build/tests.ad`: exit 0.
- Signal masks after the fix: main thread and all Tracy threads have
  `SigBlk=0x2000`; only `emulator` has `0`.
- `ctest --test-dir build -LE xfail`: 262/262 pass.
