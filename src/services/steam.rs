//! Steam: where it is, what's installed, what the signed-in account owns, which apps are VR.
//! Installed games come from Steam's own files with no sign-in. Owned games need the account:
//! a Steam Web API key (GetOwnedGames) for the Steam ID that signed in through OpenID.
use crate::services::library::{Art, Game, Source};
use crate::sys::{self, vdf};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

/// Tools, runtimes and redistributables.
const NOT_GAMES: &[&str] = &["228980", "250820", "1070560", "1391110", "1628350", "1493710", "2180100", "961940", "1054830", "1113280", "1245040", "1420170", "1580130", "1887720", "2348590", "2805730", "323910", "1826330"];
const VR_CATEGORIES: &[i64] = &[31, 53, 54, 401, 402];

pub fn root() -> Option<PathBuf> {
    #[cfg(windows)]
    {
        if let Some(p) = sys::reg::string("HKCU\\Software\\Valve\\Steam", "SteamPath").map(PathBuf::from).filter(|p| p.exists()) {
            return Some(p);
        }
        return sys::reg::string("HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath").map(PathBuf::from).filter(|p| p.exists());
    }
    #[cfg(not(windows))]
    {
        let h = sys::home();
        [h.join(".steam/steam"), h.join(".local/share/Steam"), h.join(".var/app/com.valvesoftware.Steam/.local/share/Steam")].into_iter().find(|p| p.exists())
    }
}

pub fn libraries(root: &Path) -> Vec<PathBuf> {
    let mut libs: Vec<PathBuf> = vec![root.to_path_buf()];
    let key = |p: &Path| if cfg!(windows) { p.to_string_lossy().to_lowercase().replace('/', "\\") } else { p.to_string_lossy().into_owned() };
    for f in [root.join("steamapps/libraryfolders.vdf"), root.join("config/libraryfolders.vdf")] {
        let Ok(t) = std::fs::read_to_string(&f) else { continue };
        let kv = vdf::parse(&t);
        if let Some(lf) = kv.get("libraryfolders") {
            for (_, v) in lf.entries() {
                if let Some(p) = v.str("path").map(PathBuf::from).filter(|p| p.exists()) {
                    if !libs.iter().any(|l| key(l) == key(&p)) {
                        libs.push(p);
                    }
                }
            }
        }
    }
    libs
}

/// The account signed in to the Steam client on this PC.
pub fn local_user(root: &Path) -> Option<(String, String)> {
    let t = std::fs::read_to_string(root.join("config/loginusers.vdf")).ok()?;
    let kv = vdf::parse(&t);
    let users = kv.get("users")?;
    let mut best = None;
    for (id, u) in users.entries() {
        let entry = (id.clone(), u.str("personaname").unwrap_or_default().to_string());
        if u.str("mostrecent") == Some("1") {
            return Some(entry);
        }
        best.get_or_insert(entry);
    }
    best
}

/// Which VR API a game folder ships, a few levels deep.
pub fn scan_vr(dir: &Path, depth: u32) -> Option<&'static str> {
    let rd = std::fs::read_dir(dir).ok()?;
    let mut subdirs = vec![];
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_lowercase();
        let Ok(ft) = e.file_type() else { continue };
        if ft.is_file() {
            if name.starts_with("openxr_loader") || name.starts_with("libopenxr_loader") {
                return Some("openxr");
            }
            if name.starts_with("openvr_api") || name.starts_with("libopenvr_api") {
                return Some("openvr");
            }
            if name == "ovrplugin.dll" || name.starts_with("libovrrt") || name == "oculusxrplugin.dll" {
                return Some("oculus");
            }
        } else if ft.is_dir() && depth > 0 && !name.starts_with('.') && !["_commonredist", "redist", "directx", "vcredist", "__installer"].contains(&name.as_str()) {
            subdirs.push(e.path());
        }
    }
    subdirs.into_iter().find_map(|d| scan_vr(&d, depth - 1))
}

