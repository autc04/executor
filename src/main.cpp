/* Copyright 1988 - 2005 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

/*
 * Errors should be handled more cleanly.
 */

#include <base/common.h>


#include <FontMgr.h>
#include <DialogMgr.h>
#include <AppleEvents.h>


#include <quickdraw/cquick.h>
#include <textedit/tesave.h>
#include <mman/mman.h>
#include <menu/menu.h>
#include <prefs/prefs.h>
#include <commandline/flags.h>
#include <time/syncint.h>
#include <vdriver/vdriver.h>
#include <time/vbl.h>
#include <time/time.h>
#include <rsys/segment.h>
#include <rsys/version.h>
#include <base/m68kint.h>
#include <quickdraw/rgbutil.h>
#include <vdriver/refresh.h>
#include <rsys/executor.h>
#include <wind/wind.h>
#include <osevent/osevent.h>
#include <quickdraw/image.h>
#include <rsys/dump.h>
#include <file/file.h>
#include <ctl/ctl.h>
#include <print/print.h>
#include <mman/memsize.h>
#include <vdriver/autorefresh.h>
#include <sound/sounddriver.h>
#include <error/system_error.h>
#include <base/emustubs.h>
#include <sane/float.h>
#include <rsys/paths.h>
#include <appleevent/apple_events.h>
#include <rsys/gestalt.h>
#include <rsys/launch.h>
#include <quickdraw/text.h>
#include <rsys/appearance.h>
#include <hfs/hfs_plus.h>
#include <base/cpu.h>
#include <base/debugger.h>
#include <debug/mon_debugger.h>
#include <PowerCore.h>
#include <vdriver/eventrecorder.h>
#include <commandline/program_options_extended.h>
#include <commandline/option_arguments.h>

#ifndef HEADLESS_ONLY
#include "default_vdriver.h"
#endif
#include "headless.h"

#if defined(__linux__) && defined(PERSONALITY_HACK)
#include <sys/personality.h>
#define READ_IMPLIES_EXEC 0x0400000
#endif


#include <ctype.h>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include <vector>
#include <thread>

#include <iostream>

using namespace Executor;
using namespace std;

namespace po = boost::program_options;
namespace pox = program_options_extended;


static bool use_native_code_p = true;
static bool breakOnProcessStart = false;
static bool logtraps = false;
static std::vector<std::string> debugCommands;
static std::string keyboard;
static bool list_keyboards_p = false;

static bool flag_headless = false;
static std::optional<fs::path> flag_record, flag_playback;

/* 0 means "use default". */
static int flag_width, flag_height;
/* 0 means "use default". */
static int flag_bpp;
static bool flag_grayscale;


static void reportBadArgs()
{
    fprintf(stderr,
            "Type \"%s --help\" for a list of command-line options.\n",
            ROMlib_appname.c_str());
    exit(-10);
}

static void checkBadArgs(const std::vector<std::string>& args)
{
    bool bad_arg_p = false;

    if(!args.empty())
    {
        /* Only complain if we see something with a leading dash; anything
         * else might be a file to launch.
         */
        for(const auto& arg : args)
        {
            if(arg[0] == '-')
            {
                fprintf(stderr, "%s: unknown option `%s'\n",
                        ROMlib_appname.c_str(), arg.c_str());
                bad_arg_p = true;
            }
            else
                break;
        }
    }

    if(bad_arg_p)
        reportBadArgs();
}

static void updateArgcArgv(int& argc, char **argv, std::vector<std::string>& args)
{
    for(int i = 0; i < args.size(); i++)
        argv[i + 1] = args[i].data();
    argc = args.size() + 1;
    argv[argc] = nullptr;
}

struct SilentBadArgException {};


