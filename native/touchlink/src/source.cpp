// The Touches from Atrium's tracking service: the same headless session the Atrium Runtime's
// Touch swap opens (native/atrium_xr/src/touch.cpp), on its own here.
#include "source.h"
#include "meta_shared.h"

#include <algorithm>

#include <openxr/openxr_loader_negotiation.h>

namespace tl {
namespace {

enum Act {
	A_GRIP, A_TRIGGER, A_TRIGGER_TOUCH, A_SQUEEZE, A_STICK, A_STICK_CLICK, A_STICK_TOUCH, A_THUMBREST,
	A_LOWER, A_LOWER_TOUCH, A_UPPER, A_UPPER_TOUCH, A_MENU, A_HAPTIC, A_COUNT
};
struct ActDef { const char* name; XrActionType type; const char* left; const char* right; };
const ActDef kActs[A_COUNT] = {
	{ "grip", XR_ACTION_TYPE_POSE_INPUT, "input/grip/pose", "input/grip/pose" },
	{ "trigger", XR_ACTION_TYPE_FLOAT_INPUT, "input/trigger/value", "input/trigger/value" },
	{ "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/trigger/touch", "input/trigger/touch" },
	{ "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, "input/squeeze/value", "input/squeeze/value" },
	{ "stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "input/thumbstick", "input/thumbstick" },
	{ "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/thumbstick/click", "input/thumbstick/click" },
	{ "stick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/thumbstick/touch", "input/thumbstick/touch" },
	{ "thumbrest", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/thumbrest/touch", "input/thumbrest/touch" },
	{ "lower", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/x/click", "input/a/click" },
	{ "lower_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/x/touch", "input/a/touch" },
	{ "upper", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/y/click", "input/b/click" },
	{ "upper_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/y/touch", "input/b/touch" },
	{ "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/menu/click", nullptr },
	{ "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, "output/haptic", "output/haptic" },
};

// The runtime library a manifest names (relative to the manifest unless absolute).
std::wstring ManifestLibrary(const std::wstring& manifest)
{
	std::string text, lib;
	if (!ReadFileText(manifest, &text) || !JsonString(text, "library_path", &lib)) return {};
	std::wstring w = Widen(lib);
	for (auto& c : w)
		if (c == L'/') c = L'\\';
	if (w.size() < 2 || (w[1] != L':' && w[0] != L'\\')) w = manifest.substr(0, manifest.find_last_of(L"\\/") + 1) + w;
	return w;
}

} // namespace

template <typename F> F Source::Fn(const char* name)
{
	PFN_xrVoidFunction f = nullptr;
	m_gipa(m_inst, name, &f);
	return (F)f;
}

#define CHECK(call, what)                                                         \
	do {                                                                          \
		XrResult r_ = (call);                                                     \
		if (XR_FAILED(r_)) {                                                      \
			*why = std::string(what) + " failed (" + std::to_string(r_) + ")"; \
			return false;                                                         \
		}                                                                         \
	} while (0)

bool Source::Connect(const std::wstring& manifest, std::string* why)
{
	std::lock_guard<std::mutex> lk(m_lock);
	if (m_ready) return true;
	if (!m_dll) {
		std::wstring lib = ManifestLibrary(manifest);
		if (lib.empty()) {
			*why = "no library_path in " + Narrow(manifest);
			return false;
		}
		m_dll = LoadLibraryExW(lib.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!m_dll) {
			*why = Narrow(lib) + " didn't load (error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(m_dll, "xrNegotiateLoaderRuntimeInterface");
		XrNegotiateLoaderInfo li = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof li, 1,
			XR_CURRENT_LOADER_RUNTIME_VERSION, XR_MAKE_VERSION(1, 0, 0), XR_CURRENT_API_VERSION };
		XrNegotiateRuntimeRequest rq = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof rq };
		if (!negotiate || XR_FAILED(negotiate(&li, &rq)) || !rq.getInstanceProcAddr) {
			*why = "negotiating with " + Narrow(lib) + " failed";
			FreeLibrary(m_dll);
			m_dll = nullptr;
			return false;
		}
		m_gipa = rq.getInstanceProcAddr;
	}

	PFN_xrCreateInstance createInstance = nullptr;
	m_gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&createInstance);
	const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME };
	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy_s(ici.applicationInfo.applicationName, "TouchLink");
	strcpy_s(ici.applicationInfo.engineName, "TouchLink");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = exts;
	CHECK(createInstance(&ici, &m_inst), "connecting to the tracking service");

	m_destroyInstance = Fn<PFN_xrDestroyInstance>("xrDestroyInstance");
	// From here on a failure leaves the instance to Disconnect().
	auto fail = [&](const std::string& what, XrResult r) {
		*why = what + " failed (" + std::to_string(r) + ")";
		m_destroyInstance(m_inst);
		m_inst = XR_NULL_HANDLE;
		m_sess = XR_NULL_HANDLE;
		return false;
	};
	m_pollEvent = Fn<PFN_xrPollEvent>("xrPollEvent");
	m_sync = Fn<PFN_xrSyncActions>("xrSyncActions");
	m_getBool = Fn<PFN_xrGetActionStateBoolean>("xrGetActionStateBoolean");
	m_getFloat = Fn<PFN_xrGetActionStateFloat>("xrGetActionStateFloat");
	m_getVec2 = Fn<PFN_xrGetActionStateVector2f>("xrGetActionStateVector2f");
	m_getPose = Fn<PFN_xrGetActionStatePose>("xrGetActionStatePose");
	m_locate = Fn<PFN_xrLocateSpace>("xrLocateSpace");
	m_haptic = Fn<PFN_xrApplyHapticFeedback>("xrApplyHapticFeedback");
	m_fromQpc = Fn<PFN_xrConvertWin32PerformanceCounterToTimeKHR>("xrConvertWin32PerformanceCounterToTimeKHR");
	auto getSystem = Fn<PFN_xrGetSystem>("xrGetSystem");
	auto createSession = Fn<PFN_xrCreateSession>("xrCreateSession");
	auto beginSession = Fn<PFN_xrBeginSession>("xrBeginSession");
	auto stringToPath = Fn<PFN_xrStringToPath>("xrStringToPath");
	auto createSet = Fn<PFN_xrCreateActionSet>("xrCreateActionSet");
	auto createAction = Fn<PFN_xrCreateAction>("xrCreateAction");
	auto suggest = Fn<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");
	auto attach = Fn<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets");
	auto createActionSpace = Fn<PFN_xrCreateActionSpace>("xrCreateActionSpace");
	auto createRefSpace = Fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace");
	XrResult r;

	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
	XrSystemId sys = XR_NULL_SYSTEM_ID;
	if (XR_FAILED(r = getSystem(m_inst, &sgi, &sys))) return fail("finding the Rift", r);

	stringToPath(m_inst, "/user/hand/left", &m_hand[0]);
	stringToPath(m_inst, "/user/hand/right", &m_hand[1]);
	XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(asci.actionSetName, "touchlink");
	strcpy_s(asci.localizedActionSetName, "TouchLink");
	if (XR_FAILED(r = createSet(m_inst, &asci, &m_set))) return fail("action set", r);

	std::vector<XrActionSuggestedBinding> binds;
	for (int a = 0; a < A_COUNT; a++) {
		XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
		strcpy_s(aci.actionName, kActs[a].name);
		strcpy_s(aci.localizedActionName, kActs[a].name);
		aci.actionType = kActs[a].type;
		aci.countSubactionPaths = kActs[a].right ? 2 : 1;
		aci.subactionPaths = m_hand;
		if (XR_FAILED(r = createAction(m_set, &aci, &m_act[a]))) return fail("action", r);
		for (int h = 0; h < 2; h++) {
			const char* sub = h ? kActs[a].right : kActs[a].left;
			if (!sub) continue;
			XrPath p;
			std::string full = std::string(h ? "/user/hand/right/" : "/user/hand/left/") + sub;
			stringToPath(m_inst, full.c_str(), &p);
			binds.push_back({ m_act[a], p });
		}
	}
	XrInteractionProfileSuggestedBinding ipsb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	stringToPath(m_inst, "/interaction_profiles/oculus/touch_controller", &ipsb.interactionProfile);
	ipsb.countSuggestedBindings = (uint32_t)binds.size();
	ipsb.suggestedBindings = binds.data();
	if (XR_FAILED(r = suggest(m_inst, &ipsb))) return fail("bindings", r);

	XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO, nullptr, 0, sys };   // headless: no graphics
	if (XR_FAILED(r = createSession(m_inst, &sci, &m_sess))) return fail("headless session", r);
	XrSessionActionSetsAttachInfo sai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &m_set };
	if (XR_FAILED(r = attach(m_sess, &sai))) return fail("attach", r);

	// The Touch tracking space: Monado's stage (or local) space. Calibration maps it to SteamVR's.
	XrReferenceSpaceCreateInfo rci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_STAGE, kIdentity };
	if (XR_FAILED(createRefSpace(m_sess, &rci, &m_stage))) {
		rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		if (XR_FAILED(r = createRefSpace(m_sess, &rci, &m_stage))) return fail("reference space", r);
	}
	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo s = { XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, m_act[A_GRIP], m_hand[h], kIdentity };
		if (XR_FAILED(r = createActionSpace(m_sess, &s, &m_grip[h]))) return fail("grip space", r);
	}

