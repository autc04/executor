/* Copyright 1994, 1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <base/common.h>

#include <stdarg.h>

#include <FileMgr.h>
#include <AliasMgr.h>
#include <MemoryMgr.h>
#include <ToolboxUtil.h>

#include <file/file.h>
#include <hfs/hfs.h>
#include <util/string.h>

#include <rsys/alias.h>

#include <algorithm>

using namespace Executor;

/* NOTE: if we want to be more like the Mac, we should have a 'fld#',0
   resource that will have in it: type, four bytes of 0, pascal string,
   potential padding to even things up, type, four bytes of 0, ... */

static const char *
find_sub_dir(OSType folderType)
{
    typedef struct
    {
        OSType type;
        const char *name;
    } sys_sub_dir_match_t;

    static sys_sub_dir_match_t matches[] = {
        {
            kPrintMonitorDocsFolderType, "PrintMonitor Documents",
        },
        {
            kStartupFolderType, "Startup Items",
        },
        {
            kAppleMenuFolderType, "Apple Menu Items",
        },
        {
            kExtensionFolderType, "Extensions",
        },
        {
            kPreferencesFolderType, "Preferences",
        },
        {
            kControlPanelFolderType, "Control Panels",
        },
        {
            kFontFolderType, "Fonts",
        },
    };
    int i;
    const char *retval;

    for(i = 0; i < (int)std::size(matches) && matches[i].type != folderType; ++i)
        ;
    if(i < (int)std::size(matches))
        retval = matches[i].name;
    else
        retval = 0;
    return retval;
}

static OSErr
get_sys_vref_and_dirid(INTEGER *sys_vrefp, LONGINT *sys_diridp)
{
    OSErr err;
    WDPBRec wdp;

    wdp.ioVRefNum = LM(BootDrive);
    wdp.ioWDIndex = 0;
    wdp.ioNamePtr = nullptr;
    err = PBGetWDInfo(&wdp, false);
    if(err == noErr)
    {
        *sys_vrefp = wdp.ioWDVRefNum;
        *sys_diridp = wdp.ioWDDirID;
    }
    return err;
}

static OSErr
test_directory(INTEGER vref, LONGINT dirid, const char *sub_dirp,
               LONGINT *new_idp)
{
    OSErr err;
    CInfoPBRec cpb;
    Str255 file_name;

    str255_from_c_string(file_name, sub_dirp);
    cpb.hFileInfo.ioNamePtr = (StringPtr)file_name;
    cpb.hFileInfo.ioVRefNum = vref;
    cpb.hFileInfo.ioFDirIndex = 0;
    cpb.hFileInfo.ioDirID = dirid;
    err = PBGetCatInfo(&cpb, false);
    if(err == noErr && !(cpb.hFileInfo.ioFlAttrib & ATTRIB_ISADIR))
        err = dupFNErr;
    if(err == noErr)
        *new_idp = cpb.dirInfo.ioDrDirID;
    return err;
}


static OSErr
get_tmp_vref_and_dirid(INTEGER vref, INTEGER *tmp_vrefp, LONGINT *tmp_diridp)
{
    // FIXME: get proper paths
    // FIXME: temp directory for other volumes?
#ifdef _WIN32
    auto spec = nativePathToFSSpec("C:/temp");
#else
    auto spec = nativePathToFSSpec("/tmp");
#endif
    if(!spec)
        return fnfErr;

    CInfoPBRec cpb;

    cpb.hFileInfo.ioNamePtr = spec->name;
    cpb.hFileInfo.ioVRefNum = spec->vRefNum;
    cpb.hFileInfo.ioFDirIndex = 0;
    cpb.hFileInfo.ioDirID = spec->parID;
    OSErr err = PBGetCatInfo(&cpb, false);
    if(err == noErr && !(cpb.hFileInfo.ioFlAttrib & ATTRIB_ISADIR))
        err = dupFNErr;
    if(err == noErr)
    {
        *tmp_diridp = cpb.dirInfo.ioDrDirID;
        *tmp_vrefp = spec->vRefNum;
    }
    return err;
}
static OSErr
create_directory(INTEGER sys_vref, LONGINT sys_dirid, const char *sub_dirp,
                 LONGINT *new_idp)
{
    warning_unimplemented("");
    return paramErr;
}

