/* Copyright 1996 - 2000 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include <base/common.h>

#include <rsys/aboutbox.h>
#include <mman/mman.h>
#include <util/string.h>
#include <quickdraw/cquick.h>
#include <quickdraw/quick.h>
#include <ctl/ctl.h>
#include <rsys/version.h>
#include <CQuickDraw.h>
#include <Gestalt.h>
#include <ToolboxEvent.h>
#include <TextEdit.h>
#include <FontMgr.h>
#include <OSUtil.h>
#include <ctype.h>
#include <file/file.h>
#include <rsys/osutil.h>
#include <SegmentLdr.h>
#include <rsys/segment.h>
#include <rsys/gestalt.h>
#include <osevent/osevent.h>
#include <rsys/paths.h>
#include <algorithm>
#include <textedit/textedit.h>

#include <cmrc/cmrc.hpp>

#include <algorithm>

CMRC_DECLARE(resources);

#define ABOUT_BOX_WIDTH 500
#define ABOUT_BOX_HEIGHT 300
#define BUTTON_WIDTH 85
#define BUTTON_HEIGHT 20
#define SCROLL_BAR_WIDTH 16
#define TE_MARGIN 4
#define TE_WIDTH (ABOUT_BOX_WIDTH - 20)
#define TE_HEIGHT (ABOUT_BOX_HEIGHT - 106)
#define TE_LEFT ((ABOUT_BOX_WIDTH - TE_WIDTH) / 2)
#define TE_RIGHT (TE_LEFT + TE_WIDTH)
#define TE_TOP 66
#define TE_BOTTOM (TE_TOP + TE_HEIGHT)

#define DONE_BUTTON_NAME "OK"

#define LICENSE_BUTTON_NAME "License"
#define TIPS_BUTTON_NAME "Tips"

//#define COPYRIGHT_STRING_1 "Copyright \251 ARDI 1986-2006"
//#define COPYRIGHT_STRING_2 "All rights reserved."

using namespace Executor;

struct AboutPage
{
    std::string name;
    cmrc::file text;
    ControlHandle ctl;
};

std::vector<AboutPage> about_box_buttons;

static WindowPtr about_box;
static ControlHandle about_scrollbar;
static TEHandle about_te;
static ControlActionUPP scroll_bar_callback;

enum
{
    THROTTLE_TICKS = 4
};

static bool
enough_time_has_passed(void)
{
    static ULONGINT old_ticks;
    ULONGINT new_ticks;
    bool retval;

    new_ticks = TickCount();
    retval = new_ticks - old_ticks >= THROTTLE_TICKS;
    if(retval)
        old_ticks = new_ticks;
    return retval;
}

static void
help_scroll(ControlHandle c, INTEGER part)
{
    if(enough_time_has_passed())
    {
        int page_size, old_value, new_value, delta;
        int line_height;

        old_value = GetControlValue(c);
        line_height = TE_LINE_HEIGHT(about_te);
        page_size = RECT_HEIGHT(&CTL_RECT(c)) / line_height;

        switch(part)
        {
            case inUpButton:
                SetControlValue(c, old_value - 1);
                break;
            case inDownButton:
                SetControlValue(c, old_value + 1);
                break;
            case inPageUp:
                SetControlValue(c, old_value - page_size);
                break;
            case inPageDown:
                SetControlValue(c, old_value + page_size);
                break;
            default:
                break;
        }

        new_value = GetControlValue(about_scrollbar);
        delta = new_value - old_value;
        if(delta != 0)
            TEScroll(0, -delta * line_height, about_te);
    }
}

static syn68k_addr_t
scroll_stub(syn68k_addr_t junk, void *junk2)
{
    syn68k_addr_t retaddr;
    ControlHandle ctl;
    INTEGER part;

    retaddr = POPADDR();
    part = POPUW();
    ctl = (ControlHandle)SYN68K_TO_US(POPUL());
    help_scroll(ctl, part);
    return retaddr;
}

/*
 * get an upper bound on the number of tips that may be in the buffer
 */

static int
approximate_tips(const char *p, const char *q)
{
    int retval;

    retval = 0;
    while(p != q)
    {
        ++retval;
        while(p != q && *p != '\n')
            ++p;
        while(p != q && (*p == '\n' || *p == '\r'))
            ++p;
    }
    return retval;
}

typedef struct
{
    int tip_offset;
    int tip_length;
} tip_t;

