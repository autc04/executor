#include "available_geometry.h"

#include <QVector>
#include <QRect>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QEventLoop>
#include <QResizeEvent>

QVector<ScreenGeometry> getScreenGeometries()
{
    QVector<ScreenGeometry> geometries;
    for(QScreen *screen : QGuiApplication::screens())
        geometries.push_back({screen->geometry(), screen->devicePixelRatio()});

    return geometries;
}

#ifndef __linux__

/* Actually, this should be the Qt-on-anything-but-X11 case. */

QVector<ScreenGeometry> getAvailableScreenGeometries()
{
    QVector<ScreenGeometry> geometries;
    for(QScreen *screen : QGuiApplication::screens())
        geometries.push_back({screen->availableGeometry(), screen->devicePixelRatio()});

    return geometries;
}

#else

/* QScreen::availableGeometry() is documented not to work
   on multi-screen X11 systems.

   If platformName() != "xcb", we can use availableGeometry();
   on X11, we instead figure out available screen geometries by opening
   invisible maximized windows on each screen.
 */

template <class F>
struct SizeTestWindow : QWindow
{
    F f;
    SizeTestWindow(F f)
        : f(f)
    {
    }
    void resizeEvent(QResizeEvent *evt) override
    {
        f(evt->size());
    }
};

template <class F>
QWindow* makeSizeTestWindow(F f) { return new SizeTestWindow<F>(f); }

QVector<ScreenGeometry> getAvailableScreenGeometries()
{
    if(qApp->platformName() != QLatin1String("xcb"))
    {
        QVector<ScreenGeometry> geometries;
        for(QScreen *screen : QGuiApplication::screens())
            geometries.push_back({screen->availableGeometry(), screen->devicePixelRatio()});

        return geometries;
    }

    QVector<QWindow*> windows;
    int windowCount = 0;
    QEventLoop loop;

    for(QScreen *screen : QGuiApplication::screens())
    {
        ++windowCount;
        QWindow *window = makeSizeTestWindow(
            [&loop, &windowCount](QSize sz) {
                if(sz.width() > 100 && sz.height() > 100)
                {
                    if(--windowCount <= 0)
                        loop.exit();
                }
            }
        );
        windows.push_back(window);
        window->setFlag(Qt::FramelessWindowHint, true);
        window->setFlag(Qt::NoDropShadowWindowHint, true);
        window->setOpacity(0);
        window->resize(100,100);
        window->setPosition(screen->availableGeometry().topLeft());
        window->showMaximized();
    }
    loop.exec();

    QVector<ScreenGeometry> geometries;
    for(QWindow *w : windows)
    {
        geometries.push_back({w->geometry(), w->devicePixelRatio()});
        delete w;
    }
    return geometries;
}
#endif
