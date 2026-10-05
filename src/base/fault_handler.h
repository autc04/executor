#pragma once

namespace Executor
{
    // Install a SIGSEGV/SIGBUS handler that recognises host faults caused by
    // emulated (guest) code dereferencing an out-of-range address.  Such a fault
    // is normally indistinguishable from a host crash; with this handler
    // installed, the faulting guest address (and, when syn68k_track_pc is set,
    // the guest PC of the faulting instruction) is reported instead.
    //
    // Enabled with `--debug segfault`.  Guest-PC tracking must be turned on
    // (syn68k_track_pc) before any guest code is translated.
    void installFaultHandler();
}
