#include <vdriver/vdriver.h>
#include <vdriver/responsiveness.h>
#include <error/error.h>
#include <quickdraw/region.h>

#include <thread>
#include <condition_variable>

using namespace Executor;

std::unique_ptr<VideoDriver> Executor::vdriver;

/* How opaque the desktop dim becomes while the guest is captured. */
static constexpr float kCaptureDim = 0.5f;

Framebuffer::Framebuffer(int w, int h, int d)
    : width(w), height(h), bpp(d)
{
    rowBytes = ((width * bpp + 31) & ~31) / 8;
    data = std::shared_ptr<uint8_t>(new uint8_t[rowBytes * height], [](uint8_t* p) { delete[] p; });
}


class VideoDriver::ThreadPool
{
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable workCond_, doneCond_;
    
    bool terminate_ = false;
    int workingThreads_;
    
    int workNumber_ = 0;
    std::function<void()> work_;
    
public:
    ThreadPool()
    {
        int nThreads = std::thread::hardware_concurrency() ? std::max(0, (int)std::thread::hardware_concurrency() - 2) : 4;
        
        for(int i = 0; i < nThreads; i++)
            threads_.emplace_back([this]() {
                std::unique_lock lk(mutex_);
                for(int round = 1;;round++)
                {
                    workCond_.wait(lk, [this, round] { return terminate_ || workNumber_ == round; });
                    if(terminate_)
                        return;
                    
                    if(auto w = work_)
                    {
                        lk.unlock();
                        w();
                        lk.lock();
                    }
                    
                    work_ = {};
                    if(--workingThreads_ == 0)
                        doneCond_.notify_one();
                }
            });
    }
    
    ~ThreadPool()
    {
        {
            std::lock_guard lk(mutex_);
            terminate_ = true;
            workCond_.notify_all();
        }
        for(auto& t : threads_)
            t.join();
    }
    
    void run(std::function<void()> work)
    {
        if(threads_.empty())
        {
            work();
            return;
        }
        std::unique_lock lk(mutex_);
        work_ = work;
        workingThreads_ = threads_.size();
        ++workNumber_;
        workCond_.notify_all();
        work();
        doneCond_.wait(lk, [this] { return workingThreads_ == 0; });
    }
};

VideoDriver::VideoDriver(IEventListener *cb)
    : callbacks_(cb)
{
    std::fill(begin(colors_), end(colors_), 0);
}

VideoDriver::~VideoDriver()
{
}

bool VideoDriver::isAcceptableMode(int width, int height, int bpp, bool grayscale_p)
{
    if(width && width < VDRIVER_MIN_SCREEN_WIDTH)
        return false;
    if(height && height < VDRIVER_MIN_SCREEN_HEIGHT)
        return false;
    if(bpp & (bpp - 1))
        return false;
    if(bpp > 32)
        return false;
    return true;
}

void VideoDriver::putScrap(OSType type, LONGINT length, char *p, int scrap_cnt)
{
}

LONGINT VideoDriver::getScrap(OSType type, Handle h)
{
    return 0;
}

void VideoDriver::weOwnScrap()
{
}

void VideoDriver::setTitle(const std::string& name)
{
}

void VideoDriver::setCursor(char *cursor_data, uint16_t cursor_mask[16], int hotspot_x, int hotspot_y)
{
}

void VideoDriver::setCursorVisible(bool show_p)
{
}

void VideoDriver::beepAtUser()
{
}


void VideoDriver::setColors(int num_colors, const Executor::vdriver_color_t *color_array)
{
    std::lock_guard lk(mutex_);
    for(int i = 0; i < num_colors; i++)
    {
        colors_[i] = (0xFF << 24)
                    | ((color_array[i].red >> 8) << 16)
                    | ((color_array[i].green >> 8) << 8)
                    | (color_array[i].blue >> 8);
    }
    dirtyRects_.add(0,0, height(), width());
    requestUpdate();
}


void VideoDriver::setRootlessRegion(RgnHandle rgn)
{
    std::lock_guard lk(mutex_);

    RgnVector inRgn(rgn);
    pendingRootlessRegion_.clear();
        
    if(inRgn.size())
        pendingRootlessRegion_.insert(pendingRootlessRegion_.end(), inRgn.begin(), inRgn.end());
    else
        pendingRootlessRegion_.insert(pendingRootlessRegion_.end(),
            { (*rgn)->rgnBBox.top.get(), (*rgn)->rgnBBox.left.get(), (*rgn)->rgnBBox.right.get(), RGN_STOP,
            (*rgn)->rgnBBox.bottom.get(), (*rgn)->rgnBBox.left.get(), (*rgn)->rgnBBox.right.get(), RGN_STOP,
            RGN_STOP });
    rootlessRegionDirty_ = true;
    requestUpdate();
}

