//! Platform access shared by the services: processes, the Windows registry, Valve's KeyValues
//! text format, JSON files, the settings file, and sealed secrets.
pub mod autostart;
pub mod secret;
pub mod settings;
pub mod vdf;
#[cfg(windows)]
pub mod inject;
#[cfg(windows)]
pub mod reg;

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// Run a program, wait, return stdout ("" on failure). No console window on Windows.
pub fn output(program: impl AsRef<std::ffi::OsStr>, args: &[&str]) -> String {
    let mut cmd = Command::new(program);
    cmd.args(args).stdin(Stdio::null()).stderr(Stdio::null());
    no_window(&mut cmd);
    cmd.output().map(|o| String::from_utf8_lossy(&o.stdout).into_owned()).unwrap_or_default()
}

/// Close a child process when Atrium ends, however it ends (Windows: a kill-on-close job that
/// lives as long as Atrium).
#[cfg(windows)]
pub fn end_with_atrium(child: &std::process::Child) {
    use std::os::windows::io::AsRawHandle;
    use std::sync::OnceLock;
    use windows_sys::Win32::System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    };
    static JOB: OnceLock<usize> = OnceLock::new();
    let job = *JOB.get_or_init(|| unsafe {
        let j = CreateJobObjectW(std::ptr::null(), std::ptr::null());
        if !j.is_null() {
            let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info as *const _ as *const _, std::mem::size_of_val(&info) as u32);
        }
        j as usize
    });
    if job != 0 {
        unsafe { AssignProcessToJobObject(job as _, child.as_raw_handle() as _) };
    }
}

/// Run a program as administrator (Windows asks with UAC) and wait. Ok if it ran and exited 0.
#[cfg(windows)]
pub fn run_elevated(program: &Path, args: &str) -> Result<(), String> {
    let ps = format!(
        "$p = Start-Process -FilePath '{}' -ArgumentList '{}' -Verb RunAs -Wait -WindowStyle Hidden -PassThru; exit $p.ExitCode",
        program.display().to_string().replace('\'', "''"),
        args.replace('\'', "''")
    );
    let mut cmd = Command::new("powershell.exe");
    cmd.args(["-NoProfile", "-Command", &ps]);
    no_window(&mut cmd);
    let st = cmd.status().map_err(|e| e.to_string())?;
    if st.success() { Ok(()) } else { Err("Windows didn't allow it, or it didn't finish".into()) }
}

pub fn no_window(cmd: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    #[cfg(not(windows))]
    let _ = cmd;
}

/// Names of running processes, lower-case ("ovrserver_x64.exe", "vrserver", ...).
pub fn processes() -> std::collections::HashSet<String> {
    #[cfg(windows)]
    let names: Vec<String> = output("tasklist.exe", &["/fo", "csv", "/nh"])
        .lines()
        .filter_map(|l| l.split("\",\"").next())
        .map(|s| s.trim_matches('"').to_lowercase())
        .collect();
    #[cfg(not(windows))]
    let names: Vec<String> = output("ps", &["-eo", "comm"]).lines().map(|s| s.trim().to_lowercase()).collect();
    names.into_iter().filter(|s| !s.is_empty()).collect()
}

pub fn read_json(path: &Path) -> Option<serde_json::Value> {
    let text = std::fs::read_to_string(path).ok()?;
    serde_json::from_str(text.trim_start_matches('\u{feff}')).ok()
}

pub fn exists(p: impl AsRef<Path>) -> bool {
    p.as_ref().exists()
}

pub fn home() -> PathBuf {
    dirs::home_dir().unwrap_or_default()
}

/// Atrium's own folder: %APPDATA%\Atrium, ~/.config/atrium.
pub fn data_dir() -> PathBuf {
    let d = dirs::config_dir().unwrap_or_else(home).join(if cfg!(windows) { "Atrium" } else { "atrium" });
    let _ = std::fs::create_dir_all(&d);
    d
}

/// Folder the atrium executable lives in (native tools ship next to it).
pub fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default()
}

/// A native helper (ovrprobe.exe, SteamFrameSim.exe, atrium-service): next to atrium, or in
/// native/out (native/out/linux on Linux) when run from a source checkout.
pub fn native_tool(name: &str) -> Option<PathBuf> {
    let mut c = vec![exe_dir().join(name)];
    let mut d = exe_dir();
    for _ in 0..4 {
        let out = d.join("native").join("out");
        c.push(if cfg!(windows) { out.join(name) } else { out.join("linux").join(name) });
        if !d.pop() {
            break;
        }
    }
    c.into_iter().find(|p| p.exists())
}

pub fn log(msg: &str) {
    use std::io::Write;
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(data_dir().join("atrium.log")) {
        let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        let _ = writeln!(f, "{t} {msg}");
    }
}

pub fn now_ms() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}
