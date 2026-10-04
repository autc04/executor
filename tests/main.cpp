#include "gtest/gtest.h"
#include <Quickdraw.h>
#include <Windows.h>
#include <Dialogs.h>
#include <TextEdit.h>

extern "C"
int mkdir(const char*, mode_t)
{
    return -1;
}

extern "C"
char *getcwd(char *buf, size_t size)
{
    buf[0] = '/';
    buf[1] = 0;
    return buf;
} 

int main(int argc, char **argv)
{
#ifdef USE_CONSOLE
    std::cout << "Running Executor 2000 test suite..." << std::endl;
    // side effect: initialized Toolbox,
#else
    freopen("out", "w", stdout);
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    TEInit();
    InitDialogs(nullptr);
#endif

    testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
#ifdef USE_CONSOLE
    std::cout << "Press a key to exit - Test suite result: " << result << std::endl;
    getchar();
#endif
    return result;
}
