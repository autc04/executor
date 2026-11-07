add_executable(executor-tool executor-tool.cpp)

target_link_libraries(executor-tool romlib mon)

target_compile_features(executor-tool PRIVATE cxx_std_20)

find_package(Boost REQUIRED CONFIG COMPONENTS json)

add_executable(executor-mcp executor-mcp.cpp)

target_link_libraries(executor-mcp Boost::json)

target_compile_features(executor-mcp PRIVATE cxx_std_20)

# Optional: compresses screenshot PNGs (the stored-deflate fallback works but is
# much larger).
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(executor-mcp ZLIB::ZLIB)
    target_compile_definitions(executor-mcp PRIVATE HAVE_ZLIB=1)
endif()
