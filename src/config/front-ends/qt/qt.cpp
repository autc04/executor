#include "qt.h"

#include <QGuiApplication>
#include <QPainter>
#include <QMouseEvent>
#include <QBitmap>
#include <QScreen>
#include <QBackingStore>
#include <QPaintDevice>
#include <QSurfaceFormat>
#include <QTimer>
#include <cmath>
#ifdef STATIC_WINDOWS_QT
#include <QtPlugin>
#endif

#include <optional>


#include <vdriver/vdriver.h>
#include <quickdraw/region.h>

#include "available_geometry.h"

//#include "keycode_map.h"

#include <iostream>
#include <memory>
#include <unordered_map>
#include <condition_variable>

#ifdef __APPLE__
void macosx_hide_menu_bar(int mouseX, int mouseY, int width, int height);
void macosx_autorelease_pool(const std::function<void ()>& fun);
#endif
#include <../x/x_keycodes.h>

#ifdef STATIC_WINDOWS_QT
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
#endif

constexpr bool log_key_events = false;

/* How often the GUI thread re-evaluates the guest's captured state, and how
 * often while the desktop dim is fading (which needs smoother frames). */
constexpr int kIdlePollMs = 100;
constexpr int kFadeFrameMs = 16;

using namespace Executor;

namespace Executor
{
    extern std::unordered_map<Qt::Key,int> qtToMacKeycodeMap;
}

#ifdef __APPLE__
    // for some inexplicable reason, when we create a window that covers every pixel of the screen,
    // a click at y = 0 still misses the window and brings a background app to the front.
    // As a hack, extend the window by 1 pixel upwards.
const int windowTopPadding = 1;
#else
const int windowTopPadding = 0;
#endif

class QtVideoDriver::ExecutorWindow : public QWindow
{
    QtVideoDriver* drv_;
    QBackingStore *backingStore_;
public:
    ExecutorWindow(QtVideoDriver* drv)
        : drv_(drv)
    {
        setFlag(Qt::FramelessWindowHint, true);
        setFlag(Qt::NoDropShadowWindowHint, true);

        backingStore_ = new QBackingStore(this);
    }

    void resizeEvent(QResizeEvent *resizeEvent) override
    {
        backingStore_->resize(resizeEvent->size());
    }

    void paintEvent(QPaintEvent *e) override
    {
        if (!isExposed())
            return;

        drv_->render(backingStore_, e->region());
    }

    void mousePressRelease(QMouseEvent *ev)
    {
        drv_->callbacks_->mouseButtonEvent(!!(ev->buttons() & Qt::LeftButton), guestX(ev->position().x()), guestY(ev->position().y()));
    }
    void mousePressEvent(QMouseEvent *ev) override
    {
        mousePressRelease(ev);
    }
    void mouseReleaseEvent(QMouseEvent *ev) override
    {
        mousePressRelease(ev);
    }

    void mouseMoveEvent(QMouseEvent *ev) override
    {
#ifdef __APPLE__
        macosx_hide_menu_bar(qRound(ev->position().x()), qRound(ev->position().y()) - windowTopPadding, width(), height());
#endif
        drv_->callbacks_->mouseMoved(guestX(ev->position().x()), guestY(ev->position().y()));
    }

private:
    /* Window coordinates are logical (device-independent) pixels, while the
     * guest framebuffer may have a different resolution. */
    int guestX(double x) const
    {
        return drv_->framebuffer_.width && width()
            ? qRound(x * drv_->framebuffer_.width / width())
            : 0;
    }

    int guestY(double y) const
    {
        int contentHeight = height() - windowTopPadding;
        return drv_->framebuffer_.height && contentHeight > 0
            ? qRound((y - windowTopPadding) * drv_->framebuffer_.height / contentHeight)
            : 0;
    }

public:

