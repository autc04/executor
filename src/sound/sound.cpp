/* Copyright 1990, 1992, 1995, 1996 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in SoundMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <MemoryMgr.h>
#include <ResourceMgr.h>
#include <SoundDvr.h>
#include <SoundMgr.h>
#include <FileMgr.h>
#include <sound/sounddriver.h>
#include <mman/mman.h>
#include <base/functions.impl.h>
#include <time/syncint.h>

using namespace Executor;

/* true when we want to pretend this host has no sound support. */
bool Executor::sound_disabled_p;

/*
 * TODO: The flag values below are just ctm guesses.  We need to run
 *	 tests on a Mac to see whether or not these flags are present
 *	 and if so, what values they take.
 */

#if defined(SOUNDGLUEROUTINES)
void Executor::StartSound(Ptr srec, LONGINT nb, ProcPtr comp)
{
}

void Executor::StopSound()
{
}

Boolean Executor::SoundDone()
{
    return true;
}

void Executor::GetSoundVol(INTEGER *volp)
{
}

void Executor::SetSoundVol(INTEGER vol)
{
}
#endif

static inline bool
qfull_p(SndChannelPtr chanp)
{
    return ((chanp->qTail == chanp->qHead - 1 || (chanp->qHead == 0
                                                          && chanp->qTail == chanp->qLength - 1)));
}

static inline bool
qempty_p(SndChannelPtr chanp)
{
    /* TetrisMax sometimes does something with a 0-length queue under
   * DOS when you quit, which causes a division by zero on a % in
   * `deq'; this is probably a bug somewhere else, but checking for a
   * spewy queue will make Executor more robust and may be adequate to
   * get 2.0 out the door.
   */
    return (chanp->qHead == chanp->qTail
            || chanp->qLength <= 0);
}

static inline void
enq(SndChannelPtr chanp, SndCommand cmd)
{
    unsigned tail;

    tail = chanp->qTail;
    chanp->queue[tail] = cmd;
    chanp->qTail = (tail + 1) % chanp->qLength;
}

static inline SndCommand
deq(SndChannelPtr chanp)
{
    SndCommand ret;
    unsigned head;

    head = chanp->qHead;
    ret = chanp->queue[head];
    chanp->qHead = (head + 1) % chanp->qLength;

    return ret;
}

#if 0
static inline double
f2d (snd_time x)
{
  return x / (double) (1ULL << (4 * sizeof x));
}
#endif

static inline void
wait_on_channel(volatile SndChannelPtr vchanp)
{
    while(!qempty_p(vchanp) || SND_CHAN_CMDINPROG_P(vchanp))
        syncint_check_interrupt();
}

static snd_time
snd_fixed_div(snd_time x, snd_time y)
{
    decltype(x) temp1, temp2;
    int bits;

    bits = 4 * sizeof x;

    temp1 = x / y;

    temp2 = (x - y * temp1) << bits;
    temp2 = temp2 / y;

    return (temp1 << bits) + temp2;
}

static snd_time
snd_fixed_mul(snd_time x, snd_time y)
{
    decltype(x) x1, x0, y1, y0;
    int bits;

    bits = 4 * sizeof x;
    x0 = x % (1ULL << bits);
    x1 = x >> bits;
    y0 = y % (1ULL << bits);
    y1 = y >> bits;

    return (((x0 * y0) >> bits) + x0 * y1 + x1 * y0 + ((x1 * y1) << bits));
}

/* This table is used to add two sound values together and pin
 * the value to avoid overflow.
 */