OSErr Executor::C_FindFolder(int16_t vRefNum, OSType folderType,
                             Boolean createFolder,
                             GUEST<int16_t> *foundVRefNum,
                             GUEST<int32_t> *foundDirID)
{
    OSErr retval;
    const char *sub_dir;

    sub_dir = find_sub_dir(folderType);
    if(sub_dir)
    {
        INTEGER sys_vref;
        LONGINT sys_dirid, new_id;

        retval = get_sys_vref_and_dirid(&sys_vref, &sys_dirid);
        if(retval == noErr)
        {
            retval = test_directory(sys_vref, sys_dirid, sub_dir, &new_id);
            if(retval == fnfErr && createFolder)
                retval = create_directory(sys_vref, sys_dirid, sub_dir, &new_id);
            if(retval == noErr)
            {
                *foundVRefNum = sys_vref;
                *foundDirID = new_id;
            }
        }
    }
    else
        switch(folderType)
        {
            case kSystemFolderType:
            {
                INTEGER sys_vref;
                LONGINT sys_dirid;

                retval = get_sys_vref_and_dirid(&sys_vref, &sys_dirid);
                if(retval == noErr)
                {
                    /* NOTE: IMVI 9-44 tells us to not create System Folder if it
	       doesn't already exist */
                    *foundVRefNum = sys_vref;
                    *foundDirID = sys_dirid;
                }
            }
            break;
            case kDesktopFolderType:
            case kTrashFolderType:
            case kWhereToEmptyTrashFolderType:
            case kTemporaryFolderType:
                /* These cases aren't properly handled, but they should allow some apps
   to get further */
                {
                    INTEGER tmp_vref;
                    LONGINT tmp_dirid;

                    retval = get_tmp_vref_and_dirid(vRefNum, &tmp_vref, &tmp_dirid);
                    warning_unimplemented("poorly implemented");
                    if(retval == fnfErr && createFolder)
                        warning_unimplemented("Didn't attempt to create folder");
                    if(retval == noErr)
                    {
                        *foundVRefNum = tmp_vref;
                        *foundDirID = tmp_dirid;
                    }
                }
                break;
            default:
                warning_unexpected("unknown folderType");
                retval = fnfErr;
                break;
        }
    return retval;
}

OSErr Executor::C_NewAlias(FSSpecPtr fromFile, FSSpecPtr target,
                           GUEST<AliasHandle> *alias)
{
    OSErr retval;

    *alias = 0;
    warning_unimplemented("poorly implemented");

    retval = NewAliasMinimal(target, alias);
    return retval;
}

OSErr Executor::C_UpdateAlias(FSSpecPtr fromFile, FSSpecPtr target,
                              AliasHandle alias, Boolean *wasChanged)
{
    warning_unimplemented("");
    return paramErr;
}

enum
{
    FULL_PATH_TAG = 0x0002,
    TAIL_TAG = 0x0009,
};

/*
 * ResolveAlias is just a stub so we can recover the fsspecs that are stored
 * in the AppleEvent that is constructed as part of the process of launching
 * another application.  This stub doesn't look at fromFile, doesn't consider
 * the fact that the alias may point to an alias.  It won't work if a full-path
 * alias is supplied either.
 */

