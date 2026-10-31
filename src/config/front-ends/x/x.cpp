/* Copyright 1994, 1995, 1996 by Abacus Research and
 * Development, Inc.  All rights reserved.
 */

#include "x.h"
#include <base/common.h>
#include <QuickDraw.h>
#include <ScrapMgr.h>
#include <OSUtil.h>

#include <vdriver/dirtyrect.h>
#include <rsys/keyboard.h>

#include <assert.h>

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>

#include <X11/X.h>
#include <X11/Xlib.h>
/* declares a data type `Region' */
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>

#include <CQuickDraw.h>
#include <MemoryMgr.h>
#include <OSEvent.h>
#include <EventMgr.h>
#include <ToolboxEvent.h>
#include <rsys/adb.h>

#include <quickdraw/cquick.h>
#include <prefs/prefs.h>
#include <mman/mman.h>
#include <vdriver/refresh.h>
#include <vdriver/vdriver.h>
#include <syn68k_public.h>
#include <base/m68kint.h>
#include <quickdraw/depthconv.h>
#include <quickdraw/rgbutil.h>
#include <commandline/flags.h>
#include <osevent/osevent.h>

#include "x_keycodes.h"
#include "x_keysym.h"

#include <atomic>

namespace Executor {
        // FIXME: not including rsys/paths.h because that pulls in boost/filesystem, which CMake doesn't know about yet
        // (the dependencies are the wrong way aroudn anyway)
    extern std::string ROMlib_appname;
}

using namespace Executor;

static int x_fd = -1;

#define EXECUTOR_WINDOW_EVENT_MASK (KeyPressMask | KeyReleaseMask         \
                                    | ButtonPressMask | ButtonReleaseMask \
                                    | FocusChangeMask   \
                                    | ExposureMask | PointerMotionMask    \
                                    )

void cursor_init(void);

/* visual we are using the for x_window */
XVisualInfo *visual;

/* x window being driven */
static Window x_window;

static XImage *x_x_image;
static unsigned char *x_fbuf;
static int x_fbuf_bpp;
static int x_fbuf_row_bytes;

/* max dimensions */
static int x_image_width, x_image_height;   // fixme: this hurts

/* x cursor stuff */
static ::Cursor x_hidden_cursor, x_cursor = -1;

static XImage *x_image_cursor_data, *x_image_cursor_mask;
static Pixmap pixmap_cursor_data, pixmap_cursor_mask;

static GC copy_gc, cursor_data_gc, cursor_mask_gc;

static int cursor_visible_p = false;

static Atom x_selection_atom;
static Atom wmDeleteMessage;

static char *selectiontext = nullptr;
static int selectionlength;

/* true if we should turn of autorepeat when the pointer is the
   executor window */
static int frob_autorepeat_p = false;


Display *x_dpy;
static int x_screen;

static int have_shm, shm_err = false;

static int shm_major_opcode;
static int shm_first_event;
static int shm_first_error;

static int wakeup_fd[2] = {-1, -1};
static std::atomic_bool exitMainLoop = false;
static bool updateRequested = false;

int x_error_handler(Display *err_dpy, XErrorEvent *err_evt)
{
    if(err_evt->request_code == shm_major_opcode)
        shm_err = true;
    else
    {
        char error_text[256];
        char t[256];

        XGetErrorText(err_dpy, err_evt->error_code, error_text, 256);
        fprintf(stderr, "%s: XError `%d': %s\n", ROMlib_appname.c_str(),
                err_evt->error_code, error_text);

        sprintf(t, "%d", err_evt->request_code);
        XGetErrorDatabaseText(x_dpy, "XRequest", t, "",
                              error_text, sizeof error_text);
        fprintf(stderr, "%s\n", error_text);

        exit(EXIT_FAILURE);
    }
    /* what do x error handlers return? */
    return 0;
}


