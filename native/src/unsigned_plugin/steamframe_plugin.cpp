// SteamFrame.dll: an OVRServer headset plugin that makes a non-Meta headset a first-class Oculus device.
//
// OVRServer loads every DLL in its plugin folder that exports GetOVRInterfaceFactory. Meta ships an
// emulated-headset plugin (Emulation.dll) that already implements a complete HMD + two Touch
// controllers behind the full runtime: compositor, Dash, the Devices page, OpenXR, LibOVR. It is
// switched off unless OVREmulationPlugin is 1..3. This plugin loads a private copy of it
// (SteamFrame\EmulationCore.dll, kept in a subfolder so OVRServer doesn't load it a second time),
// turns it on, rewrites its "HMD X" description into a Steam Frame, and drives its pose and input
// from SteamFrameSim.exe through shared memory, using the same command interface that OVRServer's
// "emu" debug commands use.
//
// RVAs are for Emulation.dll from the Meta Horizon Link runtime 208.0.0.84 (Emulation.pdb build
// 84b2a646e2abfd96). CheckCore() and InstallTailHook() refuse to patch anything else.
#include <windows.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "steamframe_shared.h"

// ---- Emulation.dll layout ----------------------------------------------------------------------
namespace rva {
const uint32_t SetupTail = 0x7dead;    // mode-specific HmdInfo filled in, nothing derived from it yet
const uint32_t MakeString = 0x827c0;   // String** MakeString(String** out, const char*)
const uint32_t AssignString = 0x745c0; // void Assign(String** field, String** src)
// CommandInterface methods; `this` is EmulationHeadsetInterface + 8
const uint32_t SetConnect = 0x7a840;
const uint32_t SetPose = 0x7ab30;
const uint32_t SetButtonState = 0x7aff0;
const uint32_t SetIndexTrigger = 0x7b310;
const uint32_t SetHandTrigger = 0x7b490;
const uint32_t SetThumbPosition = 0x7b610;
}  // namespace rva

// EmulationHeadsetInterface fields written by the mode-3 ("HMD X") setup.
namespace off {
const uint32_t ProductName = 0x78, Manufacturer = 0x80;
const uint32_t Resolution = 0x90;          // int w, h (whole panel)
const uint32_t ScreenSize = 0x98;          // float w, h meters
const uint32_t CenterFromTop = 0xa4;       // float meters
const uint32_t LensSeparation = 0xa8;      // float meters
const uint32_t Eye[2] = {0xac, 0xe4};      // per eye: int w,h; float w,h; vec3 lens; vec3 eye; quat
const uint32_t VsyncPeriod = 0x3b0;        // float seconds
const uint32_t HmdSerial = 0x8c8;
const uint32_t UsbVid = 0x900, UsbPid = 0x904;
}  // namespace off

enum DevType { Dev_HMD = 0, Dev_LTouch = 1, Dev_RTouch = 2, Dev_Camera = 6 };
enum EmuButton { Btn_AX = 0, Btn_BY = 1, Btn_Home = 2, Btn_Thumb = 3 };

struct EmuPose {  // what SetPose copies (0x38 bytes); the device keeps the first 32
    float qx, qy, qz, qw;
    float px, py, pz, pad;
    float extra[6];
};

typedef void** (*MakeString_t)(void** out, const char* s);
typedef void (*AssignString_t)(void** field, void** src);
typedef void (*SetConnect_t)(void* cmd, int type, int idx, bool on, double t);
typedef void (*SetPose_t)(void* cmd, int type, int idx, const EmuPose* p, double t);
typedef void (*SetButtonState_t)(void* cmd, int type, int idx, int button, bool down, double t);
typedef void (*SetTrigger_t)(void* cmd, int type, int idx, float v, double t);
typedef void (*SetThumbPosition_t)(void* cmd, int type, int idx, const float* xy, double t);

static HMODULE g_self, g_core;
static uint8_t* g_base;
static void* volatile g_iface;
static HANDLE g_thread;
static volatile bool g_stop;

