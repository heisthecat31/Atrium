// riftprobe: read a Rift CV1 directly over HID, with no Meta software involved, and track its
// orientation from the IMU. The first piece of Atrium's own Rift driver.
//
// Read-only: it never sends a feature report, so it can run next to Meta's service (which keeps
// the headset awake) without changing anything. Packet formats follow OpenHMD's Rift driver
// (Boost Software License 1.0, github.com/OpenHMD/OpenHMD, src/drv_oculus_rift).
//
//   riftprobe          info + 10 s of orientation (read-only)
//   riftprobe info     configuration reports only
//   riftprobe touch    Touch controllers through the radio (read-only)
//   riftprobe drive    Atrium drives the headset itself: display/audio/LEDs on, keep-alive,
//                      15 s of orientation, then the displays Windows sees. Only with Meta's
//                      service stopped: two programs keeping the headset alive would fight.
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "user32.lib")

static const wchar_t* kCv1Sensor = L"vid_2833&pid_0031&mi_00";

// ---- HID ------------------------------------------------------------------------------------
struct Hid {
	HANDLE h = INVALID_HANDLE_VALUE;
	USHORT inLen = 0, featLen = 0;
	std::wstring path;
};

static bool OpenHid(const wchar_t* match, Hid& out)
{
	GUID guid;
	HidD_GetHidGuid(&guid);
	HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) return false;
	SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
	bool found = false;
	for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifd); i++) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
		std::vector<BYTE> buf(need);
		auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
		detail->cbSize = sizeof(*detail);
		if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;
		std::wstring path = detail->DevicePath;
		std::wstring lower = path;
		for (auto& c : lower) c = towlower(c);
		if (lower.find(match) == std::wstring::npos) continue;
		// Shared with Meta's service: read and write share, no exclusive access.
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			printf("found %ls but couldn't open it (error %lu)\n", path.c_str(), GetLastError());
			continue;
		}
		PHIDP_PREPARSED_DATA pp;
		HIDP_CAPS caps = {};
		if (HidD_GetPreparsedData(h, &pp)) {
			HidP_GetCaps(pp, &caps);
			HidD_FreePreparsedData(pp);
		}
		out.h = h;
		out.inLen = caps.InputReportByteLength;
		out.featLen = caps.FeatureReportByteLength;
		out.path = path;
		found = true;
	}
	SetupDiDestroyDeviceInfoList(set);
	return found;
}

static int GetFeature(Hid& d, uint8_t id, std::vector<uint8_t>& buf)
{
	buf.assign(d.featLen, 0);
	buf[0] = id;
	if (!HidD_GetFeature(d.h, buf.data(), (ULONG)buf.size())) return -1;
	return (int)buf.size();
}

static bool SetFeature(Hid& d, const uint8_t* data, size_t len)
{
	std::vector<uint8_t> buf(d.featLen > len ? d.featLen : len, 0);
	memcpy(buf.data(), data, len);
	return HidD_SetFeature(d.h, buf.data(), (ULONG)buf.size()) != FALSE;
}

// The displays Windows knows about, active or not (the Rift's panel shows up as a monitor
// on the GPU's HDMI output once the headset powers it).
static void ListDisplays()
{
	DISPLAY_DEVICEW adapter = { sizeof(adapter) };
	for (DWORD a = 0; EnumDisplayDevicesW(nullptr, a, &adapter, 0); a++) {
		DISPLAY_DEVICEW mon = { sizeof(mon) };
		for (DWORD m = 0; EnumDisplayDevicesW(adapter.DeviceName, m, &mon, 0); m++)
			printf("  %ls -> %ls  [%ls]%s\n", adapter.DeviceName, mon.DeviceString, mon.DeviceID,
				(mon.StateFlags & DISPLAY_DEVICE_ACTIVE) ? " active" : "");
	}
}

static uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t U32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static float Fixed(const uint8_t* p) { return (float)(int32_t)U32(p) / 1000000.0f; }

// Three 21-bit signed values packed into 8 bytes (OpenHMD packet.c, decode_sample).
static void DecodeSample(const uint8_t* b, int32_t* s)
{
	int x = (b[0] << 24) | (b[1] << 16) | ((b[2] & 0xF8) << 8);
	int y = ((b[2] & 0x07) << 29) | (b[3] << 21) | (b[4] << 13) | ((b[5] & 0xC0) << 5);
	int z = ((b[5] & 0x3F) << 26) | (b[6] << 18) | (b[7] << 10);
	s[0] = x >> 11;
	s[1] = y >> 11;
	s[2] = z >> 11;
}

// ---- orientation ----------------------------------------------------------------------------
struct Quat { double w = 1, x = 0, y = 0, z = 0; };

static Quat Mul(const Quat& a, const Quat& b)
{
	return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	         a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	         a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	         a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}

static void Normalize(Quat& q)
{
	double n = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	q.w /= n; q.x /= n; q.y /= n; q.z /= n;
}

