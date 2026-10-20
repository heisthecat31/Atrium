//! Atrium's state and the commands the page sends. The page (web/) holds no state of its own:
//! after every change Rust pushes a full snapshot (`atrium.update(...)`) and the page redraws.
//! Slow work (USB/Meta probing, Steam/Meta requests, sign-in, file dialogs) runs on threads.
use crate::login;
use crate::services::{headsets::{self, Hardware}, launcher, library::{self as lib, Game, Owned}, meta_runtime, runtime::{self as rt, RuntimeState}};
use crate::sys::{self, settings::{GameOptions, LocalGame, Settings}};
use serde_json::{json, Value};
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
        let snap = self.snapshot();
        self.script(format!("window.atrium && atrium.update({snap})"));
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
            "ovr": launcher::ovr_dir(),
            "modes": set.game_options.iter().map(|(k, v)| (k.clone(), json!(v.mode))).collect::<serde_json::Map<_, _>>(),
            "starts": starts,
            "errors": s.owned.errors,
        })
    }

    fn spawn(self: &Arc<Self>, f: impl FnOnce(&Arc<App>) + Send + 'static) {
        let app = self.clone();
        std::thread::spawn(move || f(&app));
    }

    fn watch_hardware(self: &Arc<Self>) {
        self.spawn(|app| loop {
            let hw = headsets::detect();
            let st = rt::state(&hw);
            rt::write_backend(&st);
            // A wired Rift: OVRServer runs inside Atrium for as long as Atrium does.
            if meta_runtime::needed(&hw) && meta_runtime::installed() && !meta_runtime::running() {
                if let Err(e) = meta_runtime::start() {
                    sys::log(&e);
                }
            }
            {
                let mut s = app.st();
                s.hw = Some(hw);
                s.rt = Some(st);
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
                "resize" => WindowAction::Resize(arg("edge")),
                _ => return,
            })),
            "refresh" => self.refresh_owned(),
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
                        let wired = app.st().hw.as_ref().is_some_and(meta_runtime::needed);
                        if g.installed && wired {
                            if !meta_runtime::running() {
                                app.notice("Starting the headset…");
                            }
                            meta_runtime::prepare()?;
                            // Starting OVRServer powers the Rift up and it drops off USB for a
                            // moment; a hardware pass in that gap picks the wrong backend. Pick
                            // again now that Meta's runtime reports the headset.
                            let hw = headsets::detect();
                            let st = rt::state(&hw);
                            let mut s = app.st();
                            s.hw = Some(hw);
                            s.rt = Some(st);
                        }
                        let mut guard = app.st();
                        let s = &mut *guard;
                        launcher::launch(&g, &mut s.settings, s.rt.as_ref())
                    })();
                    app.st().launching = None;
                    match r {
                        Ok(()) => {
                            app.notice(if g.installed { format!("Starting {}", g.name) } else { format!("Opening {} in its store", g.name) });
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
            "setAutostart" => {
                if !sys::autostart::set(m["on"].as_bool().unwrap_or(false)) {
                    self.notice("Couldn't change the startup setting");
                }
                self.push();
            }
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