static int
find_tips(tip_t *tips, const char *p, const char *q)
{
    const char *orig_p;
    int retval;

    retval = 0;

    orig_p = p;
    while(p != q)
    {
        /* suck up any excess leading \n */
        while(p != q && (*p == '\n' || *p == '\r'))
            ++p;

        if(p != q)
        {
            tips->tip_offset = p - orig_p;

            while(p != q && (*p != '\n' || (p + 1 != q && p[1] != '\n' && p[1] != '\r')))
                ++p;

            tips->tip_length = p - orig_p - tips->tip_offset;
            ++retval;
            ++tips;
        }
    }

    return retval;
}

static void
add_to_str(char*& pp, const char *ip, int n)
{
    int i;

    for(i = 0; i < n; ++i)
    {
        if(ip[i] == '\r')
            ;
        else if(ip[i] == '\n')
            *pp++ = ' ';
        else
            *pp++ = ip[i];
    }
}

static char *
parse_and_randomize_tips(const char *buf, const char *bufend)
{
    int n_tips, chars_needed;
    tip_t *tips;
    bool seen_tip;
    char *retval;
    char *p;
    int i;

    n_tips = approximate_tips(buf, bufend);
    tips = (tip_t *)alloca(n_tips * sizeof *tips);
    n_tips = find_tips(tips, buf, bufend);

    chars_needed = 0;
    for(i = 0; i < n_tips; ++i)
        chars_needed += tips[i].tip_length;
    chars_needed += 2 * (n_tips - 1) + 1;

    retval = (char *)malloc(chars_needed);
    seen_tip = false;
    p = retval;
    while(n_tips)
    {
        if(seen_tip)
        {
            *p++ = '\r'; *p++ = '\r';
        }
        else
            seen_tip = true;
        i = rand() % n_tips;
        add_to_str(p, buf + tips[i].tip_offset, tips[i].tip_length);
        tips[i] = tips[n_tips - 1];
        --n_tips;
    }
    *p = 0;
    return retval;
}

static void
load_pages()
{
    if(!about_box_buttons.empty())
        return;
    
    auto efs = cmrc::resources::get_filesystem();

    for(auto file : efs.iterate_directory("about"))
    {
        about_box_buttons.push_back({
            file.filename(),
            efs.open("about/" + file.filename()),
            nullptr
        });
    }

    for(auto& b : about_box_buttons)
    {
        size_t pos;
        if((pos = b.name.find("_")) != std::string::npos)
            b.name = b.name.substr(pos + 1);
        if((pos = b.name.find(".")) != std::string::npos)
            b.name = b.name.substr(0,pos);
    }

    std::sort(about_box_buttons.begin(), about_box_buttons.end(),
        [](auto a, auto b) { return a.name < b.name; });

        about_box_buttons.push_back({
            DONE_BUTTON_NAME,
            {},
            nullptr
        });
}

static void
create_about_box()
{
    load_pages();

    static Rect scroll_bar_bounds = {
        TE_TOP,
        TE_RIGHT - SCROLL_BAR_WIDTH,
        TE_BOTTOM,
        TE_RIGHT
    };
    static Rect te_bounds = {
        TE_TOP + 1,
        TE_LEFT + TE_MARGIN,
        TE_BOTTOM - 1,
        TE_RIGHT - TE_MARGIN - SCROLL_BAR_WIDTH
    };
    Rect about_box_bounds;
    int b;

    
    SetRect(&about_box_bounds,
            (qdGlobals().screenBits.bounds.right - ABOUT_BOX_WIDTH) / 2U,
            (qdGlobals().screenBits.bounds.bottom - ABOUT_BOX_HEIGHT) / 3U + 15,
            (qdGlobals().screenBits.bounds.right + ABOUT_BOX_WIDTH) / 2U,
            (qdGlobals().screenBits.bounds.bottom + 2 * ABOUT_BOX_HEIGHT) / 3U + 15);

    /* Create the window. */
    about_box = NewCWindow(nullptr, &about_box_bounds,
                                      (StringPtr) "\016About Executor",
                                      false, dBoxProc, (WindowPtr)-1,
                                      true, /* go away flag */
                                      -5 /* unused */);
    ThePortGuard guard(about_box);
    /* Create the push buttons. */
    for(b = 0; b < about_box_buttons.size(); b++)
    {
        Str255 str;
        Rect r;

        /* Set up the rectangle enclosing each button. */
        r.top = ABOUT_BOX_HEIGHT - 30;
        r.bottom = ABOUT_BOX_HEIGHT - 30 + BUTTON_HEIGHT;
        r.left = (b * ABOUT_BOX_WIDTH / about_box_buttons.size())
                    + (ABOUT_BOX_WIDTH / about_box_buttons.size()
                       - BUTTON_WIDTH)
                        / 2;
        r.right = r.left + BUTTON_WIDTH;

        str255_from_c_string(str, about_box_buttons[b].name.c_str());
        about_box_buttons[b].ctl = NewControl(about_box, &r, str, true, 0,
                                              0, 1, pushButProc, b);
    }

    about_scrollbar = NewControl(about_box, &scroll_bar_bounds, nullptr, true,
                                 0, 0, 100, scrollBarProc, -1);
    about_te = TENew(&te_bounds, &te_bounds);
    TESetAlignment(teFlushLeft, about_te);
}

