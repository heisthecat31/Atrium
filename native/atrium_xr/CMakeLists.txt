# The Atrium Runtime: Atrium's OpenXR runtime (src/runtime.cpp).
#
# Output: <build>/out/atrium_xr.dll and atrium_openxr.json (the runtime manifest the OpenXR
# loader reads). native\build.bat copies both to native\out\, where the Atrium app finds them.
cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0135 NEW)
project(AtriumRuntime LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Atrium Runtime builds for 64-bit Windows for now.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OUT_DIR "${CMAKE_BINARY_DIR}/out")
foreach(cfg "" _RELEASE _DEBUG _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} "${OUT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${cfg} "${OUT_DIR}")
endforeach()

# Khronos OpenXR SDK 1.1.63 (Apache-2.0): headers only; the loader is the game's.
include(FetchContent)
FetchContent_Declare(openxr_sdk
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/f2448a8797c85814aa892efc1ab8707900fbcc78.tar.gz
    URL_HASH SHA256=1681e0f6c9633c73117c2fc376d1fe71cbd6fdd2aeb896ed5854ff022aa82eaa
    SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(openxr_sdk)

add_library(atrium_xr SHARED src/runtime.cpp)
target_include_directories(atrium_xr PRIVATE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_definitions(atrium_xr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(atrium_xr PRIVATE /W3 /EHsc)
target_link_libraries(atrium_xr PRIVATE shlwapi shell32 ole32)
configure_file(atrium_openxr.json "${OUT_DIR}/atrium_openxr.json" COPYONLY)
