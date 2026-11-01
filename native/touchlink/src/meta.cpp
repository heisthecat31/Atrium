// touchlink meta: the Touches as Meta's runtime tracks them, for TouchLink's driver (the "meta"
// source: config.json "source": "meta"). As mm0zct's Oculus_Touch_Steam_Link (ovr_test, ovr_dummy):
//   - an invisible, focus-aware session with Meta's runtime (LibOVR, the Oculus app installed; the
//     sensors on Meta's driver). It shows the Rift nothing, so it never needs the Rift's display,
//     which SteamVR holds while it runs another headset (an Index): drawing to the Rift as cv1bridge
//     does failed there ("Failed to obtain exclusive access of direct display"), the app never got
//     the focus and the Touches' buttons came as nothing (2026-11-01);
//   - a visible focus-aware session that opens and closes at once (touchlink meta-poke, as
//     ovr_dummy), at the start and every 200 s;
//   - the runtime only tracks the Touches while the Rift is "worn": Meta's Debug Tool turns the
//     sensor's check off and simulates it worn (OVRServer's EnableProximitySensor setting and its
//     prox command);
//   - both Touches (pose, velocities, buttons) go to shared memory about 500 times a second
//     (meta_shared.h); rumble comes back the same way.
// Started by the Atrium app as soon as Meta's runtime runs, or by the driver; one at a time.
#include "common.h"
#include "meta_shared.h"

#include <d3d11.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <thread>

#include "OVR_CAPI.h"
#include "OVR_CAPI_D3D.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")

