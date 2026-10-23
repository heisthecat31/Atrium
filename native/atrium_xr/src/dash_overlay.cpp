// The Atrium dash over any game.
//
// atrium-dash (native/atrium_dash) draws the dash; this part lives in every game that runs on the
// Atrium Runtime and puts that picture on top of the game's frame (atrium_dash_shared.h):
//   - Atrium's own actions ride along with the game's: aim and grip poses, trigger, squeeze, stick,
//     haptics and the left menu button, added to the bindings the game suggests (only for the
//     controllers the game itself binds, so the runtime picks the same controller profile).
//   - The dash button (Controls tab: menu, X, Y, A, B or a stick click; held 0.45 s or pressed)
//     opens or closes the dash. Held: a shorter press still reaches the game, once let go.
//     Pressed: the game never sees that button.
//   - Button remapping (Controls tab) rewrites the game's Touch bindings when it suggests them.
//   - While the dash is open the game loses input focus, as under Oculus Dash (it's told its
//     session went VISIBLE, so it pauses, mutes and hides its hands), its actions aren't synced
//     (it sees no input), and each
//     frame the eyes and controllers go to atrium-dash and its latest picture goes on top of the
//     game's layers as a projection layer, posed with the views it was rendered from.
// D3D11 and D3D12 games for now; others run as before, without the dash.
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <stdio.h>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <shlobj.h>
#include "../../atrium_dash/atrium_dash_shared.h"
#include "dash_overlay.h"

PFN_xrVoidFunction AtriumNext(const char* name);
XRAPI_ATTR XrResult XRAPI_CALL AtriumLayeredGipa(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
void AtriumLog(const char* fmt, ...);

template <typename F> static F NextFn(const char* name) { return (F)AtriumNext(name); }

// The function for calls the dash makes itself: past Atrium's own wrappers, but through the
// Touch swap when it's on (so the dash's pointer and buttons are the CV1 Touches too).
template <typename F> static F Backend(XrInstance instance, const char* name)
{
	PFN_xrVoidFunction f = nullptr;
	AtriumLayeredGipa(instance, name, &f);
	return (F)f;
}
#define BK(inst, fn) Backend<PFN_##fn>(inst, #fn)

static const LONGLONG kHoldMs = 450;

struct DashState
{
	std::mutex lock;
	XrInstance instance = XR_NULL_HANDLE;
	XrSession session = XR_NULL_HANDLE;

	// The game's graphics.
	int api = 0; // 11, 12 or 0 (no dash)
	ID3D11Device* d11 = nullptr;
	ID3D12Device* d12 = nullptr;
	ID3D12CommandQueue* q12 = nullptr;

	// Atrium's actions.
	XrActionSet set = XR_NULL_HANDLE;
	XrAction aim = XR_NULL_HANDLE, grip = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE;
	XrAction stick = XR_NULL_HANDLE, buzz = XR_NULL_HANDLE;
	XrAction button[8] = {}; // one per kButtons entry (the dash button can be any of them)
	XrPath hand[2] = {};
	bool suggested = false; // ours went out with at least one of the game's profiles
	bool attached = false;
	std::set<uint64_t> gameButtonActions[8]; // the game's actions on each kButtons entry
	bool systemButton = true; // the runtime took a binding on the Oculus button
	// The game's button actions and where they're bound (logged when they change: remap checks).
	std::unordered_map<uint64_t, std::string> gameButtonPaths;
	std::unordered_map<uint64_t, bool> loggedState;

	// Controls tab (dash.json), re-read every second.
	int openButton = 0;
	bool openHold = true, pauseGame = false;
	ULONGLONG controlsRead = 0;
	XrSpace local = XR_NULL_HANDLE, aimSpace[2] = {}, gripSpace[2] = {};

	// Menu button: hold to toggle the dash, short presses go through to the game late.
	bool menuDown = false, menuUsed = false;
	ULONGLONG menuSince = 0;
	int passPress = 0; // syncs left to show the game a press
	std::unordered_map<uint64_t, bool> reported;

	// The dash's picture.
	HANDLE map = nullptr;
	AtriumDashShared* shm = nullptr;
	ULONGLONG nextMapTry = 0;
	XrSwapchain swap = XR_NULL_HANDLE;
	uint32_t width = 0, height = 0;
	std::vector<ID3D11Texture2D*> img11;
	std::vector<ID3D12Resource*> img12;
	ID3D11Texture2D* shared11[ATRIUM_DASH_SLOTS] = {};
	ID3D12Resource* shared12[ATRIUM_DASH_SLOTS] = {};
	ID3D12CommandAllocator* alloc[3] = {};
	ID3D12GraphicsCommandList* list = nullptr;
	ID3D12Fence* fence = nullptr;
	UINT64 fenceValue = 0, slotValue[3] = {};
	HANDLE fenceEvent = nullptr;
	int slot = 0;
	bool failed = false; // stop trying after an unrecoverable error
	int lastHaptic = 0;

	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
};
static DashState g_dash;

static bool ShmReady()
{
	DashState& d = g_dash;
	if (d.shm && d.shm->magic == ATRIUM_DASH_MAGIC && d.shm->version == ATRIUM_DASH_VERSION) return true;
	if (d.shm || GetTickCount64() < d.nextMapTry) return false;
	d.nextMapTry = GetTickCount64() + 2000;
	d.map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, ATRIUM_DASH_SHARED);
	if (!d.map) return false;
	d.shm = (AtriumDashShared*)MapViewOfFile(d.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AtriumDashShared));
	return d.shm && d.shm->magic == ATRIUM_DASH_MAGIC && d.shm->version == ATRIUM_DASH_VERSION;
}