	// READY first, then begin; a headless session is focused right after.
	for (int i = 0; i < 100; i++) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		if (m_pollEvent(m_inst, &ev) != XR_SUCCESS) {
			Sleep(10);
			continue;
		}
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
			((XrEventDataSessionStateChanged*)&ev)->state == XR_SESSION_STATE_READY)
			break;
	}
	XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
	if (XR_FAILED(r = beginSession(m_sess, &sbi))) return fail("begin", r);

	m_ready = true;
	return true;
}

// ---- the Meta source --------------------------------------------------------------------------
bool Source::ConnectMeta(std::string* why)
{
	std::lock_guard<std::mutex> lk(m_lock);
	if (!m_metaMap) m_metaMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMetaMapping);
	if (!m_metaMap) {
		*why = "waiting for touchlink meta (Meta's runtime)";
		return false;
	}
	if (!m_meta) m_meta = (MetaShared*)MapViewOfFile(m_metaMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MetaShared));
	if (!m_meta || m_meta->magic != kMetaMagic || m_meta->version != kMetaVersion) {
		*why = "touchlink meta isn't ready (or is another version)";
		return false;
	}
	MetaShared now;
	if (!MetaRead(&now)) {
		*why = "touchlink meta isn't writing";
		return false;
	}
	LARGE_INTEGER q, f;
	QueryPerformanceCounter(&q);
	QueryPerformanceFrequency(&f);
	if ((q.QuadPart - now.qpc) > f.QuadPart) {
		*why = now.status[0] ? std::string(now.status) : "touchlink meta: no Touches from Meta's runtime";
		return false;
	}
	m_ready = true;
	return true;
}

