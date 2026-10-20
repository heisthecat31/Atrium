//! Starting games. Steam games go through Steam (overlay, cloud saves, DRM); Meta and local games
//! start directly from their folder, or through the translation layer (Oculus SDK → OpenXR) when
//! the game's mode says so.
use crate::services::library::{Game, Source};
use crate::sys::{self, settings::Settings};
use std::path::{Path, PathBuf};
use std::process::Command;

/// How a game will start, resolved from its options ("auto" picks per source).
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
            Mode::Translate => "Translated to OpenXR",
        }
    }
}

/// The translation launcher (RiftLift-compatible), from settings or next to atrium.
pub fn translator(settings: &Settings) -> Option<PathBuf> {
    let name = "RiftLiftLauncher.exe";
    settings.translation_dir.as_ref().map(|d| Path::new(d).join(name)).filter(|p| p.exists()).or_else(|| sys::native_tool(name))
}

pub fn mode(game: &Game, settings: &Settings) -> Mode {
    let chosen = settings.game_options.get(&game.id).map(|o| o.mode.as_str()).unwrap_or("auto");
    match (chosen, game.source) {
        ("steam", Source::Steam) => Mode::Steam,
        ("direct", _) if game.exe.is_some() => Mode::Direct,
        ("translate", _) if game.exe.is_some() && translator(settings).is_some() => Mode::Translate,
        (_, Source::Steam) => Mode::Steam,
        _ => Mode::Direct,
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

pub fn launch(game: &Game, settings: &mut Settings) -> Result<(), String> {
    if !game.installed {
        let url = get_url(game);
        if url.is_empty() {
            return Err(format!("{} isn't on this PC any more", game.name));
        }
        return open::that(url).map_err(|e| e.to_string());
    }
    match mode(game, settings) {
        Mode::Steam => open::that(format!("steam://rungameid/{}", game.app_id)).map_err(|e| format!("Couldn't start Steam: {e}"))?,
        Mode::Direct => {
            let exe = game.exe.as_ref().ok_or("No executable known for this game")?;
            let dir = exe.parent().map(Path::to_path_buf).or_else(|| game.install_dir.clone()).unwrap_or_default();
            Command::new(exe).args(split_args(&game.args)).current_dir(dir).spawn().map_err(|e| format!("Couldn't start {}: {e}", game.name))?;
        }
        Mode::Translate => {
            let exe = game.exe.as_ref().ok_or("No executable known for this game")?;
            let tr = translator(settings).ok_or("The translation layer isn't installed")?;
            let dir = exe.parent().map(Path::to_path_buf).unwrap_or_default();
            let key = if game.canonical.is_empty() { game.app_id.clone() } else { game.canonical.clone() };
            Command::new(&tr)
                .arg("/openxr")
                .args(["/app", &key])
                .arg("/cwd")
                .arg(&dir)
                .arg(exe)
                .args(split_args(&game.args))
                .current_dir(tr.parent().unwrap_or(Path::new(".")))
                .spawn()
                .map_err(|e| format!("Couldn't start the translation layer: {e}"))?;
        }
    }
    settings.plays.insert(game.id.clone(), sys::now_ms());
    settings.save();
    sys::log(&format!("launch {} ({:?})", game.id, mode(game, settings)));
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
