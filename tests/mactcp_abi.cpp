/* MacTCP ABI conformance: sizes and byte offsets of the parameter
 * blocks an application shares with the .IPP driver.
 *
 * This file is deliberately dual-mode, and the two modes answer two
 * *different* questions:
 *
 *   Native (-DEXECUTOR), against multiversal's generated MacTCP.h:
 *       does the header the generator produced actually lay out the
 *       way defs/MacTCP.yaml says it does?  The `size:` assertions in
 *       the YAML only pin total sizes, so a mac68k alignment mistake
 *       in the middle of a struct that happens to preserve the total
 *       can slip through.  These per-field offsets catch that.
 *
 *   Retro68, against Apple's Universal Interfaces MacTCP.h:
 *       are those numbers actually correct?  This is the ground truth.
 *
 * The expected values below have been confirmed field by field against
 * Universal Interfaces 3.4.2, by compiling Apple's MacTCP.h for a
 * 4-byte-pointer target with 2-byte packing (which is what mac68k
 * alignment amounts to for these types) and reading back the offsets
 * the compiler computed.  Two blocks did *not* match the MacTCP
 * Programmer's Guide, and both are called out where they appear below:
 * TCPReceivePB's field order, and TCPStatusPB's connStatPtr, whose
 * omission also made TCPiopb 98 bytes instead of 102.
 *
 * IMPORTANT: the Retro68 half only means something when it compiles
 * against *Apple's* headers.  Retro68 generates its CIncludes from the
 * same multiversal YAML that Executor uses, so if MacTCP.yaml is ever
 * added to Retro68's multiversal too, building this against those
 * generated CIncludes would compare our definitions to themselves and
 * prove nothing.  Genuine Universal Interfaces must come first on the
 * include path.
 *
 * Today that happens by default: Retro68's multiversal has no MacTCP
 * definitions at all, so <MacTCP.h> there can only be Apple's, which
 * arrives via Retro68's interfaces-and-libraries.sh from a copy of
 * Apple's Universal Interfaces you supply yourself.  Retro68's own
 * LaunchAPPL/Server/MacTCPStream.cc is a working MacTCP client built
 * that way, and every parameter block field it touches agrees with the
 * names used here.
 *
 * To re-confirm after a change:
 *
 *     ./tests --gtest_filter=MacTCPABI.DumpLayout   # native, ours
 *     ... same test built as the Retro68 application, run under
 *     Basilisk II or on real hardware via LaunchAPPL ...
 *     diff ours.txt apples.txt
 *
 * A compile failure on the Retro68 side is itself a result: it means a
 * field name in defs/MacTCP.yaml does not match Apple's.
 *
 * The expected values are written out here rather than derived from
 * the YAML, so that a transcription error in the YAML shows up as a
 * failure instead of being quietly mirrored.
 */

#include "gtest/gtest.h"

#include "compat.h"

/* Retro68 builds that have no MacTCP.h at all shouldn't fail to build;
 * they just can't answer the question. */
#if !defined(__has_include)
#define MACTCP_ABI_HAVE_HEADER 1
#elif __has_include(<MacTCP.h>)
#define MACTCP_ABI_HAVE_HEADER 1
#else
#define MACTCP_ABI_HAVE_HEADER 0
#endif

#if MACTCP_ABI_HAVE_HEADER

#include <MacTCP.h>

#include <cstddef>
#include <cstdio>

#ifdef EXECUTOR
using namespace Executor;
#endif

/* Absolute offsets inside TCPiopb.  Reaching the csParam members
 * through the enclosing block rather than through each sub-struct
 * keeps this independent of what the sub-structs are named, and
 * absolute offsets are what the ABI actually constrains.
 *
 * X(field-expression, expected-offset)
 */
