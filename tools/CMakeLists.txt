add_executable(executor-tool executor-tool.cpp)

target_link_libraries(executor-tool romlib mon)

target_compile_features(executor-tool PRIVATE cxx_std_20)

find_package(Boost REQUIRED CONFIG COMPONENTS json)

# executor-mcp is optional: it needs libpng for screenshot output.
find_package(PNG)
if(PNG_FOUND)
    add_subdirectory(mcp)
else()
    message(STATUS "executor-mcp will not be built: libpng not found")
endif()
