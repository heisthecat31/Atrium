// CV1 Touch swap.
//
// The backend (Virtual Desktop, ...) keeps the headset and the picture; the game's controllers
// are Rift CV1 Touches. Atrium's own runtime service (Monado with the rift driver, no compositor:
// XRT_COMPOSITOR_NULL=1) tracks them with the Rift sensors. This file:
//   - opens a headless OpenXR session on that service, in the game's process (the "source"),
//     with one action per Touch input, and reads the Touches through it;
//   - records the game's actions and bindings, and answers the game's action states, hand
//     poses, interaction profile and haptics from the Touches instead of the backend;
//   - places the Touches in the backend's space with a calibration: Touch tracking space ->
//     the backend's STAGE (or LOCAL) space, saved in %APPDATA%\Atrium\touch_calibration.json.
//
// Calibrating: hold the right Touch and the right Quest controller together in one hand, then
// click both Touch thumbsticks for 2 s (or create %APPDATA%\Atrium\touch_calibrate). The right
// Touch buzzes; wave and turn your hand for ~8 s until it buzzes again. Until a calibration
// exists the game keeps the backend's own controllers.
#include "touch.h"

#include <shlobj.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <openxr/openxr_loader_negotiation.h>

namespace touch {
namespace {

void (*g_log)(const char*, ...) = [](const char*, ...) {};
std::wstring g_manifest;

// ---- math -------------------------------------------------------------------------------------
XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
XrQuaternionf QConj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }
XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v)
{
	XrVector3f t = { 2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x) };
	return { v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
		v.z + q.w * t.z + (q.x * t.y - q.y * t.x) };
}
XrPosef PMul(const XrPosef& a, const XrPosef& b)
{
	XrVector3f p = QRot(a.orientation, b.position);
	return { QMul(a.orientation, b.orientation), { a.position.x + p.x, a.position.y + p.y, a.position.z + p.z } };
}
XrPosef PInv(const XrPosef& a)
{
	XrQuaternionf c = QConj(a.orientation);
	XrVector3f p = QRot(c, a.position);
	return { c, { -p.x, -p.y, -p.z } };
}
const XrPosef kIdentity = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
const XrSpaceLocationFlags kPoseValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

