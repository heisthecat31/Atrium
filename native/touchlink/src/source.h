// TouchLink's source: the CV1 Touches, read from Atrium's tracking service (Monado with the Rift
// drivers and the Rift sensors, no compositor) through a headless OpenXR session. No Meta software.
#pragma once

#include "common.h"

#include <mutex>

namespace tl {

// One Touch's buttons and axes at the last Sync.
struct TouchInput {
	bool active = false;
	float trigger = 0, squeeze = 0;
	XrVector2f stick = {};
	bool triggerTouch = false, stickClick = false, stickTouch = false, thumbrest = false;
	bool lower = false, lowerTouch = false, upper = false, upperTouch = false;   // X/A, Y/B
	bool menu = false;   // left: the menu button; right: the Oculus button (Meta's source)
};

// One Touch's grip pose in the Touch tracking space, with velocities in that space.
struct TouchPose {
	bool valid = false;            // orientation and position known
	bool positionTracked = false;  // the sensors see it now (not dead reckoning)
	XrPosef grip = kIdentity;
	XrVector3f linVel = {}, angVel = {};
};

class Source {
public:
	// Connects to the service whose runtime manifest is given. Blocks up to a second or so.
	bool Connect(const std::wstring& manifest, std::string* why);
	// Or to the Touches as Meta's runtime tracks them (touchlink.exe meta, meta.cpp, shared memory).
	bool ConnectMeta(std::string* why);
	void Disconnect();
	bool Ready() const { return m_ready; }

	bool Sync(TouchInput out[2]);
	bool Locate(int hand, LONGLONG qpc, TouchPose* out);
	void Haptic(int hand, float seconds, float frequency, float amplitude);

private:
	template <typename F> F Fn(const char* name);

	// The Meta source (meta_shared.h): its read of the shared memory, and the pose it read at.
	bool MetaRead(struct MetaShared* out);
	HANDLE m_metaMap = nullptr;
	struct MetaShared* m_meta = nullptr;

	std::mutex m_lock;
	HMODULE m_dll = nullptr;
	PFN_xrGetInstanceProcAddr m_gipa = nullptr;
	XrInstance m_inst = XR_NULL_HANDLE;
	XrSession m_sess = XR_NULL_HANDLE;
	XrSpace m_stage = XR_NULL_HANDLE;
	XrActionSet m_set = XR_NULL_HANDLE;
	XrAction m_act[16] = {};
	XrPath m_hand[2] = {};
	XrSpace m_grip[2] = {};
	bool m_ready = false;

	PFN_xrPollEvent m_pollEvent = nullptr;
	PFN_xrSyncActions m_sync = nullptr;
	PFN_xrGetActionStateBoolean m_getBool = nullptr;
	PFN_xrGetActionStateFloat m_getFloat = nullptr;
	PFN_xrGetActionStateVector2f m_getVec2 = nullptr;
	PFN_xrGetActionStatePose m_getPose = nullptr;
	PFN_xrLocateSpace m_locate = nullptr;
	PFN_xrApplyHapticFeedback m_haptic = nullptr;
	PFN_xrConvertWin32PerformanceCounterToTimeKHR m_fromQpc = nullptr;
	PFN_xrDestroyInstance m_destroyInstance = nullptr;
};

} // namespace tl
