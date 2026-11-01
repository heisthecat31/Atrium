// TouchLink: Rift CV1 Touches as SteamVR controllers next to any other headset (Valve Index, ...),
// in every SteamVR game.
//
// Base stations can't see a Touch (it has IR LEDs for cameras, no photodiodes for the lasers), so:
//   - Atrium's tracking service (Monado with the Rift drivers, no compositor) tracks the Touches
//     with the Rift sensors; the Rift headset only needs USB, for its radio. No Meta software.
//   - A one-time calibration (calibration.cpp) maps the sensors' space onto SteamVR's raw space:
//     until there is one, hold a Touch against the side of the headset and look around until it
//     buzzes (X+Y+A+B does it again).
//   - Each Touch is an "oculus_touch" controller (Valve's own Touch input profile and CV1 render
//     model, already in SteamVR), so games' Touch bindings apply as with a real Rift.
//
// Files in %APPDATA%\TouchLink: config.json (touchlink.exe install), calibration.json,
// status.json (written every second; touchlink status / Atrium read it), driver.log,
// service.log, calibrate (create it to start a calibration).
#include "openvr_driver.h"

#include "calibration.h"
#include "source.h"
#include "meta_shared.h"

#include <tlhelp32.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <algorithm>
#include <atomic>
#include <thread>

