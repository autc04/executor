#pragma once

#include <vdriver/vdriver.h>

class QRect;
class QGuiApplication;
class QImage;
class QBackingStore;
class QRegion;
class QTimer;

class QtVideoDriver : public Executor::VideoDriver
{
    class ExecutorWindow;

    QGuiApplication *qapp;
    QImage *qimage;
    ExecutorWindow *window = nullptr;
    QTimer *responsivenessTimer_ = nullptr;

    bool clickCaptureDirty_ = false;

    void render(QBackingStore *bs, QRegion rgn);
    void requestUpdate() override;
    void setClickCapture(bool capture) override;
public:
    QtVideoDriver(Executor::IEventListener *eventListener, int& argc, char* argv[]);
    ~QtVideoDriver();

    bool setMode(int width, int height, int bpp, bool grayscale_p) override;
    void setCursor(char *cursor_data, uint16_t cursor_mask[16], int hotspot_x, int hotspot_y) override;
    void setCursorVisible(bool show_p) override;
    void runEventLoop() override;
    void endEventLoop() override;
};
