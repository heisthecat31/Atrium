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
        _ => ui::run(),
    }
}
