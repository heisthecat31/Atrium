//! Atrium's in-VR dashboard (native/atrium_dash, atrium-dash.exe).
//!
//! Started with Atrium and ended with it. It draws from the same state snapshot as Atrium's window
//! (one JSON line on its stdin after every change, plus `running` and `home`) and sends back the
//! same commands the window sends (one JSON line each on its stdout). Inside games the Atrium
//! Runtime shows it (hold the left menu button); with no game running it runs its own OpenXR
//! session on the Atrium Runtime ("home"), so the headset shows the dash as soon as it's on.
use crate::services::runtime;
use crate::sys;
use crate::sys::settings::DashSettings;
use std::io::{BufRead, BufReader, Write};
use std::process::{ChildStdin, Command, Stdio};
use std::sync::Mutex;

static STDIN: Mutex<Option<ChildStdin>> = Mutex::new(None);

pub fn available() -> bool {
    sys::native_tool("atrium-dash.exe").is_some()
}

/// Start atrium-dash; each line it prints goes to `on_command`. If it ends (a crash, or swapped
/// for a new build), it's started again a few seconds later.
pub fn start(on_command: impl Fn(String) + Send + Sync + 'static) -> Result<(), String> {
    let on_command = std::sync::Arc::new(on_command);
    let first = start_once(on_command.clone());
    std::thread::spawn(move || loop {
        std::thread::sleep(std::time::Duration::from_secs(3));
        if STDIN.lock().unwrap_or_else(|e| e.into_inner()).is_none() {
            let c = on_command.clone();
            match start_once(c) {
                Ok(()) => sys::log("dash: started again"),
                Err(e) => sys::log(&e),
            }
            std::thread::sleep(std::time::Duration::from_secs(10));
        }
    });
    first
}

fn start_once(on_command: std::sync::Arc<dyn Fn(String) + Send + Sync>) -> Result<(), String> {
    let exe = sys::native_tool("atrium-dash.exe").ok_or("atrium-dash isn't built (native\\atrium_dash)")?;
    let mut cmd = Command::new(&exe);
    cmd.current_dir(exe.parent().unwrap_or(std::path::Path::new("."))).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null());
    // Home runs on the Atrium Runtime whatever the system's OpenXR runtime is.
    if let Some(m) = runtime::manifest() {
        cmd.env("XR_RUNTIME_JSON", m);
    }
    sys::no_window(&mut cmd);
    let mut child = cmd.spawn().map_err(|e| format!("Couldn't start the dash: {e}"))?;
    sys::end_with_atrium(&child);
    let out = child.stdout.take().ok_or("no dash output")?;
    *STDIN.lock().unwrap_or_else(|e| e.into_inner()) = child.stdin.take();
    std::thread::spawn(move || {
        for line in BufReader::new(out).lines().map_while(Result::ok) {
            if !line.trim().is_empty() {
                on_command(line);
            }
        }
        sys::log("dash: atrium-dash ended");
        *STDIN.lock().unwrap_or_else(|e| e.into_inner()) = None;
        let _ = child.wait();
    });
    sys::log("dash: started atrium-dash");
    Ok(())
}

/// Send the dash the current state (a full snapshot, as the window gets).
pub fn send(state: &serde_json::Value) {
    let mut guard = STDIN.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(stdin) = guard.as_mut() {
        let line = format!("{state}\n");
        if stdin.write_all(line.as_bytes()).and_then(|_| stdin.flush()).is_err() {
            *guard = None;
        }
    }
}

/// Ask the dash to start its home session again (Meta's runtime now hands it the headset).
pub fn restart_home() {
    send(&serde_json::json!({ "type": "restartHome" }));
}

/// The dash's settings as the dash and the Atrium Runtime read them (%APPDATA%\Atrium\dash.json).
pub fn write_settings(d: &DashSettings) {
    let remap = d.remap.iter().map(|(k, v)| format!("{k}={v}")).collect::<Vec<_>>().join(",");
    let text = format!(
        "{{ \"distance\": {:.3}, \"width\": {:.3}, \"curve\": {:.3}, \"accent\": {}, \"environment\": {},\n  \"openButton\": \"{}\", \"openMode\": \"{}\", \"pauseGame\": \"{}\", \"remap\": \"{}\" }}\n",
        d.distance,
        d.width,
        d.curve,
        d.accent,
        d.environment,
        d.open_button,
        if d.open_mode == "press" { "press" } else { "hold" },
        if d.pause_game { "on" } else { "off" },
        remap
    );
    let _ = std::fs::write(sys::data_dir().join("dash.json"), text);
}

/// The settings the dash last saved itself (before Atrium owned them), if any.
pub fn read_settings_file() -> Option<DashSettings> {
    let j = sys::read_json(&sys::data_dir().join("dash.json"))?;
    let mut d = DashSettings::default();
    let f = |k: &str, v: f32| j[k].as_f64().map(|x| x as f32).unwrap_or(v);
    d.distance = f("distance", d.distance);
    d.width = f("width", d.width);
    d.curve = f("curve", d.curve);
    d.accent = j["accent"].as_i64().unwrap_or(0) as i32;
    d.environment = j["environment"].as_bool().unwrap_or(true);
    d.open_button = j["openButton"].as_str().unwrap_or("left_menu").to_string();
    d.open_mode = j["openMode"].as_str().unwrap_or("hold").to_string();
    d.pause_game = j["pauseGame"].as_str() == Some("on");
    for pair in j["remap"].as_str().unwrap_or("").split(',') {
        if let Some((k, v)) = pair.split_once('=') {
            d.remap.insert(k.to_string(), v.to_string());
        }
    }
    Some(d)
}
