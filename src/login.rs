//! Sign-in.
//!
//! Steam: OpenID 2.0 in the system browser, returning to a one-shot listener on 127.0.0.1, then
//! verified with Steam (check_authentication) before the Steam ID is trusted.
//!
//! Meta: the native-SSO flow in services/meta.rs. The Meta page finishes by navigating to an
//! oculus:// URL. Atrium shows the page in its own webview window and catches that navigation
//! there, so Atrium never registers as the system's oculus:// handler (Meta's app owns it).
//! The webview runs in a child process (`atrium --login-meta`): tao's event loop must own the main
//! thread, and the app's main thread already belongs to eframe. Protocol with the child:
//!   parent -> stdin:  the Meta sign-in URL, one line
//!   child  -> stdout: `callback <oculus://...>` once, then exits; no line = cancelled
//! The request token never leaves the parent.
use crate::services::meta;
use crate::sys::{self, settings::{MetaAccount, SteamAccount}};
use std::io::{BufRead, BufReader, Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

const SIGN_IN_TIMEOUT: Duration = Duration::from_secs(600);
const STEAM_OPENID: &str = "https://steamcommunity.com/openid/login";
const OPENID_NS: &str = "http://specs.openid.net/auth/2.0";
const OPENID_SELECT: &str = "http://specs.openid.net/auth/2.0/identifier_select";

fn page(title: &str, body: &str) -> String {
    format!(
        "<!doctype html><meta charset=utf-8><title>{title}</title><body style=\"font:16px system-ui;background:#0f1117;color:#e8eaf0;display:grid;place-items:center;height:90vh\"><div style=text-align:center><h2>{title}</h2><p>{body}</p></div>"
    )
}

// ---- Steam ----------------------------------------------------------------------------------
/// Blocks until the browser comes back, the user cancels, or ten minutes pass.
pub fn steam(cancel: &AtomicBool) -> Result<SteamAccount, String> {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").map_err(|e| format!("Couldn't open a local port: {e}"))?;
    listener.set_nonblocking(true).map_err(|e| e.to_string())?;
    let port = listener.local_addr().map_err(|e| e.to_string())?.port();
    let nonce = format!("{:032x}", rand::random::<u128>());
    let return_to = format!("http://127.0.0.1:{port}/steam?state={nonce}");
    let realm = format!("http://127.0.0.1:{port}/");
    let q = url::form_urlencoded::Serializer::new(String::new())
        .append_pair("openid.ns", OPENID_NS)
        .append_pair("openid.mode", "checkid_setup")
        .append_pair("openid.return_to", &return_to)
        .append_pair("openid.realm", &realm)
        .append_pair("openid.identity", OPENID_SELECT)
        .append_pair("openid.claimed_id", OPENID_SELECT)
        .finish();
    open::that(format!("{STEAM_OPENID}?{q}")).map_err(|e| format!("Couldn't open the browser: {e}"))?;

    let start = Instant::now();
    loop {
        if cancel.load(Ordering::Relaxed) {
            return Err("Sign-in cancelled".into());
        }
        if start.elapsed() > SIGN_IN_TIMEOUT {
            return Err("Sign-in timed out".into());
        }
        let (mut stream, _) = match listener.accept() {
            Ok(s) => s,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(150));
                continue;
            }
            Err(e) => return Err(e.to_string()),
        };
        let _ = stream.set_nonblocking(false);
        let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
        let Some(target) = read_request_target(&mut stream) else { continue };
        let Ok(u) = url::Url::parse(&format!("http://127.0.0.1{target}")) else { continue };
        if u.path() != "/steam" {
            respond(&mut stream, "404 Not Found", "");
            continue;
        }
        let params: Vec<(String, String)> = u.query_pairs().into_owned().collect();
        let result = verify_steam(&params, &nonce, &return_to);
        match &result {
            Ok(_) => respond(&mut stream, "200 OK", &page("Signed in to Steam", "You can close this tab and go back to Atrium.")),
            Err(e) => respond(&mut stream, "200 OK", &page("Steam sign-in failed", &html_escape(e))),
        }
        // A stale or forged return doesn't end the wait; a real answer from Steam does.
        if result.is_ok() || params.iter().any(|(k, v)| k == "state" && v == &nonce) {
            let steam_id = result?;
            let name = crate::services::steam::profile_name(&steam_id).unwrap_or_default();
            return Ok(SteamAccount { steam_id, name });
        }
    }
}

