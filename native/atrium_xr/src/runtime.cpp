// The Atrium Runtime: Atrium's OpenXR runtime, the only one games are given.
//
// v1 drives the headset through a backend: the vendor OpenXR runtime that already talks to the
// hardware (Meta, SteamVR, WMR, Monado...). The Atrium app picks it from what's connected and
// writes it to %APPDATA%\Atrium\runtime.json ("backend": its manifest); ATRIUM_XR_BACKEND
// overrides that for one process. On negotiation the backend's library is loaded and negotiated
// with; xrGetInstanceProcAddr then hands out the backend's own functions (no overhead) except
// the few Atrium wraps:
//   xrGetInstanceProperties  the runtime is "Atrium"
//   OVRPlugin games          runtime name and time-0 poses (see "OVRPlugin games" below)
//   frame loop + swapchains  traced to %APPDATA%\Atrium\logs\xr.log when ATRIUM_XR_TRACE=1
// Later versions replace backends with Atrium's own drivers, headset by headset.
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <d3d11.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_reflection.h>

// ---- log ------------------------------------------------------------------------------------
static std::mutex g_logLock;
static wchar_t g_logPath[MAX_PATH];
static bool g_trace;

static void Log(const char* fmt, ...)
{
	std::lock_guard<std::mutex> lk(g_logLock);
	if (!g_logPath[0]) return;
	FILE* f = nullptr;
	if (_wfopen_s(&f, g_logPath, L"a") || !f) return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static bool EverySecond(std::atomic<long long>& last)
{
	long long now = (long long)GetTickCount64();
	long long prev = last.load();
	return now - prev >= 1000 && last.compare_exchange_strong(prev, now);
}

#define XR_ENUM_STR(name, val) case name: return #name;
static const char* ResultName(XrResult r)
{
	switch (r) { XR_LIST_ENUM_XrResult(XR_ENUM_STR) default: return "?"; }
}
static const char* TypeName(XrStructureType t)
{
	switch (t) { XR_LIST_ENUM_XrStructureType(XR_ENUM_STR) default: return "?"; }
}
static const char* SpaceName(XrReferenceSpaceType t)
{
	switch (t) { XR_LIST_ENUM_XrReferenceSpaceType(XR_ENUM_STR) default: return "?"; }
}

// ---- backend --------------------------------------------------------------------------------
static HMODULE g_self;
static HMODULE g_backend;
static PFN_xrGetInstanceProcAddr g_backendGipa;
static std::string g_backendName;

// The string value of "key" in a JSON text (enough for runtime manifests and runtime.json).
static std::string JsonString(const std::string& text, const char* key)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t at = text.find(needle);
	if (at == std::string::npos) return {};
	at = text.find(':', at + needle.size());
	if (at == std::string::npos) return {};
	at = text.find('"', at);
	if (at == std::string::npos) return {};
	std::string out;
	for (size_t i = at + 1; i < text.size() && text[i] != '"'; i++) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			char c = text[++i];
			out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
		} else {
			out += text[i];
		}
	}
	return out;
}

static std::wstring Widen(const std::string& s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n ? n - 1 : 0, L'\0');
	if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
	return w;
}

static std::string ReadFile(const std::wstring& path)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return {};
	std::string s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
	fclose(f);
	return s;
}

static std::wstring AtriumDir()
{
	PWSTR roaming = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return {};
	std::wstring d = std::wstring(roaming) + L"\\Atrium";
	CoTaskMemFree(roaming);
	return d;
}

// The backend's manifest: ATRIUM_XR_BACKEND, else runtime.json from the Atrium app.
static std::wstring BackendManifest()
{
	wchar_t env[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"ATRIUM_XR_BACKEND", env, MAX_PATH);
	if (n && n < MAX_PATH) return env;
	std::string cfg = ReadFile(AtriumDir() + L"\\runtime.json");
	return Widen(JsonString(cfg, "backend"));
}