#define MACTCP_IOPB_FIELDS(X)              \
    X(qLink, 0)                            \
    X(qType, 4)                            \
    X(ioTrap, 6)                           \
    X(ioCmdAddr, 8)                        \
    X(ioCompletion, 12)                    \
    X(ioResult, 16)                        \
    X(ioNamePtr, 18)                       \
    X(ioVRefNum, 22)                       \
    X(ioCRefNum, 24)                       \
    X(csCode, 26)                          \
    X(tcpStream, 28)                       \
    /* TCPCreate */                        \
    X(csParam.create.rcvBuff, 32)          \
    X(csParam.create.rcvBuffLen, 36)       \
    X(csParam.create.notifyProc, 40)       \
    X(csParam.create.userDataPtr, 44)      \
    /* TCPActiveOpen / TCPPassiveOpen */   \
    X(csParam.open.ulpTimeoutValue, 32)    \
    X(csParam.open.ulpTimeoutAction, 33)   \
    X(csParam.open.validityFlags, 34)      \
    X(csParam.open.commandTimeoutValue, 35)\
    X(csParam.open.remoteHost, 36)         \
    X(csParam.open.remotePort, 40)         \
    X(csParam.open.localHost, 42)          \
    X(csParam.open.localPort, 46)          \
    X(csParam.open.tosFlags, 48)           \
    X(csParam.open.precedence, 49)         \
    X(csParam.open.dontFrag, 50)           \
    X(csParam.open.timeToLive, 51)         \
    X(csParam.open.security, 52)           \
    X(csParam.open.optionCnt, 53)          \
    X(csParam.open.options, 54)            \
    X(csParam.open.userDataPtr, 94)        \
    /* TCPSend -- filler at 37, confirmed against Apple */ \
    X(csParam.send.ulpTimeoutValue, 32)    \
    X(csParam.send.ulpTimeoutAction, 33)   \
    X(csParam.send.validityFlags, 34)      \
    X(csParam.send.pushFlag, 35)           \
    X(csParam.send.urgentFlag, 36)         \
    X(csParam.send.wdsPtr, 38)             \
    X(csParam.send.sendFree, 42)           \
    X(csParam.send.sendLength, 46)         \
    X(csParam.send.userDataPtr, 48)        \
    /* TCPRcv / TCPNoCopyRcv / TCPRcvBfrReturn.  markFlag and         \
     * urgentFlag are at the front, not the tail: the Programmer's     \
     * Guide is wrong about this block and Apple's header says so. */  \
    X(csParam.receive.commandTimeoutValue, 32) \
    X(csParam.receive.markFlag, 33)        \
    X(csParam.receive.urgentFlag, 34)      \
    X(csParam.receive.rcvBuff, 36)         \
    X(csParam.receive.rcvBuffLen, 40)      \
    X(csParam.receive.rdsPtr, 42)          \
    X(csParam.receive.rdsLength, 46)       \
    X(csParam.receive.secondTimeStamp, 48) \
    X(csParam.receive.userDataPtr, 50)     \
    /* TCPClose */                         \
    X(csParam.close.ulpTimeoutValue, 32)   \
    X(csParam.close.ulpTimeoutAction, 33)  \
    X(csParam.close.validityFlags, 34)     \
    X(csParam.close.userDataPtr, 36)       \
    /* TCPAbort */                         \
    X(csParam.abort.userDataPtr, 32)       \
    /* TCPStatus.  connStatPtr at 94 is absent from the Guide's        \
     * parameter table; leaving it out shortens the block to 66 and     \
     * TCPiopb to 98. */                                                \
    X(csParam.status.ulpTimeoutValue, 32)  \
    X(csParam.status.ulpTimeoutAction, 33) \
    X(csParam.status.remoteHost, 38)       \
    X(csParam.status.remotePort, 42)       \
    X(csParam.status.localHost, 44)        \
    X(csParam.status.localPort, 48)        \
    X(csParam.status.tosFlags, 50)         \
    X(csParam.status.precedence, 51)       \
    X(csParam.status.connectionState, 52)  \
    X(csParam.status.sendWindow, 54)       \
    X(csParam.status.rcvWindow, 56)        \
    X(csParam.status.amtUnackedData, 58)   \
    X(csParam.status.amtUnreadData, 60)    \
    X(csParam.status.securityLevelPtr, 62) \
    X(csParam.status.sendUnacked, 66)      \
    X(csParam.status.sendNext, 70)         \
    X(csParam.status.congestionWindow, 74) \
    X(csParam.status.rcvNext, 78)          \
    X(csParam.status.srtt, 82)             \
    X(csParam.status.lastRTT, 86)          \
    X(csParam.status.sendMaxSegSize, 90)   \
    X(csParam.status.connStatPtr, 94)      \
    X(csParam.status.userDataPtr, 98)

