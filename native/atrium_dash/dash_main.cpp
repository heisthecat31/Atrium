// atrium-dash: Atrium's in-VR dashboard.
//
// A 3D scene (curved glass panel, a toolbar under it, controller models with lasers; an
// environment around it at home) rendered in stereo into a shared texture (atrium_dash_shared.h).
// Inside a game the Atrium Runtime shows that picture over the game's frame and feeds back where
// the eyes and controllers are; with no game running, atrium-dash runs its own OpenXR session
// ("home") so the headset shows the dash the moment it's on.
//
// The Atrium app starts atrium-dash and owns all state: it writes its state snapshot (the same one
// its window draws from) as one JSON line on stdin after every change, and reads commands (the
// same ones its window sends) as JSON lines on stdout. The dash's own look (distance, size,
// curve, colour) is in %APPDATA%\Atrium\dash.json.
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "atrium_dash_shared.h"
#include "dash_json.h"
#include "dash_math.h"

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static const float kPi = 3.14159265f;

// ---- log ------------------------------------------------------------------------------------
static std::wstring g_dataDir;
static bool g_preview; // --preview: own shared names, never touch the real mouse
static std::mutex g_logLock;

static void Log(const char* fmt, ...)
{
	std::lock_guard<std::mutex> lk(g_logLock);
	FILE* f = nullptr;
	if (_wfopen_s(&f, (g_dataDir + L"\\logs\\dash.log").c_str(), L"a") || !f) return;
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

static std::wstring Widen(const std::string& s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n ? n - 1 : 0, L'\0');
	if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
	return w;
}

// ---- the app's state (stdin) and commands (stdout) -----------------------------------------
static std::mutex g_stateLock;
static Json g_state;
static std::atomic<int> g_stateSeq{ 0 };
static std::atomic<bool> g_quit{ false };
static std::atomic<bool> g_restartHome{ false };

static void StdinThread()
{
	std::string line;
	char buf[65536];
	DWORD n;
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	while (ReadFile(in, buf, sizeof buf, &n, nullptr) && n) {
		for (DWORD i = 0; i < n; i++) {
			if (buf[i] != '\n') { line += buf[i]; continue; }
			Json j = Json::parse(line);
			line.clear();
			if (j["type"].str() == "quit") { g_quit = true; return; }
			if (j["type"].str() == "restartHome") { g_restartHome = true; continue; }
			std::lock_guard<std::mutex> lk(g_stateLock);
			g_state = std::move(j);
			g_stateSeq++;
		}
	}
	g_quit = true; // the app went away
}

static void Send(const std::string& json)
{
	static std::mutex lock;
	std::lock_guard<std::mutex> lk(lock);
	std::string line = json + "\n";
	DWORD w;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), (DWORD)line.size(), &w, nullptr);
}

// ---- the dash's own look ---------------------------------------------------------------------
struct Look
{
	float distance = 1.15f; // m, eyes to panel centre
	float width = 1.70f;    // m, along the curve
	float curve = 0.55f;    // 0 flat .. 1 strongly curved
	int accent = 0;
	bool environment = true; // home: draw the environment
	// Controls (also read by the Atrium Runtime inside games):
	std::string openButton = "left_menu"; // which button opens the dash
	bool openHold = true;                  // hold it (a short press still reaches the game) or press it
	bool pauseGame = false;                // the game loses focus (pauses) while the dash is open
	std::map<std::string, std::string> remap; // button -> button, for games (from the next start)
};

// Touch buttons Atrium can use for the dash and remap for games (ids as in dash.json).
struct ButtonDef { const char* id; const wchar_t* name; };
static const ButtonDef kButtons[] = {
	{ "left_menu", L"Menu" }, { "x", L"X" }, { "y", L"Y" }, { "a", L"A" }, { "b", L"B" },
	{ "left_stick", L"L stick" }, { "right_stick", L"R stick" }, { "oculus", L"Oculus" },
};
// The Oculus button opens the dash but isn't remapped for games (they never get it).
static const int kButtonCount = sizeof kButtons / sizeof kButtons[0];
static const int kRemapCount = kButtonCount - 1;
static Look g_look;

struct Accent { const char* name; float r, g, b; };
static const Accent kAccents[] = {
	{ "Aurora", 0.36f, 0.62f, 1.00f }, { "Mint", 0.24f, 0.88f, 0.70f }, { "Ember", 1.00f, 0.48f, 0.26f },
	{ "Orchid", 0.78f, 0.45f, 1.00f }, { "Gold", 1.00f, 0.78f, 0.30f },
};
static const int kAccentCount = sizeof kAccents / sizeof kAccents[0];

static void PublishControls();

static void SaveLook()
{
	PublishControls();
	FILE* f = nullptr;
	if (_wfopen_s(&f, (g_dataDir + L"\\dash.json").c_str(), L"w") || !f) return;
	// "remap" is one string ("x=a,a=x") so the runtime's small JSON reader can take it.
	std::string remap;
	for (auto& r : g_look.remap) remap += (remap.empty() ? "" : ",") + r.first + "=" + r.second;
	fprintf(f, "{ \"distance\": %.3f, \"width\": %.3f, \"curve\": %.3f, \"accent\": %d, \"environment\": %s,\n"
		"  \"openButton\": \"%s\", \"openMode\": \"%s\", \"pauseGame\": \"%s\", \"remap\": \"%s\" }\n", g_look.distance,
		g_look.width, g_look.curve, g_look.accent, g_look.environment ? "true" : "false", g_look.openButton.c_str(),
		g_look.openHold ? "hold" : "press", g_look.pauseGame ? "on" : "off", remap.c_str());
	fclose(f);
}

static void LoadLook()
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, (g_dataDir + L"\\dash.json").c_str(), L"rb") || !f) return;
	std::string s;
	char b[1024];
	size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
	fclose(f);
	Json j = Json::parse(s);
	g_look.distance = (float)j["distance"].num(g_look.distance);
	g_look.width = (float)j["width"].num(g_look.width);
	g_look.curve = (float)j["curve"].num(g_look.curve);
	g_look.accent = (int)j["accent"].num(0) % kAccentCount;
	g_look.environment = j["environment"].type == Json::Bool ? j["environment"].b : true;
	g_look.openButton = j["openButton"].str("left_menu");
	g_look.openHold = j["openMode"].str("hold") != "press";
	g_look.pauseGame = j["pauseGame"].str("off") == "on";
	std::string remap = j["remap"].str("");
	for (size_t i = 0; i < remap.size();) {
		size_t end = remap.find(',', i);
		if (end == std::string::npos) end = remap.size();
		std::string pair = remap.substr(i, end - i);
		size_t eq = pair.find('=');
		if (eq != std::string::npos) g_look.remap[pair.substr(0, eq)] = pair.substr(eq + 1);
		i = end + 1;
	}
}

// ---- graphics -------------------------------------------------------------------------------
static AtriumDashShared* g_shm;
static const char* const kButtonIds[] = { "left_menu", "x", "y", "a", "b", "left_stick", "right_stick", "oculus" };

static void PublishControls()
{
	if (!g_shm) return;
	int b = 0;
	for (int i = 0; i < 8; i++) if (g_look.openButton == kButtonIds[i]) b = i;
	g_shm->openButton = b;
	g_shm->openHold = g_look.openHold ? 1 : 0;
	g_shm->pauseGame = g_look.pauseGame ? 1 : 0;
}

static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<ID2D1Factory1> g_d2d;
static ComPtr<IDWriteFactory> g_dw;
static ComPtr<IWICImagingFactory> g_wic;

static const UINT kUiW = 2048, kUiH = 1280;   // panel 2048x1088 + toolbar 2048x192
static const UINT kPanelPxH = 1088, kBarPxH = 192;
static const UINT kEyeW = 1440, kEyeH = 1600;

static ComPtr<ID3D11Texture2D> g_uiTex;
static ComPtr<ID3D11ShaderResourceView> g_uiSrv;
static ComPtr<ID2D1RenderTarget> g_uiRt;

static ComPtr<ID3D11Texture2D> g_outTex; // 2 eyes side by side (drawn here, then published)
static ComPtr<ID3D11Texture2D> g_slotTex[ATRIUM_DASH_SLOTS]; // shared with games
static ComPtr<ID3D11Query> g_doneQuery;
static ComPtr<ID3D11RenderTargetView> g_outRtv;
static ComPtr<ID3D11DepthStencilView> g_depth;
static HANDLE g_slotShared[ATRIUM_DASH_SLOTS];

static HANDLE g_shmMap;
static bool CreateGraphics()
{
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx))) {
		Log("no D3D11 device");
		return false;
	}
	D2D1_FACTORY_OPTIONS o = {};
	D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &o, (void**)g_d2d.GetAddressOf());
	DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)g_dw.GetAddressOf());
	CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));

	// The UI texture Direct2D draws on and the 3D pass samples.
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = kUiW; td.Height = kUiH; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_uiTex))) return false;
	g_dev->CreateShaderResourceView(g_uiTex.Get(), nullptr, &g_uiSrv);
	ComPtr<IDXGISurface> surf;
	g_uiTex.As(&surf);
	D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,
		D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	if (FAILED(g_d2d->CreateDxgiSurfaceRenderTarget(surf.Get(), &rp, &g_uiRt))) { Log("no D2D target"); return false; }
	g_uiRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

	// The stereo picture (drawn here) and the three shared slots finished pictures go into.
	td.Width = kEyeW * 2; td.Height = kEyeH;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_outTex))) { Log("no picture texture"); return false; }
	g_dev->CreateRenderTargetView(g_outTex.Get(), nullptr, &g_outRtv);
	for (int s = 0; s < ATRIUM_DASH_SLOTS; s++) {
		D3D11_TEXTURE2D_DESC sd = td;
		sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &g_slotTex[s]))) { Log("no shared texture"); return false; }
		ComPtr<IDXGIResource1> res;
		g_slotTex[s].As(&res);
		wchar_t name[64];
		swprintf_s(name, L"%s%d", g_preview ? L"Local\\AtriumDashPreviewTexture" : ATRIUM_DASH_TEXTURE, s);
		if (FAILED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &g_slotShared[s]))) {
			Log("couldn't share the texture (another atrium-dash running?)");
			return false;
		}
	}
	D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
	g_dev->CreateQuery(&qd, &g_doneQuery);
	D3D11_TEXTURE2D_DESC dd = td;
	dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL; dd.MiscFlags = 0;
	ComPtr<ID3D11Texture2D> depthTex;
	g_dev->CreateTexture2D(&dd, nullptr, &depthTex);
	g_dev->CreateDepthStencilView(depthTex.Get(), nullptr, &g_depth);

	g_shmMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(AtriumDashShared), g_preview ? L"Local\\AtriumDashPreviewShared" : ATRIUM_DASH_SHARED);
	if (!g_shmMap) return false;
	g_shm = (AtriumDashShared*)MapViewOfFile(g_shmMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AtriumDashShared));
	if (!g_shm) return false;
	ZeroMemory(g_shm, sizeof *g_shm);
	g_shm->version = ATRIUM_DASH_VERSION;
	g_shm->latestSlot = 0;
	g_shm->eyeWidth = kEyeW;
	g_shm->eyeHeight = kEyeH;
	PublishControls();
	MemoryBarrier();
	g_shm->magic = ATRIUM_DASH_MAGIC;
	return true;
}

