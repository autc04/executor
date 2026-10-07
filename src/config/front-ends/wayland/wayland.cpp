#include "wayland.h"
#include <../x/x_keycodes.h>

#include <QuickDraw.h>
#include <quickdraw/region.h>

#include <iostream>
#include <algorithm>

#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <sys/eventfd.h>
#include <poll.h>


using namespace wayland;
using namespace Executor;
using namespace std::chrono_literals;

/* How often the event loop wakes to re-evaluate the guest's captured state, so
 * that capture starts even while the guest never yields. */
static constexpr int kResponsivenessPollMs = 100;

template<typename... Args>
std::shared_ptr<std::tuple<Args...>> argCollector(std::function<void (Args...)>& f)
{
    auto args = std::make_shared<std::tuple<Args...>>();
    f = [args](Args... arg) {
        *args = { arg... };
    };
    return args;
}

WaylandVideoDriver::SharedMem::SharedMem(size_t size)
    : size_(size)
{
    fd_ = memfd_create("executor", MFD_CLOEXEC);
    ftruncate(fd_, size_);
    mem_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
}

WaylandVideoDriver::SharedMem::~SharedMem()
{
    if(mem_)
    {
        munmap(mem_, size_);
        close(fd_);
    }
}

WaylandVideoDriver::Buffer::Buffer(shm_t& shm, int w, int h)
    : mem_(w * h * 4), 
    wlbuffer_(shm.create_pool(mem_.fd(), mem_.size())
            .create_buffer(0, w, h, w*4, wayland::shm_format::argb8888)),
    width_(w), height_(h)
{
    std::fill(data(), data() + w * h, 0);
}

WaylandVideoDriver::WaylandVideoDriver(Executor::IEventListener *eventListener, int& argc, char* argv[])
    : VideoDriver(eventListener)
{
    wakeFd_ = eventfd(0, 0);

    registry_ = display_.get_registry();
    registry_.on_global() = [this] (uint32_t name, const std::string& interface, uint32_t version) {
        if(interface == compositor_t::interface_name)
            registry_.bind(name, compositor_, version);
        else if(interface == xdg_wm_base_t::interface_name)
            registry_.bind(name, xdg_wm_base_, version);
        else if(interface == seat_t::interface_name)
            registry_.bind(name, seat_, version);
        else if(interface == shm_t::interface_name)
            registry_.bind(name, shm_, version);
        // TODO: handle changes
    };
    display_.roundtrip();

    xdg_wm_base_.on_ping() = [this] (uint32_t serial) { xdg_wm_base_.pong(serial); };

    surface_ = compositor_.create_surface();
    xdg_surface_ = xdg_wm_base_.get_xdg_surface(surface_);
    xdg_toplevel_ = xdg_surface_.get_toplevel();
    xdg_toplevel_.set_title("Executor");
    xdg_toplevel_.set_app_id("io.github.autc04.executor");
    xdg_toplevel_.set_min_size(VDRIVER_MIN_SCREEN_WIDTH, VDRIVER_MIN_SCREEN_HEIGHT);

    auto toplevelConfigureArgs = argCollector(xdg_toplevel_.on_configure());
    xdg_surface_.on_configure() = [this, toplevelConfigureArgs] (uint32_t serial) {
        auto [inWidth,inHeight,inStates] = *toplevelConfigureArgs;

        std::cout << "configure " << inWidth << " " << inHeight << " " << serial << std::endl;

        int width = inWidth ? std::max(inWidth, VDRIVER_MIN_SCREEN_WIDTH) : configuredShape_.width;
        int height = inHeight ? std::max(inHeight, VDRIVER_MIN_SCREEN_HEIGHT) : configuredShape_.height;
        std::vector<xdg_toplevel_state> states = inStates;

        bool maximized = std::find(states.begin(), states.end(), xdg_toplevel_state::maximized) != states.end();
        bool activated = std::find(states.begin(), states.end(), xdg_toplevel_state::activated) != states.end();

        if(activated != activated_)
        {
            if(activated)
                callbacks_->resumeEvent(true);
            else 
                callbacks_->suspendEvent();
            activated_ = activated;
        }

        if(!maximized && configuredShape_.maximized)
        {
            if(!inWidth)
            {
                width = std::max(VDRIVER_MIN_SCREEN_WIDTH, committedShape_.width / 2);
                height = std::max(VDRIVER_MIN_SCREEN_HEIGHT, committedShape_.height / 2);
            }
        }

        std::lock_guard lk(mutex_);
        
        configuredShape_ = { serial, width, height, maximized };
        
        if(state_ == State::idle || state_ == State::unconfigured)
        {
            state_ = State::waitingForModeSwitch;
            stateChanged_.notify_all();
        }
        else if(state_ == State::waitingForUpdate)
        {
            state_ = State::waitingForObsoleteUpdate;
            stateChanged_.notify_all();
        }
        if(state_ == State::waitingForModeSwitch)
            callbacks_->wake();
    };

    xdg_toplevel_.on_close() = [this] () {
        callbacks_->requestQuit();
    };

    pointer_ = seat_.get_pointer();
    pointer_.on_button() = [this] (uint32_t serial, uint32_t time, uint32_t button, pointer_button_state state) {
        if(button == BTN_LEFT)
        {
            if(state == pointer_button_state::pressed)
                lastMouseDownSerial_ = serial;

            if(state == pointer_button_state::pressed
                && !committedShape_.maximized
                && mouseX_ > committedShape_.width - 16 && mouseY_ > committedShape_.height - 16)
            {
                xdg_toplevel_.resize(seat_, serial, xdg_toplevel_resize_edge::bottom_right);
            }
            else
                callbacks_->mouseButtonEvent(state == pointer_button_state::pressed);
        }
    };
    pointer_.on_motion() = [this] (uint32_t serial, double x, double y) {
        mouseX_ = x;
        mouseY_ = y;
        callbacks_->mouseMoved(x, y);
    };

    keyboard_ = seat_.get_keyboard();
    keyboard_.on_key() = [this] (uint32_t serial, uint32_t time, uint32_t key, keyboard_key_state state) {
        bool down = state == keyboard_key_state::pressed;

        std::cout << "key: " << std::hex << key << std::dec << " " << (down ? "down" : "up") << std::endl;

        auto mkvkey = x_keycode_to_mac_virt[key + 8];

        callbacks_->keyboardEvent(down, mkvkey);
    };

    pointer_.on_enter() = [this](uint32_t serial, surface_t surface, double x, double y) {
        pointer_.set_cursor(serial, cursorSurface_, hotSpot_.first, hotSpot_.second);
        cursorEnterSerial_ = serial;
    };


    cursorBuffer_ = Buffer(shm_, 16,16);
    cursorSurface_ = compositor_.create_surface();
    hotSpot_ = {0,0};
    
    cursorSurface_.attach(cursorBuffer_.wlbuffer(), 0, 0);
    cursorSurface_.commit();
}