namespace tl {
namespace {

std::atomic<bool> g_running { true };
std::atomic<bool> g_oculusButton { false };   // the right Touch's Oculus button, held

// LibOVR keeps the Oculus button from apps (ovrButton_Private); OVRServer writes each press and
// release to its log ("Oculus Button: true/false"), so it's read from there, as the Atrium dash does.
void OculusButtonThread()
{
	wchar_t local[MAX_PATH];
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
	const std::wstring dir = std::wstring(local) + L"\\Oculus\\";
	std::wstring file;
	LONGLONG offset = -1;
	ULONGLONG nextScan = 0;
	std::string pending;
	while (g_running) {
		if (GetTickCount64() >= nextScan) {
			// The newest service log (OVRServer starts a new one each time it starts).
			nextScan = GetTickCount64() + 3000;
			WIN32_FIND_DATAW fd;
			HANDLE h = FindFirstFileW((dir + L"Service_*.txt").c_str(), &fd);
			std::wstring newest;
			FILETIME best = {};
			if (h != INVALID_HANDLE_VALUE) {
				do {
					if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; newest = fd.cFileName; }
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			if (!newest.empty() && newest != file) { file = newest; offset = -1; pending.clear(); }
		}
		if (!file.empty()) {
			HANDLE f = CreateFileW((dir + file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, 0, nullptr);
			if (f != INVALID_HANDLE_VALUE) {
				LARGE_INTEGER size;
				GetFileSizeEx(f, &size);
				if (offset < 0 || size.QuadPart < offset) offset = size.QuadPart;   // only presses from now on
				if (size.QuadPart > offset) {
					LARGE_INTEGER at;
					at.QuadPart = offset;
					SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
					std::string buf((size_t)std::min<LONGLONG>(size.QuadPart - offset, 1 << 20), 0);
					DWORD got = 0;
					ReadFile(f, &buf[0], (DWORD)buf.size(), &got, nullptr);
					offset += got;
					pending.append(buf.data(), got);
					size_t nl;
					while ((nl = pending.find('\n')) != std::string::npos) {
						if (pending.find("Oculus Button: true") < nl) g_oculusButton = true;
						else if (pending.find("Oculus Button: false") < nl) g_oculusButton = false;
						pending.erase(0, nl + 1);
					}
				}
				CloseHandle(f);
			}
		}
		Sleep(15);
	}
}
ovrGraphicsLuid g_luid;

// Meta's Debug Tool CLI, with a script of commands. False if it isn't installed.
bool DebugTool(const char* script)
{
	HKEY key;
	wchar_t base[MAX_PATH] = {};
	DWORD size = sizeof base - sizeof(wchar_t);
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Oculus VR, LLC\\Oculus", 0, KEY_READ, &key)) return false;
	LONG r = RegQueryValueExW(key, L"Base", nullptr, nullptr, (BYTE*)base, &size);
	RegCloseKey(key);
	if (r) return false;
	std::wstring cli = std::wstring(base) + L"Support\\oculus-diagnostics\\OculusDebugToolCLI.exe";
	if (!FileExists(cli)) return false;
	std::wstring file = DataFile(L"meta_debugtool.txt");
	if (!WriteFileText(file, script)) return false;
	std::wstring cmd = L"\"" + cli + L"\" -f \"" + file + L"\"";
	STARTUPINFOW si = { sizeof si };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(cli.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
	if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

// The Touch's turn rate comes in its own axes (cv1bridge measured it); the driver wants the room's.
ovrVector3f Rotate(const ovrQuatf& q, const ovrVector3f& v)
{
	float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
	return { v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx) };
}

uint32_t Buttons(const ovrInputState& in, int hand)
{
	uint32_t b = 0;
	unsigned bt = in.Buttons;
	if (hand == 0) {
		if (bt & ovrButton_X) b |= kMetaLower;
		if (bt & ovrButton_Y) b |= kMetaUpper;
		if (bt & ovrButton_LThumb) b |= kMetaStick;
		if (bt & ovrButton_Enter) b |= kMetaMenu;
	} else {
		if (bt & ovrButton_A) b |= kMetaLower;
		if (bt & ovrButton_B) b |= kMetaUpper;
		if (bt & ovrButton_RThumb) b |= kMetaStick;
	}
	return b;
}

uint32_t Touches(const ovrInputState& in, int hand)
{
	uint32_t t = 0;
	unsigned tt = in.Touches;
	if (hand == 0) {
		if (tt & ovrTouch_X) t |= kMetaLower;
		if (tt & ovrTouch_Y) t |= kMetaUpper;
		if (tt & ovrTouch_LThumb) t |= kMetaStick;
		if (tt & ovrTouch_LIndexTrigger) t |= kMetaTrigger;
		if (tt & ovrTouch_LThumbRest) t |= kMetaThumbrest;
	} else {
		if (tt & ovrTouch_A) t |= kMetaLower;
		if (tt & ovrTouch_B) t |= kMetaUpper;
		if (tt & ovrTouch_RThumb) t |= kMetaStick;
		if (tt & ovrTouch_RIndexTrigger) t |= kMetaTrigger;
		if (tt & ovrTouch_RThumbRest) t |= kMetaThumbrest;
	}
	return t;
}

// touchlink meta-poke, run by the helper: a visible, focus-aware session that opens and closes at
// once (mm0zct's ovr_dummy, whose ovr_test reads the Touches invisibly the same way).
void Poke()
{
	wchar_t self[MAX_PATH];
	if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
	std::wstring cmd = L"\"" + std::wstring(self) + L"\" meta-poke";
	STARTUPINFOW si = { sizeof si };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(self, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return;
	if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
}

} // namespace

int MetaPokeMain()
{
	ovrInitParams ip = {};
	ip.Flags = ovrInit_RequestVersion | ovrInit_FocusAware;
	ip.RequestedMinorVersion = OVR_MINOR_VERSION;
	if (OVR_FAILURE(ovr_Initialize(&ip))) return 1;
	ovrSession session = nullptr;
	ovrGraphicsLuid luid;
	if (OVR_SUCCESS(ovr_Create(&session, &luid))) {
		ovr_SetTrackingOriginType(session, ovrTrackingOrigin_FloorLevel);
		ovr_Destroy(session);
	}
	ovr_Shutdown();
	return 0;
}

int MetaMain()
{
	setvbuf(stdout, nullptr, _IONBF, 0);   // its log: driver-started, stdout is a file
	// One helper at a time: a second one couldn't have the Rift anyway (meta_shared.h).
	HANDLE one = CreateMutexW(nullptr, FALSE, kMetaHelperMutex);
	if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
		printf("another touchlink meta is running and has the Rift: using that one\n");
		return 0;
	}
	HANDLE map =CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(MetaShared), kMetaMapping);
	if (!map) {
		fprintf(stderr, "Couldn't make the shared memory (%lu).\n", GetLastError());
		return 1;
	}
	auto* sh = (MetaShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MetaShared));
	if (!sh) return 1;
	auto status = [&](const char* s) {
		snprintf(sh->status, sizeof sh->status, "%s", s);
		printf("%s\n", s);
	};
	sh->magic = kMetaMagic;
	sh->version = kMetaVersion;

	ovrInitParams ip = {};
	ip.Flags = ovrInit_RequestVersion | ovrInit_Invisible | ovrInit_FocusAware;   // see the top
	ip.RequestedMinorVersion = OVR_MINOR_VERSION;
	ovrSession session = nullptr;
	for (;;) {
		// Meta's runtime may still be starting (or off): keep trying. The error is read before
		// ovr_Shutdown, which forgets it.
		ovrErrorInfo e = {};
		const char* step = "starting";
		ovrResult r = ovr_Initialize(&ip);
		if (OVR_SUCCESS(r)) {
			step = "opening the Rift";
			r = ovr_Create(&session, &g_luid);
			if (OVR_SUCCESS(r)) break;
			ovr_GetLastErrorInfo(&e);
			ovr_Shutdown();
		} else {
			ovr_GetLastErrorInfo(&e);
		}
		// The usual reasons, in words: no runtime (Oculus app not installed / OVRServer not running),
		// or no Rift (its display isn't connected: Meta's runtime needs the HDMI cable too).
		const char* hint = r == ovrError_ServiceConnection || r == ovrError_ServiceError || r == ovrError_LibLoad
			? "is the Oculus app installed, with Meta's software running?"
			: r == ovrError_NoHmd || r == ovrError_DisplayInit
			? "Meta's runtime doesn't see the Rift: it needs the Rift's HDMI cable in the graphics card too, not only USB"
			: "";
		char line[300];
		snprintf(line, sizeof line, "Meta's runtime: %s failed (%d: %s) %s", step, (int)r, e.ErrorString[0] ? e.ErrorString : "no detail", hint);
		status(line);
		Sleep(3000);
	}
	auto begin = [&] {
		ovr_SetTrackingOriginType(session, ovrTrackingOrigin_FloorLevel);
		// Tracked only while "worn": turn the proximity check off. EnableProximitySensor is one of
		// OVRServer's session properties (next to UseFovStencil, ...), set through LibOVR itself.
		ovrBool before = ovr_GetBool(session, "EnableProximitySensor", ovrTrue);
		ovrBool set = ovr_SetBool(session, "EnableProximitySensor", ovrFalse);
		ovrBool after = ovr_GetBool(session, "EnableProximitySensor", ovrTrue);
		printf("EnableProximitySensor: %d -> set %d -> %d\n", before, set, after);
	};
	begin();
	Poke();
	ULONGLONG lastPoke = GetTickCount64();
	std::thread oculusButton(OculusButtonThread);
	const char* kTracking = "tracking the Touches with Meta's runtime";
	status(kTracking);

	timeBeginPeriod(1);
	uint32_t hapticSeen[2] = { sh->haptic[0].sequence, sh->haptic[1].sequence };
	double hapticEnd[2] = {};
	float hapticAmp[2] = {};
	bool hapticSaid = false;
	const ovrTouchHapticsDesc hd = ovr_GetTouchHapticsDesc(session, ovrControllerType_RTouch);
	const double hapticRate = hd.SampleRateHz > 0 ? hd.SampleRateHz : 320;
	ULONGLONG lastProx = GetTickCount64();
	// Whether the buttons come through: the first press or touch says so in meta.log.
	int focus = -1;
	bool inputSeen = false;
	char lastSession[200] = "";
	while (g_running) {
		// The driver gone (SteamVR closed): nobody else has the mapping open... the job object ends us
		// with the driver anyway; this is for a helper started by hand.
		double now = ovr_GetTimeInSeconds();
		ovrTrackingState ts = ovr_GetTrackingState(session, now, ovrFalse);
		ovrInputState in = {};
		ovrResult inResult = ovr_GetInputState(session, ovrControllerType_Touch, &in);
		unsigned connected = ovr_GetConnectedControllerTypes(session);
		ovrSessionStatus ss = {};
		ovr_GetSessionStatus(session, &ss);
		LARGE_INTEGER qpc;
		QueryPerformanceCounter(&qpc);

		sh->sequence++;   // odd: writing
		MemoryBarrier();
		sh->qpc = qpc.QuadPart;
		sh->hmdMounted = ss.HmdMounted;
		sh->focus = ss.HasInputFocus;
		for (int h = 0; h < 2; h++) {
			const ovrPoseStatef& p = ts.HandPoses[h];
			MetaHand& o = sh->hands[h];
			o.connected = (connected & (h ? ovrControllerType_RTouch : ovrControllerType_LTouch)) != 0;
			o.positionTracked = (ts.HandStatusFlags[h] & ovrStatus_PositionTracked) != 0;
			o.valid = (ts.HandStatusFlags[h] & ovrStatus_OrientationValid) && (ts.HandStatusFlags[h] & ovrStatus_PositionValid);
			o.orientation[0] = p.ThePose.Orientation.x; o.orientation[1] = p.ThePose.Orientation.y;
			o.orientation[2] = p.ThePose.Orientation.z; o.orientation[3] = p.ThePose.Orientation.w;
			o.position[0] = p.ThePose.Position.x; o.position[1] = p.ThePose.Position.y; o.position[2] = p.ThePose.Position.z;
			o.linearVelocity[0] = p.LinearVelocity.x; o.linearVelocity[1] = p.LinearVelocity.y; o.linearVelocity[2] = p.LinearVelocity.z;
			ovrVector3f w = Rotate(p.ThePose.Orientation, p.AngularVelocity);
			o.angularVelocity[0] = w.x; o.angularVelocity[1] = w.y; o.angularVelocity[2] = w.z;
			o.buttons = Buttons(in, h) | (h == 1 && g_oculusButton ? kMetaOculus : 0);
			o.touches = Touches(in, h);
			o.trigger = in.IndexTrigger[h];
			o.grip = in.HandTrigger[h];
			o.stick[0] = in.Thumbstick[h].x;
			o.stick[1] = in.Thumbstick[h].y;
		}
		MemoryBarrier();
		sh->sequence++;   // even: done

		// Rumble asked for by the driver, as buffered samples (as ovr_test): ovr_SetControllerVibration
		// only reaches the app in focus, and this invisible one never is (no rumble, 2026-11-01). While
		// a rumble lasts, the Touch's queue is kept at 16 samples (50 ms at its 320 Hz).
		for (int h = 0; h < 2; h++) {
			const ovrControllerType type = h ? ovrControllerType_RTouch : ovrControllerType_LTouch;
			if (sh->haptic[h].sequence != hapticSeen[h]) {
				hapticSeen[h] = sh->haptic[h].sequence;
				float amp = std::min(1.0f, std::max(0.0f, sh->haptic[h].amplitude));
				hapticAmp[h] = amp;
				hapticEnd[h] = amp > 0 ? now + std::max(8.0 / hapticRate, (double)sh->haptic[h].seconds) : 0;
			}
			if (hapticEnd[h] <= now) continue;
			ovrHapticsPlaybackState st = {};
			ovr_GetControllerVibrationState(session, type, &st);
			int need = std::min(16 - std::min(16, st.SamplesQueued), (int)lround((hapticEnd[h] - now) * hapticRate));
			if (need <= 0) continue;
			uint8_t samples[16];
			memset(samples, (int)lroundf(255 * sqrtf(hapticAmp[h])), sizeof samples);   // sqrt: weak rumbles still felt
			ovrHapticsBuffer b = {};
			b.Samples = samples;
			b.SamplesCount = need;
			b.SubmitMode = ovrHapticsBufferSubmit_Enqueue;
			ovrResult r = ovr_SubmitControllerVibration(session, type, &b);
			if (!hapticSaid) {
				hapticSaid = true;
				printf("rumble: %s (%d)\n", OVR_SUCCESS(r) ? "sent" : "refused", (int)r);
			}
		}
		// Not worn again (the Rift woke and checked its sensor): say worn again, every few seconds at most.
		if (!ss.HmdMounted && GetTickCount64() - lastProx > 5000) {
			lastProx = GetTickCount64();
			DebugTool("server: prox 1\nexit\n");
			status("Meta's runtime thinks the Rift isn't worn, so it doesn't track the Touches; if this stays, cover the "
				   "Rift's proximity sensor (inside, above the nose)");
		} else if (ss.HmdMounted && sh->status[0] && strstr(sh->status, "isn't worn")) {
			status(kTracking);
		}

		// What Meta's runtime says about this app, each time it changes (meta.log): whether buttons
		// can reach it, and if not, why.
		{
			char line[200];
			snprintf(line, sizeof line, "session: visible %d, worn %d, input focus %d, overlay %d, display lost %d, should quit %d; input %d, connected 0x%x",
				ss.IsVisible, ss.HmdMounted, ss.HasInputFocus, ss.OverlayPresent, ss.DisplayLost, ss.ShouldQuit, (int)inResult, connected);
			if (strcmp(line, lastSession)) {
				strcpy_s(lastSession, line);
				printf("%s\n", line);
			}
		}

		if ((int)ss.HasInputFocus != focus) {
			focus = ss.HasInputFocus;
			printf("input focus: %s\n", focus ? "yes" : "no");
		}
		if (!inputSeen && (in.Buttons || in.Touches || in.IndexTrigger[0] > 0.1f || in.IndexTrigger[1] > 0.1f)) {
			inputSeen = true;
			printf("buttons come through (the first press or touch seen)\n");
		}
		// The ovr_dummy step again, every 200 s.
		if (GetTickCount64() - lastPoke > 200000) {
			lastPoke = GetTickCount64();
			Poke();
		}
		if (ss.ShouldQuit) break;
		Sleep(2);
	}
	g_running = false;
	oculusButton.join();
	timeEndPeriod(1);
	if (session) ovr_Destroy(session);
	ovr_Shutdown();
	return 0;
}

} // namespace tl
