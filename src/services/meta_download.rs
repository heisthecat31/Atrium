//! Downloading Rift/PC VR apps a signed-in Meta account owns, any version Meta still serves.
//!
//! The same calls as meta-pcvr-downloader (MIT, github.com/Villagers654/meta-pcvr-downloader),
//! which RiftLift uses, with the Oculus profile token Atrium's Meta sign-in already holds:
//! - the app's builds come from graph.oculus.com (`supportedBinaries`, newest first);
//! - a build's manifest is a zip from securecdn (`get_manifest=1`) listing every file, its
//!   SHA-256 and its segments (each `segmentSize` bytes of the file, by SHA-256);
//! - each segment is zlib data from securecdn's `binaries/segment`.
//!
//! Segments are written straight into the game's files (no second copy on disk). A segment that
//! is already right on disk is kept, so a stopped download resumes; one that another downloaded
//! version of the game has at the same place is copied from there, so a new version only fetches
//! what changed. Every file is checked against its SHA-256 at the end.
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

const GRAPHQL: &str = "https://graph.oculus.com/graphql";
const VERSIONS_DOC: &str = "1586217024733717";
const CDN: &str = "https://securecdn.oculus.com/binaries";
const WORKERS: usize = 8;
const ATTEMPTS: u32 = 5;

#[derive(Clone, Debug, Serialize)]
pub struct Build {
    pub binary_id: String,
    pub version: String,
    pub version_code: i64,
}

/// What a finished download left on disk (saved in settings, shown in the library).
#[derive(Clone, Debug)]
pub struct Installed {
    pub canonical: String,
    pub launch_file: String,
    pub launch_args: String,
}

fn agent() -> ureq::Agent {
    ureq::AgentBuilder::new().timeout_connect(Duration::from_secs(20)).timeout_read(Duration::from_secs(120)).build()
}

/// Every PC build Meta serves for `app_id`, newest first, and the app's name.
pub fn builds(token: &str, app_id: &str) -> Result<(String, Vec<Build>), String> {
    let vars = format!("{{\"id\":\"{app_id}\"}}");
    let j: serde_json::Value = agent()
        .post(GRAPHQL)
        .send_form(&[("access_token", token), ("variables", &vars), ("doc_id", VERSIONS_DOC)])
        .map_err(|e| format!("Meta: {}", redact(&e.to_string())))?
        .into_json()
        .map_err(|e| e.to_string())?;
    if let Some(m) = j["errors"][0]["message"].as_str() {
        return Err(format!("Meta: {m}"));
    }
    let node = &j["data"]["node"];
    let name = node["displayName"].as_str().or(node["display_name"].as_str()).unwrap_or(app_id).to_string();
    let mut out: Vec<Build> = node["supportedBinaries"]["edges"]
        .as_array()
        .ok_or("Meta has no PC version of this app")?
        .iter()
        .map(|e| &e["node"])
        .filter(|b| b["__typename"].as_str() == Some("PCBinary"))
        .filter_map(|b| {
            Some(Build {
                binary_id: b["id"].as_str()?.to_string(),
                version: b["version"].as_str().unwrap_or("unknown").to_string(),
                version_code: b["versionCode"].as_i64().unwrap_or(0),
            })
        })
        .collect();
    if out.is_empty() {
        return Err("Meta has no PC version of this app".into());
    }
    out.sort_by(|a, b| b.version_code.cmp(&a.version_code));
    Ok((name, out))
}

/// A build's version, file count and size, from its manifest (nothing downloaded but the list).
pub fn build_info(token: &str, binary_id: &str) -> Result<(String, usize, u64), String> {
    let m = manifest(token, binary_id)?;
    let files = m["files"].as_object().map(|f| (f.len(), f.values().filter_map(|v| v["size"].as_u64()).sum())).unwrap_or((0, 0));
    Ok((m["version"].as_str().unwrap_or("?").to_string(), files.0, files.1))
}

/// Tokens in URLs must never reach logs or the page.
fn redact(s: &str) -> String {
    match s.find("access_token=") {
        Some(i) => {
            let end = s[i..].find(['&', ' ', ':']).map(|n| i + n).unwrap_or(s.len());
            format!("{}access_token=…{}", &s[..i], &s[end..])
        }
        None => s.to_string(),
    }
}