static std::vector<std::string> parseCommandLine(int& argc, char **argv)
{
    po::options_description desc;

    bool modeHelp = false, modeKeyboards = false, modeVersion = false;

    po::options_description modes("Getting Information");
    modes.add_options()
        ("help,h", po::bool_switch(&modeHelp), "display help message")
        ("version,v", po::bool_switch(&modeVersion), "display version")
        ("keyboards", po::bool_switch(&modeKeyboards), "display all supported keyboard layouts")
        ;
    desc.add(modes);
 
    po::options_description screen("Screen");
    screen.add_options()
        ("bpp", pox::value(&flag_bpp)->validator([](int d) {
            return !(d > 32 || d < 1 || (d & (d-1)));
        }), "screen depth (1,2,4,8,16,32)")
        ("size", pox::value<Size2D>()
            ->validator([](Size2D s) {
                return s.width >= 512 && s.height >= 342;
            })
            ->notifier([](Size2D s) {
                flag_width = s.width;
                flag_height = s.height;
            })
        , "screen size in pixels")
        ("grayscale", po::bool_switch(&flag_grayscale), "grayscale graphics (for use with --bpp 2,4,8)");
    desc.add(screen);


    po::options_description printing("Printing");
    printing.add_options()
        ("print", po::bool_switch(&ROMlib_print), "tell emulated application to print the specified document(s)")
        ("cities", pox::inverted_bool_switch(&ROMlib_fontsubstitution), "do not substitute standard PostScript fonts for classic Mac fonts")
        ("prvers", pox::value<VersionNumber>()
            ->validator([](VersionNumber v) {
                return v.minor < 10 && v.patch == 0;
            })
            ->notifier([](VersionNumber v) {
                ROMlib_PrDrvrVers = v.major * 10 + v.minor;
            })
        , "printer driver version to report to the application")
        ("prres", pox::value<Size2D>()
            ->validator([](Size2D s) {
                return s.width >= 60 && s.height >= 60;
            })
            ->notifier([](Size2D s) {
                ROMlib_optional_res_x = s.width;
                ROMlib_optional_res_y = s.height;
            })
        , "printer resolution");
    desc.add(printing);

    po::options_description testing("Automated Testing");
    testing.add_options()
#ifdef HEADLESS_ONLY
        ("headless", po::bool_switch(&flag_headless), "disable all graphics output (always on in this build)")
#else
        ("headless", po::bool_switch(&flag_headless), "disable all graphics output")
#endif
        ("record", po::value(&flag_record), "record events to file")
        ("playback", po::value(&flag_playback), "play back events from file")
        ("timewarp", pox::value<Ratio>()
            ->validator([](Ratio r) { return r.numer > 0 && r.denom > 0; })
            ->notifier([&](Ratio r) { ROMlib_SetTimewarp(r.numer, r.denom); })
        , "speed up or slow down time")
        ;
    desc.add(testing);

    po::options_description debugging("Debugging");
    debugging.add_options()
        ("logtraps", po::bool_switch(&logtraps), "print all operating system and toolbox calls and their arguments")
        ("break", po::bool_switch(&breakOnProcessStart), "break into debugger at program start")
        ("debug-cmd", po::value(&debugCommands), "run a debugger command at startup (e.g. \"atb \\\"PBGetFInfo/PBHGetFInfo\\\"\"); may be repeated")
        ("debug", po::value<std::string>()->notifier([](const std::string& s) {
            if (!error_parse_option_string(s.c_str()))
                throw SilentBadArgException();
        }), 
            "enable certain debugging output and consistency checks.  This "
            "is primarily used by ARDI developers, but we are making it "
            "available during the pre-beta period to expert users.  The next "
            "argument must be a list of comma-separated words describing which "
            "debug options you want enabled.  You can abbreviate the debug "
            "options as long as the abbreviation is unambiguous.  Here is a "
            "list of the options (some of which may do nothing):  "
            "\"all\" enables all debugging options, "
            "\"fslog\" enables filesystem call logging, "
            "\"memcheck\" enables heap consistency checking (slow!), "
            "\"textcheck\" enables text record consistency checking (slow!), "
            "\"trace\" enables miscellaneous  trace information, "
            "\"sound\" enables miscellaneous sound logging information, "
            "\"trapfailure\" enables warnings when traps return error codes, "
            "\"errno\" enables some C library-related warnings, "
            "\"unexpected\" enables warnings for unexpected events, "
            "\"unimplemented\" enables warnings for unimplemented traps.  "
            "Example: \"executor -debug unimp,trace\""
        )
        ;
    desc.add(debugging);


    po::options_description emulation("Emulation");
    emulation.add_options()
        ("ppc", po::bool_switch(&ROMlib_prefer_ppc), "prefer PowerPC code in FAT binaries")
#ifdef GENERATE_NATIVE_CODE
        ("no-jit", pox::inverted_bool_switch(&use_native_code_p), "enable JIT compiler")
#endif
        /* TODO: ("memory", po::value<std::size_t>()->notify([](size_t x) { std::cout << "a\n"; }), 
            "specify the total memory you want reserved for use by the programs "
            "run under Executor and for Executor's internal system software.  "
            "For example, \"executor -memory 5.5M\" would "
            "make five and a half megabytes available to the virtual machine.  "
            "Executor will require extra memory above and beyond this amount "
            "for other uses."
        )*/
        ("applzone", pox::value<MemorySize>(&ROMlib_applzone_size), 
            "specify the memory to allocate for the application being run, "
            "e.g. \"executor -applzone 4M\" would make four megabytes "
            "of RAM available to the application.  \"applzone\" stands for "
            "\"application zone\".")
        ("syszone", pox::value(&ROMlib_syszone_size),
            "like -applzone, but specifies the amount of memory to make "
            "available to Executor's internal system software."
        )
        ("stack", pox::value(&ROMlib_stack_size), 
            "like -applzone, but specifies the amount of stack memory to allocate."
        )
        ("system", pox::value<VersionNumber>()
            ->validator([](VersionNumber v) {
                return v.major < 10 && v.minor < 16 && v.patch < 16;
            }) 
            ->notifier([](VersionNumber v) {
                system_version = CREATE_SYSTEM_VERSION(v.major, v.minor, v.patch);
            })
        , "specify the system version that executor reports to applications")
        ;
    desc.add(emulation);

    po::options_description sound("Sound");
    sound.add_options()
        ("nosound", pox::inverted_bool_switch(&sound_disabled_p), "disabe sound output")
        ;
    desc.add(sound);

    po::options_description misc("Miscellaneous");
    misc.add_options()
        ("noclock", po::bool_switch(&ROMlib_noclock), "disable timer interrupt")
        ("speech", po::bool_switch(&ROMlib_speech_enabled), "enable speech manager (mac hosts only)")
        ("noautorefresh", pox::inverted_bool_switch(&do_autorefresh_p), "turns off automatic detection of programs that bypass QuickDraw")
        ("refresh", po::value(&ROMlib_refresh)
            ->implicit_value(10),
            "Handle programs that bypass QuickDraw, at a performance penalty."
            "Follow -refresh with an number indicating how many 60ths of a second "
            "to wait between each screen refresh, e.g. \"executor -refresh 10\".")
        ("appearance", po::value<std::string>()->notifier([&](const std::string& s) {
            if(!ROMlib_parse_appearance(s.c_str()))
                throw SilentBadArgException();
        }), "(mac or windows) specify the appearance of windows and "
                    "menus.  For example \"executor -appearance windows\" will make each "
                    "window have a blue title bar")
        ("scancodes", po::bool_switch(&ROMlib_use_scan_codes), 
            "different form of key mapping (may be useful in "
            "conjunction with -keyboard; not supported for all vdrivers)")
        ("keyboard", po::value(&keyboard), "choose a specific keyboard map")
#if defined(__linux__)
        ("nodrivesearch", po::bool_switch(&nodrivesearch_p), 
            "Do not look for a floppy drive, CD-ROM drive or any other drive "
            "except as specified by the MacVolumes environment variable"
        )
#endif
        ("nobrowser", po::bool_switch(&ROMlib_nobrowser), "don't run Browser")
        ("sticky", po::bool_switch(&ROMlib_sticky_menus_p), "sticky menus")
        ;
    desc.add(misc);

    std::vector<std::string> unrecognized;

    try
    {
        auto parsed = po::command_line_parser(argc, argv)
            .options(desc)
            .allow_unregistered()
            .run();
        po::variables_map vm;
        po::store(parsed, vm);
        po::notify(vm);

        std::cout << argc << ":";
        for(int i = 0; i < argc; i++)
            std::cout << " " << argv[i];
        std::cout << std::endl;

        unrecognized = po::collect_unrecognized(parsed.options, po::include_positional);
    }
    catch (const po::error& err)
    {
        std::cerr << err.what();
        exit(1);
    }
    catch (const SilentBadArgException&)
    {
        exit(1);
    }
    for(auto x : unrecognized)
        std::cout << " " << x;
    std::cout << std::endl;
    if(modeHelp)
    {
        std::cout << desc;
        exit(0);
    }
    else if(modeVersion)
    {
        fprintf(stdout, "%s\n", ROMlib_executor_full_name);
        exit(0);
    }
    else if(modeKeyboards)
    {
        list_keyboards_p = true;
        flag_headless = true;
    }

    return unrecognized;
}

