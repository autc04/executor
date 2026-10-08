#if !defined(__RSYS_SOUNDOPTS__)
#define __RSYS_SOUNDOPTS__

#include <SoundMgr.h>
/* to get extern for `ROMlib_PretendSound' */
#include <prefs/prefs.h>


namespace Executor
{
#if defined(MACOSX_)
extern void ROMlib_outbuffer(char *buf, LONGINT nsamp, LONGINT rate, void *chanp);
extern void ROMlib_callcompletion(void *chanp);
#else /* !MACOSX_ */
#define ROMlib_outbuffer(buf, nsamp, rate, chanp) \
    ROMlib_soundcomplete(chanp)
#define ROMlib_callcompletion(chanp) \
    ROMlib_soundcomplete(chanp)
#endif /* !MACOSX_ */

extern void ROMlib_soundcomplete(void *chanp);

#define CHAN_ALLOC_FLAG (1 << 0)
#define CHAN_BUSY_FLAG (1 << 1)
#define CHAN_IMMEDIATE_FLAG (1 << 2)
#define CHAN_CMDINPROG_FLAG (1 << 3)
#define CHAN_DBINPROG_FLAG (1 << 4)


extern void clear_pending_sounds(void);

/* patl stuff */

extern GUEST<SndChannelPtr> allchans;

typedef uint64_t snd_time;

#define SND_PROMOTE(x) (((snd_time)x) << (4 * sizeof(snd_time)))
#define SND_DEMOTE(x) (((snd_time)x) >> (4 * sizeof(snd_time)))

typedef LONGINT TimeL;

// ###autc04 TODO: snd_time? 64 bit values on the mac?
//                 is this an internal executor-only struct?

struct ModifierStub;
typedef ModifierStub *ModifierStubPtr;

using snthfp = UPP<Boolean(SndChannelPtr, SndCommand *, ModifierStubPtr)>;
struct ModifierStub
{
    GUEST<struct _ModifierStub *> nextStub;
    GUEST<snthfp> code;
    LONGINT userInfo;
    TimeL count;
    TimeL every;
    SignedByte flags;
    SignedByte hState;
    snd_time current_start;
    snd_time time;
    GUEST<uint8_t> prev_samp;
    SndDoubleBufferHeader *dbhp;
    int current_db;
};

#define SND_CHAN_FIRSTMOD(c) ((ModifierStubPtr)c->firstMod)
#define SND_CHAN_CURRENT_START(c) (SND_CHAN_FIRSTMOD(c)->current_start)
#define SND_CHAN_TIME(c) (SND_CHAN_FIRSTMOD(c)->time)
#define SND_CHAN_PREV_SAMP(c) (SND_CHAN_FIRSTMOD(c)->prev_samp)
#define SND_CHAN_CURRENT_DB(c) (SND_CHAN_FIRSTMOD(c)->current_db)
#define SND_CHAN_DBHP(c) (SND_CHAN_FIRSTMOD(c)->dbhp)

#define SND_CHAN_CMDINPROG_P(c) ((c)->flags & CHAN_CMDINPROG_FLAG)
#define SND_CHAN_DBINPROG_P(c) ((c)->flags & CHAN_DBINPROG_FLAG)

extern syn68k_addr_t sound_callback(syn68k_addr_t, void *);

extern bool sound_disabled_p;

extern int ROMlib_get_snd_cmds(Handle sndh, SndCommand **cmdsp);

Boolean C_snth5(SndChannelPtr, SndCommand *, ModifierStubPtr);
}

#endif /* !defined(__RSYS_SOUNDOPTS__) */
