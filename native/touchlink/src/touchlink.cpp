// touchlink.exe: sets up and checks the TouchLink SteamVR driver (driver.cpp).
//
//   touchlink [setup] [--service DIR]   everything, in order (also what a double-click does): install,
//                                       Meta's service off + sensors on WinUSB (one UAC prompt),
//                                       (re)start SteamVR, wait for tracking, walk through calibrating
//   touchlink install [--service DIR]   register the driver with SteamVR, remember the tracking service
//   touchlink uninstall                 unregister it; give the Rift back to Meta if setup took it
//   touchlink calibrate                 start a calibration and follow it
//   touchlink status [--json]           what the driver reports (status.json) and what's in the way
//
// The driver folder sits next to this exe (touchlink\driver.vrdrivermanifest). The tracking
// service is Atrium's (native\atrium_monado, build.ps1 -Rift); without --service it's looked for
// in ATRIUM_MONADO_DIR, next to this exe (service\), and in an Atrium checkout around it.
#include "common.h"

#include <conio.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winsvc.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace tl { int CameraMain(); int MetaMain(); int MetaPokeMain(); }   // camera.cpp, meta.cpp

using namespace tl;

namespace {

std::wstring ExeDir()
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	std::wstring p = buf;
	return p.substr(0, p.find_last_of(L'\\'));
}

std::wstring FullPath(const std::wstring& p)
{
	wchar_t buf[MAX_PATH];
	DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, buf, nullptr);
	return (n && n < MAX_PATH) ? buf : p;
}

std::wstring FindService()
{
	std::wstring exe, manifest;
	wchar_t env[MAX_PATH];
	std::vector<std::wstring> dirs;
	if (GetEnvironmentVariableW(L"ATRIUM_MONADO_DIR", env, MAX_PATH)) dirs.push_back(env);
	std::wstring here = ExeDir();
	dirs.push_back(here + L"\\service");
	dirs.push_back(here + L"\\..\\..\\atrium_monado\\build\\win");   // native\out\touchlink in a checkout: the Rift build
	dirs.push_back(here + L"\\..\\monado");                          // native\out\monado (staged)
	for (const auto& d : dirs)
		if (ServiceFiles(FullPath(d), &exe, &manifest)) return FullPath(d);
	return {};
}

// SteamVR's folder, from the runtimes openvrpaths.vrpath lists (the first with vrpathreg.exe).
std::wstring SteamVRDir()
{
	wchar_t local[MAX_PATH];
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return {};
	std::string text;
	if (!ReadFileText(std::wstring(local) + L"\\openvr\\openvrpaths.vrpath", &text)) return {};
	size_t at = text.find("\"runtime\"");
	if (at == std::string::npos) return {};
	size_t end = text.find(']', at);
	for (size_t q = text.find('"', text.find('[', at)); q != std::string::npos && q < end; q = text.find('"', q + 1)) {
		size_t close = text.find('"', q + 1);
		std::string dir;
		for (size_t i = q + 1; i < close; i++) {
			if (text[i] == '\\' && text[i + 1] == '\\') i++;
			dir.push_back(text[i]);
		}
		q = close;
		if (FileExists(Widen(dir) + L"\\bin\\win64\\vrpathreg.exe")) return Widen(dir);
	}
	return {};
}

std::wstring VrPathReg()
{
	std::wstring dir = SteamVRDir();
	return dir.empty() ? dir : dir + L"\\bin\\win64\\vrpathreg.exe";
}

int Run(const std::wstring& exe, const std::wstring& args)
{
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	STARTUPINFOW si = { sizeof si };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return (int)code;
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

void KillProcess(const wchar_t* exe)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return;
	PROCESSENTRY32W pe = { sizeof pe };
	for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
		if (_wcsicmp(pe.szExeFile, exe)) continue;
		HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
		if (!p) continue;
		TerminateProcess(p, 1);
		WaitForSingleObject(p, 5000);
		CloseHandle(p);
	}
	CloseHandle(snap);
}