void VideoDriver::updateScreen(int top, int left, int bottom, int right)
{
    std::lock_guard lk(mutex_);
    dirtyRects_.add(top, left, bottom, right);
    requestUpdate();
}

bool VideoDriver::updateResponsivenessFeedback()
{
    /* The feedback is only meaningful in rootless mode, where clicks can fall
     * through the holes onto the host desktop. */
    if(!isRootless())
        return false;

    bool captured = Responsiveness::instance().poll();

    if(captured == clickCapture_)
        return false;

    clickCapture_ = captured;
    desktopDim_ = captured ? kCaptureDim : 0.0f;

    {
        std::lock_guard lk(mutex_);
        /* The dim paints the whole desktop, so the holes have to be redrawn
         * even where nothing else changed. */
        dirtyRects_.add(0, 0, height(), width());
    }

    setClickCapture(captured);
    return true;
}

void VideoDriver::commitRootlessRegion()
{
    if(!rootlessRegionDirty_)
        return;
    
    if(!rootlessRegion_.size())
        rootlessRegion_.insert(rootlessRegion_.end(),
            { 0, 0, (int16_t)width(), RGN_STOP,
            (int16_t)height(), 0, (int16_t)width(), RGN_STOP,
            RGN_STOP });

    // calculate symmetric difference (XorRgn) and add to dirtyRects.
    std::vector<int16_t> rgnDiff(rootlessRegion_.size() + pendingRootlessRegion_.size());

    auto p = rootlessRegion_.begin();
    auto q = pendingRootlessRegion_.begin();
    auto dst = rgnDiff.begin();

    while(*p < RGN_STOP || *q < RGN_STOP)
    {
        if(*p < *q)
            while((*dst++ = *p++) < RGN_STOP)
                ;
        else if(*p > *q)
            while((*dst++ = *q++) < RGN_STOP)
                ;
        else
        {
            auto startOfLine = dst;
            *dst++ = *p++;
            q++;

            while(*p < RGN_STOP || *q < RGN_STOP)
            {
                if(*p < *q)
                    *dst++ = *p++;
                else if(*p > *q)
                    *dst++ = *q++;
                else
                    p++, q++;
            }

            if(dst == startOfLine + 1)
                dst = startOfLine;
            else
                *dst++ = RGN_STOP;
                
            p++, q++;
        }
    }
    *dst++ = RGN_STOP;

    forEachRect(rgnDiff.begin(), [this](int l, int t, int r, int b) {
        //std::cout << "region changed: " << l << " " << t << " " << r << " " << b << std::endl;
        dirtyRects_.add(t, l, b, r);
    });

    rootlessRegion_ = pendingRootlessRegion_;
    rootlessRegionDirty_ = false;
}

template<int depth>
struct IndexedPixelGetter
{
    uint8_t *src;
    int shift;
    std::array<uint32_t, 256>& colors;

    static constexpr uint8_t mask = (1 << depth) - 1;

    IndexedPixelGetter(std::array<uint32_t, 256>& colors, uint8_t *line, int x)
        : colors(colors)
    {
        src = line + x * depth / 8;
        shift = 8 - (x * depth % 8) - depth;
    }

    uint32_t operator() ()
    {
        auto c = colors[(*src >> shift) & mask];
        shift -= depth;
        if(shift < 0)
        {
            ++src;
            shift = 8 - depth;
        }
        return c;
    }
};

