add_executable(executor-mcp
    main.cpp
    session.cpp
    tools.cpp
    base64.cpp
    png.cpp
)

target_link_libraries(executor-mcp Boost::json)

target_compile_features(executor-mcp PRIVATE cxx_std_20)

# Keep the binary next to executor-tool.
set_target_properties(executor-mcp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(executor-mcp ZLIB::ZLIB)
    target_compile_definitions(executor-mcp PRIVATE HAVE_ZLIB=1)
endif()
