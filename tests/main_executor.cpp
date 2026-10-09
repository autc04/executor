#include "gtest/gtest.h"
#include <FileMgr.h>
#include <file/file.h>
#include <MemoryMgr.h>
#include <mman/mman.h>
#include <WindowMgr.h>
#include <MenuMgr.h>
#include <FontMgr.h>
#include <rsys/executor.h>
#include <osevent/osevent.h>

using namespace Executor;

// Host path of the temporary working directory created below; exposed so native
// tests (tests/localvolume_test_util.h) can build host trees alongside the
// FileManager's view of the same directory.
fs::path ExecutorTestTempDir;

QDGlobals qd;
#include <quickdraw/cquick.h>
#include <vdriver/vdriver.h>
#include <ResourceMgr.h>

#include <PowerCore.h>

StringPtr PSTR(const char* s)
{
    size_t n = strlen(s);
    StringPtr p = (StringPtr)NewPtr(n + 1);   // just leaking it
    p[0] = n;
    memcpy(p+1, s, n);
    return p;
}

class MockVDriver : public VideoDriver
{
    using VideoDriver::VideoDriver;

    virtual bool setMode(int width, int height, int bpp,
                                bool grayscale_p) override
    {
        framebuffer_ = Framebuffer(512, 342, 1);
        return true;
    }

    virtual void runEventLoop() override {}
    virtual void endEventLoop() override {}
protected:
    virtual void requestUpdate() override {}
};

class ExecutorTestEnvironment : public testing::Environment
{
    char *thingOnStack;
    fs::path tempDir;
public:
    ExecutorTestEnvironment(char* thingOnStack) : thingOnStack(thingOnStack) {}

    virtual void SetUp() override
    {
        Executor::InitMemory(thingOnStack);

        EventSink::instance = std::make_unique<EventSink>();
        vdriver = std::make_unique<MockVDriver>(EventSink::instance.get());
        initialize_68k_emulator(nullptr, false, (uint32_t *)SYN68K_TO_US(0), 0);
        traps::init(false);
        Executor::InitLowMem();

        PowerCore& cpu = getPowerCore();
#if SIZEOF_CHAR_P == 4 && !defined(TWENTYFOUR_BIT_ADDRESSING)
        cpu.memoryBases[0] = (void*)ROMlib_offset;
        cpu.memoryBases[1] = nullptr;
        cpu.memoryBases[2] = nullptr;
        cpu.memoryBases[3] = nullptr;
#else
        cpu.memoryBases[0] = (void*)ROMlib_offsets[0];
        cpu.memoryBases[1] = (void*)ROMlib_offsets[1];
        cpu.memoryBases[2] = (void*)ROMlib_offsets[2];
        cpu.memoryBases[3] = (void*)ROMlib_offsets[3];
#endif
        EM_A5 = EM_A7 = ptr_to_longint(LM(MemTop)-4);

        Executor::ROMlib_fileinit();

        tempDir = fs::current_path() / fs::unique_path("temptest-%%%%-%%%%-%%%%-%%%%");
        fs::create_directory(tempDir);
        ExecutorTestTempDir = tempDir;

        if(auto fsspec = nativePathToFSSpec(tempDir))
        {
            WDPBRec wdpb;

            wdpb.ioVRefNum = fsspec->vRefNum;
            wdpb.ioWDDirID = fsspec->parID;
            wdpb.ioWDProcID = "Xctr"_4;
            wdpb.ioNamePtr = fsspec->name;
            PBOpenWD(&wdpb, false);
            SetVol(nullptr, wdpb.ioVRefNum);
        }

        InitResources();
        ROMlib_InitGDevices(512, 342, 1, false);
        ROMlib_eventinit();

        ROMlib_color_init();
        InitGraf(&qd.thePort);
        InitFonts();
        InitWindows();
        InitMenus();
    }

    virtual void TearDown() override
    {
        if(!tempDir.empty())
            fs::remove_all(tempDir);
    }
};

TEST(MemoryMgr, NewClearGetSizeDispose)
{
    Ptr p = NewPtrClear(42);

    ASSERT_NE(nullptr, p);
    ASSERT_EQ(0, *p);

    ASSERT_EQ(42, GetPtrSize(p));

    DisposePtr(p);
}

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);

    char thingOnStack;
    testing::AddGlobalTestEnvironment(new ExecutorTestEnvironment(&thingOnStack));

    int result = RUN_ALL_TESTS();
    return result;
}
