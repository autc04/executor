/*
 * executor-tool - inspect classic Mac files, resources and 68K code.
 *
 * This is an ordinary host program (not a classic Mac program): console
 * output and files written to the host go through std::cout / std::ofstream
 * rather than the File Manager.
 *
 * It initialises just enough of Executor to get the File Manager, the HFS /
 * LocalVolume back ends and the Resource Manager running, then exposes them
 * through a handful of sub-commands.  68K disassembly reuses cxmon's
 * disassembler, so the output has the usual MacOS syntax and A-line trap
 * numbers are replaced by their names.
 */

#include <base/common.h>
#include <base/traps.h>
#include <mman/mman.h>
#include <rsys/executor.h>
#include <file/file.h>
#include <hfs/hfs.h>
#include <res/resource.h>

#include <MemoryMgr.h>
#include <ResourceMgr.h>
#include <FileMgr.h>
#include <OSUtil.h>

#include <syn68k_public.h>

#include <mon.h>
#include <disass/dis-asm.h>

#include <algorithm>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace Executor;

/* defined in cxmon's mon_disass.cpp; controls generic_print_address()'s
 * substitution of low-memory global names */
extern bool lookup_lowmem;

namespace
{
    bool g_verbose = false;
    bool g_lowmem = false;
    int g_savedFd[2] = { -1, -1 };

    /* Executor's start-up prints machine-level chatter to stdout/stderr
     * ("Volume ... mounted ...", "PBOpenWD: ..."), which would corrupt the
     * tool's own, parseable output.  Silence it while it runs. */
    void silenceOutput()
    {
        if(g_verbose)
            return;
        std::cout.flush();
        std::cerr.flush();
        g_savedFd[0] = ::dup(1);
        g_savedFd[1] = ::dup(2);
        int devnull = ::open("/dev/null", O_WRONLY);
        if(devnull >= 0)
        {
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
            ::close(devnull);
        }
    }

    void unsilenceOutput()
    {
        if(g_savedFd[0] < 0)
            return;
        std::cout.flush();
        std::cerr.flush();
        ::dup2(g_savedFd[0], 1);
        ::dup2(g_savedFd[1], 2);
        ::close(g_savedFd[0]);
        ::close(g_savedFd[1]);
        g_savedFd[0] = g_savedFd[1] = -1;
    }
}

/* ------------------------------------------------------------------ */
/* initialisation                                                      */
/* ------------------------------------------------------------------ */

namespace
{
    /* Must be a *stack* address: InitMemory() uses it to map the main
     * thread's stack into guest memory (block 2 on 64-bit hosts), which is
     * what lets ordinary host stack/temporary pointers be converted to
     * guest pointers. */
    void initExecutor(char *thingOnStack)
    {
        InitMemory(thingOnStack);
        initialize_68k_emulator(nullptr, false, (uint32_t *)SYN68K_TO_US(0), 0);
        traps::init(false);
        InitLowMem();
        EM_A5 = EM_A7 = ptr_to_longint(LM(MemTop) - 4);

        /* Sets up the FCBs, mounts the host root as a LocalVolume, mounts
         * anything in $MacVolumes, and locates the (built-in) System file. */
        ROMlib_fileinit();

        /* Minimal Resource Manager start-up.  We deliberately do *not* call
         * InitResources(), which opens the boot volume's System file and
         * exit()s if it is missing. */
        LM(TopMapHndl) = nullptr;
        LM(SysMapHndl) = nullptr;
        LM(SysMap) = 0;
        LM(CurMap) = 0;
        ROMlib_setreserr(noErr);
        SetResLoad(true);
    }