// ---- 3D -------------------------------------------------------------------------------------
static const char kShader[] = R"(
cbuffer C : register(b0) {
	float4x4 mvp; float4x4 model; float4 color; float4 accent; float4 uvRect; float4 params; float4 eye;
};
Texture2D ui : register(t0);
SamplerState samp : register(s0);
struct VI { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD; };
struct VO { float4 pos : SV_Position; float3 wpos : WPOS; float3 nrm : NORMAL; float2 uv : TEXCOORD; };
VO vs(VI i) {
	VO o;
	o.pos = mul(mvp, float4(i.pos, 1));
	o.wpos = mul(model, float4(i.pos, 1)).xyz;
	o.nrm = normalize(mul((float3x3)model, i.nrm));
	o.uv = i.uv;
	return o;
}
float4 ps(VO i) : SV_Target {
	int mode = (int)params.x;
	float t = params.y;
	float3 V = normalize(eye.xyz - i.wpos);
	float3 N = normalize(i.nrm);
	if (dot(N, V) < 0) N = -N;
	float fres = pow(1 - saturate(dot(N, V)), 3);
	if (mode == 0) {            // lit solid with an accent rim
		float3 L = normalize(float3(0.3, 0.9, 0.4));
		float diff = 0.35 + 0.65 * saturate(dot(N, L));
		float spec = pow(saturate(dot(reflect(-L, N), V)), 40) * 0.6;
		float3 c = color.rgb * diff + spec + accent.rgb * fres * params.z;
		return float4(c * color.a, color.a);
	}
	if (mode == 1) {            // the UI on glass: the picture, a frosted backing, a glowing edge
		float2 uv = lerp(uvRect.xy, uvRect.zw, i.uv);
		float4 u = ui.Sample(samp, uv);
		float2 e = min(i.uv, 1 - i.uv);
		float edge = saturate(1 - min(e.x * 70, e.y * 40));
		float4 glass = float4(0.035, 0.045, 0.075, 0.80) * (0.9 + 0.1 * fres);
		float4 c = u + glass * (1 - u.a);
		c.rgb += accent.rgb * edge * 0.9 * c.a;
		return c * color.a;
	}
	if (mode == 2) {            // laser: bright core fading to the tip
		float a = (1 - i.uv.y) * 0.85 + 0.15;
		float core = 1 - abs(i.uv.x * 2 - 1);
		return float4(lerp(accent.rgb, 1, core * 0.6) * a * core, 0) * color.a;
	}
	if (mode == 3) {            // home sky: deep gradient with a soft horizon band
		float3 d = normalize(i.wpos - eye.xyz);
		float h = d.y;
		float3 top = float3(0.012, 0.016, 0.040), mid = float3(0.050, 0.060, 0.120), low = float3(0.010, 0.010, 0.020);
		float3 c = h > 0 ? lerp(mid, top, saturate(h * 1.6)) : lerp(mid, low, saturate(-h * 4));
		c += accent.rgb * 0.10 * exp(-abs(h) * 14);
		float stars = step(0.9975, frac(sin(dot(floor(d.xz * 280 + d.y * 97), float2(12.9898, 78.233))) * 43758.5453)) * saturate(h * 3);
		c += stars * 0.6;
		return float4(c, 1);
	}
	if (mode == 4) {            // home floor: a fading grid with a glowing ring under you
		float2 p = i.wpos.xz;
		float2 g = abs(frac(p * 2) - 0.5);
		float line_ = saturate(1 - min(g.x, g.y) * 40);
		float dist = length(p - eye.xz);
		float fade = exp(-dist * 0.18);
		float ring = exp(-pow((dist - 1.1) * 9, 2));
		float pulse = 0.6 + 0.4 * sin(t * 1.5);
		float3 c = float3(0.008, 0.010, 0.020) + accent.rgb * (line_ * 0.25 * fade + ring * 0.6 * pulse);
		return float4(c, 1);
	}
	if (mode == 5) {            // cursor dot
		float r = length(i.uv * 2 - 1);
		float a = saturate(1 - r) * (r < 0.45 ? 1 : 0.55);
		return float4(lerp(accent.rgb, 1, saturate(1 - r * 2)) * a, a) * color.a;
	}
	return color;
}
)";

struct Vtx { float px, py, pz, nx, ny, nz, u, v; };
struct Mesh { ComPtr<ID3D11Buffer> vb, ib; UINT count = 0; };

struct Consts
{
	float mvp[16], model[16];
	float color[4], accent[4], uvRect[4], params[4], eye[4];
};

static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader> g_ps;
static ComPtr<ID3D11InputLayout> g_layout;
static ComPtr<ID3D11Buffer> g_cb;
static ComPtr<ID3D11SamplerState> g_samp;
static ComPtr<ID3D11BlendState> g_blendPremul, g_blendAdd;
static ComPtr<ID3D11DepthStencilState> g_depthOn, g_depthRead;
static ComPtr<ID3D11RasterizerState> g_raster;

static Mesh MakeMesh(const std::vector<Vtx>& v, const std::vector<uint32_t>& idx)
{
	Mesh m;
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = (UINT)(v.size() * sizeof(Vtx)); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	D3D11_SUBRESOURCE_DATA sd = { v.data() };
	g_dev->CreateBuffer(&bd, &sd, &m.vb);
	bd.ByteWidth = (UINT)(idx.size() * 4); bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	sd.pSysMem = idx.data();
	g_dev->CreateBuffer(&bd, &sd, &m.ib);
	m.count = (UINT)idx.size();
	return m;
}

// A curved slab: the front face (uv 0..1) of a vertical cylinder segment of `radius` around the
// origin, centred on -Z, `arcLen` wide and `height` tall, with `depth` of thickness behind it.
static Mesh CurvedPanel(float radius, float arcLen, float height, float depth, bool frontOnly)
{
	std::vector<Vtx> v;
	std::vector<uint32_t> idx;
	const int cols = 48;
	float arc = arcLen / radius;
	auto ring = [&](float r, bool front) {
		uint32_t base = (uint32_t)v.size();
		for (int c = 0; c <= cols; c++) {
			float u = (float)c / cols, a = (u - 0.5f) * arc;
			float x = std::sin(a) * r, z = -std::cos(a) * r;
			V3 n = V3(-std::sin(a), 0, std::cos(a)) * (front ? 1.0f : -1.0f);
			v.push_back({ x, height / 2, z, n.x, n.y, n.z, u, 0 });
			v.push_back({ x, -height / 2, z, n.x, n.y, n.z, u, 1 });
		}
		for (int c = 0; c < cols; c++) {
			uint32_t a = base + c * 2;
			idx.insert(idx.end(), { a, a + 2, a + 1, a + 1, a + 2, a + 3 });
		}
	};
	ring(radius, true);
	if (!frontOnly) {
		ring(radius + depth, false);
		// top, bottom and side walls between the two faces
		uint32_t f = 0, b = (uint32_t)(cols + 1) * 2;
		for (int c = 0; c < cols; c++) {
			for (int edge = 0; edge < 2; edge++) {
				uint32_t a0 = f + c * 2 + edge, a1 = f + (c + 1) * 2 + edge, b0 = b + c * 2 + edge, b1 = b + (c + 1) * 2 + edge;
				idx.insert(idx.end(), { a0, b0, a1, a1, b0, b1 });
			}
		}
		for (uint32_t side : { 0u, (uint32_t)cols }) {
			uint32_t a0 = f + side * 2, a1 = a0 + 1, b0 = b + side * 2, b1 = b0 + 1;
			idx.insert(idx.end(), { a0, a1, b0, b0, a1, b1 });
		}
	}
	return MakeMesh(v, idx);
}

