//! Starting games. Steam games go through Steam (overlay, cloud saves, DRM). Meta and local games
//! start from their folder: directly, or through Atrium OVR, Atrium's translation layer for
//! Oculus SDK games (native/atrium_ovr), which answers their LibOVR calls over OpenXR.
use crate::services::library::{Game, Source};
use crate::services::runtime::{self, RuntimeState};
use crate::sys::{self, settings::Settings};
use std::path::{Path, PathBuf};
use std::process::Command;

/// How a game will start, resolved from its options ("auto" picks per game).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Steam,
    Direct,
    Translate,
}

impl Mode {
    pub fn label(self) -> &'static str {
        match self {
            Mode::Steam => "Through Steam",
            Mode::Direct => "Directly",
            Mode::Translate => "Through Atrium OVR (OpenXR)",
        }
    }
}

/// Atrium OVR's folder (the one holding LibOVRRT64_1.dll): next to atrium, native/out/ovr, or
/// the CMake build output in a source checkout.
pub fn ovr_dir() -> Option<PathBuf> {
    const DLL: &str = "LibOVRRT64_1.dll";
    sys::native_tool(&format!("ovr/{DLL}"))
        .or_else(|| {
            let mut d = sys::exe_dir();
            for _ in 0..4 {
                let p = d.join("native/atrium_ovr/build/out/ovr").join(DLL);
                if p.exists() {
                    return Some(p);
                }
                if !d.pop() {
                    break;
                }
            }
            None
        })
        .and_then(|p| p.parent().map(Path::to_path_buf))
}

/// The OpenXR runtime games started by Atrium use: the Atrium Runtime when it's built (it drives
/// the headset through the backend in runtime.json), else the vendor backend Atrium picked.
pub fn xr_runtime(rt: Option<&RuntimeState>) -> Option<PathBuf> {
    runtime::manifest().or_else(|| rt.and_then(|r| r.chosen.as_ref()).map(|b| b.manifest.clone()))
}

pub fn mode(game: &Game, settings: &Settings, _rt: Option<&RuntimeState>) -> Mode {
    let chosen = settings.game_options.get(&game.id).map(|o| o.mode.as_str()).unwrap_or("auto");
    let can_translate = game.exe.is_some() && ovr_dir().is_some();
    match (chosen, game.source) {
        ("steam", Source::Steam) => Mode::Steam,
        ("direct", _) if game.exe.is_some() => Mode::Direct,
        ("translate", _) if can_translate => Mode::Translate,
        (_, Source::Steam) => Mode::Steam,
        // Oculus SDK games go through Atrium OVR whenever it's built, so they only see Atrium.
        _ if game.vr_api == Some("oculus") && can_translate => Mode::Translate,
        _ => Mode::Direct,
    }
}

/// Start an Oculus SDK program through Atrium OVR, on the OpenXR runtime `xr` (None = the
/// system's active one).
///
/// Newer Oculus SDK games only load a Meta-signed LibOVRRT64_1.dll, so Atrium OVR is injected
/// before the game starts; when the game then loads Meta's signed runtime, Atrium OVR's
/// LoadLibrary hook returns itself. Older games (no signature check) also find it through
/// LIBOVR_DLL_DIR. Its folder must not go first on PATH: the SDK's loader would find the
/// unsigned copy there and stop looking.
pub fn translated(exe: &Path, args: &[String], xr: Option<&Path>) -> Result<u32, String> {
    let ovr = ovr_dir().ok_or("Atrium OVR isn't built (native\\build.bat)")?;
    let dir = exe.parent().map(Path::to_path_buf).unwrap_or_default();
    let mut env: Vec<(&str, &std::ffi::OsStr)> = vec![("LIBOVR_DLL_DIR", ovr.as_os_str())];
    if let Some(m) = xr {
        env.push(("XR_RUNTIME_JSON", m.as_os_str()));
    }
    #[cfg(windows)]
    {
        sys::inject::spawn_with_dll(exe, args, &dir, &env, &ovr.join("LibOVRRT64_1.dll"))
    }
    #[cfg(not(windows))]
    {
        // Linux: Windows games run under Proton, which passes the environment through.
        Command::new(exe).args(args).current_dir(dir).envs(env).spawn().map(|c| c.id()).map_err(|e| e.to_string())
    }
}

/// What the main button does for a game that can't start yet.
pub fn get_url(game: &Game) -> String {
    match game.source {
        Source::Steam => format!("steam://install/{}", game.app_id),
        Source::Meta => crate::services::meta::store_url(&game.app_id),
        Source::Local => String::new(),
    }
}

pub fn launch(game: &Game, settings: &mut Settings, rt: Option<&RuntimeState>) -> Result<(), String> {
    if !game.installed {
        let url = get_url(game);
        if url.is_empty() {
            return Err(format!("{} isn't on this PC any more", game.name));
        }
        return open::that(url).map_err(|e| e.to_string());
    }
    // The Atrium Runtime reads its backend when the game starts; make sure it's current.
    if let Some(st) = rt {
        runtime::write_backend(st);
    }
    let mode = mode(game, settings, rt);
    match mode {
        Mode::Steam => open::that(format!("steam://rungameid/{}", game.app_id)).map_err(|e| format!("Couldn't start Steam: {e}"))?,
        Mode::Direct => {
            let exe = game.exe.as_ref().ok_or("No executable known for this game")?;
            let dir = exe.parent().map(Path::to_path_buf).or_else(|| game.install_dir.clone()).unwrap_or_default();
            let mut cmd = Command::new(exe);
            cmd.args(split_args(&game.args)).current_dir(dir);
            // OpenXR games started here run on the Atrium Runtime even when it isn't the system's.
            if game.vr_api == Some("openxr") {
                if let Some(m) = runtime::manifest() {
                    cmd.env("XR_RUNTIME_JSON", m);
                }
            }
            cmd.spawn().map_err(|e| format!("Couldn't start {}: {e}", game.name))?;
        }
        Mode::Translate => {
            let exe = game.exe.as_ref().ok_or("No executable known for this game")?;
            translated(exe, &split_args(&game.args), xr_runtime(rt).as_deref())?;
        }
    }
    settings.plays.insert(game.id.clone(), sys::now_ms());
    settings.save();
    sys::log(&format!("launch {} ({mode:?})", game.id));
    Ok(())
}

/// Splits a command line the way most games expect: spaces separate, double quotes group.
fn split_args(s: &str) -> Vec<String> {
    let mut out = vec![];
    let mut cur = String::new();
    let mut quoted = false;
    let mut any = false;
    for c in s.chars() {
        match c {
            '"' => {
                quoted = !quoted;
                any = true;
            }
            c if c.is_whitespace() && !quoted => {
                if any || !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                }
                any = false;
            }
            c => cur.push(c),
        }
    }
    if any || !cur.is_empty() {
        out.push(cur);
    }
    out
}
