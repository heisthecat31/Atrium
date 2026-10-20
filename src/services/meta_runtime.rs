//! Meta's runtime, cut down to the part Atrium uses: OVRServer, which drives the Rift and puts
//! frames on its panel (NVIDIA only hands the panel to Meta's direct mode). Everything else Meta
//! starts with it is closed: the Meta Horizon Link app, Dash (Meta's in-headset home, kept off
//! with Meta's own PreventDashLaunch switch) and the platform runtime (store sign-in and
//! entitlements; Atrium will answer those itself). RemoteDesktopCompanion stays: OVRServer
//! treats it as a core process and restarts it within a second of closing (tested 2026-10-20).
use crate::sys;

/// What Meta starts besides OVRServer, closed whenever they show up.
const EXTRAS: &[&str] = &["client.exe", "oculusdash.exe", "oculus-platform-runtime.exe"];

const CONFIG: &str = "HKLM\\SOFTWARE\\Wow6432Node\\Oculus VR, LLC\\Oculus\\Config";

/// Meta's runtime is installed on this PC.
pub fn installed() -> bool {
    #[cfg(windows)]
    return sys::reg::string("HKLM\\SOFTWARE\\Wow6432Node\\Oculus VR, LLC\\Oculus", "Base").is_some();
    #[cfg(not(windows))]
    false
}

pub fn running() -> bool {
    sys::processes().contains("ovrserver_x64.exe")
}

/// Close Meta's extras that are running, if OVRServer is. Returns the ones closed.
#[cfg(windows)]
pub fn strip() -> Vec<&'static str> {
    use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::System::Diagnostics::ToolHelp::{CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS};
    use windows_sys::Win32::System::Threading::{OpenProcess, TerminateProcess, PROCESS_TERMINATE};
    let mut found: Vec<(u32, &'static str)> = vec![];
    let mut server = false;
    unsafe {
        let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snap == INVALID_HANDLE_VALUE {
            return vec![];
        }
        let mut e: PROCESSENTRY32W = std::mem::zeroed();
        e.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        let mut ok = Process32FirstW(snap, &mut e) != 0;
        while ok {
            let len = e.szExeFile.iter().position(|&c| c == 0).unwrap_or(e.szExeFile.len());
            let name = String::from_utf16_lossy(&e.szExeFile[..len]).to_lowercase();
            if name == "ovrserver_x64.exe" {
                server = true;
            } else if let Some(&x) = EXTRAS.iter().find(|&&x| x == name) {
                found.push((e.th32ProcessID, x));
            }
            ok = Process32NextW(snap, &mut e) != 0;
        }
        CloseHandle(snap);
    }
    let mut closed = vec![];
    if !server {
        return closed;
    }
    for (pid, name) in found {
        unsafe {
            let h = OpenProcess(PROCESS_TERMINATE, 0, pid);
            if !h.is_null() {
                if TerminateProcess(h, 1) != 0 && !closed.contains(&name) {
                    closed.push(name);
                }
                CloseHandle(h);
            }
        }
    }
    if !closed.is_empty() {
        sys::log(&format!("meta runtime: closed {}", closed.join(", ")));
    }
    closed
}

#[cfg(not(windows))]
pub fn strip() -> Vec<&'static str> {
    vec![]
}

/// Meta's runtime relaunches its app each time a VR game connects; close it as soon as it
/// appears (a snapshot of the process list costs well under a millisecond).
pub fn watch() {
    std::thread::spawn(|| loop {
        strip();
        std::thread::sleep(std::time::Duration::from_millis(200));
    });
}

/// Dash is off and the service is running: nothing needs admin.
pub fn ready() -> bool {
    #[cfg(windows)]
    return sys::reg::dword(CONFIG, "PreventDashLaunch") == Some(1) && running();
    #[cfg(not(windows))]
    false
}

/// One admin step (UAC): turn Dash off with Meta's switch and start Meta's service if it's
/// stopped. Skipped when both are already so.
pub fn prepare() -> Result<(), String> {
    #[cfg(windows)]
    {
        if !installed() {
            return Err("Meta's runtime isn't installed".into());
        }
        if ready() {
            return Ok(());
        }
        let inner = format!(
            "reg add '{CONFIG}' /v PreventDashLaunch /t REG_DWORD /d 1 /f; if ((Get-Service OVRService).Status -ne 'Running') {{ Start-Service OVRService }}"
        );
        let ps = format!(
            "Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '-NoProfile','-Command','{}'",
            inner.replace('\'', "''")
        );
        let mut cmd = std::process::Command::new("powershell.exe");
        cmd.args(["-NoProfile", "-Command", &ps]);
        sys::no_window(&mut cmd);
        let st = cmd.status().map_err(|e| e.to_string())?;
        if !st.success() {
            return Err("Windows didn't allow starting Meta's runtime".into());
        }
        // OVRServer comes up a few seconds after the service.
        for _ in 0..20 {
            if running() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(500));
        }
        sys::log("meta runtime: prepared (Dash off, service running)");
        Ok(())
    }
    #[cfg(not(windows))]
    Err("Meta's runtime is Windows only".into())
}
