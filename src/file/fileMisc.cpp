/* Copyright 1986-1997 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/* Forward declarations in FileMgr.h (DO NOT DELETE THIS LINE) */

#include <base/common.h>
#include <QuickDraw.h>
#include <FileMgr.h>
#include <OSEvent.h>
#include <VRetraceMgr.h>
#include <OSUtil.h>
#include <MemoryMgr.h>
#include <StdFilePkg.h>

#include <hfs/hfs.h>
#include <file/file.h>
#include <hfs/futzwithdosdisks.h>
#include <print/ini.h>
#include <util/string.h>
#include <rsys/segment.h>
#include <rsys/paths.h>
#include <util/macstrings.h>


#include <ctype.h>
#include <algorithm>

#include <cmrc/cmrc.hpp>

CMRC_DECLARE(resources);

using namespace Executor;

namespace Executor
{
std::string ROMlib_appname;
}


#if !defined(NDEBUG)
void Executor::fs_err_hook(OSErr err)
{
}
#endif


Byte
Executor::open_attrib_bits(LONGINT file_id, VCB *vcbp, GUEST<INTEGER> *refnump)
{
    Byte retval;
    int i;

    retval = 0;
    *refnump = 0;
    for(i = 0; i < NFCB; i++)
    {
        if(ROMlib_fcblocks[i].fdfnum == file_id
           && ROMlib_fcblocks[i].fcvptr == vcbp)
        {
            if(*refnump == 0)
                *refnump = i * 94 + 2;
            if(ROMlib_fcblocks[i].fcflags & fcfisres)
                retval |= ATTRIB_RESOPEN;
            else
                retval |= ATTRIB_DATAOPEN;
        }
    }
    if(retval & (ATTRIB_RESOPEN | ATTRIB_DATAOPEN))
        retval |= ATTRIB_ISOPEN;
    return retval;
}

/* NOTE:  calling most of the routines here is a sign that the user may
	  be depending on the internal layout of things a bit too much */

void Executor::C_FInitQueue() /* IMIV-128 */
{
}

QHdrPtr Executor::C_GetFSQHdr() /* IMIV-175 */
{
    return (&LM(FSQHdr)); /* in UNIX domain, everything is synchronous */
}

QHdrPtr Executor::C_GetVCBQHdr() /* IMIV-178 */
{
    return (&LM(VCBQHdr));
}

QHdrPtr Executor::C_GetDrvQHdr() /* IMIV-182 */
{
    return (&LM(DrvQHdr));
}

/*
 * NOTE:  This is *the* PCBGetFCBInfo that we use whether
 *	  we're looking at hfs or ufs stuff.
 */

OSErr Executor::PBGetFCBInfo(FCBPBPtr pb, Boolean async)
{
    filecontrolblock *fcbp, *efcbp;
    INTEGER i;

    if((i = pb->ioFCBIndx) > 0)
    {
        fcbp = (filecontrolblock *)(LM(FCBSPtr) + sizeof(INTEGER));
        efcbp = (filecontrolblock *)(LM(FCBSPtr) + *(GUEST<INTEGER> *)LM(FCBSPtr));
        if(pb->ioVRefNum < 0)
        {
            for(; fcbp != efcbp; fcbp++)
                if(fcbp->fcbFlNum && fcbp->fcbVPtr->vcbVRefNum == pb->ioVRefNum && --i <= 0)
                    break;
        }
        else if(pb->ioVRefNum == 0)
        {
            for(; fcbp != efcbp && (fcbp->fcbFlNum == 0 || --i > 0); fcbp++)
                ;
        }
        else /* if (pb->ioVRefNum > 0 */
        {
            for(; fcbp != efcbp; fcbp++)
                if(fcbp->fcbFlNum && fcbp->fcbVPtr->vcbDrvNum == pb->ioVRefNum && --i <= 0)
                    break;
        }
        if(fcbp == efcbp)
            PBRETURN(pb, fnOpnErr);
        pb->ioRefNum = (char *)fcbp - (char *)LM(FCBSPtr);
    }
    else
    {
        fcbp = ROMlib_refnumtofcbp(pb->ioRefNum);
        if(!fcbp)
            PBRETURN(pb, rfNumErr);
    }
    if(pb->ioNamePtr)
        str255assign(pb->ioNamePtr, fcbp->fcbCName);
    pb->ioFCBFlNm = fcbp->fcbFlNum;
    pb->ioFCBFlags = (fcbp->fcbMdRByt << 8) | (unsigned char)fcbp->fcbTypByt;
    pb->ioFCBStBlk = fcbp->fcbSBlk;
    pb->ioFCBEOF = fcbp->fcbEOF;
    pb->ioFCBCrPs = fcbp->fcbCrPs; 
    pb->ioFCBPLen = fcbp->fcbPLen;
    pb->ioFCBVRefNum = fcbp->fcbVPtr->vcbVRefNum;
    if(pb->ioFCBIndx <= 0 || pb->ioVRefNum == 0)
        pb->ioVRefNum = pb->ioFCBVRefNum;
    pb->ioFCBClpSiz = fcbp->fcbClmpSize;
    pb->ioFCBParID = fcbp->fcbDirID;
    PBRETURN(pb, noErr);
}