    void keyEvent(QKeyEvent *ev, bool down_p)
    {
        unsigned char mkvkey;

        auto p = qtToMacKeycodeMap.find(Qt::Key(ev->key()));
        if(p == qtToMacKeycodeMap.end())
            mkvkey = 0x89;// NOTAKEY
        else
            mkvkey = p->second;
        if(mkvkey == 0x89 && ev->nativeScanCode() > 1 && ev->nativeScanCode() < std::size(x_keycode_to_mac_virt))
        {
            mkvkey = x_keycode_to_mac_virt[ev->nativeScanCode()];
            if constexpr(log_key_events)
                std::cout << "mkvkey: " << ev->nativeScanCode() << " -> " << std::hex << (int)mkvkey << std::dec << std::endl;
        }
#ifdef __APPLE__
        if(ev->nativeVirtualKey())
            mkvkey = ev->nativeVirtualKey();
#endif
        drv_->callbacks_->keyboardEvent(down_p, mkvkey);
    }
    
    void keyPressEvent(QKeyEvent *ev) override
    {
        if constexpr(log_key_events)
            std::cout << "press: " << std::hex << ev->key() << " " << ev->nativeScanCode() << " " << ev->nativeVirtualKey() << std::dec << std::endl;
        if(!ev->isAutoRepeat())
            keyEvent(ev, true);
    }
    void keyReleaseEvent(QKeyEvent *ev) override
    {
        if constexpr(log_key_events)
            std::cout << "release\n";
        if(!ev->isAutoRepeat())
            keyEvent(ev, false);
    }

    bool event(QEvent *ev) override
    {
        switch(ev->type())
        {
            case QEvent::FocusIn:
                drv_->callbacks_->resumeEvent(true);
                break;
            case QEvent::FocusOut:
                drv_->callbacks_->suspendEvent();
                break;
            case QEvent::UpdateRequest:
                drv_->render(backingStore_, {});
                break;
            case QEvent::Close:
                drv_->callbacks_->requestQuit();
                ev->ignore();
                break;
            default:
                ;
        }
        return QWindow::event(ev);
    }
};

QtVideoDriver::QtVideoDriver(Executor::IEventListener *eventListener, int& argc, char* argv[])
    : VideoDriver(eventListener)
{
    qapp = new QGuiApplication(argc, argv);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);
}

QtVideoDriver::~QtVideoDriver()
{
}

void QtVideoDriver::endEventLoop()
{
    QMetaObject::invokeMethod(qapp, [this]{
        delete window;
        window = nullptr;
        qapp->quit();
    },Qt::QueuedConnection);
}

void QtVideoDriver::runEventLoop()
{
    qapp->exec();
}

