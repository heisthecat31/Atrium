//! The Atrium window: a tao window with the system webview (WebView2 on Windows, WebKitGTK on
//! Linux) showing the page in web/. No browser engine ships with Atrium.
//!
//! The page is served from Atrium's own `atrium` protocol (http://atrium.localhost/ on Windows,
//! atrium://localhost/ elsewhere), compiled into the exe. Meta's local store art is served at
//! /art?p=<path>, limited to Meta's StoreAssets folder. The page talks to Rust with
//! `ipc.postMessage(json)`; Rust answers by running `atrium.update(...)` in the page (app.rs).
//!
//! The window has no system title bar (like Meta's app): the page draws its own top bar with
//! minimise / maximise / close, and asks Rust to move or resize the window (`UiEvent::Window`).
//!
//! Closing asks "Close to tray instead?" (or does what the user chose in Settings). In the tray
//! Atrium keeps running: OVRServer for a wired Rift, the streaming service, the hardware watch.
//! The tray icon opens the window again; its menu can quit. `atrium --tray` starts in the tray.
use crate::app::{App, UiEvent, WindowAction};
use crate::sys;
use std::borrow::Cow;
use std::path::PathBuf;
use tao::dpi::LogicalSize;
use tao::event::{Event, WindowEvent};
use tao::event_loop::{ControlFlow, EventLoopBuilder};
use tao::window::{ResizeDirection, WindowBuilder};
use wry::http::{header::CONTENT_TYPE, Request, Response, StatusCode};
use wry::{WebContext, WebViewBuilder};

const START_URL: &str = if cfg!(windows) { "http://atrium.localhost/" } else { "atrium://localhost/" };

fn page_file(path: &str) -> Option<(&'static [u8], &'static str)> {
    Some(match path {
        "/" | "/index.html" => (include_bytes!("../web/index.html"), "text/html; charset=utf-8"),
        "/app.css" => (include_bytes!("../web/app.css"), "text/css; charset=utf-8"),
        "/app.js" => (include_bytes!("../web/app.js"), "text/javascript; charset=utf-8"),
        "/icons.js" => (include_bytes!("../web/icons.js"), "text/javascript; charset=utf-8"),
        "/logo.png" => (include_bytes!("../assets/atrium.png"), "image/png"),
        _ => return None,
    })
}

/// A local art file, only from folders Atrium reads art from.
fn art(query: &str) -> Option<(Vec<u8>, &'static str)> {
    let p = url::form_urlencoded::parse(query.as_bytes()).find(|(k, _)| k == "p").map(|(_, v)| PathBuf::from(v.into_owned()))?;
    let p = std::fs::canonicalize(p).ok()?;
    let allowed = [crate::services::meta::STORE_ASSETS].iter().filter_map(|d| std::fs::canonicalize(d).ok()).any(|d| p.starts_with(d));
    if !allowed {
        return None;
    }
    let ext = p.extension()?.to_string_lossy().to_lowercase();
    let mime = match ext.as_str() {
        "jpg" | "jpeg" => "image/jpeg",
        "png" => "image/png",
        "webp" => "image/webp",
        _ => return None,
    };
    Some((std::fs::read(p).ok()?, mime))
}

fn serve(req: Request<Vec<u8>>) -> Response<Cow<'static, [u8]>> {
    let uri = req.uri();
    let (body, mime, cache): (Cow<'static, [u8]>, &str, &str) = if let Some((b, m)) = page_file(uri.path()) {
        (Cow::Borrowed(b), m, "no-store")
    } else if uri.path() == "/art" {
        match art(uri.query().unwrap_or_default()) {
            Some((b, m)) => (Cow::Owned(b), m, "max-age=86400"),
            None => return Response::builder().status(StatusCode::NOT_FOUND).body(Cow::Borrowed(&b""[..])).unwrap(),
        }
    } else {
        return Response::builder().status(StatusCode::NOT_FOUND).body(Cow::Borrowed(&b""[..])).unwrap();
    };
    Response::builder().header(CONTENT_TYPE, mime).header("Cache-Control", cache).body(body).unwrap()
}

/// The tray icon: click opens the window, the menu opens or quits. Its events go to the event
/// loop as window actions.
fn tray(proxy: tao::event_loop::EventLoopProxy<UiEvent>) -> Option<tray_icon::TrayIcon> {
    use tray_icon::menu::{Menu, MenuEvent, MenuItem, PredefinedMenuItem};
    use tray_icon::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
    let img = image::load_from_memory(include_bytes!("../assets/atrium.png")).ok()?.into_rgba8();
    let (w, h) = img.dimensions();
    let icon = tray_icon::Icon::from_rgba(img.into_raw(), w, h).ok()?;
    let open = MenuItem::new("Open Atrium", true, None);
    let quit = MenuItem::new("Quit Atrium", true, None);
    let menu = Menu::new();
    menu.append_items(&[&open, &PredefinedMenuItem::separator(), &quit]).ok()?;
    let (open_id, quit_id) = (open.id().clone(), quit.id().clone());
    let p = proxy.clone();
    MenuEvent::set_event_handler(Some(move |e: MenuEvent| {
        let action = if e.id == quit_id { WindowAction::Quit } else if e.id == open_id { WindowAction::Show } else { return };
        let _ = p.send_event(UiEvent::Window(action));
    }));
    TrayIconEvent::set_event_handler(Some(move |e: TrayIconEvent| {
        if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = e {
            let _ = proxy.send_event(UiEvent::Window(WindowAction::Show));
        }
    }));
    TrayIconBuilder::new()
        .with_tooltip("Atrium")
        .with_icon(icon)
        .with_menu(Box::new(menu))
        .with_menu_on_left_click(false)
        .build()
        .map_err(|e| sys::log(&format!("tray: {e}")))
        .ok()
}