static XrResult LoadBackend(const XrNegotiateLoaderInfo* loaderInfo)
{
	std::wstring manifest = BackendManifest();
	if (manifest.empty()) {
		Log("no backend: start the Atrium app once with a headset connected, or set ATRIUM_XR_BACKEND");
		return XR_ERROR_RUNTIME_UNAVAILABLE;
	}
	std::string text = ReadFile(manifest);
	std::wstring lib = Widen(JsonString(text, "library_path"));
	g_backendName = JsonString(text, "name");
	if (lib.empty()) {
		Log("backend manifest unreadable: %ls", manifest.c_str());
		return XR_ERROR_RUNTIME_UNAVAILABLE;
	}
	if (PathIsRelativeW(lib.c_str())) {
		wchar_t dir[MAX_PATH];
		wcscpy_s(dir, manifest.c_str());
		PathRemoveFileSpecW(dir);
		wchar_t full[MAX_PATH];
		PathCombineW(full, dir, lib.c_str());
		lib = full;
	}
	g_backend = LoadLibraryExW(lib.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g_backend) {
		Log("backend %ls didn't load (error %lu)", lib.c_str(), GetLastError());
		return XR_ERROR_RUNTIME_UNAVAILABLE;
	}
	if (g_backend == g_self) {
		Log("backend is Atrium itself (%ls); refusing", lib.c_str());
		return XR_ERROR_RUNTIME_UNAVAILABLE;
	}
	auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(g_backend, "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) {
		Log("backend %ls has no xrNegotiateLoaderRuntimeInterface", lib.c_str());
		return XR_ERROR_RUNTIME_UNAVAILABLE;
	}
	XrNegotiateRuntimeRequest req = {};
	req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	req.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
	req.structSize = sizeof(req);
	XrResult r = negotiate(loaderInfo, &req);
	if (XR_FAILED(r) || !req.getInstanceProcAddr) {
		Log("backend negotiation failed: %s", ResultName(r));
		return XR_FAILED(r) ? r : XR_ERROR_RUNTIME_UNAVAILABLE;
	}
	g_backendGipa = req.getInstanceProcAddr;
	Log("backend: %s (%ls)", g_backendName.c_str(), lib.c_str());
	return XR_SUCCESS;
}

// ---- wrapped functions ----------------------------------------------------------------------
// The backend's function for each wrapped name, filled the first time the loader asks for it.
static std::mutex g_fnLock;
static std::unordered_map<std::string, PFN_xrVoidFunction> g_next;

template <typename F> static F Next(const char* name)
{
	std::lock_guard<std::mutex> lk(g_fnLock);
	auto it = g_next.find(name);
	return it == g_next.end() ? nullptr : (F)it->second;
}

// ---- OVRPlugin games ------------------------------------------------------------------------
// Unity/Unreal games built with Meta's OVRPlugin create their instance as "Oculus VR Plugin
// (...)". Two things trip it on runtimes other than Meta's own:
//   - it picks its code paths from the runtime name and needs "Oculus";
//   - it asks for poses at XrTime 0, which only Meta's runtime takes as "now".
// For those instances only, the name reads "Oculus" and time 0 becomes the current time
// (through XR_KHR_win32_convert_performance_counter_time, enabled for them at instance
// creation if the backend has it). Every other app gets the backend's functions untouched.
// The idea comes from RiftLift's OpenXR layer (MIT); this is Atrium's own code.
static const char kOvrPluginName[] = "Oculus VR Plugin";
static std::mutex g_ovrLock;
static std::unordered_map<uint64_t, bool> g_ovrInstances;   // instance -> is OVRPlugin
static std::atomic<uint64_t> g_ovrInstance;                 // the OVRPlugin instance, for time conversion
static PFN_xrConvertWin32PerformanceCounterToTimeKHR g_convertTime;

static bool IsOvrPlugin(XrInstance instance)
{
	std::lock_guard<std::mutex> lk(g_ovrLock);
	auto it = g_ovrInstances.find((uint64_t)instance);
	return it != g_ovrInstances.end() && it->second;
}

// Time 0 from OVRPlugin: the current time on the runtime's clock.
static XrTime OvrTime(XrTime t)
{
	if (t != 0 || !g_convertTime) return t;
	LARGE_INTEGER qpc;
	XrTime now = 0;
	if (QueryPerformanceCounter(&qpc) && XR_SUCCEEDED(g_convertTime((XrInstance)g_ovrInstance.load(), &qpc, &now))) return now;
	return t;
}

