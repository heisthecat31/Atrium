// CV1 Touch swap: the game's controllers come from Rift CV1 Touches that Atrium's own runtime
// service tracks (Monado's rift driver + the Rift sensors), while the backend (Virtual Desktop,
// ...) keeps the headset and the picture. See touch.cpp.
#pragma once

#include <string>

#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <unknwn.h>   // the Win32 platform header's IUnknown (WIN32_LEAN_AND_MEAN drops it)
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace touch {

// Set up from runtime.json ("touch": the Atrium service's runtime manifest) or ATRIUM_XR_TOUCH.
// Called once at negotiation; the swap stays off when nothing is configured.
void Configure(const std::wstring& manifest, void (*log)(const char*, ...));
bool Configured();

// After the game's instance exists (it was created with the time conversion extension when
// the backend has it). fixTime turns OVRPlugin's time 0 into now.
void OnInstanceCreated(XrInstance instance, PFN_xrGetInstanceProcAddr backendGipa, XrTime (*fixTime)(XrTime));
void OnInstanceDestroyed(XrInstance instance);

// The wrapper for an OpenXR function name, or null when the swap doesn't touch it.
PFN_xrVoidFunction Hook(const char* name);

} // namespace touch