// Runs a console tool and returns what it printed (exit code in *code).
std::string RunOutput(const std::wstring& exe, const std::wstring& args, int* code)
{
	SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
	HANDLE rd, wr;
	*code = -1;
	if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	STARTUPINFOW si = { sizeof si };
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = si.hStdError = wr;
	PROCESS_INFORMATION pi = {};
	BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(wr);
	std::string out;
	if (ok) {
		char buf[1024];
		DWORD n;
		while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD c = 0;
		GetExitCodeProcess(pi.hProcess, &c);
		*code = (int)c;
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
	CloseHandle(rd);
	return out;
}

// This exe again, as administrator (one UAC prompt); returns its exit code, -1 if refused.
int RunElevated(const wchar_t* args)
{
	wchar_t self[MAX_PATH];
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	SHELLEXECUTEINFOW sei = { sizeof sei };
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.lpVerb = L"runas";
	sei.lpFile = self;
	sei.lpParameters = args;
	sei.nShow = SW_HIDE;
	if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
	WaitForSingleObject(sei.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(sei.hProcess, &code);
	CloseHandle(sei.hProcess);
	return (int)code;
}

// ---- Meta's runtime ---------------------------------------------------------------------------
// OVRService starts OVRServer, which holds the Rift's radio and sensors. Setup stops it and sets it
// to Manual so it stays off after a restart; uninstall puts it back to Automatic.
const wchar_t kMetaService[] = L"OVRService";

// SERVICE_AUTO_START etc., or 0 when Meta's service isn't installed.
DWORD MetaStartType()
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!scm) return 0;
	DWORD type = 0;
	if (SC_HANDLE svc = OpenServiceW(scm, kMetaService, SERVICE_QUERY_CONFIG)) {
		BYTE buf[8192];
		DWORD n;
		if (QueryServiceConfigW(svc, (QUERY_SERVICE_CONFIGW*)buf, sizeof buf, &n)) type = ((QUERY_SERVICE_CONFIGW*)buf)->dwStartType;
		CloseServiceHandle(svc);
	}
	CloseServiceHandle(scm);
	return type;
}

bool MetaInTheWay()
{
	return ProcessRunning(L"OVRServer_x64.exe") || MetaStartType() == SERVICE_AUTO_START;
}

// (admin) Meta's service on (Automatic, started) or off (Manual, stopped, its processes closed).
bool SetMetaService(bool on)
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!scm) return false;
	bool ok = true;
	if (SC_HANDLE svc = OpenServiceW(scm, kMetaService, SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS)) {
		ok = ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, on ? SERVICE_AUTO_START : SERVICE_DEMAND_START, SERVICE_NO_CHANGE,
			nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != FALSE;
		SERVICE_STATUS st;
		if (on) StartServiceW(svc, 0, nullptr);
		else ControlService(svc, SERVICE_CONTROL_STOP, &st);
		CloseServiceHandle(svc);
	}
	CloseServiceHandle(scm);
	if (!on)
		for (const wchar_t* exe : { L"OVRServer_x64.exe", L"OVRRedir.exe", L"OVRServiceLauncher.exe" }) KillProcess(exe);
	return ok;
}

// ---- the sensors ------------------------------------------------------------------------------
// sensordriver.exe (next to this exe) puts them on Windows' WinUSB, which the tracking service
// opens them with, and turns USB 3 power saving off.
std::wstring SensorDriver()
{
	for (const std::wstring& p : { ExeDir() + L"\\sensordriver.exe", ExeDir() + L"\\..\\sensordriver.exe" })
		if (FileExists(p)) return FullPath(p);
	return {};
}

// How many sensors are plugged in, and whether they're all ready for the tracking service.
int Sensors(bool* ready)
{
	*ready = false;
	std::wstring tool = SensorDriver();
	if (tool.empty()) return -1;
	int code;
	std::string out = RunOutput(tool, L"status", &code);
	if (code == 2) return 0;
	int n = 0;
	bool all = true;
	for (size_t at = 0; at < out.size();) {
		size_t end = out.find('\n', at);
		std::string line = out.substr(at, end == std::string::npos ? std::string::npos : end - at);
		at = end == std::string::npos ? out.size() : end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		if (line.rfind("usb3-lpm", 0) == 0) {
			if (line != "usb3-lpm off") all = false;
		} else if (!line.empty()) {
			n++;
			if (line.size() < 7 || line.compare(line.size() - 7, 7, " winusb")) all = false;
		}
	}
	*ready = n > 0 && all;
	return n;
}

