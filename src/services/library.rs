//! One library: Steam, Meta and local games as the same `Game` record.
use std::path::PathBuf;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Source {
    #[default]
    Local,
    Steam,
    Meta,
}

/// Image URLs (https:// or file://).
#[derive(Clone, Debug, Default)]
pub struct Art {
    pub cover: Option<String>,
    pub hero: Option<String>,
    pub wide: Option<String>,
}

#[derive(Clone, Debug, Default)]
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
