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

/// Close Meta's extras that are running. Returns the ones closed.
pub fn strip() -> Vec<&'static str> {
    let procs = sys::processes();
    let mut closed = vec![];
    for &name in EXTRAS {
        if procs.contains(name) {
            sys::output("taskkill.exe", &["/f", "/t", "/im", name]);
            closed.push(name);
        }
    }
    if !closed.is_empty() {
        sys::log(&format!("meta runtime: closed {}", closed.join(", ")));
    }
    closed
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