void alloc_x_image(int bpp, int width, int height,
                   int *row_bytes_return,
                   XImage **x_image_return, unsigned char **fbuf_return,
                   int *bpp_return)
{
    /* return value */
    /* assignments to shut up gcc */
    XImage *x_image = nullptr;
    int row_bytes = 0;
    unsigned char *fbuf = nullptr;
    int resultant_bpp = 0;

    if(have_shm)
    {
        XShmSegmentInfo *shminfo;

        /* note: this memory doesn't get reclaimed */
        shminfo = (XShmSegmentInfo *)malloc(sizeof *shminfo);
        memset(shminfo, '\0', sizeof *shminfo);

        x_image = XShmCreateImage(x_dpy, visual->visual,
                                  bpp, ZPixmap, (char *)0,
                                  shminfo, width, height);
        if(x_image)
        {
            row_bytes = x_image->bytes_per_line;
            resultant_bpp = x_image->bits_per_pixel;
            shminfo->shmid = shmget(IPC_PRIVATE,
                                    row_bytes * height,
                                    IPC_CREAT | 0777);
            if(shminfo->shmid < 0)
            {
                XDestroyImage(x_image);
                free(shminfo);
                x_image = 0;
            }
            else
            {
                fbuf = (unsigned char *)(shminfo->shmaddr = x_image->data
                                         = (char *)shmat(shminfo->shmid, 0, 0));
                if(fbuf == (unsigned char *)-1)
                {
                    /* do we need to delete the shmid here? */
                    free(shminfo);
                    XDestroyImage(x_image);
                    fbuf = nullptr;
                }
                else
                {
                    shminfo->readOnly = False;
                    XShmAttach(x_dpy, shminfo);
                    XSync(x_dpy, False);
                    if(shm_err)
                    {
                        /* do we need to delete the shmid here? */
                        free(shminfo);
                        XDestroyImage(x_image);
                        fbuf = nullptr;
                        /* reset the shm_err flag */
                        shm_err = false;
                    }
                    else
                    {
                        shmctl(shminfo->shmid, IPC_RMID, 0);
                    }
                }
            }
        }
    }

    if(fbuf == nullptr)
    {
        have_shm = false;

        warning_unexpected("not using shared memory");
        row_bytes = ((width * 32 + 31) / 32) * 4;
        fbuf = (unsigned char *)calloc(row_bytes * height, 1);
        x_image = XCreateImage(x_dpy, visual->visual, bpp, ZPixmap, 0,
                               (char *)fbuf, width, height, 32, row_bytes);
        resultant_bpp = x_image->bits_per_pixel;
    }

        // FIXME: this should be host byte order
    x_image->byte_order = LSBFirst;
    x_image->bitmap_bit_order = LSBFirst;

    *fbuf_return = fbuf;
    *row_bytes_return = row_bytes;
    *x_image_return = x_image;
    *bpp_return = resultant_bpp;

    x_image_width = width;
    x_image_height = height;
}


/* convert x keysym to mac virtual `keywhat'; return true the
   conversion was successful */

static bool
x_keysym_to_mac_virt(unsigned int keysym, unsigned char *virt_out)
{
    key_table_t *table;
    uint8_t keysym_high_byte, keysym_low_byte;
    int16_t mkvkey;
    int i;

    if(ROMlib_use_scan_codes)
    {
        if(keysym == 0xff)
            mkvkey = NOTKEY;
        else
            mkvkey = keysym;
    }
    else
    {
        keysym_high_byte = (keysym >> 8) & 0xFF;
        keysym_low_byte = keysym & 0xFF;

        /* switch off the high 8bits of the keysym to determine which table
	 to index into */
        for(i = 0; key_tables[i].data; i++)
        {
            table = &key_tables[i];
            if(table->high_byte == keysym_high_byte)
                break;
        }
        if(key_tables[i].data == nullptr)
            return false;

        if(keysym_low_byte < table->min
           || keysym_low_byte > (table->min + table->size))
            return false;

        mkvkey = table->data[keysym_low_byte - table->min];
    }

    if(mkvkey == NOTKEY)
        return false;

    mkvkey = ROMlib_right_to_left_key_map(mkvkey);

    *virt_out = mkvkey;
    return true;
}

