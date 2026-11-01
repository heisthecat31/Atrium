// TouchLink: shared bits (math, files, a tiny JSON reader) for the SteamVR driver and touchlink.exe.
#pragma once

#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <unknwn.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>

namespace tl {

// ---- math -------------------------------------------------------------------------------------
inline XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
inline XrQuaternionf QConj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }
inline XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v)
{
	XrVector3f t = { 2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x) };
	return { v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
		v.z + q.w * t.z + (q.x * t.y - q.y * t.x) };
}
inline XrVector3f VAdd(const XrVector3f& a, const XrVector3f& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline XrVector3f VSub(const XrVector3f& a, const XrVector3f& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline XrVector3f VCross(const XrVector3f& a, const XrVector3f& b)
{
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline XrPosef PMul(const XrPosef& a, const XrPosef& b)
{
	return { QMul(a.orientation, b.orientation), VAdd(a.position, QRot(a.orientation, b.position)) };
}
inline XrPosef PInv(const XrPosef& a)
{
	XrQuaternionf c = QConj(a.orientation);
	XrVector3f p = QRot(c, a.position);
	return { c, { -p.x, -p.y, -p.z } };
}
const XrPosef kIdentity = { { 0, 0, 0, 1 }, { 0, 0, 0 } };

// Turn about X, Y, Z (degrees), applied in that order.
inline XrQuaternionf FromEulerDeg(float x, float y, float z)
{
	const float k = 3.14159265f / 360.f;
	XrQuaternionf qx = { sinf(x * k), 0, 0, cosf(x * k) }, qy = { 0, sinf(y * k), 0, cosf(y * k) },
				  qz = { 0, 0, sinf(z * k), cosf(z * k) };
	return QMul(qz, QMul(qy, qx));
}

inline void QToMatrix(const XrQuaternionf& q, double m[3][3])
{
	double x = q.x, y = q.y, z = q.z, w = q.w;
	m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
	m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
	m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}

inline XrQuaternionf QFromMatrix(const double m[3][3])
{
	XrQuaternionf q;
	double tr = m[0][0] + m[1][1] + m[2][2];
	if (tr > 0) {
		double s = sqrt(tr + 1.0) * 2;
		q = { (float)((m[2][1] - m[1][2]) / s), (float)((m[0][2] - m[2][0]) / s), (float)((m[1][0] - m[0][1]) / s), (float)(0.25 * s) };
	} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
		double s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
		q = { (float)(0.25 * s), (float)((m[0][1] + m[1][0]) / s), (float)((m[0][2] + m[2][0]) / s), (float)((m[2][1] - m[1][2]) / s) };
	} else if (m[1][1] > m[2][2]) {
		double s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
		q = { (float)((m[0][1] + m[1][0]) / s), (float)(0.25 * s), (float)((m[1][2] + m[2][1]) / s), (float)((m[0][2] - m[2][0]) / s) };
	} else {
		double s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
		q = { (float)((m[0][2] + m[2][0]) / s), (float)((m[1][2] + m[2][1]) / s), (float)(0.25 * s), (float)((m[1][0] - m[0][1]) / s) };
	}
	float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return { q.x / n, q.y / n, q.z / n, q.w / n };
}

// ---- files: everything lives in %APPDATA%\TouchLink -------------------------------------------
inline std::wstring DataDir()
{
	wchar_t buf[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
	std::wstring d = (n && n < MAX_PATH) ? std::wstring(buf) + L"\\TouchLink" : L"TouchLink";
	CreateDirectoryW(d.c_str(), nullptr);
	return d;
}
inline std::wstring DataFile(const wchar_t* name) { return DataDir() + L"\\" + name; }

inline bool ReadFileText(const std::wstring& path, std::string* out)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
	out->clear();
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
	fclose(f);
	return true;
}

// Written to a temporary file and moved over, so readers never see half a file.
inline bool WriteFileText(const std::wstring& path, const std::string& text)
{
	std::wstring tmp = path + L".tmp";
	FILE* f = nullptr;
	if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return false;
	fwrite(text.data(), 1, text.size(), f);
	fclose(f);
	return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

inline bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

inline std::wstring Widen(const std::string& s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}
inline std::string Narrow(const std::wstring& w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, 0);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

// ---- a tiny JSON reader: top-level "key": value of our own flat files --------------------------
inline const char* JsonValue(const std::string& text, const char* key)
{
	std::string k = std::string("\"") + key + "\"";
	size_t at = text.find(k);
	if (at == std::string::npos) return nullptr;
	at = text.find(':', at + k.size());
	if (at == std::string::npos) return nullptr;
	const char* p = text.c_str() + at + 1;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	return p;
}
inline bool JsonString(const std::string& text, const char* key, std::string* out)
{
	const char* p = JsonValue(text, key);
	if (!p || *p != '"') return false;
	out->clear();
	for (p++; *p && *p != '"'; p++) {
		if (*p == '\\' && p[1]) p++;
		out->push_back(*p);
	}
	return true;
}
inline bool JsonNumber(const std::string& text, const char* key, double* out)
{
	const char* p = JsonValue(text, key);
	if (!p) return false;
	char* end = nullptr;
	double v = strtod(p, &end);
	if (end == p) return false;
	*out = v;
	return true;
}
inline bool JsonBool(const std::string& text, const char* key, bool* out)
{
	const char* p = JsonValue(text, key);
	if (!p) return false;
	if (!strncmp(p, "true", 4)) *out = true;
	else if (!strncmp(p, "false", 5)) *out = false;
	else return false;
	return true;
}
inline int JsonNumbers(const std::string& text, const char* key, double* out, int max)
{
	const char* p = JsonValue(text, key);
	if (!p || *p != '[') return 0;
	p++;
	int n = 0;
	while (n < max) {
		while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n' || *p == '\t') p++;
		char* end = nullptr;
		double v = strtod(p, &end);
		if (end == p) break;
		out[n++] = v;
		p = end;
	}
	return n;
}
inline std::string JsonEscape(const std::string& s)
{
	std::string o;
	for (char c : s) {
		if (c == '"' || c == '\\') o.push_back('\\');
		o.push_back(c);
	}
	return o;
}

// ---- config.json (touchlink.exe install writes it) ---------------------------------------------
// SteamVR's oculus_cv1_controller_{left,right} render models put "openxr_grip" here relative to
// the device's raw pose. The driver reports raw = grip * this^-1, which lines up the model,
// /pose/* and OpenXR grip/aim as SteamVR derives them for a real Rift.
inline XrPosef GripLocal(int hand) { return { FromEulerDeg(20.6f, 0, 0), { hand ? -0.007f : 0.007f, -0.00182941f, 0.1019482f } }; }

// Meta's runtime (LibOVR) reports a Touch at its own origin: the grip pose turned -60 degrees
// about X and moved, the same for both hands (EchoXR's kGripToOvrHand, from Meta's OVRPlugin;
// SteamVR's Oculus models agree to the millimetre). The Meta source gives the driver
// grip = LibOVR pose * this^-1.
const XrPosef kGripToOvrHand = { { -0.5f, 0.0f, 0.0f, 0.8660254f }, { 0.0f, -0.019641f, -0.045981f } };

struct Config {
	std::wstring serviceDir;   // the Atrium tracking service (Monado with the Rift drivers)
	bool startService = true;  // start it when nobody else (the Atrium app) has
	XrPosef gripOffset = kIdentity;   // fine-tunes where the Touch sits in the hand
	int handPriority = 100;    // above Index controllers, so the Touches get the hands
	bool meta = false;         // "source": "meta": the Touches from Meta's runtime (touchlink meta), not Atrium's service
};

inline Config LoadConfig()
{
	Config c;
	std::string text, s;
	if (!ReadFileText(DataFile(L"config.json"), &text)) return c;
	if (JsonString(text, "service_dir", &s)) c.serviceDir = Widen(s);
	JsonBool(text, "start_service", &c.startService);
	std::string src;
	if (JsonString(text, "source", &src)) c.meta = src == "meta";
	double v[6];
	if (JsonNumbers(text, "grip_offset", v, 6) == 6) {
		c.gripOffset.position = { (float)v[0], (float)v[1], (float)v[2] };
		c.gripOffset.orientation = FromEulerDeg((float)v[3], (float)v[4], (float)v[5]);
	}
	double p;
	if (JsonNumber(text, "hand_priority", &p)) c.handPriority = (int)p;
	return c;
}

// The service executable and the runtime manifest its clients load, in a staged folder
// (atrium-service.exe + atrium_openxr.json) or a Monado build folder (development).
inline bool ServiceFiles(const std::wstring& dir, std::wstring* exe, std::wstring* manifest)
{
	if (dir.empty()) return false;
	const std::wstring layouts[][2] = {
		{ L"\\atrium-service.exe", L"\\atrium_openxr.json" },
		{ L"\\src\\xrt\\targets\\service\\monado-service.exe", L"\\openxr_atrium-dev.json" },
		{ L"\\monado-service.exe", L"\\openxr_atrium-dev.json" },
	};
	for (const auto& l : layouts)
		if (FileExists(dir + l[0]) && FileExists(dir + l[1])) {
			*exe = dir + l[0];
			*manifest = dir + l[1];
			return true;
		}
	return false;
}

} // namespace tl
