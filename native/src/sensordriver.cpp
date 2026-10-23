// sensordriver: puts the Rift CV1 sensors on Windows' own WinUSB driver so Atrium's sensor code
// (libusb) can open them, or gives them back to Meta's driver.
//
//   sensordriver status     one line per sensor: "<instance id> <driver inf> <winusb|meta|other>",
//                           then "usb3-lpm on|off"
//   sensordriver winusb     (admin) every sensor onto WinUSB, USB 3 link power management off
//   sensordriver restore    (admin) every sensor back to the best driver Windows has (Meta's),
//                           USB 3 link power management back to what it was
//
// WinUSB comes from Windows' own signed package (C:\Windows\INF\winusb.inf, "WinUsb Device"),
// chosen for the sensor the way Device Manager's "pick from a list" does: no driver of Atrium's
// own, no certificates. Meta's package stays in the driver store, so "restore" brings it back.
// Exit code: 0 ok, 1 failed, 2 nothing to do (no sensor), 3 needs a reboot.
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <stdio.h>
#include <wchar.h>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "powrprof.lib")

#include <powrprof.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// The sensor's one interface (vendor class); its parent composite device keeps Windows' driver.
static const wchar_t kSensorId[] = L"USB\\VID_2833&PID_0211&MI_00";
static const wchar_t kUsbDeviceClass[] = L"{88BAE032-5A81-49f0-BC3D-A4FF138216D6}";
// The device interface WinUSB registers for the sensor, which libusb opens it by. winusb.inf only
// writes one for ADB devices; others bring theirs in descriptors, the sensors don't. Atrium's own.
static const wchar_t kInterfaceGuids[] = L"{6D0B3F4C-8E2A-4B71-A5C9-2F1E7D3B9A60}\0";

struct Sensor { std::wstring id; std::wstring inf; };

// USB 3 link power management (the power plan's "USB 3 Link Power Management"). With it on, a
// sensor behind a USB 3 hub can't wake its link: the first request after setup is aborted and the
// hub resets the port, so the sensor never answers (seen with a Genesys hub, 2026-10-23). Sensors
// on the PC's own ports don't mind. "winusb" turns it off in the active plan and saves the old
// value; "restore" puts it back.
static const GUID kUsbSubgroup = { 0x2a737441, 0x1930, 0x4402, { 0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3 } };
static const GUID kUsb3Lpm = { 0xd4e98f31, 0x5ffe, 0x4ce1, { 0xbe, 0x31, 0x1b, 0x38, 0xb3, 0x84, 0xc0, 0x09 } };

static std::wstring LpmSaveFile()
{
	PWSTR data = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &data))) return {};
	std::wstring dir = std::wstring(data) + L"\\Atrium";
	CoTaskMemFree(data);
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir + L"\\usb3_lpm_before.txt";
}

// The active plan's USB 3 LPM index (0 off .. 3 max savings) on AC and DC; false if unreadable.
static bool ReadLpm(DWORD* ac, DWORD* dc)
{
	GUID* scheme = nullptr;
	if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS) return false;
	bool ok = PowerReadACValueIndex(nullptr, scheme, &kUsbSubgroup, &kUsb3Lpm, ac) == ERROR_SUCCESS &&
		PowerReadDCValueIndex(nullptr, scheme, &kUsbSubgroup, &kUsb3Lpm, dc) == ERROR_SUCCESS;
	LocalFree(scheme);
	return ok;
}

static bool WriteLpm(DWORD ac, DWORD dc)
{
	GUID* scheme = nullptr;
	if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS) return false;
	bool ok = PowerWriteACValueIndex(nullptr, scheme, &kUsbSubgroup, &kUsb3Lpm, ac) == ERROR_SUCCESS &&
		PowerWriteDCValueIndex(nullptr, scheme, &kUsbSubgroup, &kUsb3Lpm, dc) == ERROR_SUCCESS &&
		PowerSetActiveScheme(nullptr, scheme) == ERROR_SUCCESS;
	LocalFree(scheme);
	return ok;
}

