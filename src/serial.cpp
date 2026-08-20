/*
 * Copyright 1989, 1990, 1995, 1997 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/*
 * This file is an ugly mess of UNIX specific code portable Mac-side code.
 * The UNIX specific stuff should be put in its appropriate place sometime.
 */

#include <base/common.h>

#if defined(__APPLE__)
// FIXME: #warning bad serial support right now
// TODO: macOS is not in the USE_TERMIOS set below, so it still takes the
// BSD sgtty path.  Linux and alpha were moved to POSIX termios; macOS has
// perfectly good termios too, so the sgtty branch could most likely be
// deleted outright -- it just needs testing on a Mac.
#include <sgtty.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <stdio.h>
#endif /* defined (__APPLE__) */

#include <Serial.h>
#include <DeviceMgr.h>
#include <FileMgr.h>
#include <MemoryMgr.h>
#include <OSUtil.h>

#include <file/file.h>
#include <hfs/hfs.h>
#include <rsys/serial.h>
#include <rsys/device.h>
#include <base/cpu.h>
#include <base/traps.impl.h>

#if defined(_WIN32)
#include "win_serial.h"
#else
#include <rsys/unixio.h>
#endif

#if !defined(_WIN32)
#include <termios.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <sys/ioctl.h>
#endif

using namespace Executor;

/* Use the POSIX termios API rather than the ancient BSD sgtty one.
 * This used to be spelled TERMIO and used the System V <termio.h>
 * interface (struct termio, TCGETA/TCSETAW/TCSBRK/TCXONC), which glibc
 * dropped in 2.42.
 *
 * Everything guarded by USE_TERMIOS is plain POSIX.1-2001; the platform
 * list is narrow only because the sgtty fallback has never been retested,
 * not because anything here is Linux-specific.  See the __APPLE__ block
 * at the top of the file. */
#if defined(__alpha) || defined(__linux__)
#define USE_TERMIOS
#endif

/*
 * TODO:
 *
 * Post events on break action or hardware handshake changes IMII-247
 * Support KillIO from the device manager IMII-248
 * Hardware handshaking on machines that support it IMII-252
 *
 * Finish up the get status routine?
 */

#define AINNAME "\004.AIn"
#define AOUTNAME "\005.AOut"
#define BINNAME "\004.BIn"
#define BOUTNAME "\005.BOut"
#define AINREFNUM (-6)
#define AOUTREFNUM (-7)
#define BINREFNUM (-8)
#define BOUTREFNUM (-9)

#if !defined(XONC)
#define XONC '\21'
#define XOFFC '\23'
#endif

#define SER_START (1)
#define SER_STOP (0)


