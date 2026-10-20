// SteamFrameSim.exe: a simulated Valve Steam Frame for the Oculus runtime.
//
// The headset itself is Meta's signed emulated-headset plugin (Emulation.dll, switched on with
// OVREmulationPlugin=3). This program plays the hardware: it keeps one OculusDebugToolCLI session
// open and streams "server:emu ..." commands into it (head pose, controller poses, buttons,
// triggers, sticks, proximity). What the runtime renders shows in OVRServer's
// "OculusEmuDirectDisplay" window: that's the headset's panels.
//
// Controls (this window focused):
//   right mouse drag     look around            Ctrl + W/A/S/D   walk (head position)
//   PgUp / PgDn          stand up / crouch      R                recenter
//   W/A/S/D              left stick             arrow keys       right stick
//   left mouse           right trigger          G                right grip
//   C                    left trigger           V                left grip
//   1 / 2                A / B                  3 / 4            X / Y
//   Tab                  menu                   Z / M            left / right stick click
//   H                    hands: raised / lowered    P             headset on / off (proximity)
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <string>

static const wchar_t* kCli = L"C:\\Program Files\\Oculus\\Support\\oculus-diagnostics\\OculusDebugToolCLI.exe";
static const char* kHmdSerial = "HMD0000000000";

struct Pose { float px, py, pz, qx, qy, qz, qw; };

struct Hand {
    Pose pose;
    bool a, b, menu, stick_click;
    float trigger, grip, sx, sy;
};

struct State {
    Pose head;
    Hand hand[2];  // 0 left, 1 right
    bool worn;
};

// ---- the Debug Tool session -------------------------------------------------------------------
static HANDLE g_cli_in, g_cli_proc;
static volatile LONG g_cli_lines;  // lines of output seen, as a liveness hint
static char g_cli_last[256];
static DWORD g_cli_started;

static DWORD WINAPI DrainOutput(void* h) {
    char buf[4096];
    DWORD n;
    std::string line;
    while (ReadFile((HANDLE)h, buf, sizeof(buf), &n, nullptr) && n) {
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!line.empty() && line.find("=>") == std::string::npos) strncpy_s(g_cli_last, line.c_str(), _TRUNCATE);
                line.clear();
                InterlockedIncrement(&g_cli_lines);
            } else if (buf[i] != '\r') line += buf[i];
        }
    }
    CloseHandle((HANDLE)h);
    return 0;
}

static bool Send(const char* text) {
    if (!g_cli_in) return false;
    DWORD n;
    if (!WriteFile(g_cli_in, text, (DWORD)strlen(text), &n, nullptr)) {
        CloseHandle(g_cli_in);
        g_cli_in = nullptr;
        return false;
    }
    return true;
}

static void Sendf(std::string& batch, const char* fmt, ...) {
    char line[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    batch += line;
}

static bool StartCli() {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 1 << 16)) return false;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 16)) return false;
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi;
    wchar_t cmd[MAX_PATH + 4];
    swprintf_s(cmd, L"\"%s\"", kCli);
    BOOL ok = CreateProcessW(kCli, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        strcpy_s(g_cli_last, "cannot start OculusDebugToolCLI.exe");
        return false;
    }
    CloseHandle(pi.hThread);
    g_cli_proc = pi.hProcess;
    g_cli_in = in_w;
    CloseHandle(CreateThread(nullptr, 0, DrainOutput, out_r, 0, nullptr));
    g_cli_started = GetTickCount();
    // Make this the active headset, put it on, and drop the emulator's two external cameras:
    // the Steam Frame tracks inside-out.
    std::string init;
    Sendf(init, "server:SetHmd %s\n", kHmdSerial);
    Sendf(init, "server:emu connect camera0 0\nserver:emu connect camera1 0\n");
    Sendf(init, "server:emu connect ltouch 1\nserver:emu connect rtouch 1\n");
    Send(init.c_str());
    return true;
}

static bool CliAlive() {
    return g_cli_proc && WaitForSingleObject(g_cli_proc, 0) == WAIT_TIMEOUT && g_cli_in;
}

// ---- simulation --------------------------------------------------------------------------------
static float g_yaw, g_pitch, g_x, g_y, g_z;
static bool g_hands_up = true, g_worn = true;
static POINT g_drag;
static bool g_dragging, g_lmb;
static State g_sent;
static bool g_have_sent;
static DWORD g_last_full;