namespace tl {
namespace {

FILE* g_logFile = nullptr;
std::mutex g_logLock;

void Log(const char* fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (vr::VRDriverLog()) {
		std::string line = std::string("TouchLink: ") + msg + "\n";
		vr::VRDriverLog()->Log(line.c_str());
	}
	std::lock_guard<std::mutex> lk(g_logLock);
	if (g_logFile) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(g_logFile, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
		fflush(g_logFile);
	}
}

vr::HmdQuaternion_t ToHmd(const XrQuaternionf& q) { return { q.w, q.x, q.y, q.z }; }

// The Touches' buttons with Meta's source: the Oculus button opens SteamVR's dashboard (and a 1 s
// hold recentres), the menu button is the game's menu. Valve's Touch profile has no menu button for
// games: the menu button is its left system button, and SteamVR keeps system buttons to itself, so
// games never got it (Echo VR asks for /input/menu, which that profile doesn't have; 2026-11-01). The
// controller type must stay oculus_touch, or games' Touch bindings don't apply (they got no input at
// all as touchlink_touch). So TouchLink brings Valve's oculus_touch profile itself, from this
// SteamVR's files, with /input/menu on the left and the system button (the Oculus button) on the
// right, and dashboard bindings to match; for SteamVR to take it, its own Oculus driver (which brings
// Valve's profile, and isn't needed with another headset) is switched off, from the next start.
enum class Buttons { Valve, Ours };
Buttons g_buttons = Buttons::Valve;
// With Valve's profile: TouchLink's dashboard binding is picked, so the dashboard is on the right
// system button (the Oculus button) and not the left one.
bool g_dashboardOnRight = false;

// A file:/// URL as SteamVR writes binding choices.
static std::string FileUrl(const std::wstring& file)
{
	std::string url = "file:///";
	for (unsigned char c : Narrow(file)) {
		if (c == '\\') url += '/';
		else if (isalnum(c) || strchr("-._~/:", c)) url += (char)c;
		else {
			char hex[4];
			snprintf(hex, sizeof hex, "%%%02X", c);
			url += hex;
		}
	}
	return url;
}
const char* const kDashboardSection = "openvr.component.vrcompositor";
const char* const kDashboardKey = "oculus_touch_250820_CurrentURL_steamvrinput";

// Copies every file of one folder into another (made if missing). False if nothing could be copied.
static bool CopyFolder(const std::wstring& from, const std::wstring& to)
{
	CreateDirectoryW(to.c_str(), nullptr);
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return false;
	bool any = false;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		any |= CopyFileW((from + L"\\" + fd.cFileName).c_str(), (to + L"\\" + fd.cFileName).c_str(), FALSE) != 0;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return any;
}

// Replaces the one `a` in s with `b`; false if s has none.
static bool ReplaceOne(std::string& s, const std::string& a, const std::string& b)
{
	size_t at = s.find(a);
	if (at == std::string::npos) return false;
	s.replace(at, a.size(), b);
	return true;
}

static void ReplaceAll(std::string& s, const std::string& a, const std::string& b)
{
	for (size_t at = 0; (at = s.find(a, at)) != std::string::npos; at += b.size()) s.replace(at, a.size(), b);
}

static bool SwapAll(std::string& s, const std::string& a, const std::string& b)
{
	const std::string mark = "\x01touchlink\x01";
	if (s.find(a) == std::string::npos || s.find(b) == std::string::npos) return false;
	ReplaceAll(s, a, mark);
	ReplaceAll(s, b, a);
	ReplaceAll(s, mark, b);
	return true;
}

// Earlier builds picked bindings for games (and the dashboard) in steamvr.vrsettings; with this
// profile none is needed: each game gets its own choice back (a workshop binding, or SteamVR's
// default), the dashboard its default (ours).
static void UndoBindingChoices(const std::wstring& data)
{
	auto* settings = vr::VRSettings();
	vr::EVRSettingsError err;
	auto ours = FileUrl;
	char current[1024];
	auto undo = [&](const std::string& section, const char* key, const std::wstring& file, const std::string& back) {
		current[0] = 0;
		settings->GetString(section.c_str(), key, current, sizeof current, &err);
		if (ours(file) != current) return;
		if (back.empty()) settings->RemoveKeyInSection(section.c_str(), key, &err);
		else settings->SetString(section.c_str(), key, back.c_str(), &err);
		Log("binding choice undone: %s %s", section.c_str(), back.empty() ? "(SteamVR's default)" : back.c_str());
	};
	undo(kDashboardSection, kDashboardKey, data + L"\\dashboard_touch.json", "");
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((data + L"\\bindings\\*.json").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		std::wstring name = fd.cFileName;
		size_t w = name.find(L"_workshop_"), o = name.find(L"_oculus_touch_");
		size_t cut = w != std::wstring::npos ? w : o;
		if (cut == std::wstring::npos) continue;
		std::string back = w != std::wstring::npos ? "vr-input-workshop://" + Narrow(name.substr(w + 10, name.size() - w - 10 - 5)) : "";
		undo(Narrow(name.substr(0, cut)), "oculus_touch_250820_CurrentURL_openxr", data + L"\\bindings\\" + name, back);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

static void SetUpButtons(bool meta)
{
	auto* settings = vr::VRSettings();
	vr::EVRSettingsError err;
	const std::wstring marker = DataFile(L"oculus_driver_off");
	if (!meta) {
		// Atrium's service: Valve's profile (no Oculus button there), and SteamVR's Oculus driver back if we turned it off.
		if (FileExists(marker)) {
			settings->SetBool("driver_oculus", "enable", true, &err);
			settings->SetBool("driver_oculus_legacy", "enable", true, &err);
			DeleteFileW(marker.c_str());
			Log("buttons: SteamVR's Oculus driver is on again (from SteamVR's next start)");
		}
		UndoBindingChoices(DataDir());
		return;
	}
	// SteamVR's Oculus drivers off from the next start; this one already has them if they're loaded.
	const bool loaded = GetModuleHandleW(L"driver_oculus.dll") || GetModuleHandleW(L"driver_oculus_legacy.dll");
	if (settings->GetBool("driver_oculus", "enable", &err) || err != vr::VRSettingsError_None ||
		settings->GetBool("driver_oculus_legacy", "enable", &err) || err != vr::VRSettingsError_None) {
		settings->SetBool("driver_oculus", "enable", false, &err);
		settings->SetBool("driver_oculus_legacy", "enable", false, &err);
		WriteFileText(marker, "SteamVR's Oculus drivers were switched off by TouchLink (its Meta source)\n");
		Log("buttons: SteamVR's Oculus driver switched off (from SteamVR's next start)");
	}
	// Valve's profile and the files it uses, from this SteamVR, into our resources.
	wchar_t exe[MAX_PATH], self[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);   // ...\SteamVR\bin\win64\vrserver.exe
	std::wstring steamVr = exe;
	for (int up = 0; up < 3; up++) steamVr = steamVr.substr(0, steamVr.find_last_of(L'\\'));
	HMODULE mod = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Log, &mod);
	GetModuleFileNameW(mod, self, MAX_PATH);
	std::wstring driver = self;
	for (int up = 0; up < 3; up++) driver = driver.substr(0, driver.find_last_of(L'\\'));   // bin\win64\dll -> the driver folder
	const std::wstring valve = steamVr + L"\\drivers\\oculus\\resources", res = driver + L"\\resources";
	CreateDirectoryW(res.c_str(), nullptr);
	std::string profile, dashboard;
	bool ok = CopyFolder(valve + L"\\icons", res + L"\\icons") && CopyFolder(valve + L"\\input", res + L"\\input") &&
		ReadFileText(res + L"\\input\\touch_profile.json", &profile) && ReadFileText(res + L"\\input\\vrcompositor_bindings_touch.json", &dashboard);
	if (ok) {
		ReplaceAll(profile, "{oculus}", "{touchlink}");
		ok = ReplaceOne(profile, "\"resource_root\" : \"oculus\"", "\"resource_root\" : \"touchlink\"") &&
			ReplaceOne(profile, "\"driver_name\" : \"oculus\"", "\"driver_name\" : \"touchlink\"");
		// Found after the renames above: they move everything below them.
		size_t sys = profile.find("\"/input/system\" : {");
		size_t side = sys == std::string::npos ? sys : profile.find("\"side\" : \"left\",", sys);
		ok = ok && side != std::string::npos && side < profile.find('}', sys);
		if (ok) {
			profile.replace(side, strlen("\"side\" : \"left\","), "\"side\" : \"right\",");   // the Oculus button
			profile.insert(sys, "\"/input/menu\" : {\n        \"type\" : \"button\",\n        \"side\" : \"left\",\n"
				"        \"binding_image_point\" : [ 72, 39 ],\n        \"click\" : true,\n        \"touch\" : false,\n        \"order\" : 0\n    },\n    ");
		}
		// The dashboard: Valve's, on the right system button (it was on the left), and holding it 1 s
		// recentres, as the Index controllers' dashboard bindings have it.
		ok = ok && SwapAll(dashboard, "/user/hand/left/input/system", "/user/hand/right/input/system") &&
			ReplaceOne(dashboard, "\"bindings\": {", "\"bindings\": {\n\t\t\"/actions/quickrecenter\": {\n\t\t\t\"sources\": [\n\t\t\t\t{\n"
				"\t\t\t\t\t\"inputs\": { \"long\": { \"output\": \"/actions/quickrecenter/in/Recenter\" } },\n\t\t\t\t\t\"mode\": \"button\",\n"
				"\t\t\t\t\t\"parameters\": { \"long_press_delay\": 1, \"long_press_expiry\": 3 },\n"
				"\t\t\t\t\t\"path\": \"/user/hand/right/input/system\"\n\t\t\t\t}\n\t\t\t]\n\t\t},") &&
			WriteFileText(res + L"\\input\\touch_profile.json", profile) && WriteFileText(res + L"\\input\\vrcompositor_bindings_touch.json", dashboard);
	}
	if (!ok) {
		Log("buttons: SteamVR's Touch files in %s couldn't be made over: Valve's buttons (the menu button opens the dashboard)", Narrow(valve).c_str());
		UndoBindingChoices(DataDir());   // a dashboard binding on the right system button would leave none
		return;
	}
	// Games' binding choices from earlier builds go back to their own (our profile needs none).
	UndoBindingChoices(DataDir());
	// The dashboard binding is always picked, not left to the profile: SteamVR takes the oculus_touch
	// type's default bindings from Valve's files even with its Oculus driver off (tested 2026-11-01),
	// and Valve's dashboard is on the left system button, which our profile has as the menu button.
	const std::wstring file = DataDir() + L"\\dashboard_touch.json";
	if (WriteFileText(file, dashboard)) {
		settings->SetString(kDashboardSection, kDashboardKey, FileUrl(file).c_str(), &err);
		g_dashboardOnRight = err == vr::VRSettingsError_None;
	}
	if (loaded) {
		// This start still has Valve's profile: no /input/menu, so no game's menu button yet.
		Log("buttons: SteamVR's Oculus driver is loaded this time: %s; restart SteamVR for the game's menu button",
			g_dashboardOnRight ? "the Oculus button opens the dashboard" : "the Oculus and menu buttons open the dashboard");
		return;
	}
	g_buttons = Buttons::Ours;
	Log("buttons: %s, the menu button is the game's menu",
		g_dashboardOnRight ? "the Oculus button opens the dashboard (hold 1 s: recentre)" : "no dashboard button (its binding couldn't be picked)");
}

XrPosef FromMatrix(const vr::HmdMatrix34_t& m)
{
	double r[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) r[i][j] = m.m[i][j];
	return { QFromMatrix(r), { m.m[0][3], m.m[1][3], m.m[2][3] } };
}

LONGLONG Qpc()
{
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	return li.QuadPart;
}

bool ProcessRunning(const wchar_t* exe)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32W pe = { sizeof pe };
	bool found = false;
	for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) found = !_wcsicmp(pe.szExeFile, exe);
	CloseHandle(snap);
	return found;
}

std::atomic<bool> g_calibrateRequest { false };

// ---- one Touch ----------------------------------------------------------------------------------
class TouchDevice : public vr::ITrackedDeviceServerDriver {
public:
	explicit TouchDevice(int hand) : m_hand(hand)
	{
		m_pose = {};
		m_pose.qWorldFromDriverRotation = { 1, 0, 0, 0 };
		m_pose.qDriverFromHeadRotation = { 1, 0, 0, 0 };
		m_pose.qRotation = { 1, 0, 0, 0 };
		m_pose.result = vr::TrackingResult_Uninitialized;
	}