// ---- the source: a headless session on Atrium's service ---------------------------------------
enum Act {
	A_GRIP, A_AIM, A_TRIGGER, A_TRIGGER_TOUCH, A_SQUEEZE, A_STICK, A_STICK_CLICK, A_STICK_TOUCH, A_THUMBREST,
	A_LOWER, A_LOWER_TOUCH, A_UPPER, A_UPPER_TOUCH, A_MENU, A_HAPTIC, A_COUNT
};
struct ActDef { const char* name; XrActionType type; const char* left; const char* right; };
const ActDef kActs[A_COUNT] = {
	{ "grip", XR_ACTION_TYPE_POSE_INPUT, "input/grip/pose", "input/grip/pose" },
	{ "aim", XR_ACTION_TYPE_POSE_INPUT, "input/aim/pose", "input/aim/pose" },
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

// One Touch, as read at the last sync.
struct HandState {
	bool active = false;
	float value[A_COUNT] = {};   // booleans as 0/1
	XrVector2f stick = {};
};

struct Source {
	HMODULE dll = nullptr;
	PFN_xrGetInstanceProcAddr gipa = nullptr;
	XrInstance inst = XR_NULL_HANDLE;
	XrSession sess = XR_NULL_HANDLE;
	XrSpace stage = XR_NULL_HANDLE;
	XrActionSet set = XR_NULL_HANDLE;
	XrAction act[A_COUNT] = {};
	XrPath hand[2] = {};
	XrSpace grip[2] = {}, aim[2] = {};
	bool ready = false;
	ULONGLONG lastTry = 0;

	PFN_xrPollEvent pollEvent = nullptr;
	PFN_xrSyncActions sync = nullptr;
	PFN_xrGetActionStateBoolean getBool = nullptr;
	PFN_xrGetActionStateFloat getFloat = nullptr;
	PFN_xrGetActionStateVector2f getVec2 = nullptr;
	PFN_xrGetActionStatePose getPose = nullptr;
	PFN_xrLocateSpace locate = nullptr;
	PFN_xrApplyHapticFeedback haptic = nullptr;
	PFN_xrConvertWin32PerformanceCounterToTimeKHR fromQpc = nullptr;

	HandState state[2];
};
std::mutex g_srcLock;   // guards g_src
Source g_src;

template <typename F> F SrcFn(const char* name)
{
	PFN_xrVoidFunction f = nullptr;
	g_src.gipa(g_src.inst, name, &f);
	return (F)f;
}

#define SRC_CHECK(call, what)                                         \
	do {                                                              \
		XrResult r_ = (call);                                         \
		if (XR_FAILED(r_)) {                                          \
			g_log("touch: Atrium service %s failed (%d)", what, r_); \
			return false;                                             \
		}                                                             \
	} while (0)

// Connects to the Atrium service. g_srcLock held.
bool SourceStart()
{
	if (!g_src.dll) {
		std::wstring manifest = g_manifest;
		FILE* f = nullptr;
		if (_wfopen_s(&f, manifest.c_str(), L"rb") || !f) {
			g_log("touch: can't read %ls", manifest.c_str());
			return false;
		}
		std::string text;
		char buf[2048];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
		fclose(f);
		size_t at = text.find("\"library_path\"");
		at = at == std::string::npos ? at : text.find('"', text.find(':', at) + 1);
		size_t end = at == std::string::npos ? at : text.find('"', at + 1);
		if (end == std::string::npos) {
			g_log("touch: no library_path in %ls", manifest.c_str());
			return false;
		}
		std::string lib = text.substr(at + 1, end - at - 1);
		for (size_t i = 0; (i = lib.find("\\\\", i)) != std::string::npos; i++) lib.erase(i, 1);
		std::wstring wlib(lib.begin(), lib.end());
		if (wlib.size() < 2 || (wlib[1] != L':' && wlib[0] != L'\\')) {
			std::wstring dir = manifest.substr(0, manifest.find_last_of(L"\\/") + 1);
			wlib = dir + wlib;
		}
		g_src.dll = LoadLibraryExW(wlib.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!g_src.dll) {
			g_log("touch: %ls didn't load (error %lu)", wlib.c_str(), GetLastError());
			return false;
		}
		auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(g_src.dll, "xrNegotiateLoaderRuntimeInterface");
		XrNegotiateLoaderInfo li = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof li, 1,
			XR_CURRENT_LOADER_RUNTIME_VERSION, XR_MAKE_VERSION(1, 0, 0), XR_CURRENT_API_VERSION };
		XrNegotiateRuntimeRequest rq = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof rq };
		if (!negotiate || XR_FAILED(negotiate(&li, &rq)) || !rq.getInstanceProcAddr) {
			g_log("touch: negotiating with %ls failed", wlib.c_str());
			return false;
		}
		g_src.gipa = rq.getInstanceProcAddr;
	}

	PFN_xrCreateInstance createInstance = nullptr;
	g_src.gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&createInstance);
	const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME };
	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy_s(ici.applicationInfo.applicationName, "Atrium Touch");
	strcpy_s(ici.applicationInfo.engineName, "Atrium");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = exts;
	SRC_CHECK(createInstance(&ici, &g_src.inst), "instance (is atrium-service running?)");

	g_src.pollEvent = SrcFn<PFN_xrPollEvent>("xrPollEvent");
	g_src.sync = SrcFn<PFN_xrSyncActions>("xrSyncActions");
	g_src.getBool = SrcFn<PFN_xrGetActionStateBoolean>("xrGetActionStateBoolean");
	g_src.getFloat = SrcFn<PFN_xrGetActionStateFloat>("xrGetActionStateFloat");
	g_src.getVec2 = SrcFn<PFN_xrGetActionStateVector2f>("xrGetActionStateVector2f");
	g_src.getPose = SrcFn<PFN_xrGetActionStatePose>("xrGetActionStatePose");
	g_src.locate = SrcFn<PFN_xrLocateSpace>("xrLocateSpace");
	g_src.haptic = SrcFn<PFN_xrApplyHapticFeedback>("xrApplyHapticFeedback");
	g_src.fromQpc = SrcFn<PFN_xrConvertWin32PerformanceCounterToTimeKHR>("xrConvertWin32PerformanceCounterToTimeKHR");
	auto getSystem = SrcFn<PFN_xrGetSystem>("xrGetSystem");
	auto createSession = SrcFn<PFN_xrCreateSession>("xrCreateSession");
	auto beginSession = SrcFn<PFN_xrBeginSession>("xrBeginSession");
	auto stringToPath = SrcFn<PFN_xrStringToPath>("xrStringToPath");
	auto createSet = SrcFn<PFN_xrCreateActionSet>("xrCreateActionSet");
	auto createAction = SrcFn<PFN_xrCreateAction>("xrCreateAction");
	auto suggest = SrcFn<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");
	auto attach = SrcFn<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets");
	auto createActionSpace = SrcFn<PFN_xrCreateActionSpace>("xrCreateActionSpace");
	auto createRefSpace = SrcFn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace");

	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
	XrSystemId sys = XR_NULL_SYSTEM_ID;
	SRC_CHECK(getSystem(g_src.inst, &sgi, &sys), "system");

	stringToPath(g_src.inst, "/user/hand/left", &g_src.hand[0]);
	stringToPath(g_src.inst, "/user/hand/right", &g_src.hand[1]);
	XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(asci.actionSetName, "atrium_touch");
	strcpy_s(asci.localizedActionSetName, "Atrium Touch");
	SRC_CHECK(createSet(g_src.inst, &asci, &g_src.set), "action set");

	std::vector<XrActionSuggestedBinding> binds;
	for (int a = 0; a < A_COUNT; a++) {
		XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
		strcpy_s(aci.actionName, kActs[a].name);
		strcpy_s(aci.localizedActionName, kActs[a].name);
		aci.actionType = kActs[a].type;
		aci.countSubactionPaths = kActs[a].right ? 2 : 1;
		aci.subactionPaths = g_src.hand;
		SRC_CHECK(createAction(g_src.set, &aci, &g_src.act[a]), "action");
		for (int h = 0; h < 2; h++) {
			const char* sub = h ? kActs[a].right : kActs[a].left;
			if (!sub) continue;
			XrPath p;
			std::string full = std::string(h ? "/user/hand/right/" : "/user/hand/left/") + sub;
			stringToPath(g_src.inst, full.c_str(), &p);
			binds.push_back({ g_src.act[a], p });
		}
	}
	XrInteractionProfileSuggestedBinding ipsb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	stringToPath(g_src.inst, "/interaction_profiles/oculus/touch_controller", &ipsb.interactionProfile);
	ipsb.countSuggestedBindings = (uint32_t)binds.size();
	ipsb.suggestedBindings = binds.data();
	SRC_CHECK(suggest(g_src.inst, &ipsb), "bindings");

	XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO, nullptr, 0, sys };   // headless: no graphics binding
	SRC_CHECK(createSession(g_src.inst, &sci, &g_src.sess), "headless session");
	XrSessionActionSetsAttachInfo sai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &g_src.set };
	SRC_CHECK(attach(g_src.sess, &sai), "attach");

	XrReferenceSpaceCreateInfo rci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_STAGE, kIdentity };
	if (XR_FAILED(createRefSpace(g_src.sess, &rci, &g_src.stage))) {
		rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		SRC_CHECK(createRefSpace(g_src.sess, &rci, &g_src.stage), "reference space");
	}
	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo s = { XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, g_src.act[A_GRIP], g_src.hand[h], kIdentity };
		SRC_CHECK(createActionSpace(g_src.sess, &s, &g_src.grip[h]), "grip space");
		s.action = g_src.act[A_AIM];
		SRC_CHECK(createActionSpace(g_src.sess, &s, &g_src.aim[h]), "aim space");
	}

	// The session must be READY before it can begin; a headless one is focused right after.
	for (int i = 0; i < 100; i++) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		if (g_src.pollEvent(g_src.inst, &ev) != XR_SUCCESS) {
			Sleep(10);
			continue;
		}
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
			((XrEventDataSessionStateChanged*)&ev)->state == XR_SESSION_STATE_READY)
			break;
	}
	XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
	SRC_CHECK(beginSession(g_src.sess, &sbi), "begin");

	g_src.ready = true;
	g_log("touch: connected to the Atrium service (%s space)",
		rci.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage" : "local");
	return true;
}

// Reads the Touches. g_srcLock held.
void SourceSync()
{
	if (!g_src.ready) {
		ULONGLONG now = GetTickCount64();
		if (now - g_src.lastTry < 5000) return;
		g_src.lastTry = now;
		if (!SourceStart()) return;
	}
	XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
	while (g_src.pollEvent(g_src.inst, &ev) == XR_SUCCESS) ev = { XR_TYPE_EVENT_DATA_BUFFER };

	XrActiveActionSet active = { g_src.set, XR_NULL_PATH };
	XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active };
	if (XR_FAILED(g_src.sync(g_src.sess, &si))) return;

	for (int h = 0; h < 2; h++) {
		HandState& s = g_src.state[h];
		XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO, nullptr, g_src.act[A_GRIP], g_src.hand[h] };
		XrActionStatePose pose = { XR_TYPE_ACTION_STATE_POSE };
		g_src.getPose(g_src.sess, &gi, &pose);
		s.active = pose.isActive;
		for (int a = 0; a < A_COUNT; a++) {
			if (h == 1 && !kActs[a].right) continue;
			gi.action = g_src.act[a];
			switch (kActs[a].type) {
			case XR_ACTION_TYPE_BOOLEAN_INPUT: {
				XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
				g_src.getBool(g_src.sess, &gi, &b);
				s.value[a] = b.currentState ? 1.f : 0.f;
				break;
			}
			case XR_ACTION_TYPE_FLOAT_INPUT: {
				XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT };
				g_src.getFloat(g_src.sess, &gi, &f);
				s.value[a] = f.currentState;
				break;
			}
			case XR_ACTION_TYPE_VECTOR2F_INPUT: {
				XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
				g_src.getVec2(g_src.sess, &gi, &v);
				s.stick = v.currentState;
				break;
			}
			default: break;
			}
		}
	}
}