// `touchlink admin on|off`: the steps that need administrator, run elevated by setup/uninstall.
// off: Meta's service off and Manual, sensors on WinUSB. on: both back the way Meta had them.
int Admin(bool meta)
{
	int r = 0;
	if (MetaStartType() && !SetMetaService(meta)) r = 1;
	std::wstring tool = SensorDriver();
	if (!tool.empty()) {
		int code = Run(tool, meta ? L"restore" : L"winusb");
		if (code == 1) r = 1;
		else if (code == 3 && r == 0) r = 3;
	}
	return r;
}

int Install(int argc, wchar_t** argv, bool quiet = false)
{
	std::wstring service;
	for (int i = 2; i < argc; i++)
		if (!wcscmp(argv[i], L"--service") && i + 1 < argc) service = FullPath(argv[++i]);
	if (service.empty()) service = FindService();
	std::wstring exe, manifest;
	if (service.empty() || !ServiceFiles(service, &exe, &manifest)) {
		fprintf(stderr, "Atrium's tracking service wasn't found%s%s.\n"
						"Build it (native\\atrium_monado\\build.ps1 -Rift) or pass --service <folder>.\n",
			service.empty() ? "" : " in ", Narrow(service).c_str());
		return 1;
	}
	std::wstring driver = ExeDir() + L"\\touchlink";
	if (!FileExists(driver + L"\\driver.vrdrivermanifest") || !FileExists(driver + L"\\bin\\win64\\driver_touchlink.dll")) {
		fprintf(stderr, "The driver isn't next to touchlink.exe (%s).\n", Narrow(driver).c_str());
		return 1;
	}
	std::wstring reg = VrPathReg();
	if (reg.empty()) {
		fprintf(stderr, "SteamVR wasn't found (no runtime in %%LOCALAPPDATA%%\\openvr\\openvrpaths.vrpath). Run SteamVR once.\n");
		return 1;
	}
	if (Run(reg, L"adddriver \"" + driver + L"\"") != 0) {
		fprintf(stderr, "vrpathreg adddriver failed.\n");
		return 1;
	}
	// Keep a grip offset or hand priority the user already set.
	Config old = LoadConfig();
	std::string text, keep;
	ReadFileText(DataFile(L"config.json"), &text);
	double v[6];
	char line[256];
	if (JsonNumbers(text, "grip_offset", v, 6) == 6) {
		snprintf(line, sizeof line, ",\n  \"grip_offset\": [%g, %g, %g, %g, %g, %g]", v[0], v[1], v[2], v[3], v[4], v[5]);
		keep += line;
	}
	snprintf(line, sizeof line, ",\n  \"hand_priority\": %d,\n  \"source\": \"%s\"", old.handPriority, old.meta ? "meta" : "atrium");
	keep += line;
	std::string cfg = "{\n  \"service_dir\": \"" + JsonEscape(Narrow(service)) + "\",\n  \"start_service\": " +
		(old.startService ? "true" : "false") + keep + "\n}\n";
	if (!WriteFileText(DataFile(L"config.json"), cfg)) {
		fprintf(stderr, "Couldn't write %s.\n", Narrow(DataFile(L"config.json")).c_str());
		return 1;
	}
	printf("TouchLink is registered with SteamVR: %s\nTracking service: %s\n", Narrow(driver).c_str(), Narrow(service).c_str());
	if (!quiet) printf("\nNext: touchlink setup (Meta's software off, the sensors, SteamVR, calibration).\n");
	return 0;
}

