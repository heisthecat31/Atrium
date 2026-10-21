//! Meta (Oculus) PC library: apps installed through Meta Horizon Link, and the Rift/PC VR apps a
//! signed-in Meta account owns.
//!
//! Sign-in is Meta's native-SSO flow as RiftLift (MIT, github.com/Villagers654/RiftLift) does it.
//! Meta's page finishes on an oculus:// URL; Atrium catches it inside its own sign-in window
//! (src/login.rs) and never registers as the system's oculus:// handler (Meta's app owns it).
use crate::services::library::{Art, Game, Source};
use crate::sys;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::path::PathBuf;

const FRL_APP_ID: &str = "512466987071624";
const OCULUS_APP_ID: &str = "1582076955407037";
const FRL_CLIENT_TOKEN: &str = "FRL|512466987071624|01d4a1f7fd0682aea7ee8ae987704d63";
const META_GRAPH: &str = "https://meta.graph.meta.com";
const META_AUTH_URL: &str = "https://auth.meta.com/native_sso/confirm";
const PROFILE_TOKEN_DOC: &str = "24112177345042346";
const ENTITLEMENTS_DOC: &str = "4850747515044496";
pub const STORE_ASSETS: &str = "C:\\Program Files\\Oculus\\CoreData\\Software\\StoreAssets";

// ---- installed ------------------------------------------------------------------------------
fn library_folders() -> Vec<PathBuf> {
    let mut out: Vec<PathBuf> = vec![];
    #[cfg(windows)]
    for v in sys::reg::subkeys("HKCU\\Software\\Oculus VR, LLC\\Oculus\\Libraries") {
        // OriginalPath is the drive-letter form; Path a volume-GUID path some games can't run from.
        let p = [v.get("OriginalPath"), v.get("Path")].into_iter().flatten().map(PathBuf::from).find(|p| p.join("Manifests").exists());
        if let Some(p) = p {
            if !out.iter().any(|o| o.to_string_lossy().eq_ignore_ascii_case(&p.to_string_lossy())) {
                out.push(p);
            }
        }
    }
    out
}

pub fn local_art(canonical: &str) -> Art {
    let dir = PathBuf::from(STORE_ASSETS).join(format!("{canonical}_assets"));
    let pick = |f: &str| {
        let p = dir.join(f);
        p.exists().then(|| format!("file://{}", p.to_string_lossy().replace('\\', "/")))
    };
    Art { cover: pick("cover_square_image.jpg"), hero: pick("cover_landscape_image_large.png").or_else(|| pick("cover_landscape_image.jpg")), wide: pick("cover_landscape_image.jpg") }
}

fn prettify(c: &str) -> String {
    c.split(['-', '_']).filter(|w| !w.is_empty()).map(|w| {
        let mut ch = w.chars();
        ch.next().map(|f| f.to_uppercase().collect::<String>() + ch.as_str()).unwrap_or_default()
    }).collect::<Vec<_>>().join(" ")
}

pub fn installed() -> Vec<Game> {
    let mut out = vec![];
    let mut seen = std::collections::HashSet::new();
    for lib in library_folders() {
        let Ok(rd) = std::fs::read_dir(lib.join("Manifests")) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if !n.ends_with(".json") || n.ends_with("_assets.json") {
                continue;
            }
            let Some(m) = sys::read_json(&e.path()) else { continue };
            let app_id = m["appId"].as_str().unwrap_or_default().to_string();
            if m["isCore"].as_bool().unwrap_or(false) || m["packageType"].as_str() != Some("APP") || !seen.insert(app_id.clone()) {
                continue;
            }
            let cn = m["canonicalName"].as_str().unwrap_or_default().to_string();
            let dir = lib.join("Software").join(&cn);
            let exe = m["launchFile"].as_str().map(|f| dir.join(f));
            out.push(Game {
                id: format!("meta:{app_id}"),
                source: Source::Meta,
                name: prettify(&cn),
                installed: exe.as_ref().is_some_and(|p| p.exists()),
                install_dir: Some(dir),
                exe,
                args: m["launchParameters"].as_str().unwrap_or_default().to_string(),
                vr: true,
                vr_known: true,
                vr_api: Some("oculus"),
                art: local_art(&cn),
                canonical: cn,
                app_id,
                ..Default::default()
            });
        }
    }
    out
}

/// Real names without signing in: the public store page title ("Echo VR on Oculus Rift | ...").
pub fn store_names(ids: &[String], cache: &mut HashMap<String, Option<String>>) {
    for id in ids.iter().filter(|i| !cache.contains_key(*i)).take(20).cloned().collect::<Vec<_>>() {
        let Ok(r) = ureq::get(&format!("https://www.meta.com/experiences/{id}/")).set("User-Agent", "Mozilla/5.0").set("Accept-Language", "en").timeout(std::time::Duration::from_secs(15)).call() else { continue };
        let html = r.into_string().unwrap_or_default();
        let title = html.split_once("<title").and_then(|(_, t)| t.split_once('>')).and_then(|(_, t)| t.split_once("</title>")).map(|(t, _)| t).unwrap_or("");
        let mut name = title.split('|').next().unwrap_or("").trim().to_string();
        for suffix in [" on Oculus Rift", " on Meta Quest", " on Rift"] {
            if let Some(i) = name.find(suffix) {
                name.truncate(i);
            }
        }
        let name = name.replace("&amp;", "&").replace("&#039;", "'").replace("&#x27;", "'");
        cache.insert(id, (!name.is_empty() && !name.to_lowercase().contains("meta store")).then_some(name));
    }
}

