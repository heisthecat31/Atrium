//! One library: Steam, Meta and local games as the same `Game` record.
//!
//! `scan` is the fast part (files on disk, no network). `refresh_owned` adds what the signed-in
//! accounts own and fills the name/VR caches; it talks to Steam and Meta, so run it off the UI
//! thread.
use crate::services::{meta, steam};
use crate::sys::{self, settings::Settings};
use serde::Serialize;
use std::collections::HashMap;
use std::path::PathBuf;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Source {
    #[default]
    Local,
    Steam,
    Meta,
}

impl Source {
    pub fn label(self) -> &'static str {
        match self {
            Source::Local => "Local",
            Source::Steam => "Steam",
            Source::Meta => "Meta",
        }
    }
}

/// Image URLs (https:// or file://).
#[derive(Clone, Debug, Default, Serialize)]
pub struct Art {
    pub cover: Option<String>,
    pub hero: Option<String>,
    pub wide: Option<String>,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Game {
    /// "steam:<appid>", "meta:<appid>", "local:<id>"
    pub id: String,
    pub source: Source,
    pub name: String,
    pub app_id: String,
    /// Meta canonical name (folder under Software\).
    pub canonical: String,
    pub installed: bool,
    pub owned: bool,
    pub install_dir: Option<PathBuf>,
    pub exe: Option<PathBuf>,
    pub args: String,
    /// Bytes on disk.
    pub size: u64,
    /// ms since epoch.
    pub last_played: u64,
    pub vr: bool,
    pub vr_only: bool,
    /// `vr` came from a reliable source (manifest, store tags), not a guess.
    pub vr_known: bool,
    /// "openxr" | "openvr" | "oculus"
    pub vr_api: Option<&'static str>,
    pub art: Art,
}

/// What the accounts own, fetched by `refresh_owned`. Kept between scans so a rescan of the disk
/// doesn't drop owned-but-not-installed games.
#[derive(Clone, Debug, Default)]
pub struct Owned {
    /// (appid, name, minutes played)
    pub steam: Vec<(String, String, u64)>,
    /// (appid, name, canonical)
    pub meta: Vec<(String, String, String)>,
    pub errors: Vec<String>,
}

/// Everything Atrium knows about, merged and sorted (recently played first, then by name).
pub fn scan(settings: &Settings, owned: &Owned) -> Vec<Game> {
    let mut games: HashMap<String, Game> = HashMap::new();

    if let Some(root) = steam::root() {
        for g in steam::installed(&root) {
            games.insert(g.id.clone(), g);
        }
    }
    for (id, name, _) in &owned.steam {
        let g = games.entry(format!("steam:{id}")).or_insert_with(|| Game {
            id: format!("steam:{id}"),
            source: Source::Steam,
            name: name.clone(),
            app_id: id.clone(),
            art: steam::art(id),
            ..Default::default()
        });
        g.owned = true;
    }
    for g in games.values_mut().filter(|g| g.source == Source::Steam) {
        match settings.steam_vr_cache.get(&g.app_id) {
            Some(Some((vr, only))) => {
                g.vr = *vr || g.vr_api.is_some();
                g.vr_only = *only;
                g.vr_known = true;
            }
            _ => {
                g.vr = g.vr_api.is_some();
                g.vr_known = g.vr_api.is_some();
            }
        }
    }

    for mut g in meta::installed() {
        g.owned = owned.meta.iter().any(|(id, ..)| *id == g.app_id);
        games.insert(g.id.clone(), g);
    }
    for (id, name, canonical) in &owned.meta {
        let g = games.entry(format!("meta:{id}")).or_insert_with(|| Game {
            id: format!("meta:{id}"),
            source: Source::Meta,
            app_id: id.clone(),
            canonical: canonical.clone(),
            vr: true,
            vr_known: true,
            vr_api: Some("oculus"),
            ..Default::default()
        });
        g.owned = true;
        g.name = name.clone();
    }
    for g in games.values_mut().filter(|g| g.source == Source::Meta) {
        g.vr_only = true;
        if let Some(Some(n)) = settings.meta_name_cache.get(&g.app_id) {
            if !owned.meta.iter().any(|(id, ..)| *id == g.app_id) {
                g.name = n.clone();
            }
        }
    }

    for l in &settings.local_games {
        let exe = PathBuf::from(&l.exe);
        let id = format!("local:{}", l.id);
        games.insert(id.clone(), Game {
            id,
            source: Source::Local,
            name: l.name.clone(),
            app_id: l.id.clone(),
            installed: exe.exists(),
            install_dir: exe.parent().map(PathBuf::from),
            vr_api: exe.parent().and_then(|d| steam::scan_vr(d, 3)),
            exe: Some(exe),
            args: l.args.clone(),
            vr: true,
            vr_known: true,
            owned: true,
            ..Default::default()
        });
    }

    let mut out: Vec<Game> = games.into_values().collect();
    for g in &mut out {
        if let Some(t) = settings.plays.get(&g.id) {
            g.last_played = g.last_played.max(*t);
        }
    }
    out.sort_by(|a, b| b.last_played.cmp(&a.last_played).then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase())));
    out
}

/// Network part: owned games for every signed-in account, plus store lookups that fill the VR
/// and name caches in `settings` (the caller saves it).
pub fn refresh_owned(settings: &mut Settings) -> Owned {
    let mut owned = Owned::default();

    if let (Some(acc), Some(key)) = (&settings.steam_account, settings.steam_api_key.as_deref().and_then(sys::secret::unseal)) {
        match steam::owned_via_api_key(&acc.steam_id, &key) {
            Ok(v) => owned.steam = v,
            Err(e) => owned.errors.push(e),
        }
    }
    for acc in &settings.meta_accounts {
        let Some(token) = sys::secret::unseal(&acc.token) else {
            owned.errors.push(format!("Meta ({}): sign in again", acc.name));
            continue;
        };
        match meta::owned(&token) {
            Ok(v) => {
                for item in v {
                    if !owned.meta.iter().any(|(id, ..)| *id == item.0) {
                        owned.meta.push(item);
                    }
                }
            }
            Err(e) => owned.errors.push(format!("Meta ({}): {e}", acc.name)),
        }
    }

    // VR tags for Steam games we haven't classified yet: installed ones first.
    let mut ids: Vec<String> = steam::root().map(|r| steam::installed(&r).into_iter().map(|g| g.app_id).collect()).unwrap_or_default();
    ids.extend(owned.steam.iter().map(|(id, ..)| id.clone()));
    ids.dedup();
    steam::vr_tags(&ids, &mut settings.steam_vr_cache);

    // Real names for installed Meta games the accounts don't cover.
    let meta_ids: Vec<String> = meta::installed().into_iter().map(|g| g.app_id).filter(|id| !owned.meta.iter().any(|(o, ..)| o == id)).collect();
    meta::store_names(&meta_ids, &mut settings.meta_name_cache);

    owned
}