// A Touch pose (grip or aim) in the Touch tracking space at a QPC time. g_srcLock held.
bool SourceLocate(int hand, bool aim, LONGLONG qpc, XrPosef* pose, XrSpaceLocationFlags* flags)
{
	if (!g_src.ready || !g_src.fromQpc) return false;
	LARGE_INTEGER li;
	li.QuadPart = qpc;
	XrTime t = 0;
	if (XR_FAILED(g_src.fromQpc(g_src.inst, &li, &t))) return false;
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(g_src.locate(aim ? g_src.aim[hand] : g_src.grip[hand], g_src.stage, t, &loc))) return false;
	*pose = loc.pose;
	*flags = loc.locationFlags;
	return (loc.locationFlags & kPoseValid) == kPoseValid;
}

void SourceHaptic(int hand, const XrHapticVibration& v)
{
	std::lock_guard<std::mutex> lk(g_srcLock);
	if (!g_src.ready) return;
	XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO, nullptr, g_src.act[A_HAPTIC], g_src.hand[hand] };
	g_src.haptic(g_src.sess, &hi, (const XrHapticBaseHeader*)&v);
}

void Buzz(int hand, float seconds)
{
	XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION, nullptr, (XrDuration)(seconds * 1e9), XR_FREQUENCY_UNSPECIFIED, 1.f };
	SourceHaptic(hand, v);
}

// ---- the game's side ----------------------------------------------------------------------------
// What a game binding reads from a Touch.
enum Comp {
	C_NONE, C_TRIGGER, C_TRIGGER_TOUCH, C_SQUEEZE, C_STICK, C_STICK_X, C_STICK_Y, C_STICK_CLICK, C_STICK_TOUCH,
	C_THUMBREST, C_LOWER, C_LOWER_TOUCH, C_UPPER, C_UPPER_TOUCH, C_MENU, C_GRIP, C_AIM, C_HAPTIC
};
struct Bind { int hand; Comp comp; };

Comp ParseComp(const std::string& s)
{
	static const struct { const char* path; Comp comp; } map[] = {
		{ "input/trigger/value", C_TRIGGER }, { "input/trigger/click", C_TRIGGER }, { "input/trigger", C_TRIGGER },
		{ "input/select/click", C_TRIGGER }, { "input/select", C_TRIGGER }, { "input/trigger/touch", C_TRIGGER_TOUCH },
		{ "input/squeeze/value", C_SQUEEZE }, { "input/squeeze/click", C_SQUEEZE }, { "input/squeeze/force", C_SQUEEZE },
		{ "input/squeeze", C_SQUEEZE }, { "input/thumbstick", C_STICK }, { "input/thumbstick/x", C_STICK_X },
		{ "input/thumbstick/y", C_STICK_Y }, { "input/thumbstick/click", C_STICK_CLICK },
		{ "input/thumbstick/touch", C_STICK_TOUCH }, { "input/thumbrest/touch", C_THUMBREST },
		{ "input/x/click", C_LOWER }, { "input/a/click", C_LOWER }, { "input/x/touch", C_LOWER_TOUCH },
		{ "input/a/touch", C_LOWER_TOUCH }, { "input/y/click", C_UPPER }, { "input/b/click", C_UPPER },
		{ "input/y/touch", C_UPPER_TOUCH }, { "input/b/touch", C_UPPER_TOUCH }, { "input/menu/click", C_MENU },
		{ "input/menu", C_MENU }, { "input/grip/pose", C_GRIP }, { "input/grip_surface/pose", C_GRIP },
		{ "input/palm_ext/pose", C_GRIP }, { "input/aim/pose", C_AIM }, { "output/haptic", C_HAPTIC },
	};
	for (const auto& m : map)
		if (s == m.path) return m.comp;
	return C_NONE;
}

// A component's value from a Touch: analog (0..1 or -1..1) or 0/1.
float CompValue(const HandState& s, Comp c)
{
	switch (c) {
	case C_TRIGGER: return s.value[A_TRIGGER];
	case C_TRIGGER_TOUCH: return s.value[A_TRIGGER_TOUCH];
	case C_SQUEEZE: return s.value[A_SQUEEZE];
	case C_STICK_X: return s.stick.x;
	case C_STICK_Y: return s.stick.y;
	case C_STICK_CLICK: return s.value[A_STICK_CLICK];
	case C_STICK_TOUCH: return s.value[A_STICK_TOUCH];
	case C_THUMBREST: return s.value[A_THUMBREST];
	case C_LOWER: return s.value[A_LOWER];
	case C_LOWER_TOUCH: return s.value[A_LOWER_TOUCH];
	case C_UPPER: return s.value[A_UPPER];
	case C_UPPER_TOUCH: return s.value[A_UPPER_TOUCH];
	case C_MENU: return s.value[A_MENU];
	default: return 0.f;
	}
}

struct Action { XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT; std::string name; std::vector<Bind> binds; };
// The value the game reads for one action and subaction filter (0 any, 1 left, 2 right).
struct Value { bool active = false; float f = 0; XrVector2f v = {}; bool changed = false; XrTime lastChange = 0; };
struct ActionSpace { XrAction action; XrPath sub; XrPosef offset; };

struct Backend {
	PFN_xrStringToPath stringToPath;
	PFN_xrPathToString pathToString;
	PFN_xrCreateActionSet createActionSet;
	PFN_xrCreateAction createAction;
	PFN_xrSuggestInteractionProfileBindings suggest;
	PFN_xrAttachSessionActionSets attach;
	PFN_xrCreateActionSpace createActionSpace;
	PFN_xrCreateReferenceSpace createReferenceSpace;
	PFN_xrDestroySpace destroySpace;
	PFN_xrSyncActions sync;
	PFN_xrGetActionStateBoolean getBool;
	PFN_xrGetActionStateFloat getFloat;
	PFN_xrGetActionStateVector2f getVec2;
	PFN_xrGetActionStatePose getPose;
	PFN_xrLocateSpace locate;
	PFN_xrLocateSpaces locateSpaces;
	PFN_xrLocateSpaces locateSpacesKHR;
	PFN_xrGetCurrentInteractionProfile currentProfile;
	PFN_xrApplyHapticFeedback applyHaptic;
	PFN_xrStopHapticFeedback stopHaptic;
	PFN_xrPollEvent pollEvent;
	PFN_xrDestroySession destroySession;
	PFN_xrConvertTimeToWin32PerformanceCounterKHR toQpc;
	PFN_xrConvertWin32PerformanceCounterToTimeKHR fromQpc;
};
Backend g_b;
XrInstance g_inst = XR_NULL_HANDLE;
XrTime (*g_fixTime)(XrTime) = [](XrTime t) { return t; };

std::mutex g_lock;   // guards everything below
std::unordered_map<uint64_t, Action> g_actions;
std::unordered_map<std::string, std::vector<std::pair<XrAction, std::string>>> g_suggested;   // profile -> bindings
std::string g_profile;   // the profile the game sees for the Touches
XrPath g_profilePath = XR_NULL_PATH;
std::unordered_map<uint64_t, ActionSpace> g_actionSpaces;
std::unordered_map<uint64_t, Value> g_values;   // action * 3 + subaction filter
XrPath g_handPath[2] = {};
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_ref = XR_NULL_HANDLE;   // the backend space the calibration is in
bool g_refStage = false;
bool g_profileEvent = false;

// Calibration: Touch tracking space -> g_ref.
bool g_calibrated = false;
XrPosef g_calib = kIdentity;
// Atrium's own action set on the game's instance: the backend's controller grip poses, only
// read while calibrating.
XrActionSet g_calSet = XR_NULL_HANDLE;
XrAction g_calGrip = XR_NULL_HANDLE;
XrSpace g_calSpace[2] = {};

