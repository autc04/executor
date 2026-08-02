/* Runtime tests for the native MacTCP (.IPP) driver.
 *
 * Native-only: these drive the driver through the real Device Manager
 * entry points (OpenDriver / PBControl) inside the test harness set up
 * by main_executor.cpp, so no 68k application and no emulator window
 * are involved.
 *
 * The TCP tests are hermetic -- the test process itself listens on a
 * loopback socket and the driver connects back to it, so nothing here
 * depends on the machine having a network, a route, or a name server.
 */

#include "gtest/gtest.h"

#include "compat.h"

#include <DeviceMgr.h>
#include <FileMgr.h>
#include <MacTCP.h>
#include <MemoryMgr.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

using namespace Executor;

namespace
{

/* Buffers reachable by the driver have to live in guest memory: the
 * driver turns the GUEST<Ptr> fields back into host pointers with
 * guest_cast, which is only meaningful for addresses inside the guest
 * address space.  NewPtr gives us that; the host stack would not. */
Ptr guestBuffer(uint32_t size)
{
    Ptr p = NewPtrClear(size);
    EXPECT_NE(nullptr, p);
    return p;
}

INTEGER openIPP()
{
    GUEST<INTEGER> refnum = 0;
    OSErr err = OpenDriver(PSTR(".IPP"), &refnum);
    EXPECT_EQ(noErr, err) << "OpenDriver(\".IPP\") failed";
    return refnum;
}

/* Issue one PBControl against .IPP.  The parameter block may live on
 * the host stack: it is never handed to emulated code, because these
 * are synchronous calls with no completion routine. */
OSErr control(INTEGER refnum, TCPiopb& pb, INTEGER csCode)
{
    pb.ioCRefNum = refnum;
    pb.csCode = csCode;
    return PBControl((ParmBlkPtr)&pb, false);
}

/* A host-side listening socket for the driver to connect to. */
class LoopbackListener
{
public:
    LoopbackListener()
    {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd_, 0);

        struct sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0; /* let the kernel pick */
        EXPECT_EQ(0, bind(fd_, (struct sockaddr *)&a, sizeof(a)));
        EXPECT_EQ(0, listen(fd_, 1));

        socklen_t len = sizeof(a);
        EXPECT_EQ(0, getsockname(fd_, (struct sockaddr *)&a, &len));
        port_ = ntohs(a.sin_port);
    }

    ~LoopbackListener()
    {
        if(accepted_ >= 0)
            close(accepted_);
        if(fd_ >= 0)
            close(fd_);
    }

    uint16_t port() const { return port_; }

    int accepted()
    {
        if(accepted_ < 0)
            accepted_ = accept(fd_, nullptr, nullptr);
        return accepted_;
    }

private:
    int fd_ = -1;
    int accepted_ = -1;
    uint16_t port_ = 0;
};

/* Open a stream and connect it to the listener.  Returns the refnum;
 * the stream cookie is left in pb.tcpStream. */
void createAndConnect(INTEGER refnum, TCPiopb& pb, Ptr rcvBuff,
                      uint32_t rcvBuffLen, uint16_t port)
{
    std::memset(&pb, 0, sizeof(pb));
    pb.csParam.create.rcvBuff = rcvBuff;
    pb.csParam.create.rcvBuffLen = rcvBuffLen;
    ASSERT_EQ(noErr, control(refnum, pb, TCPCreate));
    ASSERT_NE(nullptr, pb.tcpStream) << "TCPCreate returned a null stream";

    auto stream = pb.tcpStream;
    std::memset(&pb.csParam, 0, sizeof(pb.csParam));
    pb.tcpStream = stream;
    pb.csParam.open.remoteHost = 0x7F000001; /* 127.0.0.1, numeric */
    pb.csParam.open.remotePort = port;
    pb.csParam.open.ulpTimeoutValue = 5;
    ASSERT_EQ(noErr, control(refnum, pb, TCPActiveOpen));
}

} /* namespace */

/* ---- step 2: the driver is reachable at all ----------------------- */

TEST(MacTCP, OpenDriverByName)
{
    GUEST<INTEGER> refnum = 0;

    ASSERT_EQ(noErr, OpenDriver(PSTR(".IPP"), &refnum));
    /* Registered at refnum -48 == unit 47, the top of the unit table. */
    EXPECT_EQ(-48, (INTEGER)refnum);
    EXPECT_NE(nullptr, GetDCtlEntry(refnum));
}

TEST(MacTCP, OpenDriverIsCaseInsensitive)
{
    GUEST<INTEGER> refnum = 0;

    /* ROMlib_driveropen matches driver names case-insensitively. */
    ASSERT_EQ(noErr, OpenDriver(PSTR(".ipp"), &refnum));
    EXPECT_EQ(-48, (INTEGER)refnum);
}

