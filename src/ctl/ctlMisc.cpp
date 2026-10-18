/* Copyright 1986, 1988, 1989, 1990, 1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in ControlMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <WindowMgr.h>
#include <ControlMgr.h>
#include <MemoryMgr.h>

#include <quickdraw/cquick.h>
#include <ctl/ctl.h>
#include <res/resource.h>
#include <rsys/hook.h>
#include <base/functions.impl.h>

/* cheat, and make this a plain ctab, so we can use our old accessor
   macros */

using namespace Executor;

AuxCtlHandle Executor::default_aux_ctl;

#define BLACK_RGB         \
    {                     \
        0            \
        , 0, 0, \
    }
#define WHITE_RGB                                                   \
    {                                                               \
        (unsigned short)0xFFFF                                 \
        , (unsigned short)0xFFFF, (unsigned short)0xFFFF, \
    }

#define LT_BLUISH_RGB                                              \
    {                                                              \
        (unsigned short)0xCCCC                                \
        , (unsigned short)0xCCCC, (unsigned short)0xFFFF \
    }
#define DK_BLUISH_RGB              \
    {                              \
        0x3333                \
        , 0x3333, 0x6666 \
    }

#define DK_GRAY                                                    \
    {                                                              \
        (unsigned short)0x5555                                \
        , (unsigned short)0x5555, (unsigned short)0x5555 \
    }

const ColorSpec Executor::default_ctl_colors[] = {
    { cFrameColor, BLACK_RGB },
    { cBodyColor, WHITE_RGB },
    { cTextColor, BLACK_RGB },
    { cThumbColor, WHITE_RGB },
    { 4, DK_GRAY },
    { cArrowsColorLight, WHITE_RGB },
    { cArrowsColorDark, BLACK_RGB },
    { cThumbLight, WHITE_RGB },
    { cThumbDark, BLACK_RGB },
    /* used same as the w... color component */
    { cHiliteLight, WHITE_RGB },
    { cHiliteDark, BLACK_RGB },
    { cTitleBarLight, WHITE_RGB },
    { cTitleBarDark, BLACK_RGB },
    { cTingeLight, LT_BLUISH_RGB },
    { cTingeDark, DK_BLUISH_RGB },
};

#define KEEP_DEFAULT_CTL_CTAB_AROUND_FOR_OLD_SYSTEM_FILE_USERS

#if defined(KEEP_DEFAULT_CTL_CTAB_AROUND_FOR_OLD_SYSTEM_FILE_USERS)
CTabHandle Executor::default_ctl_ctab;
#endif

void Executor::ctl_color_init(void)
{
    default_aux_ctl = (AuxCtlHandle)NewHandle(sizeof(AuxCtlRec));

#if defined(KEEP_DEFAULT_CTL_CTAB_AROUND_FOR_OLD_SYSTEM_FILE_USERS)
    default_ctl_ctab = (CTabHandle)ROMlib_getrestid("cctb"_4, 0);

    if(!default_ctl_ctab)
    {
        default_ctl_ctab
            = (CTabHandle)NewHandle(CTAB_STORAGE_FOR_SIZE(14));
        CTAB_SIZE(default_ctl_ctab) = 14;
        CTAB_SEED(default_ctl_ctab) = 0;
        CTAB_FLAGS(default_ctl_ctab) = 0;
        memcpy(&CTAB_TABLE(default_ctl_ctab)[0],
               &default_ctl_colors[0],
               15 * sizeof default_ctl_colors[0]);
    }
#endif

    (*default_aux_ctl)->acNext = nullptr;
    (*default_aux_ctl)->acOwner = nullptr;
    (*default_aux_ctl)->acCTable = (CCTabHandle)GetResource("cctb"_4, 0);
    (*default_aux_ctl)->acFlags = 0;
    (*default_aux_ctl)->acReserved = 0;
    (*default_aux_ctl)->acRefCon = 0;
}

GUEST<AuxCtlHandle> *
Executor::lookup_aux_ctl(ControlHandle ctl)
{
    GUEST<AuxCtlHandle> *t;

    for(t = &LM(AuxCtlHead);
        *t && (**t)->acOwner != ctl;
        t = &(**t)->acNext)
        ;
    return t;
}

void Executor::C_SetControlReference(ControlHandle c, LONGINT data) /* IMI-327 */
{
    (*c)->contrlRfCon = data;
}

LONGINT Executor::C_GetControlReference(ControlHandle c) /* IMI-327 */
{
    return (*c)->contrlRfCon;
}

void Executor::C_SetControlAction(ControlHandle c, ControlActionUPP a) /* IMI-328 */
{
    if(a != (ControlActionUPP)-1)
        (*c)->contrlAction = a;
    else
        (*c)->contrlAction = guest_cast<ControlActionUPP>(-1);
}

ControlActionUPP Executor::C_GetControlAction(ControlHandle c) /* IMI-328 */
{
    return (*c)->contrlAction;
}

INTEGER Executor::C_GetControlVariant(ControlHandle c) /* IMV-222 */
{
    AuxCtlHandle h;

    for(h = LM(AuxCtlHead); h != 0 && (*h)->acOwner != c; h = (*h)->acNext)
        ;
    return h != 0 ? (*h)->acFlags.get() : (INTEGER)0;
}

/* according to IM-MTE; this has been renamed
   `GetAuxiliaryControlRecord ()', possibly because of the
   inconsistency below, i can only assume they have the same trap word */
Boolean Executor::C_GetAuxiliaryControlRecord(ControlHandle ctl,
                              GUEST<AuxCtlHandle> *aux_ctl) /* IMV-222 */
{
    /* according to testing on the Mac+
     `GetAuxiliaryControlRecord ()' returns false (not true) and leaves
     aux_ctl untouched; this is not the case of later
     mac version, such as the color classic v7.1 */

    if(!ctl)
    {
        *aux_ctl = default_aux_ctl;
        return true;
    }
    else
    {
        GUEST<AuxCtlHandle> t;

        t = *lookup_aux_ctl(ctl);
        if(t)
        {
            *aux_ctl = t;
            return false;
        }
        else
        {
            *aux_ctl = default_aux_ctl;
            return true;
        }
    }
}

int32_t Executor::ROMlib_ctlcall(ControlHandle c, int16_t i, int32_t l)
{
    Handle defproc;
    int32_t retval;
    ControlDefUPP cp;

    defproc = CTL_DEFPROC(c);

    if(*defproc == nullptr)
        LoadResource(defproc);

    cp = (ControlDefUPP)*defproc;

    ROMlib_hook(ctl_cdefnumber);
    HLockGuard guard(defproc);
    retval = cp(VAR(c), c, i, l);
    
    return retval;
}