static bool BackendHasExtension(const char* ext)
{
	PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
	g_backendGipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumerate);
	uint32_t n = 0;
	if (!enumerate || XR_FAILED(enumerate(nullptr, 0, &n, nullptr))) return false;
	std::vector<XrExtensionProperties> props(n, { XR_TYPE_EXTENSION_PROPERTIES });
	if (XR_FAILED(enumerate(nullptr, n, &n, props.data()))) return false;
	for (const XrExtensionProperties& p : props)
		if (strcmp(p.extensionName, ext) == 0) return true;
	return false;
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrCreateInstance(const XrInstanceCreateInfo* info, XrInstance* instance)
{
	auto next = Next<PFN_xrCreateInstance>("xrCreateInstance");
	bool ovr = info && strncmp(info->applicationInfo.applicationName, kOvrPluginName, sizeof kOvrPluginName - 1) == 0;
	if (!ovr) return next(info, instance);

	// OVRPlugin: make sure the time conversion is there for its time-0 queries.
	const char* convert = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
	std::vector<const char*> exts(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
	bool has = false;
	for (const char* e : exts) has = has || strcmp(e, convert) == 0;
	XrInstanceCreateInfo ci = *info;
	if (!has && BackendHasExtension(convert)) {
		exts.push_back(convert);
		ci.enabledExtensionCount = (uint32_t)exts.size();
		ci.enabledExtensionNames = exts.data();
		has = true;
	}
	XrResult r = next(&ci, instance);
	if (XR_SUCCEEDED(r)) {
		{
			std::lock_guard<std::mutex> lk(g_ovrLock);
			g_ovrInstances[(uint64_t)*instance] = true;
		}
		g_ovrInstance = (uint64_t)*instance;
		if (has) g_backendGipa(*instance, "xrConvertWin32PerformanceCounterToTimeKHR", (PFN_xrVoidFunction*)&g_convertTime);
		Log("OVRPlugin game (%s): compatibility on, time conversion %s", info->applicationInfo.applicationName, g_convertTime ? "ready" : "missing");
	}
	return r;
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrDestroyInstance(XrInstance instance)
{
	XrResult r = Next<PFN_xrDestroyInstance>("xrDestroyInstance")(instance);
	if (XR_SUCCEEDED(r)) {
		std::lock_guard<std::mutex> lk(g_ovrLock);
		g_ovrInstances.erase((uint64_t)instance);
		if (g_ovrInstance == (uint64_t)instance) {
			g_ovrInstance = 0;
			g_convertTime = nullptr;
		}
	}
	return r;
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrLocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location)
{
	return Next<PFN_xrLocateSpace>("xrLocateSpace")(space, base, OvrTime(time), location);
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
	uint32_t capacity, uint32_t* count, XrView* views)
{
	auto next = Next<PFN_xrLocateViews>("xrLocateViews");
	if (!info || info->displayTime != 0) return next(session, info, state, capacity, count, views);
	XrViewLocateInfo li = *info;
	li.displayTime = OvrTime(0);
	return next(session, &li, state, capacity, count, views);
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* props)
{
	XrResult r = Next<PFN_xrGetInstanceProperties>("xrGetInstanceProperties")(instance, props);
	if (XR_SUCCEEDED(r)) {
		char name[XR_MAX_RUNTIME_NAME_SIZE];
		if (IsOvrPlugin(instance))
			snprintf(name, sizeof name, "Oculus");
		else
			snprintf(name, sizeof name, "Atrium (%s)", g_backendName.c_str());
		strcpy_s(props->runtimeName, name);
	}
	return r;
}

static std::unordered_map<uint64_t, std::string> g_spaces;   // trace: handle -> what it is
static std::mutex g_spaceLock;

static std::string SpaceDesc(XrSpace s)
{
	std::lock_guard<std::mutex> lk(g_spaceLock);
	auto it = g_spaces.find((uint64_t)s);
	return it == g_spaces.end() ? "action/unknown space" : it->second;
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session)
{
	XrResult r = Next<PFN_xrCreateSession>("xrCreateSession")(instance, info, session);
	const XrBaseInStructure* b = (const XrBaseInStructure*)info->next;
	Log("xrCreateSession (%s): %s", b ? TypeName(b->type) : "no graphics binding", ResultName(r));
	return r;
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* info, XrSpace* space)
{
	XrResult r = Next<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(session, info, space);
	if (XR_SUCCEEDED(r)) {
		const XrPosef& p = info->poseInReferenceSpace;
		char d[256];
		snprintf(d, sizeof d, "%s at pos %+.3f %+.3f %+.3f rot %+.3f %+.3f %+.3f %+.3f", SpaceName(info->referenceSpaceType),
			p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
		{
			std::lock_guard<std::mutex> lk(g_spaceLock);
			g_spaces[(uint64_t)*space] = d;
		}
		Log("space %llx = %s", (unsigned long long)*space, d);
	}
	return r;
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain)
{
	XrResult r = Next<PFN_xrCreateSwapchain>("xrCreateSwapchain")(session, info, swapchain);
	Log("swapchain %llx: format %lld %ux%u array %u mips %u samples %u faces %u usage 0x%llx flags 0x%llx: %s",
		XR_SUCCEEDED(r) ? (unsigned long long)*swapchain : 0ull, (long long)info->format, info->width, info->height, info->arraySize,
		info->mipCount, info->sampleCount, info->faceCount, (unsigned long long)info->usageFlags, (unsigned long long)info->createFlags, ResultName(r));
	return r;
}

static std::atomic<long long> g_lastWait, g_lastEnd, g_lastAcquire;
static std::atomic<int> g_acquires, g_releases;

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrAcquireSwapchainImage(XrSwapchain sc, const XrSwapchainImageAcquireInfo* info, uint32_t* index)
{
	XrResult r = Next<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage")(sc, info, index);
	g_acquires++;
	if (EverySecond(g_lastAcquire))
		Log("images: %d acquired, %d released in the last second (last: swapchain %llx image %u, %s)",
			g_acquires.exchange(0), g_releases.exchange(0), (unsigned long long)sc, *index, ResultName(r));
	return r;
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrReleaseSwapchainImage(XrSwapchain sc, const XrSwapchainImageReleaseInfo* info)
{
	g_releases++;
	return Next<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage")(sc, info);
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state)
{
	XrResult r = Next<PFN_xrWaitFrame>("xrWaitFrame")(session, info, state);
	if (EverySecond(g_lastWait)) {
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		Log("xrWaitFrame: predicted %lld period %lld shouldRender %d (qpc now %lld): %s", (long long)state->predictedDisplayTime,
			(long long)state->predictedDisplayPeriod, (int)state->shouldRender, (long long)now.QuadPart, ResultName(r));
	}
	return r;
}

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrEndFrame(XrSession session, const XrFrameEndInfo* info)
{
	XrResult r = Next<PFN_xrEndFrame>("xrEndFrame")(session, info);
	if (EverySecond(g_lastEnd)) {
		Log("xrEndFrame: display %lld, blend %d, %u layers: %s", (long long)info->displayTime, (int)info->environmentBlendMode,
			info->layerCount, ResultName(r));
		for (uint32_t l = 0; l < info->layerCount; l++) {
			const XrCompositionLayerBaseHeader* h = info->layers[l];
			if (!h) continue;
			Log("  layer %u: %s flags 0x%llx in %s", l, TypeName(h->type), (unsigned long long)h->layerFlags, SpaceDesc(h->space).c_str());
			if (h->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
			const XrCompositionLayerProjection* p = (const XrCompositionLayerProjection*)h;
			for (uint32_t v = 0; v < p->viewCount; v++) {
				const XrCompositionLayerProjectionView& view = p->views[v];
				const XrPosef& pose = view.pose;
				const XrBaseInStructure* next = (const XrBaseInStructure*)view.next;
				Log("    view %u: pos %+.3f %+.3f %+.3f rot %+.3f %+.3f %+.3f %+.3f fov %.3f %.3f %.3f %.3f; swapchain %llx rect %d,%d %dx%d layer %u%s%s",
					v, pose.position.x, pose.position.y, pose.position.z, pose.orientation.x, pose.orientation.y, pose.orientation.z,
					pose.orientation.w, view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown,
					(unsigned long long)view.subImage.swapchain, view.subImage.imageRect.offset.x, view.subImage.imageRect.offset.y,
					view.subImage.imageRect.extent.width, view.subImage.imageRect.extent.height, view.subImage.imageArrayIndex,
					next ? ", next: " : "", next ? TypeName(next->type) : "");
			}
		}
	}
	return r;
}

// When each wrapper is handed out: always, only with ATRIUM_XR_TRACE=1, or only to OVRPlugin
// instances (everything else keeps the backend's function, no overhead).
enum class When { Always, Trace, OvrPlugin };
struct Wrap { const char* name; PFN_xrVoidFunction fn; When when; };
static const Wrap g_wraps[] = {
	{ "xrCreateInstance", (PFN_xrVoidFunction)Atrium_xrCreateInstance, When::Always },
	{ "xrDestroyInstance", (PFN_xrVoidFunction)Atrium_xrDestroyInstance, When::Always },
	{ "xrGetInstanceProperties", (PFN_xrVoidFunction)Atrium_xrGetInstanceProperties, When::Always },
	{ "xrLocateSpace", (PFN_xrVoidFunction)Atrium_xrLocateSpace, When::OvrPlugin },
	{ "xrLocateViews", (PFN_xrVoidFunction)Atrium_xrLocateViews, When::OvrPlugin },
	{ "xrCreateSession", (PFN_xrVoidFunction)Atrium_xrCreateSession, When::Trace },
	{ "xrCreateReferenceSpace", (PFN_xrVoidFunction)Atrium_xrCreateReferenceSpace, When::Trace },
	{ "xrCreateSwapchain", (PFN_xrVoidFunction)Atrium_xrCreateSwapchain, When::Trace },
	{ "xrAcquireSwapchainImage", (PFN_xrVoidFunction)Atrium_xrAcquireSwapchainImage, When::Trace },
	{ "xrReleaseSwapchainImage", (PFN_xrVoidFunction)Atrium_xrReleaseSwapchainImage, When::Trace },
	{ "xrWaitFrame", (PFN_xrVoidFunction)Atrium_xrWaitFrame, When::Trace },
	{ "xrEndFrame", (PFN_xrVoidFunction)Atrium_xrEndFrame, When::Trace },
};

static XRAPI_ATTR XrResult XRAPI_CALL Atrium_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function)
{
	XrResult r = g_backendGipa(instance, name, function);
	if (XR_FAILED(r) || !*function) return r;
	for (const Wrap& w : g_wraps) {
		if (strcmp(name, w.name) != 0) continue;
		if ((w.when == When::Trace && !g_trace) || (w.when == When::OvrPlugin && !IsOvrPlugin(instance))) break;
		{
			std::lock_guard<std::mutex> lk(g_fnLock);
			g_next[name] = *function;
		}
		*function = w.fn;
		break;
	}
	return r;
}

// ---- entry ----------------------------------------------------------------------------------
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
	XrNegotiateRuntimeRequest* runtimeRequest)
{
	if (!loaderInfo || !runtimeRequest || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
		runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST)
		return XR_ERROR_INITIALIZATION_FAILED;
	if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION || loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
		return XR_ERROR_INITIALIZATION_FAILED;

	if (!g_backendGipa) {
		XrResult r = LoadBackend(loaderInfo);
		if (XR_FAILED(r)) return r;
	}
	runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;
	runtimeRequest->getInstanceProcAddr = Atrium_xrGetInstanceProcAddr;
	return XR_SUCCESS;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = module;
		std::wstring logs = AtriumDir() + L"\\logs";
		SHCreateDirectoryExW(nullptr, logs.c_str(), nullptr);
		swprintf_s(g_logPath, L"%s\\xr.log", logs.c_str());
		FILE* f = nullptr;
		if (_wfopen_s(&f, g_logPath, L"w") == 0 && f) fclose(f);
		wchar_t v[8];
		g_trace = GetEnvironmentVariableW(L"ATRIUM_XR_TRACE", v, 8) && v[0] == L'1';
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		Log("Atrium Runtime loaded into %ls%s", exe, g_trace ? " (tracing)" : "");
	}
	return TRUE;
}
