/* mactcp.cpp — native `.IPP` (MacTCP) driver for Executor 2000.
 *
 * Phase 1 skeleton: driver registration, csCode dispatch, stream
 * table, and synchronous TCP client operations over non-blocking
 * POSIX sockets.  Modeled on src/serial.cpp.
 *
 * Design notes:
 *   - Async model: like serial.cpp, operations currently execute
 *     synchronously and then "complete" (set ioResult, invoke the
 *     ioCompletion routine via callcomp if the async trap bit is
 *     set).  Phase 2 replaces the bodies with a pending-op queue
 *     drained from the accRun pump.
 *   - Pump: driver open sets NEEDTIMEBIT/dCtlDelay=0 on the DCE, which
 *     is what C_SystemTask() (desk.cpp) looks for when deciding whom to
 *     send accRun to.
 *
 *     BLOCKER FOR PHASE 2: that pump does not currently run at all.
 *     LM(UnitNtryCnt) is set to 0 in init.cpp and is never updated by
 *     anything, so C_SystemTask's loop body never executes and no
 *     driver has ever received accRun.  Raising it naively would then
 *     null-deref on the empty slots between installed units, so
 *     C_SystemTask and C_SystemMenu now skip empty slots.  Deciding
 *     how UnitNtryCnt should actually be maintained is a Device
 *     Manager question to settle with upstream before phase 2 depends
 *     on it; phase 1 is unaffected because it is fully synchronous.
 *   - Apps that spin-poll ioResult without calling WaitNextEvent will
 *     starve even once the pump works; a secondary pump point in the
 *     trap path may be needed.
 *   - StreamPtr: an opaque cookie (never a real pointer); apps are
 *     documented to treat it as opaque.
 *   - Byte order: guest is big-endian == network order; GUEST<>
 *     reads yield host-order values, so htonl/htons at the sockaddr
 *     boundary is both necessary and sufficient.
 */

#include <base/common.h>
#include <DeviceMgr.h>
#include <FileMgr.h>
#include <MemoryMgr.h>
#include <OSUtil.h>
#include <DeskMgr.h> /* accRun */
#include <MacTCP.h>  /* generated from defs/MacTCP.yaml */
#include <rsys/device.h>
#include <rsys/mactcp.h>
/* Required for REGISTER_FUNCTION_PTR below: it expands to an explicit
 * template instantiation of WrappedFunction<>, whose member definitions
 * live here.  Without it everything compiles and the link fails on
 * WrappedFunction<...>::init().  serial.cpp includes it for the same
 * reason. */
#include <base/traps.impl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h> /* writev, struct iovec */
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

using namespace Executor;

/* ---- ABI guard rails ------------------------------------------------
 * If the generated headers disagree with the documented MacTCP ABI,
 * fail the build rather than corrupt guest memory at run time.
 */
static_assert(offsetof(TCPiopb, ioCompletion) == 12, "TCPiopb ABI");
static_assert(offsetof(TCPiopb, ioResult) == 16, "TCPiopb ABI");
static_assert(offsetof(TCPiopb, ioCRefNum) == 24, "TCPiopb ABI");
static_assert(offsetof(TCPiopb, csCode) == 26, "TCPiopb ABI");
static_assert(offsetof(TCPiopb, tcpStream) == 28, "TCPiopb ABI");
static_assert(offsetof(TCPiopb, csParam) == 32, "TCPiopb ABI");
static_assert(offsetof(TCPOpenPB, remoteHost) == 4, "TCPOpenPB ABI");
static_assert(offsetof(TCPOpenPB, localHost) == 10, "TCPOpenPB ABI");
static_assert(offsetof(TCPSendPB, wdsPtr) == 6, "TCPSendPB ABI");
static_assert(offsetof(TCPReceivePB, rcvBuff) == 4, "TCPReceivePB ABI");
static_assert(offsetof(TCPReceivePB, markFlag) == 1, "TCPReceivePB ABI");
static_assert(offsetof(TCPReceivePB, urgentFlag) == 2, "TCPReceivePB ABI");
static_assert(sizeof(TCPStatusPB) == 70, "TCPStatusPB ABI");
static_assert(sizeof(TCPiopb) == 102, "TCPiopb ABI");
static_assert(offsetof(GetAddrParamBlock, ourAddress) == 28,
              "GetAddrParamBlock ABI");