bool QtVideoDriver::setMode(int width, int height, int bpp, bool grayscale_p)
{
    std::mutex initEndMutex;
    std::condition_variable initEndCond;
    bool initEnded = false;

    QMetaObject::invokeMethod(qapp, [=, &initEndMutex, &initEndCond, &initEnded] {
#ifdef __APPLE__
        macosx_hide_menu_bar(500, 0, 1000, 1000);
        QVector<ScreenGeometry> screenGeometries = getScreenGeometries();
#else
        QVector<ScreenGeometry> screenGeometries = getAvailableScreenGeometries();
#endif

        printf("set_mode: %d %d %d\n", width, height, bpp);
        
        QRect geom = screenGeometries[0].rect;
        double devicePixelRatio = screenGeometries[0].devicePixelRatio;
        for(const ScreenGeometry& s : screenGeometries)
            if(s.rect.width() * s.rect.height() > geom.width() * geom.height())
            {
                geom = s.rect;
                devicePixelRatio = s.devicePixelRatio;
            }

        /* screen_scale is expressed in standard-display pixels; on a display
         * with a device pixel ratio of `devicePixelRatio` that corresponds to
         * devicePixelRatio * screen_scale device pixels per emulated pixel.
         * Fractional device pixels make no sense, so round down (but never
         * below 1).  This is what makes --scale 1.5 mean 3x on a 2x display
         * and 1x on a standard display. */
        int pixelsPerEmulatedPixel = std::max(1, (int)std::floor(devicePixelRatio * screen_scale + 1e-6));
        int guestWidth = qRound(geom.width() * devicePixelRatio / pixelsPerEmulatedPixel);
        int guestHeight = qRound(geom.height() * devicePixelRatio / pixelsPerEmulatedPixel);

        framebuffer_ = Framebuffer(guestWidth, guestHeight, bpp ? bpp : 8);
        framebuffer_.rootless = true;

        /* Render with per-pixel alpha.  updateBuffer() already maps the rootless
         * holes to fully transparent pixels, but an opaque image format turns
         * those into black; that is invisible on X11/macOS (the window mask
         * clips them) but shows on Qt's Wayland backend, which does not clip the
         * mask.  With alpha the holes stay see-through everywhere. */
        qimage = new QImage(framebuffer_.width, framebuffer_.height, QImage::Format_ARGB32_Premultiplied);
        
        if(!window)
            window = new ExecutorWindow(this);
        {
            /* Ask the platform for a translucent surface so the compositor
             * blends our per-pixel alpha instead of showing it as black. */
            QSurfaceFormat fmt = window->format();
            fmt.setAlphaBufferSize(8);
            window->setFormat(fmt);
        }
        window->setGeometry(geom);
        window->showMaximized();

        if(!responsivenessTimer_)
        {
            /* Periodically re-evaluate the guest's captured state even while it
             * never yields. */
            responsivenessTimer_ = new QTimer(window);
            QObject::connect(responsivenessTimer_, &QTimer::timeout, qapp, [this] {
                if(updateResponsivenessFeedback())
                {
                    requestUpdate();
                    responsivenessTimer_->start(kFadeFrameMs);
                }
                else
                    responsivenessTimer_->start(kIdlePollMs);
            });
            responsivenessTimer_->start(kIdlePollMs);
        }

#ifdef __APPLE__
        geom.setY(geom.y() - 1);
        geom.setHeight(geom.height() + 1);
        window->setGeometry(geom);
#endif
        {
            std::unique_lock lk(initEndMutex);
            initEnded = true;
            initEndCond.notify_all();
        }
    });

    std::unique_lock lk(initEndMutex);
    initEndCond.wait(lk, [&] { return initEnded; });

    return true;
}

QRect QtVideoDriver::guestRectToWindow(const QRect& r) const
{
    if(!window || framebuffer_.width == 0 || framebuffer_.height == 0)
        return r;

    int contentWidth = window->width();
    int contentHeight = window->height() - windowTopPadding;
    if(contentWidth <= 0 || contentHeight <= 0)
        return r;

    int left = (int)std::floor(double(r.left()) * contentWidth / framebuffer_.width);
    int top = (int)std::floor(double(r.top()) * contentHeight / framebuffer_.height) + windowTopPadding;
    int right = (int)std::ceil(double(r.left() + r.width()) * contentWidth / framebuffer_.width);
    int bottom = (int)std::ceil(double(r.top() + r.height()) * contentHeight / framebuffer_.height) + windowTopPadding;

    return QRect(left, top, right - left, bottom - top);
}

QRect QtVideoDriver::windowRectToGuestRect(const QRect& r) const
{
    if(!window || framebuffer_.width == 0 || framebuffer_.height == 0)
        return r;

    int contentWidth = window->width();
    int contentHeight = window->height() - windowTopPadding;
    if(contentWidth <= 0 || contentHeight <= 0)
        return r;

    int left = (int)std::floor(double(r.left()) * framebuffer_.width / contentWidth);
    int top = (int)std::floor(double(r.top() - windowTopPadding) * framebuffer_.height / contentHeight);
    int right = (int)std::ceil(double(r.left() + r.width()) * framebuffer_.width / contentWidth);
    int bottom = (int)std::ceil(double(r.top() - windowTopPadding + r.height()) * framebuffer_.height / contentHeight);

    QRect result(left, top, right - left, bottom - top);
    return result;
}