// ---- log ---------------------------------------------------------------------------------------
static void Log(const char* fmt, ...) {
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH)) return;
    strcat_s(path, "\\Oculus\\SteamFrame.log");
    FILE* f;
    if (fopen_s(&f, path, "a")) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fclose(f);
}

template <class T> static T At(uint32_t r) { return (T)(g_base + r); }
template <class T> static T& Field(void* obj, uint32_t o) { return *(T*)((uint8_t*)obj + o); }

static void SetString(void* iface, uint32_t o, const char* s) {
    void* tmp = nullptr;
    void** src = At<MakeString_t>(rva::MakeString)(&tmp, s);
    At<AssignString_t>(rva::AssignString)((void**)((uint8_t*)iface + o), src);
    if (tmp) (*(void (***)(void*))tmp)[2](tmp);  // Release
}

// ---- Steam Frame description -------------------------------------------------------------------
// 2160x2160 per eye, ~110 degree lenses, 90 Hz default (the panel does 72-144).
static void ApplySteamFrame(void* iface) {
    SetString(iface, off::ProductName, "Steam Frame");
    SetString(iface, off::Manufacturer, "Valve");
    SetString(iface, off::HmdSerial, "SFRAME00000001");
    Field<uint32_t>(iface, off::UsbVid) = 0x28de;
    Field<uint32_t>(iface, off::UsbPid) = 0x2300;

    const int eye_px = 2160;
    // FOV comes from screen half-size over the lens distance (0.047124 m in HMD X).
    // tan(55 deg) * 0.047124 = 0.0673 m half size -> ~110 degrees across each axis.
    const float lens_z = 0.047124f;
    const float half = tanf(55.0f * 3.14159265f / 180.0f) * lens_z;
    Field<int>(iface, off::Resolution) = eye_px * 2;
    Field<int>(iface, off::Resolution + 4) = eye_px;
    Field<float>(iface, off::ScreenSize) = half * 4;
    Field<float>(iface, off::ScreenSize + 4) = half * 2;
    Field<float>(iface, off::CenterFromTop) = half;
    for (int e = 0; e < 2; e++) {
        uint8_t* b = (uint8_t*)iface + off::Eye[e];
        ((int*)b)[0] = eye_px;
        ((int*)b)[1] = eye_px;
        ((float*)b)[2] = half * 2;
        ((float*)b)[3] = half * 2;
        ((float*)b)[5] = 0.0f;  // lens centre: no vertical offset, symmetric FOV
        ((float*)b)[8] = 0.0f;
    }
    Field<float>(iface, off::VsyncPeriod) = 1.0f / 90.0f;
    g_iface = iface;
    Log("applied Steam Frame description to interface %p", iface);
}

// Mid-function hook at SetupTail: save volatile state, call ApplySteamFrame(rdi), replay the
// 17 displaced bytes (two movs into [rdi+...]) and jump back.
static const uint8_t kTailBytes[17] = {0xC6, 0x87, 0x18, 0x09, 0x00, 0x00, 0x01,
                                       0xC7, 0x87, 0x1B, 0x09, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01};