pub fn art(app_id: &str) -> Art {
    let cdn = format!("https://cdn.cloudflare.steamstatic.com/steam/apps/{app_id}");
    Art { cover: Some(format!("{cdn}/library_600x900.jpg")), hero: Some(format!("{cdn}/library_hero.jpg")), wide: Some(format!("{cdn}/header.jpg")) }
}

pub fn installed(root: &Path) -> Vec<Game> {
    let mut out = vec![];
    for lib in libraries(root) {
        let apps = lib.join("steamapps");
        let Ok(rd) = std::fs::read_dir(&apps) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if !(n.starts_with("appmanifest_") && n.ends_with(".acf")) {
                continue;
            }
            let Ok(t) = std::fs::read_to_string(e.path()) else { continue };
            let kv = vdf::parse(&t);
            let Some(st) = kv.get("appstate") else { continue };
            let id = st.str("appid").unwrap_or_default().to_string();
            let name = st.str("name").unwrap_or_default().to_string();
            let lname = name.to_lowercase();
            if id.is_empty() || NOT_GAMES.contains(&id.as_str()) || ["redistributable", "proton", "steam linux runtime", "steamworks common"].iter().any(|x| lname.contains(x)) {
                continue;
            }
            let dir = apps.join("common").join(st.str("installdir").unwrap_or_default());
            let flags: u64 = st.str("stateflags").and_then(|s| s.parse().ok()).unwrap_or(0);
            out.push(Game {
                id: format!("steam:{id}"),
                source: Source::Steam,
                name,
                installed: flags & 4 == 4,
                install_dir: Some(dir.clone()),
                size: st.str("sizeondisk").and_then(|s| s.parse().ok()).unwrap_or(0),
                last_played: st.str("lastplayed").and_then(|s| s.parse::<u64>().ok()).unwrap_or(0) * 1000,
                vr_api: scan_vr(&dir, 4),
                art: art(&id),
                app_id: id,
                ..Default::default()
            });
        }
    }
    out
}

/// VR flags from the store, one request per app, cached forever. Returns (vr, vr_only).
pub fn vr_tags(ids: &[String], cache: &mut HashMap<String, Option<(bool, bool)>>) {
    for id in ids.iter().filter(|i| !cache.contains_key(*i)).take(40).cloned().collect::<Vec<_>>() {
        let url = format!("https://store.steampowered.com/api/appdetails?appids={id}&filter=categories");
        let Ok(r) = ureq::get(&url).timeout(std::time::Duration::from_secs(10)).call() else { continue };
        let Ok(j) = r.into_json::<serde_json::Value>() else { continue };
        let cats: Vec<i64> = j[&id]["data"]["categories"].as_array().map(|a| a.iter().filter_map(|c| c["id"].as_i64()).collect()).unwrap_or_default();
        let ok = j[&id]["success"].as_bool().unwrap_or(false);
        cache.insert(id, ok.then(|| (cats.iter().any(|c| VR_CATEGORIES.contains(c)), cats.contains(&54))));
    }
}

pub fn owned_via_api_key(steam_id: &str, key: &str) -> Result<Vec<(String, String, u64)>, String> {
    let url = format!("https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?key={key}&steamid={steam_id}&include_appinfo=1&include_played_free_games=1");
    let j: serde_json::Value = ureq::get(&url).call().map_err(|e| format!("Steam Web API: {e}"))?.into_json().map_err(|e| e.to_string())?;
    Ok(j["response"]["games"]
        .as_array()
        .map(|a| a.iter().map(|g| (g["appid"].as_u64().unwrap_or(0).to_string(), g["name"].as_str().unwrap_or("").to_string(), g["playtime_forever"].as_u64().unwrap_or(0))).collect())
        .unwrap_or_default())
}

/// Public profile name for a Steam ID.
pub fn profile_name(steam_id: &str) -> Option<String> {
    let xml = ureq::get(&format!("https://steamcommunity.com/profiles/{steam_id}/?xml=1")).call().ok()?.into_string().ok()?;
    let a = xml.find("<steamID><![CDATA[")? + 18;
    let b = xml[a..].find("]]>")? + a;
    Some(xml[a..b].to_string())
}