    [[noreturn]] void die(const std::string &msg)
    {
        std::cerr << "executor-tool: " << msg << "\n";
        std::exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static std::string fourcc(ResType t)
{
    std::string s(4, '.');
    s[0] = (t >> 24) & 0xFF;
    s[1] = (t >> 16) & 0xFF;
    s[2] = (t >> 8) & 0xFF;
    s[3] = t & 0xFF;
    return s;
}

static bool parseResType(const std::string &s, ResType &out)
{
    if(s.size() == 4)
    {
        out = ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint8_t)s[3];
        return true;
    }
    if(s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        out = std::strtoul(s.c_str(), nullptr, 16);
        return true;
    }
    return false;
}

static bool parseId(const std::string &s, int16_t &out)
{
    char *end = nullptr;
    long v = std::strtol(s.c_str(), &end, 0);
    if(!end || *end || v < -32768 || v > 65535)
        return false;
    out = (int16_t)v;
    return true;
}

static bool parseU32(const std::string &s, uint32_t &out)
{
    if(s.empty())
        return false;
    char *end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, 16);   /* default: hex */
    if(!end || *end)
        return false;
    out = (uint32_t)v;
    return true;
}

static std::string pstring(const uint8_t *p)
{
    return std::string((const char *)p + 1, p[0]);
}

static std::optional<FSSpec> resolvePath(const std::string &path)
{
    return cmdlinePathToFSSpec(path);
}

static std::vector<VCBExtra *> volumes()
{
    std::vector<VCBExtra *> v;
    for(auto vcb = (VCBExtra *)LM(VCBQHdr).qHead; vcb; vcb = (VCBExtra *)vcb->vcb.qLink)
        v.push_back(vcb);
    return v;
}

/* Mount a disk image; returns the vRefNum of the newly mounted volume (or
 * std::nullopt). */
static std::optional<INTEGER> mountImage(const std::string &path)
{
    std::set<INTEGER> before;
    for(auto vcb : volumes())
        before.insert(vcb->vcb.vcbVRefNum);

    GUEST<LONGINT> msg;
    ROMlib_openharddisk(path.c_str(), &msg);

    for(auto vcb : volumes())
        if(!before.count(vcb->vcb.vcbVRefNum))
            return vcb->vcb.vcbVRefNum;

    return std::nullopt;
}

/* ------------------------------------------------------------------ */
/* sub-command: volumes                                                */
/* ------------------------------------------------------------------ */

static void cmdVolumes()
{
    for(auto vcb : volumes())
    {
        std::string name = pstring(vcb->vcb.vcbVN);
        std::cout << "'" << name << "'  vRefNum " << (int)vcb->vcb.vcbVRefNum << "\n";
    }
}

/* ------------------------------------------------------------------ */
/* sub-command: ls                                                     */
/* ------------------------------------------------------------------ */

static void listDir(INTEGER vref, LONGINT dirID)
{
    for(int idx = 1;; idx++)
    {
        CInfoPBRec pb = {};
        Str255 name = { 0 };

        pb.dirInfo.ioVRefNum = vref;
        pb.dirInfo.ioDrDirID = dirID;
        pb.dirInfo.ioFDirIndex = idx;
        pb.dirInfo.ioNamePtr = name;

        OSErr err = PBGetCatInfoSync(&pb);
        if(err != noErr)
            break;

        std::string nm = pstring(name);

        if(pb.dirInfo.ioFlAttrib & ATTRIB_ISADIR)
            std::printf("d            %s\n", nm.c_str());
        else
            std::printf("f %8u %8u %s\n",
                        (unsigned)pb.hFileInfo.ioFlLgLen,
                        (unsigned)pb.hFileInfo.ioFlRLgLen,
                        nm.c_str());
    }
}