static bool InstallTailHook() {
    uint8_t* site = g_base + rva::SetupTail;
    if (memcmp(site, kTailBytes, sizeof(kTailBytes))) {
        Log("setup tail bytes differ; not patching");
        return false;
    }
    uint8_t* stub = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    uint8_t* p = stub;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *p++ = x; };
    auto put64 = [&](uint64_t v) { memcpy(p, &v, 8); p += 8; };
    put({0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // push rax rcx rdx r8-r11
    put({0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00});  // sub rsp, 0x88: xmm0-5, shadow space, alignment
    for (int i = 0; i < 6; i++)  // movdqu [rsp+0x20+16i], xmm_i
        put({0xF3, 0x0F, 0x7F, (uint8_t)(0x44 | (i << 3)), 0x24, (uint8_t)(0x20 + i * 16)});
    put({0x48, 0x89, 0xF9});  // mov rcx, rdi
    put({0x48, 0xB8}); put64((uint64_t)&ApplySteamFrame);  // mov rax, imm64
    put({0xFF, 0xD0});  // call rax
    for (int i = 0; i < 6; i++)
        put({0xF3, 0x0F, 0x6F, (uint8_t)(0x44 | (i << 3)), 0x24, (uint8_t)(0x20 + i * 16)});
    put({0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00});
    put({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58});
    memcpy(p, kTailBytes, sizeof(kTailBytes)); p += sizeof(kTailBytes);
    put({0xFF, 0x25, 0, 0, 0, 0}); put64((uint64_t)(site + sizeof(kTailBytes)));  // jmp [rip]

    DWORD old;
    VirtualProtect(site, sizeof(kTailBytes), PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xFF; site[1] = 0x25; *(uint32_t*)(site + 2) = 0; *(uint64_t*)(site + 6) = (uint64_t)stub;
    memset(site + 14, 0x90, 3);
    VirtualProtect(site, sizeof(kTailBytes), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kTailBytes));
    return true;
}

// ---- driving the device ------------------------------------------------------------------------
static SfShared* OpenShared() {
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SF_MAP_NAME);
    if (!h) return nullptr;
    SfShared* s = (SfShared*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SfShared));
    CloseHandle(h);
    if (s && (s->magic != SF_MAGIC || s->version != SF_VERSION)) { UnmapViewOfFile(s); s = nullptr; }
    return s;
}

static EmuPose ToEmu(const SfPose& p) {
    EmuPose e = {};
    e.qx = p.qx; e.qy = p.qy; e.qz = p.qz; e.qw = p.qw;
    e.px = p.px; e.py = p.py; e.pz = p.pz;
    return e;
}

static DWORD WINAPI Pump(void*) {
    while (!g_iface && !g_stop) Sleep(50);
    Sleep(1500);  // let the devices finish attaching
    void* cmd = (uint8_t*)g_iface + 8;
    auto setConnect = At<SetConnect_t>(rva::SetConnect);
    auto setPose = At<SetPose_t>(rva::SetPose);
    auto setButton = At<SetButtonState_t>(rva::SetButtonState);
    auto setITrig = At<SetTrigger_t>(rva::SetIndexTrigger);
    auto setHTrig = At<SetTrigger_t>(rva::SetHandTrigger);
    auto setThumb = At<SetThumbPosition_t>(rva::SetThumbPosition);

    // Inside-out headset: no external sensors.
    setConnect(cmd, Dev_Camera, 0, false, 0);
    setConnect(cmd, Dev_Camera, 1, false, 0);
    Log("pump running");

    SfShared* s = nullptr;
    uint32_t last_seq = 0, last_buttons[2] = {0, 0};
    float last_it[2] = {-1, -1}, last_ht[2] = {-1, -1}, last_sx[2] = {9, 9}, last_sy[2] = {9, 9};
    DWORD retry = 0;
    while (!g_stop) {
        if (!s && GetTickCount() - retry > 1000) {
            retry = GetTickCount();
            if ((s = OpenShared())) Log("simulator attached (pid %u)", s->sim_pid);
        }
        if (s) {
            s->heartbeat++;
            if (s->seq != last_seq) {
                last_seq = s->seq;
                EmuPose hp = ToEmu(s->head);
                setPose(cmd, Dev_HMD, 0, &hp, 0);
                for (int h = 0; h < 2; h++) {
                    const SfHand& hd = s->hand[h];
                    int dev = h ? Dev_RTouch : Dev_LTouch;
                    EmuPose p = ToEmu(hd.pose);
                    setPose(cmd, dev, 0, &p, 0);
                    uint32_t changed = hd.buttons ^ last_buttons[h];
                    if (changed & SF_BTN_A) setButton(cmd, dev, 0, Btn_AX, (hd.buttons & SF_BTN_A) != 0, 0);
                    if (changed & SF_BTN_B) setButton(cmd, dev, 0, Btn_BY, (hd.buttons & SF_BTN_B) != 0, 0);
                    if (changed & SF_BTN_MENU) setButton(cmd, dev, 0, Btn_Home, (hd.buttons & SF_BTN_MENU) != 0, 0);
                    if (changed & SF_BTN_STICK) setButton(cmd, dev, 0, Btn_Thumb, (hd.buttons & SF_BTN_STICK) != 0, 0);
                    last_buttons[h] = hd.buttons;
                    // The emulator's trigger value runs 1 released .. 0 pulled.
                    if (hd.index_trigger != last_it[h]) { setITrig(cmd, dev, 0, 1.0f - hd.index_trigger, 0); last_it[h] = hd.index_trigger; }
                    if (hd.grip != last_ht[h]) { setHTrig(cmd, dev, 0, 1.0f - hd.grip, 0); last_ht[h] = hd.grip; }
                    if (hd.stick_x != last_sx[h] || hd.stick_y != last_sy[h]) {
                        float xy[2] = {hd.stick_x, hd.stick_y};
                        setThumb(cmd, dev, 0, xy, 0);
                        last_sx[h] = hd.stick_x; last_sy[h] = hd.stick_y;
                    }
                }
            }
        }
        Sleep(2);
    }
    if (s) UnmapViewOfFile(s);
    return 0;
}