OSErr Executor::C_ResolveAlias(FSSpecPtr fromFile, AliasHandle alias,
                               FSSpecPtr target, Boolean *wasAliased)
{
    OSErr retval;
    alias_head_t *headp;
    Str255 volname;
    FSSpec fs;
    HParamBlockRec pb;

    warning_unimplemented("stub for Launch WON'T WORK WITH FULL PATH SPEC");
    retval = noErr;
    headp = (decltype(headp))*alias;
    str255assign(volname, headp->volumeName);
    fs.parID = headp->ioDirID; /* NOT VALID IF THIS IS A FULL PATH SPEC */
    str255assign(fs.name, headp->fileName);

    pb.volumeParam.ioNamePtr = (StringPtr)volname;
    pb.volumeParam.ioVolIndex = -1;
    pb.volumeParam.ioVRefNum = 0;
    retval = PBHGetVInfo(&pb, false);
    if(retval == noErr)
    {
        fs.vRefNum = pb.volumeParam.ioVRefNum;
        *wasAliased = false;
        *target = fs;
    }

    return retval;
}

OSErr Executor::C_ResolveAliasFile(FSSpecPtr theSpec,
                                   Boolean resolveAliasChains,
                                   Boolean *targetIsFolder, Boolean *wasAliased)
{
    HParamBlockRec hpb;
    OSErr retval;

    memset(&hpb, 0, sizeof hpb);
    hpb.fileParam.ioNamePtr = (StringPtr)theSpec->name;
    hpb.fileParam.ioDirID = theSpec->parID;
    hpb.fileParam.ioVRefNum = theSpec->vRefNum;
    retval = PBHGetFInfo(&hpb, false);

    if(retval == noErr)
    {
        *targetIsFolder = !!(hpb.fileParam.ioFlAttrib & ATTRIB_ISADIR);
        *wasAliased = false;
    }

    warning_unimplemented("'%.*s' retval = %d, isFolder = %d", theSpec->name[0],
                          theSpec->name + 1, retval, *targetIsFolder);

    return retval;
}

OSErr Executor::C_MatchAlias(FSSpecPtr fromFile, int32_t rulesMask,
                             AliasHandle alias, GUEST<int16_t> *aliasCount,
                             FSSpecArrayPtr aliasList, Boolean *needsUpdate,
                             AliasFilterUPP aliasFilter, Ptr yourDataPtr)
{
    warning_unimplemented("");
    return paramErr;
}

OSErr Executor::C_GetAliasInfo(AliasHandle alias, AliasTypeInfo index,
                               Str63 theString)
{
    warning_unimplemented("");
    return paramErr;
}

static int
EVENUP(int n)
{
    int retval = n;

    if(retval & 1)
        ++retval;
    return retval;
}

#if 0
static OSErr
parse2 (AliasHandle ah, const void *addrs[], int count)
{
  OSErr retval;
  Size size;
	
  size = GetHandleSize ((Handle) ah);
  if (size < sizeof (alias_head_t) + sizeof (INTEGER))
    retval = paramErr;
  else if (count < 0)
    retval = paramErr;
  else
    {
      const alias_head_t *headp;
      const INTEGER *partp, *ep;
		
      headp = (alias_head_t *) *ah;
      partp = (INTEGER *) (&headp[1]);
      ep = (INTEGER *) ((char *) headp + std::min(size, headp->length));
      memset (addrs, 0, count * sizeof addrs[0]);
      for (; partp < ep && *partp != -1;
	   partp = (INTEGER *) ((char *) partp + EVENUP (4 + partp[1])))
	{
	  int part;
			
	  part = *partp;
	  if (part < count)
	    addrs[part] = partp + 1;
	}
      retval = *partp == -1 ? noErr : paramErr;
    }
	
  return retval;
}
#endif

