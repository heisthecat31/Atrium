// Atrium OVR entry point. Replaces Revive's ReviveXR/main.cpp (its injection glue).
//
// Atrium doesn't inject: the game's own LibOVR loader finds this DLL through LIBOVR_DLL_DIR.
// Two of Revive's hooks still matter once loaded, so they're kept:
//   - OpenEvent: games (Unity especially) poll Meta's "HMD connected" event before they start
//     VR. Meta's service owns that event; without it running the game would wait forever, so
//     opening it returns an event that is always set.
//   - LoadLibrary(Ex)W: a game or plugin that loads LibOVRRT64_1.dll again by name gets this
//     DLL (with its ref count bumped), never Meta's from another folder.
// ReviveXR calls DetachDetours/AttachDetours around xrCreateInstance so the OpenXR loader's
// own LoadLibrary calls run unhooked.
//
// Log: %APPDATA%\Atrium\logs\ovr.log (one per run, overwritten).
#include <Windows.h>
#include <Shlwapi.h>
#include <ShlObj.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <detours/detours.h>

#include "OVR_CAPI.h"
#include "OVR_Version.h"

static HMODULE(WINAPI* TrueLoadLibraryW)(LPCWSTR) = LoadLibraryW;
static HMODULE(WINAPI* TrueLoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = LoadLibraryExW;
static HANDLE(WINAPI* TrueOpenEventW)(DWORD, BOOL, LPCWSTR) = OpenEventW;

static WCHAR g_selfPath[MAX_PATH];
static WCHAR g_ovrName[64];   // "LibOVRRT64_1"
static WCHAR g_logPath[MAX_PATH];
static CRITICAL_SECTION g_logLock;

void AtriumOvrLog(const char* fmt, ...)
{
	if (!g_logPath[0]) return;
	EnterCriticalSection(&g_logLock);
	FILE* f = nullptr;
	if (_wfopen_s(&f, g_logPath, L"a") == 0 && f) {
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
	LeaveCriticalSection(&g_logLock);
}

// Revive's CHK_XR (patched, see patches/0001): every failed OpenXR call, with its source line.
void AtriumOvrLogFail(const char* call, int result, const char* file, int line)
{
	const char* base = strrchr(file, '\\');
	AtriumOvrLog("FAILED %d at %s:%d  %s", result, base ? base + 1 : file, line, call);
}

bool AtriumOvrEverySecond(long long* last)
{
	long long now = (long long)GetTickCount64();
	if (now - *last < 1000) return false;
	*last = now;
	return true;
}

static bool IsOvrRuntime(LPCWSTR path)
{
	if (!path) return false;
	LPCWSTR name = PathFindFileNameW(path);
	size_t len = wcslen(g_ovrName);
	return _wcsnicmp(name, g_ovrName, len) == 0 && (name[len] == L'\0' || name[len] == L'.');
}

static HANDLE WINAPI HookOpenEventW(DWORD access, BOOL inherit, LPCWSTR name)
{
	if (name && wcscmp(name, OVR_HMD_CONNECTED_EVENT_NAME) == 0)
		return CreateEventW(nullptr, TRUE, TRUE, nullptr);
	return TrueOpenEventW(access, inherit, name);
}

static HMODULE WINAPI HookLoadLibraryW(LPCWSTR file)
{
	return TrueLoadLibraryW(IsOvrRuntime(file) ? g_selfPath : file);
}

static HMODULE WINAPI HookLoadLibraryExW(LPCWSTR file, HANDLE h, DWORD flags)
{
	return TrueLoadLibraryExW(IsOvrRuntime(file) ? g_selfPath : file, h, flags);
}

void AttachDetours()
{
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)TrueLoadLibraryW, HookLoadLibraryW);
	DetourAttach(&(PVOID&)TrueLoadLibraryExW, HookLoadLibraryExW);
	DetourAttach(&(PVOID&)TrueOpenEventW, HookOpenEventW);
	DetourTransactionCommit();
}

void DetachDetours()
{
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourDetach(&(PVOID&)TrueLoadLibraryW, HookLoadLibraryW);
	DetourDetach(&(PVOID&)TrueLoadLibraryExW, HookLoadLibraryExW);
	DetourDetach(&(PVOID&)TrueOpenEventW, HookOpenEventW);
	DetourTransactionCommit();
}

static void OpenLog()
{
	PWSTR roaming = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return;
	swprintf_s(g_logPath, L"%s\\Atrium\\logs", roaming);
	CoTaskMemFree(roaming);
	SHCreateDirectoryExW(nullptr, g_logPath, nullptr);
	wcscat_s(g_logPath, L"\\ovr.log");
	FILE* f = nullptr;
	if (_wfopen_s(&f, g_logPath, L"w") == 0 && f) fclose(f);

	WCHAR exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	AtriumOvrLog("Atrium OVR loaded into %ls", exe);
	AtriumOvrLog("from %ls", g_selfPath);
	char runtime[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("XR_RUNTIME_JSON", runtime, MAX_PATH);
	AtriumOvrLog("OpenXR runtime: %s", n ? runtime : "system default (ActiveRuntime)");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (DetourIsHelperProcess()) return TRUE;
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		InitializeCriticalSection(&g_logLock);
		GetModuleFileNameW(module, g_selfPath, MAX_PATH);
		swprintf_s(g_ovrName, L"LibOVRRT64_%d", OVR_MAJOR_VERSION);
		OpenLog();
		DetourRestoreAfterWith();
		AttachDetours();
		break;
	case DLL_PROCESS_DETACH:
		DetachDetours();
		break;
	}
	return TRUE;
}
