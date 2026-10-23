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