static bool DashOpenHere()
{
	return ShmReady() && g_dash.shm->open && g_dash.shm->gamePid == GetCurrentProcessId() && !g_dash.shm->homeActive;
}

// ---- actions --------------------------------------------------------------------------------
static XrPath Path(XrInstance inst, const char* s)
{
	XrPath p = XR_NULL_PATH;
	BK(inst, xrStringToPath)(inst, s, &p);
	return p;
}

static void EnsureActions(XrInstance inst)
{
	DashState& d = g_dash;
	if (d.set || !inst) return;
	d.instance = inst;
	XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(si.actionSetName, "atrium_dash");
	strcpy_s(si.localizedActionSetName, "Atrium dash");
	si.priority = 0;
	if (XR_FAILED(BK(inst, xrCreateActionSet)(inst, &si, &d.set))) { AtriumLog("dash: no action set"); d.set = XR_NULL_HANDLE; return; }
	d.hand[0] = Path(inst, "/user/hand/left");
	d.hand[1] = Path(inst, "/user/hand/right");
	auto make = [&](const char* name, XrActionType type, XrAction& out) {
		XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
		snprintf(ai.actionName, sizeof ai.actionName, "atrium_dash_%s", name);
		snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "Atrium dash %s", name);
		ai.actionType = type;
		ai.countSubactionPaths = 2;
		ai.subactionPaths = d.hand;
		BK(inst, xrCreateAction)(d.set, &ai, &out);
	};
	make("aim", XR_ACTION_TYPE_POSE_INPUT, d.aim);
	make("grip", XR_ACTION_TYPE_POSE_INPUT, d.grip);
	make("trigger", XR_ACTION_TYPE_FLOAT_INPUT, d.trigger);
	make("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, d.squeeze);
	make("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, d.stick);
	for (int i = 0; i < 8; i++) {
		// No hand: each is one button.
		XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
		snprintf(ai.actionName, sizeof ai.actionName, "atrium_dash_button_%d", i);
		snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "Atrium dash button %d", i);
		ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
		BK(inst, xrCreateAction)(d.set, &ai, &d.button[i]);
	}
	make("buzz", XR_ACTION_TYPE_VIBRATION_OUTPUT, d.buzz);
	AtriumLog("dash: actions ready");
}

// The buttons the dash can use and games can have remapped (ids as the dash's Controls tab).
struct ButtonPath { const char* id; const char* base; bool touch; };
static const ButtonPath kButtons[8] = {
	{ "left_menu", "/user/hand/left/input/menu", false }, { "x", "/user/hand/left/input/x", true },
	{ "y", "/user/hand/left/input/y", true }, { "a", "/user/hand/right/input/a", true }, { "b", "/user/hand/right/input/b", true },
	{ "left_stick", "/user/hand/left/input/thumbstick", true }, { "right_stick", "/user/hand/right/input/thumbstick", true },
	// The Oculus button: runtimes may keep it for themselves (Meta's does while its Dash is on).
	{ "oculus", "/user/hand/right/input/system", false },
};
static const int kSystemButton = 7;

static std::string DashJson()
{
	PWSTR roaming = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return {};
	std::wstring path = std::wstring(roaming) + L"\\Atrium\\dash.json";
	CoTaskMemFree(roaming);
	FILE* f = nullptr;
	std::string s;
	if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return s;
	char b[2048];
	size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
	fclose(f);
	return s;
}

// A string value from dash.json ("" when missing).
static std::string DashSetting(const std::string& json, const char* key)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t at = json.find(needle);
	if (at == std::string::npos) return {};
	at = json.find('"', json.find(':', at + needle.size()));
	size_t end = at == std::string::npos ? at : json.find('"', at + 1);
	return end == std::string::npos ? std::string() : json.substr(at + 1, end - at - 1);
}

// The Controls tab's choices, from the dash's shared memory (no file reads on the game's thread).
static bool ShmReady();
static void ReadControls()
{
	DashState& d = g_dash;
	if (!ShmReady()) return;
	d.openButton = d.shm->openButton >= 0 && d.shm->openButton < 8 ? d.shm->openButton : 0;
	d.openHold = d.shm->openHold != 0;
	d.pauseGame = d.shm->pauseGame != 0;
}

