# TouchLink: Rift CV1 Touches as SteamVR controllers with any headset (src/driver.cpp).
# Stands alone: needs Atrium's tracking service at run time; of Atrium's sources it builds only
# sensordriver (one file).
#
# Output: <build>/out/touchlink.exe and <build>/out/touchlink/ (the SteamVR driver folder:
# driver.vrdrivermanifest, bin/win64/driver_touchlink.dll). native\build.bat copies them to
# native\out\touchlink\.
cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0135 NEW)
project(TouchLink LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "TouchLink builds for 64-bit Windows.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OUT_DIR "${CMAKE_BINARY_DIR}/out")

# Khronos OpenXR SDK 1.1.63 (Apache-2.0): headers only (the service's runtime is loaded directly).
include(FetchContent)
FetchContent_Declare(openxr_sdk
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/f2448a8797c85814aa892efc1ab8707900fbcc78.tar.gz
    URL_HASH SHA256=1681e0f6c9633c73117c2fc376d1fe71cbd6fdd2aeb896ed5854ff022aa82eaa
    SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(openxr_sdk)

# OpenVR SDK 2.15.6 (BSD-3-Clause): the one driver header.
set(OPENVR_DIR "${CMAKE_BINARY_DIR}/openvr")
if(NOT EXISTS "${OPENVR_DIR}/openvr_driver.h")
    file(DOWNLOAD
        https://raw.githubusercontent.com/ValveSoftware/openvr/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr_driver.h
        "${OPENVR_DIR}/openvr_driver.h"
        EXPECTED_HASH SHA256=1036efe998d63e82d1d3db2b32a2f58df4a8eeaf5280f50aaf28220ff60a40ab)
endif()

# OpenVR's client API (BSD-3-Clause), for touchlink.exe's camera mode (src/camera.cpp): the
# headset's own cameras. openvr_api.dll ships next to touchlink.exe.
foreach(f "headers/openvr.h|1e6ed57199896cc1f7c5484e50fa18955e97be15be690beb28d998c877ead7fd"
          "lib/win64/openvr_api.lib|a0bf57c5920f569e8d21ab3e5bc95bac4b73e2016217f8b5b93495a2a7197bbb"
          "bin/win64/openvr_api.dll|bab8ac6ef64e68a9ca53315b0014d131088584b2efdfa6db511d67ec03cfcb4a")
    string(REPLACE "|" ";" parts "${f}")
    list(GET parts 0 path)
    list(GET parts 1 hash)
    get_filename_component(name "${path}" NAME)
    if(NOT EXISTS "${OPENVR_DIR}/${name}")
        file(DOWNLOAD https://raw.githubusercontent.com/ValveSoftware/openvr/0924064316de3effbcd1acf1e309182a2deb1c05/${path}
            "${OPENVR_DIR}/${name}" EXPECTED_HASH SHA256=${hash})
    endif()
endforeach()

set(TL_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

add_library(driver_touchlink SHARED src/driver.cpp src/source.cpp src/calibration.cpp)
target_include_directories(driver_touchlink PRIVATE ${openxr_sdk_SOURCE_DIR}/include ${OPENVR_DIR})
target_compile_definitions(driver_touchlink PRIVATE ${TL_DEFS})
target_compile_options(driver_touchlink PRIVATE /W3 /EHsc)
target_link_libraries(driver_touchlink PRIVATE winmm)
set_target_properties(driver_touchlink PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}/touchlink/bin/win64"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}/touchlink/bin/win64"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUT_DIR}/touchlink/bin/win64")
configure_file(driver/driver.vrdrivermanifest "${OUT_DIR}/touchlink/driver.vrdrivermanifest" COPYONLY)

# Oculus PC SDK 1.55 headers and its loader shim (as native/atrium_ovr), for touchlink.exe's meta
# mode (src/meta.cpp): the Touches as Meta's runtime tracks them, an option. Build time only; at run
# time the shim loads Meta's own installed runtime.
FetchContent_Declare(ovr_sdk
    URL https://github.com/sparsebase/ovr_sdk_win/archive/4b1d6b7e86077062e5de33dda7d77b5790202c4c.tar.gz
    URL_HASH SHA256=b5febe29430080e901128e5de85ec4d16b8115c47f8a2953763bec378263e8c8
    SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(ovr_sdk)

add_executable(touchlink src/touchlink.cpp src/camera.cpp src/meta.cpp src/source.cpp src/calibration.cpp
    ${ovr_sdk_SOURCE_DIR}/LibOVR/Shim/OVR_CAPIShim.c)
target_include_directories(touchlink PRIVATE ${openxr_sdk_SOURCE_DIR}/include ${OPENVR_DIR} ${ovr_sdk_SOURCE_DIR}/LibOVR/Include)
target_compile_definitions(touchlink PRIVATE ${TL_DEFS})
target_compile_options(touchlink PRIVATE /W3 /EHsc)
target_link_libraries(touchlink PRIVATE "${OPENVR_DIR}/openvr_api.lib")
configure_file("${OPENVR_DIR}/openvr_api.dll" "${OUT_DIR}/openvr_api.dll" COPYONLY)
set_target_properties(touchlink PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUT_DIR}")

# sensordriver: Atrium's tool that puts the Rift sensors on WinUSB (native/src/sensordriver.cpp,
# one file, Windows only). touchlink setup runs it, so it ships next to touchlink.exe.
add_executable(sensordriver ../src/sensordriver.cpp)
target_compile_definitions(sensordriver PRIVATE UNICODE _UNICODE)
target_compile_options(sensordriver PRIVATE /W3 /EHsc)
set_target_properties(sensordriver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUT_DIR}")

# calib_check: the solve on synthetic tracks (a check, not shipped).
add_executable(calib_check src/calib_check.cpp src/calibration.cpp)
target_include_directories(calib_check PRIVATE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_definitions(calib_check PRIVATE ${TL_DEFS})
target_compile_options(calib_check PRIVATE /W3 /EHsc)
set_target_properties(calib_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/check"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/check")