static OSErr
decompose_full_path(INTEGER path_len, Ptr fullPath, Str27 volumeName,
                    Str31 fileName)
{
    OSErr retval;
    char *first_colon;
    char *last_colon;
    char *p, *ep;
    int volume_len;
    int file_len;

    for(p = (char *)fullPath,
    ep = p + path_len, first_colon = 0, last_colon = 0;
        p != ep;
        ++p)
    {
        if(*p == ':')
        {
            if(!first_colon)
                first_colon = p;
            last_colon = p;
        }
    }
    if(!first_colon)
        retval = paramErr;
    else
    {
        volume_len = first_colon - (char *)fullPath;
        file_len = ep - last_colon - 1;
        if(volume_len > 27 || file_len > 31)
            retval = paramErr;
        else
        {
            volumeName[0] = volume_len;
            memcpy(volumeName + 1, fullPath, volume_len);
            fileName[0] = file_len;
            memcpy(fileName + 1, last_colon + 1, file_len);
            retval = noErr;
        }
    }
    return retval;
}

static void
init_head(alias_head_t *headp, Str27 volumeName, Str31 fileName)
{
    memset(headp, 0, sizeof *headp);
    headp->usually_2 = 2;
    memcpy(headp->volumeName, volumeName, volumeName[0] + 1);
    memcpy(headp->fileName, fileName, fileName[0] + 1);
    headp->mystery_words[0] = -1;
    headp->mystery_words[1] = -1;
    headp->mystery_words[3] = 17;
}

static void
init_tail(alias_tail_t *tailp, Str32 zoneName, Str31 serverName,
          Str27 volumeName)
{
    Handle h;

    memset(tailp, 0, sizeof *tailp);
    memcpy(tailp->zone, zoneName, zoneName[0] + 1);
    memcpy(tailp->server, serverName, serverName[0] + 1);
    memcpy(tailp->volumeName, volumeName, volumeName[0] + 1);
    h = (Handle)GetString(-16096);
    if(!h)
        tailp->network_identity_owner_name[0] = 0;
    else
    {
        int name_len;

        name_len = std::min(GetHandleSize(h), 31);
        memcpy(tailp->network_identity_owner_name, *h, name_len);
    }
    tailp->weird_info[0] = 0x00A8;
    tailp->weird_info[1] = 0x6166;
    tailp->weird_info[2] = 0x706D;
    tailp->weird_info[5] = 0x0003;
    tailp->weird_info[6] = 0x0018;
    tailp->weird_info[7] = 0x0039;
    tailp->weird_info[8] = 0x0059;
    tailp->weird_info[9] = 0x0075;
    tailp->weird_info[10] = 0x0095;
    tailp->weird_info[11] = 0x009E;
}

static OSErr
assemble_pieces(GUEST<AliasHandle> *ahp, alias_head_t *headp, int n_pieces, ...)
{
    Size n_bytes_needed;
    va_list va;
    int i;
    Handle h;
    OSErr retval;

    n_bytes_needed = sizeof *headp;
    va_start(va, n_pieces);
    for(i = 0; i < n_pieces; ++i)
    {
        INTEGER tag;
        INTEGER length;
        void *p;

        tag = va_arg(va, int);
        length = va_arg(va, int);
        p = va_arg(va, void *);
        n_bytes_needed += sizeof(INTEGER) + sizeof(INTEGER) + EVENUP(length);
    }
    va_end(va);
    n_bytes_needed += sizeof(INTEGER) + sizeof(INTEGER);
    h = NewHandle(n_bytes_needed);
    if(!h)
        retval = MemError();
    else
    {
        char *op;

        headp->length = n_bytes_needed;
        op = (char *)*h;
        memcpy(op, headp, sizeof(*headp));
        op += sizeof(*headp);
        va_start(va, n_pieces);
        for(i = 0; i < n_pieces; ++i)
        {
            INTEGER tag;
            GUEST<INTEGER> tag_x;
            INTEGER length;
            GUEST<INTEGER> length_x;
            void *p;

            tag = va_arg(va, int);
            tag_x = tag;
            length = va_arg(va, int);
            length_x = length;
            p = va_arg(va, void *);
            memcpy(op, &tag_x, sizeof tag_x);
            op += sizeof tag_x;
            memcpy(op, &length_x, sizeof length_x);
            op += sizeof length_x;
            memcpy(op, p, length);
            op += length;
            if(length & 1)
                *op++ = 0;
        }
        va_end(va);
        memset(op, -1, sizeof(INTEGER));
        op += sizeof(INTEGER);
        memset(op, 0, sizeof(INTEGER));

        *ahp = (AliasHandle)h;
        retval = noErr;
    }
    return retval;
}