// Where a game's binding goes with the Controls tab's remapping ("x=y": pressing X, the game
// gets Y). A game input on button B is pressed by every button remapped to B, and by B itself
// unless B is remapped to something else. Empty: no button gives it any more.
static std::vector<std::string> Physical(const std::string& path, const std::string& remap)
{
	std::vector<std::pair<const ButtonPath*, const ButtonPath*>> pairs;
	for (size_t i = 0; i < remap.size();) {
		size_t end = remap.find(',', i);
		if (end == std::string::npos) end = remap.size();
		std::string pair = remap.substr(i, end - i);
		i = end + 1;
		size_t eq = pair.find('=');
		if (eq == std::string::npos) continue;
		const ButtonPath *from = nullptr, *to = nullptr;
		for (const ButtonPath& k : kButtons) {
			if (pair.substr(0, eq) == k.id) from = &k;
			if (pair.substr(eq + 1) == k.id) to = &k;
		}
		if (from && to && from != to) pairs.push_back({ from, to });
	}
	for (const ButtonPath& btn : kButtons) {
		for (const char* suffix : { "/click", "/touch" }) {
			if (path != std::string(btn.base) + suffix) continue;
			std::vector<std::string> out;
			bool moved = false;
			for (auto& pr : pairs) moved = moved || pr.first == &btn;
			if (!moved) out.push_back(path);
			for (auto& pr : pairs)
				if (pr.second == &btn && (strcmp(suffix, "/click") == 0 || pr.first->touch)) out.push_back(std::string(pr.first->base) + suffix);
			return out;
		}
	}
	return { path };
}

static bool TouchFamily(const char* profile) { return strstr(profile, "touch") != nullptr; }

struct ProfilePaths { const char* profile; const char* trigger; const char* squeeze; const char* stick; const char* menu; };
static const ProfilePaths kProfiles[] = {
	{ "/interaction_profiles/oculus/touch_controller", "/input/trigger/value", "/input/squeeze/value", "/input/thumbstick", "/user/hand/left/input/menu/click" },
	{ "/interaction_profiles/meta/touch_controller_plus", "/input/trigger/value", "/input/squeeze/value", "/input/thumbstick", "/user/hand/left/input/menu/click" },
	{ "/interaction_profiles/meta/touch_pro_controller", "/input/trigger/value", "/input/squeeze/value", "/input/thumbstick", "/user/hand/left/input/menu/click" },
	{ "/interaction_profiles/valve/index_controller", "/input/trigger/value", "/input/squeeze/value", "/input/thumbstick", "/user/hand/left/input/b/click" },
	{ "/interaction_profiles/microsoft/motion_controller", "/input/trigger/value", "/input/squeeze/click", "/input/thumbstick", "/user/hand/left/input/menu/click" },
	{ "/interaction_profiles/htc/vive_controller", "/input/trigger/value", "/input/squeeze/click", nullptr, "/user/hand/left/input/menu/click" },
	{ "/interaction_profiles/khr/simple_controller", "/input/select/click", nullptr, nullptr, "/user/hand/left/input/menu/click" },
};

static const ProfilePaths* FindProfile(XrInstance inst, XrPath profile)
{
	for (const ProfilePaths& p : kProfiles)
		if (Path(inst, p.profile) == profile) return &p;
	return nullptr;
}

static void AddOurBindings(XrInstance inst, const ProfilePaths& p, std::vector<XrActionSuggestedBinding>& out)
{
	DashState& d = g_dash;
	for (const char* side : { "/user/hand/left", "/user/hand/right" }) {
		std::string s(side);
		out.push_back({ d.aim, Path(inst, (s + "/input/aim/pose").c_str()) });
		out.push_back({ d.grip, Path(inst, (s + "/input/grip/pose").c_str()) });
		out.push_back({ d.trigger, Path(inst, (s + p.trigger).c_str()) });
		if (p.squeeze) out.push_back({ d.squeeze, Path(inst, (s + p.squeeze).c_str()) });
		if (p.stick) out.push_back({ d.stick, Path(inst, (s + p.stick).c_str()) });
		out.push_back({ d.buzz, Path(inst, (s + "/output/haptic").c_str()) });
	}
	// The dash button candidates: all of them on Touch, the menu button elsewhere.
	if (TouchFamily(p.profile)) {
		for (int i = 0; i < 8; i++) {
			if (i == kSystemButton && !d.systemButton) continue;
			out.push_back({ d.button[i], Path(inst, (std::string(kButtons[i].base) + "/click").c_str()) });
		}
	} else {
		out.push_back({ d.button[0], Path(inst, p.menu) });
	}
}