static void YawPitch(float yaw, float pitch, Pose& p) {
    // yaw about +Y, then pitch about +X (Oculus: -Z forward, +Y up)
    float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    p.qw = cy * cp;
    p.qx = cy * sp;
    p.qy = sy * cp;
    p.qz = -sy * sp;
}

static bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static State Sample(HWND wnd, float dt) {
    State s = {};
    bool focused = GetForegroundWindow() == wnd;
    bool ctrl = focused && Key(VK_CONTROL);
    float fx = 0, fz = 0;
    if (focused) {
        if (Key('W')) fz -= 1;
        if (Key('S')) fz += 1;
        if (Key('A')) fx -= 1;
        if (Key('D')) fx += 1;
        if (Key(VK_PRIOR)) g_y = fminf(g_y + dt, 0.4f);
        if (Key(VK_NEXT)) g_y = fmaxf(g_y - dt, -1.0f);
    }
    if (ctrl) {
        float c = cosf(g_yaw), sn = sinf(g_yaw), sp = 1.5f * dt;
        g_x += (fx * c + fz * sn) * sp;
        g_z += (-fx * sn + fz * c) * sp;
    }
    YawPitch(g_yaw, g_pitch, s.head);
    s.head.px = g_x; s.head.py = g_y; s.head.pz = g_z;
    s.worn = g_worn;

    for (int h = 0; h < 2; h++) {
        Hand& hd = s.hand[h];
        float lx = h ? 0.2f : -0.2f, ly = g_hands_up ? -0.25f : -0.55f, lz = g_hands_up ? -0.4f : -0.15f;
        float c = cosf(g_yaw), sn = sinf(g_yaw);
        hd.pose.px = g_x + lx * c + lz * sn;
        hd.pose.py = g_y + ly;
        hd.pose.pz = g_z - lx * sn + lz * c;
        YawPitch(g_yaw, g_hands_up ? -0.3f : -1.2f, hd.pose);
        if (!focused) continue;
        if (h == 0) {
            if (!ctrl) { hd.sx = fx; hd.sy = -fz; }
            hd.trigger = Key('C') ? 1.0f : 0.0f;
            hd.grip = Key('V') ? 1.0f : 0.0f;
            hd.a = Key('3');
            hd.b = Key('4');
            hd.menu = Key(VK_TAB);
            hd.stick_click = Key('Z');
        } else {
            if (Key(VK_LEFT)) hd.sx -= 1;
            if (Key(VK_RIGHT)) hd.sx += 1;
            if (Key(VK_UP)) hd.sy += 1;
            if (Key(VK_DOWN)) hd.sy -= 1;
            hd.trigger = g_lmb ? 1.0f : 0.0f;
            hd.grip = Key('G') ? 1.0f : 0.0f;
            hd.a = Key('1');
            hd.b = Key('2');
            hd.stick_click = Key('M');
        }
    }
    return s;
}

static bool PoseChanged(const Pose& a, const Pose& b) {
    const float* x = &a.px; const float* y = &b.px;
    for (int i = 0; i < 7; i++) if (fabsf(x[i] - y[i]) > 1e-5f) return true;
    return false;
}

static void AddPose(std::string& out, const char* dev, const Pose& p) {
    Sendf(out, "server:emu setpose %s %.4f %.4f %.4f %.5f %.5f %.5f %.5f\n", dev, p.px, p.py, p.pz, p.qx, p.qy, p.qz, p.qw);
}

// Only what changed goes out, plus a full refresh every 2 s in case the runtime restarted.
static void Publish(const State& s) {
    bool full = !g_have_sent || GetTickCount() - g_last_full > 2000;
    if (full) g_last_full = GetTickCount();
    const State& o = g_sent;
    std::string out;
    if (full || PoseChanged(s.head, o.head)) AddPose(out, "hmd", s.head);
    if (full || s.worn != o.worn) Sendf(out, "server:emu prox %d\n", s.worn ? 1 : 0);
    static const char* dev[2] = {"ltouch", "rtouch"};
    static const char* btnA[2] = {"x", "a"};
    static const char* btnB[2] = {"y", "b"};
    static const char* stick[2] = {"lt", "rt"};
    for (int h = 0; h < 2; h++) {
        const Hand& n = s.hand[h];
        const Hand& p = o.hand[h];
        if (full || PoseChanged(n.pose, p.pose)) AddPose(out, dev[h], n.pose);
        if (full || n.a != p.a) Sendf(out, "server:emu setbutton %s %s %d\n", dev[h], btnA[h], n.a);
        if (full || n.b != p.b) Sendf(out, "server:emu setbutton %s %s %d\n", dev[h], btnB[h], n.b);
        if (full || n.stick_click != p.stick_click) Sendf(out, "server:emu setbutton %s %s %d\n", dev[h], stick[h], n.stick_click);
        if (h == 0 && (full || n.menu != p.menu)) Sendf(out, "server:emu setbutton ltouch enter %d\n", n.menu);
        // Apps see the value as sent (0 released .. 1 pulled), whatever the CLI help says.
        if (full || n.trigger != p.trigger) Sendf(out, "server:emu setitrigger %s %.3f\n", dev[h], n.trigger);
        if (full || n.grip != p.grip) Sendf(out, "server:emu sethtrigger %s %.3f\n", dev[h], n.grip);
        if (full || n.sx != p.sx || n.sy != p.sy) Sendf(out, "server:emu setthumbstick %s %.3f %.3f\n", dev[h], n.sx, n.sy);
    }
    if (out.empty()) return;
    if (Send(out.c_str())) {
        g_sent = s;
        g_have_sent = true;
    }
}