std::string
Executor::expandPath(std::string name)
{
    if(name.empty())
        return "";

    switch(name[0])
    {
        case '~':
        {
#ifdef _WIN32
            auto home = getenv("USERPROFILE");
#else
            auto home = getenv("HOME");
#endif
            if(home)
            {
                name = home + name.substr(1);
                break;
            }
        }
        break;
    }

#if defined(_WIN32)
    std::replace(name.begin(), name.end(), '/', '\\');
#endif

    return name;
}

StringPtr Executor::ROMlib_exefname;

std::string Executor::ROMlib_ConfigurationFolder;
static std::string ROMlib_SystemFolder;
fs::path Executor::ROMlib_DirectoryMap;
static std::string ROMlib_MacVolumes;
std::string Executor::ROMlib_ScreenDumpFile;
static std::string ROMlib_OffsetFile;

LONGINT Executor::ROMlib_magic_offset = -1;

static void
skip_comments(FILE *fp)
{
    int c;

    while((c = getc(fp)) == '#')
    {
        while(c != '\n')
            c = getc(fp);
    }
    ungetc(c, fp);
}

static void
parse_offset_file(void)
{
    FILE *fp;

    fp = fopen(ROMlib_OffsetFile.c_str(), "r");
    if(!fp)
    {
#if 0
      warning_unexpected ("Couldn't open \"%s\"", ROMlib_OffsetFile);
#endif
    }
    else
    {
        int n_found;

        skip_comments(fp);
        n_found = fscanf(fp, "0x%08x", &ROMlib_magic_offset);
        if(n_found != 1)
            warning_unexpected("n_found = %d", n_found);
        fclose(fp);
    }
}

static bool
is_unix_path(const char *pathname)
{
    bool retval;

#if defined(_WIN32)
    if(pathname[0] && pathname[1] == ':' && (pathname[2] == '/' || pathname[2] == '\\'))
        pathname += 3;
#endif
    retval = strchr(pathname, ':') == 0;
    return retval;
}

std::optional<FSSpec> Executor::cmdlinePathToFSSpec(const std::string& path)
{
    if(is_unix_path(path.c_str()))
    {
        if(auto native = nativePathToFSSpec(path))
            return native;
    }

    VCBExtra *vcbp;

    std::string swappedString = path;
    for(char& c : swappedString)
    {
        if(c == ':')
            c = '/';
        else if(c == '/')
            c = ':';
    }

    std::vector<mac_string> elements;
    
    for(auto sep = swappedString.begin();
        sep != swappedString.end();
        )
    {            
        auto p = sep;
        sep = std::find(p, swappedString.end(), '/');

        if(p != sep)
            elements.push_back(toMacRomanFilename(std::string(p, sep)));

        if(sep != swappedString.end())
            ++sep;
    }

    if(elements.empty())
        return {};

    for(vcbp = (VCBExtra *)LM(VCBQHdr).qHead; vcbp; vcbp = (VCBExtra *)vcbp->vcb.qLink)
    {
        if(mac_string_view(vcbp->vcb.vcbVN) == elements[0])
        {
            long dirID = 2;
            
            for(int i = 1; i < elements.size() -1; i++)
            {
                CInfoPBRec pb;
                pb.dirInfo.ioCompletion = nullptr;
                pb.dirInfo.ioVRefNum = vcbp->vcb.vcbVRefNum;
                pb.dirInfo.ioDrDirID = dirID;
                pb.dirInfo.ioFDirIndex = 0;

                unsigned char strbuf[64];
                std::copy(elements[i].begin(), elements[i].end(), strbuf+1);
                strbuf[0] = elements[i].size();
                
                pb.dirInfo.ioNamePtr = strbuf;
                OSErr err = PBGetCatInfoSync(&pb);
                if(err != noErr)
                    return {};

                dirID = pb.dirInfo.ioDrDirID;
            }

            FSSpec spec;
            spec.vRefNum = vcbp->vcb.vcbVRefNum;
            spec.parID = dirID;

            if(elements.size() >= 2)
            {
                std::copy(elements.back().begin(), elements.back().end(), spec.name+1);
                spec.name[0] = elements.back().size();
            }
            else
                spec.name[0] = 0;
            return spec;
        }
    }
    return {};
}