int Uninstall()
{
	std::wstring reg = VrPathReg();
	if (reg.empty()) {
		fprintf(stderr, "SteamVR wasn't found.\n");
		return 1;
	}
	int r = Run(reg, L"removedriver \"" + ExeDir() + L"\\touchlink\"");
	printf(r == 0 ? "TouchLink is no longer registered with SteamVR (restart SteamVR).\n" : "vrpathreg removedriver failed.\n");
	// Give the Rift back to Meta, if setup took it.
	if (FileExists(DataFile(L"rift_taken"))) {
		printf("Giving the Rift back to Meta's software (Windows asks for permission)...\n");
		int a = RunElevated(L"admin on");
		if (a == 0 || a == 3) DeleteFileW(DataFile(L"rift_taken").c_str());
		printf(a == 0 ? "Done.\n" : a == 3 ? "Done; restart the PC to finish.\n" : "That didn't work (or permission was refused).\n");
	}
	return r == 0 ? 0 : 1;
}

bool ReadStatus(std::string* text)
{
	if (!ReadFileText(DataFile(L"status.json"), text)) return false;
	double updated = 0;
	return JsonNumber(*text, "updated", &updated) && time(nullptr) - (time_t)updated < 5;   // the driver is running
}

bool FollowCalibration();

// `touchlink source atrium|meta`: where the Touches come from (config.json "source"): Atrium's
// tracking service (the default), or Meta's runtime (touchlink meta). SteamVR uses it from its next start.
int SetSource(bool meta)
{
	std::string text;
	ReadFileText(DataFile(L"config.json"), &text);
	std::string want = meta ? "\"source\": \"meta\"" : "\"source\": \"atrium\"";
	size_t at = text.find("\"source\"");
	if (at != std::string::npos) {
		size_t open = text.find('"', text.find(':', at));
		size_t close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
		if (close == std::string::npos) return 1;
		text.replace(at, close + 1 - at, want);
	} else {
		size_t end = text.rfind('}');
		if (end == std::string::npos) text = "{\n  " + want + "\n}\n";
		else text.insert(end, ",\n  " + want + "\n");
	}
	if (!WriteFileText(DataFile(L"config.json"), text)) return 1;
	printf("The Touches come from %s (from SteamVR's next start).\n", meta ? "Meta's runtime" : "Atrium's tracking service");
	return 0;
}

int Calibrate()
{
	std::string text;
	if (!ReadStatus(&text)) {
		fprintf(stderr, "The TouchLink driver isn't running: start SteamVR.\n");
		return 1;
	}
	WriteFileText(DataFile(L"calibrate"), "");
	printf("Calibrating: hold a Touch against the side of the headset and look around until it buzzes.\n");
	return FollowCalibration() ? 0 : 1;
}

int Status(bool json)
{
	std::string text;
	bool running = ReadStatus(&text);
	if (json) {
		printf("%s", running ? text.c_str() : "{\"running\": false}\n");
		return running ? 0 : 1;
	}
	Config cfg = LoadConfig();
	std::wstring exe, manifest;
	printf("Tracking service: %s\n", cfg.serviceDir.empty() ? "not set (touchlink install)"
		: ServiceFiles(cfg.serviceDir, &exe, &manifest) ? Narrow(cfg.serviceDir).c_str() : "missing");
	if (ProcessRunning(L"OVRServer_x64.exe")) printf("Warning: Meta's OVRServer is running; it fights the tracking service for the Rift.\n");
	if (!running) {
		printf("Driver: not running (start SteamVR)\n");
		return 1;
	}
	bool b = false;
	double mm = 0;
	std::string s;
	JsonBool(text, "connected", &b);
	printf("Driver: running, %s\n", b ? "connected to the tracking service" : "not connected to the tracking service");
	for (const char* hand : { "left", "right" }) {
		size_t at = text.find(std::string("\"") + hand + "\"");
		std::string part = at == std::string::npos ? "" : text.substr(at, text.find('}', at) - at);
		bool active = false, tracked = false;
		JsonBool(part, "active", &active);
		JsonBool(part, "tracked", &tracked);
		printf("  %-5s Touch: %s\n", hand, !active ? "off" : tracked ? "tracked" : "on, not seen by a sensor");
	}
	JsonBool(text, "calibrated", &b);
	JsonNumber(text, "error_mm", &mm);
	if (b) printf("Calibrated (error %.1f mm)\n", mm);
	else printf("Not calibrated: hold a Touch against the side of the headset and look around until it buzzes\n");
	if (JsonBool(text, "other_universe", &b) && b) printf("Warning: the base stations' space changed since calibrating: calibrate again.\n");
	if (JsonString(text, "message", &s) && !s.empty()) printf("%s\n", s.c_str());
	return 0;
}

