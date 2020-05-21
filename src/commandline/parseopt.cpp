/* Copyright 1995 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <base/common.h>
#include <Gestalt.h>
#include <commandline/parseopt.h>
#include <commandline/flags.h>
#include <prefs/prefs.h>
#include <rsys/version.h>
#include <rsys/paths.h>

#include <ctype.h>
#include <string.h>

using namespace Executor;
using namespace std;

/* Parse version e.g. "executor -system 7.0.2".  Omitted
 * digits will be zero, so "executor -system 7" is equivalent to
 * "executor -system 7.0.0".  Returns true on success, else false.
 */

bool Executor::ROMlib_parse_version(string vers, uint32_t *version_out)
{
    bool success_p;
    int major_version, minor_version, teeny_version;
    char *major_str, *minor_str, *teeny_str;
    char *temp_str, *system_str;
    char zero_str[] = "0";

    /* Copy the version to a temp string we can manipulate. */
    system_str = (char *)alloca(vers.length() + 1);
    strcpy(system_str, vers.c_str());

    major_str = system_str;

    temp_str = strchr(major_str, '.');
    if(temp_str)
    {
        *temp_str = 0;
        minor_str = &temp_str[1];
    }
    else
        minor_str = zero_str;

    temp_str = strchr(minor_str, '.');
    if(temp_str)
    {
        *temp_str = 0;
        teeny_str = &temp_str[1];
    }
    else
        teeny_str = zero_str;

    major_version = atoi(major_str);
    minor_version = atoi(minor_str);
    teeny_version = atoi(teeny_str);

    if(major_version <= 0 || major_version > 0xF
       || minor_version < 0 || minor_version > 0xF
       || teeny_version < 0 || teeny_version > 0xF)
        success_p = false;
    else
    {
        *version_out = CREATE_SYSTEM_VERSION(major_version, minor_version,
                                             teeny_version);
        success_p = true;
    }

    return success_p;
}

/* Parse -system option, e.g. "executor -system 7.0.2".  Omitted
 * digits will be zero, so "executor -system 7" is equivalent to
 * "executor -system 7.0.0".  Returns true on success, else false.
 */
bool Executor::parse_system_version(string vers)
{
    bool retval;

    retval = ROMlib_parse_version(vers, &system_version);
    if(retval)
        ;
    else
    {
        fprintf(stderr, "%s: bad option `-system': invalid version\n",
                ROMlib_appname.c_str());
    }

    return retval;
}

/* Parse -size option, e.g. "executor -size 640x480".  Returns false
 * on parse error.
 */
bool Executor::parse_size_opt(string opt, string arg1)
{
    bool success_p;
    int w, h;
    const char *arg = arg1.c_str();

    w = h = 0;
    if(arg != nullptr)
    {
#if defined(CYGWIN32)
        if(strcasecmp(arg, "maximum") == 0)
        {
            h = os_maximum_window_height();
            w = os_maximum_window_width();
        }
        else
#else
// FIXME: #warning we should support "-size maximum"
#endif
        {
            const char *p;
            for(p = arg; isdigit(*p); p++)
                w = (10 * w) + (*p - '0');
            if(*p == 'x')
            {
                for(++p; isdigit(*p); p++)
                    h = (10 * h) + (*p - '0');
                if(*p != '\0')
                    h = 0;
            }
        }
    }

    if(w == 0 || h == 0)
    {
        fprintf(stderr, "Invalid screen size.  Use something like "
                        "\"-%s 640x480\".\n",
                opt.c_str());
        success_p = false;
    }
    else if(w < 512 || h < 342)
    {
        fprintf(stderr, "Screen size must be at least 512x342\n");
        success_p = false;
    }
    else
    {
        flag_width = w;
        flag_height = h;
        success_p = true;
    }

    return success_p;
}

/*
 * It's silly for us to do this by hand, but it was real quick to write.
 */

bool
Executor::parse_prres_opt(INTEGER *outx, INTEGER *outy, string arg1)
{
    bool retval;
    INTEGER x, y, *p;
    const char *arg = arg1.c_str();

    x = 0;
    y = 0;
    p = &x;

    for(retval = true; *arg && retval; ++arg)
    {
        switch(*arg)
        {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            {
                int old_val;
                int digit;

                old_val = *p;
                digit = *arg - '0';
                *p = 10 * *p + digit;
                if(*p <= old_val)
                    retval = false; /* overflow */
            }
            break;
            case 'x':
                if(p == &x)
                    p = &y;
                else
                    retval = false; /* extra x */
                break;
            default:
                retval = false; /* unknown character */
                break;
        }
    }

    if(x <= 0 || y <= 0)
        retval = false;

    if(retval)
    {
        *outx = x;
        *outy = y;
    }

    return retval;
}
