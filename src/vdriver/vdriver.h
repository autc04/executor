
#if !defined(_VDRIVER_H_)
#define _VDRIVER_H_

#include <ExMacTypes.h>
#include <vdriver/dirtyrect.h>


#include <functional>
#include <memory>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <string>
#include <array>
#include <vector>


namespace Executor
{
struct ColorSpec;
struct Region;
typedef Region *RgnPtr;
typedef GUEST<RgnPtr> *RgnHandle;
struct rgb_spec_t;
}

namespace Executor
{

/* Minimum screen size we'll allow. */
#define VDRIVER_MIN_SCREEN_WIDTH 512
#define VDRIVER_MIN_SCREEN_HEIGHT 342

/* Suggested default screen size (may be safely ignored). */
#define VDRIVER_DEFAULT_SCREEN_WIDTH 1024
#define VDRIVER_DEFAULT_SCREEN_HEIGHT 768

struct vdriver_color_t
{
    uint16_t red, green, blue;
};

struct Framebuffer
{
        // The following should be shared_ptr<uint8_t[]>.
        // This is possible in C++17 or later, but Apple is still causing problems (as of 10.15)
    std::shared_ptr<uint8_t> data; 


    int width = 0, height = 0;
    int bpp = 0;
    int rowBytes = 0;
    int cursorBpp = 1;
    bool grayscale = false;
    bool rootless = false;
    rgb_spec_t *rgbSpec = nullptr;

    Framebuffer() = default;
    Framebuffer(int w, int h, int d);
};

class IEventListener
{
public:
    void mouseButtonEvent(bool down, int h, int v)
    {
        mouseMoved(h,v);
        mouseButtonEvent(down);
    }

    virtual void mouseButtonEvent(bool down) = 0;
    virtual void mouseMoved(int h, int v) = 0;
    virtual void keyboardEvent(bool down, unsigned char mkvkey) = 0;
    virtual void suspendEvent() = 0;
    virtual void resumeEvent(bool updateClipboard /* TODO: does this really make sense? */) = 0;
    virtual void requestQuit() = 0;
    virtual void wake() = 0;
};

class EventSink : public IEventListener
{
public:
    virtual void mouseButtonEvent(bool down) override;
    virtual void mouseMoved(int h, int v) override;
    virtual void keyboardEvent(bool down, unsigned char mkvkey) override;
    virtual void suspendEvent() override;
    virtual void resumeEvent(bool updateClipboard) override;
    virtual void requestQuit() override;
    virtual void wake() override;

    void pumpEvents();

    static std::unique_ptr<EventSink> instance;
private:
    GUEST<uint32_t> keytransState = 0;

    std::mutex mutex_;
    std::vector<std::function<void ()>> todo_;

    void runOnEmulatorThread(std::function<void ()> f);
};

class VideoDriver
{
public:
    VideoDriver(IEventListener *cb);
    VideoDriver(const VideoDriver&) = delete;
    VideoDriver& operator=(const VideoDriver&) = delete;
    virtual ~VideoDriver();

    virtual void runEventLoop() = 0;
    virtual void endEventLoop() = 0;
    
    virtual void updateScreen(int top, int left, int bottom, int right);
    virtual bool isAcceptableMode(int width, int height, int bpp, bool grayscale_p);
    virtual void setColors(int num_colors, const vdriver_color_t *colors);
    virtual bool setMode(int width, int height, int bpp, bool grayscale_p) = 0;

    virtual void putScrap(OSType type, LONGINT length, char *p, int scrap_cnt);
    virtual LONGINT getScrap(OSType type, Handle h);
    virtual void weOwnScrap();

    virtual void setTitle(const std::string& name);

    virtual void setCursor(char *cursor_data,
                                uint16_t cursor_mask[16],
                                int hotspot_x, int hotspot_y);
    virtual void setCursorVisible(bool show_p);


    virtual void setRootlessRegion(RgnHandle rgn);

        // TODO: should move to sound driver?
    virtual void beepAtUser();

    virtual void noteUpdatesDone() {}
    virtual bool updateMode() { return false; }

    virtual bool handleMenuBarDrag() { return false; }

    uint8_t *framebuffer() { return framebuffer_.data.get(); }
    int cursorDepth() { return framebuffer_.cursorBpp; }
    int width() { return framebuffer_.width; }
    int height() { return framebuffer_.height; }
    int rowBytes() { return framebuffer_.rowBytes; }
    int bpp() { return framebuffer_.bpp; }
    rgb_spec_t *rgbSpec() { return framebuffer_.rgbSpec; }
    bool isGrayscale() { return framebuffer_.grayscale; }
    bool isRootless() { return framebuffer_.rootless; }

protected:
    IEventListener *callbacks_ = nullptr;

    Framebuffer framebuffer_;

    std::mutex mutex_;
    std::array<uint32_t, 256> colors_;

    std::vector<int16_t> rootlessRegion_;
    std::vector<int16_t> pendingRootlessRegion_;
    bool rootlessRegionDirty_ = false;
    bool clickCapture_ = false;
    float desktopDim_ = 0.0f;
    float dimTarget_ = 0.0f;
    float dimStart_ = 0.0f;
    std::chrono::steady_clock::time_point fadeStart_;
    bool fading_ = false;

    Executor::DirtyRects dirtyRects_;

    void updateBuffer(const Framebuffer& fb, uint32_t* buffer, int bufferWidth, int bufferHeight,
                    const Executor::DirtyRects::Rects& rects);

    virtual void requestUpdate() = 0;

    // Re-evaluate whether the guest should have the mouse captured and apply
    // the (rootless) feedback.  Front-ends call this periodically from their
    // frame/timer, on the GUI thread; it returns true if a repaint is needed.
    // See docs/ai/2026-10-07-unresponsive-app-detection-plan.md.
    bool updateResponsivenessFeedback();

    // Front-end hook: capture (true) or release (false) clicks that would
    // otherwise fall through the rootless holes onto the host.  Default no-op.
    virtual void setClickCapture(bool capture) {}

    void commitRootlessRegion();

    class ThreadPool;
    std::unique_ptr<ThreadPool> threadpool_;
};

extern std::unique_ptr<VideoDriver> vdriver;

}
#endif /* !_VDRIVER_H_ */
