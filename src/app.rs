//! Atrium's state and the commands the page sends. The page (web/) holds no state of its own:
//! after every change Rust pushes a full snapshot (`atrium.update(...)`) and the page redraws.
//! Slow work (USB/Meta probing, Steam/Meta requests, sign-in, file dialogs) runs on threads.
use crate::login;
use crate::services::{headsets::{self, Hardware}, atrium_service, dash, launcher, library::{self as lib, Game, Owned}, meta_download::{self as dl, Build, Progress}, meta_runtime, runtime::{self as rt, RuntimeState}, standalone};
use crate::sys::{self, settings::{GameOptions, LocalGame, MetaInstall, MetaVersion, Settings}};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

/// Sent to the window's event loop (src/ui.rs).
pub enum UiEvent {
    /// Run in the page.
    Script(String),
    /// From the page's own title bar.
    Window(WindowAction),
}

pub enum WindowAction {
    Drag,
    Minimize,
    ToggleMaximize,
    Close,
    /// Hide to the tray (Atrium keeps running: OVRServer, streaming, the hardware watch).
    Hide,
    /// Show the window again (tray icon / menu).
    Show,
    /// Quit Atrium for real.
    Quit,
    /// n, s, e, w, ne, nw, se, sw
    Resize(String),
}

#[derive(Default)]
struct State {
    settings: Settings,
    hw: Option<Hardware>,
    rt: Option<RuntimeState>,
    games: Vec<Game>,
    owned: Owned,
    scanning: bool,
    refreshing: bool,
    signing_in: Option<&'static str>,
    cancel: Arc<AtomicBool>,
    /// A game being started (waiting for the headset can take a while); one at a time.
    launching: Option<String>,
    /// Meta builds per app id, fetched when a game's details open (Err = why not).
    versions: HashMap<String, Result<Vec<Build>, String>>,
    /// Meta downloads running now, by app id.
    downloads: HashMap<String, Progress>,
    /// The game Atrium last started, while its process runs (the dash's Now Playing / Quit).
    running: Option<Running>,
}

#[derive(Clone)]
struct Running {
    id: String,
    name: String,
    /// Its executable's file name, lower case (how it's found among processes).
    exe: String,
    start: &'static str,
}

pub struct App {
    state: Mutex<State>,
    proxy: Mutex<tao::event_loop::EventLoopProxy<UiEvent>>,
}

impl App {
    pub fn new(proxy: tao::event_loop::EventLoopProxy<UiEvent>) -> Arc<App> {
        let app = Arc::new(App { state: Mutex::new(State { settings: Settings::load(), ..Default::default() }), proxy: Mutex::new(proxy) });
        app.watch_hardware();
        meta_runtime::watch();
        // The in-VR dashboard: its commands are the window's commands.
        if dash::available() {
            let a = app.clone();
            if let Err(e) = dash::start(move |line| a.command(&line)) {
                sys::log(&e);
            }
        }
        app.rescan();
        app.refresh_owned();
        app
    }