template<typename F>
static void ForEachPath(std::string_view macVolumes, F f)
{
    std::string::size_type p = 0, q;
    
    while((q = macVolumes.find_first_of(":;",  p)) != std::string::npos)
    {
        if(auto s = macVolumes.substr(p, q-p); !s.empty())
            f(s);
        p = q + 1;
    }
    if(auto s = macVolumes.substr(p); !s.empty())
        f(s);
}

static void MountMacVolumes(std::string macVolumes)
{
    ForEachPath(macVolumes, [](std::string_view pathstr) {
        fs::path path(expandPath(std::string(pathstr)));
        GUEST<LONGINT> m;

        if(fs::is_directory(path))
        {
            for(auto& file : fs::directory_iterator(path))
                if(!fs::is_directory(file))
                    ROMlib_openharddisk(file.path().string().c_str(), &m);
        }
        else
            ROMlib_openharddisk(path.string().c_str(), &m);
    });

    futzwithdosdisks();
}

void Executor::InitSystemFolder(std::string systemFolder)
{
    CInfoPBRec cpb;

    unsigned char nameBuf[64];
    if(auto sysFolderSpec = cmdlinePathToFSSpec(systemFolder))
    {
        mac_string systemFilePath = 
            mac_string((unsigned char*)":") + 
            mac_string(mac_string_view(sysFolderSpec->name)) + 
            (unsigned char*)":" + 
            mac_string(mac_string_view((unsigned char*)SYSMACNAME));
        cpb.hFileInfo.ioNamePtr = assignPString(nameBuf, systemFilePath, 63);
        cpb.hFileInfo.ioVRefNum = sysFolderSpec->vRefNum;
        cpb.hFileInfo.ioDirID = sysFolderSpec->parID;            
    }
    else
    {
        fprintf(stderr, "Couldn't find '%s'\n", systemFolder.c_str());
        exit(1);
    }
    
    cpb.hFileInfo.ioFDirIndex = 0;
    if(PBGetCatInfo(&cpb, false) == noErr)
    {
        WDPBRec wpb;
        wpb.ioNamePtr = 0;
        wpb.ioVRefNum = cpb.hFileInfo.ioVRefNum;
        wpb.ioWDProcID = "unix"_4;
        wpb.ioWDDirID = cpb.hFileInfo.ioFlParID;
        if(PBOpenWD(&wpb, false) == noErr)
            LM(BootDrive) = wpb.ioVRefNum;
    }
    else
    {
        fprintf(stderr, "Couldn't locate System: '%s'\n", ROMlib_SystemFolder.c_str());
        exit(1);
    }
}

