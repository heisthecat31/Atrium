//! Standalone headsets (Quest, Pico) and the Atrium headset app on them (native/atrium_client,
//! package vr.atrium.client). Atrium talks to them over adb: a headset in developer mode on USB
//! shows up here, and Atrium can install or update its app and open it. The picture itself goes
//! over Wi-Fi or USB through Atrium's streaming server, not through this.
use crate::sys;
use serde::Serialize;
use std::path::PathBuf;

pub const PACKAGE: &str = "vr.atrium.client";

#[derive(Clone, Debug, Default, Serialize)]
pub struct Standalone {
    pub serial: String,
    pub model: String,
    /// "device" when usable; "unauthorized" until the user allows USB debugging in the headset
    pub state: String,
    /// the Atrium app's versionName on the headset, if installed
    pub atrium_version: Option<String>,
}

/// adb: next to atrium, on PATH, or in an Android SDK.
pub fn adb() -> Option<PathBuf> {
    let exe = if cfg!(windows) { "adb.exe" } else { "adb" };
    let mut c = vec![sys::exe_dir().join(exe), sys::exe_dir().join("platform-tools").join(exe)];
    if let Some(path) = std::env::var_os("PATH") {
        c.extend(std::env::split_paths(&path).map(|d| d.join(exe)));
    }
    for var in ["ANDROID_HOME", "ANDROID_SDK_ROOT"] {
        if let Some(d) = std::env::var_os(var) {
            c.push(PathBuf::from(d).join("platform-tools").join(exe));
        }
    }
    if let Some(d) = dirs::data_local_dir() {
        c.push(d.join("Android").join("Sdk").join("platform-tools").join(exe));
    }
    c.into_iter().find(|p| p.exists())
}

fn run(adb: &PathBuf, serial: &str, args: &[&str]) -> String {
    let mut all = vec!["-s", serial];
    all.extend_from_slice(args);
    sys::output(adb, &all)
}

/// Headsets adb can see. Empty when adb isn't available.
pub fn detect() -> Vec<Standalone> {
    let Some(adb) = adb() else { return vec![] };
    let mut out = vec![];
    for line in sys::output(&adb, &["devices", "-l"]).lines().skip(1) {
        let mut parts = line.split_whitespace();
        let (Some(serial), Some(state)) = (parts.next(), parts.next()) else { continue };
        let mut h = Standalone { serial: serial.into(), state: state.into(), ..Default::default() };
        if state == "device" {
            h.model = run(&adb, serial, &["shell", "getprop", "ro.product.model"]).trim().to_string();
            let dump = run(&adb, serial, &["shell", "dumpsys", "package", PACKAGE]);
            h.atrium_version = dump.lines().find_map(|l| l.trim().strip_prefix("versionName=").map(str::to_string));
        } else {
            h.model = parts.find_map(|p| p.strip_prefix("model:")).unwrap_or("Headset").replace('_', " ");
        }
        out.push(h);
    }
    out
}

/// The Atrium headset app built from this checkout (native/out/atrium-client.apk) or shipped
/// next to atrium.
pub fn apk() -> Option<PathBuf> {
    sys::native_tool("atrium-client.apk").or_else(|| {
        // native_tool looks in native/out/linux on Linux; the APK is platform-neutral.
        let mut d = sys::exe_dir();
        for _ in 0..4 {
            let p = d.join("native").join("out").join("atrium-client.apk");
            if p.exists() {
                return Some(p);
            }
            if !d.pop() {
                break;
            }
        }
        None
    })
}

/// Install (or update) the Atrium app on a headset.
pub fn install(serial: &str) -> Result<(), String> {
    let adb = adb().ok_or("adb isn't available (install Android platform-tools)")?;
    let apk = apk().ok_or("The Atrium headset app isn't built (native\\atrium_client\\build.ps1)")?;
    let out = run(&adb, serial, &["install", "-r", "-g", &apk.to_string_lossy()]);
    sys::log(&format!("standalone {serial}: install: {}", out.trim()));
    if out.contains("Success") {
        Ok(())
    } else {
        Err(format!("Installing on the headset failed: {}", out.lines().last().unwrap_or("no answer from adb")))
    }
}

/// Open the Atrium app on a headset.
pub fn open(serial: &str) -> Result<(), String> {
    let adb = adb().ok_or("adb isn't available")?;
    let out = run(&adb, serial, &["shell", "monkey", "-p", PACKAGE, "-c", "android.intent.category.LAUNCHER", "1"]);
    if out.contains("No activities found") || out.contains("monkey aborted") {
        return Err("The Atrium app isn't installed on this headset".into());
    }
    Ok(())
}