static void cmdLs(const std::vector<std::string> &args)
{
    if(args.empty())
    {
        for(auto vcb : volumes())
        {
            std::cout << pstring(vcb->vcb.vcbVN) << ":\n";
            listDir(vcb->vcb.vcbVRefNum, 2);
        }
        return;
    }

    const std::string &path = args[0];

    /* A plain host file: try to mount it as a disk image and list the volume
     * that appears. */
    if(path.find(':') == std::string::npos && !fs::is_directory(path) && fs::exists(path))
    {
        auto vref = mountImage(path);
        if(!vref)
            die("'" + path + "' is neither a directory nor a mountable disk image");
        listDir(*vref, 2);
        return;
    }

    /* Otherwise it is a Mac path ("Volume:Folder:...") or a host directory. */
    auto spec = resolvePath(path);
    if(!spec)
        die("cannot resolve '" + path + "'");

    CInfoPBRec pb = {};
    pb.dirInfo.ioNamePtr = spec->name;
    pb.dirInfo.ioVRefNum = spec->vRefNum;
    pb.dirInfo.ioDrDirID = spec->parID;
    pb.dirInfo.ioFDirIndex = 0;
    if(PBGetCatInfoSync(&pb) != noErr)
        die("cannot stat '" + path + "'");
    if(!(pb.dirInfo.ioFlAttrib & ATTRIB_ISADIR))
        die("'" + path + "' is not a directory");

    std::string label = path;
    if(label.empty() || label.back() != ':')
        label += ':';
    std::cout << label << "\n";
    listDir(spec->vRefNum, pb.dirInfo.ioDrDirID);
}

/* ------------------------------------------------------------------ */
/* resource file access                                                */
/* ------------------------------------------------------------------ */

/* RAII wrapper: opens a resource file and closes it again. */
class ResFile
{
public:
    explicit ResFile(const std::string &path)
    {
        auto spec = resolvePath(path);
        if(!spec)
            die("cannot resolve resource file '" + path + "'");
        ref_ = FSpOpenResFile(&*spec, fsRdPerm);
        if(ref_ == -1)
            die("cannot open resource file '" + path + "' (ResError " + std::to_string(ResError()) + ")");
    }

    ~ResFile()
    {
        if(ref_ != -1)
            CloseResFile(ref_);
    }

    INTEGER ref() const { return ref_; }

private:
    INTEGER ref_ = -1;
};

/* ------------------------------------------------------------------ */
/* sub-command: types                                                  */
/* ------------------------------------------------------------------ */

static void cmdTypes(const std::vector<std::string> &args)
{
    if(args.size() != 1)
        die("usage: executor-tool types <file>");

    ResFile rf(args[0]);

    INTEGER n = CountTypes();
    for(INTEGER i = 1; i <= n; i++)
    {
        GUEST<ResType> t;
        GetIndType(&t, i);
        std::printf("%s  %d\n", fourcc(t).c_str(), (int)CountResources(t));
    }
}

/* ------------------------------------------------------------------ */
/* sub-command: res                                                    */
/* ------------------------------------------------------------------ */

static void listResources(ResType t)
{
    INTEGER n = CountResources(t);
    for(INTEGER i = 1; i <= n; i++)
    {
        Handle h = GetIndResource(t, i);
        if(!h)
            continue;

        GUEST<INTEGER> id;
        GUEST<ResType> type;
        Str255 name = { 0 };
        GetResInfo(h, &id, &type, name);

        std::printf("%s %6d  %6ld  %s\n", fourcc(type).c_str(), (int)id,
                    (long)GetResourceSizeOnDisk(h), pstring(name).c_str());
    }
}

static void cmdRes(const std::vector<std::string> &args)
{
    if(args.empty() || args.size() > 2)
        die("usage: executor-tool res <file> [<type>]");

    ResFile rf(args[0]);

    if(args.size() == 2)
    {
        ResType t;
        if(!parseResType(args[1], t))
            die("bad resource type '" + args[1] + "'");
        listResources(t);
    }
    else
    {
        INTEGER n = CountTypes();
        for(INTEGER i = 1; i <= n; i++)
        {
            GUEST<ResType> t;
            GetIndType(&t, i);
            listResources(t);
        }
    }
}

/* ------------------------------------------------------------------ */
/* sub-command: dump                                                   */
/* ------------------------------------------------------------------ */