static bool LpmOff()
{
	DWORD ac, dc;
	if (!ReadLpm(&ac, &dc)) {
		printf("USB 3 link power management: can't read the power plan\n");
		return false;
	}
	if (ac == 0 && dc == 0) return true;
	std::wstring save = LpmSaveFile();
	FILE* f = nullptr;
	if (!save.empty() && GetFileAttributesW(save.c_str()) == INVALID_FILE_ATTRIBUTES && _wfopen_s(&f, save.c_str(), L"w") == 0 && f) {
		fprintf(f, "%lu %lu\n", ac, dc);
		fclose(f);
	}
	if (!WriteLpm(0, 0)) {
		printf("USB 3 link power management: can't change the power plan (%lu)\n", GetLastError());
		return false;
	}
	printf("USB 3 link power management: off (was %lu on AC, %lu on battery)\n", ac, dc);
	return true;
}

static void LpmRestore()
{
	std::wstring save = LpmSaveFile();
	FILE* f = nullptr;
	if (save.empty() || _wfopen_s(&f, save.c_str(), L"r") || !f) return;
	unsigned long ac = 2, dc = 2;
	int n = fscanf_s(f, "%lu %lu", &ac, &dc);
	fclose(f);
	if (n == 2 && WriteLpm(ac, dc)) {
		DeleteFileW(save.c_str());
		printf("USB 3 link power management: back to %lu on AC, %lu on battery\n", ac, dc);
	}
}

static std::wstring Prop(HDEVINFO set, SP_DEVINFO_DATA* dev, DWORD prop)
{
	wchar_t buf[2048] = {};
	if (!SetupDiGetDeviceRegistryPropertyW(set, dev, prop, nullptr, (BYTE*)buf, sizeof buf - sizeof(wchar_t), nullptr)) return {};
	return buf;
}

static bool HasHardwareId(HDEVINFO set, SP_DEVINFO_DATA* dev, const wchar_t* id)
{
	wchar_t buf[2048] = {};
	if (!SetupDiGetDeviceRegistryPropertyW(set, dev, SPDRP_HARDWAREID, nullptr, (BYTE*)buf, sizeof buf - 2 * sizeof(wchar_t), nullptr)) return false;
	for (const wchar_t* p = buf; *p; p += wcslen(p) + 1)
		if (_wcsicmp(p, id) == 0) return true;
	return false;
}

static std::wstring DriverInf(HDEVINFO set, SP_DEVINFO_DATA* dev)
{
	HKEY key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
	if (key == INVALID_HANDLE_VALUE) return {};
	wchar_t inf[MAX_PATH] = {};
	DWORD size = sizeof inf - sizeof(wchar_t);
	RegQueryValueExW(key, L"InfPath", nullptr, nullptr, (BYTE*)inf, &size);
	RegCloseKey(key);
	return inf;
}

static bool HasInterfaceGuid(HDEVINFO set, SP_DEVINFO_DATA* dev)
{
	HKEY key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
	if (key == INVALID_HANDLE_VALUE) return false;
	bool has = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
	RegCloseKey(key);
	return has;
}

// Gives a sensor its interface GUID; WinUSB registers the interface when the sensor (re)starts,
// so a sensor already on WinUSB is restarted.
static bool SetInterfaceGuid(HDEVINFO set, SP_DEVINFO_DATA* dev, bool restart, bool* reboot)
{
	HKEY key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_SET_VALUE);
	if (key == INVALID_HANDLE_VALUE) key = SetupDiCreateDevRegKeyW(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
	if (key == INVALID_HANDLE_VALUE) {
		printf("  can't open the device's registry key (%lu)\n", GetLastError());
		return false;
	}
	LONG r = RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE*)kInterfaceGuids, sizeof kInterfaceGuids);
	RegCloseKey(key);
	if (r != ERROR_SUCCESS) {
		printf("  can't write the interface GUID (%ld)\n", r);
		return false;
	}
	if (!restart) return true;
	SP_PROPCHANGE_PARAMS pc = { { sizeof(SP_CLASSINSTALL_HEADER), DIF_PROPERTYCHANGE }, DICS_PROPCHANGE, DICS_FLAG_GLOBAL, 0 };
	if (!SetupDiSetClassInstallParamsW(set, dev, &pc.ClassInstallHeader, sizeof pc) || !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, dev)) {
		printf("  restarting the sensor failed (%lu): unplug it and plug it back in\n", GetLastError());
		*reboot = true;
	}
	return true;
}