static OSErr C_ROMlib_serialopen(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(ROMlib_serialopen, D0(A0,A1));
static OSErr C_ROMlib_serialprime(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(ROMlib_serialprime, D0(A0,A1));
static OSErr C_ROMlib_serialctl(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(ROMlib_serialctl, D0(A0,A1));
static OSErr C_ROMlib_serialstatus(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(ROMlib_serialstatus, D0(A0,A1));
static OSErr C_ROMlib_serialclose(ParmBlkPtr pbp, DCtlPtr dcp);
REGISTER_FUNCTION_PTR(ROMlib_serialclose, D0(A0,A1));

OSErr Executor::RAMSDOpen(SPortSel port) /* IMII-249 */
{
    OSErr err;
    GUEST<INTEGER> rn;

    switch(port)
    {
        case sPortA:
            if((err = OpenDriver((StringPtr)AINNAME, &rn)) == noErr)
                err = OpenDriver((StringPtr)AOUTNAME, &rn);
            break;
        case sPortB:
            if((err = OpenDriver((StringPtr)BINNAME, &rn)) == noErr)
                err = OpenDriver((StringPtr)BOUTNAME, &rn);
            break;
        default:
            err = openErr;
            break;
    }
    return err;
}

void Executor::RAMSDClose(SPortSel port) /* IMII-250 */
{
    switch(port)
    {
        case sPortA:
            (void)CloseDriver(AINREFNUM);
            (void)CloseDriver(AOUTREFNUM);
            break;
        case sPortB:
            (void)CloseDriver(BINREFNUM);
            (void)CloseDriver(BOUTREFNUM);
            break;
    }
}

OSErr Executor::SerReset(INTEGER rn, INTEGER config) /* IMII-250 */
{
    GUEST<INTEGER> config_s = config;

    return Control(rn, kSERDConfiguration, (Ptr)&config_s);
}

OSErr Executor::SerSetBuf(INTEGER rn, Ptr p, INTEGER len) /* IMII-251 */
{
    sersetbuf_t temp;

    temp.p = p;
    temp.i = len;

    return Control(rn, kSERDInputBuffer, (Ptr)&temp);
}

OSErr Executor::SerHShake(INTEGER rn, const SerShk *flags) /* IMII-251 */
{
    return Control(rn, kSERDSerHShake, (Ptr)flags);
}

OSErr Executor::SerSetBrk(INTEGER rn) /* IMII-252 */
{
    return Control(rn, kSERDSetBreak, (Ptr)0);
}

OSErr Executor::SerClrBrk(INTEGER rn) /* IMII-253 */
{
    return Control(rn, kSERDClearBreak, (Ptr)0);
}

OSErr Executor::SerGetBuf(INTEGER rn, GUEST<LONGINT> *lp) /* IMII-253 */
{
    INTEGER status[11];
    OSErr err;

    if((err = Status(rn, kSERDInputCount, (Ptr)status)) == noErr)
        *lp = *(GUEST<LONGINT> *)status;
    return err;
}

OSErr Executor::SerStatus(INTEGER rn, SerStaRec *serstap) /* IMII-253 */
{
    INTEGER status[11];
    OSErr err;

    if((err = Status(rn, kSERDStatus, (Ptr)status)) == noErr)
        BlockMoveData((Ptr)status, (Ptr)serstap, (Size)sizeof(*serstap));
    return err;
}

#define OPENBIT (1 << 0)

#if !defined(_WIN32)
#if defined(USE_TERMIOS)

typedef struct
{
    LONGINT fd; /* will be the same for both In and Out */
    struct termios state;
} hidden, *hiddenp;

#else

typedef struct
{
    LONGINT fd; /* will be the same for both In and Out */
    struct sgttyb sgttyb;
    struct tchars tchars;
    ULONGINT lclmode;
} hidden, *hiddenp;

#endif
#else

typedef struct
{
    LONGINT fd; /* probably need to save more state information */
    /* right now we just use fd to keep track of which port we're talking to */
} hidden, *hiddenp;

#endif

typedef GUEST<hiddenp> *hiddenh;

static DCtlPtr otherdctl(ParmBlkPtr);
static OSErr serset(LONGINT fd, INTEGER param);
static OSErr serxhshake(LONGINT fd, SerShk *sershkp);
static OSErr setbaud(LONGINT fd, INTEGER baud);
static OSErr flow(LONGINT fd, LONGINT flag);
static OSErr ctlbrk(LONGINT fd, INTEGER flag);

static DCtlPtr otherdctl(ParmBlkPtr pbp)
{
    DCtlHandle h;

    h = 0;
    switch(pbp->cntrlParam.ioCRefNum)
    {
        case AINREFNUM:
            h = GetDCtlEntry(AOUTREFNUM);
            break;
        case AOUTREFNUM:
            h = GetDCtlEntry(AINREFNUM);
            break;
        case BINREFNUM:
            h = GetDCtlEntry(BOUTREFNUM);
            break;
        case BOUTREFNUM:
            h = GetDCtlEntry(BINREFNUM);
            break;
    }
    return h ? *h : 0;
}

#if defined(__linux__) || defined(__APPLE__)

/*
 * NOTE:  Currently we're using cufa and cufb; we really should
 *	  delay the opening until we know just what type of flow
 *	  control the user wants to do.
 */
static const char *specialname(ParmBlkPtr pbp)
{
    const char *retval;
    retval = 0;

    switch(pbp->cntrlParam.ioCRefNum)
    {
        case AINREFNUM:
        case AOUTREFNUM:
            retval = "/dev/ttyUSB1";
            break;
        case BINREFNUM:
        case BOUTREFNUM:
            retval = "/dev/ttyUSB2";
            break;
    }
    return retval;
}
#endif /* defined (__linux__) || defined (__APPLE__) */

typedef void (*compfuncp)(void);

/* callcomp() now lives in device.cpp and is declared in rsys/device.h,
 * so that other native drivers can complete asynchronous calls too. */

#define DOCOMPLETION(pbp, err)                                                \
    (pbp)->ioParam.ioResult = err;                                           \
    if(((pbp)->ioParam.ioTrap & asyncTrpBit) && (pbp)->ioParam.ioCompletion) \
        callcomp(pbp, (pbp)->ioParam.ioCompletion, err);                     \
    return err


#define SERIALDEBUG


int ROMlib_lasterrnomapped;

#define MAX_ERRNO 50

#define install_errno(uerr, merr)         \
    do                                    \
    {                                     \
        gui_assert(uerr < std::size(xtable)); \
        xtable[uerr] = merr;              \
    } while(false);

OSErr Executor::ROMlib_maperrno() /* INTERNAL */
{
    OSErr retval;
    static OSErr xtable[MAX_ERRNO + 1];
    static char been_here = false;
    int errno_save;

    if(!been_here)
    {
        int i;

        for(i = 0; i < (int)std::size(xtable); ++i)
            xtable[i] = fsDSIntErr;

        install_errno(0, noErr);
        install_errno(EPERM, permErr);
        install_errno(ENOENT, fnfErr);
        install_errno(EIO, ioErr);
        install_errno(ENXIO, paramErr);
        install_errno(EBADF, fnOpnErr);
        install_errno(EAGAIN, fLckdErr);
        install_errno(ENOMEM, memFullErr);
        install_errno(EACCES, permErr);
        install_errno(EFAULT, paramErr);
        install_errno(EBUSY, fBsyErr);
        install_errno(EEXIST, dupFNErr);
        install_errno(EXDEV, fsRnErr);
        install_errno(ENODEV, nsvErr);
        install_errno(ENOTDIR, dirNFErr);
        install_errno(EINVAL, paramErr);
        install_errno(ENFILE, tmfoErr);
        install_errno(EMFILE, tmfoErr);
        install_errno(EFBIG, dskFulErr);
        install_errno(ENOSPC, dskFulErr);
        install_errno(ESPIPE, posErr);
        install_errno(EROFS, wPrErr);
        install_errno(EMLINK, dirFulErr);
#if !defined(_WIN32)
        install_errno(ETXTBSY, fBsyErr);
        install_errno(EWOULDBLOCK, permErr);
#endif

        been_here = true;
    }

    errno_save = errno;
    ROMlib_lasterrnomapped = errno_save;

    if(errno_save < 0 || errno_save >= (int)std::size(xtable))
        retval = fsDSIntErr;
    else
        retval = xtable[errno_save];

    if(retval == fsDSIntErr)
        warning_unexpected("fsDSIntErr errno = %d", errno_save);

    if(retval == dirNFErr)
        warning_trace_info("dirNFErr errno = %d", errno_save);

    fs_err_hook(retval);
    return retval;
}


static OSErr C_ROMlib_serialopen(ParmBlkPtr pbp, DCtlPtr dcp) /* INTERNAL */
{
    OSErr err;
    DCtlPtr otherp;
    hiddenh h;
#if defined(__linux__) || defined(__APPLE__)
    const char *devname;
#endif

    err = noErr;
    if(!(dcp->dCtlFlags & OPENBIT))
    {
        h = (hiddenh)NewHandle(sizeof(hidden));
        dcp->dCtlStorage = (Handle)h;
        otherp = otherdctl(pbp);
        if(otherp && (otherp->dCtlFlags & OPENBIT))
        {
            **h = **(hiddenh)otherp->dCtlStorage;
            dcp->dCtlFlags |= OPENBIT;
        }
        else
        {
#if defined(__linux__) || defined(__APPLE__)
            err = permErr;
            if((devname = specialname(pbp)))
            {
                err = noErr;
            }
#endif
            if(err == noErr)
            {
#if defined(__linux__) || defined(__APPLE__)
                (*h)->fd = open(devname, O_RDWR, 0);
                if((*h)->fd < 0)
                    err = ROMlib_maperrno();
                else
                {
#if defined(USE_TERMIOS)
                    err = tcgetattr((*h)->fd, &(*h)->state) < 0 ? ROMlib_maperrno() : noErr;
#else
                    if(ioctl((*h)->fd, TIOCGETP, &(*h)->sgttyb) < 0 || ioctl((*h)->fd, TIOCGETC, &(*h)->tchars) < 0 || ioctl((*h)->fd, TIOCLGET, &(*h)->lclmode) < 0)
                        err = ROMlib_maperrno();
#endif
#else
                (*h)->fd = (pbp->cntrlParam.ioCRefNum == AINREFNUM || pbp->cntrlParam.ioCRefNum == AOUTREFNUM) ? 0 : 1;
#endif
                    dcp->dCtlFlags |= OPENBIT;
                    SerReset(pbp->cntrlParam.ioCRefNum,
                             (pbp->cntrlParam.ioCRefNum == AINREFNUM || pbp->cntrlParam.ioCRefNum == AOUTREFNUM) ? LM(SPPortA) : LM(SPPortB));
#if defined(__linux__) || defined(__APPLE__)
                }
#endif
            }
        }
    }
#if defined(SERIALDEBUG)
    warning_trace_info("serial open returning %d", (LONGINT)err);
#endif
    DOCOMPLETION(pbp, err);
}

static OSErr C_ROMlib_serialprime(ParmBlkPtr pbp, DCtlPtr dcp) /* INTERNAL */
{
    OSErr err;
    hiddenh h;
    char *buf;
    size_t req_count;

    buf = (char *)pbp->ioParam.ioBuffer;
    req_count = pbp->ioParam.ioReqCount;

    err = noErr;
    if(!(dcp->dCtlFlags & OPENBIT))
        err = notOpenErr;
    else
    {
#if defined(SERIALDEBUG)
        warning_trace_info("serial prime code %d", (LONGINT)(pbp->ioParam.ioTrap & 0xFF));
#endif
        h = (hiddenh)dcp->dCtlStorage;
        switch(pbp->ioParam.ioTrap & 0xFF)
        {
            case aRdCmd:
                if(pbp->cntrlParam.ioCRefNum != AINREFNUM && pbp->cntrlParam.ioCRefNum != BINREFNUM)
                    err = readErr;
                else
                {
/* this may have to be changed since we aren't looking for
		   parity and framing errors */
#if defined(__linux__) || defined(__APPLE__)
                    pbp->ioParam.ioActCount = read((*h)->fd, buf, req_count);
#elif defined(_WIN32)
                    pbp->ioParam.ioActCount = serial_bios_read((*h)->fd, buf,
                                                                  req_count);
#else
// FIXME: #warning not sure what to do here
#endif
#if defined(SERIALDEBUG)
                    warning_trace_info("serial prime read %d bytes, first is 0x%0x",
                                       (LONGINT)pbp->ioParam.ioActCount,
                                       (LONGINT)(unsigned char)buf[0]);
#endif
                }
                break;
            case aWrCmd:
                if(pbp->cntrlParam.ioCRefNum != AOUTREFNUM && pbp->cntrlParam.ioCRefNum != BOUTREFNUM)
                    err = writErr;
                else
                {
#if defined(SERIALDEBUG)
                    warning_trace_info("serial prime writing %d bytes, first is 0x%0x",
                                       (LONGINT)req_count,
                                       (LONGINT)(unsigned char)buf[0]);
#endif
#if defined(__linux__) || defined(__APPLE__)
                    pbp->ioParam.ioActCount = write((*h)->fd,
                                                       buf, req_count);
#elif defined(_WIN32)
                    pbp->ioParam.ioActCount = serial_bios_write((*h)->fd,
                                                                   buf, req_count);
#else
// FIXME: #warning not sure what to do here
#endif
                }
                break;
            default:
                err = badUnitErr;
                break;
        }
    }
#if defined(SERIALDEBUG)
    warning_trace_info("serial prime returning %d", (LONGINT)err);
#endif
    DOCOMPLETION(pbp, err);
}

#if defined(USE_TERMIOS)

#define OURCS7 (CS7)
#define OURCS8 (CS8)
#define OURODDP (PARENB | PARODD)
#define OUREVENP (PARENB)

/* EXTA/EXTB are legacy BSD aliases, exposed by glibc only under
 * _DEFAULT_SOURCE and not part of POSIX; B19200/B38400 are.  The sgtty
 * branch below still needs the EXT* spellings, where they are sgtty
 * speed codes rather than termios ones. */
#define OURB19200 (B19200)
#define OURB38400 (B38400)

#else

#define OURCS7 (0)

#if defined(NEXTSTEP)
#define OURCS8 (LPASS8 | LPASS8OUT)
#else
#define OURCS8 (LPASS8)
#endif

#define OURODDP (ODDP)
#define OUREVENP (EVENP)

#define OURB19200 (EXTA)
#define OURB38400 (EXTB)

#endif

static OSErr serset(LONGINT fd, INTEGER param)
{
#if defined(_WIN32)
    OSErr retval;

    retval = serial_bios_serset(fd, param);
    return retval;
#else

#if defined(USE_TERMIOS)
    struct termios settings;
#else
    struct sgttyb sgttyb;
    struct tchars tchars;
    ULONGINT lclmode;
#endif

    OSErr err;
    LONGINT baud, csize, stopbits, parity;

#if !defined(LETGCCWAIL)
    baud = 0;
    csize = 0;
    stopbits = 0;
#endif

    err = noErr;

    switch(param & 0x3FF)
    {
        case baud300:
            baud = B300;
            break;
        case baud600:
            baud = B600;
            break;
        case baud1200:
            baud = B1200;
            break;
        case baud1800:
            baud = B1800;
            break;
        case baud2400:
            baud = B2400;
            break;
        case baud4800:
            baud = B4800;
            break;
        case baud9600:
            baud = B9600;
            break;
        case baud19200:
            baud = OURB19200;
            break;
        case 1: /* really 38400 according to Terminal 2.2 source */
            baud = OURB38400;
            break;
        case baud3600: /* these case labels aren't necessary since they */
        case baud7200: /* fall into default anyway, just thought you */
        case baud57600: /* might like to know what you're missing */
        default:
/*
 * NOTE: setting controlErr here isn't probably a good idea, since
 *       Compuserve Signup does a serset with baud57600 and then later
 *	 adjusts the baud with a "setbaud"
 */
#if 0
	err = controlErr;
#else
            baud = B2400;
#endif
            break;
    }

    switch(param & 0xC00)
    {

        case data5:
        case data6:
            err = controlErr;
            break;

        case data7:
            csize = OURCS7;
            break;

        case data8:
            csize = OURCS8;
            break;
    }

    switch(param & 0x3000)
    {

        case oddParity:
            parity = OURODDP;
            break;

        case evenParity:
            parity = OUREVENP;
            break;

        default:
            parity = 0;
            break;
    }

    switch((param >> 14) & 0x3)
    {
        case 1:
            stopbits = 0;
            break;
        case 3:
#if defined(USE_TERMIOS)
            stopbits = CSTOPB;
            break;
#endif
        case 0:
        case 2:
            err = controlErr;
            break;
    }

    if(err == noErr)
    {
#if defined(USE_TERMIOS)
        /* Build the line settings from scratch.  Zero first: the old
         * System V version left c_cc[] uninitialized, which meant VMIN
         * and VTIME were garbage.  VMIN = VTIME = 0 gives a polling
         * read(), which is what the Prime routine wants -- it checks
         * FIONREAD before reading. */
        memset(&settings, 0, sizeof settings);
        settings.c_iflag = IGNPAR | IXON | IXOFF;
        settings.c_oflag = 0;
        /* CREAD was missing here; without it the receiver stays off and
         * no input ever arrives.  CLOCAL keeps us from blocking on
         * modem control lines we don't emulate. */
        settings.c_cflag = csize | stopbits | parity | CREAD | CLOCAL;
        settings.c_lflag = 0;
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 0;
        if(cfsetispeed(&settings, baud) < 0 || cfsetospeed(&settings, baud) < 0)
            err = ROMlib_maperrno();
        else
            err = tcsetattr(fd, TCSADRAIN, &settings) < 0 ? ROMlib_maperrno() : noErr;
#else
        sgttyb.sg_ispeed = sgttyb.sg_ospeed = baud;
        sgttyb.sg_erase = -1;
        sgttyb.sg_kill = -1;
        sgttyb.sg_flags = TANDEM | CBREAK | parity;
        tchars.t_intrc = -1;
        tchars.t_quitc = -1;
#if 0 /* This isn't the place to mess with flow control */
	if (sershkp->fXOn || sershkp->fInX) {
	    tchars.t_startc = sershkp->xOn;
	    tchars.t_stopc = sershkp->xOff;
	} else {
	    tchars.t_startc = sershkp->xOn;
	    tchars.t_stopc = sershkp->xOff;
	}
	/* Cliff, LDECCTQ restricts XON to startc ONLY! --Lee */
	lclmode = sershkp->fXOn ? LDECCTQ : 0;
#else
        lclmode = 0;
#endif
        tchars.t_eofc = -1;
        tchars.t_brkc = -1;
        lclmode |= LLITOUT | LNOHANG | csize | LNOFLSH;
        if(ioctl(fd, TIOCSETP, &sgttyb) < 0 || ioctl(fd, TIOCSETC, &tchars) < 0 || ioctl(fd, TIOCLSET, &lclmode) < 0)
            err = ROMlib_maperrno();
#endif
    }
    return err;
#endif
}

static OSErr serxhshake(LONGINT fd, SerShk *sershkp)
{
#if defined(_WIN32)
    OSErr retval;

    retval = serial_bios_serxhshake(fd, sershkp);
    return retval;
#else

#if defined(USE_TERMIOS)
    struct termios settings;
#else
    struct sgttyb sgttyb;
    struct tchars tchars;
#endif
    OSErr err;

    err = noErr;
#if defined(USE_TERMIOS)
    if(tcgetattr(fd, &settings) < 0)
        err = ROMlib_maperrno();
#else
    if(ioctl(fd, TIOCGETP, &sgttyb) < 0 || ioctl(fd, TIOCGETC, &tchars) < 0)
        err = ROMlib_maperrno();
#endif
    else
    {
        if(sershkp->fXOn)
        {
#if defined(USE_TERMIOS)
            settings.c_iflag |= IXON;
        }
        else
            settings.c_iflag &= ~IXON;
#else
            /* NB: LDECCTQ is left (un)set in the local mode word. */
            tchars.t_startc = -1;
            tchars.t_stopc = -1;
        }
        else
        {
            tchars.t_startc = sershkp->xOn;
            tchars.t_stopc = sershkp->xOff;
        }
#endif
        if(sershkp->fInX)
        {
#if defined(USE_TERMIOS)
            settings.c_iflag |= IXOFF;
        }
        else
            settings.c_iflag &= ~IXOFF;
        err = tcsetattr(fd, TCSADRAIN, &settings) < 0 ? ROMlib_maperrno() : noErr;
#else
            /* Can't enable TANDEM without setting startc & stopc. If I
	     * do that, input flow control is also enabled. This is what
	     * has been implemented here. The alternative is to refuse the
	     * request.
	     */
            sgttyb.sg_flags |= TANDEM;
            tchars.t_startc = sershkp->xOn;
            tchars.t_stopc = sershkp->xOff;
        }
        else
            sgttyb.sg_flags &= ~TANDEM;
        if(ioctl(fd, TIOCSETP, &sgttyb) < 0 || ioctl(fd, TIOCSETC, &tchars) < 0)
            err = ROMlib_maperrno();
#endif
    }
    return err;
#endif
}

static OSErr setbaud(LONGINT fd, INTEGER baud)
{
#if defined(_WIN32)
    OSErr retval;

    retval = serial_bios_setbaud(fd, baud);
    return retval;
#else

#if defined(USE_TERMIOS)
    struct termios settings;
#else
    struct sgttyb sgttyb;
#endif
    static INTEGER rates[] = {
        50, 75, 110, 134, 150, 200, 300,
        600, 1200, 1800, 2400, 4800, 9600, 19200,
        (INTEGER)38400, (INTEGER)32767,
    },
                   *ip;
#if defined(USE_TERMIOS)
    /* Parallel to rates[], minus the 32767 sentinel.  The old code
     * relied on the B* constants being numerically equal to the index
     * plus one, which is true on Linux but is not portable. */
    static const speed_t speeds[] = {
        B50, B75, B110, B134, B150, B200, B300,
        B600, B1200, B1800, B2400, B4800, B9600, B19200,
        B38400,
    };
#endif
    OSErr err;

    for(ip = rates; *ip < baud; ip++)
        ;
    /* ip != rates guards the ip[-1] read below; the original walked off
     * the front of the array for any requested rate <= 50. */
    if(*ip == 32767 || (ip != rates && ip[0] - baud > baud - ip[-1]))
        --ip;
#if defined(USE_TERMIOS)
    if(tcgetattr(fd, &settings) < 0)
#else
    if(ioctl(fd, TIOCGETP, &sgttyb) < 0)
#endif
        err = ROMlib_maperrno();
    else
    {
#if defined(USE_TERMIOS)
        speed_t speed = speeds[ip - rates];

        if(cfsetispeed(&settings, speed) < 0 || cfsetospeed(&settings, speed) < 0)
            err = ROMlib_maperrno();
        else
            err = tcsetattr(fd, TCSADRAIN, &settings) < 0 ? ROMlib_maperrno() : noErr;
#else
        sgttyb.sg_ispeed = sgttyb.sg_ospeed = ip - rates + 1;
        err = ioctl(fd, TIOCGETP, &sgttyb) < 0 ? ROMlib_maperrno() : noErr;
#endif
    }
    return err;
#endif
}

static OSErr ctlbrk(LONGINT fd, INTEGER flag)
{
#if defined(_WIN32)
    OSErr retval;

    retval = serial_bios_ctlbrk(fd, flag);
    return retval;
#else
    OSErr err;

#if defined(USE_TERMIOS)
    if(flag == SER_START)
    {
        /* NOTE:  This will send a break for 1/4 sec */
        err = tcsendbreak(fd, 0);
    }
    else
        err = noErr;
#else
    err = ioctl(fd, flag == SER_START ? TIOCSBRK : TIOCCBRK, 0);
#endif
    return err < 0 ? ROMlib_maperrno() : noErr;
#endif
}

static OSErr flow(LONGINT fd, LONGINT flag)
{
#if defined(_WIN32)
    OSErr retval;

    retval = serial_bios_setflow(fd, flag);
    return retval;
#else
    OSErr err;

#if defined(USE_TERMIOS)
    /* TCXONC's 0/1 arguments were TCOOFF/TCOON. */
    err = tcflow(fd, flag == SER_STOP ? TCOOFF : TCOON);
#else
    err = ioctl(fd, flag == SER_STOP ? TIOCSTOP : TIOCSTART, 0);
#endif
    return err < 0 ? ROMlib_maperrno() : noErr;
#endif
}

#define SERKILLIO 1

static OSErr C_ROMlib_serialctl(ParmBlkPtr pbp, DCtlPtr dcp) /* INTERNAL */
{
    OSErr err;
    hiddenh h;

    if(!(dcp->dCtlFlags & OPENBIT))
        err = notOpenErr;
    else
    {
#if defined(SERIALDEBUG)
        warning_trace_info("serial control code = %d, param0 = 0x%x",
                           (LONGINT)pbp->cntrlParam.csCode,
                           (LONGINT)(unsigned short)pbp->cntrlParam.csParam[0]);
#endif
        h = (hiddenh)dcp->dCtlStorage;
        switch(pbp->cntrlParam.csCode)
        {
            case SERKILLIO:
                err = noErr; /* All I/O done synchronously */
                break;
            case kSERDConfiguration:
                err = serset((*h)->fd, pbp->cntrlParam.csParam[0]);
                break;
            case kSERDInputBuffer:
                err = noErr; /* ignored */
                break;
            case kSERDSerHShake:
            case kSERDHandshake: /* NOTE:  DTR handshake isn't supported  */
                err = serxhshake((*h)->fd, (SerShk *)pbp->cntrlParam.csParam);
                break;
            case kSERDSetBreak:
                err = ctlbrk((*h)->fd, SER_START);
                break;
            case kSERDClearBreak:
                err = ctlbrk((*h)->fd, SER_STOP);
                break;
            case kSERDBaudRate:
                err = setbaud((*h)->fd, pbp->cntrlParam.csParam[0]);
                break;
            case kSERDMiscOptions:
#if defined(_WIN32)
                err = serial_bios_setdtr((*h)->fd);
#else
                err = controlErr; /* not supported */
#endif
                break;
            case kSERDAssertDTR:
#if defined(_WIN32)
                err = serial_bios_clrdtr((*h)->fd);
#else
                err = controlErr; /* not supported */
#endif
                break;
            case kSERDNegateDTR:
                err = controlErr; /* not supported */
                break;
            case kSERDSetPEChar:
                err = controlErr; /* not supported */
                break;
            case kSERDSetPEAltChar:
                err = controlErr; /* not supported */
                break;
            case kSERDSetXOffFlag:
                err = flow((*h)->fd, SER_STOP);
                break;
            case kSERDClearXOffFlag:
                err = flow((*h)->fd, SER_START);
                break;
            case kSERDSendXOn:
                err = controlErr; /* not supported */
                break;
#if defined(__linux__) || defined(__APPLE__)
            case kSERDSendXOnOut:
            {
                char c = XONC;
                err = write((*h)->fd, &c, 1) != 1 ? ROMlib_maperrno() : noErr;
                break;
            }
#endif
            case kSERDSendXOff:
                err = controlErr; /* not supported */
                break;
#if defined(__linux__) || defined(__APPLE__)
            case kSERDSendXOffOut:
            {
                char c = XOFFC;
                err = write((*h)->fd, &c, 1) != 1 ? ROMlib_maperrno() : noErr;
                break;
            }
#endif
            case kSERDResetChannel:
                err = controlErr; /* not supported */
                break;
            default:
                err = controlErr;
                break;
        }
    }
#if defined(SERIALDEBUG)
    warning_trace_info("serial control returning %d", (LONGINT)err);
#endif
    DOCOMPLETION(pbp, err);
}

/*
 * NOTE:  kSERDStatus lies about everything except rdPend.
 */

static OSErr C_ROMlib_serialstatus(ParmBlkPtr pbp, DCtlPtr dcp) /* INTERNAL */
{
    OSErr err;
    hiddenh h;
    LONGINT n;

    if(!(dcp->dCtlFlags & OPENBIT))
        err = notOpenErr;
    else
    {
#if defined(SERIALDEBUG)
        warning_trace_info("serial status csCode = %d", (LONGINT)pbp->cntrlParam.csCode);
#endif
        h = (hiddenh)dcp->dCtlStorage;
        switch(pbp->cntrlParam.csCode)
        {
            case kSERDInputCount:
#if defined(__linux__) || defined(__APPLE__)
                if(ioctl((*h)->fd, FIONREAD, &n) < 0)
#else
                if(serial_bios_fionread((*h)->fd, &n) < 0)
#endif
                    err = ROMlib_maperrno();
                else
                {
#if defined(SERIALDEBUG)
                    warning_trace_info("serial status getbuf = %d", (LONGINT)n);
#endif
                    *(GUEST<LONGINT> *)pbp->cntrlParam.csParam = n;
                    err = noErr;
                }
                break;
            case kSERDStatus:
#if defined(__linux__) || defined(__APPLE__)
                if(ioctl((*h)->fd, FIONREAD, &n) < 0)
#else
                if(serial_bios_fionread((*h)->fd, &n) < 0)
#endif
                    err = ROMlib_maperrno();
                else
                {
                    ((SerStaRec *)pbp->cntrlParam.csParam)->cumErrs = 0;
                    ((SerStaRec *)pbp->cntrlParam.csParam)->xOffSent = 0;
                    ((SerStaRec *)pbp->cntrlParam.csParam)->rdPend = n > 0 ? 1 : 0;
                    ((SerStaRec *)pbp->cntrlParam.csParam)->wrPend = 0;
                    ((SerStaRec *)pbp->cntrlParam.csParam)->ctsHold = 0;
                    ((SerStaRec *)pbp->cntrlParam.csParam)->xOffHold = 0;
                    err = noErr;
                }
                break;
            default:
                err = statusErr;
                break;
        }
    }
#if defined(SERIALDEBUG)
    warning_trace_info("serial status returning %d", (LONGINT)err);
#endif
    DOCOMPLETION(pbp, err);
}

static void restorecloseanddispose(hiddenh h)
{
#if defined(__linux__) || defined(__APPLE__)
#if defined(USE_TERMIOS)
    tcsetattr((*h)->fd, TCSADRAIN, &(*h)->state);
#else
    ioctl((*h)->fd, TIOCSETP, &(*h)->sgttyb);
    ioctl((*h)->fd, TIOCSETC, &(*h)->tchars);
    ioctl((*h)->fd, TIOCLSET, &(*h)->lclmode);
#endif
    close((*h)->fd);
#endif
    DisposeHandle((Handle)h);
}

static OSErr C_ROMlib_serialclose(ParmBlkPtr pbp, DCtlPtr dcp) /* INTERNAL */
{
    OSErr err;
    hiddenh h;

    if(dcp->dCtlFlags & OPENBIT)
    {
        h = (hiddenh)dcp->dCtlStorage;
        restorecloseanddispose(h);
        dcp->dCtlFlags &= ~OPENBIT;
        err = noErr;
    }
    else
        err = notOpenErr;
#if defined(SERIALDEBUG)
    warning_trace_info("serial close returning %d", (LONGINT)err);
#endif
    DOCOMPLETION(pbp, err);
}

void Executor::InitSerialDriver()
{
    RegisterDriver({
            &ROMlib_serialopen, &ROMlib_serialprime, &ROMlib_serialctl,
            &ROMlib_serialstatus, &ROMlib_serialclose, (StringPtr) "\04.AIn", -6,
        });

     RegisterDriver({
            &ROMlib_serialopen, &ROMlib_serialprime, &ROMlib_serialctl,
            &ROMlib_serialstatus, &ROMlib_serialclose, (StringPtr) "\05.AOut", -7,
        });

    RegisterDriver({
            &ROMlib_serialopen, &ROMlib_serialprime, &ROMlib_serialctl,
            &ROMlib_serialstatus, &ROMlib_serialclose, (StringPtr) "\04.BIn", -8,
        });

    RegisterDriver({
            &ROMlib_serialopen, &ROMlib_serialprime, &ROMlib_serialctl,
            &ROMlib_serialstatus, &ROMlib_serialclose, (StringPtr) "\05.BOut", -9,
        });
}