// A consistent copy: between two equal even sequence numbers.
bool Source::MetaRead(MetaShared* out)
{
	if (!m_meta) return false;
	for (int i = 0; i < 8; i++) {
		int64_t a = m_meta->sequence;
		if (a & 1) {
			Sleep(0);
			continue;
		}
		MemoryBarrier();
		memcpy(out, (const void*)m_meta, sizeof *out);
		MemoryBarrier();
		if (m_meta->sequence == a) return true;
	}
	return false;
}

void Source::Disconnect()
{
	std::lock_guard<std::mutex> lk(m_lock);
	if (m_meta) {
		UnmapViewOfFile(m_meta);
		m_meta = nullptr;
	}
	if (m_metaMap) {
		CloseHandle(m_metaMap);
		m_metaMap = nullptr;
	}
	if (m_inst && m_destroyInstance) m_destroyInstance(m_inst);   // takes the session and spaces with it
	m_inst = XR_NULL_HANDLE;
	m_sess = XR_NULL_HANDLE;
	m_ready = false;
}

bool Source::Sync(TouchInput out[2])
{
	std::lock_guard<std::mutex> lk(m_lock);
	if (!m_ready) return false;
	if (m_meta) {
		MetaShared m;
		if (!MetaRead(&m)) return true;   // mid-write: the last input stands
		LARGE_INTEGER q, f;
		QueryPerformanceCounter(&q);
		QueryPerformanceFrequency(&f);
		if (q.QuadPart - m.qpc > f.QuadPart) {   // the helper stopped
			m_ready = false;
			return false;
		}
		for (int h = 0; h < 2; h++) {
			const MetaHand& mh = m.hands[h];
			TouchInput& s = out[h];
			s.active = mh.connected != 0;
			s.trigger = mh.trigger;
			s.squeeze = mh.grip;
			s.stick = { mh.stick[0], mh.stick[1] };
			s.triggerTouch = (mh.touches & kMetaTrigger) != 0;
			s.stickClick = (mh.buttons & kMetaStick) != 0;
			s.stickTouch = (mh.touches & kMetaStick) != 0;
			s.thumbrest = (mh.touches & kMetaThumbrest) != 0;
			s.lower = (mh.buttons & kMetaLower) != 0;
			s.lowerTouch = (mh.touches & kMetaLower) != 0;
			s.upper = (mh.buttons & kMetaUpper) != 0;
			s.upperTouch = (mh.touches & kMetaUpper) != 0;
			// Left: the menu button; right: the Oculus button (the driver decides what each does).
			s.menu = (mh.buttons & (kMetaMenu | kMetaOculus)) != 0;
		}
		return true;
	}
	XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
	while (m_pollEvent(m_inst, &ev) == XR_SUCCESS) {
		if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING ||
			(ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
				((XrEventDataSessionStateChanged*)&ev)->state >= XR_SESSION_STATE_STOPPING)) {
			m_ready = false;   // the service went away; the driver reconnects
			return false;
		}
		ev = { XR_TYPE_EVENT_DATA_BUFFER };
	}
	XrActiveActionSet active = { m_set, XR_NULL_PATH };
	XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active };
	XrResult sr = m_sync(m_sess, &si);
	if (sr == XR_ERROR_INSTANCE_LOST || sr == XR_ERROR_SESSION_LOST || sr == XR_ERROR_RUNTIME_FAILURE) {
		m_ready = false;
		return false;
	}
	if (XR_FAILED(sr)) return false;

	for (int h = 0; h < 2; h++) {
		TouchInput& s = out[h];
		auto b = [&](int a) {
			XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_act[a], m_hand[h] };
			XrActionStateBoolean v = { XR_TYPE_ACTION_STATE_BOOLEAN };
			m_getBool(m_sess, &gi, &v);
			return v.currentState == XR_TRUE;
		};
		auto f = [&](int a) {
			XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_act[a], m_hand[h] };
			XrActionStateFloat v = { XR_TYPE_ACTION_STATE_FLOAT };
			m_getFloat(m_sess, &gi, &v);
			return v.currentState;
		};
		XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_act[A_GRIP], m_hand[h] };
		XrActionStatePose pose = { XR_TYPE_ACTION_STATE_POSE };
		m_getPose(m_sess, &gi, &pose);
		s.active = pose.isActive == XR_TRUE;
		s.trigger = f(A_TRIGGER);
		s.squeeze = f(A_SQUEEZE);
		gi.action = m_act[A_STICK];
		XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
		m_getVec2(m_sess, &gi, &v);
		s.stick = v.currentState;
		s.triggerTouch = b(A_TRIGGER_TOUCH);
		s.stickClick = b(A_STICK_CLICK);
		s.stickTouch = b(A_STICK_TOUCH);
		s.thumbrest = b(A_THUMBREST);
		s.lower = b(A_LOWER);
		s.lowerTouch = b(A_LOWER_TOUCH);
		s.upper = b(A_UPPER);
		s.upperTouch = b(A_UPPER_TOUCH);
		s.menu = h == 0 && b(A_MENU);
	}
	return true;
}

