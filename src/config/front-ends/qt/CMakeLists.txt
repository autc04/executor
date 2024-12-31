cmake_policy(SET CMP0020 NEW) # Automatically link Qt executables to qtmain target on Windows.
find_package(Qt6 COMPONENTS Gui)

if(TARGET Qt6::Gui)
    add_library(front-end-qt
            default_vdriver.h
            qt.h
            qt.cpp
            available_geometry.h
            available_geometry.cpp
            qtkeycodes.cpp
            ../x/x_keycodes.cpp
        )

    target_include_directories(front-end-qt
            PUBLIC .)

    target_link_libraries(front-end-qt syn68k romlib)

    if(WIN32 AND MSYS AND BUILD_STATIC)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(QT5_PKGCONFIG Qt5Gui)

        target_compile_definitions(front-end-qt PRIVATE STATIC_WINDOWS_QT)

        target_link_libraries(front-end-qt
            Qt5::QWindowsIntegrationPlugin 
            /opt/mxe/usr/x86_64-w64-mingw32.static.posix/qt5/lib/libQt5ThemeSupport.a 
            /opt/mxe/usr/x86_64-w64-mingw32.static.posix/qt5/lib/libQt5EventDispatcherSupport.a  
            /opt/mxe/usr/x86_64-w64-mingw32.static.posix/qt5/lib/libQt5WindowsUIAutomationSupport.a 
            /opt/mxe/usr/x86_64-w64-mingw32.static.posix/qt5/lib/libQt5FontDatabaseSupport.a
            Qt5::Gui
            -ldwmapi -lcomdlg32 -loleaut32 -limm32 -lopengl32 -lharfbuzz -lcairo -lgobject-2.0 -lfontconfig
            -lfreetype -lm -lusp10 -lmsimg32 -lgdi32 -lpixman-1 -lffi -lexpat -lbz2 -lpng16 -lharfbuzz_too
            -lfreetype_too -lglib-2.0 -lshlwapi -lpcre -lintl -liconv -lmpr -lnetapi32 -luserenv -lversion
            -lws2_32 -lkernel32 -luser32 -lshell32 -luuid -lole32 -ladvapi32 -lwinmm -lz -lpcre2-16 )
    else()
         target_link_libraries(front-end-qt Qt6::Gui)
    endif()
endif()