std::wstring AtriumFile(const wchar_t* name)
{
	PWSTR roaming = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return {};
	std::wstring p = std::wstring(roaming) + L"\\Atrium\\" + name;
	CoTaskMemFree(roaming);
	return p;
}

// Whether the game gets the Touches; otherwise it keeps the backend's controllers.
bool Active() { return g_calibrated && g_src.ready && g_session != XR_NULL_HANDLE && g_ref != XR_NULL_HANDLE; }

std::string PathString(XrPath p)
{
	char buf[XR_MAX_PATH_LENGTH];
	uint32_t n = 0;
	if (XR_FAILED(g_b.pathToString(g_inst, p, sizeof buf, &n, buf))) return {};
	return buf;
}

bool QpcOf(XrTime t, LONGLONG* qpc)
{
	LARGE_INTEGER li;
	if (!g_b.toQpc || XR_FAILED(g_b.toQpc(g_inst, t, &li))) return false;
	*qpc = li.QuadPart;
	return true;
}

XrTime Now()
{
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	XrTime t = 0;
	if (g_b.fromQpc) g_b.fromQpc(g_inst, &li, &t);
	return t;
}

// ---- calibration --------------------------------------------------------------------------------
void LoadCalibration()
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, AtriumFile(L"touch_calibration.json").c_str(), L"rb") || !f) return;
	char text[1024] = {};
	fread(text, 1, sizeof text - 1, f);
	fclose(f);
	const char* p = strstr(text, "\"pose\"");
	p = p ? strchr(p, '[') : nullptr;
	XrPosef c;
	if (p && sscanf_s(p, "[%f,%f,%f,%f,%f,%f,%f]", &c.position.x, &c.position.y, &c.position.z, &c.orientation.x,
				 &c.orientation.y, &c.orientation.z, &c.orientation.w) == 7) {
		g_calib = c;
		g_refStage = strstr(text, "\"stage\"") != nullptr;
		g_calibrated = true;
		g_log("touch: calibration loaded (%s space)", g_refStage ? "stage" : "local");
	}
}

void SaveCalibration(double errorMm, size_t samples)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, AtriumFile(L"touch_calibration.json").c_str(), L"wb") || !f) return;
	const XrPosef& c = g_calib;
	fprintf(f,
		"{\n  \"reference\": \"%s\",\n  \"pose\": [%.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f],\n  \"error_mm\": %.1f,\n  \"samples\": %zu\n}\n",
		g_refStage ? "stage" : "local", c.position.x, c.position.y, c.position.z, c.orientation.x, c.orientation.y,
		c.orientation.z, c.orientation.w, errorMm, samples);
	fclose(f);
}

struct Sample { XrPosef quest, touch; };
enum class Cal { Idle, Collecting } g_cal = Cal::Idle;
std::vector<Sample> g_samples;
ULONGLONG g_calStart = 0, g_comboSince = 0, g_lastSample = 0;
const ULONGLONG kCalMs = 8000;

// Largest-eigenvalue eigenvector of a symmetric 4x4 (Jacobi).
void Eigen4(double a[4][4], double out[4])
{
	double v[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
	for (int sweep = 0; sweep < 50; sweep++) {
		double off = 0;
		for (int p = 0; p < 4; p++)
			for (int q = p + 1; q < 4; q++) off += a[p][q] * a[p][q];
		if (off < 1e-18) break;
		for (int p = 0; p < 4; p++)
			for (int q = p + 1; q < 4; q++) {
				if (fabs(a[p][q]) < 1e-30) continue;
				double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
				double t = (theta >= 0 ? 1 : -1) / (fabs(theta) + sqrt(theta * theta + 1));
				double c = 1 / sqrt(t * t + 1), s = t * c;
				for (int k = 0; k < 4; k++) {
					double akp = a[k][p], akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < 4; k++) {
					double apk = a[p][k], aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (int k = 0; k < 4; k++) {
					double vkp = v[k][p], vkq = v[k][q];
					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
	}
	int best = 0;
	for (int i = 1; i < 4; i++)
		if (a[i][i] > a[best][best]) best = i;
	for (int k = 0; k < 4; k++) out[k] = v[k][best];
}

// Rotation vector (axis * angle) of q.
XrVector3f RotVec(XrQuaternionf q)
{
	if (q.w < 0) q = { -q.x, -q.y, -q.z, -q.w };
	double s = sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
	if (s < 1e-9) return { 0, 0, 0 };
	double k = 2 * atan2(s, q.w) / s;
	return { (float)(q.x * k), (float)(q.y * k), (float)(q.z * k) };
}

// quest_i = Z * touch_i * X, solved for Z (Touch space -> g_ref): rotation from matching the
// world-frame rotation axes of both devices (Horn), then Z's position and X's offset by least
// squares. Returns the mean position error in meters, or < 0 when the motion was too small.
double SolveCalibration(const std::vector<Sample>& s, XrPosef* z)
{
	double S[3][3] = {};
	int pairs = 0, turned = 0;
	double mismatch = 0;
	for (size_t i = 0; i + 8 < s.size(); i++) {
		size_t j = i + 8;
		XrVector3f a = RotVec(QMul(s[j].quest.orientation, QConj(s[i].quest.orientation)));
		XrVector3f b = RotVec(QMul(s[j].touch.orientation, QConj(s[i].touch.orientation)));
		double la = sqrt(a.x * a.x + a.y * a.y + a.z * a.z), lb = sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
		if (la < 0.25) continue;   // too little turn to say anything
		turned++;
		mismatch += fabs(la - lb);
		// The two turned by different amounts: not held together, or one is tracking badly (a hand
		// holding a Touch tracks worse than a controller, so allow a quarter of the turn).
		if (fabs(la - lb) > fmax(0.15, 0.25 * la)) continue;
		double av[3] = { a.x, a.y, a.z }, bv[3] = { b.x, b.y, b.z };
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) S[r][c] += bv[r] * av[c];
		pairs++;
	}
	g_log("touch: calibration: %zu samples, %d turning pairs, %d agree, mean turn mismatch %.0f deg", s.size(), turned, pairs,
		turned ? mismatch / turned * 57.3 : 0.0);
	if (pairs < 15) return -1;
	double N[4][4] = {
		{ S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0] },
		{ S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2] },
		{ S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1] },
		{ S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2] },
	};
	double e[4];
	Eigen4(N, e);
	XrQuaternionf rz = { (float)e[1], (float)e[2], (float)e[3], (float)e[0] };
	double n = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
	rz = { (float)(rz.x / n), (float)(rz.y / n), (float)(rz.z / n), (float)(rz.w / n) };

	// quest_p = Rz * (touch_p + touch_R * d) + t   ->   [I | Rz*touch_R] [t; d] = quest_p - Rz*touch_p
	double A[6][7] = {};
	auto colOf = [](const XrQuaternionf& q, int col) {
		XrVector3f u = { col == 0 ? 1.f : 0.f, col == 1 ? 1.f : 0.f, col == 2 ? 1.f : 0.f };
		return QRot(q, u);
	};
	for (const Sample& x : s) {
		XrQuaternionf c = QMul(rz, x.touch.orientation);
		XrVector3f rp = QRot(rz, x.touch.position);
		double r[3] = { x.quest.position.x - rp.x, x.quest.position.y - rp.y, x.quest.position.z - rp.z };
		double J[3][6] = {};
		for (int k = 0; k < 3; k++) J[k][k] = 1;
		for (int col = 0; col < 3; col++) {
			XrVector3f cc = colOf(c, col);
			J[0][3 + col] = cc.x;
			J[1][3 + col] = cc.y;
			J[2][3 + col] = cc.z;
		}
		for (int p = 0; p < 6; p++) {
			for (int q = 0; q < 6; q++)
				for (int k = 0; k < 3; k++) A[p][q] += J[k][p] * J[k][q];
			for (int k = 0; k < 3; k++) A[p][6] += J[k][p] * r[k];
		}
	}
	for (int p = 0; p < 6; p++) {   // Gauss-Jordan with partial pivoting
		int piv = p;
		for (int q = p + 1; q < 6; q++)
			if (fabs(A[q][p]) > fabs(A[piv][p])) piv = q;
		if (fabs(A[piv][p]) < 1e-9) return -1;
		for (int k = 0; k < 7; k++) {
			double t = A[p][k];
			A[p][k] = A[piv][k];
			A[piv][k] = t;
		}
		for (int q = 0; q < 6; q++) {
			if (q == p) continue;
			double f = A[q][p] / A[p][p];
			for (int k = 0; k < 7; k++) A[q][k] -= f * A[p][k];
		}
	}
	double sol[6];
	for (int p = 0; p < 6; p++) sol[p] = A[p][6] / A[p][p];
	XrVector3f t = { (float)sol[0], (float)sol[1], (float)sol[2] }, d = { (float)sol[3], (float)sol[4], (float)sol[5] };

	double err = 0;
	for (const Sample& x : s) {
		XrVector3f off = QRot(QMul(rz, x.touch.orientation), d);
		XrVector3f rp = QRot(rz, x.touch.position);
		double dx = rp.x + off.x + t.x - x.quest.position.x, dy = rp.y + off.y + t.y - x.quest.position.y,
			   dz = rp.z + off.z + t.z - x.quest.position.z;
		err += sqrt(dx * dx + dy * dy + dz * dz);
	}
	*z = { rz, t };
	return err / s.size();
}

