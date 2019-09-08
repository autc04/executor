#if !defined(_FINDER_H_)
#define _FINDER_H_

/*
 * Copyright 1995 by Abacus Research and Development, Inc.
 * All rights reserved.
 *
 */

#include <ExMacTypes.h>

#define MODULE_NAME Finder
#include <base/api-module.h>

namespace Executor
{
typedef struct DTPBRec
{
    GUEST_STRUCT;
    GUEST<QElemPtr> qLink;
    GUEST<INTEGER> qType;
    GUEST<INTEGER> ioTrap;
    GUEST<Ptr> ioCmdAddr;
    GUEST<ProcPtr> ioCompletion;
    GUEST<OSErr> ioResult;
    GUEST<StringPtr> ioNamePtr;
    GUEST<INTEGER> ioVRefNum;
    GUEST<INTEGER> ioDTRefNum;
    GUEST<INTEGER> ioIndex;
    GUEST<LONGINT> ioTagInfo;
    GUEST<Ptr> ioDTBuffer;
    GUEST<LONGINT> ioDTReqCount;
    GUEST<LONGINT> ioDTActCount;
    GUEST<SignedByte> filler1;
    GUEST<SignedByte> ioIconType;
    GUEST<INTEGER> filler2;
    GUEST<LONGINT> ioDirID;
    GUEST<OSType> ioFileCreator;
    GUEST<OSType> ioFileType;
    GUEST<LONGINT> ioFiller3;
    GUEST<LONGINT> ioDTLgLen;
    GUEST<LONGINT> ioDTPyLen;
    GUEST<INTEGER[14]> ioFiller4;
    GUEST<LONGINT> ioAPPLParID;
} * DTPBRecPtr, *DTPBPtr;

EXTERN_DISPATCHER_TRAP(FSDispatch, 0xA060, D0W);

extern OSErr PBDTGetPath(DTPBPtr dtp);
REGISTER_SUBTRAP2(PBDTGetPath, 0xA260, 0x20, FSDispatch, D0(A0));

extern OSErr PBDTCloseDown(DTPBPtr dtp);
REGISTER_SUBTRAP2(PBDTCloseDown, 0xA260, 0x21, FSDispatch, D0(A0));

extern OSErr PBDTAddIcon(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTAddIcon, DTPBPtr, 0xA260, 0x22, FSDispatch);

extern OSErr PBDTGetIcon(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTGetIcon, DTPBPtr, 0xA260, 0x23, FSDispatch);

extern OSErr PBDTGetIconInfo(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTGetIconInfo, DTPBPtr, 0xA260, 0x24, FSDispatch);

extern OSErr PBDTAddAPPL(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTAddAPPL, DTPBPtr, 0xA260, 0x25, FSDispatch);

extern OSErr PBDTRemoveAPPL(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTRemoveAPPL, DTPBPtr, 0xA260, 0x26, FSDispatch);

extern OSErr PBDTGetAPPL(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTGetAPPL, DTPBPtr, 0xA260, 0x27, FSDispatch);

extern OSErr PBDTSetComment(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTSetComment, DTPBPtr, 0xA260, 0x28, FSDispatch);

extern OSErr PBDTRemoveComment(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTRemoveComment, DTPBPtr, 0xA260, 0x29, FSDispatch);

extern OSErr PBDTGetComment(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTGetComment, DTPBPtr, 0xA260, 0x2A, FSDispatch);

extern OSErr PBDTFlush(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTFlush, DTPBPtr, 0xA260, 0x2B, FSDispatch);

extern OSErr PBDTReset(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTReset, DTPBPtr, 0xA260, 0x2C, FSDispatch);

extern OSErr PBDTGetInfo(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTGetInfo, DTPBPtr, 0xA260, 0x2D, FSDispatch);

extern OSErr PBDTOpenInform(DTPBPtr dtp);
REGISTER_SUBTRAP2(PBDTOpenInform, 0xA260, 0x2E, FSDispatch, D0(A0));

extern OSErr PBDTDelete(DTPBPtr dtp, Boolean async);
FILE_SUBTRAP(PBDTDelete, DTPBPtr, 0xA260, 0x2F, FSDispatch);

static_assert(sizeof(DTPBRec) == 104);
}
#endif