	const char* Serial() const { return m_hand ? "TouchLink_Controller_Right" : "TouchLink_Controller_Left"; }
	uint32_t Id() const { return m_id; }
	vr::VRInputComponentHandle_t HapticHandle() const { return m_haptic; }

	vr::EVRInitError Activate(uint32_t id) override
	{
		m_id = id;
		auto* p = vr::VRProperties();
		vr::PropertyContainerHandle_t c = p->TrackedDeviceToPropertyContainer(id);
		const bool r = m_hand == 1;
		p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "oculus");
		p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Oculus");
		p->SetStringProperty(c, vr::Prop_ModelNumber_String, r ? "Oculus Rift CV1 (Right Controller)" : "Oculus Rift CV1 (Left Controller)");
		p->SetStringProperty(c, vr::Prop_SerialNumber_String, Serial());
		p->SetStringProperty(c, vr::Prop_RenderModelName_String, r ? "oculus_cv1_controller_right" : "oculus_cv1_controller_left");
		p->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String, r ? "oculus/TouchLink_Controller_Right" : "oculus/TouchLink_Controller_Left");
		p->SetStringProperty(c, vr::Prop_ControllerType_String, "oculus_touch");
		const std::string res = g_buttons == Buttons::Ours ? "{touchlink}" : "{oculus}";
		p->SetStringProperty(c, vr::Prop_InputProfilePath_String, (res + "/input/touch_profile.json").c_str());
		p->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, r ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
		// Low until the Touch is usable (SetUsable): other controllers (Index) keep the hand meanwhile.
		m_priority = LoadConfig().handPriority;
		m_usable = false;
		p->SetInt32Property(c, vr::Prop_ControllerHandSelectionPriority_Int32, kStandbyPriority);
		// SteamVR's own CV1 Touch icons (its oculus driver), not blank squares.
		const std::string icon = res + "/icons/cb_" + (r ? "right" : "left") + "_controller_";
		p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String, (icon + "off.png").c_str());
		p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearching_String, (icon + "searching.gif").c_str());
		p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearchingAlert_String, (icon + "searching_alert.gif").c_str());
		p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String, (icon + "ready.png").c_str());
		p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReadyAlert_String, (icon + "ready_alert.png").c_str());
		p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceNotReady_String, (icon + "error.png").c_str());
		p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceStandby_String, (icon + "off.png").c_str());
		p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceAlertLow_String, (icon + "ready_low.png").c_str());
		p->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
		p->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
		p->SetBoolProperty(c, vr::Prop_WillDriftInYaw_Bool, false);
		// The same tracking universe as the headset: the calibration puts the Touches in its space.
		vr::ETrackedPropertyError err;
		uint64_t universe = p->GetUint64Property(p->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd),
			vr::Prop_CurrentUniverseId_Uint64, &err);
		if (err == vr::TrackedProp_Success && universe) p->SetUint64Property(c, vr::Prop_CurrentUniverseId_Uint64, universe);

		auto* in = vr::VRDriverInput();
		auto b = [&](const char* path) {
			vr::VRInputComponentHandle_t h = vr::k_ulInvalidInputComponentHandle;
			in->CreateBooleanComponent(c, path, &h);
			return h;
		};
		auto s = [&](const char* path, bool twoSided) {
			vr::VRInputComponentHandle_t h = vr::k_ulInvalidInputComponentHandle;
			in->CreateScalarComponent(c, path, &h, vr::VRScalarType_Absolute,
				twoSided ? vr::VRScalarUnits_NormalizedTwoSided : vr::VRScalarUnits_NormalizedOneSided);
			return h;
		};
		m_h[I_LOWER] = b(r ? "/input/a/click" : "/input/x/click");
		m_h[I_LOWER_TOUCH] = b(r ? "/input/a/touch" : "/input/x/touch");
		m_h[I_UPPER] = b(r ? "/input/b/click" : "/input/y/click");
		m_h[I_UPPER_TOUCH] = b(r ? "/input/b/touch" : "/input/y/touch");
		// The menu button (left) and the Oculus button (right: its system button, which SteamVR takes
		// with Valve's profile too). With Valve's profile there's no /input/menu, so the left one is the
		// system button (SetUpButtons).
		if (g_buttons == Buttons::Ours) m_h[I_MENU] = b(r ? "/input/system/click" : "/input/menu/click");
		else m_h[I_MENU] = b("/input/system/click");
		m_h[I_STICK_CLICK] = b("/input/joystick/click");
		m_h[I_STICK_TOUCH] = b("/input/joystick/touch");
		m_h[I_STICK_X] = s("/input/joystick/x", true);
		m_h[I_STICK_Y] = s("/input/joystick/y", true);
		m_h[I_TRIGGER] = s("/input/trigger/value", false);
		m_h[I_TRIGGER_TOUCH] = b("/input/trigger/touch");
		m_h[I_GRIP] = s("/input/grip/value", false);
		m_h[I_GRIP_TOUCH] = b("/input/grip/touch");
		m_h[I_THUMBREST] = b("/input/thumbrest/touch");
		in->CreateHapticComponent(c, "/output/haptic", &m_haptic);
		Log("%s hand active (device %u)", r ? "right" : "left", id);
		return vr::VRInitError_None;
	}

	void Deactivate() override { m_id = vr::k_unTrackedDeviceIndexInvalid; }
	void EnterStandby() override {}
	void* GetComponent(const char*) override { return nullptr; }
	void DebugRequest(const char* request, char* response, uint32_t size) override
	{
		// Other tools (Atrium) can start a calibration through IVRSystem::DriverDebugRequest.
		if (!strcmp(request, "calibrate")) {
			g_calibrateRequest = true;
			snprintf(response, size, "ok");
		} else if (size)
			snprintf(response, size, "unknown request (try \"calibrate\")");
	}
	vr::DriverPose_t GetPose() override { return m_pose; }

	void UpdateInput(const TouchInput& t)
	{
		if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
		float v[I_COUNT] = {};
		v[I_LOWER] = t.lower; v[I_LOWER_TOUCH] = t.lowerTouch || t.lower;
		v[I_UPPER] = t.upper; v[I_UPPER_TOUCH] = t.upperTouch || t.upper;
		v[I_MENU] = t.menu;
		v[I_STICK_CLICK] = t.stickClick; v[I_STICK_TOUCH] = t.stickTouch || t.stickClick;
		v[I_STICK_X] = t.stick.x; v[I_STICK_Y] = t.stick.y;
		v[I_TRIGGER] = t.trigger; v[I_TRIGGER_TOUCH] = t.triggerTouch || t.trigger > 0.05f;
		v[I_GRIP] = t.squeeze; v[I_GRIP_TOUCH] = t.squeeze > 0.05f;   // the Touch has no grip touch sensor
		v[I_THUMBREST] = t.thumbrest;
		auto* in = vr::VRDriverInput();
		for (int i = 0; i < I_COUNT; i++) {
			if (m_h[i] == vr::k_ulInvalidInputComponentHandle || (m_sent && v[i] == m_last[i])) continue;
			if (i == I_STICK_X || i == I_STICK_Y || i == I_TRIGGER || i == I_GRIP) in->UpdateScalarComponent(m_h[i], v[i], 0);
			else in->UpdateBooleanComponent(m_h[i], v[i] != 0, 0);
			m_last[i] = v[i];
		}
		m_sent = true;
	}

	void UpdatePose(const vr::DriverPose_t& pose)
	{
		m_pose = pose;
		if (m_id != vr::k_unTrackedDeviceIndexInvalid) vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, m_pose, sizeof m_pose);
	}

	// The Touch claims its hand only while it's on, calibrated and tracked; otherwise another
	// controller for that hand (Index) has it. Dropping back waits 2 s, so a short loss of tracking
	// doesn't flip the hand back and forth.
	void SetUsable(bool usable)
	{
		if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
		ULONGLONG now = GetTickCount64();
		if (usable) m_lastUsable = now;
		bool want = usable || (m_usable && now - m_lastUsable < 2000);
		if (want == m_usable) return;
		m_usable = want;
		auto* p = vr::VRProperties();
		p->SetInt32Property(p->TrackedDeviceToPropertyContainer(m_id), vr::Prop_ControllerHandSelectionPriority_Int32,
			want ? m_priority : kStandbyPriority);
		Log("%s Touch %s its hand", m_hand ? "right" : "left", want ? "takes" : "gives up");
	}

	// The last good pose, held through a short loss of tracking.
	const vr::DriverPose_t& LastPose() const { return m_pose; }