/* Closes about box and frees up memory taken by it. */
static void
dispose_about_box(void)
{
    C_DisposeWindow(about_box);
    about_box = nullptr;
    about_scrollbar = nullptr;
}

/* Sets the text currently being displayed. */
static void
set_text(const char *text, size_t size)
{
    TESetText((Ptr)text, size, about_te);
    SetControlMaximum(about_scrollbar,
              std::max(0, (TE_N_LINES(about_te)
                      - ((TE_HEIGHT - 2) / TE_LINE_HEIGHT(about_te)))));
    SetControlValue(about_scrollbar, 0);
    TE_DEST_RECT(about_te) = TE_VIEW_RECT(about_te);
    InvalRect(&TE_VIEW_RECT(about_te));
}

/* Specifies which button is currently pressed. */
static void
set_current_button(int button)
{
    int i;
    if(about_box_buttons[button].name == TIPS_BUTTON_NAME)
    {
        srand(LM(Ticks).get());
        char* str = parse_and_randomize_tips(about_box_buttons[button].text.begin(),
                                             about_box_buttons[button].text.end());
        set_text(str, strlen(str));
        free(str);
    }
    else
    {
        char *str = (char*)alloca(about_box_buttons[button].text.size());
        char *p = str;
        for(char c : about_box_buttons[button].text)
        {
            if(c == '\n')
                *p++ = '\r';
            else if(c == '\r')
                ;
            else
                *p++ = c;
        }
        set_text(str, p-str);
    }
    for(i = 0; i < about_box_buttons.size(); i++)
        HiliteControl(about_box_buttons[i].ctl, (i == button) ? 255 : 0);
}

static void
draw_mem_string(char *s, int y)
{
    MoveTo((ABOUT_BOX_WIDTH - 9 - TextWidth((Ptr)s, 0, strlen(s))), y);
    DrawText_c_string(s);
}

static void
draw_status_info()
{
    char total_ram_string[128];
    char applzone_ram_string[128];
    char syszone_ram_string[128];
    bool gestalt_success_p;
    LONGINT total_ram;
    GUEST<LONGINT> total_ram_x;
    const char *ram_tag, *system_ram_tag, *application_ram_tag;

    ram_tag = "Emulated RAM: ";
    system_ram_tag = "System RAM free: ";
    application_ram_tag = "Application RAM free: ";

/* Compute a string for total RAM. */
#define MB (1024 * 1024U)
    gestalt_success_p = (C_GestaltTablesOnly(gestaltLogicalRAMSize, &total_ram_x)
                         == noErr);
    total_ram = total_ram_x;
    if(gestalt_success_p)
        sprintf(total_ram_string, "%s%u.%02u MB", ram_tag,
                total_ram / MB, (total_ram % MB) * 100 / MB);
    else
        sprintf(total_ram_string, "%s??? MB", ram_tag);

    /* Compute a string for LM(ApplZone) RAM. */
    {
        TheZoneGuard guard(LM(ApplZone));
        sprintf(applzone_ram_string, "%s%lu KB / %u KB", application_ram_tag,
                FreeMem() / 1024UL, ROMlib_applzone_size / 1024U);
    }

    /* Compute a string for LM(SysZone) RAM. */
    sprintf(syszone_ram_string, "%s%lu KB / %u KB", system_ram_tag,
            FreeMemSys() / 1024UL, ROMlib_syszone_size / 1024U);

    draw_mem_string(total_ram_string, 20);
    draw_mem_string(syszone_ram_string, 36);
    draw_mem_string(applzone_ram_string, 52);
}

