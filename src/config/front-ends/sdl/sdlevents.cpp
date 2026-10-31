/* Copyright 1997, 1998 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include "sdl.h"
#include <vdriver/vdriver.h>
#include <base/common.h>
#include <QuickDraw.h>
#include <EventMgr.h>
#include <SegmentLdr.h>
#include <OSEvent.h>
#include <ToolboxEvent.h>
#include <ScrapMgr.h>

#include <rsys/segment.h>
#include <base/m68kint.h>
#include <osevent/osevent.h>
#include <rsys/keyboard.h>
#include <rsys/adb.h>
#include <rsys/scrap.h>
#include <rsys/toolevent.h>
#include <rsys/keyboard.h>
#include <prefs/prefs.h>

#include <SDL/SDL.h>

#include <cstdio>
#include <cstdlib>

using namespace Executor;

#if SDL_MAJOR_VERSION == 0 && SDL_MINOR_VERSION < 9

#include "sdlk_to_mkv.h"

static void
init_sdlk_to_mkv(void)
{
}
#else

enum
{
    NOTAKEY = 0x89
};

static unsigned char sdlk_to_mkv[SDLK_LAST];

typedef struct
{
    SDLKey sdlk;
    unsigned char mkv;
} sdl_to_mkv_map_t;

static sdl_to_mkv_map_t map[] = {
    { SDLK_BACKSPACE, MKV_BACKSPACE },
    {
        SDLK_TAB, MKV_TAB,
    },
    {
        SDLK_CLEAR, NOTAKEY,
    },
    {
        SDLK_RETURN, MKV_RETURN,
    },
    {
        SDLK_ESCAPE, MKV_ESCAPE,
    },
    {
        SDLK_SPACE, MKV_SPACE,
    },
    {
        SDLK_QUOTE, MKV_TICK,
    },
    {
        SDLK_COMMA, MKV_COMMA,
    },
    {
        SDLK_MINUS, MKV_MINUS,
    },
    {
        SDLK_PERIOD, MKV_PERIOD,
    },
    {
        SDLK_SLASH, MKV_SLASH,
    },
    {
        SDLK_0, MKV_0,
    },
    {
        SDLK_1, MKV_1,
    },
    {
        SDLK_2, MKV_2,
    },
    {
        SDLK_3, MKV_3,
    },
    {
        SDLK_4, MKV_4,
    },
    {
        SDLK_5, MKV_5,
    },
    {
        SDLK_6, MKV_6,
    },
    {
        SDLK_7, MKV_7,
    },
    {
        SDLK_8, MKV_8,
    },
    {
        SDLK_9, MKV_9,
    },
    {
        SDLK_SEMICOLON, MKV_SEMI,
    },
    {
        SDLK_EQUALS, MKV_EQUAL,
    },
    {
        SDLK_KP0, MKV_NUM0,
    },
    {
        SDLK_KP1, MKV_NUM1,
    },
    {
        SDLK_KP2, MKV_NUM2,
    },
    {
        SDLK_KP3, MKV_NUM3,
    },
    {
        SDLK_KP4, MKV_NUM4,
    },
    {
        SDLK_KP5, MKV_NUM5,
    },
    {
        SDLK_KP6, MKV_NUM6,
    },
    {
        SDLK_KP7, MKV_NUM7,
    },
    {
        SDLK_KP8, MKV_NUM8,
    },
    {
        SDLK_KP9, MKV_NUM9,
    },
    {
        SDLK_KP_PERIOD, MKV_NUMPOINT,
    },
    {
        SDLK_KP_DIVIDE, MKV_NUMDIVIDE,
    },
    {
        SDLK_KP_MULTIPLY, MKV_NUMMULTIPLY,
    },
    {
        SDLK_KP_MINUS, MKV_NUMMINUS,
    },
    {
        SDLK_KP_PLUS, MKV_NUMPLUS,
    },
    {
        SDLK_KP_ENTER, MKV_NUMENTER,
    },
    {
        SDLK_LEFTBRACKET, MKV_LEFTBRACKET,
    },
    {
        SDLK_BACKSLASH, MKV_BACKSLASH,
    },
    {
        SDLK_RIGHTBRACKET, MKV_RIGHTBRACKET,
    },
    {
        SDLK_BACKQUOTE, MKV_BACKTICK,
    },
    {
        SDLK_a, MKV_a,
    },
    {
        SDLK_b, MKV_b,
    },
    {
        SDLK_c, MKV_c,
    },
    {
        SDLK_d, MKV_d,
    },
    {
        SDLK_e, MKV_e,
    },
    {
        SDLK_f, MKV_f,
    },
    {
        SDLK_g, MKV_g,
    },
    {
        SDLK_h, MKV_h,
    },
    {
        SDLK_i, MKV_i,
    },
    {
        SDLK_j, MKV_j,
    },
    {
        SDLK_k, MKV_k,
    },
    {
        SDLK_l, MKV_l,
    },
    {
        SDLK_m, MKV_m,
    },
    {
        SDLK_n, MKV_n,
    },
    {
        SDLK_o, MKV_o,
    },
    {
        SDLK_p, MKV_p,
    },
    {
        SDLK_q, MKV_q,
    },
    {
        SDLK_r, MKV_r,
    },
    {
        SDLK_s, MKV_s,
    },
    {
        SDLK_t, MKV_t,
    },
    {
        SDLK_u, MKV_u,
    },
    {
        SDLK_v, MKV_v,
    },
    {
        SDLK_w, MKV_w,
    },
    {
        SDLK_x, MKV_x,
    },
    {
        SDLK_y, MKV_y,
    },
    {
        SDLK_z, MKV_z,
    },
    {
        SDLK_DELETE, MKV_DELFORWARD,
    },
    {
        SDLK_F1, MKV_F1,
    },
    {
        SDLK_F2, MKV_F2,
    },
    {
        SDLK_F3, MKV_F3,
    },
    {
        SDLK_F4, MKV_F4,
    },
    {
        SDLK_F5, MKV_F5,
    },
    {
        SDLK_F6, MKV_F6,
    },
    {
        SDLK_F7, MKV_F7,
    },
    {
        SDLK_F8, MKV_F8,
    },
    {
        SDLK_F9, MKV_F9,
    },
    {
        SDLK_F10, MKV_F10,
    },
    {
        SDLK_F11, MKV_F11,
    },
    {
        SDLK_F12, MKV_F12,
    },
    {
        SDLK_F13, MKV_F13,
    },
    {
        SDLK_F14, MKV_F14,
    },
    {
        SDLK_F15, MKV_F15,
    },
    {
        SDLK_PAUSE, MKV_PAUSE,
    },
    {
        SDLK_NUMLOCK, MKV_NUMCLEAR,
    },
    {
        SDLK_UP, MKV_UPARROW,
    },
    {
        SDLK_DOWN, MKV_DOWNARROW,
    },
    {
        SDLK_RIGHT, MKV_RIGHTARROW,
    },
    {
        SDLK_LEFT, MKV_LEFTARROW,
    },
    {
        SDLK_INSERT, MKV_HELP,
    },
    {
        SDLK_HOME, MKV_HOME,
    },
    {
        SDLK_END, MKV_END,
    },
    {
        SDLK_PAGEUP, MKV_PAGEUP,
    },
    {
        SDLK_PAGEDOWN, MKV_PAGEDOWN,
    },
    {
        SDLK_CAPSLOCK, MKV_CAPS,
    },
    {
        SDLK_SCROLLOCK, MKV_SCROLL_LOCK,
    },
    {
        SDLK_RSHIFT, MKV_RIGHTSHIFT,
    },
    {
        SDLK_LSHIFT, MKV_LEFTSHIFT,
    },
    {
        SDLK_RCTRL, MKV_RIGHTCNTL,
    },
    {
        SDLK_LCTRL, MKV_LEFTCNTL,
    },
    {
        SDLK_RALT, MKV_RIGHTOPTION,
    },
    {
        SDLK_LALT, MKV_CLOVER,
    },
    {
        SDLK_RMETA, MKV_RIGHTOPTION,
    },
    {
        SDLK_LMETA, MKV_CLOVER,
    },
    {
        SDLK_HELP, MKV_HELP,
    },
    {
        SDLK_PRINT, MKV_PRINT_SCREEN,
    },
    {
        SDLK_SYSREQ, NOTAKEY,
    },
    {
        SDLK_MENU, NOTAKEY,
    },
    {
        SDLK_BREAK, NOTAKEY,
    },
};

static void
init_sdlk_to_mkv(void)
{
    static bool been_here = false;

    if(!been_here)
    {
        unsigned int i;

        for(i = 0; i < std::size(sdlk_to_mkv); ++i)
            sdlk_to_mkv[i] = NOTAKEY;

        for(i = 0; i < std::size(map); ++i)
        {
            SDLKey sdlk;
            unsigned char mkv;

            sdlk = map[i].sdlk;
            mkv = map[i].mkv;
            sdlk_to_mkv[sdlk] = mkv;
        }
        been_here = true;
    }
}

#endif

#include "sdlevents.h"
#include "sdlscrap.h"
#include "syswm_map.h"


/* This function runs in a separate thread (usually) */
int sdl_event_interrupt(const SDL_Event *event)
{
    if(event->type == SDL_QUIT)
    {
        /* Query whether or not we should quit */
     //   if(!sdl_really_quit())
      //      return (0);
    }
    else if(event->type == SDL_SYSWMEVENT)
    {
        /* Pass it to a system-specific event handler */
        return (sdl_syswm_event(event));
    }
    return (1);
}