// ---- Dash restarts -----------------------------------------------------------------------------
// A freshly started Dash only takes VR focus (and draws) after the headset is put on, i.e. a
// proximity off -> on edge. A real headset gives that when worn; the sim fakes it whenever
// OculusDash.exe is new (sim start, Dash crash/restart, OVRServer restart).
static DWORD g_dash_pid, g_remount_at;

static DWORD FindDash() {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, L"OculusDash.exe")) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    return pid;
}

static void WatchDash() {
    DWORD pid = FindDash();
    if (pid && pid != g_dash_pid) g_remount_at = GetTickCount() + 2000;  // give it time to connect
    g_dash_pid = pid;
    if (g_remount_at && GetTickCount() >= g_remount_at && g_worn && CliAlive()) {
        g_remount_at = 0;
        Send("server:emu prox 0\n");
        Sleep(300);
        Send("server:emu prox 1\n");
    }
}

// ---- the headset's display ---------------------------------------------------------------------
// The runtime presents into OVRServer's "OculusEmuDirectDisplay" window. A DWM thumbnail shows that
// window live inside ours (the compositor does the copy; nothing touches OVRServer).
static const int kHeader = 64;
static HWND g_src;
static HTHUMBNAIL g_thumb;
static bool g_help, g_src_hidden;
static RECT g_src_home;  // where the display window was before we moved it off-screen