// Runs from the game's xrSyncActions. g_lock held.
void CalibrationTick(XrTime syncTime)
{
	if (g_calSet == XR_NULL_HANDLE || g_session == XR_NULL_HANDLE || g_ref == XR_NULL_HANDLE) return;
	ULONGLONG now = GetTickCount64();

	if (g_cal == Cal::Idle) {
		bool combo;
		{
			std::lock_guard<std::mutex> lk(g_srcLock);
			combo = g_src.ready && g_src.state[0].value[A_STICK_CLICK] > 0.5f && g_src.state[1].value[A_STICK_CLICK] > 0.5f;
		}
		g_comboSince = combo ? (g_comboSince ? g_comboSince : now) : 0;
		std::wstring request = AtriumFile(L"touch_calibrate");
		bool requested = GetFileAttributesW(request.c_str()) != INVALID_FILE_ATTRIBUTES;
		if (requested) DeleteFileW(request.c_str());
		if (!requested && !(g_comboSince && now - g_comboSince > 2000)) return;
		g_comboSince = 0;
		g_cal = Cal::Collecting;
		g_samples.clear();
		g_calStart = now;
		g_log("touch: calibrating, hold the right Touch and the right Quest controller together and move them");
		Buzz(1, 0.3f);
		return;
	}

	if (now - g_lastSample >= 30) {
		g_lastSample = now;
		LONGLONG qpc;
		XrSpaceLocation q = { XR_TYPE_SPACE_LOCATION };
		Sample smp;
		XrSpaceLocationFlags tf = 0;
		bool ok = QpcOf(syncTime, &qpc) && XR_SUCCEEDED(g_b.locate(g_calSpace[1], g_ref, syncTime, &q)) &&
			(q.locationFlags & kPoseValid) == kPoseValid;
		if (ok) {
			std::lock_guard<std::mutex> lk(g_srcLock);
			ok = SourceLocate(1, false, qpc, &smp.touch, &tf) && (tf & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
		}
		if (ok) {
			smp.quest = q.pose;
			g_samples.push_back(smp);
		}
	}
	if (now - g_calStart < kCalMs) return;

	g_cal = Cal::Idle;
	XrPosef z;
	double err = SolveCalibration(g_samples, &z);
	if (err < 0 || err > 0.03) {
		g_log("touch: calibration failed (%zu samples, error %.1f mm): move and turn more, keep both tracked", g_samples.size(),
			err * 1000);
		Buzz(1, 0.1f);
		return;
	}
	g_calib = z;
	bool first = !g_calibrated;
	g_calibrated = true;
	SaveCalibration(err * 1000, g_samples.size());
	g_log("touch: calibrated from %zu samples, mean error %.1f mm", g_samples.size(), err * 1000);
	Buzz(1, 0.6f);
	if (first) g_profileEvent = true;   // the game switches over to the Touches
}

// ---- action values ------------------------------------------------------------------------------
Value Compute(const Action& a, int filter, const HandState hands[2])
{
	Value v;
	for (const Bind& b : a.binds) {
		if (filter && b.hand != filter - 1) continue;
		const HandState& s = hands[b.hand];
		if (!s.active) continue;
		v.active = true;
		switch (a.type) {
		case XR_ACTION_TYPE_BOOLEAN_INPUT:
			if (CompValue(s, b.comp) > 0.5f) v.f = 1;
			break;
		case XR_ACTION_TYPE_FLOAT_INPUT: {
			float x = CompValue(s, b.comp);
			if (fabsf(x) > fabsf(v.f)) v.f = x;
			break;
		}
		case XR_ACTION_TYPE_VECTOR2F_INPUT:
			if (b.comp == C_STICK && s.stick.x * s.stick.x + s.stick.y * s.stick.y > v.v.x * v.v.x + v.v.y * v.v.y) v.v = s.stick;
			break;
		default: break;
		}
	}
	return v;
}

int FilterOf(XrPath sub)
{
	if (sub == XR_NULL_PATH) return 0;
	if (sub == g_handPath[0]) return 1;
	if (sub == g_handPath[1]) return 2;
	return -1;
}

const Value* ValueFor(XrAction action, XrPath sub)
{
	int f = FilterOf(sub);
	if (f < 0) return nullptr;
	auto it = g_values.find((uint64_t)action * 3 + f);
	return it == g_values.end() ? nullptr : &it->second;
}

// The Touch a pose action space follows and which pose, or false if it isn't one.
bool SpaceTouch(XrSpace space, int* hand, bool* aim, XrPosef* offset)
{
	auto it = g_actionSpaces.find((uint64_t)space);
	if (it == g_actionSpaces.end()) return false;
	auto ai = g_actions.find((uint64_t)it->second.action);
	if (ai == g_actions.end()) return false;
	int f = FilterOf(it->second.sub);
	for (const Bind& b : ai->second.binds) {
		if ((b.comp != C_GRIP && b.comp != C_AIM) || (f > 0 && b.hand != f - 1)) continue;
		*hand = b.hand;
		*aim = b.comp == C_AIM;
		*offset = it->second.offset;
		return true;
	}
	return false;
}

// A space's pose in g_ref: a Touch, or the backend's answer. g_lock held.
bool InRef(XrSpace space, XrTime time, XrPosef* pose, XrSpaceLocationFlags* flags)
{
	int hand;
	bool aim;
	XrPosef offset;
	if (SpaceTouch(space, &hand, &aim, &offset)) {
		LONGLONG qpc;
		XrPosef p;
		XrSpaceLocationFlags f = 0;
		std::lock_guard<std::mutex> lk(g_srcLock);
		if (!QpcOf(time, &qpc) || !g_src.state[hand].active || !SourceLocate(hand, aim, qpc, &p, &f)) {
			*flags = 0;
			return false;
		}
		*pose = PMul(PMul(g_calib, p), offset);
		*flags = f;
		return true;
	}
	if (space == g_ref) {
		*pose = kIdentity;
		*flags = kPoseValid | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
		return true;
	}
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(g_b.locate(space, g_ref, time, &loc))) return false;
	*pose = loc.pose;
	*flags = loc.locationFlags;
	return (loc.locationFlags & kPoseValid) == kPoseValid;
}