XRAPI_ATTR XrResult XRAPI_CALL Dash_xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* b)
{
	auto next = NextFn<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");
	std::lock_guard<std::mutex> lk(g_dash.lock);
	EnsureActions(instance);
	const ProfilePaths* p = b ? FindProfile(instance, b->interactionProfile) : nullptr;
	if (!p || !g_dash.set || g_dash.attached) return next(instance, b);
	// The game's Touch bindings, remapped as the Controls tab says.
	std::vector<XrActionSuggestedBinding> all;
	std::string remap = TouchFamily(p->profile) ? DashSetting(DashJson(), "remap") : std::string();
	for (uint32_t i = 0; i < b->countSuggestedBindings; i++) {
		XrActionSuggestedBinding sb = b->suggestedBindings[i];
		if (!remap.empty()) {
			char buf[XR_MAX_PATH_LENGTH];
			uint32_t n = 0;
			if (XR_SUCCEEDED(BK(instance, xrPathToString)(instance, sb.binding, sizeof buf, &n, buf))) {
				for (const std::string& to : Physical(buf, remap)) {
					XrActionSuggestedBinding moved = sb;
					moved.binding = Path(instance, to.c_str());
					all.push_back(moved);
				}
				continue;
			}
		}
		all.push_back(sb);
	}
	// Remember where the game's button actions are bound, for the log.
	for (const XrActionSuggestedBinding& sb : all) {
		char buf[XR_MAX_PATH_LENGTH];
		uint32_t n = 0;
		if (XR_SUCCEEDED(BK(instance, xrPathToString)(instance, sb.binding, sizeof buf, &n, buf)) && strstr(buf, "/click")) {
			std::string& s = g_dash.gameButtonPaths[(uint64_t)sb.action];
			s += (s.empty() ? "" : " ") + std::string(buf + strlen("/user/hand/"));
		}
	}
	// Remember the game's actions on each dash-button candidate: filtered while it's the dash's.
	for (const XrActionSuggestedBinding& sb : all)
		for (int k = 0; k < 8; k++)
			if (sb.binding == Path(instance, (std::string(kButtons[k].base) + "/click").c_str()) ||
				(k == 0 && sb.binding == Path(instance, p->menu)))
				g_dash.gameButtonActions[k].insert((uint64_t)sb.action);
	size_t gameCount = all.size();
	AddOurBindings(instance, *p, all);
	XrInteractionProfileSuggestedBinding merged = *b;
	merged.countSuggestedBindings = (uint32_t)all.size();
	merged.suggestedBindings = all.data();
	XrResult r = next(instance, &merged);
	if (XR_FAILED(r) && TouchFamily(p->profile) && g_dash.systemButton) {
		// The runtime keeps the Oculus button: everything else without it.
		AtriumLog("dash: %s: the runtime keeps the Oculus button (%d)", p->profile, (int)r);
		g_dash.systemButton = false;
		all.resize(gameCount);
		AddOurBindings(instance, *p, all);
		merged.countSuggestedBindings = (uint32_t)all.size();
		merged.suggestedBindings = all.data();
		r = next(instance, &merged);
	}
	if (XR_FAILED(r)) {
		AtriumLog("dash: bindings for %s refused (%d); the game's alone", p->profile, (int)r);
		merged.countSuggestedBindings = (uint32_t)gameCount;
		return next(instance, &merged);
	}
	if (!remap.empty()) AtriumLog("dash: %s remapped (%s)", p->profile, remap.c_str());
	g_dash.suggested = true;
	return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Dash_xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info)
{
	auto next = NextFn<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets");
	std::lock_guard<std::mutex> lk(g_dash.lock);
	DashState& d = g_dash;
	if (!d.set || session != d.session || !info) return next(session, info);
	if (!d.suggested) {
		// The game bound no controller profile Atrium knows: suggest Touch and simple ourselves.
		for (const ProfilePaths& p : kProfiles) {
			if (strstr(p.profile, "oculus/touch") == nullptr && strstr(p.profile, "khr/simple") == nullptr) continue;
			std::vector<XrActionSuggestedBinding> ours;
			AddOurBindings(d.instance, p, ours);
			XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
			s.interactionProfile = Path(d.instance, p.profile);
			s.countSuggestedBindings = (uint32_t)ours.size();
			s.suggestedBindings = ours.data();
			BK(d.instance, xrSuggestInteractionProfileBindings)(d.instance, &s);
		}
	}
	std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
	sets.push_back(d.set);
	XrSessionActionSetsAttachInfo ai = *info;
	ai.countActionSets = (uint32_t)sets.size();
	ai.actionSets = sets.data();
	XrResult r = next(session, &ai);
	if (XR_FAILED(r)) {
		AtriumLog("dash: attaching Atrium's actions failed (%d); the game's alone", (int)r);
		return next(session, info);
	}
	d.attached = true;
	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo si = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		si.poseInActionSpace.orientation.w = 1;
		si.subactionPath = d.hand[h];
		si.action = d.aim;
		BK(d.instance, xrCreateActionSpace)(session, &si, &d.aimSpace[h]);
		si.action = d.grip;
		BK(d.instance, xrCreateActionSpace)(session, &si, &d.gripSpace[h]);
	}
	AtriumLog("dash: Atrium's actions attached (%u of the game's sets)", info->countActionSets);
	return r;
}

