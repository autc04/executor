add_executable(executor-tool executor-tool.cpp)

target_link_libraries(executor-tool romlib mon)

target_compile_features(executor-tool PRIVATE cxx_std_17)