static std::vector<uint8_t> getResourceBytes(ResType t, int16_t id)
{
    /* NB: GetResource() deliberately redirects 'PACK' lookups (Executor's
     * ResEdit "icky hack"), so use the current-file-only Get1Resource(). */
    Handle h = Get1Resource(t, id);
    if(!h)
        die("resource '" + fourcc(t) + "':" + std::to_string(id) + " not found");

    LoadResource(h);
    HLock(h);
    Size n = GetHandleSize(h);
    const uint8_t *p = (const uint8_t *)*h;
    return std::vector<uint8_t>(p, p + n);
}

static void cmdDump(const std::vector<std::string> &args)
{
    if(args.size() != 4)
        die("usage: executor-tool dump <file> <type> <id> <outfile>");

    ResType t;
    int16_t id;
    if(!parseResType(args[1], t))
        die("bad resource type '" + args[1] + "'");
    if(!parseId(args[2], id))
        die("bad resource id '" + args[2] + "'");

    ResFile rf(args[0]);
    auto bytes = getResourceBytes(t, id);

    std::ofstream out(args[3], std::ios::binary);
    if(!out)
        die("cannot write '" + args[3] + "'");
    out.write((const char *)bytes.data(), bytes.size());
    std::cerr << "wrote " << bytes.size() << " bytes to " << args[3] << "\n";
}

/* ------------------------------------------------------------------ */
/* sub-commands: dis / disres                                          */
/* ------------------------------------------------------------------ */

namespace
{
    std::vector<uint8_t> g_code;

    uint32_t readCode(mon_addr_t adr)
    {
        return adr < g_code.size() ? g_code[adr] : 0;
    }

    std::string g_text;

    int appendText(std::string *out, const char *fmt, ...)
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        if(n > 0)
            out->append(buf, std::min<size_t>(n, sizeof buf - 1));
        return n;
    }

    /* Print addresses numerically.  cxmon's own generic_print_address()
     * substitutes low-memory global names for *any* address in 0x100..0x3000,
     * which for a blob disassembled at offset 0 would mislabel branch targets
     * too; that behaviour is available via --lowmem.  A-line traps are always
     * named (via mon_macos_mode). */
    void printAddressNumber(bfd_vma addr, disassemble_info *info)
    {
        info->fprintf_func(info->stream, "$%08x", (unsigned)addr);
    }

    void disassemble(const std::vector<uint8_t> &code, uint32_t start, uint32_t end)
    {
        if(code.empty())
        {
            std::cerr << "(empty)\n";
            return;
        }

        g_code = code;
        mon_read_byte = readCode;
        mon_macos_mode = true;    /* name A-line traps */

        uint32_t last = code.size() - 1;
        if(end > last)
            end = last;

        for(uint32_t pc = start; pc <= end;)
        {
            g_text.clear();
            disassemble_info info;
            INIT_DISASSEMBLE_INFO(info, &g_text, (fprintf_ftype)appendText);
            if(g_lowmem)
            {
                lookup_lowmem = true;
                info.print_address_func = generic_print_address;
            }
            else
                info.print_address_func = printAddressNumber;

            int n = print_insn_m68k(pc, &info);

            std::printf("%08x: ", pc);
            for(int i = 0; i < 6; i += 2)
                if(n > i)
                    std::printf("%04x ", mon_read_half(pc + i));
                else
                    std::printf("     ");
            if(n == 8)
                std::printf("%04x\t", mon_read_half(pc + 6));
            else if(n > 8)
                std::printf("...\t");
            else
                std::printf("   \t");
            std::puts(g_text.c_str());

            pc += (n > 0 ? n : 2);
        }
    }

    std::vector<uint8_t> readHostFile(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if(!in)
            die("cannot read '" + path + "'");
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
    }

    void parseRange(const std::vector<std::string> &args, size_t first,
                    uint32_t &start, uint32_t &end, size_t size)
    {
        start = 0;
        end = size ? size - 1 : 0;
        if(args.size() > first && !args[first].empty())
            if(!parseU32(args[first], start))
                die("bad start address '" + args[first] + "'");
        if(args.size() > first + 1 && !args[first + 1].empty())
            if(!parseU32(args[first + 1], end))
                die("bad end address '" + args[first + 1] + "'");
    }
}