X11VideoDriver::X11VideoDriver(Executor::IEventListener *listener, int& argc, char* argv[])
    : VideoDriver(listener)
{
    x_dpy = XOpenDisplay("");
    if(x_dpy == nullptr)
    {
        fprintf(stderr, "%s: could not open x server `%s'.\n",
                ROMlib_appname.c_str(), XDisplayName(""));
        exit(EXIT_FAILURE);
    }
    x_screen = XDefaultScreen(x_dpy);

    XSetErrorHandler(x_error_handler);

    /* determine if the server supports the `XShm' extension */
    have_shm = XQueryExtension(x_dpy, "MIT-SHM",
                               &shm_major_opcode,
                               &shm_first_event,
                               &shm_first_error);



    XVisualInfo *visuals, vistemplate;


    int n_visuals;
    vistemplate.screen = x_screen;
    vistemplate.depth = 8;
    vistemplate.c_class = TrueColor;

    visuals = XGetVisualInfo(x_dpy,
                                (VisualScreenMask | VisualClassMask),
                                &vistemplate, &n_visuals);

    visual = nullptr;
    for(int i = 0; i < n_visuals; i++)
    {
        if(visuals[i].depth >= 24)
        {
            visual = &visuals[i];
            x_fbuf_bpp = visual->depth;
            break;
        }
    }

    if(visual == nullptr)
    {
        fprintf(stderr, "%s: no acceptable visual found, exiting.\n",
                ROMlib_appname.c_str());
        exit(EXIT_FAILURE);
    }

    cursor_init();

    x_selection_atom = XInternAtom(x_dpy, "ROMlib_selection", False);
    
    x_fd = XConnectionNumber(x_dpy);
    pipe(wakeup_fd);
}