void WaylandVideoDriver::endEventLoop()
{
    exitMainThread_ = true;
    wakeEventLoop();
}

WaylandVideoDriver::~WaylandVideoDriver()
{
}

void WaylandVideoDriver::runEventLoop()
{
    int waylandFd = display_.get_fd();    
    pollfd fds[] = {
        { waylandFd, POLLIN, 0 },
        { wakeFd_, POLLIN, 0 }
    };

    while(!exitMainThread_)
    {
        int timeout = -1;
        bool timedOut = false;

        display_.dispatch_pending();

        /* Re-evaluate the captured-mouse state even while the guest is stuck in
         * a loop that never calls SystemTask/WaitNextEvent. */
        bool captureChanged = updateResponsivenessFeedback();
        {
            std::lock_guard lk(mutex_);
            if(captureChanged || clickCaptureDirty_)
                requestUpdate();
        }

        {
            std::lock_guard lk(mutex_);

            if(state_ == State::waitingForUpdate
                || state_ == State::waitingForObsoleteUpdate)
            {
                auto now = std::chrono::steady_clock::now();

                if (updateTimeout_ <= now)
                    timedOut = true;
                else
                    timeout = std::chrono::duration_cast<std::chrono::milliseconds>(updateTimeout_ - now).count();
            }
        }

        /* Never block indefinitely: we have to keep polling the captured state. */
        if(timeout < 0 || timeout > kResponsivenessPollMs)
            timeout = kResponsivenessPollMs;

        if(timedOut)
            noteUpdatesDone();

        display_.flush();

        {
            auto intent = display_.obtain_read_intent();
            int result = poll(fds, std::size(fds), timeout);

            if(result > 0)
            {
                if(fds[0].revents & POLLIN)
                    intent.read();
                if(fds[1].revents & POLLIN)
                {
                    uint64_t evt;
                    ::read(wakeFd_, &evt, sizeof(evt));
                }
            }
        }
    }
}

