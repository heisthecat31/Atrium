// touch_probe: prints what the CV1 Touch swap sees. Connects to the Atrium service headless,
// the same way atrium_xr does in a game, and shows each Touch's buttons and pose for a while.
//
//   touch_probe <atrium service runtime manifest or openxr_atrium.dll> [seconds]
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

static PFN_xrGetInstanceProcAddr gipa;
static XrInstance inst;

template <typename F> static F Fn(const char* name)
{
	PFN_xrVoidFunction f = nullptr;
	gipa(inst, name, &f);
	return (F)f;
}

#define CHECK(call)                                                   \
	do {                                                              \
		XrResult r_ = (call);                                         \
		if (XR_FAILED(r_)) {                                          \
			printf("%s failed: %d\n", #call, r_);                     \
			return 1;                                                 \
		}                                                             \
	} while (0)

static std::wstring LibraryOf(const std::wstring& arg)
{
	if (arg.size() > 4 && _wcsicmp(arg.c_str() + arg.size() - 4, L".dll") == 0) return arg;
	FILE* f = nullptr;
	if (_wfopen_s(&f, arg.c_str(), L"rb") || !f) return {};
	std::string text(8192, '\0');
	text.resize(fread(&text[0], 1, text.size(), f));
	fclose(f);
	size_t at = text.find("\"library_path\"");
	if (at == std::string::npos) return {};
	at = text.find('"', text.find(':', at) + 1);
	std::string lib = text.substr(at + 1, text.find('"', at + 1) - at - 1);
	for (size_t i = 0; (i = lib.find("\\\\", i)) != std::string::npos; i++) lib.erase(i, 1);
	std::wstring w(lib.begin(), lib.end());
	if (w.size() < 2 || (w[1] != L':' && w[0] != L'\\')) w = arg.substr(0, arg.find_last_of(L"\\/") + 1) + w;
	return w;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2) {
		printf("usage: touch_probe <runtime manifest or openxr_atrium.dll> [seconds]\n");
		return 2;
	}
	int seconds = argc > 2 ? _wtoi(argv[2]) : 20;
	std::wstring lib = LibraryOf(argv[1]);
	HMODULE dll = LoadLibraryExW(lib.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!dll) {
		printf("can't load %ls (%lu)\n", lib.c_str(), GetLastError());
		return 1;
	}
	auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(dll, "xrNegotiateLoaderRuntimeInterface");
	XrNegotiateLoaderInfo li = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof li, 1,
		XR_CURRENT_LOADER_RUNTIME_VERSION, XR_MAKE_VERSION(1, 0, 0), XR_CURRENT_API_VERSION };
	XrNegotiateRuntimeRequest rq = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof rq };
	CHECK(negotiate(&li, &rq));
	gipa = rq.getInstanceProcAddr;

	const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME };
	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy_s(ici.applicationInfo.applicationName, "Atrium Touch probe");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = exts;
	CHECK(Fn<PFN_xrCreateInstance>("xrCreateInstance")(&ici, &inst));

	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
	XrSystemId sys;
	CHECK(Fn<PFN_xrGetSystem>("xrGetSystem")(inst, &sgi, &sys));
	auto toPath = Fn<PFN_xrStringToPath>("xrStringToPath");
	XrPath hand[2];
	toPath(inst, "/user/hand/left", &hand[0]);
	toPath(inst, "/user/hand/right", &hand[1]);

	XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(asci.actionSetName, "probe");
	strcpy_s(asci.localizedActionSetName, "probe");
	XrActionSet set;
	CHECK(Fn<PFN_xrCreateActionSet>("xrCreateActionSet")(inst, &asci, &set));
	struct { const char* name; XrActionType type; const char* l; const char* r; XrAction a; } acts[] = {
		{ "grip", XR_ACTION_TYPE_POSE_INPUT, "input/grip/pose", "input/grip/pose" },
		{ "trigger", XR_ACTION_TYPE_FLOAT_INPUT, "input/trigger/value", "input/trigger/value" },
		{ "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, "input/squeeze/value", "input/squeeze/value" },
		{ "stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "input/thumbstick", "input/thumbstick" },
		{ "lower", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/x/click", "input/a/click" },
		{ "upper", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/y/click", "input/b/click" },
		{ "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, "input/thumbstick/click", "input/thumbstick/click" },
	};
	XrActionSuggestedBinding binds[32];
	uint32_t nb = 0;
	for (auto& a : acts) {
		XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
		strcpy_s(aci.actionName, a.name);
		strcpy_s(aci.localizedActionName, a.name);
		aci.actionType = a.type;
		aci.countSubactionPaths = 2;
		aci.subactionPaths = hand;
		CHECK(Fn<PFN_xrCreateAction>("xrCreateAction")(set, &aci, &a.a));
		for (int h = 0; h < 2; h++) {
			std::string p = std::string(h ? "/user/hand/right/" : "/user/hand/left/") + (h ? a.r : a.l);
			toPath(inst, p.c_str(), &binds[nb].binding);
			binds[nb++].action = a.a;
		}
	}
	XrInteractionProfileSuggestedBinding ipsb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	toPath(inst, "/interaction_profiles/oculus/touch_controller", &ipsb.interactionProfile);
	ipsb.countSuggestedBindings = nb;
	ipsb.suggestedBindings = binds;
	CHECK(Fn<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings")(inst, &ipsb));

	XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO, nullptr, 0, sys };
	XrSession sess;
	CHECK(Fn<PFN_xrCreateSession>("xrCreateSession")(inst, &sci, &sess));
	XrSessionActionSetsAttachInfo sai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set };
	CHECK(Fn<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets")(sess, &sai));
	XrPosef id = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
	XrReferenceSpaceCreateInfo rci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_STAGE, id };
	XrSpace stage, grip[2];
	CHECK(Fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(sess, &rci, &stage));
	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo s = { XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, acts[0].a, hand[h], id };
		CHECK(Fn<PFN_xrCreateActionSpace>("xrCreateActionSpace")(sess, &s, &grip[h]));
	}
	auto poll = Fn<PFN_xrPollEvent>("xrPollEvent");
	for (int i = 0; i < 100; i++) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		if (poll(inst, &ev) == XR_SUCCESS && ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
			((XrEventDataSessionStateChanged*)&ev)->state == XR_SESSION_STATE_READY)
			break;
		Sleep(10);
	}
	XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
	CHECK(Fn<PFN_xrBeginSession>("xrBeginSession")(sess, &sbi));
	printf("connected, headless session running\n");

	auto sync = Fn<PFN_xrSyncActions>("xrSyncActions");
	auto getF = Fn<PFN_xrGetActionStateFloat>("xrGetActionStateFloat");
	auto getB = Fn<PFN_xrGetActionStateBoolean>("xrGetActionStateBoolean");
	auto getV = Fn<PFN_xrGetActionStateVector2f>("xrGetActionStateVector2f");
	auto getP = Fn<PFN_xrGetActionStatePose>("xrGetActionStatePose");
	auto locate = Fn<PFN_xrLocateSpace>("xrLocateSpace");
	auto convert = Fn<PFN_xrConvertWin32PerformanceCounterToTimeKHR>("xrConvertWin32PerformanceCounterToTimeKHR");
	int tracked[2] = {}, located[2] = {}, frames = 0;
	for (ULONGLONG end = GetTickCount64() + seconds * 1000ull; GetTickCount64() < end; Sleep(100), frames++) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		while (poll(inst, &ev) == XR_SUCCESS) ev = { XR_TYPE_EVENT_DATA_BUFFER };
		XrActiveActionSet aas = { set, XR_NULL_PATH };
		XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &aas };
		sync(sess, &si);
		LARGE_INTEGER qpc;
		QueryPerformanceCounter(&qpc);
		XrTime now = 0;
		if (convert) convert(inst, &qpc, &now);
		for (int h = 0; h < 2; h++) {
			XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO, nullptr, acts[0].a, hand[h] };
			XrActionStatePose p = { XR_TYPE_ACTION_STATE_POSE };
			getP(sess, &gi, &p);
			XrActionStateFloat tr = { XR_TYPE_ACTION_STATE_FLOAT }, sq = { XR_TYPE_ACTION_STATE_FLOAT };
			gi.action = acts[1].a;
			getF(sess, &gi, &tr);
			gi.action = acts[2].a;
			getF(sess, &gi, &sq);
			XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
			gi.action = acts[3].a;
			getV(sess, &gi, &st);
			XrActionStateBoolean lo = { XR_TYPE_ACTION_STATE_BOOLEAN }, up = { XR_TYPE_ACTION_STATE_BOOLEAN }, sc = { XR_TYPE_ACTION_STATE_BOOLEAN };
			gi.action = acts[4].a;
			getB(sess, &gi, &lo);
			gi.action = acts[5].a;
			getB(sess, &gi, &up);
			gi.action = acts[6].a;
			getB(sess, &gi, &sc);
			XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
			locate(grip[h], stage, now, &loc);
			if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) tracked[h]++;
			if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) located[h]++;
			if (frames % 10 == 0)
				printf("%s %s trig %.2f grip %.2f stick %+.2f %+.2f %s%s%s | pos %s %+.3f %+.3f %+.3f rot %+.2f %+.2f %+.2f %+.2f\n",
					h ? "R" : "L", p.isActive ? "on " : "off", tr.currentState, sq.currentState, st.currentState.x, st.currentState.y,
					lo.currentState ? (h ? "A " : "X ") : "  ", up.currentState ? (h ? "B " : "Y ") : "  ", sc.currentState ? "stick " : "      ",
					(loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) ? "tracked" : (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? "held   " : "none   ",
					loc.pose.position.x, loc.pose.position.y, loc.pose.position.z, loc.pose.orientation.x, loc.pose.orientation.y,
					loc.pose.orientation.z, loc.pose.orientation.w);
		}
	}
	printf("summary over %d samples: left rotation %d, position tracked %d; right rotation %d, position tracked %d\n", frames, located[0],
		tracked[0], located[1], tracked[1]);
	return 0;
}