private:
	static const int32_t kStandbyPriority = -100;   // below every other controller
	int32_t m_priority = 100;
	bool m_usable = false;
	ULONGLONG m_lastUsable = 0;
	enum {
		I_LOWER, I_LOWER_TOUCH, I_UPPER, I_UPPER_TOUCH, I_MENU, I_STICK_CLICK, I_STICK_TOUCH, I_STICK_X, I_STICK_Y,
		I_TRIGGER, I_TRIGGER_TOUCH, I_GRIP, I_GRIP_TOUCH, I_THUMBREST, I_COUNT
	};
	int m_hand;
	uint32_t m_id = vr::k_unTrackedDeviceIndexInvalid;
	vr::VRInputComponentHandle_t m_h[I_COUNT] = {};
	vr::VRInputComponentHandle_t m_haptic = vr::k_ulInvalidInputComponentHandle;
	float m_last[I_COUNT] = {};
	bool m_sent = false;
	vr::DriverPose_t m_pose;
};

// ---- the provider -------------------------------------------------------------------------------
class Provider : public vr::IServerTrackedDeviceProvider {
public:
	vr::EVRInitError Init(vr::IVRDriverContext* ctx) override
	{
		VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
		{
			std::lock_guard<std::mutex> lk(g_logLock);
			_wfopen_s(&g_logFile, DataFile(L"driver.log").c_str(), L"w");
		}
		m_cfg = LoadConfig();
		m_cal = LoadCalibration();
		Log("started; service %s; %s", m_cfg.serviceDir.empty() ? "not set (run touchlink install)" : Narrow(m_cfg.serviceDir).c_str(),
			m_cal.valid ? "calibrated" : "not calibrated yet (hold a Touch against the headset and look around)");
		DeleteFileW(DataFile(L"calibrate").c_str());
		SetUpButtons(m_cfg.meta);
		m_stop = false;
		m_worker = std::thread([this] { Worker(); });
		return vr::VRInitError_None;
	}