extern std::vector<std::string> argsHack;

int main(int argc, char **argv)
{
#if defined(__linux__) && defined(PERSONALITY_HACK)
    int pers;

    // TODO: figure out how much of this is still necessary.
    // MMAP_PAGE_ZERO should be unnecessary now,
    // but the 32-bit optimized assembly stuff might need READ_IMPLIES_EXEC.
    pers = personality(0xffffffff);
    if((pers & MMAP_PAGE_ZERO) == 0)
    {
        if(personality(pers | MMAP_PAGE_ZERO | READ_IMPLIES_EXEC) == 0)
            execv(argv[0], argv);
    }
#endif

    /* Guarantee various time variables are set up properly. */
    msecs_elapsed();

    ROMlib_appname = fs::path(argv[0]).filename().string();
    auto remainingArgs = parseCommandLine(argc, argv);

    if(flag_playback)
        EventSink::instance = std::make_unique<EventPlayback>(*flag_playback);
    else if(flag_record)
        EventSink::instance = std::make_unique<EventRecorder>(*flag_record);
    else
        EventSink::instance = std::make_unique<EventSink>();

    updateArgcArgv(argc, argv, remainingArgs);

#ifdef HEADLESS_ONLY
    vdriver = std::make_unique<HeadlessVideoDriver>(EventSink::instance.get());
#else
    if(flag_headless)
        vdriver = std::make_unique<HeadlessVideoDriver>(EventSink::instance.get());
    else
        vdriver = std::make_unique<DefaultVDriver>(EventSink::instance.get(), argc, argv);
#endif

    remainingArgs = std::vector<std::string>(argv + 1, argv + argc);
    
    checkBadArgs(remainingArgs);
    argsHack = remainingArgs;

    auto executorThread = std::thread([&] {
        try
        {
            char thingOnStack; /* used to determine an approximation of the stack base address */
            InitMemory(&thingOnStack);

            initialize_68k_emulator(nullptr,
                                    use_native_code_p,
                                    (uint32_t *)SYN68K_TO_US(0),
                                    0);

            EM_A7 = ptr_to_longint(LM(CurStackBase));

            Executor::traps::init(logtraps);
            InitLowMem();
            syncint_init(); // timer interrupts: must not be inited before cpu & trapvevtors

            ROMlib_InitGDevices(flag_width, flag_height, flag_bpp, flag_grayscale);
            
            ROMlib_eventinit();
            hle_init();

            ROMlib_fileinit();

            InitUtil();

            InitResources();
            
            ROMlib_set_system_version(system_version);

            {
                bool keyboard_set_failed = false;

                if(!keyboard.empty())
                {
                    keyboard_set_failed = !ROMlib_set_keyboard(keyboard.c_str());
                    if(keyboard_set_failed)
                        printf("``%s'' is not an available keyboard\n", keyboard.c_str());
                }

                if(keyboard_set_failed || list_keyboards_p)
                    display_keyboard_choices();
            }

            InitAppFiles(remainingArgs);

            InitFonts();

        #if !defined(NDEBUG)
            dump_init(nullptr);
        #endif

            ROMlib_color_init();

            wind_color_init();
            image_inits();  // must be called after `ROMlib_color_init ()'
            sb_ctl_init();  //  must be after `image_inits ()'

            AE_init();

            {
                INTEGER env = 0;
                ROMlib_Fsetenv(inout(env), 0);
            }

            sound_init();

            set_refresh_rate(ROMlib_refresh);

            InitMonDebugger();
            base::Debugger::instance->setBreakOnProcessEntry(breakOnProcessStart);
            RunDebuggerCommands(debugCommands);

            executor_main();
            ExitToShell();   
        }
        catch(const ExitToShellException& e)
        {
        }

        vdriver->endEventLoop();
    });

    vdriver->runEventLoop();
    executorThread.join();

    vdriver.reset();

    /* NOT REACHED */
    return 0;
}
