add_executable(executor-mcp
    main.cpp
    session.cpp
    tools.cpp
    base64.cpp
    png.cpp
)

target_link_libraries(executor-mcp Boost::json PNG::PNG)

if(WIN32)
    # asio needs the Winsock libraries, and MinGW does not autolink.
    target_link_libraries(executor-mcp ws2_32 mswsock)
endif()

target_compile_features(executor-mcp PRIVATE cxx_std_20)

# Keep the binary next to executor-tool.
set_target_properties(executor-mcp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