void sdl_events_init(void)
{
    /* then set up a filter that triggers the event interrupt */
    SDL_SetEventFilter(sdl_event_interrupt);
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
}


void SDLVideoDriver::runEventLoop()
{
    SDL_Event event;

    for(;;)
    {
        SDL_WaitEvent(&event);

        switch(event.type)
        {
            case SDL_ACTIVEEVENT:
                {
                    if(event.active.state & SDL_APPINPUTFOCUS)
                    {
                        if(!event.active.gain)
                            callbacks_->suspendEvent();
                        else
                        {
                            if(!we_lost_clipboard())
                                callbacks_->resumeEvent(false);
                            else
                            {
                                ZeroScrap();
                                callbacks_->resumeEvent(true);
                            }
                        }
                    }
                }
                break;

            case SDL_MOUSEMOTION:
                callbacks_->mouseMoved(event.motion.x, event.motion.y);
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if(event.button.button == 4 || event.button.button == 5)
                {
                    /* SDL 1.2 reports wheel motion as buttons 4 and 5. */
                    if(std::getenv("EXECUTOR_WHEEL_DEBUG"))
                        fprintf(stderr, "[wheel] sdl1 button=%u type=%u\n",
                                (unsigned)event.button.button,
                                (unsigned)event.type);
                    if(event.type == SDL_MOUSEBUTTONDOWN)
                        callbacks_->mouseWheelEvent(0,
                                                    event.button.button == 4 ? 1 : -1,
                                                    event.button.x, event.button.y);
                }
                else
                    callbacks_->mouseButtonEvent(event.button.state == SDL_PRESSED, event.button.x, event.button.y);
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                {
                    unsigned char mkvkey;

                    init_sdlk_to_mkv();

                    if(ROMlib_use_scan_codes)
                        mkvkey = ibm_virt_to_mac_virt[event.key.keysym.scancode];
                    else
                        mkvkey = sdlk_to_mkv[event.key.keysym.sym];
                    
                    callbacks_->keyboardEvent(event.key.state == SDL_PRESSED, mkvkey);
                }
                break;

            case SDL_QUIT:
                callbacks_->requestQuit();
                break;
        }

        std::lock_guard lk(mutex_);
        if(quit_)
            return;
        if(!todos_.empty())
        {
            for(const auto& f : todos_)
                f();
            todos_.clear();
            done_.notify_all();
        }
        if(!dirtyRects_.empty())
        {
            auto rects = dirtyRects_.getAndClear();
            SDL_Rect sdlRects[DirtyRects::MAX_DIRTY_RECTS];

            for(int i = 0; i < rects.size(); i++)
            {
                const auto& r = rects[i];
                SDL_Rect& sdlR = sdlRects[i];
                sdlR.x = r.left;
                sdlR.y = r.top;
                sdlR.w = r.right - r.left;
                sdlR.h = r.bottom - r.top;

            }
            SDL_UpdateRects(screen, rects.size(), sdlRects);
        }
    }
}


void SDLVideoDriver::endEventLoop()
{
    std::lock_guard lk(mutex_);
    requestUpdate();
    quit_ = true;
}


void SDLVideoDriver::requestUpdate()
{
    SDL_Event evt;
    evt.type = SDL_USEREVENT;
    SDL_PushEvent(&evt);
}

void SDLVideoDriver::onMainThread(std::function<void()> f)
{
    std::unique_lock lk(mutex_);
    todos_.push_back(f);
    requestUpdate();

    done_.wait(lk, [this] { return todos_.empty(); });
}
