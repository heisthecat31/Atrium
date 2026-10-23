//! settings.json in Atrium's data folder. Written through a temp file so a crash can't truncate it.
use serde::{Deserialize, Serialize};
use std::collections::HashMap;

#[derive(Serialize, Deserialize, Default, Clone)]
#[serde(default)]
pub struct Settings {
    pub steam_account: Option<SteamAccount>,
    /// DPAPI-sealed (Windows) Steam Web API key, base64.
    pub steam_api_key: Option<String>,
    pub meta_accounts: Vec<MetaAccount>,
    pub local_games: Vec<LocalGame>,
    pub game_options: HashMap<String, GameOptions>,
    pub plays: HashMap<String, u64>,
    pub steam_vr_cache: HashMap<String, Option<(bool, bool)>>,
    pub meta_name_cache: HashMap<String, Option<String>>,
    pub vr_only: Option<bool>,
    /// What closing the window does: "tray" or "quit"; None = ask each time.
    pub close_action: Option<String>,
    /// Which eye Oculus SDK games show on the desktop: "right"; None = the game's own (left).
    pub mirror_eye: Option<String>,
    /// Where Meta games Atrium downloads go (one folder per game). None = ask on first download.
    pub download_dir: Option<String>,
    /// Meta games Atrium downloaded, by app id.
    pub meta_installs: HashMap<String, MetaInstall>,
    /// Mix & match: another headset with these controllers (services/mix.rs).
    pub mix: MixSettings,
    /// The in-VR dash's look and controls (services/dash.rs writes them to dash.json, which the
    /// dash and the Atrium Runtime read). None until set: then dash.json as the dash left it.
    pub dash: Option<DashSettings>,
    /// Meta runtime tweaks for wired Rifts (applied to OVRServer each time it starts).
    pub rift: RiftTweaks,
}

#[derive(Serialize, Deserialize, Clone, PartialEq)]
#[serde(default)]
pub struct DashSettings {
    /// Metres from the eyes to the panel's centre.
    pub distance: f32,
    /// Metres along the curve.
    pub width: f32,
    /// 0 flat .. 1 strongly curved.
    pub curve: f32,
    /// Colour: 0 Aurora, 1 Mint, 2 Ember, 3 Orchid, 4 Gold.
    pub accent: i32,
    /// Home surroundings: space (true) or plain.
    pub environment: bool,
    /// left_menu, x, y, a, b, left_stick, right_stick, oculus.
    pub open_button: String,
    /// "hold" or "press".
    pub open_mode: String,
    pub pause_game: bool,
    /// Pressing the key, games get the value (button ids as open_button, without "oculus").
    pub remap: std::collections::BTreeMap<String, String>,
}

impl Default for DashSettings {
    fn default() -> Self {
        Self {
            distance: 1.15,
            width: 1.70,
            curve: 0.55,
            accent: 0,
            environment: true,
            open_button: "left_menu".into(),
            open_mode: "hold".into(),
            pause_game: false,
            remap: Default::default(),
        }
    }
}

/// Oculus Tray Tool-style tweaks, sent to OVRServer through Meta's Debug Tool CLI.
#[derive(Serialize, Deserialize, Clone, PartialEq)]
#[serde(default)]
pub struct RiftTweaks {
    /// Pixels per display pixel (supersampling); 0 = the game's own.
    pub supersampling: f32,
    /// "auto", "off", "45", "30", "18".
    pub asw: String,
    /// FOV tangent multipliers (1 = full field of view).
    pub fov_x: f32,
    pub fov_y: f32,
    /// Meta's performance HUD: 0 off, 1 summary, 2 latency, 3 app render, 4 compositor, 5 ASW, 6 version.
    pub perf_hud: i32,
    pub adaptive_gpu: bool,
    pub mipmaps_all_layers: bool,
    pub fov_stencil: bool,
    pub touch_never_sleep: bool,
}

impl Default for RiftTweaks {
    fn default() -> Self {
        Self {
            supersampling: 0.0,
            asw: "auto".into(),
            fov_x: 1.0,
            fov_y: 1.0,
            perf_hud: 0,
            adaptive_gpu: false,
            mipmaps_all_layers: false,
            fov_stencil: true,
            touch_never_sleep: false,
        }
    }
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct MixSettings {
    pub enabled: bool,
    /// The controllers used instead of the headset's own: "cv1" (Rift CV1 Touches).
    pub controllers: String,
}

/// A Meta game Atrium downloaded: every version kept, one folder each, one of them played.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct MetaInstall {
    pub name: String,
    pub canonical: String,
    /// The version Play starts (a key of `versions`).
    pub active: String,
    pub versions: HashMap<String, MetaVersion>,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct MetaVersion {
    pub dir: String,
    pub launch_file: String,
    pub launch_args: String,
    pub version_code: i64,
    pub binary_id: String,
}

impl MetaInstall {
    pub fn current(&self) -> Option<&MetaVersion> {
        self.versions.get(&self.active)
    }

    /// Installed versions, newest first.
    pub fn sorted(&self) -> Vec<(&String, &MetaVersion)> {
        let mut v: Vec<_> = self.versions.iter().collect();
        v.sort_by(|a, b| b.1.version_code.cmp(&a.1.version_code));
        v
    }
}

#[derive(Serialize, Deserialize, Clone)]
pub struct SteamAccount {
    pub steam_id: String,
    pub name: String,
}

#[derive(Serialize, Deserialize, Clone)]
pub struct MetaAccount {
    pub id: String,
    pub name: String,
    /// sealed token, base64
    pub token: String,
}

#[derive(Serialize, Deserialize, Clone)]
pub struct LocalGame {
    pub id: String,
    pub name: String,
    pub exe: String,
    #[serde(default)]
    pub args: String,
}

#[derive(Serialize, Deserialize, Clone, Default, PartialEq)]
#[serde(default)]
pub struct GameOptions {
    /// "auto" | "steam" | "direct" | "translate"
    pub mode: String,
}

impl Settings {
    fn path() -> std::path::PathBuf {
        super::data_dir().join("settings.json")
    }
    pub fn load() -> Self {
        std::fs::read_to_string(Self::path()).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
    }
    pub fn save(&self) {
        let p = Self::path();
        let tmp = p.with_extension("json.tmp");
        if let Ok(t) = serde_json::to_string_pretty(self) {
            let ok = match std::fs::write(&tmp, &t).and_then(|_| std::fs::rename(&tmp, &p)) {
                Ok(()) => true,
                Err(e) => {
                    // Replace-by-rename can fail under MSIX file virtualisation; write in place.
                    super::log(&format!("settings save via rename failed: {e}"));
                    let _ = std::fs::remove_file(&tmp);
                    std::fs::write(&p, &t).map_err(|e| super::log(&format!("settings save failed: {e}"))).is_ok()
                }
            };
            if ok {
                #[cfg(unix)]
                {
                    use std::os::unix::fs::PermissionsExt;
                    let _ = std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o600));
                }
            }
        }
    }
}
