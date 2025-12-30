set(FRONT_ENDS qt x sdl sdl2 wayland CACHE STRING "frontends to build")

find_program(PERL_COMMAND perl)
if(NOT PERL_COMMAND)
    message(FATAL_ERROR "Cannot find command: 'perl' (needed for building!)")
endif()

find_program(BISON_COMMAND bison)
if(NOT BISON_COMMAND)
    message(FATAL_ERROR "Cannot find command: 'bison' (needed for building!)")
endif()

find_package(Ruby 2.1)
if(NOT RUBY_EXECUTABLE)
    message(FATAL_ERROR "Cannot find command: 'ruby' (needed for building!)")
endif()

find_package(Boost REQUIRED CONFIG COMPONENTS filesystem program_options)

find_package(Threads REQUIRED)


if(WIN32 AND MSVC)
    # compiler uses msvc-style command line
    # only clang supported at the moment
    add_compile_options(
        -Xclang -Wall -Xclang -Wextra

        -Wno-unused-parameter
        -Wno-sign-compare
    )
endif()

file(GLOB api_yaml_files
    CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../multiversal/defs/*.yaml)

file(GLOB multiversal_ruby_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/../multiversal/*.rb)


set(trap_instance_sources ${CMAKE_CURRENT_BINARY_DIR}/trap_instances/ReferenceAllTraps.cpp)
set(api_headers)
foreach(interfaceModule ${api_yaml_files})
    get_filename_component(base ${interfaceModule} NAME_WE)

        # Special case: MacTypes.h is still around on modern macs.
        # Let's rename our version to avoid conflicts.
    if(${base} STREQUAL MacTypes)
        set(base ExMacTypes)
    endif()
    list(APPEND trap_instance_sources ${CMAKE_CURRENT_BINARY_DIR}/trap_instances/${base}.cpp)
    list(APPEND api_headers ${CMAKE_CURRENT_BINARY_DIR}/api/${base}.h)
endforeach()

add_custom_command(
    OUTPUT ${api_headers} ${trap_instance_sources}
    DEPENDS
        ${api_yaml_files}
        ${multiversal_ruby_sources}
    COMMAND ${RUBY_EXECUTABLE} make-multiverse.rb -G Executor -o "${CMAKE_CURRENT_BINARY_DIR}/"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../multiversal
)

if(WIN32)
    set(HOST_OS "win32")
    set(host_os_sources
        config/os/win32/win_serial.cpp
        config/os/win32/win_serial.h
        config/os/win32/win_stat.h
        config/os/win32/win_stat.cpp)
    set(host_os_libs)
elseif(APPLE AND UNIX)
    set(HOST_OS "macosx")
    set(host_os_sources
        config/os/macosx/macosx.mm)
    find_library(COCOA_LIBRARY Cocoa)
    set(host_os_libs ${COCOA_LIBRARY})    
elseif(UNIX AND NOT APPLE)
    set(HOST_OS "linux")
    set(host_os_sources)
    set(host_os_libs)
else()
    set(host_os_sources)
    set(host_os_libs)
endif()
source_group(HostOS FILES ${host_os_sources})

if(CMAKE_COMPILER_IS_GNUCXX)
    # -Wno-enum-compare: Mac OSErr codes are currently spread between different
    #                    anonymous enums in the headers.
    #                    Code like (blah ? noErr : paramErr) triggers the enum-compare warning.

    add_compile_options(
        -Wall
        -Wno-unused-but-set-variable
        -Wno-sign-compare
        -Werror=return-type
        -Wno-enum-compare)
endif()

add_subdirectory(config/front-ends/x)
add_subdirectory(config/front-ends/sdl)
add_subdirectory(config/front-ends/sdl2)
add_subdirectory(config/front-ends/qt)
add_subdirectory(config/front-ends/wayland)
add_subdirectory(config/front-ends/headless)

function(linkheader from to variant)
    if(DEFINED ARGV3)
        set(input ${ARGV3})
    else()
        set(input ${variant}.h)
    endif()

    configure_file(${from}/${variant}/${input} ${to} COPYONLY)
endfunction()

add_executable(map_to_c map_to_c/map_to_c.cpp)
target_include_directories(map_to_c PRIVATE .)

set(MAP_FILES
        wind/active.map                 ctl/arrow_down_inactive.map
        ctl/arrow_right_active.map      ctl/arrow_up_inactive.map
        wind/ractive.map                wind/zoom.map
        menu/apple.map                  ctl/arrow_left_active.map
        ctl/arrow_right_inactive.map    wind/go_away.map
        ctl/thumb_horiz.map             ctl/arrow_down_active.map
        ctl/arrow_left_inactive.map     ctl/arrow_up_active.map
        wind/grow.map                   ctl/thumb_vert.map
    )
set(GENERATED_FILES)
foreach(mapfile ${MAP_FILES})
    get_filename_component(base ${mapfile} NAME_WE)
    add_custom_command(
        OUTPUT ${base}.cmap
        DEPENDS ${mapfile}
        COMMAND map_to_c < ${CMAKE_CURRENT_SOURCE_DIR}/${mapfile} > ${base}.cmap
        )
    list(APPEND GENERATED_FILES ${base}.cmap)
endforeach()


add_custom_command(OUTPUT parse.tab.cpp
        DEPENDS prefs/parse.ypp
        COMMAND ${BISON_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/prefs/parse.ypp)
list(APPEND GENERATED_FILES parse.tab.cpp)


set(ctl_sources
    #include/ControlMgr.h
    ctl/ctl.h
    ctl/ctlArrows.cpp ctl/ctlDisplay.cpp ctl/ctlIMIV.cpp ctl/ctlInit.cpp ctl/ctlMisc.cpp
    ctl/ctlMouse.cpp ctl/ctlSet.cpp ctl/ctlSize.cpp ctl/ctlStddef.cpp ctl/ctlPopup.cpp)
source_group(Controls FILES ${ctl_sources})

set(dial_sources
    #include/DialogMgr.h
    dial/dial.h
    dial/itm.h
    dial/dialAlert.cpp dial/dialCreate.cpp dial/dialHandle.cpp dial/dialInit.cpp
    dial/dialManip.cpp dial/dialDispatch.cpp dial/dialItem.cpp)
source_group(Dialogs FILES ${dial_sources})

set(file_sources
    #include/FileMgr.h
    file/file.h
    file/volume.h

    file/fileMisc.cpp
    file/fileHighlevel.cpp  file/fileUnimplemented.cpp
    file/volume.cpp
    
    file/localvolume/localvolume.h 
    file/localvolume/localvolume.cpp 

    file/localvolume/openfile.h

    file/localvolume/item.h 
    file/localvolume/item.cpp

    file/localvolume/itemcache.h
    file/localvolume/itemcache.cpp

    file/localvolume/basilisk.h
    file/localvolume/basilisk.cpp
    file/localvolume/appledouble.h
    file/localvolume/appledouble.cpp
    file/localvolume/plain.h
    file/localvolume/plain.cpp
    file/localvolume/mac.h
    file/localvolume/mac.cpp

    file/localvolume/cnidmapper.h
    file/localvolume/simplecnidmapper.h
    file/localvolume/simplecnidmapper.cpp
    file/localvolume/lmdbcnidmapper.h
    file/localvolume/lmdbcnidmapper.cpp
    )

source_group(Files FILES ${file_sources})

set(hfs_sources 
    hfs/hfs.h
    hfs/hfs_plus.h
    hfs/partition.h
    hfs/drive_flags.h
    hfs/futzwithdosdisks.h
    hfs/hfsBtree.cpp hfs/hfsChanging.cpp hfs/hfsCreate.cpp hfs/hfsFile.cpp
    hfs/hfsHelper.cpp hfs/hfsHier.cpp hfs/hfsMisc.cpp hfs/hfsVolume.cpp
    hfs/hfsWorkingdir.cpp hfs/hfsXbar.cpp hfs/futzwithdosdisks.cpp)
source_group(HFS FILES ${hfs_sources})

set(list_sources
    list/list.h
    list/listAccess.cpp list/listAddDel.cpp list/listCreate.cpp list/listDisplay.cpp
    list/listMouse.cpp list/listOps.cpp list/listStdLDEF.cpp)
source_group(Lists FILES ${list_sources})

set(menu_sources
    menu/menu.h
    menu/menu.cpp menu/menuColor.cpp menu/menuV.cpp menu/stdmdef.cpp menu/stdmbdf.cpp)
source_group(Menus FILES ${menu_sources})

set(print_sources
    print/print.h
    print/nextprint.h
    print/ini.h
    print/ini.cpp 
    print/PSstrings.h
    print/PSstrings.cpp print/prError.cpp print/prInit.cpp print/prLowLevel.cpp
    print/prPrinting.cpp print/prRecords.cpp print/PSprint.cpp )
source_group(Printing FILES ${print_sources})

set(qd_sources
    quickdraw/quick.h
    quickdraw/cquick.h    
    quickdraw/depthconv.h
    quickdraw/qcolor.h
    quickdraw/rawblt.h
    quickdraw/gworld.h
    quickdraw/font.h
    quickdraw/picture.h
    quickdraw/region.h
    quickdraw/rgbutil.h
    quickdraw/srcblt.h
    quickdraw/stdbits.h
    quickdraw/xdata.h
    quickdraw/xdblt.h
    quickdraw/image.h
    quickdraw/text.h

    quickdraw/hintemplate.h

    quickdraw/qBit.cpp quickdraw/qCConv.cpp quickdraw/qCGrafPort.cpp
    quickdraw/qCRegular.cpp quickdraw/qColor.cpp quickdraw/qColorMgr.cpp
    quickdraw/qColorutil.cpp quickdraw/qCursor.cpp quickdraw/qGrafport.cpp
    quickdraw/qIMIV.cpp quickdraw/qIMV.cpp quickdraw/qIMVxfer.cpp
    quickdraw/qMisc.cpp quickdraw/qPaletteMgr.cpp quickdraw/qPen.cpp
    quickdraw/qPicstuff.cpp quickdraw/qPicture.cpp quickdraw/qPixMapConv.cpp
    quickdraw/qPoint.cpp quickdraw/qPoly.cpp quickdraw/qRect.cpp
    quickdraw/qRegion.cpp quickdraw/qRegular.cpp quickdraw/qScale.cpp
    quickdraw/qStandard.cpp quickdraw/qStdArc.cpp quickdraw/qStdBits.cpp
    quickdraw/qStdLine.cpp quickdraw/qStdOval.cpp quickdraw/qStdPic.cpp
    quickdraw/qStdPoly.cpp quickdraw/qStdRRect.cpp quickdraw/qStdRect.cpp
    quickdraw/qStdRgn.cpp quickdraw/qStdText.cpp quickdraw/qText.cpp
    quickdraw/qGWorld.cpp quickdraw/qGDevice.cpp quickdraw/qIMVI.cpp
    quickdraw/qHooks.cpp quickdraw/xdata.cpp quickdraw/xdblt.cpp
    quickdraw/rawpatblt.cpp quickdraw/rawsrcblt.cpp quickdraw/srcblt.cpp
     quickdraw/qPict2.cpp quickdraw/image.cpp
    quickdraw/image_inits.cpp quickdraw/default_ctab_values.cpp
    quickdraw/dcconvert.cpp quickdraw/rgbutil.cpp quickdraw/dcmaketables.cpp
    quickdraw/fontIMVI.cpp quickdraw/font.cpp
    quickdraw/displays.cpp
)

source_group(QuickDraw FILES ${qd_sources})

add_executable(mkseedtables quickdraw/mkseedtables/mkseedtables.c)
add_executable(mkultable quickdraw/mkultable/mkultable.c)
add_executable(mksspairtable quickdraw/mksspairtable/mksspairtable.c)
add_custom_command(OUTPUT seedtables.ctable COMMAND mkseedtables > seedtables.ctable)
add_custom_command(OUTPUT ultable.ctable COMMAND mkultable > ultable.ctable)
add_custom_command(OUTPUT sspairtable.ctable COMMAND mksspairtable > sspairtable.ctable)
list(APPEND GENERATED_FILES seedtables.ctable ultable.ctable sspairtable.ctable)

add_custom_command(OUTPUT rawpatstubs.ctable
    DEPENDS quickdraw/makerawblt.pl quickdraw/pat-blitters.tmpl
    COMMAND ${PERL_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/quickdraw/makerawblt.pl
        < ${CMAKE_CURRENT_SOURCE_DIR}/quickdraw/pat-blitters.tmpl
        > rawpatstubs.ctable)

add_custom_command(OUTPUT rawsrcstubs.ctable
    DEPENDS quickdraw/makerawblt.pl quickdraw/src-blitters.tmpl
    COMMAND ${PERL_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/quickdraw/makerawblt.pl
        < ${CMAKE_CURRENT_SOURCE_DIR}/quickdraw/src-blitters.tmpl
        > rawsrcstubs.ctable)
list(APPEND GENERATED_FILES rawpatstubs.ctable rawsrcstubs.ctable)

list(TRANSFORM GENERATED_FILES PREPEND ${CMAKE_CURRENT_BINARY_DIR}/)
source_group(Generated FILES ${GENERATED_FILES})

set(res_sources
    res/resource.h

    res/resGet.cpp res/resGetinfo.cpp res/resGettype.cpp res/resIMIV.cpp
    res/resInit.cpp res/resMisc.cpp res/resMod.cpp res/resOpen.cpp
    res/resSetcur.cpp res/resPartial.cpp) 

source_group(Resources FILES ${res_sources})

set(te_sources
    textedit/tesave.h
    textedit/textedit.h

    textedit/teAccess.cpp textedit/teDisplay.cpp textedit/teEdit.cpp 
    textedit/teIMIV.cpp textedit/teIMV.cpp textedit/teInit.cpp
    textedit/teInsert.cpp textedit/teMisc.cpp textedit/teScrap.cpp)
source_group(TextEdit FILES ${te_sources})

set(wind_sources
    wind/wind.h
    
    wind/windColor.cpp wind/windDisplay.cpp wind/windDocdef.cpp
    wind/windInit.cpp wind/windMisc.cpp wind/windMouse.cpp wind/windSize.cpp
    wind/windUpdate.cpp wind/windRootless.cpp)
source_group(Windows FILES ${wind_sources})

set(ae_sources
    appleevent/apple_events.h
    appleevent/AE.cpp appleevent/AE_desc.cpp appleevent/AE_hdlr.cpp
    appleevent/AE_coercion.cpp)
source_group(AppleEvents FILES ${ae_sources})

set(sound_sources
    sound/sounddriver.h
    sound/soundopts.h
    sound/soundfake.h
    
    sound/sounddriver.cpp sound/sound.cpp sound/soundIMVI.cpp sound/soundfake.cpp
    sound/snth5.cpp

    SpeechManager.cpp)
if(HOST_OS STREQUAL macosx)
set(sound_sources ${sound_sources} SpeechManager-MacBridge.mm)
endif()
source_group(Sound FILES ${sound_sources})

set(num_sources
    sane/float_fcw.h sane/float.h sane/floatconv.h
    sane/float4.cpp sane/float5.cpp sane/float7.cpp sane/floatnext.cpp)
source_group(Numerics FILES ${num_sources})


set(base_sources
        # include/ExMacTypes.h
        
        base/common.h
        base/mactype.h
        base/lowglobals.h
        base/functions.h
        base/functions.impl.h
        base/byteswap.h

        base/m68kint.h
        base/cpu.h
        base/cpu.cpp    

        base/traps.h
        base/traps.impl.h
        base/trapname.h
        base/dispatcher.h
        base/trapglue.h
        base/logging.h
        base/api-module.h
        base/emustubs.h
    
        base/trapname.cpp
        base/traps.cpp
        base/dispatcher.cpp
        base/emustubs.cpp
        base/logging.cpp
    
        base/builtinlibs.h
        base/builtinlibs.cpp

        base/debugger.h
        base/debugger.cpp

        base/patches.cpp
    )
source_group(Base FILES ${base_sources})

set(vdriver_sources
        vdriver/vdriver.cpp
        vdriver/refresh.cpp
        vdriver/autorefresh.cpp 
        vdriver/dirtyrect.cpp
        vdriver/dirtyrect.h
        vdriver/autorefresh.h
        vdriver/refresh.h
        vdriver/vdriver.h
        vdriver/eventsink.cpp
        vdriver/eventrecorder.h
        vdriver/eventrecorder.cpp
    )
source_group(VDriver FILES ${vdriver_sources})

set(mman_sources
    mman/memsize.h
    mman/mman.h
    mman/mman_private.h
    mman/tempalloc.h

    mman/mman.cpp mman/mmansubr.cpp
    mman/tempmem.cpp
)
source_group(MMan FILES ${mman_sources})

set(time_sources
        time/time.h
        time/vbl.h
        time/syncint.h

        time/time.cpp
        time/vbl.cpp
        time/syncint.cpp
    )
source_group(Time FILES ${time_sources})

set(osevent_sources
        osevent/osevent.h
        osevent/hle.cpp
        osevent/ibm_keycodes.cpp
        osevent/osevent.cpp
    )
source_group(OSEvent FILES ${osevent_sources})

set(error_sources
    error/error.h error/syserr.h error/system_error.h
    error/error.cpp error/syserr.cpp error/system_error.cpp
)

source_group(Error FILES ${error_sources})

set(commandline_sources
    commandline/program_options_extended.h
    commandline/program_options_extended.cpp
    commandline/option_arguments.h
    commandline/flags.h
    )

source_group(Commandline FILES ${commandline_sources})

set(prefs_sources
    prefs/prefs.h
    prefs/parse.h
    prefs/options.h
    prefs/crc.h

    prefs/parse.ypp
    prefs/crc.cpp
    prefs/options.cpp
    prefs/prefs.cpp
)

source_group(Prefs FILES ${prefs_sources})

set(util_sources
    util/macstrings.h
    util/macstrings.cpp

    util/string.h
    util/string.cpp
    
    util/uniquefile.h
    util/uniquefile.cpp

    util/handle_vector.h
    util/static_vector.h
)

source_group(Util FILES ${util_sources})

set(debug_sources
    debug/mon_debugger.h
    debug/mon_debugger.cpp
)

source_group(Debugger FILES ${debug_sources})


set(mpw_sources
    mpw/mpw.h
    mpw/mpw.cpp
    )
source_group(MPW FILES ${mpw_sources})


set(misc_sources
  desk.cpp device.cpp disk.cpp diskinit.cpp dump.cpp
  gestalt.cpp iu.cpp launch.cpp  notify.cpp 
   osutil.cpp pack.cpp scrap.cpp script.cpp segment.cpp serial.cpp
  stdfile.cpp romlib_stubs.cpp  toolevent.cpp toolutil.cpp 
   aboutbox.cpp
  version.cpp shutdown.cpp screen-dump.cpp
  process.cpp alias.cpp edition.cpp  balloon.cpp
   adb.cpp color_wheel_bits.cpp finder.cpp  
  icon.cpp redrawscreen.cpp quicktime.cpp
  commtool.cpp cfm.cpp pef_hash.cpp
  mixed_mode.cpp appearance.cpp
  executor.cpp  
  prefpanel.cpp
  qColorPicker.cpp
  bindec.cpp 
  toolmath.cpp
  init.cpp
  )


set(include_sources 
                  include/rsys/aboutbox.h
                  include/rsys/adb.h
                  include/rsys/alias.h
                  include/rsys/appearance.h
                  include/rsys/arrowkeys.h
                  include/rsys/color_wheel_bits.h
                  include/rsys/desk.h
                  include/rsys/device.h
                  include/rsys/dump.h
                  include/rsys/evil.h
                  include/rsys/executor.h
                  include/rsys/filesystem.h
                  include/rsys/gestalt.h
                  include/rsys/hook.h
                  include/rsys/icon.h
                  include/rsys/keyboard.h
                  include/rsys/launch.h
                  include/rsys/macros.h
                  include/rsys/noreturn.h
                  include/rsys/osutil.h
                  include/rsys/paths.h
                  include/rsys/prefpanel.h
                  include/rsys/process.h
                  include/rsys/redrawscreen.h
                  include/rsys/safe_alloca.h
                  include/rsys/scrap.h
                  include/rsys/screen-dump.h
                  include/rsys/segment.h
                  include/rsys/serial.h
                  include/rsys/unixio.h
                  include/rsys/stdfile.h
                  include/rsys/toolevent.h
                  include/rsys/toolutil.h
                  include/rsys/version.h
                  mkvol/mkvol.h
                  mkvol/mkvol_internal.h)

source_group(APIHeaders FILES ${api_headers})
source_group(TrapInstances FILES ${trap_instance_sources})

set(sources ${base_sources} ${mman_sources} ${vdriver_sources}
    ${ctl_sources} ${dial_sources} 
    ${list_sources} ${menu_sources}
    ${print_sources} ${qd_sources} ${res_sources}
    ${te_sources} ${wind_sources} ${ae_sources}
    ${sound_sources} ${num_sources} ${misc_sources}
    ${file_sources} ${hfs_sources} ${time_sources} ${osevent_sources}
    ${error_sources} ${commandline_sources} ${prefs_sources}
    ${util_sources} ${debug_sources}
    ${api_headers} ${trap_instance_sources} ${host_os_sources}
    ${mpw_sources}
    ${include_sources})

add_library(romlib ${sources}
        ${GENERATED_FILES}
    )

target_include_directories(romlib PUBLIC . include/ ${CMAKE_CURRENT_BINARY_DIR}/api/ ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(romlib PUBLIC config/os/${HOST_OS})


target_link_libraries(romlib lmdb)

target_link_libraries(romlib Threads::Threads syn68k PowerCore mon)

target_link_libraries(romlib Boost::filesystem)  # mxe doesn't like this
if(WIN32 AND NOT MSVC)
    # mingw build.
    # Boost's 'superior' build system doesn't manage library dependencies correctly
    # but relies on MSVC's autolink feature, which doesn't exist on mingw.
    target_link_libraries(romlib bcrypt)
endif()

target_link_libraries(romlib resources)
target_link_libraries(romlib ${host_os_libs})

if(APPLE)
    set(platform_resource_files ../icon/executor.icns)
    set_source_files_properties(../icon/executor.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
else()
    set(platform_resource_files)
endif()

set(BUILT_FRONT_ENDS)
foreach(FRONT_END ${FRONT_ENDS})
    if(TARGET front-end-${FRONT_END} OR ${FRONT_END} STREQUAL headless)
        list(APPEND BUILT_FRONT_ENDS ${FRONT_END})
    endif()
endforeach()

foreach(FRONT_END ${BUILT_FRONT_ENDS})
    add_executable(executor-${FRONT_END} main.cpp ${platform_resource_files})
    target_link_libraries(executor-${FRONT_END} romlib front-end-headless Boost::program_options)
    if(${FRONT_END} STREQUAL headless)
        target_compile_definitions(executor-${FRONT_END} PUBLIC -DHEADLESS_ONLY)
    else()
        target_link_libraries(executor-${FRONT_END} romlib front-end-${FRONT_END})
    endif()
    set_target_properties(executor-${FRONT_END} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})


    if(APPLE AND NOT ${FRONT_END} STREQUAL headless)
        set_target_properties(executor-${FRONT_END} PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_IDENTIFIER com.github.autc04.executor
                MACOSX_BUNDLE_ICON_FILE executor.icns
                MACOSX_BUNDLE_BUNDLE_NAME "Executor 2000"
            )
    endif()
    
    install(TARGETS executor-${FRONT_END}
        RUNTIME DESTINATION bin
        BUNDLE DESTINATION .)

endforeach()

message("All Front Ends: ${FRONT_ENDS}")
message("Built Front Ends: ${BUILT_FRONT_ENDS}")
list(GET BUILT_FRONT_ENDS 0 DEFAULT_FRONT_END)
message("Default Front End: ${DEFAULT_FRONT_END}")
if(APPLE AND NOT ${DEFAULT_FRONT_END} STREQUAL headless)
    set_target_properties(executor-${DEFAULT_FRONT_END} PROPERTIES OUTPUT_NAME "Executor 2000")
else()
    set_target_properties(executor-${DEFAULT_FRONT_END} PROPERTIES OUTPUT_NAME executor)
endif()