pub fn run(start_in_tray: bool) -> ! {
    let event_loop = EventLoopBuilder::<UiEvent>::with_user_event().build();
    let window = WindowBuilder::new()
        .with_title("Atrium")
        .with_inner_size(LogicalSize::new(1320.0, 840.0))
        .with_min_inner_size(LogicalSize::new(960.0, 600.0))
        .with_window_icon(crate::login::window_icon())
        .with_theme(Some(tao::window::Theme::Dark))
        .with_decorations(false)
        .with_visible(!start_in_tray);
    #[cfg(windows)]
    let window = {
        use tao::platform::windows::WindowBuilderExtWindows;
        window.with_undecorated_shadow(true)
    };
    let window = window
        .build(&event_loop)
        .unwrap_or_else(|e| {
            sys::log(&format!("window: {e}"));
            std::process::exit(1)
        });

    let app = App::new(event_loop.create_proxy());
    let tray_icon = tray(event_loop.create_proxy());
    let ipc_app = app.clone();
    let mut ctx = WebContext::new(Some(sys::data_dir().join("webview-ui")));
    let builder = WebViewBuilder::new_with_web_context(&mut ctx)
        .with_url(START_URL)
        .with_background_color((15, 17, 23, 255))
        .with_custom_protocol("atrium".into(), |_, req| serve(req))
        .with_ipc_handler(move |req| ipc_app.command(req.body()))
        // Links in the page never navigate the app window; https ones open in the browser.
        .with_navigation_handler(|u| {
            let ours = u.starts_with(START_URL) || u.starts_with("about:");
            if !ours && u.starts_with("https://") {
                let _ = open::that(&u);
            }
            ours
        })
        .with_new_window_req_handler(|u, _| {
            if u.starts_with("https://") {
                let _ = open::that(&u);
            }
            wry::NewWindowResponse::Deny
        });

    #[cfg(not(target_os = "linux"))]
    let webview = builder.build(&window);
    #[cfg(target_os = "linux")]
    let webview = {
        use tao::platform::unix::WindowExtUnix;
        use wry::WebViewBuilderExtUnix;
        builder.build_gtk(window.default_vbox().expect("tao window has a vbox"))
    };
    let webview = webview.unwrap_or_else(|e| {
        sys::log(&format!("webview: {e}"));
        std::process::exit(1)
    });

    event_loop.run(move |event, _, flow| {
        *flow = ControlFlow::Wait;
        let _keep = (&window, &app, &tray_icon);
        match event {
            Event::UserEvent(UiEvent::Script(js)) => {
                let _ = webview.evaluate_script(&js);
            }
            Event::UserEvent(UiEvent::Window(action)) => match action {
                WindowAction::Drag => {
                    let _ = window.drag_window();
                }
                WindowAction::Minimize => window.set_minimized(true),
                WindowAction::ToggleMaximize => window.set_maximized(!window.is_maximized()),
                // The page asks or applies the user's choice, then sends hide or quit.
                WindowAction::Close => {
                    let _ = webview.evaluate_script("window.atrium && atrium.requestClose()");
                }
                // Without a tray icon there'd be no way back to a hidden window: quit instead.
                WindowAction::Hide if tray_icon.is_none() => *flow = ControlFlow::Exit,
                WindowAction::Hide => window.set_visible(false),
                WindowAction::Show => {
                    window.set_visible(true);
                    window.set_minimized(false);
                    window.set_focus();
                }
                WindowAction::Quit => *flow = ControlFlow::Exit,
                WindowAction::Resize(edge) => {
                    let dir = match edge.as_str() {
                        "n" => ResizeDirection::North,
                        "s" => ResizeDirection::South,
                        "e" => ResizeDirection::East,
                        "w" => ResizeDirection::West,
                        "ne" => ResizeDirection::NorthEast,
                        "nw" => ResizeDirection::NorthWest,
                        "se" => ResizeDirection::SouthEast,
                        _ => ResizeDirection::SouthWest,
                    };
                    if !window.is_maximized() {
                        let _ = window.drag_resize_window(dir);
                    }
                }
            },
            // The page shows maximise or restore, and drops its resize edges when maximised.
            Event::WindowEvent { event: WindowEvent::Resized(_), .. } => {
                let _ = webview.evaluate_script(&format!("window.atrium && atrium.maximized({})", window.is_maximized()));
            }
            // Alt+F4 / taskbar close: same as the page's close button.
            Event::WindowEvent { event: WindowEvent::CloseRequested, .. } => {
                if window.is_visible() {
                    window.set_focus();
                }
                let _ = webview.evaluate_script("window.atrium && atrium.requestClose()");
            }
            _ => {}
        }
    })
}