fn manifest(token: &str, binary_id: &str) -> Result<serde_json::Value, String> {
    let mut last = String::new();
    for attempt in 0..3 {
        match agent().get(&format!("{CDN}/download/")).query("id", binary_id).query("access_token", token).query("get_manifest", "1").call() {
            Ok(r) => {
                let mut zip = vec![];
                r.into_reader().read_to_end(&mut zip).map_err(|e| e.to_string())?;
                let mut a = zip::ZipArchive::new(std::io::Cursor::new(zip)).map_err(|_| "Meta sent a broken file list")?;
                let name = if a.file_names().any(|n| n == "manifest.json") { "manifest.json".to_string() } else { a.file_names().next().ok_or("Meta sent an empty file list")?.to_string() };
                let mut text = String::new();
                a.by_name(&name).map_err(|e| e.to_string())?.read_to_string(&mut text).map_err(|e| e.to_string())?;
                let m: serde_json::Value = serde_json::from_str(&text).map_err(|_| "Meta sent a broken file list")?;
                if !m["files"].is_object() {
                    return Err("Meta's file list is empty".into());
                }
                return Ok(m);
            }
            Err(ureq::Error::Status(401 | 403, _)) => return Err("Meta refused: this account doesn't own the PC version, or needs signing in again".into()),
            Err(ureq::Error::Status(c, _)) if ![429, 500, 502, 503, 504].contains(&c) => return Err(format!("Meta answered {c}")),
            Err(e) => last = redact(&e.to_string()),
        }
        std::thread::sleep(Duration::from_secs(1 << attempt));
    }
    Err(format!("Couldn't get the file list: {last}"))
}

/// A manifest path as a path under `root`, refusing anything that would leave it.
fn safe_path(root: &Path, rel: &str) -> Result<PathBuf, String> {
    let p = PathBuf::from(rel.replace('\\', "/"));
    if p.components().any(|c| !matches!(c, Component::Normal(_))) {
        return Err(format!("Unsafe path in Meta's file list: {rel}"));
    }
    Ok(root.join(p))
}

fn sha_hex(data: &[u8]) -> String {
    Sha256::digest(data).iter().map(|b| format!("{b:02x}")).collect()
}