static void Toggle()
{
	DashState& d = g_dash;
	if (!ShmReady()) { AtriumLog("dash: atrium-dash isn't running"); return; }
	bool open = !(d.shm->open && d.shm->gamePid == GetCurrentProcessId());
	if (d.shm->homeActive) return;
	d.shm->gamePid = GetCurrentProcessId();
	d.shm->open = open ? 1 : 0;
	AtriumLog("dash: %s", open ? "open" : "closed");
}

XRAPI_ATTR XrResult XRAPI_CALL Dash_xrSyncActions(XrSession session, const XrActionsSyncInfo* info)
{
	auto next = NextFn<PFN_xrSyncActions>("xrSyncActions");
	DashState& d = g_dash;
	if (!d.attached || session != d.session || !info) return next(session, info);
	if (ShmReady() && d.shm->activeGamePid != GetCurrentProcessId()) d.shm->activeGamePid = GetCurrentProcessId();
	bool open = DashOpenHere();
	// Open: only Atrium's set, so the game's actions read inactive (the game gets no input).
	std::vector<XrActiveActionSet> sets;
	if (!open) sets.assign(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
	sets.push_back({ d.set, XR_NULL_PATH });
	XrActionsSyncInfo si = *info;
	si.countActiveActionSets = (uint32_t)sets.size();
	si.activeActionSets = sets.data();
	XrResult r = next(session, &si);
	if (XR_FAILED(r)) return r;

	ReadControls();
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = d.button[d.openButton];
	XrActionStateBoolean m = { XR_TYPE_ACTION_STATE_BOOLEAN };
	BK(d.instance, xrGetActionStateBoolean)(session, &gi, &m);
	ULONGLONG now = GetTickCount64();
	if (d.passPress > 0) d.passPress--;
	if (!d.openHold) {
		// Pressing: every press toggles; the game never sees the button.
		if (m.currentState && !d.menuDown) {
			Toggle();
			d.lastHaptic = -1;
		}
		d.menuDown = m.currentState != XR_FALSE;
		return r;
	}
	if (m.currentState && !d.menuDown) {
		d.menuDown = true;
		d.menuUsed = false;
		d.menuSince = now;
	} else if (m.currentState && !d.menuUsed && now - d.menuSince >= (ULONGLONG)kHoldMs) {
		d.menuUsed = true;
		Toggle();
		d.lastHaptic = -1; // buzz below
	} else if (!m.currentState && d.menuDown) {
		d.menuDown = false;
		if (!d.menuUsed && !open) d.passPress = 3; // a short press: the game's after all
	}
	return r;
}

XRAPI_ATTR XrResult XRAPI_CALL Dash_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* state)
{
	XrResult r = NextFn<PFN_xrGetActionStateBoolean>("xrGetActionStateBoolean")(session, info, state);
	DashState& d = g_dash;
	if (XR_SUCCEEDED(r) && info) {
		// Which button the game sees pressed (bindings after remapping), on each change.
		auto it = d.gameButtonPaths.find((uint64_t)info->action);
		if (it != d.gameButtonPaths.end()) {
			bool& last = d.loggedState[(uint64_t)info->action ^ (uint64_t)info->subactionPath];
			if ((state->currentState != XR_FALSE) != last) {
				last = state->currentState != XR_FALSE;
				char hand[XR_MAX_PATH_LENGTH] = "any hand";
				uint32_t n = 0;
				if (info->subactionPath) BK(d.instance, xrPathToString)(d.instance, info->subactionPath, sizeof hand, &n, hand);
				AtriumLog("input: game action on [%s] for %s: %s", it->second.c_str(), hand, last ? "pressed" : "released");
			}
		}
	}
	if (XR_FAILED(r) || !info || session != d.session || !d.gameButtonActions[d.openButton].count((uint64_t)info->action)) return r;
	// The game's action on the dash button: pressed only for a late short press (held mode).
	bool pressed = d.openHold && d.passPress > 0;
	bool& last = d.reported[(uint64_t)info->action];
	state->changedSinceLastSync = pressed != last;
	state->currentState = pressed;
	last = pressed;
	return r;
}

// ---- session and picture --------------------------------------------------------------------
void DashSessionCreated(XrInstance instance, const XrSessionCreateInfo* info, XrSession session)
{
	std::lock_guard<std::mutex> lk(g_dash.lock);
	DashState& d = g_dash;
	d.instance = instance;
	d.session = session;
	EnsureActions(instance);
	for (const XrBaseInStructure* b = (const XrBaseInStructure*)info->next; b; b = b->next) {
		if (b->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
			d.api = 11;
			d.d11 = ((const XrGraphicsBindingD3D11KHR*)b)->device;
			d.d11->AddRef();
		} else if (b->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
			d.api = 12;
			d.d12 = ((const XrGraphicsBindingD3D12KHR*)b)->device;
			d.q12 = ((const XrGraphicsBindingD3D12KHR*)b)->queue;
			d.d12->AddRef();
			d.q12->AddRef();
		}
	}
	XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rs.poseInReferenceSpace.orientation.w = 1;
	BK(instance, xrCreateReferenceSpace)(session, &rs, &d.local);
	AtriumLog("dash: ready in this game (%s)", d.api == 11 ? "D3D11" : d.api == 12 ? "D3D12" : "graphics API not supported yet: no dash");
}

