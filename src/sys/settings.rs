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
