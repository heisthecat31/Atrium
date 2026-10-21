#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
mod app;
mod login;
mod services;
mod sys;
mod ui;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        // Meta sign-in window (child process, see login.rs).
        Some("--login-meta") => login::meta_window(),
        // Run any Oculus SDK program through Atrium OVR: atrium --translate <exe> [args...]
        Some("--translate") => {
            let Some(exe) = args.get(2).map(std::path::PathBuf::from) else {
                eprintln!("usage: atrium --translate <exe> [args...]");
                std::process::exit(2)
            };
            let hw = services::headsets::detect();
            let st = services::runtime::state(&hw);
            services::runtime::write_backend(&st);
            // ATRIUM_XR=<manifest.json> picks the OpenXR runtime for this run (testing backends).
            let xr = std::env::var_os("ATRIUM_XR").map(std::path::PathBuf::from).or_else(|| services::launcher::xr_runtime(Some(&st)));
            match services::launcher::translated(&exe, &args[3..], xr.as_deref(), None) {
                Ok(pid) => println!("started {} (pid {pid}) on {}", exe.display(), xr.map(|p| p.display().to_string()).unwrap_or("the active OpenXR runtime".into())),
                Err(e) => {
                    eprintln!("{e}");
                    std::process::exit(1)
                }
            }
        }
        // Run a sign-in from a terminal and save it (debugging without the UI).
        Some("--sign-in") => {
            let cancel = std::sync::atomic::AtomicBool::new(false);
            let mut s = sys::settings::Settings::load();
            match args.get(2).map(String::as_str) {
                Some("steam") => match login::steam(&cancel) {
                    Ok(a) => {
                        println!("Steam: {} ({})", a.name, a.steam_id);
                        s.steam_account = Some(a);
                        s.save();
                    }
                    Err(e) => eprintln!("Steam: {e}"),
                },
                Some("meta") => match login::meta(&cancel) {
                    Ok(a) => {
                        println!("Meta: {} ({})", a.name, a.id);
                        if let Some(t) = login::meta_token(&a) {
                            match services::meta::owned(&t) {
                                Ok(apps) => println!("owns {} PC apps", apps.len()),
                                Err(e) => eprintln!("library: {e}"),
                            }
                        }
                        login::store_meta(&mut s, a);
                        s.save();
                    }
                    Err(e) => eprintln!("Meta: {e}"),
                },
                _ => eprintln!("usage: atrium --sign-in steam|meta"),
            }
        }
        // Started at sign-in (autostart): straight to the tray.
        Some("--tray") => ui::run(true),
        _ => ui::run(false),
    }
}