static void ReleaseImages()
{
	DashState& d = g_dash;
	if (d.fence && d.fenceValue && d.fence->GetCompletedValue() < d.fenceValue) {
		d.fence->SetEventOnCompletion(d.fenceValue, d.fenceEvent);
		WaitForSingleObject(d.fenceEvent, 1000);
	}
	if (d.swap) BK(d.instance, xrDestroySwapchain)(d.swap);
	d.swap = XR_NULL_HANDLE;
	d.img11.clear();
	d.img12.clear();
	for (int s = 0; s < ATRIUM_DASH_SLOTS; s++) {
		if (d.shared11[s]) d.shared11[s]->Release();
		if (d.shared12[s]) d.shared12[s]->Release();
		d.shared11[s] = nullptr;
		d.shared12[s] = nullptr;
	}
	for (auto& a : d.alloc) { if (a) a->Release(); a = nullptr; }
	if (d.list) d.list->Release();
	if (d.fence) d.fence->Release();
	d.list = nullptr;
	d.fence = nullptr;
	d.fenceValue = 0;
}

void DashSessionDestroying(XrSession session)
{
	std::lock_guard<std::mutex> lk(g_dash.lock);
	DashState& d = g_dash;
	if (session != d.session) return;
	ReleaseImages();
	if (d.shm && d.shm->gamePid == GetCurrentProcessId()) { d.shm->open = 0; d.shm->gamePid = 0; }
	if (d.shm && d.shm->activeGamePid == GetCurrentProcessId()) d.shm->activeGamePid = 0;
	if (d.d11) d.d11->Release();
	if (d.d12) d.d12->Release();
	if (d.q12) d.q12->Release();
	d.d11 = nullptr; d.d12 = nullptr; d.q12 = nullptr;
	d.session = XR_NULL_HANDLE;
	d.attached = false;
	d.api = 0;
}

static bool EnsurePicture(XrSession session)
{
	DashState& d = g_dash;
	if (d.swap) return true;
	if (d.failed || !d.api) return false;
	d.width = d.shm->eyeWidth * 2;
	d.height = d.shm->eyeHeight;

	uint32_t n = 0;
	BK(d.instance, xrEnumerateSwapchainFormats)(session, 0, &n, nullptr);
	std::vector<int64_t> formats(n);
	BK(d.instance, xrEnumerateSwapchainFormats)(session, n, &n, formats.data());
	int64_t fmt = 0;
	for (int64_t f : formats) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) fmt = f;
	if (!fmt) for (int64_t f : formats) if (f == DXGI_FORMAT_R8G8B8A8_UNORM) fmt = f;
	if (!fmt) { AtriumLog("dash: no RGBA8 swapchain format"); d.failed = true; return false; }
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = fmt;
	ci.width = d.width; ci.height = d.height;
	ci.sampleCount = 1; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
	XrResult r = BK(d.instance, xrCreateSwapchain)(session, &ci, &d.swap);
	if (XR_FAILED(r)) { AtriumLog("dash: swapchain failed (%d)", (int)r); d.failed = true; return false; }
	BK(d.instance, xrEnumerateSwapchainImages)(d.swap, 0, &n, nullptr);
	if (d.api == 11) {
		std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		BK(d.instance, xrEnumerateSwapchainImages)(d.swap, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
		for (auto& i : imgs) d.img11.push_back(i.texture);
		ID3D11Device1* d1 = nullptr;
		if (SUCCEEDED(d.d11->QueryInterface(&d1))) {
			for (int s = 0; s < ATRIUM_DASH_SLOTS; s++) {
				wchar_t name[64];
				swprintf_s(name, L"%s%d", ATRIUM_DASH_TEXTURE, s);
				d1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&d.shared11[s]));
			}
			d1->Release();
		}
		if (!d.shared11[ATRIUM_DASH_SLOTS - 1]) { AtriumLog("dash: couldn't open the dash's pictures (D3D11)"); ReleaseImages(); return false; }
	} else {
		std::vector<XrSwapchainImageD3D12KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
		BK(d.instance, xrEnumerateSwapchainImages)(d.swap, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
		for (auto& i : imgs) d.img12.push_back(i.texture);
		for (int s = 0; s < ATRIUM_DASH_SLOTS; s++) {
			wchar_t name[64];
			swprintf_s(name, L"%s%d", ATRIUM_DASH_TEXTURE, s);
			HANDLE h = nullptr;
			if (SUCCEEDED(d.d12->OpenSharedHandleByName(name, GENERIC_ALL, &h))) {
				d.d12->OpenSharedHandle(h, IID_PPV_ARGS(&d.shared12[s]));
				CloseHandle(h);
			}
		}
		if (!d.shared12[ATRIUM_DASH_SLOTS - 1]) { AtriumLog("dash: couldn't open the dash's pictures (D3D12)"); ReleaseImages(); return false; }
		for (auto& a : d.alloc) d.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
		d.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc[0], nullptr, IID_PPV_ARGS(&d.list));
		d.list->Close();
		d.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence));
		if (!d.fenceEvent) d.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	}
	AtriumLog("dash: picture %ux%u, format %lld, %u images", d.width, d.height, (long long)fmt, n);
	return true;
}

