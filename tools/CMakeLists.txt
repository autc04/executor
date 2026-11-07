add_executable(executor-tool executor-tool.cpp)

target_link_libraries(executor-tool romlib mon)

target_compile_features(executor-tool PRIVATE cxx_std_20)

find_package(Boost REQUIRED CONFIG COMPONENTS json)

if(NOT WIN32)
    add_subdirectory(mcp)
endif()