TEST(MacTCP, GetAddrReportsAnInterface)
{
    INTEGER refnum = openIPP();

    GetAddrParamBlock pb;
    std::memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = refnum;
    pb.csCode = ipctlGetAddr;

    OSErr err = PBControl((ParmBlkPtr)&pb, false);

    /* A machine with no non-loopback IPv4 interface is a legitimate
     * configuration (CI containers, in particular), and the driver
     * reports that rather than inventing an address. */
    if(err == ipBadCnfgErr)
    {
        EXPECT_EQ(0u, (uint32_t)pb.ourAddress);
        GTEST_SKIP() << "no non-loopback IPv4 interface on this host";
    }

    ASSERT_EQ(noErr, err);
    uint32_t addr = pb.ourAddress;
    EXPECT_NE(0u, addr) << "noErr but no address filled in";
    EXPECT_NE(127u, addr >> 24) << "loopback should have been skipped";
}

TEST(MacTCP, UnknownCsCodeIsRejected)
{
    INTEGER refnum = openIPP();

    TCPiopb pb;
    std::memset(&pb, 0, sizeof(pb));
    EXPECT_EQ(controlErr, control(refnum, pb, 999));
}

/* ---- step 3: a hermetic loopback round trip ----------------------- */

TEST(MacTCP, CreateRejectsUndersizedBuffer)
{
    INTEGER refnum = openIPP();
    Ptr small = guestBuffer(1024);

    TCPiopb pb;
    std::memset(&pb, 0, sizeof(pb));
    pb.csParam.create.rcvBuff = small;
    pb.csParam.create.rcvBuffLen = 1024;

    /* MacTCP required a receive buffer of at least 4K. */
    EXPECT_EQ(invalidBufPtr, control(refnum, pb, TCPCreate));

    DisposePtr(small);
}

TEST(MacTCP, OperationsOnAnUnknownStreamFail)
{
    INTEGER refnum = openIPP();

    TCPiopb pb;
    std::memset(&pb, 0, sizeof(pb));

    /* A StreamPtr is an opaque cookie, not a real address, so it has to
     * be built with guest_cast the same way the driver builds it.
     * Assigning a fabricated host pointer instead would send
     * US_TO_SYN68K an address outside the guest space and abort. */
    pb.tcpStream = guest_cast<StreamPtr>(0xDEADBEEFu);

    EXPECT_EQ(invalidStreamPtr, control(refnum, pb, TCPStatus));
    EXPECT_EQ(invalidStreamPtr, control(refnum, pb, TCPClose));
    EXPECT_EQ(invalidStreamPtr, control(refnum, pb, TCPRelease));

    /* Zero is what a parameter block that was never filled in holds. */
    std::memset(&pb, 0, sizeof(pb));
    EXPECT_EQ(invalidStreamPtr, control(refnum, pb, TCPStatus));
}

