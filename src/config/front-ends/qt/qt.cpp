#include "qt.h"

#include <QGuiApplication>
#include <QPainter>
#include <QMouseEvent>
#include <QBitmap>
#include <QScreen>
#include <QBackingStore>
#include <QPaintDevice>
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
        drv_->callbacks_->mouseButtonEvent(!!(ev->buttons() & Qt::LeftButton), qRound(ev->position().x()), qRound(ev->position().y()) - windowTopPadding);
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
        drv_->callbacks_->mouseMoved(qRound(ev->position().x()), qRound(ev->position().y()) - windowTopPadding);
    }

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
        QVector<QRect> screenGeometries = getScreenGeometries();
#else
        QVector<QRect> screenGeometries = getAvailableScreenGeometries();
#endif

        printf("set_mode: %d %d %d\n", width, height, bpp);
        
        QRect geom = screenGeometries[0];
        for(const QRect& r : screenGeometries)
            if(r.width() * r.height() > geom.width() * geom.height())
                geom = r;

        framebuffer_ = Framebuffer(geom.width(), geom.height(), bpp ? bpp : 8);
        framebuffer_.rootless = true;

        qimage = new QImage(framebuffer_.width, framebuffer_.height, QImage::Format_RGB32);
        
        if(!window)
            window = new ExecutorWindow(this);
        window->setGeometry(geom);
        window->showMaximized();

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

void QtVideoDriver::render(QBackingStore *bs, QRegion rgn)
{
    std::unique_lock lk(mutex_);
    
    if(rootlessRegionDirty_)
    {
        commitRootlessRegion();
        
        QRegion qtRgn;
        forEachRect(rootlessRegion_.begin(), [&](int l, int t, int r, int b) {
            qtRgn += QRect(l, t + windowTopPadding, r-l, b-t);
        });

#ifdef __APPLE__
        macosx_autorelease_pool([&] {
#endif
            window->setMask(qtRgn);
#ifdef __APPLE__
        });
#endif
    }
    
    auto r = dirtyRects_.getAndClear();
    lk.unlock();

    for(int i = 0; i < r.size(); i++)
        rgn += QRect(r[i].left, r[i].top + windowTopPadding, r[i].right-r[i].left, r[i].bottom-r[i].top);
                                
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
                painter.drawImage(r, *qimage, r.translated(0, -windowTopPadding));
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