static void CopyPicture(uint32_t index, int src)
{
	DashState& d = g_dash;
	if (d.api == 11) {
		ID3D11DeviceContext* ctx = nullptr;
		d.d11->GetImmediateContext(&ctx);
		ctx->CopyResource(d.img11[index], d.shared11[src]);
		ctx->Release();
		return;
	}
	int s = d.slot;
	d.slot = (d.slot + 1) % 3;
	if (d.slotValue[s] && d.fence->GetCompletedValue() < d.slotValue[s]) {
		d.fence->SetEventOnCompletion(d.slotValue[s], d.fenceEvent);
		WaitForSingleObject(d.fenceEvent, 100);
	}
	d.alloc[s]->Reset();
	d.list->Reset(d.alloc[s], nullptr);
	D3D12_RESOURCE_BARRIER b[2] = {};
	b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b[0].Transition.pResource = d.img12[index];
	b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	b[1].Transition.pResource = d.shared12[src];
	b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	d.list->ResourceBarrier(2, b);
	d.list->CopyResource(d.img12[index], d.shared12[src]);
	std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
	std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
	d.list->ResourceBarrier(2, b);
	d.list->Close();
	ID3D12CommandList* lists[] = { d.list };
	d.q12->ExecuteCommandLists(1, lists);
	d.q12->Signal(d.fence, ++d.fenceValue);
	d.slotValue[s] = d.fenceValue;
}