bool Source::Locate(int hand, LONGLONG qpc, TouchPose* out)
{
	std::lock_guard<std::mutex> lk(m_lock);
	*out = TouchPose();
	if (m_meta) {
		// The latest pose Meta's runtime gave, moved by its velocities to the time asked for.
		MetaShared m;
		if (!m_ready || !MetaRead(&m)) return false;
		const MetaHand& mh = m.hands[hand];
		if (!mh.valid) return false;
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		float dt = (float)(qpc - m.qpc) / (float)f.QuadPart;
		dt = std::min(0.1f, std::max(-0.1f, dt));
		XrVector3f w = { mh.angularVelocity[0], mh.angularVelocity[1], mh.angularVelocity[2] };
		XrQuaternionf q = { mh.orientation[0], mh.orientation[1], mh.orientation[2], mh.orientation[3] };
		float a = sqrtf(w.x * w.x + w.y * w.y + w.z * w.z) * dt;
		if (fabsf(a) > 1e-6f) {
			float s = sinf(a / 2) / (a / dt);
			q = QMul({ w.x * s, w.y * s, w.z * s, cosf(a / 2) }, q);
		}
		// Meta's pose is LibOVR's Touch origin; the driver takes the OpenXR grip, as Atrium's service
		// gives it (kGripToOvrHand). Passing Meta's on as the grip tilted every Touch (a player on an
		// Index, 2026-11-01); taking it for SteamVR's raw pose put the pivot 10 cm off, so a Touch
		// swung away when turned. The velocity is the grip's too.
		XrPosef ovr = { q, { mh.position[0] + mh.linearVelocity[0] * dt, mh.position[1] + mh.linearVelocity[1] * dt,
			mh.position[2] + mh.linearVelocity[2] * dt } };
		out->valid = true;
		out->positionTracked = mh.positionTracked != 0;
		out->grip = PMul(ovr, PInv(kGripToOvrHand));
		out->linVel = VAdd({ mh.linearVelocity[0], mh.linearVelocity[1], mh.linearVelocity[2] },
			VCross(w, VSub(out->grip.position, ovr.position)));
		out->angVel = w;
		return true;
	}
	if (!m_ready || !m_fromQpc) return false;
	LARGE_INTEGER li;
	li.QuadPart = qpc;
	XrTime t = 0;
	if (XR_FAILED(m_fromQpc(m_inst, &li, &t))) return false;
	XrSpaceVelocity vel = { XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION, &vel };
	if (XR_FAILED(m_locate(m_grip[hand], m_stage, t, &loc))) return false;
	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	out->valid = (loc.locationFlags & valid) == valid;
	out->positionTracked = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
	out->grip = loc.pose;
	if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) out->linVel = vel.linearVelocity;
	if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) out->angVel = vel.angularVelocity;
	return out->valid;
}

void Source::Haptic(int hand, float seconds, float frequency, float amplitude)
{
	std::lock_guard<std::mutex> lk(m_lock);
	if (!m_ready) return;
	if (m_meta) {
		m_meta->haptic[hand].amplitude = amplitude;
		m_meta->haptic[hand].seconds = seconds;
		MemoryBarrier();
		m_meta->haptic[hand].sequence++;
		return;
	}
	XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO, nullptr, m_act[A_HAPTIC], m_hand[hand] };
	XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION, nullptr, seconds > 0 ? (XrDuration)(seconds * 1e9) : XR_MIN_HAPTIC_DURATION,
		frequency > 0 ? frequency : XR_FREQUENCY_UNSPECIFIED, amplitude };
	m_haptic(m_sess, &hi, (const XrHapticBaseHeader*)&v);
}

} // namespace tl
