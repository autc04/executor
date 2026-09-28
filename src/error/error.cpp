/* Copyright 1995, 1996 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <base/common.h>

#include <stdarg.h>
#include <SysErr.h>
#include <SegmentLdr.h>
#include <CQuickDraw.h>

#include <vdriver/vdriver.h>
#include <error/system_error.h>
#include <sound/soundopts.h>
#include <rsys/version.h>
#include <mman/mman.h>
#include <commandline/flags.h>
#include <errno.h>

using namespace Executor;

/* This is a bit mask containing the bitwise OR of the various
 * ERROR_..._BIT flags in error/error.h.
 */
uint32_t ROMlib_debug_level;

static const struct
{
    const char *name;
    uint32_t bit_mask;
} error_options[] = {
    { "all", ~(uint32_t)0 },
    { "fast", (ERROR_BIT_MASK(ERROR_FILESYSTEM_LOG)
               | ERROR_BIT_MASK(ERROR_TRACE_INFO)
               | ERROR_BIT_MASK(ERROR_TRAP_FAILURE)
               | ERROR_BIT_MASK(ERROR_ERRNO)
               | ERROR_BIT_MASK(ERROR_UNEXPECTED)
               | ERROR_BIT_MASK(ERROR_UNIMPLEMENTED)) },
    { "fslog", ERROR_BIT_MASK(ERROR_FILESYSTEM_LOG) },
    { "memcheck", ERROR_BIT_MASK(ERROR_MEMORY_MANAGER_SLAM) },
    { "textcheck", ERROR_BIT_MASK(ERROR_TEXT_EDIT_SLAM) },
    { "trace", ERROR_BIT_MASK(ERROR_TRACE_INFO) },
    { "trapfailure", ERROR_BIT_MASK(ERROR_TRAP_FAILURE) },
    { "errno", ERROR_BIT_MASK(ERROR_ERRNO) },
    { "unexpected", ERROR_BIT_MASK(ERROR_UNEXPECTED) },
    { "unimplemented", ERROR_BIT_MASK(ERROR_UNIMPLEMENTED) },
    { "sound", ERROR_BIT_MASK(ERROR_SOUND_LOG) },
    { "float", ERROR_BIT_MASK(ERROR_FLOATING_POINT) },
    { "segvfault", ERROR_BIT_MASK(ERROR_SEGV_FAULT) },
};

/* An option string is a sequence of comma-separated identifiers that
 * specify which debugging options should be enabled.  An example
 * would be something like "trace,unimp,memcheck".  Unambiguous prefixes
 * for the canonical flag names may be used.  This function turns
 * on the corresponding bit(s) in ROMlib_debug_level for each specified
 * option, or turns them off if the option is specified with a leading
 * "-".  For example, the string "all,-mem" will turn on all debugging,
 * and then turn off memory checking.
 *
 * Returns true iff successful, false if there was an error.
 */
bool error_parse_option_string(const char *options)
{
    const char *p, *options_end;
    char *option_name;
    int opt_len;
    bool success_p;

    success_p = true;
    option_name = (char *)alloca(strlen(options) + 1); /* big enough for any option.*/
    options_end = options + strlen(options);

    /* Munch through the option string, extracting out the comma-separated
   * fields and looking them up in our table.
   */
    for(p = options; p < options_end; p += opt_len + 1 /* skip comma */)
    {
        const char *id_end;

        /* Find the end of this string. */
        id_end = strchr(p, ',');
        if(id_end == nullptr)
            id_end = options_end;
        opt_len = id_end - p;

        if(opt_len == 0)
        {
            fprintf(stderr, "Empty debug option specified.\n");
            success_p = false;
        }
        else
        {
            int i, match = -1;
            bool on_p;

            /* You can specify a leading "-" to turn an option off. */
            if(p[0] == '-')
            {
                ++p;
                --opt_len;
                option_name[opt_len - 1] = '\0';
                on_p = false;
            }
            else
            {
                on_p = true;
            }

            strncpy(option_name, p, opt_len);
            option_name[opt_len] = '\0';

            /* Look for any matching option.  Unique prefixes are OK,
	   * so "unimp" matches "unimplemented", etc.
	   */
            for(i = 0; i < (int)std::size(error_options); i++)
                if(!strncmp(error_options[i].name, option_name, opt_len))
                {
                    if(match >= 0)
                        fprintf(stderr, "Ambiguous debug option `%s' specified; "
                                        "could be %s or %s.\n",
                                option_name, error_options[match].name,
                                error_options[i].name);
                    else
                    {
                        match = i;
                        if(on_p)
                            ROMlib_debug_level |= error_options[i].bit_mask;
                        else
                            ROMlib_debug_level &= ~error_options[i].bit_mask;
                    }
                }

            if(match == -1)
            {
                fprintf(stderr, "Unknown debugging option `%s'.\n",
                        option_name);
                success_p = false;
            }
        }
    }

    return success_p;
}

bool error_parse_option_string(std::string options)
{
    return error_parse_option_string(options.c_str());
}