// ---- core loading and exports ------------------------------------------------------------------
static bool CheckCore() {
    // The command methods start with "mov [rsp+10h], rbx"; the tail hook checks its own bytes.
    static const uint8_t prologue[5] = {0x48, 0x89, 0x5C, 0x24, 0x10};
    const uint32_t fns[] = {rva::SetConnect, rva::SetPose, rva::SetButtonState, rva::SetThumbPosition};
    for (uint32_t f : fns) {
        if (memcmp(g_base + f, prologue, 5)) {
            Log("core mismatch at %x: wrong Emulation.dll build, staying a plain emulator", f);
            return false;
        }
    }
    return true;
}

static bool LoadCore() {
    if (g_core) return true;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"SteamFrame\\EmulationCore.dll");
    // The core only enables itself when this is 1..3; 3 is the generic "HMD X" we rebuild.
    SetEnvironmentVariableW(L"OVREmulationPlugin", L"3");
    g_core = LoadLibraryW(path);
    if (!g_core) {
        Log("cannot load core %ls (%lu)", path, GetLastError());
        return false;
    }
    g_base = (uint8_t*)g_core;
    Log("core loaded at %p", g_base);
    if (CheckCore() && InstallTailHook()) {
        g_thread = CreateThread(nullptr, 0, Pump, nullptr, 0, nullptr);
    }
    return true;
}

typedef void* (*Fwd4)(void*, void*, void*, void*);
static void* Forward(const char* name, void* a, void* b, void* c, void* d) {
    if (!LoadCore()) return nullptr;
    Fwd4 f = (Fwd4)GetProcAddress(g_core, name);
    return f ? f(a, b, c, d) : nullptr;
}

extern "C" __declspec(dllexport) void* GetOVRInterfaceFactory(void* a, void* b, void* c, void* d) {
    return Forward("GetOVRInterfaceFactory", a, b, c, d);
}
extern "C" __declspec(dllexport) void* OVRInterfaceInitialize(void* a, void* b, void* c, void* d) {
    return Forward("OVRInterfaceInitialize", a, b, c, d);
}
extern "C" __declspec(dllexport) void* OVRInterfaceShutdown(void* a, void* b, void* c, void* d) {
    g_stop = true;
    if (g_thread) { WaitForSingleObject(g_thread, 500); CloseHandle(g_thread); g_thread = nullptr; }
    return Forward("OVRInterfaceShutdown", a, b, c, d);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