// ---- touchlink setup: everything in order, one permission prompt -------------------------------
// The driver's status.json, written after `since` (a SteamVR started before it doesn't count).
bool StatusSince(std::string* text, time_t since)
{
	double updated = 0;
	return ReadStatus(text) && JsonNumber(*text, "updated", &updated) && (time_t)updated >= since;
}

bool HandTracked(const std::string& text, const char* hand, bool* active)
{
	size_t at = text.find(std::string("\"") + hand + "\"");
	std::string part = at == std::string::npos ? "" : text.substr(at, text.find('}', at) - at);
	bool tracked = false;
	*active = false;
	JsonBool(part, "active", active);
	JsonBool(part, "tracked", &tracked);
	return tracked;
}

void RestartSteamVR(bool running)
{
	if (running) {
		printf("  Restarting SteamVR...\n");
		for (const wchar_t* exe : { L"vrmonitor.exe", L"vrcompositor.exe", L"vrdashboard.exe", L"vrwebhelper.exe", L"vrserver.exe", L"vrstartup.exe" })
			KillProcess(exe);
		Sleep(2000);
	} else
		printf("  Starting SteamVR...\n");
	std::wstring exe = SteamVRDir() + L"\\bin\\win64\\vrstartup.exe";
	ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Waits (up to 90 s) for the driver in a SteamVR started at `since`.
bool WaitLoaded(time_t since)
{
	std::string text;
	for (int i = 0; i < 180; i++) {
		if (StatusSince(&text, since)) return true;
		Sleep(500);
	}
	fprintf(stderr, "  TouchLink didn't start in SteamVR. Its log: %s\n", Narrow(DataFile(L"driver.log")).c_str());
	return false;
}

// Shows the driver's calibration progress on one line until it's calibrated (the driver looks
// for a Touch held to the headset by itself; Enter here starts over).
bool FollowCalibration()
{
	std::string text, phase, msg;
	bool seen = false;
	size_t width = 0;
	for (int quiet = 0; quiet < 100;) {
		Sleep(200);
		if (_kbhit() && _getwch() == L'\r') WriteFileText(DataFile(L"calibrate"), "");
		if (!ReadStatus(&text)) {
			quiet++;   // SteamVR gone for 20 s: stop
			continue;
		}
		quiet = 0;
		JsonString(text, "calibrating", &phase);
		JsonString(text, "calibration_message", &msg);
		if (!phase.empty()) {
			seen = true;
			std::string line = "  " + msg;
			printf("\r%s%*s", line.c_str(), (int)(width > line.size() ? width - line.size() : 0), "");
			width = line.size();
		} else if (seen) {
			printf("\r  %s%*s\n", msg.c_str(), (int)(width > msg.size() + 2 ? width - msg.size() - 2 : 0), "");
			return msg.rfind("calibrated", 0) == 0;
		}
	}
	fprintf(stderr, "\n  SteamVR stopped.\n");
	return false;
}

int SetupRest(bool changed);

int Setup(int argc, wchar_t** argv)
{
	printf("TouchLink setup: Rift CV1 Touches in SteamVR, next to your headset.\n\n[1/5] SteamVR driver\n  ");
	if (Install(argc, argv, true)) return 1;

	bool changed = false;
	if (LoadConfig().meta) {
		// The Meta source (touchlink source meta): Meta's runtime tracks the Touches, so it keeps the Rift.
		printf("\n[2/5] Meta's runtime (it tracks the Touches)\n");
		bool winusb = false;
		Sensors(&winusb);
		if (winusb || MetaStartType() == SERVICE_DEMAND_START) {
			printf("  Giving the Rift back to Meta's software. Windows asks for permission once.\n");
			int a = RunElevated(L"admin on");
			if (a == -1) {
				fprintf(stderr, "  Permission was refused. Run setup again and click Yes.\n");
				return 1;
			}
			DeleteFileW(DataFile(L"rift_taken").c_str());
			if (a == 3) {
				printf("  Windows needs a restart to finish the sensors. Restart the PC, then run touchlink again.\n");
				return 1;
			}
			changed = true;
		}
		printf("  Meta's software has the Rift and its sensors.\n");
		return SetupRest(changed);
	}

	printf("\n[2/5] Meta's software and the Rift sensors\n");
	bool ready = false;
	int sensors = Sensors(&ready);
	if (sensors < 0) {
		fprintf(stderr, "  sensordriver.exe isn't next to touchlink.exe.\n");
		return 1;
	}
	if (sensors == 0) {
		fprintf(stderr, "  No Rift sensor is plugged in. Plug them in (and the Rift's USB cable), then run setup again.\n");
		return 1;
	}
	bool meta = MetaInTheWay();
	if (meta || !ready) {
		printf("  %s%s%s. Windows asks for permission once.\n", meta ? "Turning Meta's software off (it stays off after a restart)" : "",
			meta && !ready ? ", and putting the sensors on WinUSB" : "", !meta ? "Putting the sensors on WinUSB" : "");
		int a = RunElevated(L"admin off");
		if (a == -1) {
			fprintf(stderr, "  Permission was refused. Run setup again and click Yes.\n");
			return 1;
		}
		WriteFileText(DataFile(L"rift_taken"), "");
		if (a == 3) {
			printf("  Windows needs a restart to finish the sensors. Restart the PC, then run touchlink again.\n");
			return 1;
		}
		Sensors(&ready);
		if (!ready || ProcessRunning(L"OVRServer_x64.exe")) {
			fprintf(stderr, "  That didn't finish: %s. Unplug the sensors, plug them back in and run setup again.\n",
				!ready ? "the sensors aren't on WinUSB yet" : "Meta's OVRServer is still running");
			return 1;
		}
		changed = true;
	}
	printf("  Meta's software is off; %d sensor%s ready.\n", sensors, sensors == 1 ? "" : "s");
	if (ProcessRunning(L"atrium.exe"))
		printf("  Note: the Atrium app is running. With a Rift plugged in it starts Meta's runtime again.\n"
			   "  Close it (its tray icon too); TouchLink doesn't need it.\n");
	return SetupRest(changed);
}

// Setup from SteamVR on: (re)started if anything changed, tracking, calibration.
int SetupRest(bool changed)
{
	printf("\n[3/5] SteamVR\n");
	std::string text;
	bool running = ProcessRunning(L"vrserver.exe");
	time_t since = 0;
	if (!running || changed || !ReadStatus(&text)) {
		since = time(nullptr);
		RestartSteamVR(running);
	}
	if (!WaitLoaded(since)) return 1;
	printf("  TouchLink is running in SteamVR.\n");

	printf("\n[4/5] Tracking\n  Turn both Touches on, put the headset on, and stand where the sensors and base stations see you.\n");
	std::string shown;
	ULONGLONG start = GetTickCount64(), unseen = 0;
	bool hinted = false, replugged = false;
	for (;;) {
		if (ReadStatus(&text)) {
			bool conn = false, hmd = false, la, ra;
			JsonBool(text, "connected", &conn);
			JsonBool(text, "hmd_tracked", &hmd);
			bool lt = HandTracked(text, "left", &la), rt = HandTracked(text, "right", &ra);
			auto touch = [](bool active, bool tracked) { return !active ? "off" : tracked ? "seen" : "not seen yet"; };
			char line[256];
			snprintf(line, sizeof line, "  Tracking service: %s | Headset: %s | Left Touch: %s | Right Touch: %s",
				conn ? "on" : "starting", hmd ? "tracked" : "not tracked", touch(la, lt), touch(ra, rt));
			if (line != shown) {
				printf("%s\n", line);
				shown = line;
			}
			if (conn && hmd && (lt || rt)) break;
			// Touches on (the radio works) but no sensor sees them for 20 s: the sensors want a
			// replug after their switch. Done in software once, then the tracking service again.
			unseen = conn && (la || ra) && !lt && !rt ? (unseen ? unseen : GetTickCount64()) : 0;
			if (!replugged && !LoadConfig().meta && unseen && GetTickCount64() - unseen > 20000) {
				replugged = true;
				printf("  The sensors don't see the Touches. Replugging them (Windows asks for permission)...\n");
				if (RunElevated(L"admin replug") != 0)
					printf("  That didn't work: unplug the sensors, plug them back in, and run touchlink again.\n");
				since = time(nullptr);
				RestartSteamVR(true);
				if (!WaitLoaded(since)) return 1;
				shown.clear();
				start = GetTickCount64();
				unseen = 0;
				continue;
			}
			if (!hinted && GetTickCount64() - start > 30000) {
				hinted = true;
				if (!conn) printf("  The tracking service hasn't started: see %s\n", Narrow(DataFile(L"service.log")).c_str());
				if (!hmd) printf("  The headset isn't tracked: wake it (put it on) and check SteamVR sees the base stations.\n");
				if (conn && !la && !ra) printf("  The Touches are off: press a button on each. They talk through the Rift, so its USB cable must be in.\n");
				else if (conn && !lt && !rt)
					printf("  No sensor sees the Touches: face a sensor, and check the sensors are plugged in. The service's log: %s\n",
						Narrow(DataFile(L"service.log")).c_str());
			}
		}
		Sleep(500);
	}

	printf("\n[5/5] Calibration (once; again only if the sensors or base stations move)\n");
	bool cal = false, other = false;
	double mm = 0;
	JsonBool(text, "calibrated", &cal);
	JsonBool(text, "other_universe", &other);
	JsonNumber(text, "error_mm", &mm);
	if (cal && !other) {
		printf("  Already calibrated (error %.1f mm). To redo it, press X+Y+A+B.\n", mm);
	} else {
		printf("  Hold either Touch flat and firm against the side of the headset (the side facing the sensors),\n"
			   "  then slowly look left, right, up, down, and tilt your head, until both Touches buzz.\n"
			   "  No buttons, no hurry: it keeps trying until it fits.\n");
		if (!FollowCalibration()) return 1;
	}
	printf("\nDone. The Touches are your SteamVR controllers. Check any time with: touchlink status\n");
	return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
	std::wstring cmd = argc > 1 ? argv[1] : L"";
	int r = -1;
	if (cmd.empty() || cmd == L"setup") r = Setup(argc, argv);
	else if (cmd == L"camera") return CameraMain();   // started by the driver
	else if (cmd == L"meta") return MetaMain();       // started by the driver ("source": "meta")
	else if (cmd == L"meta-poke") return MetaPokeMain();   // run by touchlink meta
	else if (cmd == L"source" && argc > 2) r = SetSource(!wcscmp(argv[2], L"meta"));
	else if (cmd == L"admin" && argc > 2 && !wcscmp(argv[2], L"replug")) return SensorDriver().empty() ? 1 : Run(SensorDriver(), L"replug");
	else if (cmd == L"admin" && argc > 2) return Admin(!wcscmp(argv[2], L"on"));
	else if (cmd == L"install") r = Install(argc, argv);
	else if (cmd == L"uninstall") r = Uninstall();
	else if (cmd == L"calibrate") r = Calibrate();
	else if (cmd == L"status") r = Status(argc > 2 && !wcscmp(argv[2], L"--json"));
	else {
		printf("TouchLink: Rift CV1 Touches in SteamVR with any headset.\n\n"
			   "  touchlink [setup] [--service DIR]   everything: driver, Meta off, sensors, SteamVR, calibration\n"
			   "  touchlink install [--service DIR]   only register the driver with SteamVR\n"
			   "  touchlink uninstall                 unregister it, give the Rift back to Meta\n"
			   "  touchlink calibrate                 line the Touches up with the base stations\n"
			   "  touchlink status [--json]           what the driver sees\n");
		return cmd == L"help" || cmd == L"--help" ? 0 : 1;
	}
	// Started from Explorer (a console of its own): keep the window open to read.
	DWORD procs[2];
	if (GetConsoleProcessList(procs, 2) == 1) {
		printf("\nPress Enter to close.");
		getchar();
	}
	return r;
}