static void
event_loop()
{
    static Rect frame_rect = {
        TE_TOP,
        TE_LEFT,
        TE_BOTTOM,
        TE_RIGHT - SCROLL_BAR_WIDTH + 1
    };
    EventRecord evt;
    bool done_p;
    int which_text;
    Rect te_dest_rect;
    int old_scroll_bar_value;

    /* Make sure our drawing mode is sane. */
    ForeColor(blackColor);
    BackColor(whiteColor);
    PenNormal();

    te_dest_rect = TE_DEST_RECT(about_te);

    set_current_button(0);
    which_text = 0;
    InvalRect(&about_box->portRect);

    old_scroll_bar_value = GetControlValue(about_scrollbar);

    for(done_p = false; !done_p;)
    {
        GetNextEvent((mDownMask | mUpMask
                      | keyDownMask | keyUpMask | autoKeyMask
                      | updateMask | activMask),
                     &evt);

        TEIdle(about_te);

        switch(evt.what)
        {
            case updateEvt:
                BeginUpdate(about_box);
                PenNormal();
                ForeColor(blackColor);
                BackColor(whiteColor);
                EraseRect(&about_box->portRect);
                TextFont(kFontIDHelvetica);
                TextSize(24);
                MoveTo(TE_LEFT, 30);
                DrawText_c_string(ROMlib_executor_full_name);
                TextSize(12);
                /*MoveTo(TE_LEFT, 49);
                DrawText_c_string(COPYRIGHT_STRING_1);
                MoveTo(TE_LEFT, 62);
                DrawText_c_string(COPYRIGHT_STRING_2);*/
                draw_status_info();
                FrameRect(&frame_rect);
                TEUpdate(&te_dest_rect, about_te);
                DrawControls(about_box);
                EndUpdate(about_box);
                break;

            case keyDown:
            case autoKey:
            {
                char ch;

                ch = evt.message & 0xFF;
                switch(ch)
                {
                    case '\r':
                    case NUMPAD_ENTER:
                        done_p = true;
                        break;
                    default:
                        TEKey(ch, about_te);
                        break;
                }
            }
            break;

            case mouseDown:
            {
                Point local_pt;
                bool control_p;
                ControlHandle c;

                GUEST<Point> tmpPt = evt.where;
                GlobalToLocal(&tmpPt);
                local_pt = tmpPt.get();

                GUEST<ControlHandle> bogo_c;
                control_p = FindControl(local_pt, about_box, &bogo_c);
                c = bogo_c;
                if(!control_p)
                    SysBeep(1);
                else
                {
                    if(c == about_scrollbar)
                    {
                        int new_val, delta;
                        INTEGER part;

                        part = TestControl(c, local_pt);

                        if(TrackControl(c, local_pt,
                                        (part == inThumb
                                             ? (ControlActionUPP)-1
                                             : scroll_bar_callback))
                           == inThumb)
                        {
                            new_val = GetControlValue(about_scrollbar);
                            delta = new_val - old_scroll_bar_value;
                            if(delta != 0)
                            {
                                TEScroll(0, -delta * TE_LINE_HEIGHT(about_te),
                                         about_te);
                            }
                        }

                        old_scroll_bar_value = GetControlValue(about_scrollbar);
                    }
                    else if(TrackControl(c, local_pt, (ControlActionUPP)-1)
                            == inButton)
                    {
                        int new_text;

                        new_text = CTL_REF_CON(c);
                        if(new_text != which_text)
                        {
                            if(about_box_buttons[new_text].name == DONE_BUTTON_NAME)
                                done_p = true;
                            else
                            {
                                set_current_button(new_text);
                                which_text = new_text;
                            }
                        }
                    }
                }
            }
            break;

            case activateEvt:
            case mouseUp:
            case keyUp:
                break;
        }
    }
}

void Executor::do_about_box(void)
{
    static bool busy_p = false;

    if(!busy_p)
    {
        busy_p = true; /* Only allow one about box at a time. */

        if(scroll_bar_callback == nullptr)
            scroll_bar_callback = (ControlActionUPP)SYN68K_TO_US(callback_install(scroll_stub, nullptr));

        {
            TheZoneGuard guard(LM(SysZone));
            create_about_box();

            {
                ThePortGuard portGuard(about_box);
                C_ShowWindow(about_box);
                event_loop();
            }

            TEDispose(about_te);
            about_te = nullptr;
            dispose_about_box();
        }

        busy_p = false;
    }
}