static Mesh Sphere(int seg)
{
	std::vector<Vtx> v;
	std::vector<uint32_t> idx;
	for (int y = 0; y <= seg; y++)
		for (int x = 0; x <= seg * 2; x++) {
			float th = kPi * y / seg, ph = 2 * kPi * x / (seg * 2);
			V3 p(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
			v.push_back({ p.x, p.y, p.z, p.x, p.y, p.z, (float)x / (seg * 2), (float)y / seg });
		}
	int w = seg * 2 + 1;
	for (int y = 0; y < seg; y++)
		for (int x = 0; x < seg * 2; x++) {
			uint32_t a = y * w + x;
			idx.insert(idx.end(), { a, a + 1, a + w, a + 1, a + w + 1, a + w });
		}
	return MakeMesh(v, idx);
}

// Unit cylinder along -Z (0..-1), radius 1: lasers and the controllers' bodies.
static Mesh Cylinder(int seg)
{
	std::vector<Vtx> v;
	std::vector<uint32_t> idx;
	for (int s = 0; s <= seg; s++) {
		float a = 2 * kPi * s / seg, c = std::cos(a), sn = std::sin(a);
		v.push_back({ c, sn, 0, c, sn, 0, (float)s / seg, 0 });
		v.push_back({ c, sn, -1, c, sn, 0, (float)s / seg, 1 });
	}
	for (int s = 0; s < seg; s++) {
		uint32_t a = s * 2;
		idx.insert(idx.end(), { a, a + 2, a + 1, a + 1, a + 2, a + 3 });
	}
	return MakeMesh(v, idx);
}

static Mesh Torus(float R, float r, int seg, int side)
{
	std::vector<Vtx> v;
	std::vector<uint32_t> idx;
	for (int i = 0; i <= seg; i++)
		for (int j = 0; j <= side; j++) {
			float a = 2 * kPi * i / seg, b = 2 * kPi * j / side;
			V3 c(std::cos(a) * R, std::sin(a) * R, 0);
			V3 n(std::cos(a) * std::cos(b), std::sin(a) * std::cos(b), std::sin(b));
			V3 p = c + n * r;
			v.push_back({ p.x, p.y, p.z, n.x, n.y, n.z, 0, 0 });
		}
	for (int i = 0; i < seg; i++)
		for (int j = 0; j < side; j++) {
			uint32_t a = i * (side + 1) + j, b = (i + 1) * (side + 1) + j;
			idx.insert(idx.end(), { a, b, a + 1, a + 1, b, b + 1 });
		}
	return MakeMesh(v, idx);
}

static Mesh Quad()
{
	std::vector<Vtx> v = { { -1, 0, -1, 0, 1, 0, 0, 0 }, { 1, 0, -1, 0, 1, 0, 1, 0 }, { -1, 0, 1, 0, 1, 0, 0, 1 }, { 1, 0, 1, 0, 1, 0, 1, 1 } };
	return MakeMesh(v, { 0, 1, 2, 2, 1, 3 });
}

static Mesh g_panel, g_bar, g_sphere, g_cyl, g_torus, g_quad;
static float g_panelRadius, g_panelArc, g_panelH, g_barRadius, g_barArc, g_barH, g_barY;

static void BuildPanels()
{
	// Curve 0 is (nearly) flat: a very large radius.
	g_panelRadius = g_look.distance / (0.08f + 0.92f * g_look.curve);
	g_panelArc = g_look.width;
	g_panelH = g_look.width * kPanelPxH / kUiW;
	g_panel = CurvedPanel(g_panelRadius, g_panelArc, g_panelH, 0.018f, false);
	g_barRadius = g_panelRadius - 0.10f;
	g_barArc = g_look.width * 0.66f;
	g_barH = g_barArc * kBarPxH / kUiW;
	g_barY = -g_panelH / 2 - g_barH / 2 - 0.05f;
	g_bar = CurvedPanel(g_barRadius, g_barArc, g_barH, 0.03f, false);
}

static bool Create3D()
{
	ComPtr<ID3DBlob> vsb, psb, err;
	if (FAILED(D3DCompile(kShader, sizeof kShader, "dash", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
		FAILED(D3DCompile(kShader, sizeof kShader, "dash", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
		Log("shader: %s", err ? (const char*)err->GetBufferPointer() : "?");
		return false;
	}
	g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
	g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
	D3D11_INPUT_ELEMENT_DESC il[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	g_dev->CreateInputLayout(il, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
	D3D11_BUFFER_DESC cb = { sizeof(Consts), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
	g_dev->CreateBuffer(&cb, nullptr, &g_cb);
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
	g_dev->CreateSamplerState(&sd, &g_samp);
	D3D11_BLEND_DESC bd = {};
	bd.RenderTarget[0].BlendEnable = TRUE;
	bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	g_dev->CreateBlendState(&bd, &g_blendPremul);
	bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
	bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	g_dev->CreateBlendState(&bd, &g_blendAdd);
	D3D11_DEPTH_STENCIL_DESC ds = {};
	ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	g_dev->CreateDepthStencilState(&ds, &g_depthOn);
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	g_dev->CreateDepthStencilState(&ds, &g_depthRead);
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = TRUE;
	g_dev->CreateRasterizerState(&rd, &g_raster);
	BuildPanels();
	g_sphere = Sphere(24);
	g_cyl = Cylinder(24);
	g_torus = Torus(0.042f, 0.0055f, 40, 10);
	g_quad = Quad();
	return true;
}

static M4 g_viewProj;
static V3 g_eyePos;
static float g_time;

static void Draw(const Mesh& m, const M4& model, int mode, const float color[4], float rim = 0.6f, const float uvRect[4] = nullptr)
{
	Consts c = {};
	M4 mvp = g_viewProj * model;
	// HLSL default is column-major matrices in cbuffers: upload as-is.
	memcpy(c.mvp, mvp.m, 64);
	memcpy(c.model, model.m, 64);
	memcpy(c.color, color, 16);
	const Accent& a = kAccents[g_look.accent];
	c.accent[0] = a.r; c.accent[1] = a.g; c.accent[2] = a.b; c.accent[3] = 1;
	float full[4] = { 0, 0, 1, 1 };
	memcpy(c.uvRect, uvRect ? uvRect : full, 16);
	c.params[0] = (float)mode; c.params[1] = g_time; c.params[2] = rim;
	c.eye[0] = g_eyePos.x; c.eye[1] = g_eyePos.y; c.eye[2] = g_eyePos.z;
	g_ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, &c, 0, 0);
	UINT stride = sizeof(Vtx), off = 0;
	g_ctx->IASetVertexBuffers(0, 1, m.vb.GetAddressOf(), &stride, &off);
	g_ctx->IASetIndexBuffer(m.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
	g_ctx->DrawIndexed(m.count, 0, 0);
}

// ---- UI (2D) --------------------------------------------------------------------------------
static Pose g_anchor;       // the panel's pivot (centre of its curve), world-locked
static bool g_anchored = false;

enum class Tab { Library, Playing, Desktop, Devices, Settings, Controls };

// ---- sound: the default speakers and microphone (the headset's while it's in use) -----------
static ComPtr<IAudioEndpointVolume> Endpoint(EDataFlow flow)
{
	ComPtr<IMMDeviceEnumerator> en;
	ComPtr<IMMDevice> dev;
	ComPtr<IAudioEndpointVolume> vol;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
		SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &dev)))
		dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)vol.GetAddressOf());
	return vol;
}

static float Volume()
{
	float v = 0;
	if (auto e = Endpoint(eRender)) e->GetMasterVolumeLevelScalar(&v);
	return v;
}

static void SetVolume(float v)
{
	if (auto e = Endpoint(eRender)) e->SetMasterVolumeLevelScalar(std::max(0.0f, std::min(1.0f, v)), nullptr);
}

static bool MicMuted()
{
	BOOL m = FALSE;
	if (auto e = Endpoint(eCapture)) e->GetMute(&m);
	return m != FALSE;
}

static void SetMicMuted(bool m)
{
	if (auto e = Endpoint(eCapture)) e->SetMute(m, nullptr);
}

// ---- desktop: the main monitor, live (DXGI desktop duplication) -----------------------------
static ComPtr<IDXGIOutputDuplication> g_dup;
static ComPtr<ID3D11Texture2D> g_deskTex;
static ComPtr<ID2D1Bitmap> g_deskBmp;
static RECT g_deskScreen; // the monitor in Windows' desktop coordinates
static D2D1_RECT_F g_deskRect; // where it's drawn on the panel (UI pixels)
static bool g_deskButtonDown;
static int g_deskFrames;

static bool DesktopFrame()
{
	if (!g_dup) {
		ComPtr<IDXGIDevice> dxgiDev;
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<IDXGIOutput> out;
		ComPtr<IDXGIOutput1> out1;
		if (FAILED(g_dev.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) || FAILED(adapter->EnumOutputs(0, &out)) || FAILED(out.As(&out1)))
			return false;
		DXGI_OUTPUT_DESC od;
		out->GetDesc(&od);
		g_deskScreen = od.DesktopCoordinates;
		if (FAILED(out1->DuplicateOutput(g_dev.Get(), &g_dup))) {
			Log("desktop: duplication unavailable");
			return false;
		}
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = g_deskScreen.right - g_deskScreen.left;
		td.Height = g_deskScreen.bottom - g_deskScreen.top;
		td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		g_dev->CreateTexture2D(&td, nullptr, &g_deskTex);
		ComPtr<IDXGISurface> surf;
		g_deskTex.As(&surf);
		D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
		g_uiRt->CreateSharedBitmap(__uuidof(IDXGISurface), surf.Get(), &bp, &g_deskBmp);
	}
	DXGI_OUTDUPL_FRAME_INFO fi;
	ComPtr<IDXGIResource> res;
	// The first frame can take a moment after duplication starts.
	HRESULT hr = g_dup->AcquireNextFrame(g_deskFrames++ == 0 ? 500 : 0, &fi, &res);
	if (hr == DXGI_ERROR_ACCESS_LOST) { g_dup.Reset(); g_deskBmp.Reset(); g_deskTex.Reset(); g_deskFrames = 0; return false; }
	if (SUCCEEDED(hr)) {
		ComPtr<ID3D11Texture2D> frame;
		if (SUCCEEDED(res.As(&frame))) g_ctx->CopyResource(g_deskTex.Get(), frame.Get());
		g_dup->ReleaseFrame();
	}
	return g_deskBmp != nullptr;
}

static void DesktopPointer(float px, float py, bool down)
{
	const D2D1_RECT_F& r = g_deskRect;
	if (g_preview) return;
	if (px < r.left || px > r.right || py < r.top || py > r.bottom) return;
	int x = g_deskScreen.left + (int)((px - r.left) / (r.right - r.left) * (g_deskScreen.right - g_deskScreen.left));
	int y = g_deskScreen.top + (int)((py - r.top) / (r.bottom - r.top) * (g_deskScreen.bottom - g_deskScreen.top));
	SetCursorPos(x, y);
	if (down != g_deskButtonDown) {
		INPUT in = {};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
		SendInput(1, &in, sizeof in);
		g_deskButtonDown = down;
	}
}

static void DesktopScroll(float amount)
{
	if (g_preview) return;
	INPUT in = {};
	in.type = INPUT_MOUSE;
	in.mi.dwFlags = MOUSEEVENTF_WHEEL;
	in.mi.mouseData = (DWORD)(int)(amount * WHEEL_DELTA);
	SendInput(1, &in, sizeof in);
}
static Tab g_tab = Tab::Library;
static int g_scroll = 0; // library rows scrolled
static std::wstring g_toast;
static float g_toastUntil;

struct Hit { D2D1_RECT_F r; std::function<void()> act; bool bar; };
static std::vector<Hit> g_hits;
static int g_hover = -1;

static std::map<std::string, ComPtr<ID2D1Bitmap>> g_covers;

static ComPtr<ID2D1Bitmap> Cover(const std::string& url)
{
	auto it = g_covers.find(url);
	if (it != g_covers.end()) return it->second;
	ComPtr<ID2D1Bitmap> bmp;
	std::string path = url.rfind("file://", 0) == 0 ? url.substr(7) : "";
	if (!path.empty() && g_wic) {
		ComPtr<IWICBitmapDecoder> dec;
		ComPtr<IWICBitmapFrameDecode> frame;
		ComPtr<IWICFormatConverter> conv;
		if (SUCCEEDED(g_wic->CreateDecoderFromFilename(Widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
			SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
			SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
			g_uiRt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
	}
	g_covers[url] = bmp;
	return bmp;
}

static ComPtr<IDWriteTextFormat> Font(float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL)
{
	static std::map<std::pair<int, int>, ComPtr<IDWriteTextFormat>> cache;
	auto key = std::make_pair((int)(size * 10), (int)w);
	auto it = cache.find(key);
	if (it != cache.end()) return it->second;
	ComPtr<IDWriteTextFormat> f;
	g_dw->CreateTextFormat(L"Segoe UI Variable Display", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en", &f);
	if (!f) g_dw->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en", &f);
	f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
	ComPtr<IDWriteInlineObject> ell;
	g_dw->CreateEllipsisTrimmingSign(f.Get(), &ell);
	DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
	f->SetTrimming(&tr, ell.Get());
	cache[key] = f;
	return f;
}

static D2D1_COLOR_F Col(float r, float g, float b, float a = 1) { return D2D1::ColorF(r, g, b, a); }
static D2D1_COLOR_F AccentCol(float a = 1)
{
	const Accent& c = kAccents[g_look.accent];
	return Col(c.r, c.g, c.b, a);
}

static ComPtr<ID2D1SolidColorBrush> Brush(D2D1_COLOR_F c)
{
	ComPtr<ID2D1SolidColorBrush> b;
	g_uiRt->CreateSolidColorBrush(c, &b);
	return b;
}

static void Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL,
	DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING)
{
	auto f = Font(size, w);
	f->SetTextAlignment(al);
	f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	g_uiRt->DrawText(s.c_str(), (UINT32)s.size(), f.Get(), r, Brush(c).Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static bool Hovered(size_t hitIndex) { return g_hover == (int)hitIndex; }

// A rounded button; returns its hit index.
static size_t Button(D2D1_RECT_F r, const std::wstring& label, std::function<void()> act, bool primary = false, bool on = false, bool bar = false)
{
	size_t i = g_hits.size();
	g_hits.push_back({ r, act, bar });
	bool h = Hovered(i);
	D2D1_ROUNDED_RECT rr = { r, (r.bottom - r.top) / 2, (r.bottom - r.top) / 2 };
	D2D1_COLOR_F fill = primary ? AccentCol(h ? 1.0f : 0.85f) : on ? AccentCol(0.30f) : Col(1, 1, 1, h ? 0.16f : 0.07f);
	g_uiRt->FillRoundedRectangle(rr, Brush(fill).Get());
	if (on || h) g_uiRt->DrawRoundedRectangle(rr, Brush(AccentCol(h ? 1.0f : 0.7f)).Get(), 3);
	Text(label, r, (r.bottom - r.top) * 0.42f, primary ? Col(0.02f, 0.03f, 0.06f) : Col(0.93f, 0.95f, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD,
		DWRITE_TEXT_ALIGNMENT_CENTER);
	return i;
}

static std::wstring Clock()
{
	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t b[16];
	swprintf_s(b, L"%02d:%02d", t.wHour, t.wMinute);
	return b;
}

static void Cmd(const std::string& json) { Send(json); }

static void Toast(const std::wstring& s)
{
	g_toast = s;
	g_toastUntil = g_time + 3;
}

static void DrawLibrary(const Json& st, float x0, float y0, float w, float h)
{
	Text(L"Library", { x0, y0, x0 + 600, y0 + 90 }, 64, Col(1, 1, 1), DWRITE_FONT_WEIGHT_BOLD);
	std::vector<const Json*> games;
	for (const Json& g : st["games"].a)
		if (g["installed"].truthy() && g["vr"].truthy()) games.push_back(&g);
	Text(std::to_wstring(games.size()) + L" ready to play", { x0, y0 + 80, x0 + 600, y0 + 130 }, 30, Col(0.7f, 0.75f, 0.85f));
	const int cols = 6;
	float gap = 30, gy = y0 + 150, avail = h - 150;
	float tw = std::min((w - gap * (cols - 1)) / cols, (avail - gap) / 2 - 58), th = tw + 58;
	int rows = std::max(1, (int)((avail + gap) / (th + gap)));
	int totalRows = ((int)games.size() + cols - 1) / cols;
	g_scroll = std::max(0, std::min(g_scroll, std::max(0, totalRows - rows)));
	std::string launching = st["busy"]["launching"].str();
	for (int i = g_scroll * cols; i < (int)games.size() && i < (g_scroll + rows) * cols; i++) {
		const Json& g = *games[i];
		int c = i % cols, r = i / cols - g_scroll;
		D2D1_RECT_F tile = { x0 + c * (tw + gap), gy + r * (th + gap), x0 + c * (tw + gap) + tw, gy + r * (th + gap) + th };
		std::string id = g["id"].str(), name = g["name"].str();
		size_t hi = g_hits.size();
		g_hits.push_back({ tile, [id, name] { Cmd("{\"cmd\":\"launch\",\"id\":" + JsonQuote(id) + "}"); Toast(L"Starting " + Widen(name)); }, false });
		bool hov = Hovered(hi);
		D2D1_RECT_F art = { tile.left, tile.top, tile.right, tile.top + tw };
		if (hov) { art.left -= 8; art.top -= 8; art.right += 8; art.bottom += 8; }
		D2D1_ROUNDED_RECT ar = { art, 22, 22 };
		ComPtr<ID2D1Bitmap> bmp = Cover(g["art"]["cover"].str());
		if (bmp) {
			ComPtr<ID2D1RoundedRectangleGeometry> geo;
			g_d2d->CreateRoundedRectangleGeometry(ar, &geo);
			ComPtr<ID2D1Layer> layer;
			g_uiRt->CreateLayer(&layer);
			g_uiRt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get()), layer.Get());
			D2D1_SIZE_F bs = bmp->GetSize();
			float s = std::max((art.right - art.left) / bs.width, (art.bottom - art.top) / bs.height);
			float bw = bs.width * s, bh = bs.height * s;
			float cx = (art.left + art.right) / 2, cy = (art.top + art.bottom) / 2;
			g_uiRt->DrawBitmap(bmp.Get(), { cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2 });
			g_uiRt->PopLayer();
		} else {
			// No art: a gradient tile in the game's own colour, with its name.
			unsigned hsh = 2166136261u;
			for (char ch : name) hsh = (hsh ^ (unsigned char)ch) * 16777619u;
			float hue = (hsh % 360) / 360.0f;
			D2D1_GRADIENT_STOP stops[2] = { { 0, Col(0.25f + 0.5f * hue, 0.2f, 0.55f - 0.3f * hue) }, { 1, Col(0.05f, 0.06f, 0.12f) } };
			ComPtr<ID2D1GradientStopCollection> sc;
			g_uiRt->CreateGradientStopCollection(stops, 2, &sc);
			ComPtr<ID2D1LinearGradientBrush> gb;
			g_uiRt->CreateLinearGradientBrush({ { art.left, art.top }, { art.right, art.bottom } }, sc.Get(), &gb);
			g_uiRt->FillRoundedRectangle(ar, gb.Get());
			auto f = Font(40, DWRITE_FONT_WEIGHT_BOLD);
			f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
			f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
			std::wstring wn = Widen(name);
			D2D1_RECT_F tr = { art.left + 20, art.top, art.right - 20, art.bottom };
			g_uiRt->DrawText(wn.c_str(), (UINT32)wn.size(), f.Get(), tr, Brush(Col(1, 1, 1, 0.92f)).Get());
			f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		}
		if (hov) g_uiRt->DrawRoundedRectangle(ar, Brush(AccentCol()).Get(), 5);
		std::wstring label = launching == name ? L"Starting…" : Widen(name);
		Text(label, { tile.left + 4, tile.top + tw + 8, tile.right, tile.bottom }, 27, hov ? AccentCol() : Col(0.92f, 0.94f, 1),
			DWRITE_FONT_WEIGHT_SEMI_BOLD);
	}
	if (totalRows > rows) {
		float bar = h - 160, knob = bar * rows / totalRows, top = gy + (bar - knob) * g_scroll / std::max(1, totalRows - rows);
		g_uiRt->FillRoundedRectangle({ { x0 + w + 20, top, x0 + w + 30, top + knob }, 5, 5 }, Brush(Col(1, 1, 1, 0.25f)).Get());
	}
}

static const wchar_t* ButtonName(const std::string& id);

static void DrawPlaying(const Json& st, float x0, float y0, float w, float h)
{
	const Json& run = st["running"];
	// Quick controls: volume, microphone, view.
	auto quick = [&](float qy) {
		g_uiRt->FillRoundedRectangle({ { x0, qy, x0 + w, qy + 200 }, 30, 30 }, Brush(Col(1, 1, 1, 0.05f)).Get());
		float vol = Volume();
		Text(L"Volume", { x0 + 40, qy + 20, x0 + 400, qy + 70 }, 32, Col(1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
		Button({ x0 + 40, qy + 90, x0 + 150, qy + 160 }, L"−", [] { SetVolume(Volume() - 0.05f); });
		D2D1_RECT_F bar = { x0 + 175, qy + 115, x0 + 645, qy + 135 };
		g_uiRt->FillRoundedRectangle({ bar, 10, 10 }, Brush(Col(1, 1, 1, 0.12f)).Get());
		g_uiRt->FillRoundedRectangle({ { bar.left, bar.top, bar.left + (bar.right - bar.left) * vol, bar.bottom }, 10, 10 }, Brush(AccentCol()).Get());
		Button({ x0 + 670, qy + 90, x0 + 780, qy + 160 }, L"+", [] { SetVolume(Volume() + 0.05f); });
		wchar_t pct[16];
		swprintf_s(pct, L"%d%%", (int)(vol * 100 + 0.5f));
		Text(pct, { x0 + 800, qy + 90, x0 + 920, qy + 160 }, 32, Col(0.8f, 0.85f, 0.95f));
		bool muted = MicMuted();
		Text(L"Microphone", { x0 + 980, qy + 20, x0 + 1400, qy + 70 }, 32, Col(1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
		Button({ x0 + 980, qy + 90, x0 + 1300, qy + 160 }, muted ? L"Muted" : L"On", [muted] { SetMicMuted(!muted); }, false, muted);
		Text(L"View", { x0 + 1360, qy + 20, x0 + w, qy + 70 }, 32, Col(1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
		Button({ x0 + 1360, qy + 90, x0 + w - 40, qy + 160 }, L"Reset view", [] { Cmd("{\"cmd\":\"recenter\"}"); g_anchored = false; Toast(L"View reset"); });
	};
	if (run.type != Json::Object) {
		Text(L"Nothing running", { x0, y0, x0 + w, y0 + 90 }, 64, Col(1, 1, 1), DWRITE_FONT_WEIGHT_BOLD);
		Text(L"Pick a game in Library. It starts right here, in the headset.", { x0, y0 + 90, x0 + w, y0 + 140 }, 32, Col(0.7f, 0.75f, 0.85f));
		Button({ x0, y0 + 190, x0 + 420, y0 + 280 }, L"Open Library", [] { g_tab = Tab::Library; }, true);
		quick(y0 + 380);
		return;
	}
	Text(L"NOW PLAYING", { x0, y0, x0 + w, y0 + 50 }, 30, AccentCol(), DWRITE_FONT_WEIGHT_BOLD);
	Text(Widen(run["name"].str()), { x0, y0 + 50, x0 + w, y0 + 170 }, 96, Col(1, 1, 1), DWRITE_FONT_WEIGHT_BOLD);
	Text(Widen(run["start"].str("")), { x0, y0 + 170, x0 + w, y0 + 220 }, 30, Col(0.7f, 0.75f, 0.85f));
	float by = y0 + 280;
	Button({ x0, by, x0 + 460, by + 110 }, L"Resume", [] { g_shm->open = 0; Cmd("{\"cmd\":\"dashClosed\"}"); }, true);
	std::string id = run["id"].str();
	Button({ x0 + 500, by, x0 + 960, by + 110 }, L"Quit game", [id] { Cmd("{\"cmd\":\"quitGame\",\"id\":" + JsonQuote(id) + "}"); Toast(L"Closing the game"); });
	quick(by + 150);
	Text(std::wstring(g_look.openHold ? L"Hold " : L"Press ") + ButtonName(g_look.openButton) + L" to open or close the dash at any time (Controls tab).",
		{ x0, y0 + h - 50, x0 + w, y0 + h }, 26, Col(0.6f, 0.65f, 0.75f));
}

static bool g_deskReady;

static void DrawDesktop(float x0, float y0, float w, float h)
{
	if (!g_deskReady) {
		Text(L"Desktop", { x0, y0, x0 + w, y0 + 90 }, 64, Col(1, 1, 1), DWRITE_FONT_WEIGHT_BOLD);
		Text(L"Your PC's screen isn't available right now.", { x0, y0 + 100, x0 + w, y0 + 150 }, 32, Col(0.7f, 0.75f, 0.85f));
		return;
	}
	D2D1_SIZE_F s = g_deskBmp->GetSize();
	float scale = std::min(w / s.width, (h + 40) / s.height);
	float dw = s.width * scale, dh = s.height * scale;
	g_deskRect = { x0 + (w - dw) / 2, y0 - 20, x0 + (w - dw) / 2 + dw, y0 - 20 + dh };
	g_uiRt->DrawBitmap(g_deskBmp.Get(), g_deskRect);
	g_uiRt->DrawRectangle(g_deskRect, Brush(Col(1, 1, 1, 0.15f)).Get(), 2);
}

static void DrawDevices(const Json& st, float x0, float y0, float w, float)
{
	Text(L"Devices", { x0, y0, x0 + w, y0 + 90 }, 64, Col(1, 1, 1), DWRITE_FONT_WEIGHT_BOLD);
	float y = y0 + 130;
	auto row = [&](const std::wstring& title, const std::wstring& sub, D2D1_COLOR_F dot) {
		g_uiRt->FillRoundedRectangle({ { x0, y, x0 + w, y + 110 }, 24, 24 }, Brush(Col(1, 1, 1, 0.05f)).Get());
		g_uiRt->FillEllipse({ { x0 + 50, y + 55 }, 12, 12 }, Brush(dot).Get());
		Text(title, { x0 + 90, y + 12, x0 + w - 20, y + 62 }, 38, Col(1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
		Text(sub, { x0 + 90, y + 58, x0 + w - 20, y + 100 }, 28, Col(0.65f, 0.7f, 0.8f));
		y += 130;
	};
	for (const Json& hset : st["hw"]["headsets"].a)
		row(Widen(hset["name"].str()), Widen(hset["via"].str() + (hset["active"].truthy() ? "  ·  in use" : "")), hset["active"].truthy() ? Col(0.3f, 0.9f, 0.6f) : Col(0.5f, 0.5f, 0.6f));
	for (const Json& c : st["hw"]["controllers"].a)
		row(Widen(c[(size_t)0].str()), Widen(c[(size_t)1].str()), Col(0.3f, 0.9f, 0.6f));
	const Json& rt = st["rt"];
	if (rt.type == Json::Object)
		row(L"Atrium Runtime", L"Backend: " + Widen(rt["chosen"]["label"].str("none")) + L"  ·  " + Widen(rt["reason"].str()), AccentCol());
	if (y == y0 + 130) row(L"No headset found", L"Plug in or connect a headset", Col(0.9f, 0.5f, 0.3f));
}

static void DrawSettings(const Json& st, float x0, float y0, float w, float)
{
	Text(L"Settings", { x0, y0, x0 + w, y0 + 90 }, 64, Col(1, 1, 1), DWRITE_FONT_WEIGHT_BOLD);
	float colW = (w - 60) / 2;
	float y = y0 + 100;
	auto label = [&](float x, float yy, const std::wstring& t, const std::wstring& sub) {
		Text(t, { x, yy, x + colW, yy + 40 }, 31, Col(1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
		Text(sub, { x, yy + 36, x + colW, yy + 66 }, 22, Col(0.6f, 0.65f, 0.75f));
	};
	auto choice = [&](float x, float yy, std::vector<std::pair<std::wstring, std::function<void()>>> opts, int on) {
		float bw = (colW - 12 * (opts.size() - 1)) / opts.size();
		for (size_t i = 0; i < opts.size(); i++)
			Button({ x + i * (bw + 12), yy, x + i * (bw + 12) + bw, yy + 62 }, opts[i].first, opts[i].second, false, (int)i == on);
	};
	float L = x0, R = x0 + colW + 60;

	// Left column: Atrium's own settings (the same ones as the window).
	std::string eyeSetting = st["mirrorEye"].str("left");
	label(L, y, L"Desktop view shows", L"Which eye Oculus games mirror to the PC screen");
	choice(L, y + 74, { { L"Left eye", [] { Cmd("{\"cmd\":\"setMirrorEye\",\"value\":\"left\"}"); } }, { L"Right eye", [] { Cmd("{\"cmd\":\"setMirrorEye\",\"value\":\"right\"}"); } } },
		eyeSetting == "right" ? 1 : 0);
	y += 150;
	std::string close = st["closeAction"].str("ask");
	label(L, y, L"Closing the Atrium window", L"Keep the headset ready in the tray, or quit");
	choice(L, y + 74, { { L"Ask", [] { Cmd("{\"cmd\":\"setCloseAction\",\"value\":\"ask\"}"); } }, { L"Tray", [] { Cmd("{\"cmd\":\"setCloseAction\",\"value\":\"tray\"}"); } },
		{ L"Quit", [] { Cmd("{\"cmd\":\"setCloseAction\",\"value\":\"quit\"}"); } } },
		close == "tray" ? 1 : close == "quit" ? 2 : 0);
	y += 150;
	bool auto_ = st["autostart"].truthy(), vrOnly = st["vrOnly"].truthy();
	label(L, y, L"Start with Windows", L"Atrium opens when you sign in");
	choice(L, y + 74, { { L"Off", [] { Cmd("{\"cmd\":\"setAutostart\",\"on\":false}"); } }, { L"On", [] { Cmd("{\"cmd\":\"setAutostart\",\"on\":true}"); } } }, auto_ ? 1 : 0);
	y += 150;
	label(L, y, L"Library shows", L"Only VR games, or everything you own");
	choice(L, y + 74, { { L"VR games", [] { Cmd("{\"cmd\":\"setVrOnly\",\"on\":true}"); } }, { L"Everything", [] { Cmd("{\"cmd\":\"setVrOnly\",\"on\":false}"); } } }, vrOnly ? 0 : 1);

	// Right column: the dash itself.
	float ry = y0 + 100;
	auto stepper = [&](float yy, const std::wstring& t, const std::wstring& value, std::function<void()> less, std::function<void()> more) {
		label(R, yy, t, value);
		Button({ R, yy + 74, R + 150, yy + 136 }, L"−", less);
		Button({ R + 170, yy + 74, R + 320, yy + 136 }, L"+", more);
	};
	wchar_t v[64];
	swprintf_s(v, L"%.2f m away", g_look.distance);
	stepper(ry, L"Dash distance", v, [] { g_look.distance = std::max(0.6f, g_look.distance - 0.05f); BuildPanels(); SaveLook(); },
		[] { g_look.distance = std::min(2.5f, g_look.distance + 0.05f); BuildPanels(); SaveLook(); });
	ry += 150;
	swprintf_s(v, L"%.2f m wide", g_look.width);
	stepper(ry, L"Dash size", v, [] { g_look.width = std::max(0.9f, g_look.width - 0.1f); BuildPanels(); SaveLook(); },
		[] { g_look.width = std::min(3.0f, g_look.width + 0.1f); BuildPanels(); SaveLook(); });
	ry += 150;
	swprintf_s(v, L"%d%%", (int)(g_look.curve * 100 + 0.5f));
	stepper(ry, L"Curve", v, [] { g_look.curve = std::max(0.0f, g_look.curve - 0.1f); BuildPanels(); SaveLook(); },
		[] { g_look.curve = std::min(1.0f, g_look.curve + 0.1f); BuildPanels(); SaveLook(); });
	ry += 150;
	label(R, ry, L"Colour", Widen(kAccents[g_look.accent].name));
	for (int i = 0; i < kAccentCount; i++) {
		D2D1_RECT_F r = { R + i * 92.0f, ry + 68, R + i * 92.0f + 76, ry + 144 };
		size_t hi = g_hits.size();
		g_hits.push_back({ r, [i] { g_look.accent = i; SaveLook(); }, false });
		g_uiRt->FillEllipse({ { (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, 30, 30 }, Brush(Col(kAccents[i].r, kAccents[i].g, kAccents[i].b)).Get());
		if (i == g_look.accent || Hovered(hi))
			g_uiRt->DrawEllipse({ { (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, 37, 37 }, Brush(Col(1, 1, 1)).Get(), 4);
	}
	ry += 150;
	label(R, ry, L"Home surroundings", L"What's around you with no game running");
	choice(R, ry + 74, { { L"Space", [] { g_look.environment = true; SaveLook(); } }, { L"Plain", [] { g_look.environment = false; SaveLook(); } } }, g_look.environment ? 0 : 1);
}

static const wchar_t* ButtonName(const std::string& id)
{
	for (const ButtonDef& b : kButtons)
		if (id == b.id) return b.name;
	return L"?";
}

static void DrawControls(float x0, float y0, float w, float h)
{
	Text(L"Controls", { x0, y0, x0 + w, y0 + 90 }, 64, Col(1, 1, 1), DWRITE_FONT_WEIGHT_BOLD);
	float colW = (w - 60) / 2, L = x0, R = x0 + colW + 60;
	auto label = [&](float x, float yy, const std::wstring& t, const std::wstring& sub) {
		Text(t, { x, yy, x + colW, yy + 40 }, 31, Col(1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
		Text(sub, { x, yy + 36, x + colW, yy + 66 }, 22, Col(0.6f, 0.65f, 0.75f));
	};
	// Left: the dash button and how it opens.
	float y = y0 + 100;
	label(L, y, L"Dash button", L"Which button opens and closes the dash in games");
	float bw = (colW - 12 * 3) / 4;
	for (int i = 0; i < kButtonCount; i++) {
		int r = i / 4, c = i % 4;
		std::string id = kButtons[i].id;
		Button({ L + c * (bw + 12), y + 74 + r * 74, L + c * (bw + 12) + bw, y + 136 + r * 74 }, kButtons[i].name,
			[id] { g_look.openButton = id; SaveLook(); }, false, g_look.openButton == id);
	}
	y += 240;
	label(L, y, L"Open the dash by", g_look.openHold ? L"Holding the button (a short press still reaches the game)" : L"Pressing the button (the game never sees it)");
	Button({ L, y + 74, L + colW / 2 - 6, y + 136 }, L"Holding", [] { g_look.openHold = true; SaveLook(); }, false, g_look.openHold);
	Button({ L + colW / 2 + 6, y + 74, L + colW, y + 136 }, L"Pressing", [] { g_look.openHold = false; SaveLook(); }, false, !g_look.openHold);
	y += 160;
	label(L, y, L"Pause the game while the dash is open", L"Some games slow down while paused, which makes the dash stutter");
	Button({ L, y + 74, L + colW / 2 - 6, y + 136 }, L"Keep running", [] { g_look.pauseGame = false; SaveLook(); }, false, !g_look.pauseGame);
	Button({ L + colW / 2 + 6, y + 74, L + colW, y + 136 }, L"Pause", [] { g_look.pauseGame = true; SaveLook(); }, false, g_look.pauseGame);

	// Right: remapping for games. Each button cycles through what the game gets instead.
	float ry = y0 + 100;
	label(R, ry, L"Button remapping", L"What games get when you press each button; from the next game start");
	ry += 74;
	for (int i = 0; i < kRemapCount; i++) {
		std::string from = kButtons[i].id;
		auto it = g_look.remap.find(from);
		std::string to = it == g_look.remap.end() ? from : it->second;
		float rowY = ry + i * 72;
		Text(kButtons[i].name, { R, rowY, R + 220, rowY + 62 }, 30, Col(0.9f, 0.92f, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD);
		Text(L"→", { R + 220, rowY, R + 290, rowY + 62 }, 30, Col(0.6f, 0.65f, 0.75f));
		Button({ R + 300, rowY + 4, R + colW, rowY + 62 }, to == from ? std::wstring(L"Same") : std::wstring(ButtonName(to)), [from, to] {
			// Next target in the list (wrapping to "same").
			int at = 0;
			for (int k = 0; k < kRemapCount; k++) if (to == kButtons[k].id) at = k;
			std::string next = kButtons[(at + 1) % kRemapCount].id;
			if (next == from) g_look.remap.erase(from); else g_look.remap[from] = next;
			SaveLook();
		}, false, to != from);
	}
	Button({ R, ry + kRemapCount * 72 + 6, R + 300, ry + kRemapCount * 72 + 64 }, L"Reset all", [] { g_look.remap.clear(); SaveLook(); });
	(void)h;
}

static void DrawToolbar(const Json& st)
{
	float y0 = (float)kPanelPxH, h = (float)kBarPxH;
	D2D1_ROUNDED_RECT bg = { { 0, y0, (float)kUiW, y0 + h }, h / 2, h / 2 };
	g_uiRt->FillRoundedRectangle(bg, Brush(Col(0.02f, 0.025f, 0.05f, 0.55f)).Get());
	struct T { Tab tab; const wchar_t* name; };
	T tabs[] = { { Tab::Library, L"Library" }, { Tab::Playing, L"Playing" }, { Tab::Desktop, L"Desktop" }, { Tab::Devices, L"Devices" },
		{ Tab::Settings, L"Settings" }, { Tab::Controls, L"Controls" } };
	float x = 44, bw = 222, bh = 112, by = y0 + (h - bh) / 2;
	for (const T& t : tabs) {
		Tab tab = t.tab;
		Button({ x, by, x + bw, by + bh }, t.name, [tab] { g_tab = tab; g_scroll = 0; }, false, g_tab == tab, true);
		x += bw + 18;
	}
	Text(Clock(), { x, by, x + 200, by + bh }, 52, Col(1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
	bool home = g_shm->homeActive != 0;
	if (!home)
		Button({ (float)kUiW - 44 - 210, by, (float)kUiW - 44, by + bh }, L"Close", [] { g_shm->open = 0; Cmd("{\"cmd\":\"dashClosed\"}"); }, true, false, true);
	(void)st;
}

static void DrawUi(const Json& st)
{
	g_hits.clear();
	// The desktop's newest frame is copied before Direct2D starts drawing with it.
	if (g_tab == Tab::Desktop) g_deskReady = DesktopFrame();
	g_uiRt->BeginDraw();
	g_uiRt->Clear(Col(0, 0, 0, 0));
	// The panel's own picture: soft accent light from the top left over the frosted glass.
	D2D1_GRADIENT_STOP glow[2] = { { 0, AccentCol(0.16f) }, { 1, Col(0, 0, 0, 0) } };
	ComPtr<ID2D1GradientStopCollection> gsc;
	g_uiRt->CreateGradientStopCollection(glow, 2, &gsc);
	ComPtr<ID2D1RadialGradientBrush> rg;
	g_uiRt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({ 200, 0 }, { 0, 0 }, 1400, 900), gsc.Get(), &rg);
	g_uiRt->FillRectangle({ 0, 0, (float)kUiW, (float)kPanelPxH }, rg.Get());

	// Header: the Atrium mark, what's running, the headset.
	g_uiRt->FillEllipse({ { 110, 92 }, 30, 30 }, Brush(AccentCol()).Get());
	g_uiRt->FillEllipse({ { 110, 92 }, 16, 16 }, Brush(Col(0.03f, 0.04f, 0.08f)).Get());
	Text(L"ATRIUM", { 160, 52, 600, 132 }, 46, Col(1, 1, 1), DWRITE_FONT_WEIGHT_BLACK);
	std::wstring right;
	for (const Json& hset : st["hw"]["headsets"].a)
		if (hset["active"].truthy()) right = Widen(hset["name"].str());
	if (st["running"].type == Json::Object) right = L"Playing " + Widen(st["running"]["name"].str()) + (right.empty() ? L"" : L"  ·  " + right);
	Text(right, { 700, 52, (float)kUiW - 80, 132 }, 32, Col(0.75f, 0.8f, 0.9f), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
	g_uiRt->FillRectangle({ 80, 160, (float)kUiW - 80, 162 }, Brush(Col(1, 1, 1, 0.08f)).Get());

	float x0 = 80, y0 = 200, w = (float)kUiW - 160, h = (float)kPanelPxH - 240;
	switch (g_tab) {
	case Tab::Library: DrawLibrary(st, x0, y0, w, h); break;
	case Tab::Playing: DrawPlaying(st, x0, y0, w, h); break;
	case Tab::Devices: DrawDevices(st, x0, y0, w, h); break;
	case Tab::Settings: DrawSettings(st, x0, y0, w, h); break;
	case Tab::Desktop: DrawDesktop(x0, y0, w, h); break;
	case Tab::Controls: DrawControls(x0, y0, w, h); break;
	}
	if (g_time < g_toastUntil && !g_toast.empty()) {
		D2D1_RECT_F r = { (float)kUiW / 2 - 450, (float)kPanelPxH - 130, (float)kUiW / 2 + 450, (float)kPanelPxH - 50 };
		g_uiRt->FillRoundedRectangle({ r, 40, 40 }, Brush(Col(0.06f, 0.07f, 0.12f, 0.95f)).Get());
		g_uiRt->DrawRoundedRectangle({ r, 40, 40 }, Brush(AccentCol()).Get(), 3);
		Text(g_toast, r, 34, Col(1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
	}
	DrawToolbar(st);
	g_uiRt->EndDraw();
}

// ---- interaction ----------------------------------------------------------------------------
static int g_activeHand = 1;
static float g_prevTrigger[2];
static float g_scrollAcc;

// Where a ray meets a curved panel (pivot at g_anchor, radius r, arc length, height, centred at y):
// uv on it, the hit point and the distance; false if it misses.
static bool HitCurve(V3 o, V3 d, float radius, float arcLen, float height, float yCentre, float& u, float& v, V3& hit, float& t)
{
	Q inv = g_anchor.q.conj();
	V3 lo = inv.rotate(o - g_anchor.p), ld = inv.rotate(d);
	float a = ld.x * ld.x + ld.z * ld.z, b = 2 * (lo.x * ld.x + lo.z * ld.z), c = lo.x * lo.x + lo.z * lo.z - radius * radius;
	float disc = b * b - 4 * a * c;
	if (a < 1e-6f || disc < 0) return false;
	float s = std::sqrt(disc);
	for (float tt : { (-b - s) / (2 * a), (-b + s) / (2 * a) }) {
		if (tt <= 0) continue;
		V3 p = lo + ld * tt;
		float ang = std::atan2(p.x, -p.z);
		float uu = 0.5f + ang * radius / arcLen, vv = 0.5f - (p.y - yCentre) / height;
		if (p.z < 0 && uu >= 0 && uu <= 1 && vv >= 0 && vv <= 1) {
			u = uu; v = vv; t = tt; hit = o + d * tt;
			return true;
		}
	}
	return false;
}

struct Pointer { bool valid = false; V3 from, to; bool onDash = false; };
static Pointer g_pointer[2];

static void PlaceDash(const AtriumDashInput& in)
{
	Pose head(V3((in.eye[0].px + in.eye[1].px) / 2, (in.eye[0].py + in.eye[1].py) / 2, (in.eye[0].pz + in.eye[1].pz) / 2),
		Q(in.eye[0].qx, in.eye[0].qy, in.eye[0].qz, in.eye[0].qw));
	V3 f = head.forward();
	float yaw = std::atan2(-f.x, -f.z);
	Q q = Q::axisAngle(V3(0, 1, 0), yaw);
	V3 fwd = q.rotate(V3(0, 0, -1));
	// The panel is `radius` ahead of its pivot (along the pivot's -Z), its centre `distance` ahead
	// of the eyes and a little under them.
	V3 centre = head.p + fwd * g_look.distance + V3(0, -0.12f, 0);
	g_anchor = Pose(centre - fwd * g_panelRadius, q);
	g_anchored = true;
}

static void Interact(const AtriumDashInput& in, bool& uiDirty)
{
	int newHover = -1;
	for (int h = 0; h < 2; h++) {
		Pointer& p = g_pointer[h];
		p.valid = in.handValid[h] != 0;
		p.onDash = false;
		if (!p.valid) continue;
		Pose aim(in.aim[h]);
		V3 o = aim.p, d = aim.forward();
		p.from = o;
		p.to = o + d * 3.0f;
		float u, v, t;
		V3 hit;
		bool onPanel = HitCurve(o, d, g_panelRadius, g_panelArc, g_panelH, 0, u, v, hit, t);
		bool onBar = !onPanel && HitCurve(o, d, g_barRadius, g_barArc, g_barH, g_barY, u, v, hit, t);
		if (!onPanel && !onBar) continue;
		p.onDash = true;
		p.to = hit;
		// The trigger picks the hand that points.
		if (in.trigger[h] > 0.5f && g_prevTrigger[h] <= 0.5f) g_activeHand = h;
		if (h != g_activeHand && g_pointer[g_activeHand].valid) continue;
		float px = u * kUiW, py = onPanel ? v * kPanelPxH : kPanelPxH + v * kBarPxH;
		if (g_tab == Tab::Desktop && onPanel) {
			DesktopPointer(px, py, in.trigger[h] > 0.6f);
			if (std::fabs(in.thumbY[h]) > 0.3f) DesktopScroll(in.thumbY[h] * 0.25f);
			continue;
		}
		for (size_t i = 0; i < g_hits.size(); i++) {
			const D2D1_RECT_F& r = g_hits[i].r;
			if (g_hits[i].bar == onBar && px >= r.left && px <= r.right && py >= r.top && py <= r.bottom) newHover = (int)i;
		}
		if (in.trigger[h] > 0.6f && g_prevTrigger[h] <= 0.6f && newHover >= 0) {
			auto act = g_hits[newHover].act;
			if (act) act();
			uiDirty = true;
			g_shm->hapticHand = h;
			g_shm->hapticSeq++;
		}
		// Stick up/down scrolls the library.
		g_scrollAcc += -in.thumbY[h] * 0.08f;
		if (g_scrollAcc > 1) { g_scroll++; g_scrollAcc = 0; uiDirty = true; }
		if (g_scrollAcc < -1) { g_scroll--; g_scrollAcc = 0; uiDirty = true; }
	}
	for (int h = 0; h < 2; h++) g_prevTrigger[h] = in.trigger[h];
	if (newHover != g_hover) {
		if (newHover >= 0) { g_shm->hapticHand = g_activeHand; g_shm->hapticSeq++; }
		g_hover = newHover;
		uiDirty = true;
	}
}

// ---- the 3D frame ---------------------------------------------------------------------------
static void DrawController(const Pose& grip, int hand, bool active)
{
	const float body[4] = { 0.055f, 0.06f, 0.075f, 1 };
	const float face[4] = { 0.085f, 0.09f, 0.11f, 1 };
	const float detail[4] = { 0.16f, 0.17f, 0.20f, 1 };
	const Accent& ac = kAccents[g_look.accent];
	const float glow[4] = { ac.r * 0.9f, ac.g * 0.9f, ac.b * 0.9f, 1 };
	float side = hand == 0 ? 1.0f : -1.0f; // the ring sits on the outside of each hand
	auto at = [&](V3 p, Q q) { return Pose(grip.apply(p), grip.q * q); };
	auto blob = [&](const Pose& p, V3 s, const float* c, float rim) { Draw(g_sphere, M4::fromPose(p, s), 0, c, rim); };

	// Handle: a long rounded body down through the palm, tilted forward like a Touch.
	Q tilt = Q::axisAngle(V3(1, 0, 0), 0.38f);
	blob(at(V3(0, -0.012f, 0.030f), tilt), V3(0.021f, 0.024f, 0.060f), body, 0.5f);
	// Trigger under the index finger, grip button on the side.
	blob(at(V3(0, -0.020f, -0.026f), Q::axisAngle(V3(1, 0, 0), -0.5f)), V3(0.010f, 0.013f, 0.016f), detail, 0.8f);
	blob(at(V3(side * 0.016f, -0.022f, 0.022f), tilt), V3(0.007f, 0.014f, 0.026f), detail, 0.8f);
	// Head: the face plate, the stick and two buttons.
	Pose head = at(V3(-side * 0.002f, 0.020f, -0.012f), Q::axisAngle(V3(1, 0, 0), -0.35f));
	Draw(g_sphere, M4::fromPose(head, V3(0.030f, 0.012f, 0.032f)), 0, face, 0.7f);
	Pose stick = Pose(head.apply(V3(side * 0.006f, 0.012f, -0.006f)), head.q * Q::axisAngle(V3(1, 0, 0), kPi / 2));
	Draw(g_cyl, M4::fromPose(stick, V3(0.0035f, 0.0035f, 0.010f)), 0, detail, 0.6f);
	blob(Pose(stick.apply(V3(0, 0, -0.010f)), stick.q), V3(0.0085f, 0.0085f, 0.003f), detail, 1.0f);
	for (int i = 0; i < 2; i++)
		blob(Pose(head.apply(V3(-side * (0.008f + 0.004f * i), 0.011f, 0.004f + 0.011f * i)), head.q), V3(0.0045f, 0.002f, 0.0045f), detail, 1.0f);
	// Tracking ring: a big loop over the knuckles, with a glowing line inside it.
	Pose ring = at(V3(side * 0.020f, 0.030f, -0.030f), Q::axisAngle(V3(0, 0, 1), side * 0.55f) * Q::axisAngle(V3(1, 0, 0), -1.05f));
	Draw(g_torus, M4::fromPose(ring, V3(1.15f, 1.15f, 2.2f)), 0, body, active ? 1.3f : 0.8f);
	Draw(g_torus, M4::fromPose(ring, V3(1.08f, 1.08f, 0.55f)), 0, glow, active ? 2.5f : 1.2f);
}

static void RenderEyes(const AtriumDashInput& in, bool home)
{
	float clear[4] = { 0, 0, 0, 0 };
	g_ctx->ClearRenderTargetView(g_outRtv.Get(), clear);
	g_ctx->ClearDepthStencilView(g_depth.Get(), D3D11_CLEAR_DEPTH, 1, 0);
	g_ctx->OMSetRenderTargets(1, g_outRtv.GetAddressOf(), g_depth.Get());
	g_ctx->IASetInputLayout(g_layout.Get());
	g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
	g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
	g_ctx->VSSetConstantBuffers(0, 1, g_cb.GetAddressOf());
	g_ctx->PSSetConstantBuffers(0, 1, g_cb.GetAddressOf());
	g_ctx->PSSetShaderResources(0, 1, g_uiSrv.GetAddressOf());
	g_ctx->PSSetSamplers(0, 1, g_samp.GetAddressOf());
	g_ctx->RSSetState(g_raster.Get());

	for (int e = 0; e < 2; e++) {
		D3D11_VIEWPORT vp = { (float)(e * kEyeW), 0, (float)kEyeW, (float)kEyeH, 0, 1 };
		g_ctx->RSSetViewports(1, &vp);
		Pose eye(in.eye[e]);
		g_eyePos = eye.p;
		g_viewProj = M4::projection(in.fov[e], 0.03f, 200.0f) * M4::view(eye);
		float one[4] = { 1, 1, 1, 1 };
		g_ctx->OMSetBlendState(g_blendPremul.Get(), nullptr, 0xffffffff);
		g_ctx->OMSetDepthStencilState(g_depthOn.Get(), 0);

		if (home && g_look.environment) {
			g_ctx->OMSetDepthStencilState(g_depthRead.Get(), 0);
			Draw(g_sphere, M4::fromPose(Pose(eye.p, Q()), V3(150, 150, 150)), 3, one);
			// The floor sits 1.6 m under the panel's pivot (head height when it was placed).
			Draw(g_quad, M4::fromPose(Pose(V3(eye.p.x, g_anchor.p.y - 1.45f, eye.p.z), Q()), V3(60, 1, 60)), 4, one);
			g_ctx->OMSetDepthStencilState(g_depthOn.Get(), 0);
		} else if (home) {
			float bg[4] = { 0.015f, 0.018f, 0.03f, 1 };
			g_ctx->OMSetDepthStencilState(g_depthRead.Get(), 0);
			Draw(g_sphere, M4::fromPose(Pose(eye.p, Q()), V3(150, 150, 150)), 6, bg);
			g_ctx->OMSetDepthStencilState(g_depthOn.Get(), 0);
		}

		// The panel slab (dark glass body), then its face with the UI; the same for the toolbar.
		M4 pivot = M4::fromPose(g_anchor);
		const float slab[4] = { 0.025f, 0.03f, 0.05f, 0.92f };
		Draw(g_panel, pivot, 0, slab, 1.2f);
		float panelUv[4] = { 0, 0, 1, (float)kPanelPxH / kUiH };
		// The faces carrying the UI sit a hair in front of the slabs.
		static Mesh front, barFront;
		static float builtFor = -1;
		float key = g_panelRadius * 1000 + g_panelArc * 10 + g_look.width;
		if (builtFor != key) {
			front = CurvedPanel(g_panelRadius - 0.0015f, g_panelArc, g_panelH, 0, true);
			barFront = CurvedPanel(g_barRadius - 0.0015f, g_barArc, g_barH, 0, true);
			builtFor = key;
		}
		Draw(front, pivot, 1, one, 1, panelUv);
		M4 barM = M4::fromPose(Pose(g_anchor.apply(V3(0, g_barY, 0)), g_anchor.q));
		Draw(g_bar, barM, 0, slab, 1.2f);
		float barUv[4] = { 0, (float)kPanelPxH / kUiH, 1, 1 };
		Draw(barFront, barM, 1, one, 1, barUv);

		// Controllers and their lasers.
		for (int h = 0; h < 2; h++) {
			if (!in.handValid[h]) continue;
			DrawController(Pose(in.grip[h]), h, h == g_activeHand);
		}
		g_ctx->OMSetBlendState(g_blendAdd.Get(), nullptr, 0xffffffff);
		g_ctx->OMSetDepthStencilState(g_depthRead.Get(), 0);
		for (int h = 0; h < 2; h++) {
			const Pointer& p = g_pointer[h];
			if (!p.valid) continue;
			V3 d = p.to - p.from;
			float len = d.len();
			if (len < 1e-3f) continue;
			// A thin cylinder from the controller to the hit point (or 3 m out).
			V3 z = (d * (-1.0f / len));
			V3 upv = std::fabs(z.y) > 0.9f ? V3(1, 0, 0) : V3(0, 1, 0);
			V3 x = upv.cross(z).norm(), y = z.cross(x);
			M4 m;
			float r = h == g_activeHand ? 0.0022f : 0.0012f;
			m.at(0, 0) = x.x * r; m.at(1, 0) = x.y * r; m.at(2, 0) = x.z * r;
			m.at(0, 1) = y.x * r; m.at(1, 1) = y.y * r; m.at(2, 1) = y.z * r;
			m.at(0, 2) = z.x * len; m.at(1, 2) = z.y * len; m.at(2, 2) = z.z * len;
			m.at(0, 3) = p.from.x; m.at(1, 3) = p.from.y; m.at(2, 3) = p.from.z;
			float c[4] = { 1, 1, 1, p.onDash ? 1.0f : 0.35f };
			Draw(g_cyl, m, 2, c);
			if (p.onDash) {
				V3 n = (eye.p - p.to).norm();
				V3 ux = V3(0, 1, 0).cross(n).norm(), uz = ux.cross(n);
				M4 dot;
				float s = h == g_activeHand ? 0.018f : 0.011f;
				dot.at(0, 0) = ux.x * s; dot.at(1, 0) = ux.y * s; dot.at(2, 0) = ux.z * s;
				dot.at(0, 1) = n.x; dot.at(1, 1) = n.y; dot.at(2, 1) = n.z;
				dot.at(0, 2) = uz.x * s; dot.at(1, 2) = uz.y * s; dot.at(2, 2) = uz.z * s;
				V3 at = p.to + n * 0.004f;
				dot.at(0, 3) = at.x; dot.at(1, 3) = at.y; dot.at(2, 3) = at.z;
				float cc[4] = { 1, 1, 1, 1 };
				g_ctx->OMSetBlendState(g_blendPremul.Get(), nullptr, 0xffffffff);
				Draw(g_quad, dot, 5, cc);
				g_ctx->OMSetBlendState(g_blendAdd.Get(), nullptr, 0xffffffff);
			}
		}
	}
	g_ctx->Flush();
}

// ---- one dash frame from an input sample ---------------------------------------------------
static int g_lastStateSeq = -1;
static Json g_frameState;

static void DashFrame(const AtriumDashInput& in, bool home)
{
	static LARGE_INTEGER freq, start;
	if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	g_time = (float)(now.QuadPart - start.QuadPart) / freq.QuadPart;

	if (!g_anchored) PlaceDash(in);
	bool uiDirty = false;
	int seq = g_stateSeq.load();
	if (seq != g_lastStateSeq) {
		std::lock_guard<std::mutex> lk(g_stateLock);
		g_frameState = g_state;
		g_lastStateSeq = seq;
		uiDirty = true;
	}
	Interact(in, uiDirty);
	static float lastUi;
	if (g_time - lastUi > (g_tab == Tab::Desktop ? 0.033f : 1.0f)) uiDirty = true; // the clock; the desktop is live
	if (uiDirty) {
		DrawUi(g_frameState);
		lastUi = g_time;
	}
	RenderEyes(in, home);
	if (!home) {
		// Publish: into the slot after the latest, finished on the GPU before games may take it.
		int slot = (g_shm->latestSlot + 1) % ATRIUM_DASH_SLOTS;
		g_ctx->CopyResource(g_slotTex[slot].Get(), g_outTex.Get());
		g_ctx->End(g_doneQuery.Get());
		g_ctx->Flush();
		BOOL done = FALSE;
		while (g_ctx->GetData(g_doneQuery.Get(), &done, sizeof done, 0) == S_FALSE) YieldProcessor();
		for (int e = 0; e < 2; e++) {
			g_shm->slotEye[slot][e] = in.eye[e];
			g_shm->slotFov[slot][e] = in.fov[e];
		}
		MemoryBarrier();
		g_shm->latestSlot = slot;
	}
	g_shm->frameSeq++;
}

// ---- the Oculus button ----------------------------------------------------------------------
// Meta's runtime keeps the Oculus button from apps, but with its own Dash off it still logs each
// press ("[LifeCycle] Oculus Button: true") to its service log as it happens. When that's the
// dash button (Controls tab), the dash watches the newest log and opens over the running game.
static void OculusPressed()
{
	if (g_look.openButton != "oculus" || g_shm->homeActive) return;
	uint32_t pid = g_shm->activeGamePid;
	if (!pid) return;
	if (g_shm->open && g_shm->gamePid == pid) {
		g_shm->open = 0;
		Send("{\"cmd\":\"dashClosed\"}");
	} else {
		g_shm->gamePid = pid;
		g_shm->open = 1;
	}
	g_shm->hapticHand = 1;
	g_shm->hapticSeq++;
	Log("oculus button: dash %s", g_shm->open ? "open" : "closed");
}

static void OculusButtonThread()
{
	wchar_t local[MAX_PATH];
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
	std::wstring dir = std::wstring(local) + L"\\Oculus\\";
	std::wstring file;
	LONGLONG offset = -1;
	ULONGLONG nextScan = 0;
	std::string pending;
	bool oculusDown = false, oculusUsed = false;
	ULONGLONG oculusSince = 0;
	while (!g_quit) {
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
			HANDLE f = CreateFileW((dir + file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (f != INVALID_HANDLE_VALUE) {
				LARGE_INTEGER size;
				GetFileSizeEx(f, &size);
				if (offset < 0 || size.QuadPart < offset) offset = size.QuadPart; // only presses from now on
				if (size.QuadPart > offset) {
					LARGE_INTEGER at;
					at.QuadPart = offset;
					SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
					std::string buf((size_t)std::min<LONGLONG>(size.QuadPart - offset, 1 << 20), 0);
					DWORD got = 0;
					ReadFile(f, &buf[0], (DWORD)buf.size(), &got, nullptr);
					offset += got;
					pending.append(buf.data(), got);
					size_t nlPos;
					while ((nlPos = pending.find('\n')) != std::string::npos) {
						// Pressing: toggle on press. Holding: toggle once it's been held long enough
						// (timed below, between the press and release lines).
						if (pending.find("Oculus Button: true") < nlPos) {
							if (!g_look.openHold) OculusPressed();
							oculusDown = true;
							oculusUsed = false;
							oculusSince = GetTickCount64();
						} else if (pending.find("Oculus Button: false") < nlPos) {
							oculusDown = false;
						}
						pending.erase(0, nlPos + 1);
					}
				}
				CloseHandle(f);
			}
		}
		if (g_look.openHold && oculusDown && !oculusUsed && GetTickCount64() - oculusSince >= 450) {
			oculusUsed = true;
			OculusPressed();
		}
		Sleep(25);
	}
}

// ---- home: atrium-dash's own OpenXR session -------------------------------------------------
#define XRC(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { Log("%s failed: %d", #x, (int)r_); return false; } } while (0)

static std::atomic<bool> g_homeWanted{ false };
static XrSpaceLocationFlags g_aimFlags[2];

static bool RunHome()
{
	const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy_s(ici.applicationInfo.applicationName, "Atrium Home");
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = 1;
	ici.enabledExtensionNames = exts;
	XrInstance inst = XR_NULL_HANDLE;
	XRC(xrCreateInstance(&ici, &inst));
	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys;
	if (XR_FAILED(xrGetSystem(inst, &sgi, &sys))) { xrDestroyInstance(inst); Log("home: no headset"); return false; }
	PFN_xrGetD3D11GraphicsRequirementsKHR req = nullptr;
	xrGetInstanceProcAddr(inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&req);
	XrGraphicsRequirementsD3D11KHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	if (req) req(inst, sys, &gr);
	XrGraphicsBindingD3D11KHR gb = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	gb.device = g_dev.Get();
	XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
	sci.next = &gb;
	sci.systemId = sys;
	XrSession session;
	if (XR_FAILED(xrCreateSession(inst, &sci, &session))) { Log("home: session failed"); xrDestroyInstance(inst); return false; }

	XrReferenceSpaceCreateInfo rsi = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsi.poseInReferenceSpace.orientation.w = 1;
	rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XrSpace space;
	xrCreateReferenceSpace(session, &rsi, &space);

	// Actions: aim + grip poses, trigger, squeeze, stick, haptics.
	XrActionSetCreateInfo asi = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(asi.actionSetName, "atrium_home");
	strcpy_s(asi.localizedActionSetName, "Atrium Home");
	XrActionSet set;
	xrCreateActionSet(inst, &asi, &set);
	XrPath hands[2];
	xrStringToPath(inst, "/user/hand/left", &hands[0]);
	xrStringToPath(inst, "/user/hand/right", &hands[1]);
	auto action = [&](const char* name, XrActionType type) {
		XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
		strcpy_s(ai.actionName, name);
		strcpy_s(ai.localizedActionName, name);
		ai.actionType = type;
		ai.countSubactionPaths = 2;
		ai.subactionPaths = hands;
		XrAction a;
		xrCreateAction(set, &ai, &a);
		return a;
	};
	XrAction aAim = action("aim", XR_ACTION_TYPE_POSE_INPUT), aGrip = action("grip", XR_ACTION_TYPE_POSE_INPUT);
	XrAction aTrig = action("trigger", XR_ACTION_TYPE_FLOAT_INPUT), aSq = action("squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
	XrAction aStick = action("stick", XR_ACTION_TYPE_VECTOR2F_INPUT), aBuzz = action("buzz", XR_ACTION_TYPE_VIBRATION_OUTPUT);
	auto path = [&](const char* s) { XrPath p; xrStringToPath(inst, s, &p); return p; };
	auto suggest = [&](const char* profile, std::vector<std::pair<XrAction, const char*>> b) {
		std::vector<XrActionSuggestedBinding> sb;
		for (auto& x : b) {
			for (const char* side : { "left", "right" }) {
				std::string p = std::string("/user/hand/") + side + x.second;
				sb.push_back({ x.first, path(p.c_str()) });
			}
		}
		XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		s.interactionProfile = path(profile);
		s.countSuggestedBindings = (uint32_t)sb.size();
		s.suggestedBindings = sb.data();
		XrResult r = xrSuggestInteractionProfileBindings(inst, &s);
		if (XR_FAILED(r)) Log("home: bindings for %s: %d", profile, (int)r);
	};
	suggest("/interaction_profiles/oculus/touch_controller", { { aAim, "/input/aim/pose" }, { aGrip, "/input/grip/pose" }, { aTrig, "/input/trigger/value" },
		{ aSq, "/input/squeeze/value" }, { aStick, "/input/thumbstick" }, { aBuzz, "/output/haptic" } });
	suggest("/interaction_profiles/valve/index_controller", { { aAim, "/input/aim/pose" }, { aGrip, "/input/grip/pose" }, { aTrig, "/input/trigger/value" },
		{ aSq, "/input/squeeze/value" }, { aStick, "/input/thumbstick" }, { aBuzz, "/output/haptic" } });
	suggest("/interaction_profiles/khr/simple_controller", { { aAim, "/input/aim/pose" }, { aGrip, "/input/grip/pose" }, { aTrig, "/input/select/click" },
		{ aBuzz, "/output/haptic" } });
	XrSessionActionSetsAttachInfo att = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	att.countActionSets = 1;
	att.actionSets = &set;
	xrAttachSessionActionSets(session, &att);
	XrSpace aimSpace[2], gripSpace[2];
	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo si = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		si.poseInActionSpace.orientation.w = 1;
		si.subactionPath = hands[h];
		si.action = aAim;
		xrCreateActionSpace(session, &si, &aimSpace[h]);
		si.action = aGrip;
		xrCreateActionSpace(session, &si, &gripSpace[h]);
	}

	// One wide swapchain, both eyes side by side, the same size as the shared picture.
	uint32_t nf = 0;
	xrEnumerateSwapchainFormats(session, 0, &nf, nullptr);
	std::vector<int64_t> fmts(nf);
	xrEnumerateSwapchainFormats(session, nf, &nf, fmts.data());
	int64_t fmt = 0;
	for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) fmt = f;
	if (!fmt) for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM) fmt = f;
	XrSwapchainCreateInfo sci2 = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sci2.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	sci2.format = fmt;
	sci2.width = kEyeW * 2;
	sci2.height = kEyeH;
	sci2.sampleCount = 1; sci2.faceCount = 1; sci2.arraySize = 1; sci2.mipCount = 1;
	XrSwapchain swap;
	XRC(xrCreateSwapchain(session, &sci2, &swap));
	uint32_t ni = 0;
	xrEnumerateSwapchainImages(swap, 0, &ni, nullptr);
	std::vector<XrSwapchainImageD3D11KHR> imgs(ni, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	xrEnumerateSwapchainImages(swap, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data());
	Log("home: session started (%u images, format %lld)", ni, (long long)fmt);

	bool running = false, done = false;
	g_shm->homeActive = 1;
	g_shm->open = 1;
	g_anchored = false;
	while (!done && !g_quit) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				auto* sc = (XrEventDataSessionStateChanged*)&ev;
				Log("home: session state %d", (int)sc->state);
				if (sc->state == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					xrBeginSession(session, &bi);
					running = true;
				} else if (sc->state == XR_SESSION_STATE_STOPPING) {
					xrEndSession(session);
					running = false;
				} else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
					done = true;
				}
			} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
				done = true;
			}
			ev = { XR_TYPE_EVENT_DATA_BUFFER };
		}
		// The app's latest word: a game started (no home), or start home again.
		{
			std::lock_guard<std::mutex> lk(g_stateLock);
			g_homeWanted = g_state["home"].truthy();
		}
		if ((!g_homeWanted || g_restartHome.exchange(false)) && running) { xrRequestExitSession(session); }
		if (!running) { Sleep(20); if (!g_homeWanted) break; continue; }

		XrFrameState fs = { XR_TYPE_FRAME_STATE };
		xrWaitFrame(session, nullptr, &fs);
		// Until the runtime shows us it doesn't pace frames: don't spin, keep to ~90 a second.
		static LARGE_INTEGER pf, lastWait;
		if (!pf.QuadPart) QueryPerformanceFrequency(&pf);
		LARGE_INTEGER nowWait;
		QueryPerformanceCounter(&nowWait);
		double ms = lastWait.QuadPart ? (nowWait.QuadPart - lastWait.QuadPart) * 1000.0 / pf.QuadPart : 11;
		if (ms < 10.5) Sleep((DWORD)(11 - ms));
		QueryPerformanceCounter(&lastWait);
		xrBeginFrame(session, nullptr);
		XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = fs.predictedDisplayTime;
		vli.space = space;
		XrViewState vs = { XR_TYPE_VIEW_STATE };
		XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
		uint32_t nv = 0;
		xrLocateViews(session, &vli, &vs, 2, &nv, views);

		XrActiveActionSet aas = { set, XR_NULL_PATH };
		XrActionsSyncInfo syn = { XR_TYPE_ACTIONS_SYNC_INFO };
		syn.countActiveActionSets = 1;
		syn.activeActionSets = &aas;
		xrSyncActions(session, &syn);

		AtriumDashInput in = {};
		for (int e = 0; e < 2; e++) {
			in.eye[e] = { views[e].pose.position.x, views[e].pose.position.y, views[e].pose.position.z, views[e].pose.orientation.x,
				views[e].pose.orientation.y, views[e].pose.orientation.z, views[e].pose.orientation.w };
			in.fov[e] = { views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown };
		}
		in.valid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
		for (int h = 0; h < 2; h++) {
			XrSpaceLocation la = { XR_TYPE_SPACE_LOCATION }, lg = { XR_TYPE_SPACE_LOCATION };
			xrLocateSpace(aimSpace[h], space, fs.predictedDisplayTime, &la);
			xrLocateSpace(gripSpace[h], space, fs.predictedDisplayTime, &lg);
			g_aimFlags[h] = la.locationFlags;
			bool ok = (la.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (la.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
			in.handValid[h] = ok;
			auto raw = [](const XrPosef& p) { return AtriumDashPose{ p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w }; };
			in.aim[h] = raw(la.pose);
			in.grip[h] = raw(lg.pose);
			XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
			gi.subactionPath = hands[h];
			XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT };
			gi.action = aTrig; xrGetActionStateFloat(session, &gi, &f); in.trigger[h] = f.currentState;
			gi.action = aSq; xrGetActionStateFloat(session, &gi, &f); in.squeeze[h] = f.currentState;
			XrActionStateVector2f v2 = { XR_TYPE_ACTION_STATE_VECTOR2F };
			gi.action = aStick; xrGetActionStateVector2f(session, &gi, &v2); in.thumbY[h] = v2.currentState.y;
		}
		static int lastHaptic;
		if (g_shm->hapticSeq != lastHaptic) {
			lastHaptic = g_shm->hapticSeq;
			XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
			hi.action = aBuzz;
			hi.subactionPath = hands[g_shm->hapticHand & 1];
			XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
			vib.duration = 12000000; vib.frequency = XR_FREQUENCY_UNSPECIFIED; vib.amplitude = 0.25f;
			xrApplyHapticFeedback(session, &hi, (XrHapticBaseHeader*)&vib);
		}

		XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
		XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&proj };
		XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
		fei.displayTime = fs.predictedDisplayTime;
		fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		static ULONGLONG lastLog;
		static int frames;
		frames++;
		if (GetTickCount64() - lastLog > 2000) {
			lastLog = GetTickCount64();
			Log("home: %d frames, shouldRender %d, views %u flags 0x%llx, eye y %.2f, hands %d %d (aim flags 0x%llx 0x%llx), dash frame %d", frames,
				(int)fs.shouldRender, nv, (unsigned long long)vs.viewStateFlags, in.eye[0].py, in.handValid[0], in.handValid[1],
				(unsigned long long)g_aimFlags[0], (unsigned long long)g_aimFlags[1], g_shm->frameSeq);
		}
		if (in.valid) {
			DashFrame(in, true);
			uint32_t idx;
			XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			xrAcquireSwapchainImage(swap, &ai, &idx);
			XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			wi.timeout = XR_INFINITE_DURATION;
			xrWaitSwapchainImage(swap, &wi);
			g_ctx->CopyResource(imgs[idx].texture, g_outTex.Get());
			if (frames == 200 || frames == 2000) {
				// Diagnostics: the middle of the left eye in our picture and in the runtime's image.
				auto px = [&](ID3D11Texture2D* tex) {
					D3D11_TEXTURE2D_DESC td;
					tex->GetDesc(&td);
					D3D11_TEXTURE2D_DESC sd = td;
					sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					ComPtr<ID3D11Texture2D> st;
					if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st))) return std::string("no staging (format ") + std::to_string(td.Format) + ")";
					g_ctx->CopyResource(st.Get(), tex);
					D3D11_MAPPED_SUBRESOURCE m;
					if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return std::string("no map");
					const uint8_t* c = (const uint8_t*)m.pData + (td.Height / 2) * m.RowPitch + (td.Width / 4) * 4;
					char b[160];
					snprintf(b, sizeof b, "%ux%u fmt %u misc 0x%x bind 0x%x: %u %u %u %u", td.Width, td.Height, td.Format, td.MiscFlags, td.BindFlags, c[0], c[1], c[2], c[3]);
					g_ctx->Unmap(st.Get(), 0);
					return std::string(b);
				};
				Log("home: ours %s | runtime's %s", px(g_outTex.Get()).c_str(), px(imgs[idx].texture).c_str());
			}
			XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(swap, &ri);
			for (int e = 0; e < 2; e++) {
				pv[e].pose = views[e].pose;
				pv[e].fov = views[e].fov;
				pv[e].subImage.swapchain = swap;
				pv[e].subImage.imageRect = { { (int32_t)(e * kEyeW), 0 }, { (int32_t)kEyeW, (int32_t)kEyeH } };
			}
			proj.space = space;
			proj.viewCount = 2;
			proj.views = pv;
			fei.layerCount = 1;
			fei.layers = layers;
		}
		xrEndFrame(session, &fei);
	}
	g_shm->homeActive = 0;
	g_shm->open = 0;
	g_anchored = false;
	xrDestroySwapchain(swap);
	xrDestroySession(session);
	xrDestroyInstance(inst);
	Log("home: session ended");
	return true;
}

// ---- preview: one frame to PNG files, without a headset -------------------------------------
static void SavePng(ID3D11Texture2D* tex, UINT x0, UINT w, UINT h, const std::wstring& path, bool overDark)
{
	D3D11_TEXTURE2D_DESC td;
	tex->GetDesc(&td);
	td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.MiscFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> st;
	g_dev->CreateTexture2D(&td, nullptr, &st);
	g_ctx->CopyResource(st.Get(), tex);
	D3D11_MAPPED_SUBRESOURCE m;
	g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m);
	std::vector<uint8_t> px(w * h * 4);
	for (UINT y = 0; y < h; y++)
		for (UINT x = 0; x < w; x++) {
			const uint8_t* s = (const uint8_t*)m.pData + y * m.RowPitch + (x0 + x) * 4;
			uint8_t* d = &px[(y * w + x) * 4];
			// premultiplied RGBA -> BGRA, over a dim "game" backdrop for overlay previews
			float a = s[3] / 255.0f;
			float bg[3] = { 38, 44, 58 };
			for (int c = 0; c < 3; c++) {
				float v = overDark ? s[c] + bg[c] * (1 - a) : s[c];
				d[2 - c] = (uint8_t)std::min(255.0f, v);
			}
			d[3] = overDark ? 255 : s[3];
		}
	g_ctx->Unmap(st.Get(), 0);
	ComPtr<IWICBitmapEncoder> enc;
	ComPtr<IWICStream> stream;
	g_wic->CreateStream(&stream);
	stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
	g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
	enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
	ComPtr<IWICBitmapFrameEncode> fr;
	enc->CreateNewFrame(&fr, nullptr);
	fr->Initialize(nullptr);
	fr->SetSize(w, h);
	WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA;
	fr->SetPixelFormat(&f);
	fr->WritePixels(h, w * 4, (UINT)px.size(), px.data());
	fr->Commit();
	enc->Commit();
}

static int Preview(const std::wstring& stateFile, const std::wstring& outDir, const std::wstring& tab)
{
	FILE* f = nullptr;
	std::string s;
	if (!_wfopen_s(&f, stateFile.c_str(), L"rb") && f) {
		char b[4096];
		size_t n;
		while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
		fclose(f);
	}
	{
		std::lock_guard<std::mutex> lk(g_stateLock);
		g_state = Json::parse(s);
		g_stateSeq++;
	}
	if (tab == L"settings") g_tab = Tab::Settings;
	if (tab == L"devices") g_tab = Tab::Devices;
	if (tab == L"playing") g_tab = Tab::Playing;
	if (tab == L"desktop") g_tab = Tab::Desktop;
	if (tab == L"controls") g_tab = Tab::Controls;
	auto pose = [](V3 p, Q q) { return Pose(p, q).raw(); };
	AtriumDashInput in = {};
	Q look = Q::axisAngle(V3(1, 0, 0), -0.10f);
	in.eye[0] = pose(V3(-0.032f, 1.6f, 0.35f), look);
	in.eye[1] = pose(V3(0.032f, 1.6f, 0.35f), look);
	in.fov[0] = in.fov[1] = { -0.95f, 0.95f, 0.80f, -0.80f };
	in.valid = 1;
	// Right hand points at the second library tile, left hand rests lower down.
	Q rAim = Q::axisAngle(V3(0, 1, 0), 0.18f) * Q::axisAngle(V3(1, 0, 0), 0.30f);
	in.aim[1] = pose(V3(0.20f, 1.18f, 0.0f), rAim);
	in.grip[1] = pose(V3(0.20f, 1.16f, 0.03f), rAim * Q::axisAngle(V3(1, 0, 0), -0.6f));
	Q lAim = Q::axisAngle(V3(0, 1, 0), -0.35f) * Q::axisAngle(V3(1, 0, 0), 0.05f);
	in.aim[0] = pose(V3(-0.24f, 1.12f, 0.02f), lAim);
	in.grip[0] = pose(V3(-0.24f, 1.10f, 0.05f), lAim * Q::axisAngle(V3(1, 0, 0), -0.6f));
	in.handValid[0] = in.handValid[1] = 1;

	// Home (with its surroundings), then the same dash over a game.
	g_shm->homeActive = 1;
	g_anchored = false;
	DashFrame(in, true);
	DashFrame(in, true); // second frame: hover from the first frame's hit boxes
	SavePng(g_outTex.Get(), 0, kEyeW, kEyeH, outDir + L"\\dash_home.png", false);
	g_shm->homeActive = 0;
	g_lastStateSeq = -1;
	DashFrame(in, false);
	DashFrame(in, false);
	SavePng(g_outTex.Get(), 0, kEyeW, kEyeH, outDir + L"\\dash_ingame.png", true);
	SavePng(g_uiTex.Get(), 0, kUiW, kUiH, outDir + L"\\dash_ui.png", true);
	return 0;
}

// ---- main -----------------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	PWSTR roaming = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
		g_dataDir = std::wstring(roaming) + L"\\Atrium";
		CoTaskMemFree(roaming);
	}
	SHCreateDirectoryExW(nullptr, (g_dataDir + L"\\logs").c_str(), nullptr);
	LoadLook();
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	g_preview = argc >= 4 && wcscmp(argv[1], L"--preview") == 0;
	if (!CreateGraphics() || !Create3D()) {
		Log("start failed");
		return 1;
	}
	if (g_preview) return Preview(argv[2], argv[3], argc >= 5 ? argv[4] : L"");
	Log("atrium-dash ready (%ux%u per eye)", kEyeW, kEyeH);
	std::thread(StdinThread).detach();
	std::thread(OculusButtonThread).detach();

	int lastInput = 0, lastClose = 0;
	bool wasOpen = false;
	while (!g_quit) {
		{
			std::lock_guard<std::mutex> lk(g_stateLock);
			g_homeWanted = g_state["home"].truthy();
		}
		if (g_homeWanted && !g_shm->homeActive) {
			// Home: our own session until a game starts (the app turns `home` off) or it ends.
			if (!RunHome()) Sleep(2000);
			continue;
		}
		// Inside a game: draw whenever the game side sends new eyes and controllers.
		bool open = g_shm->open != 0;
		if (open && !wasOpen) g_anchored = false; // re-place in front of you each time it opens
		wasOpen = open;
		int seq = g_shm->inputSeq;
		if (open && seq != lastInput && g_shm->input.valid) {
			lastInput = seq;
			AtriumDashInput in = g_shm->input;
			DashFrame(in, false);
		} else {
			Sleep(open ? 2 : 30);
		}
		(void)lastClose;
	}
	Log("atrium-dash: bye");
	return 0;
}
