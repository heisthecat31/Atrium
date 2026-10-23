# atrium-dash: Atrium's in-VR dashboard (dash_main.cpp).
#
# Output: <build>/out/atrium-dash.exe. native\build.bat copies it to native\out\, where the Atrium
# app finds and starts it. The Khronos OpenXR loader is linked in (home runs its own session).
cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0135 NEW)
project(AtriumDash LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OUT_DIR "${CMAKE_BINARY_DIR}/out")
foreach(cfg "" _RELEASE _DEBUG _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} "${OUT_DIR}")
endforeach()

# Khronos OpenXR SDK 1.1.63 (Apache-2.0): headers and the loader, built static.
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(openxr_sdk
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/f2448a8797c85814aa892efc1ab8707900fbcc78.tar.gz
    URL_HASH SHA256=1681e0f6c9633c73117c2fc376d1fe71cbd6fdd2aeb896ed5854ff022aa82eaa)
FetchContent_MakeAvailable(openxr_sdk)
# The SDK builds a static loader against the DLL C runtime; atrium-dash is all static.
set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(atrium-dash WIN32 dash_main.cpp)
target_compile_definitions(atrium-dash PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(atrium-dash PRIVATE /W3 /EHsc /utf-8)
target_link_libraries(atrium-dash PRIVATE openxr_loader)