#define MACTCP_GETADDR_FIELDS(X) \
    X(ourAddress, 28)            \
    X(ourNetMask, 32)

/* Compile-time is the strongest form: a mismatch stops the build
 * rather than waiting for someone to run the suite. */
#define ASSERT_IOPB_OFFSET(field, expected) \
    static_assert(offsetof(TCPiopb, field) == (expected), "TCPiopb::" #field);
MACTCP_IOPB_FIELDS(ASSERT_IOPB_OFFSET)
#undef ASSERT_IOPB_OFFSET

#define ASSERT_GETADDR_OFFSET(field, expected) \
    static_assert(offsetof(GetAddrParamBlock, field) == (expected), \
                  "GetAddrParamBlock::" #field);
MACTCP_GETADDR_FIELDS(ASSERT_GETADDR_OFFSET)
#undef ASSERT_GETADDR_OFFSET

static_assert(sizeof(TCPiopb) == 102, "TCPiopb size");
static_assert(sizeof(GetAddrParamBlock) == 36, "GetAddrParamBlock size");
static_assert(sizeof(wdsEntry) == 6, "wdsEntry size");
static_assert(sizeof(rdsEntry) == 6, "rdsEntry size");
static_assert(sizeof(ICMPReport) == 24, "ICMPReport size");

/* The same checks again at runtime.  Compile-time assertions stop at
 * the first failure and print no numbers; these report every
 * discrepancy with both values, which is what you want when diffing
 * our headers against Apple's. */
TEST(MacTCPABI, TCPiopbFieldOffsets)
{
#define CHECK_IOPB_OFFSET(field, expected) \
    EXPECT_EQ((size_t)(expected), offsetof(TCPiopb, field)) << "TCPiopb::" #field;
    MACTCP_IOPB_FIELDS(CHECK_IOPB_OFFSET)
#undef CHECK_IOPB_OFFSET
}

TEST(MacTCPABI, GetAddrParamBlockFieldOffsets)
{
#define CHECK_GETADDR_OFFSET(field, expected) \
    EXPECT_EQ((size_t)(expected), offsetof(GetAddrParamBlock, field)) \
        << "GetAddrParamBlock::" #field;
    MACTCP_GETADDR_FIELDS(CHECK_GETADDR_OFFSET)
#undef CHECK_GETADDR_OFFSET
}

TEST(MacTCPABI, StructSizes)
{
    EXPECT_EQ(102u, (unsigned)sizeof(TCPiopb));
    EXPECT_EQ(36u, (unsigned)sizeof(GetAddrParamBlock));
    EXPECT_EQ(6u, (unsigned)sizeof(wdsEntry));
    EXPECT_EQ(6u, (unsigned)sizeof(rdsEntry));
    EXPECT_EQ(24u, (unsigned)sizeof(ICMPReport));

    /* csParam covers its largest member, TCPStatusPB at 70 bytes,
     * which is what makes TCPiopb 102.  TCPOpenPB is 66. */
    EXPECT_EQ(70u, (unsigned)(sizeof(TCPiopb) - 32));
}

/* Result codes are as much a part of the ABI as the layouts: an
 * application switching on them cares about the exact values. */
TEST(MacTCPABI, ResultCodes)
{
    EXPECT_EQ(1, (int)inProgress);
    EXPECT_EQ(-23000, (int)ipBadLapErr);
    EXPECT_EQ(-23001, (int)ipBadCnfgErr);
    EXPECT_EQ(-23002, (int)ipNoCnfgErr);
    EXPECT_EQ(-23003, (int)ipLoadErr);
    EXPECT_EQ(-23004, (int)ipBadAddr);
    EXPECT_EQ(-23005, (int)connectionClosing);
    EXPECT_EQ(-23006, (int)invalidLength);
    EXPECT_EQ(-23007, (int)connectionExists);
    EXPECT_EQ(-23008, (int)connectionDoesntExist);
    EXPECT_EQ(-23009, (int)insufficientResources);
    EXPECT_EQ(-23010, (int)invalidStreamPtr);
    EXPECT_EQ(-23011, (int)streamAlreadyOpen);
    EXPECT_EQ(-23012, (int)connectionTerminated);
    EXPECT_EQ(-23013, (int)invalidBufPtr);
    EXPECT_EQ(-23014, (int)invalidRDS);
    EXPECT_EQ(-23014, (int)invalidWDS);
    EXPECT_EQ(-23015, (int)openFailed);
    EXPECT_EQ(-23016, (int)commandTimeout);
    EXPECT_EQ(-23017, (int)duplicateSocket);
    EXPECT_EQ(-23041, (int)nameSyntaxErr);
    EXPECT_EQ(-23042, (int)cacheFault);
    EXPECT_EQ(-23043, (int)noResultProc);
    EXPECT_EQ(-23044, (int)noNameServer);
    EXPECT_EQ(-23045, (int)authNameErr);
    EXPECT_EQ(-23046, (int)noAnsErr);
    EXPECT_EQ(-23047, (int)dnrErr);
}

TEST(MacTCPABI, CsCodes)
{
    EXPECT_EQ(15, (int)ipctlGetAddr);
    EXPECT_EQ(30, (int)TCPCreate);
    EXPECT_EQ(31, (int)TCPPassiveOpen);
    EXPECT_EQ(32, (int)TCPActiveOpen);
    EXPECT_EQ(34, (int)TCPSend);
    EXPECT_EQ(35, (int)TCPNoCopyRcv);
    EXPECT_EQ(36, (int)TCPRcvBfrReturn);
    EXPECT_EQ(37, (int)TCPRcv);
    EXPECT_EQ(38, (int)TCPClose);
    EXPECT_EQ(39, (int)TCPAbort);
    EXPECT_EQ(40, (int)TCPStatus);
    EXPECT_EQ(41, (int)TCPExtendedStat);
    EXPECT_EQ(42, (int)TCPRelease);
    EXPECT_EQ(43, (int)TCPGlobalInfo);
}

TEST(MacTCPABI, ConnectionStates)
{
    /* All even; the odd values were never used. */
    EXPECT_EQ(0, (int)TCPSClosed);
    EXPECT_EQ(2, (int)TCPSListen);
    EXPECT_EQ(4, (int)TCPSSynReceived);
    EXPECT_EQ(6, (int)TCPSSynSent);
    EXPECT_EQ(8, (int)TCPSEstablished);
    EXPECT_EQ(10, (int)TCPSFinWait1);
    EXPECT_EQ(12, (int)TCPSFinWait2);
    EXPECT_EQ(14, (int)TCPSCloseWait);
    EXPECT_EQ(16, (int)TCPSClosing);
    EXPECT_EQ(18, (int)TCPSLastAck);
    EXPECT_EQ(20, (int)TCPSTimeWait);
}

/* A diffable dump.  Run this on both sides -- natively, and as the
 * Retro68 application against Apple's headers -- and diff the two
 * transcripts.  Where the assertions above only say "wrong", this says
 * what the other side actually thinks the layout is.
 *
 *     ./tests --gtest_filter=MacTCPABI.DumpLayout > ours.txt
 */
TEST(MacTCPABI, DumpLayout)
{
    printf("sizeof(TCPiopb) = %u\n", (unsigned)sizeof(TCPiopb));
    printf("sizeof(GetAddrParamBlock) = %u\n",
           (unsigned)sizeof(GetAddrParamBlock));
    printf("sizeof(wdsEntry) = %u\n", (unsigned)sizeof(wdsEntry));
    printf("sizeof(rdsEntry) = %u\n", (unsigned)sizeof(rdsEntry));
    printf("sizeof(ICMPReport) = %u\n", (unsigned)sizeof(ICMPReport));

#define DUMP_IOPB_OFFSET(field, expected) \
    printf("TCPiopb.%-40s = %u\n", #field, (unsigned)offsetof(TCPiopb, field));
    MACTCP_IOPB_FIELDS(DUMP_IOPB_OFFSET)
#undef DUMP_IOPB_OFFSET

#define DUMP_GETADDR_OFFSET(field, expected) \
    printf("GetAddrParamBlock.%-31s = %u\n", #field, \
           (unsigned)offsetof(GetAddrParamBlock, field));
    MACTCP_GETADDR_FIELDS(DUMP_GETADDR_OFFSET)
#undef DUMP_GETADDR_OFFSET
}

#else /* !MACTCP_ABI_HAVE_HEADER */

TEST(MacTCPABI, DISABLED_NoMacTCPHeader)
{
}

#endif