void Executor::InitPaths()
{
    auto initpath = [](const char *varname, const char *defval) {
        if(auto v = getenv(varname))
            return expandPath(v);
        else
            return expandPath(defval);
    };

    ROMlib_ConfigurationFolder = initpath("Configuration", "~/.executor/Configuration");
    ROMlib_SystemFolder = initpath("SystemFolder", "~/.executor/System Folder");
    ROMlib_DirectoryMap = initpath("ExecutorDirectoryMap", "~/.executor/cnidmap1");
    ROMlib_MacVolumes = initpath("MacVolumes", "~/.executor/images");
    ROMlib_ScreenDumpFile = initpath("ScreenDumpFile", "/tmp/excscrn*.tif");
    ROMlib_OffsetFile = initpath("OffsetFile", "~/.executor/offset_file");
    ROMlib_PrintersIni = initpath("PrintersIni", "~/.executor/printers.ini");
    ROMlib_PrintDef = initpath("PrintDef", "~/.executor/printdef.ini");

#if !defined(LITTLEENDIAN)
    ROMlib_DirectoryMap += "-be";
#endif /* !defined(LITTLEENDIAN) */
    if constexpr(sizeof(void*) == 4)
        ROMlib_DirectoryMap += "32";

    auto efs = cmrc::resources::get_filesystem();
    fs::path systemFolder = ROMlib_SystemFolder;

    auto copyfile = [&](fs::path dest, auto from) {
        fs::ofstream out(dest, std::ios::binary);
        std::ostreambuf_iterator<char> begin_dest(out);
        std::copy(from.begin(), from.end(), begin_dest);
    };

    auto ensureFile = [&](fs::path dest) {
        if(!fs::exists(dest))
        {
            copyfile(dest, efs.open(dest.filename().string()));
            if(efs.is_file(dest.filename().string() + ".ad"))
                copyfile(dest.parent_path() / ("%" + dest.filename().string()),
                            efs.open(dest.filename().string() + ".ad"));
        }
    };

    fs::create_directories(systemFolder);
    fs::create_directories(systemFolder / "Preferences");
    ensureFile(systemFolder / "System");
    ensureFile(systemFolder / "Browser");
    ensureFile(systemFolder / "Printer");
    ensureFile(systemFolder / "godata.sav");
    ensureFile(ROMlib_PrintDef);
    ensureFile(ROMlib_PrintersIni);
    fs::create_directories(ROMlib_ConfigurationFolder);
}

void Executor::ROMlib_fileinit() /* INTERNAL */
{
    GUEST<THz> savezone;

    LM(CurDirStore) = 2;
    memset(&LM(DrvQHdr), 0, sizeof(LM(DrvQHdr)));
    memset(&LM(VCBQHdr), 0, sizeof(LM(VCBQHdr)));
    memset(&LM(FSQHdr), 0, sizeof(LM(FSQHdr)));
    LM(DefVCBPtr) = 0;
    LM(FSFCBLen) = 94;

    savezone = LM(TheZone);
    LM(TheZone) = LM(SysZone);
    LM(FCBSPtr) = NewPtr((Size)sizeof(fcbhidden));
    ((fcbhidden *)LM(FCBSPtr))->nbytes = sizeof(fcbhidden);

    for(int i = 0; i < NFCB; i++)
    {
        ROMlib_fcblocks[i].fdfnum = 0;
        ROMlib_fcblocks[i].fcleof = i + 1;
        ROMlib_fcblocks[i].fcbTypByt = 0;
        ROMlib_fcblocks[i].fcbSBlk = 0;
        ROMlib_fcblocks[i].fcPLen = 0;
        ROMlib_fcblocks[i].fcbCrPs = 0;
        ROMlib_fcblocks[i].fcbBfAdr = 0;
        ROMlib_fcblocks[i].fcbFlPos = 0;
        ROMlib_fcblocks[i].fcbClmpSize = 1;
        ROMlib_fcblocks[i].fcbFType = 0;
        ROMlib_fcblocks[i].zero[0] = 0;
        ROMlib_fcblocks[i].zero[1] = 0;
        ROMlib_fcblocks[i].zero[2] = 0;
        ROMlib_fcblocks[i].fcname[0] = 0;
    }
    ROMlib_fcblocks[NFCB - 1].fcleof = -1;

#define NWDENTRIES 40
    INTEGER wdlen = NWDENTRIES * sizeof(wdentry) + sizeof(INTEGER);
    LM(WDCBsPtr) = NewPtr((Size)wdlen);
    LM(TheZone) = savezone;
    memset(LM(WDCBsPtr), 0, wdlen);
    *(GUEST<INTEGER> *)LM(WDCBsPtr) = wdlen;

    InitPaths();

    parse_offset_file();
    ROMlib_hfsinit();
    MountLocalVolumes();

    MountMacVolumes(ROMlib_MacVolumes);

    InitSystemFolder(ROMlib_SystemFolder);
}

fcbrec *
Executor::PRNTOFPERR(INTEGER prn, OSErr *errp)
{
    fcbrec *retval;
    OSErr err;

    if(prn < 0 || prn >= *(GUEST<INTEGER> *)LM(FCBSPtr) || (prn % 94) != 2)
    {
        retval = 0;
        err = rfNumErr;
    }
    else
    {
        retval = (fcbrec *)((char *)LM(FCBSPtr) + prn);
        if(!retval->fdfnum)
        {
            retval = 0;
            err = rfNumErr;
        }
        else
            err = noErr;
    }
    *errp = err;
    return retval;
}
