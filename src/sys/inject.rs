//! Starting a program with a DLL already loaded in it (Windows). Used for Atrium OVR: Oculus SDK
//! games only load a Meta-signed runtime, so Atrium OVR has to be in the process first, where its
//! LoadLibrary hook hands itself out when the game loads Meta's (signed) LibOVRRT64_1.dll.
//!
//! The process starts suspended, a remote thread runs LoadLibraryW(dll) in it, and only then does
//! the game's main thread start.
use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use windows_sys::Win32::Foundation::{CloseHandle, WAIT_OBJECT_0};
use windows_sys::Win32::System::Console::{GetStdHandle, STD_ERROR_HANDLE, STD_INPUT_HANDLE, STD_OUTPUT_HANDLE};
use windows_sys::Win32::System::Diagnostics::Debug::WriteProcessMemory;
use windows_sys::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
use windows_sys::Win32::System::Memory::{VirtualAllocEx, VirtualFreeEx, MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_READWRITE};
use windows_sys::Win32::System::Threading::{
    CreateProcessW, CreateRemoteThread, GetExitCodeThread, ResumeThread, TerminateProcess, WaitForSingleObject, CREATE_SUSPENDED, CREATE_UNICODE_ENVIRONMENT, STARTF_USESTDHANDLES,
    PROCESS_INFORMATION, STARTUPINFOW,
};

fn wide(s: &OsStr) -> Vec<u16> {
    s.encode_wide().chain(Some(0)).collect()
}

fn quote(arg: &str) -> String {
    if !arg.is_empty() && !arg.contains([' ', '\t', '"']) {
        return arg.to_string();
    }
    let mut out = String::from("\"");
    let mut slashes = 0;
    for c in arg.chars() {
        match c {
            '\\' => slashes += 1,
            '"' => {
                out.extend(std::iter::repeat_n('\\', slashes * 2 + 1));
                out.push('"');
                slashes = 0;
                continue;
            }
            _ => {}
        }
        if c != '\\' {
            out.extend(std::iter::repeat_n('\\', slashes));
            slashes = 0;
            out.push(c);
        }
    }
    out.extend(std::iter::repeat_n('\\', slashes * 2));
    out.push('"');
    out
}

/// Start `exe` with `args` in `cwd`, environment = this process's plus `env`, with `dll` loaded
/// before the program's own code runs. Returns the process id.
pub fn spawn_with_dll(exe: &Path, args: &[String], cwd: &Path, env: &[(&str, &OsStr)], dll: &Path) -> Result<u32, String> {
    let mut cmdline: Vec<u16> = wide(OsStr::new(&std::iter::once(quote(&exe.to_string_lossy())).chain(args.iter().map(|a| quote(a))).collect::<Vec<_>>().join(" ")));
    let exe_w = wide(exe.as_os_str());
    let cwd_w = wide(cwd.as_os_str());

    // Environment block: KEY=VALUE\0...\0\0, ours overriding inherited ones.
    let mut vars: Vec<(String, std::ffi::OsString)> = std::env::vars_os().map(|(k, v)| (k.to_string_lossy().into_owned(), v)).collect();
    for (k, v) in env {
        vars.retain(|(x, _)| !x.eq_ignore_ascii_case(k));
        vars.push((k.to_string(), v.to_os_string()));
    }
    let mut block: Vec<u16> = vec![];
    for (k, v) in &vars {
        block.extend(OsStr::new(k).encode_wide());
        block.push('=' as u16);
        block.extend(v.encode_wide());
        block.push(0);
    }
    block.push(0);

    let dll_w = wide(dll.as_os_str());
    unsafe {
        let mut si: STARTUPINFOW = std::mem::zeroed();
        si.cb = std::mem::size_of::<STARTUPINFOW>() as u32;
        // Pass our console handles on (atrium --translate from a terminal shows the game's output).
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        let mut pi: PROCESS_INFORMATION = std::mem::zeroed();
        if CreateProcessW(exe_w.as_ptr(), cmdline.as_mut_ptr(), std::ptr::null(), std::ptr::null(), 1, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, block.as_ptr() as _, cwd_w.as_ptr(), &si, &mut pi) == 0 {
            return Err(format!("Couldn't start {}: {}", exe.display(), std::io::Error::last_os_error()));
        }
        let fail = |what: &str| {
            let e = std::io::Error::last_os_error();
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            Err(format!("Couldn't load Atrium OVR into the game ({what}: {e})"))
        };

        let bytes = dll_w.len() * 2;
        let remote = VirtualAllocEx(pi.hProcess, std::ptr::null(), bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if remote.is_null() {
            return fail("memory");
        }
        if WriteProcessMemory(pi.hProcess, remote, dll_w.as_ptr() as _, bytes, std::ptr::null_mut()) == 0 {
            return fail("write");
        }
        // kernel32 is mapped at the same address in every process of a boot session.
        let k32 = GetModuleHandleW(wide(OsStr::new("kernel32.dll")).as_ptr());
        let Some(load) = GetProcAddress(k32, c"LoadLibraryW".as_ptr() as _) else { return fail("LoadLibraryW") };
        let thread = CreateRemoteThread(pi.hProcess, std::ptr::null(), 0, Some(std::mem::transmute::<unsafe extern "system" fn() -> isize, unsafe extern "system" fn(*mut core::ffi::c_void) -> u32>(load)), remote, 0, std::ptr::null_mut());
        if thread.is_null() {
            return fail("thread");
        }
        let waited = WaitForSingleObject(thread, 20_000);
        let mut loaded = 0u32;
        GetExitCodeThread(thread, &mut loaded);
        CloseHandle(thread);
        VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
        if waited != WAIT_OBJECT_0 {
            return fail("timed out");
        }
        // The exit code is the low half of the module address, which can be 0; Atrium OVR's own
        // log (logs\ovr.log) is the real check.
        if loaded == 0 {
            super::log(&format!("inject: LoadLibraryW exit code 0 for {}", dll.display()));
        }

        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Ok(pi.dwProcessId)
    }
}
