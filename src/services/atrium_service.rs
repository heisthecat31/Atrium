//! The Atrium Runtime's headset service on Linux (native/atrium_xr_linux: Atrium's build of
//! Monado). It owns the headset: drivers, tracking and the compositor that takes the headset's
//! display directly. Games load libopenxr_atrium.so, which talks to it over a socket
//! ($XDG_RUNTIME_DIR/atrium_comp_ipc). Atrium runs it as a child while a headset is connected;
//! it ends with Atrium (parent-death signal). Windows has no such service: there the Atrium
//! Runtime forwards to a vendor backend (see runtime.rs, meta_runtime.rs).
#[cfg(target_os = "linux")]
use crate::sys;

#[cfg(target_os = "linux")]
static CHILD: std::sync::Mutex<Option<std::process::Child>> = std::sync::Mutex::new(None);

/// The service is built (native/out/linux or next to atrium).
pub fn available() -> bool {
    #[cfg(target_os = "linux")]
    return sys::native_tool("atrium-service").is_some();
    #[cfg(not(target_os = "linux"))]
    false
}

/// Atrium's service is running (started by this Atrium and still alive).
pub fn running() -> bool {
    #[cfg(target_os = "linux")]
    {
        let mut c = CHILD.lock().unwrap_or_else(|e| e.into_inner());
        return match c.as_mut().map(|ch| ch.try_wait()) {
            Some(Ok(None)) => true,
            Some(_) => {
                *c = None;
                false
            }
            None => false,
        };
    }
    #[cfg(not(target_os = "linux"))]
    false
}

/// Start the service if it isn't running.
pub fn start() -> Result<(), String> {
    #[cfg(target_os = "linux")]
    {
        use std::os::unix::process::CommandExt;
        if running() {
            return Ok(());
        }
        let exe = sys::native_tool("atrium-service").ok_or("The Atrium Runtime isn't built (native/atrium_xr_linux/build.sh)")?;
        let log = std::fs::File::create(sys::data_dir().join("service.log")).map_err(|e| e.to_string())?;
        let mut cmd = std::process::Command::new(&exe);
        cmd.current_dir(exe.parent().unwrap_or(std::path::Path::new("."))).stdin(std::process::Stdio::null()).stdout(log.try_clone().map_err(|e| e.to_string())?).stderr(log);
        // End with Atrium, however Atrium ends.
        unsafe {
            cmd.pre_exec(|| {
                libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM);
                Ok(())
            });
        }
        let child = cmd.spawn().map_err(|e| format!("Couldn't start the Atrium service: {e}"))?;
        sys::log(&format!("atrium service: started (pid {})", child.id()));
        *CHILD.lock().unwrap_or_else(|e| e.into_inner()) = Some(child);
        Ok(())
    }
    #[cfg(not(target_os = "linux"))]
    Err("The Atrium service is Linux only".into())
}

/// Games can start: the service's socket exists.
pub fn ready() -> bool {
    #[cfg(target_os = "linux")]
    {
        let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
        return running() && std::path::Path::new(&dir).join("atrium_comp_ipc").exists();
    }
    #[cfg(not(target_os = "linux"))]
    false
}

/// Start the service and wait until it takes games.
pub fn prepare() -> Result<(), String> {
    start()?;
    for _ in 0..60 {
        if ready() {
            return Ok(());
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    Err("The Atrium service didn't start. See service.log in Atrium's folder".into())
}