static AtriumDashPose Raw(const XrPosef& p)
{
	return { p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
}

static XrPosef Xr(const AtriumDashPose& p)
{
	XrPosef o;
	o.position = { p.px, p.py, p.pz };
	o.orientation = { p.qx, p.qy, p.qz, p.qw };
	return o;
}

bool DashEndFrame(XrSession session, const XrFrameEndInfo* info, std::vector<const XrCompositionLayerBaseHeader*>& layers)
{
	std::lock_guard<std::mutex> lk(g_dash.lock);
	DashState& d = g_dash;
	if (session != d.session || !d.attached || !DashOpenHere()) return false;
	{
		// How fast the game's frames come while the dash is open (stutter diagnostics).
		static LARGE_INTEGER freq, last, since;
		static double worst;
		static int count;
		LARGE_INTEGER now;
		if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&now);
		if (last.QuadPart) {
			double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
			if (ms > worst) worst = ms;
			count++;
			if ((now.QuadPart - since.QuadPart) > freq.QuadPart * 2) {
				AtriumLog("dash: game at %.0f fps with the dash open, worst frame %.1f ms, dash pictures %d", count * 1.0 * freq.QuadPart / (now.QuadPart - since.QuadPart),
					worst, d.shm->frameSeq);
				since = now;
				worst = 0;
				count = 0;
			}
		} else {
			since = now;
		}
		last = now;
	}
	if (!EnsurePicture(session)) return false;
	XrInstance inst = d.instance;

	// Tell atrium-dash where the eyes and hands are for this frame.
	XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
	vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	vli.displayTime = info->displayTime;
	vli.space = d.local;
	XrViewState vs = { XR_TYPE_VIEW_STATE };
	XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	uint32_t nv = 0;
	BK(inst, xrLocateViews)(session, &vli, &vs, 2, &nv, views);
	AtriumDashInput in = {};
	for (int e = 0; e < 2 && e < (int)nv; e++) {
		in.eye[e] = Raw(views[e].pose);
		in.fov[e] = { views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown };
	}
	in.valid = nv == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
	for (int h = 0; h < 2; h++) {
		XrSpaceLocation la = { XR_TYPE_SPACE_LOCATION }, lg = { XR_TYPE_SPACE_LOCATION };
		BK(inst, xrLocateSpace)(d.aimSpace[h], d.local, info->displayTime, &la);
		BK(inst, xrLocateSpace)(d.gripSpace[h], d.local, info->displayTime, &lg);
		in.handValid[h] = (la.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (la.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
		in.aim[h] = Raw(la.pose);
		in.grip[h] = Raw(lg.pose);
		XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
		gi.subactionPath = d.hand[h];
		XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT };
		gi.action = d.trigger;
		BK(inst, xrGetActionStateFloat)(session, &gi, &f);
		in.trigger[h] = f.currentState;
		gi.action = d.squeeze;
		f = { XR_TYPE_ACTION_STATE_FLOAT };
		BK(inst, xrGetActionStateFloat)(session, &gi, &f);
		in.squeeze[h] = f.currentState;
		XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
		gi.action = d.stick;
		BK(inst, xrGetActionStateVector2f)(session, &gi, &v);
		in.thumbY[h] = v.currentState.y;
	}
	d.shm->input = in;
	MemoryBarrier();
	d.shm->inputSeq++;

	// A tick on the controller when the dash asks (hover, click) or it just opened.
	if (d.lastHaptic != d.shm->hapticSeq) {
		XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
		hi.action = d.buzz;
		hi.subactionPath = d.hand[d.lastHaptic == -1 ? 0 : (d.shm->hapticHand & 1)];
		XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
		vib.duration = d.lastHaptic == -1 ? 40000000 : 12000000;
		vib.frequency = XR_FREQUENCY_UNSPECIFIED;
		vib.amplitude = d.lastHaptic == -1 ? 0.6f : 0.25f;
		BK(inst, xrApplyHapticFeedback)(session, &hi, (XrHapticBaseHeader*)&vib);
		d.lastHaptic = d.shm->hapticSeq;
	}

	// Nothing drawn yet: the game's frame as it is.
	if (d.shm->frameSeq == 0) return false;
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (XR_FAILED(BK(inst, xrAcquireSwapchainImage)(d.swap, &ai, &index))) return false;
	XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wi.timeout = 100000000;
	BK(inst, xrWaitSwapchainImage)(d.swap, &wi);
	// The latest finished picture and the views it was drawn for (read together: the dash
	// publishes a slot only after filling its views).
	int slot = d.shm->latestSlot % ATRIUM_DASH_SLOTS;
	MemoryBarrier();
	AtriumDashPose eyes[2] = { d.shm->slotEye[slot][0], d.shm->slotEye[slot][1] };
	AtriumDashFov fovs[2] = { d.shm->slotFov[slot][0], d.shm->slotFov[slot][1] };
	CopyPicture(index, slot);
	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	BK(inst, xrReleaseSwapchainImage)(d.swap, &ri);

	for (int e = 0; e < 2; e++) {
		d.views[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		d.views[e].pose = Xr(eyes[e]);
		const AtriumDashFov& f = fovs[e];
		d.views[e].fov = { f.left, f.right, f.up, f.down };
		d.views[e].subImage.swapchain = d.swap;
		d.views[e].subImage.imageRect = { { (int32_t)(e * d.width / 2), 0 }, { (int32_t)(d.width / 2), (int32_t)d.height } };
	}
	d.layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	d.layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	d.layer.space = d.local;
	d.layer.viewCount = 2;
	d.layer.views = d.views;
	layers.assign(info->layers, info->layers + info->layerCount);
	layers.push_back((const XrCompositionLayerBaseHeader*)&d.layer);
	return true;
}

// ---- focus: the game is VISIBLE (not FOCUSED) while the dash is open ------------------------
static XrSessionState g_realState = XR_SESSION_STATE_UNKNOWN, g_toldState = XR_SESSION_STATE_UNKNOWN;

static bool StateEvent(XrEventDataBuffer* ev, XrSessionState state)
{
	auto* sc = (XrEventDataSessionStateChanged*)ev;
	*sc = { XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
	sc->session = g_dash.session;
	sc->state = state;


	sc->time = 0;
	g_toldState = state;
	return true;
}

XRAPI_ATTR XrResult XRAPI_CALL Dash_xrPollEvent(XrInstance instance, XrEventDataBuffer* ev)
{
	auto next = NextFn<PFN_xrPollEvent>("xrPollEvent");
	if (g_dash.session && g_realState == XR_SESSION_STATE_FOCUSED) {
		ReadControls();
		bool open = DashOpenHere() && g_dash.pauseGame;
		if (open && g_toldState == XR_SESSION_STATE_FOCUSED) { StateEvent(ev, XR_SESSION_STATE_VISIBLE); AtriumLog("dash: game told VISIBLE"); return XR_SUCCESS; }
		if (!open && g_toldState == XR_SESSION_STATE_VISIBLE) { StateEvent(ev, XR_SESSION_STATE_FOCUSED); AtriumLog("dash: game told FOCUSED"); return XR_SUCCESS; }
	}
	XrResult r = next(instance, ev);
	if (r == XR_SUCCESS && ev->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
		auto* sc = (XrEventDataSessionStateChanged*)ev;
		if (sc->session == g_dash.session) {
			g_realState = sc->state;
			if (sc->state == XR_SESSION_STATE_FOCUSED && DashOpenHere() && g_dash.pauseGame) sc->state = XR_SESSION_STATE_VISIBLE;
			g_toldState = sc->state;
		}
	}
	return r;
}
