cmake_minimum_required(VERSION 3.25)
project(CodexProcessGuardNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(guard_core src/core.cpp src/native.cpp)
target_include_directories(guard_core PUBLIC src)
target_compile_options(guard_core PRIVATE /W4 /WX /permissive-)
target_link_libraries(guard_core PUBLIC iphlpapi psapi ws2_32)

add_executable(guard_tests tests/tests.cpp)
target_link_libraries(guard_tests PRIVATE guard_core)
target_compile_options(guard_tests PRIVATE /W4 /WX /permissive-)

add_executable(CodexProcessGuardNative WIN32 src/app.cpp)
target_link_libraries(CodexProcessGuardNative PRIVATE guard_core comctl32 advapi32)
target_compile_definitions(CodexProcessGuardNative PRIVATE UNICODE _UNICODE)
target_compile_options(CodexProcessGuardNative PRIVATE /W4 /WX /permissive- $<$<NOT:$<CONFIG:Debug>>:/O2>)

enable_testing()
add_test(NAME guard_tests COMMAND guard_tests)