/*
FULL_PATH_TAG, path_len, fullPath,
TAIL_TAG     , sizeof (tail)-2, &tail.weird_info)
*/

OSErr Executor::C_NewAliasMinimalFromFullPath(
    INTEGER path_len, Ptr fullPath, Str32 zoneName, Str31 serverName,
    GUEST<AliasHandle> *ahp)
{
    OSErr retval;

    warning_unimplemented("not tested much");
    if(zoneName[0] > 32 || serverName[0] > 31 || !ahp)
        retval = paramErr;
    else
    {
        Str27 volumeName;
        Str63 fileName;

        retval = decompose_full_path(path_len, fullPath, volumeName, fileName);
        if(retval == noErr)
        {
            alias_head_t head;
            alias_tail_t tail;

            init_head(&head, volumeName, fileName);
            if(volumeName[0] < 27)
                head.volumeName[volumeName[0] + 1] = ':';
            head.zero_or_one = 1;
            head.zero_or_neg_one = -1;
            head.ioDirID = -1;
            head.ioFlCrDat = 0;
            init_tail(&tail, zoneName, serverName, volumeName);
            retval = assemble_pieces(ahp, &head, 2,
                                     FULL_PATH_TAG, path_len, (void *)fullPath,
                                     TAIL_TAG, (int)sizeof(tail) - 2,
                                     (void *)&tail.weird_info);
        }
    }

    if(retval != noErr)
        *ahp = nullptr;

    return retval;
}

OSErr Executor::C_NewAliasMinimal(FSSpecPtr fsp, GUEST<AliasHandle> *ahp)
{
    HParamBlockRec hpb;
    OSErr retval;
    Str27 volName;

    warning_unimplemented("not tested much");
    memset(&hpb, 0, sizeof hpb);
    hpb.ioParam.ioNamePtr = &volName[0];
    hpb.ioParam.ioVRefNum = fsp->vRefNum;
    retval = PBHGetVInfo(&hpb, false);
    if(retval == noErr)
    {
        alias_head_t head;

        init_head(&head, volName, fsp->name);
        head.ioVCrDate = hpb.volumeParam.ioVCrDate;
        head.ioVSigWord = hpb.volumeParam.ioVSigWord;
        memset(&hpb, 0, sizeof hpb);
        hpb.ioParam.ioNamePtr = &fsp->name[0];
        hpb.ioParam.ioVRefNum = fsp->vRefNum;
        hpb.fileParam.ioDirID = fsp->parID;
        retval = PBHGetFInfo(&hpb, false);
        if(retval == noErr)
        {
            alias_tail_t tail;
            Handle h;
            Str31 serverName;

            head.ioDirID = hpb.fileParam.ioDirID;
            head.ioFlCrDat = hpb.fileParam.ioFlCrDat;
            head.type_info = hpb.fileParam.ioFlFndrInfo.fdType;
            head.creator = hpb.fileParam.ioFlFndrInfo.fdCreator;
            h = (Handle)GetString(-16413);
            if(!h)
                serverName[0] = 0;
            else
            {
                int len;

                len = std::min(GetHandleSize(h), 32);
                memcpy(serverName, *h, len);
            }
            init_tail(&tail, (StringPtr) "\1*", serverName, volName);
            retval = assemble_pieces(ahp, &head, 1,
                                     TAIL_TAG, (int)sizeof(tail) - 2,
                                     (void *)&tail.weird_info);
        }
    }

    if(retval != noErr)
        *ahp = nullptr;
    return retval;
}
