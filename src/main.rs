mod login;
mod services;
mod sys;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        // Meta sign-in window (child process, see login.rs).
        Some("--login-meta") => login::meta_window(),
        // Until the UI exists: run a sign-in from the terminal and save it.
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
        _ => eprintln!("Atrium UI not built yet. Try: atrium --sign-in steam|meta"),
    }
}