/* Enables or disables the specified error and returns its old enabledness. */
bool error_set_enabled(int err, bool enabled_p)
{
    bool old_enabled_p = ERROR_ENABLED_P(err);

    if(enabled_p)
        ROMlib_debug_level |= ERROR_BIT_MASK(err);
    else
        ROMlib_debug_level &= ~ERROR_BIT_MASK(err);

    return old_enabled_p;
}

static const char *
notdir(const char *file)
{
    const char *retval;

    retval = strrchr(file, '/');
    return retval ? &retval[1] : file;
}

static FILE *
get_stream(void)
{
    return stderr;
}

static void
flush_stream(void)
{
}

static void err_printf(const char *fmt, ...);

static void
err_vprintf(const char *fmt, va_list ap)
{
    static bool beenhere_p = false;
    FILE *fp;

    fp = get_stream();

#define MB (1024 * 1024U)

    /* Start any log with the current Executor version. */
    if(!beenhere_p)
    {
        fprintf(fp,
                "This is %s.\n"
                "Using %u.%02u MB for applzone, "
                "%u.%02u MB for syszone, %u.%02u MB for stack\n",
                ROMlib_executor_full_name,
                ROMlib_applzone_size / MB,
                (ROMlib_applzone_size % MB) * 100 / MB,
                ROMlib_syszone_size / MB,
                (ROMlib_syszone_size % MB) * 100 / MB,
                ROMlib_stack_size / MB,
                (ROMlib_stack_size % MB) * 100 / MB);
        beenhere_p = true;
    }

    if(fmt == nullptr)
        fmt = "";

    vfprintf(fp, fmt, ap);
    fflush(fp);
}

static void
err_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    err_vprintf(fmt, ap);
    va_end(ap);
}

void _gui_fatal(const char *file, int line, const char *fn,
                const char *fmt, ...)
{
    va_list ap;
    char errbuf[10240];

    if(fmt == nullptr)
        fmt = "";

    va_start(ap, fmt);
    vsprintf(errbuf, fmt, ap);
    va_end(ap);

    if(LM(WWExist) == EXIST_YES)
    {
        char buf[10340];

        /* Make sure the screen is sane. */
        SetGDevice(LM(MainDevice));
        RestoreDeviceClut(LM(MainDevice));

        sprintf(buf,
                "Fatal error.\r"
                "%s:%d; in `%s'\r"
                "%s",
                notdir(file), line, fn, errbuf);

        system_error(buf, 0,
                     "Restart", nullptr, nullptr,
                     nullptr, nullptr, nullptr);
    }

    /* also sent output to the debug stream */
    err_printf("%s:%d; fatal error in `%s': %s\n",
               notdir(file), line, fn, errbuf);
    flush_stream();

    ExitToShell();

    exit(-1); /* Convince gcc of __attribute__ ((noreturn)) status. */
}

void _warning(int error_type, const char *type, const char *file, int line,
              const char *fn, const char *fmt, ...)
{
    va_list ap;

    if(ERROR_ENABLED_P(error_type))
    {
        if(fmt == nullptr)
            fmt = "";

        err_printf("%s:%d; %s in `%s': ",
                   notdir(file), line, type, fn);

        va_start(ap, fmt);
        err_vprintf(fmt, ap);
        va_end(ap);

        err_printf("\n");
        flush_stream();
    }
}

#if ERROR_SUPPORTED_P(ERROR_SOUND_LOG)
void _sound_warning(const char *file, int line,
                    const char *fn, const char *fmt, ...)
{
    va_list ap;

    if(ERROR_ENABLED_P(ERROR_SOUND_LOG))
    {
        const char *snd_state;

        if(fmt == nullptr)
            fmt = "";

        err_printf("%s:%d; sound log in `%s': ",
                   notdir(file), line, fn);

        switch(ROMlib_PretendSound)
        {
            case soundoff:
                snd_state = "off";
                break;
            case soundon:
                snd_state = "on";
                break;
            case soundpretend:
                snd_state = "pretend";
                break;
            default:
                snd_state = "???";
                break;
        }

        err_printf("[sound=%s] ", snd_state);

        va_start(ap, fmt);
        err_vprintf(fmt, ap);
        va_end(ap);

        err_printf("\n");
        flush_stream();
    }
}
#endif /* ERROR_SUPPORTED_P (ERROR_SOUND_LOG) */

void _errno_fatal(const char *file, int line, const char *fn,
                  const char *fmt, ...)
{
    va_list ap;
    int save_errno = errno;

    if(fmt == nullptr)
        fmt = "";

    vdriver = {};

    err_printf("%s:%d; fatal error in `%s': ",
               notdir(file), line, fn);

    va_start(ap, fmt);
    err_vprintf(fmt, ap);
    va_end(ap);

    err_printf(": %s\n", strerror(save_errno));

    flush_stream();

    exit(-1);
}

void _errno_warning(const char *file, int line, const char *fn,
                    const char *fmt, ...)
{
    va_list ap;
    int save_errno = errno;

    if(fmt == nullptr)
        fmt = "";

    err_printf("%s:%d; warning in `%s': ",
               notdir(file), line, fn);

    va_start(ap, fmt);
    err_vprintf(fmt, ap);
    va_end(ap);

    err_printf(": %s\n", strerror(save_errno));

    flush_stream();
}

