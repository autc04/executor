#if !defined(__RSYS_STDFILE__)
#define __RSYS_STDFILE__

#include <FileMgr.h>
#include <EventMgr.h>
#include <ControlMgr.h>
#include <DialogMgr.h>
#include <file/file.h>


namespace Executor
{
/*
 * Copyright 1989 - 1995 by Abacus Research and Development, Inc.
 * All rights reserved.
 *

 */

#define putPrompt 3
#define putDiskName 4
#define putNmList 8

#define getDiskName 4
#define getDotted 9

#define MICONLETTER 0
#define MICONCFOLDER 1
#define MICONFLOPPY 2
#define MICONOFOLDER 3
#define MICONAPP 4
#define MICONDISK 5

extern void ROMlib_init_stdfile(void);

enum
{
    STANDARD_HEIGHT = 200,
    STANDARD_WIDTH = 348
};

OSErr C_unixmount(CInfoPBRec *cbp);


typedef enum { get,
               put } getorput_t;
typedef enum { original_sf,
               new_sf,
               new_custom_sf } sf_flavor_t;

#if 0
typedef struct
{
} host_spf_reply_block;

extern bool host_has_spfcommon(void);
extern bool host_spfcommon(host_spf_reply_block *replyp,
                           const char *prompt, const char *filename,
                           void *fp, void *filef, int numt,
                           void *tl, getorput_t getorput,
                           sf_flavor_t flavor,
                           void *activeList, void *activateproc,
                           void *yourdatap);
#endif
}

#endif /* !defined(__RSYS_STDFILE__) */