fn verify_steam(params: &[(String, String)], nonce: &str, return_to: &str) -> Result<String, String> {
    let get = |k: &str| params.iter().find(|(n, _)| n == k).map(|(_, v)| v.as_str());
    if get("state") != Some(nonce) {
        return Err("This sign-in wasn't started by Atrium".into());
    }
    match get("openid.mode") {
        Some("id_res") => {}
        Some("cancel") => return Err("Sign-in cancelled".into()),
        _ => return Err("Steam returned an unexpected answer".into()),
    }
    if get("openid.op_endpoint") != Some(STEAM_OPENID) || get("openid.return_to") != Some(return_to) {
        return Err("Steam's answer doesn't match this sign-in".into());
    }
    let claimed = get("openid.claimed_id").unwrap_or_default();
    let steam_id = claimed.strip_prefix("https://steamcommunity.com/openid/id/").filter(|id| id.len() == 17 && id.bytes().all(|b| b.is_ascii_digit())).ok_or("Steam returned no account")?;
    // Ask Steam to confirm the signature; only openid.* fields go back.
    let mut form: Vec<(&str, &str)> = params.iter().filter(|(k, _)| k.starts_with("openid.") && k != "openid.mode").map(|(k, v)| (k.as_str(), v.as_str())).collect();
    form.push(("openid.mode", "check_authentication"));
    let body = ureq::post(STEAM_OPENID).timeout(Duration::from_secs(20)).send_form(&form).map_err(|e| format!("Couldn't reach Steam: {e}"))?.into_string().map_err(|e| e.to_string())?;
    if !body.lines().any(|l| l.trim() == "is_valid:true") {
        return Err("Steam didn't confirm the sign-in".into());
    }
    Ok(steam_id.to_string())
}

/// "/path?query" from the request line of a GET.
fn read_request_target(stream: &mut std::net::TcpStream) -> Option<String> {
    let mut buf = vec![0u8; 8192];
    let mut len = 0;
    while len < buf.len() {
        let n = stream.read(&mut buf[len..]).ok()?;
        if n == 0 {
            break;
        }
        len += n;
        if buf[..len].windows(4).any(|w| w == b"\r\n\r\n") {
            break;
        }
    }
    let head = String::from_utf8_lossy(&buf[..len]);
    let mut parts = head.lines().next()?.split_whitespace();
    (parts.next()? == "GET").then_some(())?;
    parts.next().filter(|t| t.starts_with('/')).map(str::to_string)
}

fn respond(stream: &mut std::net::TcpStream, status: &str, html: &str) {
    let _ = write!(stream, "HTTP/1.1 {status}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n{html}", html.len());
    let _ = stream.flush();
}

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

// ---- Meta (parent side) ---------------------------------------------------------------------
/// Shows Meta's sign-in window and blocks until it finishes. Returns the account with its Oculus
/// profile token sealed for this user.
pub fn meta(cancel: &AtomicBool) -> Result<MetaAccount, String> {
    let req = meta::begin_login()?;
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let mut child = std::process::Command::new(exe)
        .arg("--login-meta")
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null())
        .spawn()
        .map_err(|e| format!("Couldn't open the sign-in window: {e}"))?;
    if let Some(mut stdin) = child.stdin.take() {
        let _ = writeln!(stdin, "{}", req.url);
    }
    let stdout = child.stdout.take().ok_or("sign-in window has no output")?;
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Some(cb) = line.strip_prefix("callback ") {
                let _ = tx.send(cb.trim().to_string());
                return;
            }
        }
    });
    let start = Instant::now();
    let callback = loop {
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(cb) => break cb,
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                let _ = child.wait();
                return Err("Sign-in cancelled".into());
            }
            Err(_) if cancel.load(Ordering::Relaxed) || start.elapsed() > SIGN_IN_TIMEOUT => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(if cancel.load(Ordering::Relaxed) { "Sign-in cancelled" } else { "Sign-in timed out" }.into());
            }
            Err(_) => {}
        }
    };
    let _ = child.wait();
    let token = meta::complete_login(&req.request_token, &callback)?;
    let (id, name) = meta::identity(&token).ok_or("Signed in, but Meta didn't say which account")?;
    let sealed = sys::secret::seal(&token).ok_or("Couldn't store the Meta token securely")?;
    sys::log(&format!("meta sign-in ok: {id}"));
    Ok(MetaAccount { id, name, token: sealed })
}

