# Atrium OVR: Atrium's translation layer for Oculus SDK (LibOVR) games.
#
# Builds LibOVRRT64_1.dll, a stand-in for Meta's Oculus runtime that answers a game's LibOVR
# calls over OpenXR, so the game runs on whatever OpenXR runtime is active (the Atrium Runtime).
# The answering code is ReviveXR (Revive, MIT, LibreVR), fetched at a pinned commit; Atrium adds
# its own entry point (src/main.cpp). The OpenXR loader is linked in statically, so the layer
# is this one DLL.
#
# Games load it without injection: LibOVR's own loader (the shim compiled into every Oculus SDK
# game) checks LIBOVR_DLL_DIR for LibOVRRT64_1.dll first. Atrium's launcher sets that variable
# to this DLL's folder when a game starts in "Translated to OpenXR" mode.
#
# Output: <build>/out/ovr/LibOVRRT64_1.dll. native\build.bat copies it to native\out\ovr\.
cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
cmake_policy(SET CMP0135 NEW)
project(AtriumOVR LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Atrium OVR builds for 64-bit Windows (Linux games run it under Proton).")
endif()

# Static C runtime: players need no Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OUT_DIR "${CMAKE_BINARY_DIR}/out/ovr")
foreach(cfg "" _RELEASE _DEBUG _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} "${OUT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${cfg} "${OUT_DIR}")
endforeach()

# --- Pinned sources -----------------------------------------------------------------------
# SOURCE_SUBDIR points at a folder that doesn't exist, so these are only downloaded, never
# added as CMake projects; the targets below pick the files they need.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
# Revive (MIT), upstream ab73167 (2023-08-29): ReviveXR, glad, and its version header. Atrium's
# changes to it are the patches in patches/, applied in order after download (edit Revive in a
# git checkout of that commit and regenerate them with git format-patch).
find_package(Git REQUIRED)
file(GLOB REVIVE_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
list(SORT REVIVE_PATCHES)
FetchContent_Declare(revive
    URL https://github.com/LibreVR/Revive/archive/ab73167e2380135aee9fe9f68874ec7dd2912cb0.tar.gz
    URL_HASH SHA256=71c3e80f2215a13062e58f49e74d238296dcc9cac49e4687d4540619e2bbede9
    # --git-dir/--work-tree: the build folder may sit inside another git repo (Atrium's), where a
    # plain git apply would skip every path.
    PATCH_COMMAND "${GIT_EXECUTABLE}" --git-dir=.nogit --work-tree=. apply --whitespace=nowarn ${REVIVE_PATCHES}
    SOURCE_SUBDIR _none)
# Oculus PC SDK 1.55 headers and two shim sources. Build time only: nothing of it ships.
FetchContent_Declare(ovr_sdk
    URL https://github.com/sparsebase/ovr_sdk_win/archive/4b1d6b7e86077062e5de33dda7d77b5790202c4c.tar.gz
    URL_HASH SHA256=b5febe29430080e901128e5de85ec4d16b8115c47f8a2953763bec378263e8c8
    SOURCE_SUBDIR _none)
# Revive's externals at the commits Revive ab73167 pins (git submodules upstream).
FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/d732b2de303ce505169011d438178191136bfb00.tar.gz
    URL_HASH SHA256=570f9ae1e65466dbaf5fcab667abd079dd0a61c4ab86cf535efd492bf70a5b74
    SOURCE_SUBDIR _none)
FetchContent_Declare(microprofile
    URL https://github.com/zeux/microprofile/archive/deda3d42c56e5d816daa480b3cff066e76b6b90b.tar.gz
    URL_HASH SHA256=0ecbb5bb07dc54b559ece8d5664cc8efb4417388429c9c3202d72030702eec50
    SOURCE_SUBDIR _none)
# Microsoft Detours (MIT): Revive's D3D hooks, and the entry point's loader/event hooks.
FetchContent_Declare(detours
    URL https://github.com/microsoft/Detours/archive/1dbd4ea281d73d478aa8d09a8b83fbd265f715f7.tar.gz
    URL_HASH SHA256=38d435d2750b4e3af57f56c77af52bc058123c5e7dd734945b7680ec52e5c91c
    SOURCE_SUBDIR _none)
# Khronos OpenXR SDK 1.1.63 (Apache-2.0): headers and the loader, built as a static library.
FetchContent_Declare(openxr_sdk
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/f2448a8797c85814aa892efc1ab8707900fbcc78.tar.gz
    URL_HASH SHA256=1681e0f6c9633c73117c2fc376d1fe71cbd6fdd2aeb896ed5854ff022aa82eaa)
FetchContent_MakeAvailable(revive ovr_sdk vulkan_headers microprofile detours)

set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(INSTALL_SYMBOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(openxr_sdk)
# The SDK gives a static loader the DLL C runtime; the layer uses the static one throughout.
set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")

# --- Detours ------------------------------------------------------------------------------
set(DETOURS_SRC "${detours_SOURCE_DIR}/src")
add_library(detours STATIC
    ${DETOURS_SRC}/detours.cpp ${DETOURS_SRC}/modules.cpp ${DETOURS_SRC}/disasm.cpp
    ${DETOURS_SRC}/image.cpp ${DETOURS_SRC}/creatwth.cpp ${DETOURS_SRC}/disolx86.cpp
    ${DETOURS_SRC}/disolx64.cpp ${DETOURS_SRC}/disolia64.cpp ${DETOURS_SRC}/disolarm.cpp
    ${DETOURS_SRC}/disolarm64.cpp)
target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601 _AMD64_)
# Revive includes it as <detours/detours.h>.
set(DETOURS_INCLUDE "${CMAKE_BINARY_DIR}/detours-include")
file(COPY ${DETOURS_SRC}/detours.h ${DETOURS_SRC}/detver.h DESTINATION "${DETOURS_INCLUDE}/detours")
target_include_directories(detours PUBLIC "${DETOURS_INCLUDE}")

# --- LibOVRRT64_1.dll ---------------------------------------------------------------------
set(REVIVE "${revive_SOURCE_DIR}")
set(REVIVE_XR
    Common.cpp HapticsBuffer.cpp InputManager.cpp REV_CAPI.cpp REV_CAPI_Audio.cpp
    REV_CAPI_D3D.cpp REV_CAPI_GL.cpp REV_CAPI_Vk.cpp Session.cpp Runtime.cpp Swapchain.cpp
    SwapchainD3D11.cpp SwapchainD3D12.cpp SwapchainGL.cpp SwapchainVk.cpp microprofile.cpp)
list(TRANSFORM REVIVE_XR PREPEND "${REVIVE}/ReviveXR/")
# glad (OpenGL loader) is C; it's built on its own because the Visual Studio generator applies
# the C++ forced include below to every file of a target.
add_library(glad STATIC ${REVIVE}/Externals/glad/src/glad.c)
target_include_directories(glad PUBLIC ${REVIVE}/Externals/glad/include)
add_library(atrium_ovr SHARED
    ${REVIVE_XR}
    ${ovr_sdk_SOURCE_DIR}/LibOVR/Shim/OVR_CAPI_Util.cpp
    ${ovr_sdk_SOURCE_DIR}/LibOVR/Shim/OVR_StereoProjection.cpp
    src/main.cpp)
set_target_properties(atrium_ovr PROPERTIES OUTPUT_NAME LibOVRRT64_1 PREFIX "")
# ReviveXR's own folder stays off the include path: its OVR_CAPI.h wraps the SDK's
# <OVR_CAPI.h>, and quoted includes find the folder's files anyway. version.h is Revive's
# (ReviveOverlay/), so the reported runtime version matches upstream Revive.
target_include_directories(atrium_ovr PRIVATE
    ${ovr_sdk_SOURCE_DIR}/LibOVR/Include
    ${REVIVE}/Externals/glad/include
    ${vulkan_headers_SOURCE_DIR}/include
    ${microprofile_SOURCE_DIR}
    ${REVIVE}/ReviveOverlay)
target_compile_definitions(atrium_ovr PRIVATE
    XR_USE_PLATFORM_WIN32 VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR NOMINMAX
    MICROPROFILE_ENABLED=0 MICROPROFILE_GPU_TIMERS=0 OVR_DLL_BUILD NDEBUG
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
# Revive uses std::chrono without including it everywhere.
target_compile_options(atrium_ovr PRIVATE /EHsc /FIchrono /W1)
target_link_libraries(atrium_ovr PRIVATE
    openxr_loader detours glad
    ws2_32 opengl32 d3d11 d3d12 dxgi dxguid dsound winmm shlwapi pathcch user32 advapi32 ole32 shell32)