	void Cleanup() override
	{
		m_stop = true;
		if (m_worker.joinable()) m_worker.join();
		m_src.Disconnect();
		if (m_job) {
			CloseHandle(m_job);   // ends a service we started
			m_job = nullptr;
		}
		if (m_service) {
			CloseHandle(m_service);
			m_service = nullptr;
		}
		Log("stopped");
		std::lock_guard<std::mutex> lk(g_logLock);
		if (g_logFile) fclose(g_logFile);
		g_logFile = nullptr;
		VR_CLEANUP_SERVER_DRIVER_CONTEXT();
	}

	const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }

	void RunFrame() override
	{
		vr::VREvent_t ev;
		while (vr::VRServerDriverHost()->PollNextEvent(&ev, sizeof ev)) {
			if (ev.eventType != vr::VREvent_Input_HapticVibration) continue;
			const auto& hv = ev.data.hapticVibration;
			for (int h = 0; h < 2; h++)
				if (m_added && hv.componentHandle == m_dev[h].HapticHandle())
					m_src.Haptic(h, hv.fDurationSeconds, hv.fFrequency, hv.fAmplitude);
		}
	}

	bool ShouldBlockStandbyMode() override { return false; }
	void EnterStandby() override {}
	void LeaveStandby() override {}

private:
	// Starts Atrium's tracking service when nobody else (the Atrium app) runs it. It ends with
	// SteamVR (a job object).
	bool StartService(const std::wstring& exe)
	{
		if (m_service && WaitForSingleObject(m_service, 0) == WAIT_TIMEOUT) return true;
		if (m_service) {
			DWORD code = 0;
			GetExitCodeProcess(m_service, &code);
			Log("the tracking service ended (exit code %lu); see service.log", code);
			CloseHandle(m_service);
			m_service = nullptr;
		}
		// Its environment: ours plus what the Rift drivers need (as the Atrium app sets it).
		std::wstring env;
		if (wchar_t* block = GetEnvironmentStringsW()) {
			for (wchar_t* e = block; *e; e += wcslen(e) + 1) env.append(e).push_back(L'\0');
			FreeEnvironmentStringsW(block);
		}
		for (const wchar_t* v : { L"RIFT_PROBER_CONSTELLATION_TRACKING=1", L"XRT_COMPOSITOR_NULL=1", L"RIFT_LOG=info", L"XRT_LOG=info",
							L"ATRIUM_RIFT_HMD_UNTRACKED=1" })
			env.append(v).push_back(L'\0');
		env.push_back(L'\0');

		SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
		HANDLE log = CreateFileW(DataFile(L"service.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
		STARTUPINFOW si = { sizeof si };
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = si.hStdError = log;
		si.hStdInput = nullptr;
		PROCESS_INFORMATION pi = {};
		std::wstring cmd = L"\"" + exe + L"\"";
		std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
		BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
			&env[0], dir.c_str(), &si, &pi);
		if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
		if (!ok) {
			Log("couldn't start %s (error %lu)", Narrow(exe).c_str(), GetLastError());
			return false;
		}
		if (!m_job) {
			m_job = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
			li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &li, sizeof li);
		}
		AssignProcessToJobObject(m_job, pi.hProcess);
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);
		m_service = pi.hProcess;
		Log("started the tracking service (pid %lu)", pi.dwProcessId);
		return true;
	}

	void TryConnect()
	{
		ULONGLONG now = GetTickCount64();
		if (now - m_lastTry < 3000) return;
		m_lastTry = now;
		// The Meta source: the Touches as Meta's runtime tracks them, through touchlink meta.
		if (m_cfg.meta) {
			// A helper started before SteamVR (by the Atrium app) has the Rift's display and its
			// buttons; one started now can't get them while SteamVR shows the headset. Use it.
			HANDLE other = OpenMutexW(SYNCHRONIZE, FALSE, kMetaHelperMutex);
			bool ours = m_metaHelper && WaitForSingleObject(m_metaHelper, 0) == WAIT_TIMEOUT;
			if (other) CloseHandle(other);
			if (!other || ours) StartHelper(L"meta", &m_metaHelper);
			else if (!m_usingOtherHelper) {
				m_usingOtherHelper = true;
				Log("touchlink meta already runs (started before SteamVR): using it");
			}
			std::string why;
			if (m_src.ConnectMeta(&why)) {
				Log("connected to touchlink meta (Meta's runtime)");
				m_message.clear();
				AddDevices();
				return;
			}
			if (why != m_lastWhy) Log("meta source: %s", why.c_str());
			m_lastWhy = why;
			m_message = why;
			return;
		}
		std::wstring exe, manifest;
		if (!ServiceFiles(m_cfg.serviceDir, &exe, &manifest)) {
			m_message = m_cfg.serviceDir.empty() ? "no tracking service set: run touchlink install"
												 : "no tracking service in " + Narrow(m_cfg.serviceDir);
			return;
		}
		std::string why;
		if (m_src.Connect(manifest, &why)) {
			Log("connected to the tracking service");
			m_message.clear();
			AddDevices();
			return;
		}
		// Only one tracking service can hold the Rift. The Atrium app runs its own (Mix & match):
		// then wait for it (it takes a few seconds to answer) instead of starting a second one.
		bool ours = m_service && WaitForSingleObject(m_service, 0) == WAIT_TIMEOUT;
		if (!ours && (ProcessRunning(L"atrium-service.exe") || ProcessRunning(L"atrium.exe"))) {
			why = ProcessRunning(L"atrium-service.exe") ? "waiting for the Atrium app's tracking service"
														: "the Atrium app runs it: pick Rift CV1 Touches in its Mix & match tab";
			if (why != m_lastWhy) Log("tracking service: %s", why.c_str());
			m_lastWhy = why;
			m_message = why;
			return;
		}
		if (why != m_lastWhy) Log("tracking service: %s", why.c_str());
		m_lastWhy = why;
		m_message = "tracking service: " + why;
		if (m_cfg.startService && StartService(exe)) m_lastTry = now + 2000;   // give it time to find the Rift
	}

	// Added once, the first time the Touches are there; later they just show as off.
	void AddDevices()
	{
		if (m_added) return;
		for (int h = 0; h < 2; h++)
			vr::VRServerDriverHost()->TrackedDeviceAdded(m_dev[h].Serial(), vr::TrackedDeviceClass_Controller, &m_dev[h]);
		m_added = true;
	}

	// touchlink.exe (two folders above this driver's bin\win64) with one argument, ending with SteamVR
	// (the job object). Started again if it ended.
	void StartHelper(const wchar_t* arg, HANDLE* proc)
	{
		if (*proc && WaitForSingleObject(*proc, 0) == WAIT_TIMEOUT) return;
		if (*proc) {
			CloseHandle(*proc);
			*proc = nullptr;
		}
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Log, &self);
		wchar_t path[MAX_PATH];
		GetModuleFileNameW(self, path, MAX_PATH);
		std::wstring dir = path;
		for (int up = 0; up < 4; up++) dir = dir.substr(0, dir.find_last_of(L'\\'));   // bin\win64\dll -> touchlink\ -> its parent
		std::wstring exe = dir + L"\\touchlink.exe";
		if (!FileExists(exe)) {
			Log("touchlink.exe isn't at %s", Narrow(exe).c_str());
			return;
		}
		SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
		std::wstring logName = std::wstring(arg) + L".log";
		HANDLE log = CreateFileW(DataFile(logName.c_str()).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
		STARTUPINFOW si = { sizeof si };
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = si.hStdError = log;
		PROCESS_INFORMATION pi = {};
		std::wstring cmd = L"\"" + exe + L"\" " + arg;
		BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
			dir.c_str(), &si, &pi);
		if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
		if (!ok) {
			Log("couldn't start touchlink %s (error %lu)", Narrow(arg).c_str(), GetLastError());
			return;
		}
		if (!m_job) {
			m_job = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
			li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &li, sizeof li);
		}
		AssignProcessToJobObject(m_job, pi.hProcess);
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);
		*proc = pi.hProcess;
		Log("started touchlink %s (pid %lu)", Narrow(arg).c_str(), pi.dwProcessId);
	}

	// A calibration written by someone else (touchlink camera, from the headset's cameras) takes over.
	void ReloadCalibration()
	{
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!GetFileAttributesExW(CalibrationFile().c_str(), GetFileExInfoStandard, &a)) return;
		ULONGLONG t = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
		if (t == m_calWritten) return;
		bool first = m_calWritten == 0;
		m_calWritten = t;
		if (first) return;   // the one loaded at start
		Calibration c = LoadCalibration();
		if (!c.valid) return;
		std::lock_guard<std::mutex> lk(m_calLock);
		m_cal = c;
		Log("calibration updated (%.1f mm)", c.errorMm);
	}

	// config.json's grip_offset changed (Atrium's settings): it applies at once. The rest of the
	// config (the source, the service) waits for SteamVR's next start.
	void ReloadGripOffset()
	{
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!GetFileAttributesExW(DataFile(L"config.json").c_str(), GetFileExInfoStandard, &a)) return;
		ULONGLONG t = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
		if (t == m_cfgWritten) return;
		bool first = m_cfgWritten == 0;
		m_cfgWritten = t;
		if (first) return;   // the one loaded at start
		XrPosef o = LoadConfig().gripOffset;
		if (!memcmp(&o, &m_cfg.gripOffset, sizeof o)) return;
		m_cfg.gripOffset = o;
		Log("grip offset updated: %.1f %.1f %.1f cm", o.position.x * 100, o.position.y * 100, o.position.z * 100);
	}

	void Buzz(int hand, float seconds) { m_src.Haptic(hand, seconds, 0, 1); }


	// ---- calibration ----
	// No timing to get right: while the Touches aren't calibrated (or after X+Y+A+B, or the
	// calibrate file), the last few seconds of both Touches are solved every second, and the first
	// window where a Touch moved rigidly with the headset wins. A Touch that's only in a hand never
	// fits, so the user just holds one against the side of the headset and looks around until the
	// buzz.
	static constexpr size_t kWindow = 800;   // samples, 10 ms apart: the last 8 seconds

	void CalibrationTick(const TouchInput in[2], LONGLONG qpc, bool calibrated)
	{
		ULONGLONG now = GetTickCount64();
		bool combo = in[0].lower && in[0].upper && in[1].lower && in[1].upper;
		if (!combo) {
			m_comboSince = 0;
			m_comboUsed = false;
		} else if (!m_comboSince)
			m_comboSince = now;
		bool fileRequest = FileExists(DataFile(L"calibrate"));
		bool asked = g_calibrateRequest.exchange(false) || fileRequest || (combo && !m_comboUsed && now - m_comboSince > 1000);
		if (asked) {
			m_comboUsed = combo;
			DeleteFileW(DataFile(L"calibrate").c_str());
			Log("calibration: asked for");
			Buzz(0, 0.15f);
			Buzz(1, 0.15f);
		}
		if (m_calPhase == 0 && (asked || !calibrated)) {
			m_calPhase = 2;
			m_calStart = now;
			m_lastSolve = now;
			m_track[0].clear();
			m_track[1].clear();
			m_candHand = -1;
			m_calMessage = "hold a Touch against the side of the headset and look around";
			Log("calibration: looking for a Touch held to the headset");
		}
		if (m_calPhase != 2) return;
		if (now - m_lastSample >= 10) {
			m_lastSample = now;
			vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
			vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0, poses, vr::k_unMaxTrackedDeviceCount);
			const auto& hp = poses[vr::k_unTrackedDeviceIndex_Hmd];
			if (hp.bPoseIsValid && hp.bDeviceIsConnected && hp.eTrackingResult == vr::TrackingResult_Running_OK) {
				XrPosef hmd = FromMatrix(hp.mDeviceToAbsoluteTracking);
				for (int h = 0; h < 2; h++) {
					TouchPose tp;
					if (!m_src.Locate(h, qpc, &tp) || !tp.positionTracked) continue;
					m_track[h].push_back({ hmd, tp.grip, tp.angVel, tp.linVel });
					if (m_track[h].size() > kWindow) m_track[h].erase(m_track[h].begin(), m_track[h].end() - kWindow);
				}
			}
		}
		if (now - m_lastSolve < 1000) return;
		m_lastSolve = now;
		{
			CalResult best;
			int bestHand = -1;
			double mostTurned = -1;
			for (int h = 0; h < 2; h++) {
				if (m_track[h].size() < 300) continue;   // 3 seconds of it seen
				CalResult r = Solve(m_track[h], m_cfg.meta);
				if (r.ok && (bestHand < 0 || r.rmsMm < best.rmsMm)) {
					best = r;
					bestHand = h;
				} else if (bestHand < 0 && r.turnedDeg > mostTurned) {
					best = r;   // what to tell the user: the Touch closest to working
					mostTurned = r.turnedDeg;
				}
			}
			// The first fit is rarely the best (18.6 mm made a bad offset in games, 2026-11-01): keep
			// the best one and go on looking for 10 more seconds, unless one is already good.
			if (bestHand >= 0 && (m_candHand < 0 || best.rmsMm < m_cand.rmsMm)) {
				if (m_candHand < 0) m_candSince = now;
				m_cand = best;
				m_candHand = bestHand;
			}
			if (m_candHand >= 0 && m_cand.rmsMm > 8 && now - m_candSince < 10000) {
				m_calMessage = "fits to " + std::to_string((int)(m_cand.rmsMm + 0.5)) + " mm; keep going for a better fit (" +
					std::to_string((int)((10000 - (now - m_candSince)) / 1000)) + " s)";
				return;
			}
			if (m_candHand >= 0) {
				best = m_cand;
				bestHand = m_candHand;
				m_candHand = -1;
			}
			if (bestHand < 0) {
				std::string why = !best.why[0] ? "no Touch seen by a sensor yet"
					: best.turnedDeg < 40 ? "head turned " + std::to_string((int)best.turnedDeg) + " of 40 degrees" : best.why;
				m_calMessage = "hold a Touch against the side of the headset and look around (" + why + ")";
				if (best.why[0] && why != m_calWhy)
					Log("calibration: not yet: %s (error %.1f mm, delay %.0f ms, wobble %.1f deg, head turned %.0f deg)", best.why,
						best.rmsMm, best.lagMs, best.spread, best.turnedDeg);
				m_calWhy = why;
				return;
			}
			m_calPhase = 0;
			Calibration c;
			c.valid = true;
			c.space = best.space;
			c.errorMm = best.rmsMm;
			vr::ETrackedPropertyError err;
			c.universe = vr::VRProperties()->GetUint64Property(
				vr::VRProperties()->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd), vr::Prop_CurrentUniverseId_Uint64, &err);
			SaveCalibration(c, best, bestHand ? "right" : "left");
			{
				std::lock_guard<std::mutex> lk(m_calLock);
				m_cal = c;
			}
			char msg[160];
			snprintf(msg, sizeof msg, "calibrated with the %s Touch (error %.1f mm)", bestHand ? "right" : "left", best.rmsMm);
			m_calMessage = msg;
			Log("%s", msg);
			Buzz(0, 0.3f);
			Buzz(1, 0.3f);
		}
	}

	vr::DriverPose_t PoseFor(int hand, const TouchInput& in, LONGLONG qpc, const Calibration& cal)
	{
		vr::DriverPose_t p = {};
		p.qWorldFromDriverRotation = ToHmd(cal.space.orientation);
		p.vecWorldFromDriverTranslation[0] = cal.space.position.x;
		p.vecWorldFromDriverTranslation[1] = cal.space.position.y;
		p.vecWorldFromDriverTranslation[2] = cal.space.position.z;
		p.qDriverFromHeadRotation = { 1, 0, 0, 0 };
		p.qRotation = { 1, 0, 0, 0 };
		p.deviceIsConnected = m_src.Ready() && in.active;
		TouchPose tp;
		m_tracked[hand] = false;
		if (!p.deviceIsConnected || !m_src.Locate(hand, qpc, &tp)) {
			p.result = vr::TrackingResult_Running_OutOfRange;
			return p;
		}
		m_tracked[hand] = tp.positionTracked;
		if (!cal.valid) {
			p.result = vr::TrackingResult_Calibrating_InProgress;   // buttons work, so X+Y+A+B does
			return p;
		}
		XrPosef grip = PMul(tp.grip, m_cfg.gripOffset);
		XrPosef raw = PMul(grip, PInv(GripLocal(hand)));
		XrVector3f v = VAdd(tp.linVel, VCross(tp.angVel, VSub(raw.position, tp.grip.position)));
		XrVector3f w = QRot(QConj(raw.orientation), tp.angVel);   // SteamVR wants it in the device's frame
		p.vecPosition[0] = raw.position.x; p.vecPosition[1] = raw.position.y; p.vecPosition[2] = raw.position.z;
		p.qRotation = ToHmd(raw.orientation);
		p.vecVelocity[0] = v.x; p.vecVelocity[1] = v.y; p.vecVelocity[2] = v.z;
		p.vecAngularVelocity[0] = w.x; p.vecAngularVelocity[1] = w.y; p.vecAngularVelocity[2] = w.z;
		p.poseIsValid = true;
		p.result = vr::TrackingResult_Running_OK;
		return p;
	}

	void WriteStatus()
	{
		Calibration cal;
		{
			std::lock_guard<std::mutex> lk(m_calLock);
			cal = m_cal;
		}
		vr::ETrackedPropertyError err;
		uint64_t universe = vr::VRProperties()->GetUint64Property(
			vr::VRProperties()->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd), vr::Prop_CurrentUniverseId_Uint64, &err);
		bool otherUniverse = cal.valid && cal.universe && universe && cal.universe != universe;
		// A calibration needs the headset tracked too (touchlink setup waits for it).
		vr::TrackedDevicePose_t hp[1];
		vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0, hp, 1);
		bool hmdTracked = hp[0].bPoseIsValid && hp[0].bDeviceIsConnected && hp[0].eTrackingResult == vr::TrackingResult_Running_OK;
		m_otherUniverse = otherUniverse;
		const char* phase = m_calPhase == 2 ? "looking" : "";
		char buf[2048];
		snprintf(buf, sizeof buf,
			"{\n  \"updated\": %lld,\n  \"connected\": %s,\n  \"service_ours\": %s,\n  \"meta_running\": %s,\n  \"hmd_tracked\": %s,\n"
			"  \"left\": {\"active\": %s, \"tracked\": %s},\n  \"right\": {\"active\": %s, \"tracked\": %s},\n"
			"  \"calibrated\": %s,\n  \"error_mm\": %.1f,\n  \"other_universe\": %s,\n  \"calibrating\": \"%s\",\n"
			"  \"calibration_message\": \"%s\",\n  \"message\": \"%s\"\n}\n",
			(long long)time(nullptr), m_src.Ready() ? "true" : "false",
			(m_service && WaitForSingleObject(m_service, 0) == WAIT_TIMEOUT) ? "true" : "false", m_metaRunning ? "true" : "false",
			hmdTracked ? "true" : "false", m_input[0].active ? "true" : "false", m_tracked[0] ? "true" : "false", m_input[1].active ? "true" : "false",
			m_tracked[1] ? "true" : "false", cal.valid ? "true" : "false", cal.errorMm, otherUniverse ? "true" : "false", phase,
			JsonEscape(m_calMessage).c_str(), JsonEscape(m_message).c_str());
		WriteFileText(DataFile(L"status.json"), buf);
		if (otherUniverse && !m_warnedUniverse) {
			Log("the base stations' tracking universe changed since calibrating: calibrate again");
			m_warnedUniverse = true;
		}
	}

	void Worker()
	{
		timeBeginPeriod(1);
		ULONGLONG lastStatus = 0, lastMetaCheck = 0;
		while (!m_stop) {
			ULONGLONG now = GetTickCount64();
			if (now - lastMetaCheck > 5000) {
				lastMetaCheck = now;
				// With the Meta source, Meta's runtime is the point; otherwise it fights the service.
				bool meta = !m_cfg.meta && ProcessRunning(L"OVRServer_x64.exe");
				if (meta && !m_metaRunning) Log("Meta's OVRServer is running: it fights the tracking service for the Rift's radio");
				m_metaRunning = meta;
				// The headset's cameras keep the calibration right (it exits by itself without a camera).
				if (m_src.Ready() && !m_cameraTried) {
					m_cameraTried = true;
					StartHelper(L"camera", &m_cameraHelper);
				}
				ReloadCalibration();
				ReloadGripOffset();
			}
			if (!m_src.Ready()) {
				TryConnect();
				for (int h = 0; h < 2; h++) {
					m_input[h].active = false;
					if (m_added) m_dev[h].UpdatePose(PoseFor(h, m_input[h], 0, Calibration()));
				}
			} else if (m_src.Sync(m_input)) {
				LONGLONG qpc = Qpc();
				Calibration cal;
				{
					std::lock_guard<std::mutex> lk(m_calLock);
					cal = m_cal;
				}
				for (int h = 0; h < 2; h++) {
					TouchInput in = m_input[h];
					// Valve's profile and Valve's dashboard binding (ours couldn't be set up): the dashboard is
					// on the left system button only, so the Oculus button presses it too.
					if (h == 0 && m_cfg.meta && g_buttons == Buttons::Valve && !g_dashboardOnRight) in.menu = in.menu || m_input[1].menu;
					m_dev[h].UpdateInput(in);
					m_dev[h].UpdatePose(PoseFor(h, m_input[h], qpc, cal));
				}
				CalibrationTick(m_input, qpc, cal.valid && !m_otherUniverse);
			} else if (!m_src.Ready())
				Log("lost the tracking service; reconnecting");
			if (now - lastStatus >= 1000) {
				lastStatus = now;
				WriteStatus();
			}
			Sleep(2);
		}
		timeEndPeriod(1);
	}

	Config m_cfg;
	Source m_src;
	TouchDevice m_dev[2] = { TouchDevice(0), TouchDevice(1) };
	bool m_added = false;
	std::thread m_worker;
	std::atomic<bool> m_stop { false };
	HANDLE m_service = nullptr, m_job = nullptr;
	HANDLE m_metaHelper = nullptr, m_cameraHelper = nullptr;   // touchlink meta, touchlink camera
	ULONGLONG m_calWritten = 0;   // calibration.json's time when last loaded
	ULONGLONG m_cfgWritten = 0;   // config.json's
	bool m_usingOtherHelper = false;   // touchlink meta was already running (the Atrium app's)
	bool m_cameraTried = false;
	ULONGLONG m_lastTry = 0;
	std::string m_message, m_lastWhy, m_calMessage, m_calWhy;
	bool m_metaRunning = false, m_warnedUniverse = false, m_otherUniverse = false;
	TouchInput m_input[2];
	bool m_tracked[2] = {};

	std::mutex m_calLock;   // m_cal is read by the status writer and replaced by a calibration
	Calibration m_cal;
	int m_calPhase = 0;   // 0 idle, 2 looking for a Touch held to the headset
	ULONGLONG m_calStart = 0, m_comboSince = 0, m_lastSample = 0, m_lastSolve = 0, m_candSince = 0;
	CalResult m_cand;   // the best fit so far while looking
	int m_candHand = -1;
	bool m_comboUsed = false;
	std::vector<CalSample> m_track[2];
};

Provider g_provider;

} // namespace
} // namespace tl

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode)
{
	if (!strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version)) return &tl::g_provider;
	if (returnCode) *returnCode = vr::VRInitError_Init_InterfaceNotFound;
	return nullptr;
}