void X11VideoDriver::alloc_x_window(int width, int height, int bpp, bool grayscale_p)
{
    XSetWindowAttributes xswa;
    XSizeHints size_hints = {};
    XGCValues gc_values;

    int x = 0, y = 0;

    size_hints.flags = PSize | PMinSize | PMaxSize | PBaseSize | USSize;

    size_hints.min_width = size_hints.max_width
        = size_hints.base_width = size_hints.width = width;
    size_hints.min_height = size_hints.max_height
        = size_hints.base_height = size_hints.height = height;

    size_hints.x = x;
    size_hints.y = y;


    /* create the executor window */
    x_window = XCreateWindow(x_dpy, XRootWindow(x_dpy, x_screen),
                             x, y, width, height,
                             0, 0, InputOutput, CopyFromParent, 0, &xswa);

    XDefineCursor(x_dpy, x_window, x_hidden_cursor);

    gc_values.function = GXcopy;
    gc_values.foreground = XBlackPixel(x_dpy, x_screen);
    gc_values.background = XWhitePixel(x_dpy, x_screen);
    gc_values.plane_mask = AllPlanes;

    copy_gc = XCreateGC(x_dpy, x_window,
                        (GCFunction | GCForeground
                         | GCBackground | GCPlaneMask),
                        &gc_values);

    {
        /* various hints for `XSetWMProperties ()' */
        XWMHints wm_hints;
        XClassHint class_hint;
        XTextProperty name;

        memset(&wm_hints, 0, sizeof wm_hints);

        class_hint.res_name = "executor";
        class_hint.res_class = "Executor";

        char *program_name = const_cast<char*>(ROMlib_appname.c_str());
        XStringListToTextProperty(&program_name, 1, &name);

        XSetWMProperties(x_dpy, x_window,
                         &name, &name, /* _argv, *_argc, */ nullptr, 0,
                         &size_hints, &wm_hints, &class_hint);
    }
    wmDeleteMessage = XInternAtom(x_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(x_dpy, x_window, &wmDeleteMessage, 1);

    XSelectInput(x_dpy, x_window, EXECUTOR_WINDOW_EVENT_MASK);

    XMapRaised(x_dpy, x_window);
    XClearWindow(x_dpy, x_window);
    XFlush(x_dpy);
}

::Cursor
create_x_cursor(char *data, char *mask,
                int hotspot_x, int hotspot_y)
{
    ::Cursor retval;
    char x_mask[32], x_data[32];
    int i;

    static XColor x_black = { 0, 0, 0, 0 },
                  x_white = { (unsigned long)~0,
                              (unsigned short)~0,
                              (unsigned short)~0,
                              (unsigned short)~0 };

    for(i = 0; i < 32; i++)
    {
        x_mask[i] = data[i] | mask[i];
        x_data[i] = data[i];
    }

    x_image_cursor_data->data = x_data;
    x_image_cursor_mask->data = x_mask;

    XPutImage(x_dpy, pixmap_cursor_data, cursor_data_gc, x_image_cursor_data,
              0, 0, 0, 0, 16, 16);
    XPutImage(x_dpy, pixmap_cursor_mask, cursor_mask_gc, x_image_cursor_mask,
              0, 0, 0, 0, 16, 16);

    if(hotspot_x < 0)
        hotspot_x = 0;
    else if(hotspot_x > 16)
        hotspot_x = 16;

    if(hotspot_y < 0)
        hotspot_y = 0;
    else if(hotspot_y > 16)
        hotspot_y = 16;

    retval = XCreatePixmapCursor(x_dpy, pixmap_cursor_data, pixmap_cursor_mask,
                                 &x_black, &x_white,
                                 hotspot_x, hotspot_y);
    return retval;
}

void X11VideoDriver::setCursor(char *cursor_data,
                              uint16_t cursor_mask[16],
                              int hotspot_x, int hotspot_y)
{
    std::lock_guard lk(mutex_);
    ::Cursor orig_x_cursor = x_cursor;

    x_cursor = create_x_cursor(cursor_data, (char *)cursor_mask,
                               hotspot_x, hotspot_y);

    /* if visible, set `x_cursor' to be the current cursor */
    if(cursor_visible_p)
        XDefineCursor(x_dpy, x_window, x_cursor);

    if(orig_x_cursor != (::Cursor)-1)
        XFreeCursor(x_dpy, orig_x_cursor);
}

void X11VideoDriver::setCursorVisible(bool show_p)
{
    std::lock_guard lk(mutex_);
    if(cursor_visible_p != show_p)
        XDefineCursor(x_dpy, x_window, show_p ? x_cursor : x_hidden_cursor);
    cursor_visible_p = show_p;
}

void cursor_init(void)
{
    XGCValues gc_values;
    static char zero[2 * 16] = {
        0,
    };

    /* the following are used to create x cursors, they must
     be done before calling `create_x_cursor ()' */
    x_image_cursor_data = XCreateImage(x_dpy, XDefaultVisual(x_dpy, x_screen),
                                       1, XYBitmap, 0, nullptr, 16, 16, 8, 2);
    x_image_cursor_mask = XCreateImage(x_dpy, XDefaultVisual(x_dpy, x_screen),
                                       1, XYBitmap, 0, nullptr, 16, 16, 8, 2);

    x_image_cursor_data->byte_order = MSBFirst;
    x_image_cursor_mask->byte_order = MSBFirst;

    x_image_cursor_data->bitmap_bit_order = MSBFirst;
    x_image_cursor_mask->bitmap_bit_order = MSBFirst;

    pixmap_cursor_data = XCreatePixmap(x_dpy, XRootWindow(x_dpy, x_screen),
                                       16, 16, 1);
    pixmap_cursor_mask = XCreatePixmap(x_dpy, XRootWindow(x_dpy, x_screen),
                                       16, 16, 1);

    gc_values.function = GXcopy;
    gc_values.foreground = ~0;
    gc_values.background = 0;

    gc_values.plane_mask = AllPlanes;

    cursor_data_gc = XCreateGC(x_dpy, pixmap_cursor_data,
                               (GCFunction | GCForeground
                                | GCBackground | GCPlaneMask),
                               &gc_values);
    cursor_mask_gc = XCreateGC(x_dpy, pixmap_cursor_mask,
                               (GCFunction | GCForeground
                                | GCBackground | GCPlaneMask),
                               &gc_values);

    x_hidden_cursor = create_x_cursor(zero, zero, 0, 0);
}

void X11VideoDriver::render(std::unique_lock<std::mutex>& lk, std::optional<DirtyRects::Rect> extra)
{
    auto rects = dirtyRects_.getAndClear();
    
    if(!rects.empty())
    {
        lk.unlock();
        updateBuffer(framebuffer_, (uint32_t*)x_fbuf, x_image_width, x_image_height, rects);
        lk.lock();
    }

    auto draw = [this](const DirtyRects::Rect& r) {
        int x = r.left;
        int y = r.top;
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        if(have_shm)
            XShmPutImage(x_dpy, x_window, copy_gc, x_x_image,
                    x, y, x, y, w, h, False);
        else
            XPutImage(x_dpy, x_window, copy_gc, x_x_image,
                    x, y, x, y, w, h);
    };
    
    for(const auto& r : rects)
        draw(r);
    if(extra)
        draw(*extra);
    if(!rects.empty() || extra)
        XFlush(x_dpy);
}


X11VideoDriver::~X11VideoDriver()
{
    if(x_dpy)
        XCloseDisplay(x_dpy);
}

bool X11VideoDriver::setMode(int width, int height, int bpp, bool grayscale_p)
{
    std::lock_guard lk(mutex_);

    if(width == 0)
        width = framebuffer_.width;
    if(height == 0)
        height = framebuffer_.height;
    if(bpp == 0)
        bpp = framebuffer_.bpp;

    if(width == 0)
        width = VDRIVER_DEFAULT_SCREEN_WIDTH;
    if(height == 0)
        height = VDRIVER_DEFAULT_SCREEN_HEIGHT;
    if(bpp == 0)
        bpp = 8;

    if(width < VDRIVER_MIN_SCREEN_WIDTH)
        width = VDRIVER_MIN_SCREEN_WIDTH;
    if(height < VDRIVER_MIN_SCREEN_HEIGHT)
        height = VDRIVER_MIN_SCREEN_HEIGHT;

    if(!x_x_image)
    {
        alloc_x_image(x_fbuf_bpp, width, height,
                        &x_fbuf_row_bytes, &x_x_image, &x_fbuf,
                        &x_fbuf_bpp);
    }
    else
    {
        if(width > x_image_width)
            width = x_image_width;
        if(height > x_image_height)
            height = x_image_height;
    }

    if(!x_window)
    {
        alloc_x_window(width, height, bpp, grayscale_p);
        framebuffer_ = Framebuffer(width, height, bpp);
        framebuffer_.grayscale = grayscale_p;
        return true;
    }

    if(!isAcceptableMode(width, height, bpp, grayscale_p))
        return false;

    if(width != framebuffer_.width
       || height != framebuffer_.height)
    {
        /* resize; the event code will deal with things when the resize
	 event comes through */
        XResizeWindow(x_dpy, x_window, width, height);
    }

    framebuffer_ = Framebuffer(width, height, bpp);
    framebuffer_.grayscale = grayscale_p;
    
    return true;
}

void X11VideoDriver::handleEvents()
{
    std::unique_lock lk(mutex_);
    while(XPending(x_dpy))
    {
        XEvent evt;
        XNextEvent(x_dpy, &evt);

        switch(evt.type)
        {
            case SelectionRequest:
            {
                Atom property;
                XSelectionEvent xselevt;

                property = evt.xselectionrequest.property;
                if(property == None)
                    property = x_selection_atom;

                XChangeProperty(x_dpy, evt.xselectionrequest.requestor, property,
                                XA_STRING, (int)8, (int)PropModeReplace,
                                (unsigned char *)selectiontext, selectionlength);

                xselevt.type = SelectionNotify;
                xselevt.requestor = evt.xselectionrequest.requestor; /* check this */
                xselevt.selection = evt.xselectionrequest.selection;
                xselevt.target = evt.xselectionrequest.target;
                xselevt.property = property;
                xselevt.time = evt.xselectionrequest.time;

                XSendEvent(x_dpy, xselevt.requestor, False, 0L,
                           (XEvent *)&xselevt);

                break;
            }
            case KeyPress:
            case KeyRelease:
            {
                unsigned keysym;
                unsigned char virt;

                if(ROMlib_use_scan_codes)
                {
                    uint8_t keycode;

                    keycode = evt.xkey.keycode;
                    if(keycode < std::size(x_keycode_to_mac_virt))
                        keysym = x_keycode_to_mac_virt[keycode];
                    else
                        keysym = NOTKEY;
                }
                else
                {
                    keysym = XLookupKeysym(&evt.xkey, 0);

                    /* This hack is because the default is to map BACKSPACE to
		   the same key that DELETE produces just because people
		   don't like BACKSPACE generating a ^H ... yahoo! */

                    if(keysym == 0xFFFF && evt.xkey.keycode == 0x16)
                        keysym = 0xFF08;

                    /* This hack is because the default is to map Scroll Lock
		   and Right-Alt to the same key.  I'm not sure why */

                    if(keysym == 0xFF7E && evt.xkey.keycode == 0x4e)
                        keysym = 0xFF7D;
                }

                if(x_keysym_to_mac_virt(keysym, &virt))
                {
                    callbacks_->keyboardEvent(evt.type == KeyPress, virt);
                }
                break;
            }
            case ButtonPress:
            case ButtonRelease:
            {
                unsigned button = evt.xbutton.button;

                /* Core X11 maps wheel motion to buttons 4-7.  These are not
                 * real button presses and must not become guest clicks at the
                 * pointer. */
                if(button >= 4 && button <= 7)
                {
                    if(evt.type == ButtonPress)
                    {
                        int dx = 0, dy = 0;

                        if(getenv("EXECUTOR_WHEEL_DEBUG"))
                            fprintf(stderr, "[wheel] x button=%u at %d,%d\n",
                                    button, evt.xbutton.x, evt.xbutton.y);

                        switch(button)
                        {
                            case 4: /* wheel up */
                                dy = 1;
                                break;
                            case 5: /* wheel down */
                                dy = -1;
                                break;
                            case 6: /* wheel left */
                                dx = -1;
                                break;
                            case 7: /* wheel right */
                                dx = 1;
                                break;
                        }
                        callbacks_->mouseWheelEvent(dx, dy,
                                                    evt.xbutton.x, evt.xbutton.y);
                    }
                    break;
                }

                callbacks_->mouseButtonEvent(evt.type == ButtonPress, evt.xbutton.x, evt.xbutton.y);
                break;
            }
            case Expose:
                render(lk, DirtyRects::Rect{
                    evt.xexpose.y, evt.xexpose.x,
                    evt.xexpose.y + evt.xexpose.height,
                    evt.xexpose.x + evt.xexpose.width});
                break;
            case FocusIn:
                if(frob_autorepeat_p)
                    XAutoRepeatOff(x_dpy);
                {
                    bool cvt;
                    Window selection_owner;

                    selection_owner = XGetSelectionOwner(x_dpy, XA_PRIMARY);
                    cvt = selection_owner != None && selection_owner != x_window;
                    if(cvt)
                        ZeroScrap();
                    callbacks_->resumeEvent(cvt);
                }
                break;
            case FocusOut:
                if(frob_autorepeat_p)
                    XAutoRepeatOn(x_dpy);
                callbacks_->suspendEvent();
                break;
            case MotionNotify:
                callbacks_->mouseMoved(evt.xmotion.x, evt.xmotion.y);
                break;
            case ClientMessage:
                if (evt.xclient.data.l[0] == wmDeleteMessage)
                    callbacks_->requestQuit();
                break;
        }
    }
}

void X11VideoDriver::runEventLoop()
{
    while(!exitMainLoop)
    {
        fd_set fds;
        
        FD_ZERO(&fds);
        FD_SET(x_fd, &fds);
        FD_SET(wakeup_fd[0], &fds);
        select(std::max(x_fd, wakeup_fd[0]) + 1, &fds, 0, 0, nullptr);

        handleEvents();

        if(FD_ISSET(wakeup_fd[0], &fds))
        {
            unsigned char b;
            read(wakeup_fd[0], &b, 1);
        }

        std::unique_lock lk(mutex_);
        updateRequested = false;
        render(lk, {});
    }
}

void X11VideoDriver::endEventLoop()
{
    exitMainLoop = true;
    unsigned char b = 42;
    write(wakeup_fd[1], &b, 1);
}

void X11VideoDriver::requestUpdate()
{
    if(!updateRequested)
    {
        unsigned char b = 42;
        write(wakeup_fd[1], &b, 1);
        updateRequested = true;
    }
}


void X11VideoDriver::beepAtUser(void)
{
    std::lock_guard lk(mutex_);
    /* 50 for now */
    XBell(x_dpy, 0);
}

void X11VideoDriver::putScrap(OSType type, LONGINT length, char *p, int scrap_count)
{
    std::lock_guard lk(mutex_);
    if(type == "TEXT"_4)
    {
        if(selectiontext)
            free(selectiontext);
        selectiontext = (char *)malloc(length);
        if(selectiontext)
        {
            selectionlength = length;
            memcpy(selectiontext, p, length);
            {
                char *ip, *ep;

                for(ip = selectiontext, ep = ip + length; ip < ep; ++ip)
                    if(*ip == '\r')
                        *ip = '\n';
            }
            XSetSelectionOwner(x_dpy, XA_PRIMARY, x_window, CurrentTime);
        }
    }
}

void X11VideoDriver::weOwnScrap(void)
{
    std::lock_guard lk(mutex_);
    XSetSelectionOwner(x_dpy, XA_PRIMARY, x_window, CurrentTime);
}

int X11VideoDriver::getScrap(OSType type, Handle h)
{
    std::lock_guard lk(mutex_);
    int retval;

    retval = -1;
    if(type == "TEXT"_4)
    {
        Window selection_owner;

        selection_owner = XGetSelectionOwner(x_dpy, XA_PRIMARY);
        if(selection_owner != None && selection_owner != x_window)
        {
            XEvent xevent;
            Atom rettype;
            LONGINT actfmt;
            unsigned long nitems, ul;
            unsigned long nbytesafter;
            unsigned char *propreturn;
            int i;
            Ptr p;

            XConvertSelection(x_dpy, XA_PRIMARY, XA_STRING, None, x_window,
                              CurrentTime);
            for(i = 10;
                (--i >= 0
                 && !XCheckTypedEvent(x_dpy, SelectionNotify, &xevent));
                Delay(10L, nullptr))
                ;
            if(i >= 0 && xevent.xselection.property != None)
            {
                XGetWindowProperty(x_dpy, xevent.xselection.requestor,
                                   xevent.xselection.property, 0L, 16384L,
                                   (LONGINT)True, XA_STRING, &rettype, &actfmt,
                                   &nitems, &nbytesafter, &propreturn);
                if(rettype == XA_STRING && actfmt == 8)
                {
                    SetHandleSize((Handle)h, nitems);
                    if(LM(MemErr) == noErr)
                    {
                        memcpy(*h, propreturn, nitems);
                        for(ul = nitems, p = *h; ul > 0; ul--)
                            if(*p++ == '\n')
                                p[-1] = '\r';
                        retval = nitems;
                    }
                }
                XFree((char *)propreturn);
            }
        }
    }
    return retval;
}

void X11VideoDriver::setTitle(const std::string& newtitle)
{
    std::lock_guard lk(mutex_);
    XSizeHints xsh;

    memset(&xsh, 0, sizeof xsh);
    char *newtitle_c = const_cast<char*>(newtitle.c_str());
    XSetStandardProperties(x_dpy, x_window, newtitle_c, newtitle_c, None,
                           nullptr, 0, &xsh);
}