static void HideSource(bool hide) {
    if (!g_src || hide == g_src_hidden) return;
    if (hide) {
        GetWindowRect(g_src, &g_src_home);
        // Off-screen, not minimized: a minimized window stops being composed.
        int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 50;
        SetWindowPos(g_src, nullptr, x, g_src_home.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(g_src, nullptr, g_src_home.left, g_src_home.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_src_hidden = hide;
}

static void LayoutThumb(HWND w) {
    if (!g_thumb) return;
    RECT rc, src;
    GetClientRect(w, &rc);
    GetClientRect(g_src, &src);
    int aw = rc.right, ah = rc.bottom - kHeader;
    int sw = src.right > 0 ? src.right : 16, sh = src.bottom > 0 ? src.bottom : 9;
    int dw = aw, dh = aw * sh / sw;  // fit, keep the panels' aspect
    if (dh > ah) { dh = ah; dw = ah * sw / sh; }
    DWM_THUMBNAIL_PROPERTIES p = {};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY;
    p.rcDestination = {(aw - dw) / 2, kHeader + (ah - dh) / 2, (aw - dw) / 2 + dw, kHeader + (ah - dh) / 2 + dh};
    p.fSourceClientAreaOnly = TRUE;
    p.fVisible = !g_help;
    p.opacity = 255;
    DwmUpdateThumbnailProperties(g_thumb, &p);
}

// Finds (again, after an OVRServer restart) the display window and attaches the thumbnail.
static void TrackDisplay(HWND w) {
    if (g_src && IsWindow(g_src)) return;
    if (g_thumb) { DwmUnregisterThumbnail(g_thumb); g_thumb = nullptr; }
    g_src = FindWindowW(nullptr, L"OculusEmuDirectDisplay");
    g_src_hidden = false;
    HRESULT hr = g_src ? DwmRegisterThumbnail(w, g_src, &g_thumb) : E_FAIL;
    if (SUCCEEDED(hr)) LayoutThumb(w);
    if (g_src) {
        SIZE ss = {};
        if (g_thumb) DwmQueryThumbnailSourceSize(g_thumb, &ss);
        RECT cr = {};
        GetClientRect(g_src, &cr);
        DWORD pid = 0;
        GetWindowThreadProcessId(g_src, &pid);
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"SteamFrameSim.log");
        FILE* f;
        if (!_wfopen_s(&f, path, L"a")) {
            fprintf(f, "display window %p pid %lu hr %08lx source %ldx%ld client %ldx%ld style %08lx ex %08lx\n", g_src, pid, hr, ss.cx, ss.cy,
                    cr.right, cr.bottom, (DWORD)GetWindowLongPtrW(g_src, GWL_STYLE), (DWORD)GetWindowLongPtrW(g_src, GWL_EXSTYLE));
            fclose(f);
        }
    }
    InvalidateRect(w, nullptr, TRUE);
}

// ---- icon: a headset seen from the front, drawn here (no Valve artwork) ------------------------
static HICON MakeIcon(int size) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HGDIOBJ old = SelectObject(dc, color);
    RECT all = {0, 0, size, size};
    HBRUSH bg = CreateSolidBrush(RGB(23, 26, 33));
    FillRect(dc, &all, bg);
    float u = size / 32.0f;
    HBRUSH shell = CreateSolidBrush(RGB(232, 234, 238));
    HBRUSH glass = CreateSolidBrush(RGB(26, 159, 255));
    HPEN none = (HPEN)GetStockObject(NULL_PEN);
    SelectObject(dc, none);
    SelectObject(dc, shell);  // strap
    Rectangle(dc, (int)(2 * u), (int)(13 * u), (int)(30 * u), (int)(17 * u));
    RoundRect(dc, (int)(5 * u), (int)(8 * u), (int)(27 * u), (int)(24 * u), (int)(9 * u), (int)(9 * u));  // shell
    SelectObject(dc, glass);  // visor band
    RoundRect(dc, (int)(7 * u), (int)(12 * u), (int)(25 * u), (int)(19 * u), (int)(5 * u), (int)(5 * u));
    SelectObject(dc, old);
    HDC mdc = CreateCompatibleDC(screen);  // all-zero mask: fully opaque square icon
    HGDIOBJ mold = SelectObject(mdc, mask);
    PatBlt(mdc, 0, 0, size, size, BLACKNESS);
    SelectObject(mdc, mold);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(bg); DeleteObject(shell); DeleteObject(glass);
    DeleteObject(color); DeleteObject(mask);
    DeleteDC(mdc); DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}
static HICON g_icon_big;

// ---- window ------------------------------------------------------------------------------------
static void PaintHeader(HWND w, HDC dc, const RECT& rc) {
    RECT hdr = {0, 0, rc.right, kHeader};
    HBRUSH bar = CreateSolidBrush(RGB(23, 26, 33));
    FillRect(dc, &hdr, bar);
    DeleteObject(bar);
    DrawIconEx(dc, 12, 12, g_icon_big, 40, 40, 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    HFONT title = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT sub = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, title);
    SetTextColor(dc, RGB(240, 240, 240));
    TextOutW(dc, 64, 9, L"Steam Frame", 11);
    SelectObject(dc, sub);
    SetTextColor(dc, RGB(150, 160, 175));
    const State& s = g_sent;
    wchar_t t[256];
    swprintf_s(t, L"simulated  |  runtime link %s  |  display %s  |  headset %s  |  yaw %+.0f  pitch %+.0f  |  F1 controls",
               CliAlive() ? L"on" : L"restarting", !g_src ? L"waiting" : g_src_hidden ? L"here only (O)" : L"mirrored (O hides Meta's)",
               s.worn ? L"on" : L"off", g_yaw * 57.2958f, g_pitch * 57.2958f);
    TextOutW(dc, 64, 38, t, (int)wcslen(t));
    SelectObject(dc, of);
    DeleteObject(title);
    DeleteObject(sub);
}

static void PaintBody(HDC dc, const RECT& rc) {
    RECT body = {0, kHeader, rc.right, rc.bottom};
    FillRect(dc, &body, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 220, 220));
    HFONT f = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    RECT tr = {24, kHeader + 20, rc.right - 24, rc.bottom};
    if (g_help || !g_src) {
        const State& s = g_sent;
        wchar_t t[2048];
        swprintf_s(t,
                   L"%s"
                   L"look: right mouse drag        walk: Ctrl + W/A/S/D        stand / crouch: PgUp / PgDn\n"
                   L"left stick: W/A/S/D           right stick: arrow keys\n"
                   L"right trigger / grip: left mouse / G        left trigger / grip: C / V\n"
                   L"A B: 1 2     X Y: 3 4     menu: Tab     stick clicks: Z / M\n"
                   L"recenter: R     hands raised/lowered: H     headset on/off: P     hide Meta's display window: O\n\n"
                   L"L  trigger %.0f  grip %.0f  stick %+.0f %+.0f   X%d Y%d menu%d click%d\n"
                   L"R  trigger %.0f  grip %.0f  stick %+.0f %+.0f   A%d B%d click%d\n\n"
                   L"Debug Tool: %S\n\nF1 back to the display",
                   g_src ? L"" : L"Waiting for the headset display (OVRServer's OculusEmuDirectDisplay window)...\n\n",
                   s.hand[0].trigger, s.hand[0].grip, s.hand[0].sx, s.hand[0].sy, s.hand[0].a, s.hand[0].b, s.hand[0].menu, s.hand[0].stick_click,
                   s.hand[1].trigger, s.hand[1].grip, s.hand[1].sx, s.hand[1].sy, s.hand[1].a, s.hand[1].b, s.hand[1].stick_click,
                   g_cli_last);
        DrawTextW(dc, t, -1, &tr, DT_LEFT);
    }
    SelectObject(dc, of);
    DeleteObject(f);
}

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_RBUTTONDOWN: g_dragging = true; g_drag = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; SetCapture(w); return 0;
    case WM_RBUTTONUP: g_dragging = false; ReleaseCapture(); return 0;
    case WM_MOUSEACTIVATE:
        // The click that focuses the window must not also pull the right trigger: Dash would
        // "click" whatever the controller points at.
        return GetForegroundWindow() == w ? MA_ACTIVATE : MA_ACTIVATEANDEAT;
    case WM_LBUTTONDOWN: g_lmb = true; return 0;
    case WM_LBUTTONUP: g_lmb = false; return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            g_yaw = remainderf(g_yaw - (x - g_drag.x) * 0.005f, 6.2831853f);  // keep within +-180
            g_pitch = fmaxf(-1.5f, fminf(1.5f, g_pitch - (y - g_drag.y) * 0.005f));
            g_drag = {x, y};
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == 'R') g_yaw = g_pitch = g_x = g_y = g_z = 0;
        if (wp == 'H') g_hands_up = !g_hands_up;
        if (wp == 'P') g_worn = !g_worn;
        if (wp == VK_F1) { g_help = !g_help; LayoutThumb(w); InvalidateRect(w, nullptr, FALSE); }
        if (wp == 'O') { HideSource(!g_src_hidden); InvalidateRect(w, nullptr, FALSE); }
        return 0;
    case WM_SIZE: LayoutThumb(w); InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        // Draw off-screen, then copy once: no flicker on the 10 Hz header refresh.
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        PaintHeader(w, mem, rc);
        PaintBody(mem, rc);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
               mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DESTROY:
        HideSource(false);  // give Meta's window its place back
        if (g_thumb) DwmUnregisterThumbnail(g_thumb);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_icon_big = MakeIcon(48);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = MakeIcon(32);
    wc.lpszClassName = L"SteamFrameSim";
    RegisterClassW(&wc);
    HWND w = CreateWindowW(wc.lpszClassName, L"Steam Frame (simulated)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1200, 760, nullptr, nullptr, inst, nullptr);
    SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)g_icon_big);
    SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)MakeIcon(16));
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int frame = 0;
    DWORD next_try = 0;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                if (g_cli_in) Send("exit\n");
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!CliAlive() && GetTickCount() >= next_try) {
            if (g_cli_in) { CloseHandle(g_cli_in); g_cli_in = nullptr; }
            if (g_cli_proc) { CloseHandle(g_cli_proc); g_cli_proc = nullptr; }
            g_have_sent = false;
            if (!StartCli()) next_try = GetTickCount() + 2000;
        }
        QueryPerformanceCounter(&t1);
        float dt = (float)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
        t0 = t1;
        State s = Sample(w, dt);
        if (CliAlive()) Publish(s);
        if (++frame % 10 == 0) {
            TrackDisplay(w);
            WatchDash();
            RECT hdr;
            GetClientRect(w, &hdr);
            if (!g_help && g_src) hdr.bottom = kHeader;  // the display itself is DWM's job
            InvalidateRect(w, &hdr, FALSE);
        }
        Sleep(11);  // ~90 Hz, the headset's refresh
    }
}