// Rotate v by q.
static void Rotate(const Quat& q, const double v[3], double out[3])
{
	Quat p = { 0, v[0], v[1], v[2] };
	Quat c = { q.w, -q.x, -q.y, -q.z };
	Quat r = Mul(Mul(q, p), c);
	out[0] = r.x; out[1] = r.y; out[2] = r.z;
}

// Gyro integration with a gentle pull of the measured "up" towards world +Y (tilt only; yaw
// drifts slowly without the magnetometer or optical tracking).
struct Fusion {
	Quat q;
	bool started = false;
	void Update(const double gyro[3], const double accel[3], double dt)
	{
		double w = sqrt(gyro[0] * gyro[0] + gyro[1] * gyro[1] + gyro[2] * gyro[2]);
		if (w > 1e-9) {
			double half = w * dt * 0.5, s = sin(half) / w;
			Quat d = { cos(half), gyro[0] * s, gyro[1] * s, gyro[2] * s };
			q = Mul(q, d);
		}
		double a = sqrt(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
		if (a > 8.0 && a < 11.6) {   // only when the headset isn't accelerating much
			double up[3];
			double n[3] = { accel[0] / a, accel[1] / a, accel[2] / a };
			Rotate(q, n, up);
			// axis = up x (0,1,0), angle = acos(up.y): rotate world so up -> +Y, a little at a time
			double ax = -up[2], az = up[0];
			double len = sqrt(ax * ax + az * az);
			if (len > 1e-6) {
				double angle = atan2(len, up[1]) * (started ? 0.02 : 1.0);
				double s = sin(angle * 0.5) / len;
				Quat corr = { cos(angle * 0.5), ax * s, 0, az * s };
				q = Mul(corr, q);
			}
			started = true;
		}
		Normalize(q);
	}
	void Euler(double& yaw, double& pitch, double& roll) const
	{
		const double r2d = 180.0 / 3.14159265358979;
		yaw = atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.x * q.x)) * r2d;
		pitch = asin(fmax(-1.0, fmin(1.0, 2 * (q.w * q.x - q.z * q.y)))) * r2d;
		roll = atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.x * q.x + q.z * q.z)) * r2d;
	}
};