/// The account's usable token, or None if it can't be unsealed (other Windows user, new PC).
pub fn meta_token(account: &MetaAccount) -> Option<String> {
    sys::secret::unseal(&account.token)
}

/// Adds the account, or replaces the one with the same id (a re-sign-in refreshes its token).
pub fn store_meta(settings: &mut sys::settings::Settings, account: MetaAccount) {
    settings.meta_accounts.retain(|a| a.id != account.id);
    settings.meta_accounts.push(account);
}

// ---- Meta (child: `atrium --login-meta`) ----------------------------------------------------
fn is_meta_callback(url: &str) -> bool {
    let l = url.trim_start().to_ascii_lowercase();
    l.starts_with("oculus://") || l.starts_with("oculus-client://")
}

fn window_icon() -> Option<tao::window::Icon> {
    let img = image::load_from_memory(include_bytes!("../assets/atrium.png")).ok()?.into_rgba8();
    let (w, h) = img.dimensions();
    tao::window::Icon::from_rgba(img.into_raw(), w, h).ok()
}

/// Entry point of the sign-in window process. Never returns.
pub fn meta_window() -> ! {
    use tao::dpi::LogicalSize;
    use tao::event::{Event, WindowEvent};
    use tao::event_loop::{ControlFlow, EventLoopBuilder};
    use tao::window::WindowBuilder;
    use wry::{NewWindowResponse, WebContext, WebViewBuilder};

    let mut url = String::new();
    let _ = std::io::stdin().read_line(&mut url);
    let url = url.trim().to_string();
    if !url.starts_with("https://auth.meta.com/") {
        std::process::exit(2);
    }

    let event_loop = EventLoopBuilder::<String>::with_user_event().build();
    let window = match WindowBuilder::new().with_title("Sign in to Meta · Atrium").with_inner_size(LogicalSize::new(500.0, 760.0)).with_window_icon(window_icon()).build(&event_loop) {
        Ok(w) => w,
        Err(e) => {
            sys::log(&format!("meta sign-in window: {e}"));
            std::process::exit(3);
        }
    };

    let nav_proxy = event_loop.create_proxy();
    let popup_proxy = std::sync::Mutex::new(event_loop.create_proxy());
    // Fresh profile each time so a second Meta account isn't signed in silently as the first.
    let mut ctx = WebContext::new(Some(sys::data_dir().join("webview")));
    let builder = WebViewBuilder::new_with_web_context(&mut ctx)
        .with_url(&url)
        .with_incognito(true)
        .with_navigation_handler(move |u| {
            if is_meta_callback(&u) {
                let _ = nav_proxy.send_event(u);
                return false;
            }
            true
        })
        .with_new_window_req_handler(move |u, _| {
            if is_meta_callback(&u) {
                if let Ok(p) = popup_proxy.lock() {
                    let _ = p.send_event(u);
                }
                return NewWindowResponse::Deny;
            }
            NewWindowResponse::Allow
        });

    #[cfg(not(target_os = "linux"))]
    let webview = builder.build(&window);
    #[cfg(target_os = "linux")]
    let webview = {
        use tao::platform::unix::WindowExtUnix;
        use wry::WebViewBuilderExtUnix;
        builder.build_gtk(window.default_vbox().expect("tao window has a vbox"))
    };
    let webview = match webview {
        Ok(w) => w,
        Err(e) => {
            sys::log(&format!("meta sign-in webview: {e}"));
            std::process::exit(4);
        }
    };

    event_loop.run(move |event, _, flow| {
        *flow = ControlFlow::Wait;
        let _keep = (&webview, &window);
        match event {
            Event::UserEvent(cb) => {
                let mut out = std::io::stdout().lock();
                let _ = writeln!(out, "callback {cb}");
                let _ = out.flush();
                *flow = ControlFlow::Exit;
            }
            Event::WindowEvent { event: WindowEvent::CloseRequested, .. } => *flow = ControlFlow::Exit,
            _ => {}
        }
    })
}