void WaylandVideoDriver::wakeEventLoop()
{
    uint64_t evt = 1;
    ::write(wakeFd_, &evt, sizeof(evt));
}

bool WaylandVideoDriver::setMode(int width, int height, int bpp,
                                  bool grayscale_p)
{
    std::unique_lock lk(mutex_);

    if(bpp)
        requestedBpp_ = bpp;

    if(state_ == State::unconfigured)
    {
        if(width && height)
        {
            configuredShape_.width = width;
            configuredShape_.height = height;
        }
        else
            xdg_toplevel_.set_maximized();
        //xdg_toplevel_.set_fullscreen({});

        surface_.commit();
        display_.flush();

        stateChanged_.wait(lk, [this] {
            return state_ == State::waitingForModeSwitch;
        });
        frameRequested_ = true;

        lk.unlock();
        updateMode();
        noteUpdatesDone();
        frameCallback();
    }
    else
    {
        if(state_ == State::waitingForUpdate || state_ == State::waitingForObsoleteUpdate)
        {
            updateTimeout_ = std::chrono::steady_clock::now();
            wakeEventLoop();
        }
        stateChanged_.wait(lk, [this] {
            return state_ == State::idle
                || state_ == State::waitingForModeSwitch
                || state_ == State::drawingObsoleteUpdate;
        });

        state_ = State::waitingForModeSwitch;
        lk.unlock();
        updateMode();
    }

    return true;
}

bool WaylandVideoDriver::handleMenuBarDrag()
{
    xdg_toplevel_.move(seat_, lastMouseDownSerial_);
    return true;
}

void WaylandVideoDriver::noteUpdatesDone()
{
    std::lock_guard lk(mutex_);

    if(state_ == State::waitingForUpdate
        || state_ == State::waitingForObsoleteUpdate)
    {
        std::cout << "updates done.\n";
        if(state_ == State::waitingForObsoleteUpdate)
        {
            state_ = State::drawingObsoleteUpdate;
            stateChanged_.notify_all();
        }
        else
        {
            state_ = State::idle;
            stateChanged_.notify_all();
        }
        requestUpdate();
    }
}

bool WaylandVideoDriver::updateMode()
{
    std::lock_guard lk(mutex_);
    if (state_ != State::waitingForModeSwitch)
        return false;

    bool sizeChanged = configuredShape_.width != allocatedShape_.width
                    || configuredShape_.height != allocatedShape_.height;

    bool depthChanged = requestedBpp_ != framebuffer_.bpp;

    if(sizeChanged || depthChanged)
    {
        framebuffer_ = Framebuffer(configuredShape_.width, configuredShape_.height, requestedBpp_);
        std::fill(framebuffer_.data.get(), framebuffer_.data.get() + framebuffer_.rowBytes * framebuffer_.height, 0);
    
        framebuffer_.rootless = configuredShape_.maximized;

        if(sizeChanged)
        {
            if(framebuffer_.rootless)
                pendingRootlessRegion_ = { RGN_STOP, RGN_STOP };
            else
                pendingRootlessRegion_ = { 0, 0, (int16_t)configuredShape_.width, RGN_STOP, 
                            (int16_t)configuredShape_.height, 0, (int16_t)configuredShape_.width, RGN_STOP, RGN_STOP };
            
            rootlessRegionDirty_ = true;
            surface_.set_input_region({});
        }

        allocatedShape_ = configuredShape_;

        updateTimeout_ = std::chrono::steady_clock::now() + 100ms;
        
        dirtyRects_.clear();
        dirtyRects_.add(0, 0, configuredShape_.height, configuredShape_.width);

        state_ = State::waitingForUpdate;
        stateChanged_.notify_all();

        wakeEventLoop();
    }
    else
    {
        allocatedShape_ = configuredShape_;
        state_ = State::idle;
        stateChanged_.notify_all();
        
        requestUpdate();
    }

    return sizeChanged || depthChanged;
}