// ---- account --------------------------------------------------------------------------------
fn post_multipart(path: &str, fields: &[(&str, &str)]) -> Result<serde_json::Value, String> {
    let boundary = format!("----Atrium{:032x}", rand::random::<u128>());
    let mut body = String::new();
    for (k, v) in fields {
        body += &format!("--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n");
    }
    body += &format!("--{boundary}--\r\n");
    let r = ureq::post(&format!("{META_GRAPH}{path}"))
        .set("Content-Type", &format!("multipart/form-data; boundary={boundary}"))
        .set("Accept", "application/json")
        .send_string(&body);
    let j: serde_json::Value = match r {
        Ok(r) => r.into_json().map_err(|e| e.to_string())?,
        Err(ureq::Error::Status(_, r)) => r.into_json().unwrap_or_default(),
        Err(e) => return Err(format!("Couldn't reach Meta: {e}")),
    };
    if let Some(m) = j["error"]["message"].as_str() {
        return Err(m.to_string());
    }
    Ok(j)
}

pub struct LoginRequest {
    pub request_token: String,
    pub url: String,
}

pub fn begin_login() -> Result<LoginRequest, String> {
    let r = post_multipart("/webview_tokens_query", &[("access_token", FRL_CLIENT_TOKEN)])?;
    let (Some(t), Some(e)) = (r["native_sso_token"].as_str(), r["native_sso_etoken"].as_str()) else { return Err("Meta didn't start a sign-in".into()) };
    let q = url::form_urlencoded::Serializer::new(String::new())
        .append_pair("native_app_id", FRL_APP_ID)
        .append_pair("source_app_id", FRL_APP_ID)
        .append_pair("native_sso_etoken", e)
        .append_pair("utm_source", "atrium")
        .finish();
    Ok(LoginRequest { request_token: t.to_string(), url: format!("{META_AUTH_URL}?{q}") })
}

pub fn complete_login(request_token: &str, callback: &str) -> Result<String, String> {
    let u = url::Url::parse(callback).map_err(|e| e.to_string())?;
    if !["oculus", "oculus-client"].contains(&u.scheme()) {
        return Err("unexpected sign-in callback".into());
    }
    let q: HashMap<_, _> = u.query_pairs().into_owned().collect();
    let expected: String = Sha256::digest(request_token.as_bytes()).iter().map(|b| format!("{b:02x}")).collect::<String>()[..16].to_string();
    if q.get("token").map(String::as_str) != Some(expected.as_str()) {
        return Err("Meta returned a sign-in for another request".into());
    }
    let blob = q.get("blob").ok_or("Meta returned no session")?;
    let dec = post_multipart("/webview_blobs_decrypt", &[("access_token", FRL_CLIENT_TOKEN), ("blob", blob), ("request_token", request_token)])?;
    let meta_token = dec["access_token"].as_str().ok_or("Meta returned no account token")?;
    let vars = format!("{{\"app_id\":\"{OCULUS_APP_ID}\"}}");
    let g = post_multipart("/graphql", &[("access_token", meta_token), ("doc_id", PROFILE_TOKEN_DOC), ("variables", &vars)])?;
    g["data"]["xfr_create_profile_token"]["profile_tokens"][0]["access_token"].as_str().map(str::to_string).ok_or_else(|| "Meta returned no Oculus profile token".into())
}

pub fn identity(token: &str) -> Option<(String, String)> {
    let j: serde_json::Value = ureq::get("https://graph.oculus.com/me").query("access_token", token).query("fields", "id,alias").call().ok()?.into_json().ok()?;
    Some((format!("meta-{}", j["id"].as_str()?), j["alias"].as_str().unwrap_or("").to_string()))
}

/// (app id, name, slug) of every PC app the account owns.
pub fn owned(token: &str) -> Result<Vec<(String, String, String)>, String> {
    let j: serde_json::Value = ureq::post("https://graph.oculus.com/graphql")
        .send_form(&[("access_token", token), ("variables", "{}"), ("doc_id", ENTITLEMENTS_DOC)])
        .map_err(|e| format!("Meta: {e}"))?
        .into_json()
        .map_err(|e| e.to_string())?;
    let nodes = j["data"]["viewer"]["user"]["active_entitlements"]["nodes"].as_array().ok_or("Meta returned no library")?;
    Ok(nodes
        .iter()
        .map(|n| &n["item"])
        .filter(|i| i["platform"].as_str() == Some("PC"))
        .filter_map(|i| Some((i["id"].as_str()?.to_string(), i["display_name"].as_str()?.to_string(), i["canonical_name"].as_str().unwrap_or("").to_string())))
        .collect())
}

pub fn store_url(app_id: &str) -> String {
    format!("https://www.meta.com/experiences/pcvr/{app_id}/")
}