static const uint8_t mix8[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
    0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24,
    0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50,
    0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B,
    0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66,
    0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71,
    0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C,
    0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92,
    0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D,
    0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
    0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
    0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
    0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4,
    0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
    0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5,
    0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

/* Return true if the buffer was used up.  I hate value-result parms,
   but oh well.  A nullptr orig_outbuf is OK; the resampled waveform
   will simply be lost. */

static bool
resample(uint8_t *inbuf, uint8_t *orig_outbuf, unsigned int insize,
         unsigned int outsize, uint32_t infreq, uint32_t outfreq,
         snd_time *current_start, uint8_t *prev_samp,
         snd_time *chan_time, snd_time until)

{
    snd_time step;
    snd_time in_t, out_t;
    uint8_t *outbuf;

    /* FIXME - this does lots of pointless work when we're going to
   * ignore the resampled waveform anyway, but it makes the "real"
   * sound and "dummy" sound cases more similar.  Is it worth it?
   */
    outbuf = orig_outbuf ? orig_outbuf : (uint8_t *)alloca(outsize);

    step = snd_fixed_div(infreq, outfreq);

    while(SND_PROMOTE(*chan_time) < *current_start)
    {
        outbuf[*chan_time % outsize] = mix8[((unsigned int)*prev_samp
                                             + (unsigned int)outbuf[*chan_time % outsize])];
        (*chan_time)++;
    }

    in_t = snd_fixed_mul(SND_PROMOTE(*chan_time) - *current_start, step);
    out_t = *chan_time;

#if 0
  warning_sound_log ("step %g in_t %.9g until %.9g", f2d (step),
		     f2d (in_t),
		     (double) until);
  warning_sound_log (" cur_start %.9g insize %d", f2d (*current_start),
		     insize);
  warning_sound_log (" chan_time %.9g out_t %.9g outsize %d",
		     (double) *chan_time, (double) out_t, outsize);
#endif

    while(out_t < until && SND_DEMOTE(in_t) < insize - 1)
    {
        outbuf[out_t % outsize] = mix8[((unsigned int)inbuf[SND_DEMOTE(in_t)]
                                        + (unsigned int)outbuf[out_t % outsize])];
        in_t += step;
        out_t += 1;
    }

    *chan_time = out_t;
    if(SND_DEMOTE(in_t) < insize - 1)
        return false;
    else
    {
        *current_start = (SND_PROMOTE(out_t)
                          + snd_fixed_div(SND_PROMOTE(insize), step)
                          - snd_fixed_div(in_t, step));
        *prev_samp = inbuf[insize - 1];

        return true;
    }
}

int Executor::ROMlib_get_snd_cmds(Handle sndh, SndCommand **cmdsp)
{
    Ptr p;
    int format;
    int num_formats;
    int retval;

    p = *sndh;
    format = *(GUEST<INTEGER> *)p;
    switch(format)
    {
        case 1:
            p += 2;
            num_formats = *(GUEST<INTEGER> *)p;
            switch(num_formats)
            {
                case 0:
                    p += 2;
                    break;
                case 1:
                    p += 8;
                    break;
                default:
                    warning_sound_log(" WARNING: num_formats = %d", num_formats);
                    p += (2 + 6 * num_formats);
                    break;
            }
            break;

        case 2:
            p += 4;
            break;

        default:
            warning_sound_log("Unrecognized sound format %d", format);
            return 0; /* FIXME */
            break;
    }

    /* Now p points to "Number of sound commands" field of resource */

    retval = *(GUEST<INTEGER> *)p;
    *cmdsp = (SndCommand *)(p + 2);

    return retval;
}

OSErr Executor::C_SndPlay(SndChannelPtr chanp, Handle sndh, Boolean async)
{
    OSErr retval;
    Ptr resp;
    SndCommand *cmds;
    int num_commands, i, format;
    bool need_allocate;

    if(sndh)
        resp = *sndh;
    else
        resp = nullptr;

    if(resp)
        format = *(GUEST<INTEGER> *)resp;
    else
        format = 0;

    warning_sound_log("chanp %p fmt %d num_fmts %d async %d",
                      chanp, format, resp ? toHost(*((GUEST<INTEGER> *)(resp + 2))) : 0,
                      async);

    warning_sound_log(" sndh %s", (HGetState(sndh) & LOCKBIT) ? "Locked" : "Unlocked");

    switch(ROMlib_PretendSound)
    {
        default:
        case soundoff:
            retval = notEnoughHardware;
            break;
        case soundpretend:
            retval = noErr;
            break;
        case soundon:
            num_commands = ROMlib_get_snd_cmds(sndh, &cmds);

            warning_sound_log(" num_commands = %d", num_commands);

            need_allocate = (chanp == nullptr);

            if(need_allocate)
            {
                GUEST<SndChannelPtr> foo;

                foo = nullptr;
                SndNewChannel(&foo, sampledSynth, 0, 0);
                chanp = foo;
            }

            for(i = 0; i < num_commands; i++)
            {
                SndCommand cmd;

                cmd = cmds[i];

                /* If high bit of cmd is set, then param2 is actually an
           offset */
                if(cmd.cmd & 0x8000)
                {
                    cmd.param2 = guest_cast<LONGINT>((resp + cmd.param2));
                    cmd.cmd &= ~0x8000;
                }

                /* Need to figure this out properly; FIXME */
                if(format == 2 && i == 0 && cmd.cmd == soundCmd)
                {
                    cmd.cmd = bufferCmd;
                }

                C_SndDoCommand(chanp, &cmd, 0);
            }

            if(need_allocate || !async)
                wait_on_channel(chanp);

            if(need_allocate)
                SndDisposeChannel(chanp, 0);

            retval = noErr;
            break;
    }
    return retval;
}

GUEST<SndChannelPtr> Executor::allchans;

OSErr Executor::C_SndNewChannel(GUEST<SndChannelPtr> *chanpp, INTEGER synth,
                                LONGINT init, SndCallbackUPP userroutinep)
{
    SndChannelPtr chanp;
    OSErr retval;

    warning_sound_log("chanp %p synth %d init 0x%x userroutine %p",
                      chanpp ? *chanpp : nullptr, synth, init, userroutinep);

    switch(ROMlib_PretendSound)
    {
        default:
        case soundoff:
            retval = notEnoughHardware;
            break;
        case soundpretend:
            retval = noErr;
            break;

        case soundon:
            if(*chanpp == nullptr)
            {
                *chanpp = (SndChannelPtr)NewPtr(sizeof(SndChannel));
                chanp = *chanpp;
                chanp->flags = CHAN_ALLOC_FLAG;
            }
            else
            {
                chanp = *chanpp;
                chanp->flags = 0;
            }
            chanp->nextChan = allchans;
            allchans = chanp;
            chanp->firstMod = (Ptr)NewPtr(sizeof(ModifierStub));
            SND_CHAN_TIME(chanp) = 0;
            SND_CHAN_CURRENT_START(chanp) = 0;
            chanp->callBack = userroutinep;
            /*chanp->userInfo = 0;*/
            chanp->wait = 0;
            chanp->cmdInProg.cmd = 0;
            chanp->qLength = stdQLength;
            chanp->qHead = 0;
            chanp->qTail = 0;
            retval = noErr;
    }
    return retval;
}

#define MOD_SYNTH_FLAG 1

OSErr Executor::C_SndAddModifier(SndChannelPtr chanp, ProcPtr mod, INTEGER id,
                                 LONGINT init)
{
#if defined(OLD_BROKEN_NEXTSTEP_SOUND)
    Handle h;
    SndCommand cmd;
    ModifierStubPtr modp;
#endif /* defined(OLD_BROKEN_NEXTSTEP_SOUND) */
    OSErr retval;

    warning_sound_log("chanp %p", chanp);

    switch(ROMlib_PretendSound)
    {
        default:
        case soundoff:
            retval = notEnoughHardware;
            break;
        case soundpretend:
            retval = noErr;
            break;
#if defined(OLD_BROKEN_NEXTSTEP_SOUND)
        case soundon:
            if((unsigned short)chanp->qLength != (unsigned short)stdQLength)
                retval = badChannel;
            else
            {
                modp = (ModifierStubPtr)NewPtr(sizeof(ModifierStub));
                modp->nextStub = chanp->firstMod;
                chanp->firstMod = modp;
                modp->flags = 0;
                if(mod)
                {
                    modp->code = mod;
                    modp->hState = 0;
                }
                else
                {
                    h = GetResource("snth"_4, id);
                    LoadResource(h);
                    modp->flags = MOD_SYNTH_FLAG;
                    modp->hState = HGetState(h);
                    HLock(h);
                    modp->code = (ProcPtr)*h;
                }
                modp->userInfo = 0;
                modp->count = 0;
                modp->every = 0;
                if(modp->code)
                {
                    cmd.cmd = initCmd;
                    cmd.param1 = 0;
                    cmd.param2 = init;
                    retval = SndDoImmediate(chanp, &cmd);
                }
                else
                    retval = resProblem;
            }
            break;
#endif
    }
    return retval;
}

#if defined(OLD_BROKEN_NEXTSTEP_SOUND)
static void dumpcmd(SndCommand *cmdp)
{
    printf("#%x,1-%x,2-%x.", (LONGINT)cmdp->cmd, (LONGINT)cmdp->param1, (LONGINT)cmdp->param2);
}
#endif


Boolean callasynth(SndChannelPtr chanp, SndCommand *cmdp, ModifierStubPtr mp)
{
    return mp->code(chanp, cmdp, mp);
}

#if defined(OLD_BROKEN_NEXTSTEP_SOUND)
static void recsndcmd(SndChannelPtr chanp, SndCommand *cmdp, ModifierStubPtr mp)
{
    INTEGER i;
    Boolean doanother;

    if(mp)
    {
        i = 0;
        do
        {
            doanother = callasynth(chanp, cmdp, mp);
            recsndcmd(chanp, cmdp, mp->nextStub);
            if(doanother)
            {
                cmdp->cmd = requestNextCmd;
                cmdp->param1 = ++i;
                cmdp->param2 = 0;
            }
        } while(doanother);
    }
}
#endif /* defined(OLD_BROKEN_NEXTSTEP_SOUND) */

static inline unsigned int
snd_duration(SoundHeaderPtr hp)
{
    return hp->length;
}

#define CMD_DONE(c) ((c)->flags &= ~CHAN_CMDINPROG_FLAG)

static void
do_current_command(SndChannelPtr chanp, HungerInfo info)
{
    unsigned long duration;
    unsigned char *sp;
    SoundHeaderPtr hp;
    SndCommand cmd;

    switch(chanp->cmdInProg.cmd)
    {
        case bufferCmd:
            hp = guest_cast<SoundHeaderPtr>(chanp->cmdInProg.param2);

            if(hp->encode != stdSH)
            {
                warning_unimplemented("Ignoring unsupported SoundHeader "
                                      "encoding: %s",
                                      ((hp->encode == cmpSH)
                                           ? "compressed"
                                           : ((hp->encode == extSH)
                                                  ? "extended"
                                                  : "<unknown>")));
                CMD_DONE(chanp);
            }
            else
            {
                duration = snd_duration(hp);

                sp = (hp->samplePtr ? (unsigned char *)hp->samplePtr
                                    : hp->sampleArea);

                warning_sound_log("bufferCmd dur %d", (int)duration);

                if(resample(sp, info.buf, hp->length,
                            info.bufsize, hp->sampleRate,
                            info.rate << 16,
                            &SND_CHAN_CURRENT_START(chanp),
                            &SND_CHAN_PREV_SAMP(chanp),
                            &SND_CHAN_TIME(chanp),
                            info.t3))
                {
                    CMD_DONE(chanp);
                }
            }
            break;

        case callBackCmd:
            warning_sound_log("callBackCmd");
            cmd = chanp->cmdInProg;
            chanp->callBack
            (chanp, &cmd);
            CMD_DONE(chanp);
            break;

        case ampCmd:
            warning_sound_log("ampCmd (ignored)");
            CMD_DONE(chanp);
            break;

        default:
            warning_sound_log("UNKNOWN CMD %d\n",
                              chanp ? chanp->cmdInProg.cmd.get() : 0);
            CMD_DONE(chanp);
    }
}

#define SND_DB_DONE(c) ((c)->flags &= ~CHAN_DBINPROG_FLAG)

static void
do_current_db(SndChannelPtr chanp, HungerInfo info)
{
    SndDoubleBufferHeader *dbhp;
    SndDoubleBuffer *dbp;
    unsigned char *sp;

    dbhp = SND_CHAN_DBHP(chanp);
    dbp = dbhp->dbhBufferPtr[SND_CHAN_CURRENT_DB(chanp)];

    if(!(dbp->dbFlags & dbBufferReady))
    {
        /* This buffer isn't ready */
        warning_sound_log("notready");
        SND_CHAN_TIME(chanp) = info.t3;
        SND_CHAN_CURRENT_START(chanp) = SND_PROMOTE(info.t3);
        return;
    }
    sp = dbp->dbSoundData;

#if 0
  warning_sound_log ("curs %g t %d t3 %d dur %d rate %g",
		     f2d (SND_CHAN_CURRENT_START (chanp)),
		     (int) SND_CHAN_TIME (chanp),
		     (int) info.t3,
		     dbp->dbNumFrames,
		     dbhp->dbhSampleRate / (double) (1 << 16));
#endif
    if(resample(sp, info.buf, dbp->dbNumFrames,
                info.bufsize, dbhp->dbhSampleRate,
                info.rate << 16,
                &SND_CHAN_CURRENT_START(chanp),
                &SND_CHAN_PREV_SAMP(chanp),
                &SND_CHAN_TIME(chanp),
                info.t3))
    {
        /* We are done with this buffer */
        dbp->dbFlags &= ~dbBufferReady;
        if(dbp->dbFlags & dbLastBuffer)
            /* We are completely done */
            SND_DB_DONE(chanp);
        else
        {
            /* Start the next buffer */
            SND_CHAN_CURRENT_DB(chanp) ^= 1;
        }
#if 0
      warning_sound_log ("dblback %p ch %p bp %p",
			 dbhp->dbhDoubleBack,
			 chanp, dbp);
      warning_sound_log (" frs %d flgs %d ui1 %x ui2 %x",
			 dbp->dbNumFrames,
			 dbp->dbFlags,
			 dbp->dbUserInfo[0],
			 dbp->dbUserInfo[1]);
#endif
        (dbhp->dbhDoubleBack)(chanp, dbp);
    }
}

syn68k_addr_t
Executor::sound_callback(syn68k_addr_t interrupt_addr, void *unused)
{
    M68kReg saved_regs[16];
    CCRElement saved_ccnz, saved_ccn, saved_ccc, saved_ccv, saved_ccx;

    //  fprintf (stderr, "in sound_callback\n");

    /* Save the 68k registers and cc bits away. */
    memcpy(saved_regs, &cpu_state.regs, sizeof saved_regs);
    saved_ccnz = cpu_state.ccnz;
    saved_ccn = cpu_state.ccn;
    saved_ccc = cpu_state.ccc;
    saved_ccv = cpu_state.ccv;
    saved_ccx = cpu_state.ccx;

    /* There's no reason to think we need to decrement A7 by 32;
   * it's just a paranoid thing to do.
   */
    EM_A7 = (EM_A7 - 32) & ~3; /* Might as well long-align it. */

    
    bool did_something = false;

    HungerInfo info = sound_driver->HungerStart();

    /* For each channel, grab some samples and mix them in */
    for(SndChannelPtr chanp = allchans; chanp != nullptr; chanp = chanp->nextChan)
    {
        if(SND_CHAN_TIME(chanp) < info.t2)
        {
            snd_time diff = info.t2 - SND_CHAN_TIME(chanp);

            SND_CHAN_TIME(chanp) += diff;
            SND_CHAN_CURRENT_START(chanp) += SND_PROMOTE(diff);
        }

        while(SND_CHAN_TIME(chanp) < info.t3)
        {
            if(SND_CHAN_CMDINPROG_P(chanp))
            {
                do_current_command(chanp, info);
                did_something = true;
            }
            else if(SND_CHAN_DBINPROG_P(chanp))
            {
                do_current_db(chanp, info);
                did_something = true;
            }
            else if(!qempty_p(chanp))
            {
                chanp->cmdInProg = deq(chanp);
                chanp->flags |= CHAN_CMDINPROG_FLAG;
                did_something = true;
            }
            else
            {
                break;
            }
        }
    }

    //  fprintf (stderr, "about to call sound_hunger_finish\n");

    sound_driver->HungerFinish();

    if(!did_something)
        sound_driver->sound_stop();

    memcpy(&cpu_state.regs, saved_regs, sizeof saved_regs);
    cpu_state.ccnz = saved_ccnz;
    cpu_state.ccn = saved_ccn;
    cpu_state.ccc = saved_ccc;
    cpu_state.ccv = saved_ccv;
    cpu_state.ccx = saved_ccx;

    return MAGIC_RTE_ADDRESS;
}

OSErr Executor::C_SndDoCommand(SndChannelPtr chanp, SndCommand *cmdp,
                               Boolean nowait)
{
    OSErr retval;

#if 1
    if(!cmdp)
        warning_sound_log("cmdp = nullptr");
    else
        warning_sound_log("cmd %d param1 0x%x param2 0x%x nowait %d", cmdp->cmd,
                          cmdp->param1, cmdp->param2, nowait);
#endif

#if ERROR_SUPPORTED_P(ERROR_SOUND_LOG)
    if(cmdp->cmd == bufferCmd)
    {
        SoundHeaderPtr hp;

        hp = guest_cast<SoundHeaderPtr>(cmdp->param2);
        if(!hp)
            warning_sound_log("hp = nullptr");
        else
            warning_sound_log(" len %d rate 0x%x encode %d freq %d",
                              hp->length, hp->sampleRate, hp->encode,
                              hp->baseFrequency);
    }
#endif

    switch(ROMlib_PretendSound)
    {
        default:
        case soundoff:
            retval = notEnoughHardware;
            break;
        case soundpretend:
            retval = noErr;
            break;

        case soundon:
            retval = noErr;
            sound_driver->sound_go();

            if(qfull_p(chanp))
            {
                if(nowait)
                    retval = queueFull;
                else
                {
                    volatile SndChannelPtr vchanp = chanp;

                    while(qfull_p(vchanp))
                        syncint_check_interrupt();
                    retval = noErr;
                }
            }
            if(retval == noErr)
            {
                enq(chanp, *cmdp);
                chanp->flags |= CHAN_BUSY_FLAG;
            }
            break;
    }

    return retval;
}

OSErr Executor::C_SndDoImmediate(SndChannelPtr chanp, SndCommand *cmdp)
{
    OSErr retval;
    SndCommand cmd;

    switch(ROMlib_PretendSound)
    {
        case soundoff:
        default:
            retval = notEnoughHardware;
            break;
        case soundpretend:
            retval = noErr;
            break;
        case soundon:
            sound_driver->sound_go();
#if 0 /* This is not a good check for badChannel */
      if ((unsigned short) chanp->qLength
	  != (unsigned short) stdQLength)
	retval = badChannel;
      else
#endif
            {
                cmd = *cmdp;
                switch(cmd.cmd)
                {
                    case flushCmd:
                        warning_sound_log("flushCmd");
                        while(!qempty_p(chanp))
                            (void)deq(chanp);
                        /* We need to flush the current channel as well as the Q */
                        CMD_DONE(chanp);
                        SND_DB_DONE(chanp);
                        SND_CHAN_TIME(chanp) = 0;
                        SND_CHAN_CURRENT_START(chanp) = 0;
                        retval = noErr;
                        break;

                    case quietCmd:
                        warning_sound_log("quietCmd");
                        CMD_DONE(chanp);
                        SND_DB_DONE(chanp);
                        retval = noErr;
                        break;

                    case bufferCmd:
                        warning_sound_log("bufferCmd");
                        chanp->cmdInProg = cmd;
                        chanp->flags |= CHAN_CMDINPROG_FLAG;
                        SND_CHAN_CURRENT_START(chanp) = SND_PROMOTE(SND_CHAN_TIME(chanp));
                        retval = noErr;
                        break;

                    default:
                        warning_sound_log("UNKNOWN CMD %d", cmd.cmd);
                        retval = noErr;
                }
            }
            break;
    }
    return retval;
}

OSErr Executor::C_SndChannelStatus(SndChannelPtr chanp, INTEGER length,
                                   SCStatusPtr statusp)
{
    OSErr ret;

    warning_sound_log("chanp %p", chanp);

    switch(ROMlib_PretendSound)
    {
        default:
        case soundoff:
            ret = notEnoughHardware;
            break;
        case soundpretend:
            ret = noErr;
            break;
        case soundon:
            if(length < (int)sizeof(*statusp))
                ret = paramErr;
            else
            {
                statusp->scStartTime = 0;
                statusp->scEndTime = 0;
                statusp->scCurrentTime = 0;
                statusp->scChannelBusy = (SND_CHAN_CMDINPROG_P(chanp)
                                          || !qempty_p(chanp));
                statusp->scChannelPaused = 0; /* FIXME */
                statusp->scChannelAttributes = 0x80; /* FIXME */
                statusp->scCPULoad = 0;
                ret = noErr;
            }
            break;
    }

    return ret;
}

OSErr Executor::C_SndControl(INTEGER id, SndCommand *cmdp)
{
    OSErr retval;
#if defined(OLD_BROKEN_NEXTSTEP_SOUND)
    Handle h;
    SignedByte state;
#endif /* defined(OLD_BROKEN_NEXTSTEP_SOUND) */

    warning_sound_log("");

    switch(ROMlib_PretendSound)
    {
        default:
        case soundoff:
            retval = notEnoughHardware;
            break;
        case soundpretend:
            retval = noErr;
            break;
#if defined(OLD_BROKEN_NEXTSTEP_SOUND)
        case soundon:
            h = GetResource("snth"_4, id);
            if(!h)
                retval = resProblem;
            else
            {
                LoadResource(h);
                state = HGetState(h);
                callasynth((SndChannelPtr)0, cmdp, (ModifierStubPtr)0);
                HSetState(h, state);
                retval = noErr;
            }
            break;
#endif
    }
    return retval;
}

OSErr Executor::C_SndDisposeChannel(SndChannelPtr chanp, Boolean quitnow)
{
    OSErr retval;
    SndCommand cmd;
    GUEST<SndChannelPtr> *pp;

    warning_sound_log("chanp %p quitnow %d", chanp, quitnow);

    if(!chanp)
        return badChannel;

    switch(ROMlib_PretendSound)
    {
        default:
        case soundoff:
            retval = notEnoughHardware;
            break;
        case soundpretend:
            retval = noErr;
            break;

        case soundon:
            retval = noErr;
            if(quitnow)
            {
                cmd.cmd = flushCmd;
                cmd.param1 = 0;
                cmd.param2 = 0;
                retval = SndDoImmediate(chanp, &cmd);
            }
            if(retval == noErr)
            {
                cmd.cmd = quietCmd;
                retval = SndDoImmediate(chanp, &cmd);
            }
#if 0
	vchanp = chanp;
	while (vchanp->flags & CHAN_BUSY_FLAG) {
           // ###
	}
	nextmp = chanp->firstMod;
	while ((mp = nextmp)) {
	    nextmp = mp->nextStub;
	    cmd.cmd = freeCmd;
	    callasynth(chanp, &cmd, mp);
	    if (mp->flags & MOD_SYNTH_FLAG) {
		h = RecoverHandle((Ptr) mp->code);
		HSetState(h, mp->hState);
	    }
	    DisposePtr((Ptr) mp);
	}
#endif

            for(pp = &allchans;
                *pp && *pp != chanp;
                pp = (GUEST<SndChannelPtr> *)& (*pp)->nextChan)
                ;
            if(*pp)
            {
                *pp = chanp->nextChan;
                DisposePtr((Ptr)chanp->firstMod);
                if(chanp->flags & CHAN_ALLOC_FLAG)
                    DisposePtr((Ptr)chanp);
            }
            else
            {
                warning_sound_log("NON-EXISTENT CHAN");
                retval = badChannel;
            }

            break;
    }
    return retval;
}

#if defined(OLD_BROKEN_NEXTSTEP_SOUND)
void ROMlib_soundcomplete(void *vp)
{
    SndChannelPtr chanp;

    chanp = vp;
    if(chanp->flags & CHAN_IMMEDIATE_FLAG)
        chanp->flags &= ~CHAN_IMMEDIATE_FLAG;
    else
    {
        chanp->qHead = chanp->qHead + 1;
        if((unsigned short)chanp->qHead == (unsigned short)stdQLength)
            chanp->qHead = 0;
        if(chanp->qHead == chanp->qTail)
            chanp->flags &= ~CHAN_BUSY_FLAG;
        else
            recsndcmd(chanp, &chanp->queue[chanp->qHead], chanp->firstMod);
    }
}
#endif