// Touch controllers report through the headset's radio (CV1 interface 1): report 0x0c, 64 bytes,
// up to two 28-byte messages (OpenHMD packet.c, decode_rift_radio_report).
static int Touch()
{
	Hid r;
	if (!OpenHid(L"vid_2833&pid_0031&mi_01", r)) {
		printf("No Rift CV1 radio interface found (MI_01).\n");
		return 1;
	}
	printf("Rift CV1 radio: %ls\nReading the Touch controllers for 10 s (read-only). Press buttons, pull triggers.\n", r.path.c_str());
	std::vector<uint8_t> in(r.inLen ? r.inLen : 64);
	ULONGLONG start = GetTickCount64(), lastPrint[2] = {};
	int msgs[2] = {};
	while (GetTickCount64() - start < 10000) {
		DWORD got = 0;
		if (!ReadFile(r.h, in.data(), (DWORD)in.size(), &got, nullptr)) {
			printf("read failed (error %lu)\n", GetLastError());
			return 1;
		}
		if (got < 64 || in[0] != 0x0c) continue;
		for (int m = 0; m < 2; m++) {
			const uint8_t* p = in.data() + 3 + m * 28;
			uint16_t flags = U16(p);
			uint8_t type = p[2];
			if (flags != 0x1c || (type != 2 && type != 3)) continue;   // 2 left, 3 right
			int hand = type == 2 ? 0 : 1;
			msgs[hand]++;
			const uint8_t* q = p + 3;
			int16_t accel[3], gyro[3];
			for (int k = 0; k < 3; k++) accel[k] = (int16_t)U16(q + 4 + k * 2);
			for (int k = 0; k < 3; k++) gyro[k] = (int16_t)U16(q + 10 + k * 2);
			uint8_t buttons = q[16];
			const uint8_t* t = q + 17;
			int trigger = t[0] | ((t[1] & 0x03) << 8);
			int grip = ((t[1] & 0xfc) >> 2) | ((t[2] & 0x0f) << 6);
			int sx = ((t[2] & 0xf0) >> 4) | ((t[3] & 0x3f) << 4);
			int sy = ((t[3] & 0xc0) >> 6) | (t[4] << 2);
			ULONGLONG now = GetTickCount64();
			if (now - lastPrint[hand] >= 300) {
				lastPrint[hand] = now;
				printf("%s  buttons 0x%02x  trigger %4d  grip %4d  stick %4d %4d  accel %+6d %+6d %+6d  gyro %+6d %+6d %+6d\n",
					hand ? "right" : "left ", buttons, trigger, grip, sx, sy, accel[0], accel[1], accel[2], gyro[0], gyro[1], gyro[2]);
			}
		}
	}
	printf("messages in 10 s: left %d, right %d\n", msgs[0], msgs[1]);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc > 1 && !strcmp(argv[1], "touch"))
		return Touch();
	Hid d;
	if (!OpenHid(kCv1Sensor, d)) {
		printf("No Rift CV1 sensor interface found (VID 2833 PID 0031 MI_00).\n");
		return 1;
	}
	printf("Rift CV1: %ls\n  input report %u bytes, feature report %u bytes\n", d.path.c_str(), d.inLen, d.featLen);

	std::vector<uint8_t> f;
	if (GetFeature(d, 4, f) > 0)
		printf("Range:          accel %u g, gyro %u deg/s, mag %u\n", f[3], U16(&f[4]), U16(&f[6]));
	if (GetFeature(d, 2, f) > 0)
		printf("Sensor config:  flags 0x%02x, packet interval %u, keep-alive %u ms\n", f[3], f[4], U16(&f[5]));
	if (GetFeature(d, 9, f) > 0)
		printf("Display info:   %ux%u, screen %.4f x %.4f m, lens separation %.4f m, eye to screen %.4f/%.4f m\n",
			U16(&f[4]), U16(&f[6]), Fixed(&f[8]), Fixed(&f[12]), Fixed(&f[20]), Fixed(&f[24]), Fixed(&f[28]));
	if (argc > 1 && !strcmp(argv[1], "info")) return 0;

	const bool drive = argc > 1 && !strcmp(argv[1], "drive");
	const uint8_t keepAlive[] = { 0x08, 0, 0, 0x10, 0x27 };   // keep alive for 10000 ms
	if (drive) {
		const uint8_t enable[] = { 0x1d, 0, 0, 1 | 2 | 4 };   // display | audio | LEDs
		printf("Enable display/audio/LEDs: %s\n", SetFeature(d, enable, sizeof enable) ? "ok" : "FAILED");
		printf("Keep-alive: %s\n", SetFeature(d, keepAlive, sizeof keepAlive) ? "ok" : "FAILED");
		// Sensor config: same flags, packet interval 1 = full rate (the headset defaults to 19,
		// about 100 samples/s; Meta's service sets 1), keep-alive 10000 ms.
		const uint8_t config[] = { 0x02, 0, 0, 0x20, 1, 0x10, 0x27 };
		printf("Sensor config (full rate): %s\n", SetFeature(d, config, sizeof config) ? "ok" : "FAILED");
	}

	// IMU stream: report 11, 64 bytes; up to 2 samples per report, timestamps in microseconds.
	Fusion fusion;
	std::vector<uint8_t> in(d.inLen ? d.inLen : 64);
	uint32_t lastTs = 0;
	ULONGLONG start = GetTickCount64(), lastPrint = 0;
	int reports = 0, samples = 0;
	double accel[3] = {}, gyro[3] = {};
	int16_t temp = 0;
	const ULONGLONG duration = drive ? 15000 : 10000;
	ULONGLONG lastKeepAlive = start;
	printf("Reading the IMU for %llu s%s. Move the headset.\n", duration / 1000, drive ? "" : " (read-only)");
	while (GetTickCount64() - start < duration) {
		if (drive && GetTickCount64() - lastKeepAlive > 8000) {
			SetFeature(d, keepAlive, sizeof keepAlive);
			lastKeepAlive = GetTickCount64();
		}
		DWORD got = 0;
		if (!ReadFile(d.h, in.data(), (DWORD)in.size(), &got, nullptr)) {
			printf("read failed (error %lu)\n", GetLastError());
			break;
		}
		if (got < 64 || in[0] != 11) continue;
		const uint8_t* b = in.data() + 3;   // report id, then a 16-bit command id
		int n = b[0] < 2 ? b[0] : 2;
		temp = (int16_t)U16(b + 3);
		uint32_t ts = U32(b + 5);
		const uint8_t* s = b + 9;
		double dt = lastTs && ts > lastTs ? (ts - lastTs) / 1e6 / n : 0.001;
		for (int i = 0; i < n; i++, s += 16) {
			int32_t a[3], g[3];
			DecodeSample(s, a);
			DecodeSample(s + 8, g);
			for (int k = 0; k < 3; k++) { accel[k] = a[k] * 1e-4; gyro[k] = g[k] * 1e-4; }
			fusion.Update(gyro, accel, dt);
			samples++;
		}
		lastTs = ts;
		reports++;
		ULONGLONG now = GetTickCount64();
		if (now - lastPrint >= 250) {
			lastPrint = now;
			double yaw, pitch, roll;
			fusion.Euler(yaw, pitch, roll);
			printf("yaw %+7.1f pitch %+6.1f roll %+6.1f | accel %+6.2f %+6.2f %+6.2f m/s2 | gyro %+6.2f %+6.2f %+6.2f rad/s | %.1f C\n",
				yaw, pitch, roll, accel[0], accel[1], accel[2], gyro[0], gyro[1], gyro[2], temp / 100.0);
		}
	}
	double secs = (GetTickCount64() - start) / 1000.0;
	printf("%d reports, %d samples in %.1f s (%.0f samples/s)\n", reports, samples, secs, samples / secs);
	if (drive) {
		printf("Displays Windows sees now:\n");
		ListDisplays();
	}
	CloseHandle(d.h);
	return 0;
}