TEST(MacTCP, LoopbackRoundTrip)
{
    INTEGER refnum = openIPP();
    LoopbackListener listener;

    const uint32_t kRcvBuffLen = 8192;
    Ptr rcvBuff = guestBuffer(kRcvBuffLen);

    TCPiopb pb;
    ASSERT_NO_FATAL_FAILURE(
        createAndConnect(refnum, pb, rcvBuff, kRcvBuffLen, listener.port()));
    auto stream = pb.tcpStream;

    int peer = listener.accepted();
    ASSERT_GE(peer, 0) << "driver never connected";

    /* TCPActiveOpen should have reported the local endpoint back. */
    EXPECT_NE(0u, (uint32_t)pb.csParam.open.localPort);

    /* --- send, exercising the WDS gather list ---------------------- */
    const char part1[] = "GET / ";
    const char part2[] = "HTTP/1.0\r\n\r\n";
    const char whole[] = "GET / HTTP/1.0\r\n\r\n";

    Ptr b1 = guestBuffer(sizeof(part1));
    Ptr b2 = guestBuffer(sizeof(part2));
    std::memcpy(b1, part1, sizeof(part1) - 1);
    std::memcpy(b2, part2, sizeof(part2) - 1);

    /* Three entries: two of data, then the zero-length terminator. */
    Ptr wdsMem = guestBuffer(sizeof(wdsEntry) * 3);
    auto *wds = (wdsEntry *)wdsMem;
    wds[0].length = sizeof(part1) - 1;
    wds[0].ptr = b1;
    wds[1].length = sizeof(part2) - 1;
    wds[1].ptr = b2;
    wds[2].length = 0;
    wds[2].ptr = nullptr;

    std::memset(&pb.csParam, 0, sizeof(pb.csParam));
    pb.tcpStream = stream;
    pb.csParam.send.wdsPtr = wdsMem;
    pb.csParam.send.pushFlag = 1;
    pb.csParam.send.ulpTimeoutValue = 5;
    ASSERT_EQ(noErr, control(refnum, pb, TCPSend));

    char got[64] = {};
    ssize_t n = recv(peer, got, sizeof(got), 0);
    ASSERT_EQ((ssize_t)(sizeof(whole) - 1), n)
        << "gathered send did not arrive whole";
    EXPECT_EQ(0, std::memcmp(got, whole, sizeof(whole) - 1));

    /* --- receive --------------------------------------------------- */
    const char reply[] = "HTTP/1.0 200 OK\r\n";
    ASSERT_EQ((ssize_t)(sizeof(reply) - 1),
              send(peer, reply, sizeof(reply) - 1, 0));

    Ptr appBuff = guestBuffer(256);
    std::memset(&pb.csParam, 0, sizeof(pb.csParam));
    pb.tcpStream = stream;
    pb.csParam.receive.rcvBuff = appBuff;
    pb.csParam.receive.rcvBuffLen = 256;
    pb.csParam.receive.commandTimeoutValue = 5;
    ASSERT_EQ(noErr, control(refnum, pb, TCPRcv));

    EXPECT_EQ(sizeof(reply) - 1, (size_t)(uint16_t)pb.csParam.receive.rcvBuffLen);
    EXPECT_EQ(0, std::memcmp(appBuff, reply, sizeof(reply) - 1));

    /* --- status ---------------------------------------------------- */
    std::memset(&pb.csParam, 0, sizeof(pb.csParam));
    pb.tcpStream = stream;
    ASSERT_EQ(noErr, control(refnum, pb, TCPStatus));
    EXPECT_EQ(TCPSEstablished, (int)pb.csParam.status.connectionState);
    EXPECT_EQ(0x7F000001u, (uint32_t)pb.csParam.status.remoteHost);
    EXPECT_EQ(listener.port(), (uint16_t)pb.csParam.status.remotePort);

    /* --- close ----------------------------------------------------- */
    std::memset(&pb.csParam, 0, sizeof(pb.csParam));
    pb.tcpStream = stream;
    ASSERT_EQ(noErr, control(refnum, pb, TCPClose));

    /* Our FIN should reach the peer as end-of-stream. */
    char drain[16];
    EXPECT_EQ(0, recv(peer, drain, sizeof(drain), 0));

    /* --- release --------------------------------------------------- */
    std::memset(&pb.csParam, 0, sizeof(pb.csParam));
    pb.tcpStream = stream;
    ASSERT_EQ(noErr, control(refnum, pb, TCPRelease));
    /* Release hands the receive area back to the application. */
    EXPECT_EQ(rcvBuff, (Ptr)pb.csParam.create.rcvBuff);
    EXPECT_EQ(kRcvBuffLen, (uint32_t)pb.csParam.create.rcvBuffLen);

    /* The stream is gone afterwards. */
    std::memset(&pb.csParam, 0, sizeof(pb.csParam));
    pb.tcpStream = stream;
    EXPECT_EQ(invalidStreamPtr, control(refnum, pb, TCPStatus));

    DisposePtr(appBuff);
    DisposePtr(wdsMem);
    DisposePtr(b2);
    DisposePtr(b1);
    DisposePtr(rcvBuff);
}

TEST(MacTCP, PeerCloseIsReportedAsConnectionClosing)
{
    INTEGER refnum = openIPP();
    LoopbackListener listener;

    const uint32_t kRcvBuffLen = 8192;
    Ptr rcvBuff = guestBuffer(kRcvBuffLen);

    TCPiopb pb;
    ASSERT_NO_FATAL_FAILURE(
        createAndConnect(refnum, pb, rcvBuff, kRcvBuffLen, listener.port()));
    auto stream = pb.tcpStream;

    int peer = listener.accepted();
    ASSERT_GE(peer, 0);

    /* Peer hangs up without sending anything. */
    shutdown(peer, SHUT_WR);

    Ptr appBuff = guestBuffer(256);
    std::memset(&pb.csParam, 0, sizeof(pb.csParam));
    pb.tcpStream = stream;
    pb.csParam.receive.rcvBuff = appBuff;
    pb.csParam.receive.rcvBuffLen = 256;
    pb.csParam.receive.commandTimeoutValue = 5;

    EXPECT_EQ(connectionClosing, control(refnum, pb, TCPRcv));
    EXPECT_EQ(0, (uint16_t)pb.csParam.receive.rcvBuffLen);

    std::memset(&pb.csParam, 0, sizeof(pb.csParam));
    pb.tcpStream = stream;
    EXPECT_EQ(noErr, control(refnum, pb, TCPAbort));

    DisposePtr(appBuff);
    DisposePtr(rcvBuff);
}