static void cmdDis(const std::vector<std::string> &args)
{
    if(args.empty() || args.size() > 3)
        die("usage: executor-tool dis <hostfile> [<start>] [<end>]");

    auto code = readHostFile(args[0]);
    uint32_t start, end;
    parseRange(args, 1, start, end, code.size());
    disassemble(code, start, end);
}

static void cmdDisRes(const std::vector<std::string> &args)
{
    if(args.size() < 3 || args.size() > 5)
        die("usage: executor-tool disres <file> <type> <id> [<start>] [<end>]");

    ResType t;
    int16_t id;
    if(!parseResType(args[1], t))
        die("bad resource type '" + args[1] + "'");
    if(!parseId(args[2], id))
        die("bad resource id '" + args[2] + "'");

    ResFile rf(args[0]);
    auto code = getResourceBytes(t, id);

    uint32_t start, end;
    parseRange(args, 3, start, end, code.size());
    disassemble(code, start, end);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage()
{
    std::cout <<
        "usage: executor-tool [--volume <image>]... <command> [args]\n"
        "\n"
        "Inspect classic Mac files, resources and 68K code.  <file> arguments\n"
        "are either host paths or Mac paths ('Volume:Folder:File'; host paths\n"
        "are reachable through the volume named '/').  Disk images can be\n"
        "mounted with --volume (or via the MacVolumes environment variable).\n"
        "\n"
        "commands:\n"
        "  volumes                          list mounted volumes\n"
        "  ls [<path-or-image>]             list a directory (or all volumes)\n"
        "  types <file>                     list resource types and counts\n"
        "  res <file> [<type>]              list resources\n"
        "  dump <file> <type> <id> <out>    write resource data to <out>\n"
        "  dis <hostfile> [<start>] [<end>]          disassemble a host file\n"
        "  disres <file> <type> <id> [<s>] [<e>]     disassemble a resource\n"
        "\n"
        "--lowmem names low-memory globals in address operands (cxmon's behaviour;\n"
        "off by default because it also mislabels branch targets in a raw blob).\n"
        "\n"
        "<type> is 4 characters (e.g. PACK) or 0xRRGGBBAA; <id> is -32768..65535;\n"
        "<start>/<end> are hex byte offsets.\n";
}

int main(int argc, char **argv)
{
    std::vector<std::string> volumesToMount;
    std::vector<std::string> args;

    int i = 1;
    for(; i < argc; i++)
    {
        std::string a = argv[i];
        if(a == "--volume")
        {
            if(i + 1 >= argc)
                die("--volume needs an argument");
            volumesToMount.push_back(argv[++i]);
        }
        else if(a == "--verbose")
            g_verbose = true;
        else if(a == "--lowmem")
            g_lowmem = true;
        else if(a == "-h" || a == "--help")
        {
            usage();
            return 0;
        }
        else
            break;   /* first non-option: the command */
    }

    if(i >= argc)
    {
        usage();
        return 1;
    }

    std::string command = argv[i++];
    for(; i < argc; i++)
        args.push_back(argv[i]);

    char thingOnStack;
    silenceOutput();
    initExecutor(&thingOnStack);

    std::string failedVolume;
    for(const auto &v : volumesToMount)
        if(!mountImage(v))
        {
            failedVolume = v;
            break;
        }
    unsilenceOutput();

    if(!failedVolume.empty())
        die("could not mount '" + failedVolume + "'");

    if(command == "volumes")
        cmdVolumes();
    else if(command == "ls")
        cmdLs(args);
    else if(command == "types")
        cmdTypes(args);
    else if(command == "res")
        cmdRes(args);
    else if(command == "dump")
        cmdDump(args);
    else if(command == "dis")
        cmdDis(args);
    else if(command == "disres")
        cmdDisRes(args);
    else
    {
        std::cerr << "executor-tool: unknown command '" << command << "'\n";
        usage();
        return 1;
    }

    return 0;
}