bool IsTouchSpace(XrSpace s)
{
	int h;
	bool a;
	XrPosef o;
	return SpaceTouch(s, &h, &a, &o);
}

// Locates when space or base is a Touch; false if neither is (the caller asks the backend).
bool LocateTouch(XrSpace space, XrSpace base, XrTime time, XrSpaceLocationFlags* flags, XrPosef* pose)
{
	if (!Active() || (!IsTouchSpace(space) && !IsTouchSpace(base))) return false;
	XrPosef s, b;
	XrSpaceLocationFlags fs = 0, fb = 0;
	*flags = 0;
	*pose = kIdentity;
	if (InRef(space, time, &s, &fs) && InRef(base, time, &b, &fb)) {
		*pose = PMul(PInv(b), s);
		*flags = fs & fb;
	}
	return true;
}

void ClearVelocity(void* next)
{
	for (XrBaseOutStructure* n = (XrBaseOutStructure*)next; n; n = n->next)
		if (n->type == XR_TYPE_SPACE_VELOCITY) ((XrSpaceVelocity*)n)->velocityFlags = 0;
}

// ---- wrappers -----------------------------------------------------------------------------------
XRAPI_ATTR XrResult XRAPI_CALL W_xrCreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* action)
{
	XrResult r = g_b.createAction(set, info, action);
	if (XR_SUCCEEDED(r)) {
		std::lock_guard<std::mutex> lk(g_lock);
		Action& a = g_actions[(uint64_t)*action];
		a.type = info->actionType;
		a.name = info->actionName;
	}
	return r;
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* sb)
{
	std::string profile = PathString(sb->interactionProfile);
	std::vector<XrActionSuggestedBinding> all(sb->suggestedBindings, sb->suggestedBindings + sb->countSuggestedBindings);
	{
		std::lock_guard<std::mutex> lk(g_lock);
		auto& list = g_suggested[profile];
		list.clear();
		for (const XrActionSuggestedBinding& b : all) list.push_back({ b.action, PathString(b.binding) });
	}
	// Atrium's calibration action rides along on every profile the game suggests.
	size_t own = all.size();
	if (g_calGrip != XR_NULL_HANDLE) {
		for (int h = 0; h < 2; h++) {
			XrPath p = XR_NULL_PATH;
			g_b.stringToPath(instance, h ? "/user/hand/right/input/grip/pose" : "/user/hand/left/input/grip/pose", &p);
			all.push_back({ g_calGrip, p });
		}
	}
	XrInteractionProfileSuggestedBinding mine = *sb;
	mine.countSuggestedBindings = (uint32_t)all.size();
	mine.suggestedBindings = all.data();
	XrResult r = g_b.suggest(instance, &mine);
	if (XR_FAILED(r) && all.size() != own) r = g_b.suggest(instance, sb);   // profile without grip poses
	return r;
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info)
{
	std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
	if (g_calSet != XR_NULL_HANDLE) sets.push_back(g_calSet);
	XrSessionActionSetsAttachInfo mine = *info;
	mine.countActionSets = (uint32_t)sets.size();
	mine.actionSets = sets.data();
	XrResult r = g_b.attach(session, &mine);
	if (XR_FAILED(r)) return g_b.attach(session, info);

	std::lock_guard<std::mutex> lk(g_lock);
	// The profile the Touches answer as: the game's own Touch bindings when it has them.
	static const char* const kPrefer[] = { "/interaction_profiles/oculus/touch_controller", "/interaction_profiles/meta/touch_controller_plus",
		"/interaction_profiles/facebook/touch_controller_pro", "/interaction_profiles/meta/touch_pro_controller",
		"/interaction_profiles/khr/simple_controller" };
	g_profile.clear();
	for (const char* p : kPrefer)
		if (g_suggested.count(p)) {
			g_profile = p;
			break;
		}
	if (g_profile.empty() && !g_suggested.empty()) g_profile = g_suggested.begin()->first;
	g_b.stringToPath(g_inst, g_profile.empty() ? "/interaction_profiles/oculus/touch_controller" : g_profile.c_str(), &g_profilePath);
	for (auto& kv : g_actions) kv.second.binds.clear();
	if (!g_profile.empty()) {
		for (const auto& sb : g_suggested[g_profile]) {
			auto it = g_actions.find((uint64_t)sb.first);
			if (it == g_actions.end()) continue;
			int hand = sb.second.rfind("/user/hand/left/", 0) == 0 ? 0 : sb.second.rfind("/user/hand/right/", 0) == 0 ? 1 : -1;
			if (hand < 0) continue;
			Comp c = ParseComp(sb.second.substr(hand ? 17 : 16));
			if (c != C_NONE) it->second.binds.push_back({ hand, c });
		}
	}
	g_session = session;
	XrReferenceSpaceCreateInfo rci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_STAGE, kIdentity };
	bool stage = XR_SUCCEEDED(g_b.createReferenceSpace(session, &rci, &g_ref));
	if (!stage) {
		rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		g_b.createReferenceSpace(session, &rci, &g_ref);
	}
	if (g_calibrated && stage != g_refStage) {
		g_log("touch: the saved calibration is for the %s space, which this session doesn't have: calibrate again",
			g_refStage ? "stage" : "local");
		g_calibrated = false;
	}
	g_refStage = stage;
	if (g_calSet != XR_NULL_HANDLE) {
		for (int h = 0; h < 2; h++) {
			XrActionSpaceCreateInfo s = { XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, g_calGrip, g_handPath[h], kIdentity };
			g_b.createActionSpace(session, &s, &g_calSpace[h]);
		}
	}
	g_profileEvent = g_calibrated;
	g_log("touch: game bindings attached, Touches answer as %s%s", g_profile.empty() ? "(no bindings)" : g_profile.c_str(),
		g_calibrated ? "" : " once calibrated");
	return r;
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* space)
{
	XrResult r = g_b.createActionSpace(session, info, space);
	if (XR_SUCCEEDED(r)) {
		std::lock_guard<std::mutex> lk(g_lock);
		g_actionSpaces[(uint64_t)*space] = { info->action, info->subactionPath, info->poseInActionSpace };
	}
	return r;
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrDestroySpace(XrSpace space)
{
	{
		std::lock_guard<std::mutex> lk(g_lock);
		g_actionSpaces.erase((uint64_t)space);
	}
	return g_b.destroySpace(space);
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrSyncActions(XrSession session, const XrActionsSyncInfo* info)
{
	std::vector<XrActiveActionSet> sets(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
	if (g_calSet != XR_NULL_HANDLE && session == g_session) sets.push_back({ g_calSet, XR_NULL_PATH });
	XrActionsSyncInfo mine = *info;
	mine.countActiveActionSets = (uint32_t)sets.size();
	mine.activeActionSets = sets.data();
	XrResult r = g_b.sync(session, &mine);
	if (XR_FAILED(r)) return r;

	HandState hands[2];
	{
		std::lock_guard<std::mutex> lk(g_srcLock);
		SourceSync();
		hands[0] = g_src.state[0];
		hands[1] = g_src.state[1];
	}
	std::lock_guard<std::mutex> lk(g_lock);
	XrTime now = Now();
	CalibrationTick(now);
	if (!Active()) return r;
	for (const auto& kv : g_actions) {
		for (int f = 0; f < 3; f++) {
			Value nv = Compute(kv.second, f, hands);
			Value& old = g_values[kv.first * 3 + f];
			bool differs = nv.f != old.f || nv.v.x != old.v.x || nv.v.y != old.v.y;
			nv.changed = nv.active && old.active && differs;
			nv.lastChange = nv.changed || !old.active ? now : old.lastChange;
			old = nv;
		}
	}
	return r;
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* state)
{
	{
		std::lock_guard<std::mutex> lk(g_lock);
		const Value* v = Active() ? ValueFor(info->action, info->subactionPath) : nullptr;
		if (v) {
			state->currentState = v->f > 0.5f;
			state->changedSinceLastSync = v->changed;
			state->lastChangeTime = v->lastChange;
			state->isActive = v->active;
			return XR_SUCCESS;
		}
	}
	return g_b.getBool(session, info, state);
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* info, XrActionStateFloat* state)
{
	{
		std::lock_guard<std::mutex> lk(g_lock);
		const Value* v = Active() ? ValueFor(info->action, info->subactionPath) : nullptr;
		if (v) {
			state->currentState = v->f;
			state->changedSinceLastSync = v->changed;
			state->lastChangeTime = v->lastChange;
			state->isActive = v->active;
			return XR_SUCCESS;
		}
	}
	return g_b.getFloat(session, info, state);
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* info, XrActionStateVector2f* state)
{
	{
		std::lock_guard<std::mutex> lk(g_lock);
		const Value* v = Active() ? ValueFor(info->action, info->subactionPath) : nullptr;
		if (v) {
			state->currentState = v->v;
			state->changedSinceLastSync = v->changed;
			state->lastChangeTime = v->lastChange;
			state->isActive = v->active;
			return XR_SUCCESS;
		}
	}
	return g_b.getVec2(session, info, state);
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* info, XrActionStatePose* state)
{
	{
		std::lock_guard<std::mutex> lk(g_lock);
		const Value* v = Active() ? ValueFor(info->action, info->subactionPath) : nullptr;
		if (v) {
			state->isActive = v->active;
			return XR_SUCCESS;
		}
	}
	return g_b.getPose(session, info, state);
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrLocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location)
{
	time = g_fixTime(time);
	{
		std::lock_guard<std::mutex> lk(g_lock);
		if (LocateTouch(space, base, time, &location->locationFlags, &location->pose)) {
			ClearVelocity(location->next);
			return XR_SUCCESS;
		}
	}
	return g_b.locate(space, base, time, location);
}

XrResult LocateMany(PFN_xrLocateSpaces next, XrSession session, const XrSpacesLocateInfo* info, XrSpaceLocations* out)
{
	XrSpacesLocateInfo li = *info;
	li.time = g_fixTime(info->time);
	XrResult r = next(session, &li, out);
	if (XR_FAILED(r)) return r;
	std::lock_guard<std::mutex> lk(g_lock);
	if (!Active()) return r;
	XrSpaceVelocities* vel = nullptr;
	for (XrBaseOutStructure* n = (XrBaseOutStructure*)out->next; n; n = n->next)
		if (n->type == XR_TYPE_SPACE_VELOCITIES) vel = (XrSpaceVelocities*)n;
	for (uint32_t i = 0; i < info->spaceCount && i < out->locationCount; i++) {
		XrSpaceLocationData& d = out->locations[i];
		if (!LocateTouch(info->spaces[i], info->baseSpace, li.time, &d.locationFlags, &d.pose)) continue;
		if (vel && i < vel->velocityCount) vel->velocities[i].velocityFlags = 0;
	}
	return r;
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrLocateSpaces(XrSession s, const XrSpacesLocateInfo* i, XrSpaceLocations* o) { return LocateMany(g_b.locateSpaces, s, i, o); }
XRAPI_ATTR XrResult XRAPI_CALL W_xrLocateSpacesKHR(XrSession s, const XrSpacesLocateInfo* i, XrSpaceLocations* o) { return LocateMany(g_b.locateSpacesKHR, s, i, o); }

XRAPI_ATTR XrResult XRAPI_CALL W_xrGetCurrentInteractionProfile(XrSession session, XrPath user, XrInteractionProfileState* state)
{
	{
		std::lock_guard<std::mutex> lk(g_lock);
		if (Active() && (user == g_handPath[0] || user == g_handPath[1])) {
			std::lock_guard<std::mutex> sk(g_srcLock);
			state->interactionProfile = g_src.state[user == g_handPath[1]].active ? g_profilePath : XR_NULL_PATH;
			return XR_SUCCESS;
		}
	}
	return g_b.currentProfile(session, user, state);
}

// The hands a haptic action reaches on the Touches, or false if it's not the Touches' to play.
bool HapticHands(const XrHapticActionInfo* info, bool out[2])
{
	std::lock_guard<std::mutex> lk(g_lock);
	if (!Active()) return false;
	auto it = g_actions.find((uint64_t)info->action);
	if (it == g_actions.end()) return false;
	int f = FilterOf(info->subactionPath);
	out[0] = out[1] = false;
	for (const Bind& b : it->second.binds)
		if (b.comp == C_HAPTIC && (f <= 0 || b.hand == f - 1)) out[b.hand] = true;
	return out[0] || out[1];
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* info, const XrHapticBaseHeader* haptic)
{
	bool hands[2];
	if (!HapticHands(info, hands) || haptic->type != XR_TYPE_HAPTIC_VIBRATION) return g_b.applyHaptic(session, info, haptic);
	for (int h = 0; h < 2; h++)
		if (hands[h]) SourceHaptic(h, *(const XrHapticVibration*)haptic);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* info)
{
	bool hands[2];
	if (!HapticHands(info, hands)) return g_b.stopHaptic(session, info);
	XrHapticVibration off = { XR_TYPE_HAPTIC_VIBRATION, nullptr, XR_MIN_HAPTIC_DURATION, XR_FREQUENCY_UNSPECIFIED, 0.f };
	for (int h = 0; h < 2; h++)
		if (hands[h]) SourceHaptic(h, off);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrPollEvent(XrInstance instance, XrEventDataBuffer* ev)
{
	XrResult r = g_b.pollEvent(instance, ev);
	if (r != XR_EVENT_UNAVAILABLE) return r;
	std::lock_guard<std::mutex> lk(g_lock);
	if (!g_profileEvent || g_session == XR_NULL_HANDLE) return r;
	g_profileEvent = false;
	XrEventDataInteractionProfileChanged* e = (XrEventDataInteractionProfileChanged*)ev;
	e->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
	e->next = nullptr;
	e->session = g_session;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL W_xrDestroySession(XrSession session)
{
	{
		std::lock_guard<std::mutex> lk(g_lock);
		if (session == g_session) {
			g_session = XR_NULL_HANDLE;
			g_ref = XR_NULL_HANDLE;
			g_calSpace[0] = g_calSpace[1] = XR_NULL_HANDLE;
			g_actionSpaces.clear();
			g_values.clear();
			g_cal = Cal::Idle;
		}
	}
	return g_b.destroySession(session);
}

struct HookDef { const char* name; PFN_xrVoidFunction fn; };
const HookDef kHooks[] = {
	{ "xrCreateAction", (PFN_xrVoidFunction)W_xrCreateAction },
	{ "xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)W_xrSuggestInteractionProfileBindings },
	{ "xrAttachSessionActionSets", (PFN_xrVoidFunction)W_xrAttachSessionActionSets },
	{ "xrCreateActionSpace", (PFN_xrVoidFunction)W_xrCreateActionSpace },
	{ "xrDestroySpace", (PFN_xrVoidFunction)W_xrDestroySpace },
	{ "xrSyncActions", (PFN_xrVoidFunction)W_xrSyncActions },
	{ "xrGetActionStateBoolean", (PFN_xrVoidFunction)W_xrGetActionStateBoolean },
	{ "xrGetActionStateFloat", (PFN_xrVoidFunction)W_xrGetActionStateFloat },
	{ "xrGetActionStateVector2f", (PFN_xrVoidFunction)W_xrGetActionStateVector2f },
	{ "xrGetActionStatePose", (PFN_xrVoidFunction)W_xrGetActionStatePose },
	{ "xrLocateSpace", (PFN_xrVoidFunction)W_xrLocateSpace },
	{ "xrLocateSpaces", (PFN_xrVoidFunction)W_xrLocateSpaces },
	{ "xrLocateSpacesKHR", (PFN_xrVoidFunction)W_xrLocateSpacesKHR },
	{ "xrGetCurrentInteractionProfile", (PFN_xrVoidFunction)W_xrGetCurrentInteractionProfile },
	{ "xrApplyHapticFeedback", (PFN_xrVoidFunction)W_xrApplyHapticFeedback },
	{ "xrStopHapticFeedback", (PFN_xrVoidFunction)W_xrStopHapticFeedback },
	{ "xrPollEvent", (PFN_xrVoidFunction)W_xrPollEvent },
	{ "xrDestroySession", (PFN_xrVoidFunction)W_xrDestroySession },
};

} // namespace

void Configure(const std::wstring& manifest, void (*log)(const char*, ...))
{
	g_log = log;
	g_manifest = manifest;
	if (Configured()) g_log("touch: CV1 Touch swap on, through %ls", manifest.c_str());
}

bool Configured() { return !g_manifest.empty(); }

void OnInstanceCreated(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, XrTime (*fixTime)(XrTime))
{
	if (!Configured()) return;
	auto get = [&](const char* name) {
		PFN_xrVoidFunction f = nullptr;
		gipa(instance, name, &f);
		return f;
	};
	g_inst = instance;
	g_fixTime = fixTime;
	g_b.stringToPath = (PFN_xrStringToPath)get("xrStringToPath");
	g_b.pathToString = (PFN_xrPathToString)get("xrPathToString");
	g_b.createActionSet = (PFN_xrCreateActionSet)get("xrCreateActionSet");
	g_b.createAction = (PFN_xrCreateAction)get("xrCreateAction");
	g_b.suggest = (PFN_xrSuggestInteractionProfileBindings)get("xrSuggestInteractionProfileBindings");
	g_b.attach = (PFN_xrAttachSessionActionSets)get("xrAttachSessionActionSets");
	g_b.createActionSpace = (PFN_xrCreateActionSpace)get("xrCreateActionSpace");
	g_b.createReferenceSpace = (PFN_xrCreateReferenceSpace)get("xrCreateReferenceSpace");
	g_b.destroySpace = (PFN_xrDestroySpace)get("xrDestroySpace");
	g_b.sync = (PFN_xrSyncActions)get("xrSyncActions");
	g_b.getBool = (PFN_xrGetActionStateBoolean)get("xrGetActionStateBoolean");
	g_b.getFloat = (PFN_xrGetActionStateFloat)get("xrGetActionStateFloat");
	g_b.getVec2 = (PFN_xrGetActionStateVector2f)get("xrGetActionStateVector2f");
	g_b.getPose = (PFN_xrGetActionStatePose)get("xrGetActionStatePose");
	g_b.locate = (PFN_xrLocateSpace)get("xrLocateSpace");
	g_b.locateSpaces = (PFN_xrLocateSpaces)get("xrLocateSpaces");
	g_b.locateSpacesKHR = (PFN_xrLocateSpaces)get("xrLocateSpacesKHR");
	g_b.currentProfile = (PFN_xrGetCurrentInteractionProfile)get("xrGetCurrentInteractionProfile");
	g_b.applyHaptic = (PFN_xrApplyHapticFeedback)get("xrApplyHapticFeedback");
	g_b.stopHaptic = (PFN_xrStopHapticFeedback)get("xrStopHapticFeedback");
	g_b.pollEvent = (PFN_xrPollEvent)get("xrPollEvent");
	g_b.destroySession = (PFN_xrDestroySession)get("xrDestroySession");
	g_b.toQpc = (PFN_xrConvertTimeToWin32PerformanceCounterKHR)get("xrConvertTimeToWin32PerformanceCounterKHR");
	g_b.fromQpc = (PFN_xrConvertWin32PerformanceCounterToTimeKHR)get("xrConvertWin32PerformanceCounterToTimeKHR");
	if (!g_b.toQpc || !g_b.fromQpc) {
		g_log("touch: the backend has no time conversion (XR_KHR_win32_convert_performance_counter_time), swap off");
		g_manifest.clear();
		return;
	}
	g_b.stringToPath(instance, "/user/hand/left", &g_handPath[0]);
	g_b.stringToPath(instance, "/user/hand/right", &g_handPath[1]);

	XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(asci.actionSetName, "atrium_touch_calibration");
	strcpy_s(asci.localizedActionSetName, "Atrium Touch calibration");
	asci.priority = 0;
	if (XR_SUCCEEDED(g_b.createActionSet(instance, &asci, &g_calSet))) {
		XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
		strcpy_s(aci.actionName, "controller_grip");
		strcpy_s(aci.localizedActionName, "Controller grip");
		aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
		aci.countSubactionPaths = 2;
		aci.subactionPaths = g_handPath;
		if (XR_FAILED(g_b.createAction(g_calSet, &aci, &g_calGrip))) g_calSet = XR_NULL_HANDLE;
	}
	LoadCalibration();
	std::lock_guard<std::mutex> lk(g_srcLock);
	g_src.lastTry = GetTickCount64();
	SourceStart();
}

void OnInstanceDestroyed(XrInstance instance)
{
	if (instance != g_inst) return;
	std::lock_guard<std::mutex> lk(g_lock);
	g_inst = XR_NULL_HANDLE;
	g_calSet = XR_NULL_HANDLE;
	g_calGrip = XR_NULL_HANDLE;
	g_actions.clear();
	g_suggested.clear();
}

PFN_xrVoidFunction Hook(const char* name)
{
	if (!Configured()) return nullptr;
	for (const HookDef& h : kHooks)
		if (strcmp(name, h.name) == 0) return h.fn;
	return nullptr;
}

} // namespace touch