static_assert(sizeof(wdsEntry) == 6, "wdsEntry ABI");

/* callcomp() (re-enter emulated code to run a completion routine) is
 * declared in rsys/device.h and defined in device.cpp. */

namespace
{

/* ---- Stream table -------------------------------------------------- */

enum class StreamState : uint8_t
{
    created,     /* TCPCreate done, no connection yet   */
    connecting,  /* reserved for phase 2 async open     */
    established,
    closing,     /* we sent FIN (TCPClose)              */
    terminated,  /* connection gone, stream not yet released */
};

struct MacTCPStream
{
    int fd = -1;
    StreamState state = StreamState::created;
    bool remoteClosed = false; /* peer FIN seen */

    /* App-supplied receive area (guest memory), from TCPCreate.
     * Phase 1 (TCPRcv only) copies straight into the caller's
     * buffer; phase 2's TCPNoCopyRcv stages data here and hands
     * out rdsEntry pointers into it. */
    Ptr rcvBuff = nullptr;
    uint32_t rcvBuffLen = 0;

    /* ASR + per-stream user data, from TCPCreate. */
    ProcPtr notifyProc = nullptr;
    Ptr userDataPtr = nullptr;

    /* TODO(phase2): pending async op queue; rds bookkeeping. */
};

/* StreamPtr cookies: 'TCP\0' | id.  Never dereferenced. */
constexpr uint32_t STREAM_COOKIE_BASE = 0x54435000;
uint32_t next_stream_id = 1;
std::unordered_map<uint32_t, MacTCPStream> streams;

MacTCPStream *lookup(GUEST<StreamPtr> sp, OSErr *err)
{
    auto it = streams.find(guest_cast<uint32_t>(sp));
    if(it == streams.end())
    {
        *err = invalidStreamPtr;
        return nullptr;
    }
    return &it->second;
}

/* ---- errno -> MacTCP OSErr ---------------------------------------- */

OSErr map_socket_errno(int e)
{
    switch(e)
    {
        case 0:
            return noErr;
        case ECONNREFUSED:
        case EHOSTUNREACH:
        case ENETUNREACH:
            return openFailed;
        case ETIMEDOUT:
            return commandTimeout;
        case ECONNRESET:
        case EPIPE:
            return connectionTerminated;
        case EADDRINUSE:
            return duplicateSocket;
        case ENOBUFS:
        case ENOMEM:
        case EMFILE:
        case ENFILE:
            return insufficientResources;
        default:
            return ipBadAddr; /* generic; refine as cases surface */
    }
}

/* ---- completion ---------------------------------------------------- */

OSErr complete(TCPiopb *pb, OSErr err)
{
    pb->ioResult = err;
    if((pb->ioTrap & asyncTrpBit) && pb->ioCompletion)
        callcomp(guest_cast<ParmBlkPtr>(pb), pb->ioCompletion, err);
    return err;
}

/* ---- ASR delivery (phase 2) ---------------------------------------
 * pascal void notifyProc(StreamPtr, u16 eventCode, Ptr userDataPtr,
 *                        u16 terminReason, ICMPReport *icmpMsg)
 *
 * TODO(phase2): push args right-to-left on the emulated stack
 * (EM_A7), push a magic return address, execute68K, restore A7.
 * Check whether Executor already has a generic pascal-call helper
 * before hand-rolling one here (the menu/control defproc callers
 * are the place to look).
 */
[[maybe_unused]] void call_asr(uint32_t cookie, MacTCPStream &s,
                               uint16_t event, uint16_t reason)
{
    if(!s.notifyProc)
        return;
    (void)cookie;
    (void)event;
    (void)reason;
    warning_unimplemented("MacTCP ASR delivery (event %d)", event);
}

/* ---- helpers ------------------------------------------------------- */

int set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? fl : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Wait for fd readiness with a MacTCP-style timeout (0 = default). */
int poll_one(int fd, short events, uint8_t timeout_secs,
             uint8_t default_secs)
{
    struct pollfd p = { fd, events, 0 };
    int secs = timeout_secs ? timeout_secs : default_secs;
    return poll(&p, 1, secs * 1000);
}

/* ---- csCode implementations (phase 1: synchronous) ---------------- */

OSErr do_getaddr(GetAddrParamBlock *pb)
{
    /* First non-loopback IPv4 interface.  TODO: make configurable. */
    struct ifaddrs *ifa0;
    OSErr err = ipBadCnfgErr;

    pb->ourAddress = 0;
    pb->ourNetMask = 0;
    if(getifaddrs(&ifa0) == 0)
    {
        for(struct ifaddrs *ifa = ifa0; ifa; ifa = ifa->ifa_next)
        {
            if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            auto *sin = (struct sockaddr_in *)ifa->ifa_addr;
            uint32_t a = ntohl(sin->sin_addr.s_addr);
            if((a >> 24) == 127)
                continue;
            pb->ourAddress = a;
            if(ifa->ifa_netmask)
                pb->ourNetMask = ntohl(
                    ((struct sockaddr_in *)ifa->ifa_netmask)
                        ->sin_addr.s_addr);
            err = noErr;
            break;
        }
        freeifaddrs(ifa0);
    }
    return err;
}

OSErr do_tcp_create(TCPiopb *pb)
{
    auto &create = pb->csParam.create;

    if(!create.rcvBuff || create.rcvBuffLen < 4096)
        return invalidBufPtr; /* MacTCP demanded >= 4K rcv buffer */

    uint32_t cookie = STREAM_COOKIE_BASE + next_stream_id++;
    MacTCPStream &s = streams[cookie];
    s.rcvBuff = create.rcvBuff;
    s.rcvBuffLen = create.rcvBuffLen;
    s.notifyProc = guest_cast<ProcPtr>(create.notifyProc);
    s.userDataPtr = create.userDataPtr;

    pb->tcpStream = guest_cast<StreamPtr>(cookie);
    return noErr;
}

OSErr do_tcp_active_open(TCPiopb *pb)
{
    OSErr err = noErr;
    MacTCPStream *s = lookup(pb->tcpStream, &err);
    if(!s)
        return err;
    if(s->fd >= 0)
        return connectionExists;

    auto &open = pb->csParam.open;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        return insufficientResources;
    set_nonblocking(fd);

    if(open.localPort)
    {
        struct sockaddr_in la = {};
        la.sin_family = AF_INET;
        la.sin_port = htons(open.localPort);
        la.sin_addr.s_addr = htonl(open.localHost);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if(bind(fd, (struct sockaddr *)&la, sizeof(la)) < 0)
        {
            err = map_socket_errno(errno);
            close(fd);
            return err;
        }
    }

    struct sockaddr_in ra = {};
    ra.sin_family = AF_INET;
    ra.sin_port = htons(open.remotePort);
    ra.sin_addr.s_addr = htonl(open.remoteHost);

    if(connect(fd, (struct sockaddr *)&ra, sizeof(ra)) < 0
       && errno != EINPROGRESS)
    {
        err = map_socket_errno(errno);
        close(fd);
        return err;
    }

    /* Phase 1: block right here until connected or ULP timeout.
     * Phase 2: return inProgress, finish from the accRun pump. */
    int r = poll_one(fd, POLLOUT, open.ulpTimeoutValue, 60);
    if(r <= 0)
    {
        close(fd);
        return r == 0 ? openFailed : map_socket_errno(errno);
    }
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if(soerr)
    {
        close(fd);
        return map_socket_errno(soerr);
    }

    /* Report the resolved local endpoint back, as MacTCP did. */
    struct sockaddr_in la = {};
    socklen_t lalen = sizeof(la);
    if(getsockname(fd, (struct sockaddr *)&la, &lalen) == 0)
    {
        open.localHost = ntohl(la.sin_addr.s_addr);
        open.localPort = ntohs(la.sin_port);
    }

    s->fd = fd;
    s->state = StreamState::established;
    return noErr;
}

OSErr do_tcp_send(TCPiopb *pb)
{
    OSErr err = noErr;
    MacTCPStream *s = lookup(pb->tcpStream, &err);
    if(!s)
        return err;
    if(s->fd < 0 || s->state != StreamState::established)
        return connectionDoesntExist;

    wdsEntry *w = guest_cast<wdsEntry *>(pb->csParam.send.wdsPtr);
    if(!w)
        return invalidWDS;

    /* Gather the WDS (zero-length entry terminates the list). */
    struct iovec iov[16];
    int n = 0;
    uint32_t total = 0;
    for(; w[n].length && n < 16; ++n)
    {
        iov[n].iov_base = (void *)guest_cast<Ptr>(w[n].ptr);
        iov[n].iov_len = w[n].length;
        total += w[n].length;
    }
    if(w[n].length)
        return invalidWDS; /* TODO: dynamically sized iovec */

    /* Phase 1: loop until fully written (sockets are non-blocking). */
    uint32_t written = 0;
    while(written < total)
    {
        ssize_t r = writev(s->fd, iov, n);
        if(r < 0)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if(poll_one(s->fd, POLLOUT,
                            pb->csParam.send.ulpTimeoutValue, 60)
                   <= 0)
                    return commandTimeout;
                continue;
            }
            return map_socket_errno(errno);
        }
        written += r;
        /* Advance iov past what was written. */
        while(r > 0 && n > 0)
        {
            if((size_t)r >= iov[0].iov_len)
            {
                r -= iov[0].iov_len;
                std::memmove(&iov[0], &iov[1],
                             sizeof(iov[0]) * --n);
            }
            else
            {
                iov[0].iov_base = (char *)iov[0].iov_base + r;
                iov[0].iov_len -= r;
                r = 0;
            }
        }
    }
    if(pb->csParam.send.pushFlag)
    {
        int one = 1;
        setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one,
                   sizeof(one));
    }
    return noErr;
}

