#pragma once

#define RAMBASEDBIT (1 << 6)
#define DRIVEROPENBIT (1 << 5)

namespace Executor
{
struct driverinfo
{
    DriverUPP open;
    DriverUPP prime;
    DriverUPP ctl;
    DriverUPP status;
    DriverUPP close;
    StringPtr name;
    INTEGER refnum;
};

void RegisterDriver(const driverinfo& di);

/* Re-enter emulated code to run a driver call's completion routine.
 * A0 = param block, A1 = the routine, D0 = result -- the completion
 * routine ABI.  Any native driver implementing asynchronous calls
 * needs this, so it lives here rather than in one driver's .cpp. */
void callcomp(ParmBlkPtr pbp, ProcPtr comp, OSErr err);
}