bool WaylandVideoDriver::requestFrame()
{
    if(dirtyRects_.empty() && allocatedShape_.serial == committedShape_.serial
        && !rootlessRegionDirty_ && !clickCaptureDirty_)
        return false;

    if(state_ != State::idle
        && state_ != State::waitingForModeSwitch
        && state_ != State::drawingObsoleteUpdate)
        return false;

    if(frameRequested_)
        return false;

    frameCallback_ = surface_.frame();
    frameCallback_.on_done() = [this](uint32_t) { frameCallback(); };
    frameRequested_ = true;

    return true;
}

void WaylandVideoDriver::requestUpdate()
{
    if(requestFrame())
    {
        surface_.commit();
        display_.flush();
    }
}

void WaylandVideoDriver::frameCallback()
{
    std::unique_lock lk(mutex_);

    if(state_ != State::idle
        && state_ != State::waitingForModeSwitch
        && state_ != State::drawingObsoleteUpdate)
    {
        frameRequested_ = false;
        return;
    }

    if(state_ == State::drawingObsoleteUpdate)
    {
        state_ = State::waitingForModeSwitch;
        stateChanged_.notify_all();
    }

    auto shape = allocatedShape_;

    if(shape.width != committedShape_.width || shape.height != committedShape_.height)
    {
        buffer_ = Buffer(shm_, shape.width, shape.height);
        dirtyRects_.add(0,0,shape.height,shape.width);
    }

    if(rootlessRegionDirty_ || clickCaptureDirty_)
    {
        commitRootlessRegion();

        region_t waylandRgn = compositor_.create_region();
        if(clickCapture_)
        {
            /* While captured, accept pointer input over the whole surface so
             * that clicks meant for the (busy) application are not lost to the
             * host desktop through the rootless holes. */
            waylandRgn.add(0, 0, shape.width, shape.height);
        }
        else
        {
            forEachRect(rootlessRegion_.begin(), [&](int l, int t, int r, int b) {
                waylandRgn.add(l, t, r-l, b-t);
            });
        }
        surface_.set_input_region(waylandRgn);
        clickCaptureDirty_ = false;
    }

    auto rects = dirtyRects_.getAndClear();

    if(rects.size())
    {
        Framebuffer fb = framebuffer_;
        lk.unlock();
        updateBuffer(fb, buffer_.data(), buffer_.width(), buffer_.height(), rects);
        lk.lock();
    }

    for(const auto& r : rects)
        surface_.damage_buffer(r.left,r.top,r.right-r.left,r.bottom-r.top);

    frameRequested_ = false;

    surface_.attach(buffer_.wlbuffer(), 0, 0);

    if(shape.serial != committedShape_.serial)
    {
        xdg_surface_.ack_configure(shape.serial);
        committedShape_ = shape;
    }

    requestFrame();

    surface_.commit();
    display_.flush();
}

void WaylandVideoDriver::setClickCapture(bool capture)
{
    std::lock_guard lk(mutex_);

    clickCapture_ = capture;
    clickCaptureDirty_ = true;
}

void WaylandVideoDriver::setCursor(char *cursor_data, uint16_t cursor_mask[16], int hotspot_x, int hotspot_y)
{
    uint32_t *dst = reinterpret_cast<uint32_t*>(cursorBuffer_.data());
    uint8_t *data = reinterpret_cast<uint8_t*>(cursor_data);
    uint8_t *mask = reinterpret_cast<uint8_t*>(cursor_mask);
    for(int y = 0; y < 16; y++)
        for(int x = 0; x < 16; x++)
        {
            if(mask[2*y + x/8] & (0x80 >> x%8))
                *dst++ = (data[2*y + x/8] & (0x80 >> x%8)) ? 0xFF000000 : 0xFFFFFFFF;
            else
                *dst++ = (data[2*y + x/8] & (0x80 >> x%8)) ? 0xFF000000 : 0;
        }
    cursorSurface_.damage_buffer(0,0,16,16);
    cursorSurface_.attach(cursorBuffer_.wlbuffer(), 0, 0);

    hotSpot_ = { hotspot_x, hotspot_y };
    cursorSurface_.commit();
    pointer_.set_cursor(cursorEnterSerial_, cursorSurface_, hotSpot_.first, hotSpot_.second);
}

void WaylandVideoDriver::setCursorVisible(bool show_p)
{
    pointer_.set_cursor(cursorEnterSerial_, show_p ? cursorSurface_ : surface_t(), hotSpot_.first, hotSpot_.second);
}