void VideoDriver::updateBuffer(const Framebuffer& fb, uint32_t* buffer, int bufferWidth, int bufferHeight,
                                const Executor::DirtyRects::Rects& rects)
{
    auto before = std::chrono::high_resolution_clock::now();
    
    if(!threadpool_)
        threadpool_ = std::make_unique<ThreadPool>();
    
    int width = std::min(fb.width, bufferWidth);
    int height = std::min(fb.height, bufferHeight);

    /* Pixels outside the rootless region are the desktop, which shows through
     * the window as transparent (dimPixel == 0) or, while the guest is
     * captured, as 50% black.  The buffer is premultiplied ARGB, so black at
     * alpha a is just a << 24. */
    uint32_t dimPixel = 0;
    if(desktopDim_ > 0.0f)
    {
        unsigned a = (unsigned)(desktopDim_ * 255.0f + 0.5f);
        if(a > 255)
            a = 255;
        dimPixel = a << 24;
    }

    if(!rootlessRegion_.size())
        rootlessRegion_.insert(rootlessRegion_.end(),
            { 0, 0, (int16_t)width, RGN_STOP,
            (int16_t)height, 0, (int16_t)width, RGN_STOP,
            RGN_STOP });

    Executor::DirtyRects::Rects rectList = rects;
    int nextRect = 0;
    std::mutex mut;
    
    threadpool_->run([=, &rectList, &nextRect, &mut] {
        for(;;)
        {
            Executor::DirtyRects::Rect r;
            {
                std::lock_guard lk(mut);
                
                if(nextRect == rectList.size())
                    return;
                
                r = rectList[nextRect];
                if(r.bottom - r.top > 160)
                {
                    r.bottom = r.top + 100;
                    rectList[nextRect].top = r.bottom;
                }
                else
                    ++nextRect;
            }
            
            assert(r.left >= 0 && r.right >= 0 && r.left <= width && r.bottom <= height);
            
            RegionProcessor rgnP(rootlessRegion_.begin());

            for(int y = r.top; y < r.bottom; y++)
            {
                while(y >= rgnP.bottom())
                    rgnP.advance();

                auto blitLine = [this, &rgnP, buffer, bufferWidth, bufferHeight, y, &r, dimPixel](auto getPixel) {
                    auto rowIt = rgnP.row.begin();
                    int x = r.left;

                    while(x < r.right)
                    {
                        int nextX = std::min(r.right, (int)*rowIt++);

                        /* Outside the rootless region is the desktop.  Its
                         * white backdrop (see ROMLib_InitGrayRgn) becomes
                         * transparent -- or dimmed while the guest is captured --
                         * but anything drawn *over* the desktop without going
                         * through a window, such as GrowWindow feedback during a
                         * resize tracking loop, must be kept visible: Executor
                         * cannot intercept those draws to update the region, so
                         * the white check is what tells the two apart. */
                        for(; x < nextX; x++)
                        {
                            uint32_t pixel = getPixel();
                            buffer[y * bufferWidth + x] = pixel == 0xFFFFFFFF ? dimPixel : pixel;
                        }
                        
                        if(x >= r.right)
                            break;

                        nextX = std::min(r.right, (int)*rowIt++);

                        for(; x < nextX; x++)
                            buffer[y * bufferWidth + x] = getPixel();
                    }
                };

                uint8_t *src = fb.data.get() + y * fb.rowBytes;
                switch(fb.bpp)
                {
                    case 8:
                        src += r.left;
                        blitLine([&] { return colors_[*src++]; });
                        break;

                    case 1:
                        blitLine(IndexedPixelGetter<1>(colors_, src, r.left));
                        break;
                    case 2:
                        blitLine(IndexedPixelGetter<2>(colors_, src, r.left));
                        break;
                    case 4:
                        blitLine(IndexedPixelGetter<4>(colors_, src, r.left));
                        break;
                    case 16:
                        {
                            auto *src16 = reinterpret_cast<GUEST<uint16_t>*>(src);
                            src16 += r.left;
                            blitLine([&] {
                                uint16_t pix = *src16++;
                                auto fiveToEight = [](uint32_t x) {
                                    return (x << 3) | (x >> 2);
                                };
                                return 0xFF000000
                                    | (fiveToEight((pix >> 10) & 31) << 16)
                                    | (fiveToEight((pix >> 5) & 31) << 8)
                                    | fiveToEight(pix & 31);
                            });
                        }
                        break;

                    case 32:
                        {
                            auto *src32 = reinterpret_cast<GUEST<uint32_t>*>(src);
                            src32 += r.left;
                            blitLine([&] { return (*src32++) | 0xFF000000; });
                        }
                        break;

                }
            }
        }
    });
    
    auto after = std::chrono::high_resolution_clock::now();
    
#if 0
    static int sum = 0, count = 0;

    if(count > 100)
        count = sum = 0;

    sum += std::chrono::duration_cast<std::chrono::microseconds>(after-before).count();
    count++;

    std::cout << double(sum)/count << std::endl;
#endif
}
