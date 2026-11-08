#pragma once

#include <functional>

namespace Executor
{

/* Runs `f` on the emulator thread at the next interrupt safe point (see
 * Interrupt::handleCommon).
 *
 * Thread-safe; may be called from any thread once the emulator is running.
 * Work posted after the emulator thread has exited is never run.
 *
 * The task obeys interrupt-context rules: it may read guest memory, the
 * framebuffer and low-memory globals and may post events, but must not call
 * traps that can run guest code (FindWindow, TrackControl, ...) or change
 * qdGlobals().thePort.  See docs/ai/2026-11-06-agent-control-design.md. */
void runOnEmulatorThread(std::function<void()> f);

/* Records that the calling thread is the one that runs the emulator.  The
 * emulator thread calls this once before it starts.  runOnEmulatorThread() then
 * runs work inline when it is called from that thread: work queued there would
 * only be drained by the same thread at the next interrupt, which a caller that
 * is already on the emulator thread has no way to wait for. */
void noteEmulatorThread();

/* Pokes the emulator thread without scheduling work, for callers that only
 * need it to notice a state change. */
void wakeEmulatorThread();

}