static const wchar_t* Kind(const Sensor& s)
{
	if (_wcsicmp(s.inf.c_str(), L"winusb.inf") == 0) return L"winusb";
	if (_wcsnicmp(s.inf.c_str(), L"oem", 3) == 0) return L"meta";
	return s.inf.empty() ? L"none" : L"other";
}

// Every present sensor; the set stays open for installing.
static std::vector<SP_DEVINFO_DATA> FindSensors(HDEVINFO set, std::vector<Sensor>* out)
{
	std::vector<SP_DEVINFO_DATA> devs;
	SP_DEVINFO_DATA dev = { sizeof dev };
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
		if (!HasHardwareId(set, &dev, kSensorId)) continue;
		wchar_t id[MAX_DEVICE_ID_LEN] = {};
		SetupDiGetDeviceInstanceIdW(set, &dev, id, MAX_DEVICE_ID_LEN, nullptr);
		out->push_back({ id, DriverInf(set, &dev) });
		devs.push_back(dev);
	}
	return devs;
}

// Installs WinUSB from winusb.inf on one sensor.
static bool ToWinUsb(HDEVINFO set, SP_DEVINFO_DATA* dev, bool* reboot)
{
	// The class driver list is built for the device's class, so it becomes USBDevice first
	// (installing WinUSB would set that anyway); then winusb.inf is the only INF looked at.
	std::wstring oldClass = Prop(set, dev, SPDRP_CLASSGUID);
	if (!SetupDiSetDeviceRegistryPropertyW(set, dev, SPDRP_CLASSGUID, (const BYTE*)kUsbDeviceClass, sizeof kUsbDeviceClass)) {
		printf("  can't set the device class (%lu)\n", GetLastError());
		return false;
	}
	SP_DEVINFO_DATA fresh = { sizeof fresh };   // re-read so the element carries the new class
	wchar_t id[MAX_DEVICE_ID_LEN] = {};
	SetupDiGetDeviceInstanceIdW(set, dev, id, MAX_DEVICE_ID_LEN, nullptr);
	HDEVINFO one = SetupDiCreateDeviceInfoList(nullptr, nullptr);
	bool ok = one != INVALID_HANDLE_VALUE && SetupDiOpenDeviceInfoW(one, id, nullptr, 0, &fresh);

	SP_DEVINSTALL_PARAMS_W params = { sizeof params };
	ok = ok && SetupDiGetDeviceInstallParamsW(one, &fresh, &params);
	if (ok) {
		params.Flags |= DI_ENUMSINGLEINF;
		params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
		GetWindowsDirectoryW(params.DriverPath, MAX_PATH);
		wcscat_s(params.DriverPath, L"\\INF\\winusb.inf");
		ok = SetupDiSetDeviceInstallParamsW(one, &fresh, &params) && SetupDiBuildDriverInfoList(one, &fresh, SPDIT_CLASSDRIVER);
	}
	SP_DRVINFO_DATA_W drv = { sizeof drv };
	bool found = false;
	for (DWORD i = 0; ok && SetupDiEnumDriverInfoW(one, &fresh, SPDIT_CLASSDRIVER, i, &drv); i++) {
		if (wcscmp(drv.Description, L"WinUsb Device") == 0) {
			found = true;
			break;
		}
	}
	BOOL needReboot = FALSE;
	if (found) {
		ok = SetupDiSetSelectedDriverW(one, &fresh, &drv) && DiInstallDevice(nullptr, one, &fresh, &drv, 0, &needReboot);
		if (!ok) printf("  installing WinUSB failed (%lu)\n", GetLastError());
	} else {
		printf("  Windows' WinUsb Device driver wasn't found (%lu)\n", GetLastError());
		ok = false;
	}
	if (one != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(one);
	if (!ok && !oldClass.empty())
		SetupDiSetDeviceRegistryPropertyW(set, dev, SPDRP_CLASSGUID, (const BYTE*)oldClass.c_str(), (DWORD)((oldClass.size() + 1) * sizeof(wchar_t)));
	*reboot = *reboot || needReboot;
	return ok;
}

// Removes the sensor's device node; the rescan installs the best driver Windows has (Meta's).
static bool Restore(HDEVINFO set, SP_DEVINFO_DATA* dev, bool* reboot)
{
	BOOL needReboot = FALSE;
	if (!DiUninstallDevice(nullptr, set, dev, 0, &needReboot)) {
		printf("  removing the device failed (%lu)\n", GetLastError());
		return false;
	}
	*reboot = *reboot || needReboot;
	return true;
}

int wmain(int argc, wchar_t** argv)
{
	const wchar_t* mode = argc > 1 ? argv[1] : L"status";
	HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE) return 1;
	std::vector<Sensor> sensors;
	std::vector<SP_DEVINFO_DATA> devs = FindSensors(set, &sensors);
	if (sensors.empty()) {
		printf("no Rift sensor connected\n");
		SetupDiDestroyDeviceInfoList(set);
		return 2;
	}

	int failed = 0;
	bool reboot = false;
	for (size_t i = 0; i < sensors.size(); i++) {
		const Sensor& s = sensors[i];
		bool winusb = wcscmp(Kind(s), L"winusb") == 0;
		bool ready = winusb && HasInterfaceGuid(set, &devs[i]);
		if (wcscmp(mode, L"status") == 0) {
			// "winusb" only once libusb can open it (interface GUID set); "winusb-noguid" otherwise.
			printf("%ls %ls %ls\n", s.id.c_str(), s.inf.empty() ? L"-" : s.inf.c_str(), winusb && !ready ? L"winusb-noguid" : Kind(s));
		} else if (wcscmp(mode, L"winusb") == 0) {
			if (ready) {
				printf("%ls already on WinUSB\n", s.id.c_str());
				continue;
			}
			printf("%ls: %ls -> WinUSB\n", s.id.c_str(), s.inf.c_str());
			if (!SetInterfaceGuid(set, &devs[i], winusb, &reboot) || (!winusb && !ToWinUsb(set, &devs[i], &reboot))) failed++;
		} else if (wcscmp(mode, L"restore") == 0) {
			if (wcscmp(Kind(s), L"meta") == 0) {
				printf("%ls already on Meta's driver\n", s.id.c_str());
				continue;
			}
			printf("%ls: %ls -> Windows' choice\n", s.id.c_str(), s.inf.c_str());
			if (!Restore(set, &devs[i], &reboot)) failed++;
		} else {
			printf("usage: sensordriver status|winusb|restore\n");
			SetupDiDestroyDeviceInfoList(set);
			return 1;
		}
	}
	SetupDiDestroyDeviceInfoList(set);
	if (wcscmp(mode, L"status") == 0) {
		DWORD ac, dc;
		if (ReadLpm(&ac, &dc)) printf("usb3-lpm %s\n", ac == 0 && dc == 0 ? "off" : "on");
	} else if (wcscmp(mode, L"winusb") == 0) {
		if (!LpmOff()) failed++;
	} else if (wcscmp(mode, L"restore") == 0) {
		LpmRestore();
		DEVINST root;
		if (CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
	}
	if (failed) return 1;
	return reboot ? 3 : 0;
}