void QtVideoDriver::render(QBackingStore *bs, QRegion rgn)
{
    std::unique_lock lk(mutex_);
    
    if(rootlessRegionDirty_ || clickCaptureDirty_)
    {
        commitRootlessRegion();
        
        QRegion qtRgn;
        if(clickCapture_)
        {
            /* Captured: accept pointer input over the whole window, so clicks
             * meant for the (busy) application are not lost to the host desktop
             * through the rootless holes.  The hole pixels stay transparent, so
             * this only changes hit-testing. */
            qtRgn += QRect(0, windowTopPadding, window->width(), window->height() - windowTopPadding);
        }
        else
        {
            forEachRect(rootlessRegion_.begin(), [&](int l, int t, int r, int b) {
                qtRgn += guestRectToWindow(QRect(l, t, r-l, b-t));
            });
        }

#ifdef __APPLE__
        macosx_autorelease_pool([&] {
#endif
            window->setMask(qtRgn);
#ifdef __APPLE__
        });
#endif

        clickCaptureDirty_ = false;
    }
    
    auto r = dirtyRects_.getAndClear();
    lk.unlock();

    for(int i = 0; i < r.size(); i++)
        rgn += guestRectToWindow(QRect(r[i].left, r[i].top, r[i].right-r[i].left, r[i].bottom-r[i].top));
                                
    updateBuffer(framebuffer_, (uint32_t*)qimage->bits(), qimage->width(), qimage->height(), r);

    if(!window->isExposed())
        return;

    bs->beginPaint(rgn);

    if(QPaintDevice *device = bs->paintDevice())
    {
        // Qt documentation failed to mention that bs->paintDevice() can return nullptr
        // if window->isExposed() is false. In that case, calling bs->endPaint() crashes.
        // There is now a check for isExposed, but now I'm paranoid.

        QPainter painter(device);

        if(qimage)
        {
            for(const QRect& r : rgn)
            {
                QRect src = windowRectToGuestRect(r);
                if(!src.isEmpty())
                    painter.drawImage(r, *qimage, src);
            }
        }

        painter.end();

        bs->endPaint();
        bs->flush(rgn);
    }
}

void QtVideoDriver::requestUpdate()
{
    QMetaObject::invokeMethod(window, &QWindow::requestUpdate);
}

void QtVideoDriver::setClickCapture(bool capture)
{
    std::lock_guard lk(mutex_);

    clickCaptureDirty_ = true;
}

void QtVideoDriver::setCursor(char *cursor_data,
                              uint16_t cursor_mask[16],
                              int hotspot_x, int hotspot_y)
{
    QMetaObject::invokeMethod(qapp, [=] {
        static QCursor theCursor(Qt::ArrowCursor);

        if(cursor_data)
        {
            uchar data2[32];
            uchar mask2[32];
            memcpy(data2, cursor_data, 32);
            memcpy(mask2, cursor_mask, 32);
            
            for(int i = 0; i<32; i++)
                mask2[i] |= data2[i];
            QBitmap crsr = QBitmap::fromData(QSize(16, 16), (const uchar*)data2, QImage::Format_Mono);
            QBitmap mask = QBitmap::fromData(QSize(16, 16), (const uchar*)mask2, QImage::Format_Mono);
            
            theCursor = QCursor(crsr, mask, hotspot_x, hotspot_y);
        }
        window->setCursor(theCursor);   // TODO: should we check for visibility?
    });
}

void QtVideoDriver::setCursorVisible(bool show_p)
{
    QMetaObject::invokeMethod(qapp, [=] {
        if(show_p)
            setCursor(nullptr, nullptr, 0, 0);
        else
            window->setCursor(Qt::BlankCursor);
    });
}