OSErr do_tcp_rcv(TCPiopb *pb)
{
    OSErr err = noErr;
    MacTCPStream *s = lookup(pb->tcpStream, &err);
    if(!s)
        return err;
    if(s->fd < 0)
        return connectionDoesntExist;

    auto &rcv = pb->csParam.receive;
    Ptr buf = rcv.rcvBuff;
    uint16_t want = rcv.rcvBuffLen;
    if(!buf || !want)
        return invalidBufPtr;

    if(poll_one(s->fd, POLLIN, rcv.commandTimeoutValue, 60) <= 0)
        return commandTimeout;

    ssize_t r = read(s->fd, (void *)buf, want);
    if(r < 0)
        return map_socket_errno(errno);
    if(r == 0)
    {
        /* Peer FIN: report closing; further reads after drain
         * report connectionTerminated per MacTCP semantics. */
        s->remoteClosed = true;
        rcv.rcvBuffLen = 0;
        return connectionClosing;
    }
    rcv.rcvBuffLen = (uint16_t)r;
    rcv.urgentFlag = 0;
    rcv.markFlag = 0;
    return noErr;
}

OSErr do_tcp_close(TCPiopb *pb)
{
    OSErr err = noErr;
    MacTCPStream *s = lookup(pb->tcpStream, &err);
    if(!s)
        return err;
    if(s->fd < 0)
        return connectionDoesntExist;
    shutdown(s->fd, SHUT_WR); /* half-close; stream stays readable */
    s->state = StreamState::closing;
    return noErr;
}