    fn st(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn script(&self, js: String) {
        self.send(UiEvent::Script(js));
    }

    fn send(&self, e: UiEvent) {
        if let Ok(p) = self.proxy.lock() {
            let _ = p.send_event(e);
        }
    }

    /// Send the page everything it draws from.
    pub fn push(&self) {
        let mut snap = self.snapshot();
        self.script(format!("window.atrium && atrium.update({snap})"));
        // The dash gets the same, plus what's running and whether to show home. The dash is always
        // up while Atrium runs (user): home whenever a headset is connected and no game is
        // running or starting, over the game otherwise.
        let s = self.st();
        let headset = s.hw.as_ref().is_some_and(|h| !h.headsets.is_empty() || h.meta.as_ref().is_some_and(|m| m.hmd_present));
        let home = headset && s.running.is_none() && s.launching.is_none();
        snap["running"] = s.running.as_ref().map(|r| json!({ "id": r.id, "name": r.name, "start": r.start })).unwrap_or(Value::Null);
        snap["home"] = json!(home);
        drop(s);
        dash::send(&snap);
    }

    pub fn notice(&self, text: impl AsRef<str>) {
        self.script(format!("window.atrium && atrium.notice({})", Value::from(text.as_ref())));
    }

    fn snapshot(&self) -> Value {
        let s = self.st();
        let set = &s.settings;
        let starts: serde_json::Map<String, Value> = s.games.iter().filter(|g| g.installed).map(|g| (g.id.clone(), json!(launcher::mode(g, set, s.rt.as_ref()).label()))).collect();
        json!({
            "games": s.games,
            "hw": s.hw,
            "rt": s.rt,
            "busy": { "scanning": s.scanning, "refreshing": s.refreshing, "signingIn": s.signing_in, "launching": s.launching },
            "steam": set.steam_account.as_ref().map(|a| json!({ "id": a.steam_id, "name": a.name })),
            "steamKey": set.steam_api_key.is_some(),
            "meta": set.meta_accounts.iter().map(|a| json!({ "id": a.id, "name": a.name, "valid": sys::secret::unseal(&a.token).is_some() })).collect::<Vec<_>>(),
            "localGames": set.local_games.iter().map(|l| json!({ "id": l.id, "name": l.name, "exe": l.exe })).collect::<Vec<_>>(),
            "vrOnly": set.vr_only.unwrap_or(true),
            "autostart": sys::autostart::enabled(),
            "closeAction": set.close_action.clone().unwrap_or_else(|| "ask".into()),
            "mirrorEye": set.mirror_eye.clone().unwrap_or_else(|| "left".into()),
            "ovr": launcher::ovr_dir(),
            "modes": set.game_options.iter().map(|(k, v)| (k.clone(), json!(v.mode))).collect::<serde_json::Map<_, _>>(),
            "starts": starts,
            "errors": s.owned.errors,
            "downloadDir": set.download_dir,
            "versions": s.versions.iter().map(|(k, v)| (k.clone(), match v { Ok(b) => json!({ "builds": b }), Err(e) => json!({ "error": e }) })).collect::<serde_json::Map<_, _>>(),
            "downloads": s.downloads,
        })
    }

    fn spawn(self: &Arc<Self>, f: impl FnOnce(&Arc<App>) + Send + 'static) {
        let app = self.clone();
        std::thread::spawn(move || f(&app));
    }

    fn watch_hardware(self: &Arc<Self>) {
        let mut last_wired: Option<std::time::Instant> = None;
        let mut asked_dash_off = false;
        self.spawn(move |app| loop {
            let hw = headsets::detect();
            let mut st = rt::state(&hw);
            // A wired Rift drops off USB for a moment whenever OVRServer powers it up; don't let
            // one missed pass hand the next game to another backend (Virtual Desktop's service
            // always runs, so it would win).
            if meta_runtime::needed(&hw) {
                last_wired = Some(std::time::Instant::now());
            } else if last_wired.is_some_and(|t| t.elapsed() < Duration::from_secs(30)) {
                if let Some(meta) = st.backends.iter().find(|b| b.id == "meta").cloned() {
                    st.chosen = Some(meta);
                    st.reason = "Meta headset connected".into();
                }
            }
            rt::write_backend(&st);
            // A wired Rift: OVRServer runs inside Atrium for as long as Atrium does.
            if meta_runtime::needed(&hw) && meta_runtime::installed() && !meta_runtime::running() {
                if let Err(e) = meta_runtime::start() {
                    sys::log(&e);
                }
            }
            // ...with Meta's Dash off, so the headset goes to Atrium's dash (home) too. Asked once
            // per run: if Windows says no, games still ask again when they start.
            if meta_runtime::needed(&hw) && meta_runtime::installed() && !meta_runtime::dash_off() && !asked_dash_off {
                asked_dash_off = true;
                match meta_runtime::turn_dash_off() {
                    Ok(()) => dash::restart_home(),
                    Err(e) => app.notice(e),
                }
            }
            // Linux: the Atrium Runtime's own service drives any connected headset.
            if cfg!(target_os = "linux") && !hw.headsets.is_empty() && atrium_service::available() && !atrium_service::running() {
                if let Err(e) = atrium_service::start() {
                    sys::log(&e);
                }
            }
            {
                let mut s = app.st();
                s.hw = Some(hw);
                s.rt = Some(st);
                // The game Atrium started has ended: the dash goes back to home.
                if s.running.as_ref().is_some_and(|r| !sys::processes().contains(&r.exe)) {
                    s.running = None;
                }
            }
            app.push();
            std::thread::sleep(Duration::from_secs(5));
        });
    }

    pub fn rescan(self: &Arc<Self>) {
        {
            let mut s = self.st();
            if s.scanning {
                return;
            }
            s.scanning = true;
        }
        self.spawn(|app| {
            let (set, owned) = {
                let s = app.st();
                (s.settings.clone(), s.owned.clone())
            };
            let games = lib::scan(&set, &owned);
            {
                let mut s = app.st();
                s.games = games;
                s.scanning = false;
            }
            app.push();
        });
    }

    pub fn refresh_owned(self: &Arc<Self>) {
        let mut set = {
            let mut s = self.st();
            if s.refreshing {
                return;
            }
            s.refreshing = true;
            s.settings.clone()
        };
        self.push();
        self.spawn(move |app| {
            let owned = lib::refresh_owned(&mut set);
            let err = owned.errors.first().cloned();
            {
                let mut s = app.st();
                // Only the caches changed on the copy; everything else may have moved on since.
                s.settings.steam_vr_cache = set.steam_vr_cache;
                s.settings.meta_name_cache = set.meta_name_cache;
                s.settings.save();
                s.owned = owned;
                s.refreshing = false;
            }
            if let Some(e) = err {
                app.notice(e);
            }
            app.rescan();
        });
    }

    fn sign_in(self: &Arc<Self>, which: &'static str) {
        let cancel = {
            let mut s = self.st();
            if s.signing_in.is_some() {
                return;
            }
            s.signing_in = Some(which);
            s.cancel = Arc::new(AtomicBool::new(false));
            s.cancel.clone()
        };
        self.push();
        self.spawn(move |app| {
            let msg = if which == "steam" {
                login::steam(&cancel).map(|a| {
                    let m = format!("Signed in to Steam as {}", a.name);
                    let mut s = app.st();
                    s.settings.steam_account = Some(a);
                    s.settings.save();
                    m
                })
            } else {
                login::meta(&cancel).map(|a| {
                    let m = format!("Signed in to Meta as {}", a.name);
                    let mut s = app.st();
                    login::store_meta(&mut s.settings, a);
                    s.settings.save();
                    m
                })
            };
            app.st().signing_in = None;
            match msg {
                Ok(m) => {
                    app.notice(m);
                    app.refresh_owned();
                }
                Err(e) => {
                    app.notice(e);
                    app.push();
                }
            }
        });
    }

    /// Signed-in Meta accounts' tokens, the ones that own `app_id` first.
    fn meta_tokens(&self, app_id: &str) -> Vec<String> {
        let s = self.st();
        let owns = s.owned.meta.iter().any(|(id, ..)| id == app_id);
        let mut t: Vec<String> = s.settings.meta_accounts.iter().filter_map(|a| sys::secret::unseal(&a.token)).collect();
        // `owned` is merged across accounts, so it can't say which one; with one owner it's moot.
        if !owns {
            t.truncate(1);
        }
        t
    }

    /// Every version Meta serves for a game (for the details panel's picker).
    fn load_versions(self: &Arc<Self>, app_id: String) {
        if self.st().versions.get(&app_id).is_some_and(|v| v.is_ok()) {
            return;
        }
        self.spawn(move |app| {
            let tokens = app.meta_tokens(&app_id);
            let mut r = Err("Sign in to Meta (Accounts) to download".to_string());
            for t in &tokens {
                r = dl::builds(t, &app_id).map(|(_, b)| b);
                if r.is_ok() {
                    break;
                }
            }
            app.st().versions.insert(app_id, r);
            app.push();
        });
    }

    /// Download one build of a Meta game into its own folder (<downloads>\<game>\<version>),
    /// next to any other versions; pieces those already have are copied, not fetched. The new
    /// version becomes the one Play starts.
    fn start_download(self: &Arc<Self>, app_id: String, binary_id: String) {
        let Some(build) = self.st().versions.get(&app_id).and_then(|v| v.as_ref().ok()).and_then(|b| b.iter().find(|b| b.binary_id == binary_id).cloned()) else { return };
        if self.st().downloads.contains_key(&app_id) {
            return;
        }
        let Some(root) = self.download_dir() else { return };
        let name = self.game(&format!("meta:{app_id}")).map(|g| g.name).unwrap_or_else(|| app_id.clone());
        let cancel = Arc::new(AtomicBool::new(false));
        self.st().downloads.insert(app_id.clone(), Progress { version: build.version.clone(), phase: "Starting".into(), cancel: cancel.clone(), ..Default::default() });
        self.push();
        self.spawn(move |app| {
            let (dir, seeds) = {
                let s = app.st();
                let i = s.settings.meta_installs.get(&app_id);
                // The game's folder: where its other versions are, else a new one.
                let game_dir = i.and_then(|i| i.versions.values().next()).and_then(|v| PathBuf::from(&v.dir).parent().map(PathBuf::from)).unwrap_or_else(|| root.join(folder_name(&name, &app_id)));
                let seeds: Vec<PathBuf> = i.map(|i| i.versions.iter().filter(|(k, _)| **k != build.version).map(|(_, v)| PathBuf::from(&v.dir)).collect()).unwrap_or_default();
                (game_dir.join(folder_name(&build.version, &build.binary_id)), seeds)
            };
            let last_push = Mutex::new(std::time::Instant::now());
            let progress = |phase: &str, done: u64, total: u64| {
                if let Some(p) = app.st().downloads.get_mut(&app_id) {
                    p.phase = phase.to_string();
                    p.done = done;
                    p.total = total;
                }
                let mut t = last_push.lock().unwrap_or_else(|e| e.into_inner());
                if t.elapsed() > Duration::from_millis(400) {
                    *t = std::time::Instant::now();
                    app.push();
                }
            };
            let mut r = Err("Sign in to Meta (Accounts) to download".to_string());
            for t in app.meta_tokens(&app_id) {
                r = dl::download(&t, &build.binary_id, &dir, &seeds, &cancel, &progress);
                if r.as_ref().err().is_none_or(|e| !e.contains("refused")) {
                    break;
                }
            }
            app.st().downloads.remove(&app_id);
            match r {
                Ok(i) => {
                    {
                        let mut s = app.st();
                        let e = s.settings.meta_installs.entry(app_id.clone()).or_insert_with(|| MetaInstall { name: name.clone(), ..Default::default() });
                        e.canonical = i.canonical;
                        e.active = build.version.clone();
                        e.versions.insert(build.version.clone(), MetaVersion {
                            dir: dir.to_string_lossy().into_owned(),
                            launch_file: i.launch_file,
                            launch_args: i.launch_args,
                            version_code: build.version_code,
                            binary_id: build.binary_id.clone(),
                        });
                        s.settings.save();
                    }
                    sys::log(&format!("downloaded meta:{app_id} {} to {}", build.version, dir.display()));
                    app.notice(format!("{name} {} is ready to play", build.version));
                }
                Err(e) if e == "cancelled" => app.notice(format!("Stopped downloading {name} {}. Download it again to carry on where it stopped.", build.version)),
                Err(e) => {
                    sys::log(&format!("download meta:{app_id} failed: {e}"));
                    app.notice(format!("{name}: {e}"));
                }
            }
            app.rescan();
        });
    }

    /// The downloads folder; asks for one the first time.
    fn download_dir(&self) -> Option<PathBuf> {
        if let Some(d) = self.st().settings.download_dir.clone() {
            return Some(PathBuf::from(d));
        }
        let d = rfd::FileDialog::new().set_title("Where should Atrium save downloaded games?").pick_folder()?;
        let mut s = self.st();
        s.settings.download_dir = Some(d.to_string_lossy().into_owned());
        s.settings.save();
        Some(d)
    }

    fn game(&self, id: &str) -> Option<Game> {
        self.st().games.iter().find(|g| g.id == id).cloned()
    }

    /// A message from the page: `{"cmd": "...", ...}`.
    pub fn command(self: &Arc<Self>, raw: &str) {
        let Ok(m) = serde_json::from_str::<Value>(raw) else { return };
        let arg = |k: &str| m[k].as_str().unwrap_or_default().to_string();
        match m["cmd"].as_str().unwrap_or_default() {
            "ready" => self.push(),
            "window" => self.send(UiEvent::Window(match arg("action").as_str() {
                "drag" => WindowAction::Drag,
                "minimize" => WindowAction::Minimize,
                "maximize" => WindowAction::ToggleMaximize,
                "close" => WindowAction::Close,
                "hide" => WindowAction::Hide,
                "quit" => WindowAction::Quit,
                "resize" => WindowAction::Resize(arg("edge")),
                _ => return,
            })),
            "refresh" => self.refresh_owned(),
            // From the dash: close the running game (asked nicely, then ended after 5 s).
            "quitGame" => {
                let Some(r) = self.st().running.clone() else { return };
                self.spawn(move |app| {
                    let _ = sys::output("taskkill.exe", &["/IM", &r.exe]);
                    for _ in 0..10 {
                        std::thread::sleep(Duration::from_millis(500));
                        if !sys::processes().contains(&r.exe) {
                            break;
                        }
                    }
                    if sys::processes().contains(&r.exe) {
                        let _ = sys::output("taskkill.exe", &["/F", "/IM", &r.exe]);
                    }
                    app.st().running = None;
                    app.notice(format!("Closed {}", r.name));
                    app.push();
                });
            }
            // From the dash: Reset view (Meta's runtime: its own recenter).
            "recenter" => self.spawn(|app| {
                let ok = sys::native_tool("ovrprobe.exe").is_some_and(|p| sys::output(p, &["recenter"]).contains("recenter: 0"));
                if !ok {
                    app.notice("Couldn't reset the view");
                }
            }),
            "dashClosed" => {}
            "launch" => {
                let Some(g) = self.game(&arg("id")) else { return };
                // One launch at a time, and never a second copy of a running game.
                if let Some(other) = self.st().launching.clone() {
                    self.notice(format!("Already starting {other}"));
                    return;
                }
                let exe_name = g.exe.as_ref().and_then(|p| p.file_name()).map(|n| n.to_string_lossy().to_lowercase());
                if g.installed && exe_name.is_some_and(|n| sys::processes().contains(&n)) {
                    self.notice(format!("{} is already running", g.name));
                    return;
                }
                self.st().launching = Some(g.name.clone());
                self.push();
                self.spawn(move |app| {
                    let r = (|| {
                        // A wired Rift needs OVRServer up and taking games (and Dash off) first.
                        if g.installed && cfg!(target_os = "linux") && atrium_service::available() {
                            atrium_service::prepare()?;
                        }
                        let wired = cfg!(windows) && app.st().hw.as_ref().is_some_and(meta_runtime::needed);
                        if g.installed && wired {
                            let starting = !meta_runtime::running();
                            if starting {
                                app.notice("Starting the headset…");
                            }
                            meta_runtime::prepare()?;
                            // Starting OVRServer powers the Rift up and it drops off USB for a
                            // moment; a hardware pass in that gap picks the wrong backend. Pick
                            // again now that Meta's runtime reports the headset (only needed when
                            // it was just started: otherwise the hardware loop's state is current).
                            if starting {
                                let hw = headsets::detect();
                                let st = rt::state(&hw);
                                let mut s = app.st();
                                s.hw = Some(hw);
                                s.rt = Some(st);
                            }
                        }
                        let mut guard = app.st();
                        let s = &mut *guard;
                        launcher::launch(&g, &mut s.settings, s.rt.as_ref())
                    })();
                    app.st().launching = None;
                    match r {
                        Ok(()) => {
                            app.notice(if g.installed { format!("Starting {}", g.name) } else { format!("Opening {} in its store", g.name) });
                            if let (true, Some(exe)) = (g.installed, g.exe.as_ref().and_then(|p| p.file_name())) {
                                let start = { let s = app.st(); launcher::mode(&g, &s.settings, s.rt.as_ref()).label() };
                                app.st().running = Some(Running { id: g.id.clone(), name: g.name.clone(), exe: exe.to_string_lossy().to_lowercase(), start });
                            }
                            app.rescan();
                        }
                        Err(e) => {
                            app.notice(e);
                            app.push();
                        }
                    }
                });
            }
            "setMode" => {
                let (id, mode) = (arg("id"), arg("mode"));
                let mut s = self.st();
                if mode == "auto" {
                    s.settings.game_options.remove(&id);
                } else if ["steam", "direct", "translate"].contains(&mode.as_str()) {
                    s.settings.game_options.insert(id, GameOptions { mode });
                }
                s.settings.save();
                drop(s);
                self.push();
            }
            "setVrOnly" => {
                let mut s = self.st();
                s.settings.vr_only = Some(m["on"].as_bool().unwrap_or(true));
                s.settings.save();
                drop(s);
                self.push();
            }
            "installAtrium" | "openAtrium" => {
                let (serial, install) = (arg("serial"), m["cmd"] == "installAtrium");
                self.spawn(move |app| {
                    let r = if install { standalone::install(&serial) } else { standalone::open(&serial) };
                    app.notice(match r {
                        Ok(()) if install => "Atrium is installed on the headset".to_string(),
                        Ok(()) => "Opening Atrium on the headset".to_string(),
                        Err(e) => e,
                    });
                });
            }
            "setCloseAction" => {
                let v = arg("value");
                let mut s = self.st();
                s.settings.close_action = matches!(v.as_str(), "tray" | "quit").then_some(v);
                s.settings.save();
                drop(s);
                self.push();
            }
            "setMirrorEye" => {
                let v = arg("value");
                let mut s = self.st();
                s.settings.mirror_eye = (v == "right").then_some(v);
                s.settings.save();
                drop(s);
                self.push();
            }
            "setAutostart" => {
                if !sys::autostart::set(m["on"].as_bool().unwrap_or(false)) {
                    self.notice("Couldn't change the startup setting");
                }
                self.push();
            }
            "metaVersions" => self.load_versions(arg("appId")),
            "download" => {
                let (app_id, binary) = (arg("appId"), arg("binary"));
                // May ask for the downloads folder first: not on the UI thread.
                self.spawn(move |app| app.start_download(app_id, binary));
            }
            "cancelDownload" => {
                if let Some(p) = self.st().downloads.get(&arg("appId")) {
                    p.cancel.store(true, Ordering::Relaxed);
                }
            }
            "useVersion" => {
                let (app_id, version) = (arg("appId"), arg("version"));
                let mut s = self.st();
                let Some(i) = s.settings.meta_installs.get_mut(&app_id) else { return };
                if !i.versions.contains_key(&version) {
                    return;
                }
                i.active = version.clone();
                let name = i.name.clone();
                s.settings.save();
                drop(s);
                self.notice(format!("{name} will start version {version}"));
                self.rescan();
            }
            "removeDownload" => {
                let (app_id, version) = (arg("appId"), arg("version"));
                let Some(v) = self.st().settings.meta_installs.get(&app_id).and_then(|i| i.versions.get(&version)).cloned() else { return };
                if self.st().downloads.contains_key(&app_id) {
                    return;
                }
                self.spawn(move |app| {
                    let dir = PathBuf::from(&v.dir);
                    // Only a folder Atrium filled (its manifest is there), never a guess.
                    if dir.join("atrium_manifest.json").exists() {
                        if let Err(e) = std::fs::remove_dir_all(&dir) {
                            return app.notice(format!("Couldn't remove {}: {e}", dir.display()));
                        }
                    }
                    let name = {
                        let mut s = app.st();
                        let Some(i) = s.settings.meta_installs.get_mut(&app_id) else { return };
                        i.versions.remove(&version);
                        // Removing the version Play starts: the newest left takes over.
                        if i.active == version {
                            i.active = i.sorted().first().map(|(k, _)| (*k).clone()).unwrap_or_default();
                        }
                        let name = i.name.clone();
                        if i.versions.is_empty() {
                            s.settings.meta_installs.remove(&app_id);
                            // The game's folder too, once it's empty.
                            if let Some(p) = dir.parent() {
                                let _ = std::fs::remove_dir(p);
                            }
                        }
                        s.settings.save();
                        name
                    };
                    app.notice(format!("Removed {name} {version}"));
                    app.rescan();
                });
            }
            "pickDownloadDir" => self.spawn(|app| {
                let Some(d) = rfd::FileDialog::new().set_title("Where should Atrium save downloaded games?").pick_folder() else { return };
                {
                    let mut s = app.st();
                    s.settings.download_dir = Some(d.to_string_lossy().into_owned());
                    s.settings.save();
                }
                app.push();
            }),
            "signIn" => self.sign_in(if arg("which") == "steam" { "steam" } else { "meta" }),
            "cancelSignIn" => self.st().cancel.store(true, Ordering::Relaxed),
            "signOut" => {
                let mut s = self.st();
                if arg("which") == "steam" {
                    s.settings.steam_account = None;
                    s.owned.steam.clear();
                } else {
                    let id = arg("id");
                    s.settings.meta_accounts.retain(|a| a.id != id);
                }
                s.settings.save();
                drop(s);
                self.refresh_owned();
            }
            "saveApiKey" => {
                let key = arg("key").trim().to_string();
                if key.len() != 32 || !key.chars().all(|c| c.is_ascii_hexdigit()) {
                    return self.notice("That doesn't look like a Steam Web API key (32 letters and numbers)");
                }
                let mut s = self.st();
                s.settings.steam_api_key = sys::secret::seal(&key);
                s.settings.save();
                drop(s);
                self.refresh_owned();
            }
            "removeApiKey" => {
                let mut s = self.st();
                s.settings.steam_api_key = None;
                s.settings.save();
                drop(s);
                self.push();
            }
            "addGame" => self.spawn(|app| {
                let mut d = rfd::FileDialog::new().set_title("Pick the game's executable");
                if cfg!(windows) {
                    d = d.add_filter("Programs", &["exe"]);
                }
                let Some(p) = d.pick_file() else { return };
                let name = p.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
                {
                    let mut s = app.st();
                    s.settings.local_games.push(LocalGame { id: format!("{:x}", sys::now_ms()), name, exe: p.to_string_lossy().into_owned(), args: String::new() });
                    s.settings.save();
                }
                app.rescan();
            }),
            "removeGame" => {
                let id = arg("id");
                let mut s = self.st();
                s.settings.local_games.retain(|l| format!("local:{}", l.id) != id);
                s.settings.save();
                drop(s);
                self.rescan();
            }
            "openFolder" => {
                if let Some(d) = self.game(&arg("id")).and_then(|g| g.install_dir).filter(|d| d.exists()) {
                    let _ = open::that(d);
                }
            }
            "openUrl" => {
                let u = arg("url");
                if u.starts_with("https://") {
                    let _ = open::that(u);
                }
            }
            "startSim" => self.spawn(|app| match headsets::start_sim() {
                Ok(()) => app.notice("Starting the simulated Steam Frame"),
                Err(e) => app.notice(e),
            }),
            "registerRuntime" => self.spawn(|app| match rt::register() {
                Ok(()) => app.notice("Atrium is now the OpenXR runtime"),
                Err(e) => app.notice(e),
            }),
            other => sys::log(&format!("ui: unknown command {other}")),
        }
    }
}

/// A folder name for a downloaded game: its name without characters Windows won't take.
fn folder_name(name: &str, app_id: &str) -> String {
    let n: String = name.chars().map(|c| if c.is_alphanumeric() || " -_.()'&".contains(c) { c } else { ' ' }).collect();
    let n = n.split_whitespace().collect::<Vec<_>>().join(" ").trim_end_matches('.').to_string();
    if n.is_empty() { app_id.to_string() } else { n }
}
