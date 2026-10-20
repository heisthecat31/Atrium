//! The Atrium window: a tao window with the system webview (WebView2 on Windows, WebKitGTK on
//! Linux) showing the page in web/. No browser engine ships with Atrium.
//!
//! The page is served from Atrium's own `atrium` protocol (http://atrium.localhost/ on Windows,
//! atrium://localhost/ elsewhere), compiled into the exe. Meta's local store art is served at
//! /art?p=<path>, limited to Meta's StoreAssets folder. The page talks to Rust with
//! `ipc.postMessage(json)`; Rust answers by running `atrium.update(...)` in the page (app.rs).
use crate::app::{App, UiEvent};
use crate::sys;
use std::borrow::Cow;
use std::path::PathBuf;
use tao::dpi::LogicalSize;
use tao::event::{Event, WindowEvent};
use tao::event_loop::{ControlFlow, EventLoopBuilder};
use tao::window::WindowBuilder;
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

pub fn run() -> ! {
    let event_loop = EventLoopBuilder::<UiEvent>::with_user_event().build();
    let window = WindowBuilder::new()
        .with_title("Atrium")
        .with_inner_size(LogicalSize::new(1320.0, 840.0))
        .with_min_inner_size(LogicalSize::new(960.0, 600.0))
        .with_window_icon(crate::login::window_icon())
        .with_theme(Some(tao::window::Theme::Dark))
        .build(&event_loop)
        .unwrap_or_else(|e| {
            sys::log(&format!("window: {e}"));
            std::process::exit(1)
        });

    let app = App::new(event_loop.create_proxy());
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
        let _keep = (&window, &app);
        match event {
            Event::UserEvent(UiEvent::Script(js)) => {
                let _ = webview.evaluate_script(&js);
            }
            Event::WindowEvent { event: WindowEvent::CloseRequested, .. } => *flow = ControlFlow::Exit,
            _ => {}
        }
    })
}