OSErr do_tcp_abort(TCPiopb *pb)
{
    OSErr err = noErr;
    MacTCPStream *s = lookup(pb->tcpStream, &err);
    if(!s)
        return err;
    if(s->fd >= 0)
    {
        struct linger lg = { 1, 0 }; /* RST on close */
        setsockopt(s->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(s->fd);
        s->fd = -1;
    }
    s->state = StreamState::terminated;
    return noErr;
}

OSErr do_tcp_status(TCPiopb *pb)
{
    OSErr err = noErr;
    MacTCPStream *s = lookup(pb->tcpStream, &err);
    if(!s)
        return err;

    auto &st = pb->csParam.status;
    std::memset((void *)&st, 0, sizeof(st));

    switch(s->state)
    {
        case StreamState::established:
            st.connectionState = TCPSEstablished;
            break;
        case StreamState::closing:
            st.connectionState = s->remoteClosed ? TCPSClosing
                                                 : TCPSFinWait1;
            break;
        case StreamState::terminated:
        case StreamState::created:
        default:
            st.connectionState = TCPSClosed;
            break;
    }
    if(s->fd >= 0)
    {
        int avail = 0;
        if(ioctl(s->fd, FIONREAD, &avail) == 0)
            st.amtUnreadData = (uint16_t)std::min(avail, 0xffff);

        struct sockaddr_in a = {};
        socklen_t alen = sizeof(a);
        if(getpeername(s->fd, (struct sockaddr *)&a, &alen) == 0)
        {
            st.remoteHost = ntohl(a.sin_addr.s_addr);
            st.remotePort = ntohs(a.sin_port);
        }
        alen = sizeof(a);
        if(getsockname(s->fd, (struct sockaddr *)&a, &alen) == 0)
        {
            st.localHost = ntohl(a.sin_addr.s_addr);
            st.localPort = ntohs(a.sin_port);
        }
    }
    return noErr;
}

OSErr do_tcp_release(TCPiopb *pb)
{
    OSErr err = noErr;
    MacTCPStream *s = lookup(pb->tcpStream, &err);
    if(!s)
        return err;
    if(s->fd >= 0)
        close(s->fd);
    /* Hand the receive area back to the app, as MacTCP documented. */
    pb->csParam.create.rcvBuff = s->rcvBuff;
    pb->csParam.create.rcvBuffLen = s->rcvBuffLen;
    streams.erase(guest_cast<uint32_t>(pb->tcpStream));
    return noErr;
}

/* ---- accRun pump ---------------------------------------------------
 * Called every SystemTask via NEEDTIMEBIT (see driver open).
 * Phase 1: nothing to do — everything is synchronous.
 * Phase 2: poll(2) all live fds; complete pending async ops;
 * deliver TCPDataArrival / TCPClosing / TCPTerminate ASRs.
 */
void pump()
{
    /* TODO(phase2) */
}

} /* anonymous namespace */