fn file_sha(path: &Path) -> Option<String> {
    let mut f = File::open(path).ok()?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 4 << 20];
    loop {
        let n = f.read(&mut buf).ok()?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Some(h.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

/// Where one segment's bytes go (a segment can repeat, in one file or several).
struct Place {
    /// Path in the manifest (to find the same file in other versions' folders).
    rel: String,
    file: PathBuf,
    offset: u64,
}

struct Segment {
    sha: String,
    size: u64,
    places: Vec<Place>,
}

/// One segment, inflated and checked.
fn fetch_segment(token: &str, binary_id: &str, sha: &str, cancel: &AtomicBool) -> Result<Vec<u8>, String> {
    let mut last = String::new();
    for attempt in 0..ATTEMPTS {
        if cancel.load(Ordering::Relaxed) {
            return Err("cancelled".into());
        }
        match agent().get(&format!("{CDN}/segment/")).query("access_token", token).query("binary_id", binary_id).query("segment_sha256", sha).call() {
            Ok(r) => {
                let mut data = vec![];
                match flate2::read::ZlibDecoder::new(r.into_reader()).read_to_end(&mut data) {
                    Ok(_) if sha_hex(&data) == sha => return Ok(data),
                    Ok(_) => last = "a piece arrived damaged".into(),
                    Err(e) => last = e.to_string(),
                }
            }
            Err(ureq::Error::Status(401 | 403, _)) => return Err("Meta refused the download: sign in to Meta again".into()),
            Err(ureq::Error::Status(c, _)) if ![429, 500, 502, 503, 504].contains(&c) => return Err(format!("Meta answered {c}")),
            Err(e) => last = redact(&e.to_string()),
        }
        std::thread::sleep(Duration::from_secs((1u64 << attempt).min(30)));
    }
    Err(format!("Download failed: {last}"))
}

fn write_at(path: &Path, offset: u64, data: &[u8]) -> std::io::Result<()> {
    let mut f = OpenOptions::new().write(true).open(path)?;
    f.seek(SeekFrom::Start(offset))?;
    f.write_all(data)
}

/// Bytes already on disk where the segment goes, if they're right.
fn already_there(seg: &Segment) -> bool {
    seg.places.iter().all(|p| {
        let Ok(mut f) = File::open(&p.file) else { return false };
        let mut buf = vec![0u8; seg.size as usize];
        f.seek(SeekFrom::Start(p.offset)).is_ok() && f.read_exact(&mut buf).is_ok() && sha_hex(&buf) == seg.sha
    })
}

/// The segment's bytes from another version's folder, if one has them at the same place.
fn local_copy(seg: &Segment, seeds: &[PathBuf]) -> Option<Vec<u8>> {
    for seed in seeds {
        for p in &seg.places {
            let Ok(path) = safe_path(seed, &p.rel) else { continue };
            let Ok(mut f) = File::open(path) else { continue };
            let mut buf = vec![0u8; seg.size as usize];
            if f.seek(SeekFrom::Start(p.offset)).is_ok() && f.read_exact(&mut buf).is_ok() && sha_hex(&buf) == seg.sha {
                return Some(buf);
            }
        }
    }
    None
}

/// Download build `binary_id` into `dir`; `seeds` are other versions' folders to copy from. `progress(phase, done, total)` is called often (bytes).
pub fn download(token: &str, binary_id: &str, dir: &Path, seeds: &[PathBuf], cancel: &AtomicBool, progress: &(dyn Fn(&str, u64, u64) + Sync)) -> Result<Installed, String> {
    progress("Getting the file list", 0, 0);
    let m = manifest(token, binary_id)?;
    let files = m["files"].as_object().ok_or("Meta's file list is empty")?;
    // Files the version Atrium downloaded here before had and this one doesn't: removed once this
    // one is complete (only those: mods and saves the user added stay).
    let stale: Vec<String> = std::fs::read(dir.join("atrium_manifest.json"))
        .ok()
        .and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok())
        .and_then(|old| old["files"].as_object().map(|o| o.keys().filter(|k| !files.contains_key(*k)).cloned().collect()))
        .unwrap_or_default();
    // Saved first: it marks the folder as Atrium's (removal checks for it) and helps debugging.
    std::fs::create_dir_all(dir).map_err(|e| format!("Couldn't create {}: {e}", dir.display()))?;
    let _ = std::fs::write(dir.join("atrium_manifest.json"), serde_json::to_vec_pretty(&m).unwrap_or_default());

    // Lay out every file and gather unique segments with every place they go.
    let mut segs: Vec<Segment> = vec![];
    let mut by_sha: HashMap<String, usize> = HashMap::new();
    let mut checks: Vec<(PathBuf, u64, String)> = vec![];
    for (rel, f) in files {
        let path = safe_path(dir, rel)?;
        let size = f["size"].as_u64().unwrap_or(0);
        checks.push((path.clone(), size, f["sha256"].as_str().unwrap_or("").to_string()));
        // Segment n holds bytes n*segmentSize.. of the file (the last one the rest). The entry's
        // first number is Meta's own, its last the compressed size: neither is needed here.
        let step = f["segmentSize"].as_u64().filter(|s| *s > 0).unwrap_or(size.max(1));
        for (n, s) in f["segments"].as_array().map(Vec::as_slice).unwrap_or(&[]).iter().enumerate() {
            let Some(sha) = s[1].as_str() else { return Err(format!("Meta's file list is broken at {rel}")) };
            let at = n as u64 * step;
            let len = step.min(size.saturating_sub(at));
            let i = *by_sha.entry(sha.to_string()).or_insert_with(|| {
                segs.push(Segment { sha: sha.to_string(), size: len, places: vec![] });
                segs.len() - 1
            });
            segs[i].places.push(Place { rel: rel.clone(), file: path.clone(), offset: at });
        }
    }
    let total: u64 = checks.iter().map(|c| c.1).sum();
    let need: u64 = segs.iter().map(|s| s.size * s.places.len() as u64).sum();

    // Room for what isn't there yet (roughly: files already the right size count as there).
    let present: u64 = checks.iter().filter(|(p, size, _)| std::fs::metadata(p).map(|m| m.len() == *size).unwrap_or(false)).map(|c| c.1).sum();
    std::fs::create_dir_all(dir).map_err(|e| format!("Couldn't create {}: {e}", dir.display()))?;
    if let Some(free) = free_space(dir) {
        let missing = total.saturating_sub(present);
        if free < missing {
            return Err(format!("Not enough space: needs {:.1} GB more, {:.1} GB free", missing as f64 / 1e9, free as f64 / 1e9));
        }
    }

    // Files at their final size, so segments can land anywhere in them.
    for (path, size, _) in &checks {
        if let Some(p) = path.parent() {
            std::fs::create_dir_all(p).map_err(|e| e.to_string())?;
        }
        let f = OpenOptions::new().create(true).write(true).truncate(false).open(path).map_err(|e| format!("Couldn't write {}: {e}", path.display()))?;
        f.set_len(*size).map_err(|e| e.to_string())?;
    }

    let done = AtomicU64::new(0);
    let next = AtomicUsize::new(0);
    let failed: Mutex<Option<String>> = Mutex::new(None);
    let stop = AtomicBool::new(false);
    std::thread::scope(|sc| {
        for _ in 0..WORKERS {
            sc.spawn(|| loop {
                if stop.load(Ordering::Relaxed) || cancel.load(Ordering::Relaxed) {
                    return;
                }
                let i = next.fetch_add(1, Ordering::Relaxed);
                let Some(seg) = segs.get(i) else { return };
                let bytes = seg.size * seg.places.len() as u64;
                let r = if already_there(seg) {
                    Ok(())
                } else {
                    local_copy(seg, seeds).map(Ok).unwrap_or_else(|| fetch_segment(token, binary_id, &seg.sha, cancel)).and_then(|data| {
                        seg.places.iter().try_for_each(|p| write_at(&p.file, p.offset, &data)).map_err(|e| format!("Couldn't write the game's files: {e}"))
                    })
                };
                match r {
                    Ok(()) => {
                        let d = done.fetch_add(bytes, Ordering::Relaxed) + bytes;
                        progress("Downloading", d, need);
                    }
                    Err(e) => {
                        stop.store(true, Ordering::Relaxed);
                        failed.lock().unwrap_or_else(|e| e.into_inner()).get_or_insert(e);
                        return;
                    }
                }
            });
        }
    });
    if cancel.load(Ordering::Relaxed) {
        return Err("cancelled".into());
    }
    if let Some(e) = failed.into_inner().unwrap_or_else(|e| e.into_inner()) {
        return Err(e);
    }

    let mut checked = 0u64;
    for (path, size, sha) in &checks {
        if cancel.load(Ordering::Relaxed) {
            return Err("cancelled".into());
        }
        progress("Checking files", checked, total);
        if !sha.is_empty() && file_sha(path).as_deref() != Some(sha.as_str()) {
            return Err(format!("{} didn't download correctly; try again", path.strip_prefix(dir).unwrap_or(path).display()));
        }
        checked += size;
    }
    progress("Checking files", total, total);
    for rel in &stale {
        if let Ok(p) = safe_path(dir, rel) {
            let _ = std::fs::remove_file(p);
        }
    }

    Ok(Installed {
        canonical: m["canonicalName"].as_str().unwrap_or_default().to_string(),
        launch_file: m["launchFile"].as_str().unwrap_or_default().to_string(),
        launch_args: m["launchParameters"].as_str().unwrap_or_default().to_string(),
    })
}

#[cfg(windows)]
fn free_space(dir: &Path) -> Option<u64> {
    use std::os::windows::ffi::OsStrExt;
    let w: Vec<u16> = dir.as_os_str().encode_wide().chain([0]).collect();
    let mut free = 0u64;
    let ok = unsafe { windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW(w.as_ptr(), &mut free, std::ptr::null_mut(), std::ptr::null_mut()) };
    (ok != 0).then_some(free)
}

#[cfg(not(windows))]
fn free_space(dir: &Path) -> Option<u64> {
    use std::os::unix::ffi::OsStrExt;
    let c = std::ffi::CString::new(dir.as_os_str().as_bytes()).ok()?;
    let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
    (unsafe { libc::statvfs(c.as_ptr(), &mut s) } == 0).then(|| s.f_bavail as u64 * s.f_frsize as u64)
}

/// A download running now, as the page shows it.
#[derive(Clone, Debug, Default, Serialize)]
pub struct Progress {
    pub version: String,
    pub phase: String,
    pub done: u64,
    pub total: u64,
    #[serde(skip)]
    pub cancel: Arc<AtomicBool>,
}
