// riftprobe: read a Rift CV1 directly over HID, with no Meta software involved, and track its
// orientation from the IMU. The first piece of Atrium's own Rift driver.
//
// Read-only: it never sends a feature report, so it can run next to Meta's service (which keeps
// the headset awake) without changing anything. Packet formats follow OpenHMD's Rift driver
// (Boost Software License 1.0, github.com/OpenHMD/OpenHMD, src/drv_oculus_rift).
//
//   riftprobe          info + 10 s of orientation
//   riftprobe info     configuration reports only
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

int main(int argc, char** argv)
{
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

	// IMU stream: report 11, 64 bytes; up to 2 samples per report, timestamps in microseconds.
	Fusion fusion;
	std::vector<uint8_t> in(d.inLen ? d.inLen : 64);
	uint32_t lastTs = 0;
	ULONGLONG start = GetTickCount64(), lastPrint = 0;
	int reports = 0, samples = 0;
	double accel[3] = {}, gyro[3] = {};
	int16_t temp = 0;
	printf("Reading the IMU for 10 s (read-only). Move the headset.\n");
	while (GetTickCount64() - start < 10000) {
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
	CloseHandle(d.h);
	return 0;
}