/* ---- driver entry points ------------------------------------------
 * These live at file scope rather than in the anonymous namespace
 * above because REGISTER_FUNCTION_PTR expands to an explicit template
 * instantiation, which has to be at namespace scope.  Same shape as
 * serial.cpp: a C_-prefixed implementation plus a wrapper object of
 * the register-convention calling convention the Device Manager
 * dispatches through.
 */

static OSErr C_ROMlib_ippopen(ParmBlkPtr pbp, DCtlPtr dce);
REGISTER_FUNCTION_PTR(ROMlib_ippopen, D0(A0, A1));
static OSErr C_ROMlib_ippprime(ParmBlkPtr pbp, DCtlPtr dce);
REGISTER_FUNCTION_PTR(ROMlib_ippprime, D0(A0, A1));
static OSErr C_ROMlib_ippctl(ParmBlkPtr pbp, DCtlPtr dce);
REGISTER_FUNCTION_PTR(ROMlib_ippctl, D0(A0, A1));
static OSErr C_ROMlib_ippstatus(ParmBlkPtr pbp, DCtlPtr dce);
REGISTER_FUNCTION_PTR(ROMlib_ippstatus, D0(A0, A1));
static OSErr C_ROMlib_ippclose(ParmBlkPtr pbp, DCtlPtr dce);
REGISTER_FUNCTION_PTR(ROMlib_ippclose, D0(A0, A1));

static OSErr C_ROMlib_ippopen(ParmBlkPtr pbp, DCtlPtr dce)
{
    (void)pbp;
    /* Ask C_SystemTask to deliver accRun to us every pass. */
    dce->dCtlFlags |= NEEDTIMEBIT;
    dce->dCtlDelay = 0;
    return noErr;
}

static OSErr C_ROMlib_ippprime(ParmBlkPtr pbp, DCtlPtr dce)
{
    /* MacTCP has no Read/Write interface; everything is Control. */
    (void)pbp;
    (void)dce;
    return controlErr;
}

static OSErr C_ROMlib_ippctl(ParmBlkPtr pbp, DCtlPtr dce)
{
    (void)dce;
    TCPiopb *pb = (TCPiopb *)pbp;
    OSErr err;

    switch(pb->csCode)
    {
        case accRun:
            pump();
            return noErr; /* housekeeping: no completion semantics */

        case killCode:
            /* Phase 1 has no queued ops to kill. */
            return complete(pb, noErr);

        case ipctlGetAddr:
            err = do_getaddr((GetAddrParamBlock *)pbp);
            break;

        case TCPCreate:
            err = do_tcp_create(pb);
            break;
        case TCPActiveOpen:
            err = do_tcp_active_open(pb);
            break;
        case TCPSend:
            err = do_tcp_send(pb);
            break;
        case TCPRcv:
            err = do_tcp_rcv(pb);
            break;
        case TCPClose:
            err = do_tcp_close(pb);
            break;
        case TCPAbort:
            err = do_tcp_abort(pb);
            break;
        case TCPStatus:
            err = do_tcp_status(pb);
            break;
        case TCPRelease:
            err = do_tcp_release(pb);
            break;

        case TCPPassiveOpen:   /* phase 3 */
        case TCPNoCopyRcv:     /* phase 2 */
        case TCPRcvBfrReturn:  /* phase 2 */
        case udpCreate:        /* phase 3 ... */
        case udpRead:
        case udpBfrReturn:
        case udpWrite:
        case udpRelease:
        case udpMaxMTUSize:
        case udpStatus:
            warning_unimplemented("MacTCP csCode %d", (int)pb->csCode);
            err = invalidLength; /* TODO: most fitting stub error? */
            break;

        default:
            warning_unexpected("MacTCP unknown csCode %d",
                               (int)pb->csCode);
            err = controlErr;
            break;
    }
    return complete(pb, err);
}

static OSErr C_ROMlib_ippstatus(ParmBlkPtr pbp, DCtlPtr dce)
{
    /* MacTCP routes its status-ish calls (TCPStatus etc.) through
     * Control; a bare PBStatus on .IPP has nothing to report. */
    (void)dce;
    return complete((TCPiopb *)pbp, controlErr);
}

static OSErr C_ROMlib_ippclose(ParmBlkPtr pbp, DCtlPtr dce)
{
    (void)pbp;
    (void)dce;
    for(auto &kv : streams)
        if(kv.second.fd >= 0)
            close(kv.second.fd);
    streams.clear();
    return noErr;
}

/* ---- registration --------------------------------------------------
 * WIRE-UP: add `InitMacTCPDriver();` to InitBuiltinDrivers() in
 * device.cpp.
 *
 * Unit slot: 47 (refnum -48), the top of the 48-entry unit table —
 * far from .Sony (-5) and the serial units (-6..-9).  Real MacTCP
 * grabbed whatever free unit the Device Manager gave it, and no
 * application may depend on the number: they must PBOpen by name.
 */
void Executor::InitMacTCPDriver()
{
    RegisterDriver({
        &ROMlib_ippopen, &ROMlib_ippprime, &ROMlib_ippctl,
        &ROMlib_ippstatus, &ROMlib_ippclose,
        (StringPtr) "\04.IPP", -48,
    });
}
