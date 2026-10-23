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
            let set = sys::settings::Settings::load();
            let st = services::runtime::state(&hw, set.mix.enabled);
            services::runtime::write_backend(&st, services::mix::touch_for(&set).as_deref());
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
        // Meta downloads from a terminal (debugging without the UI):
        // atrium --meta-versions <appid> | atrium --meta-download <appid> <version|latest> <folder> [other version folders]
        Some(cmd @ ("--meta-versions" | "--meta-download")) => {
            let s = sys::settings::Settings::load();
            let Some(token) = s.meta_accounts.iter().find_map(|a| sys::secret::unseal(&a.token)) else {
                eprintln!("sign in to Meta first");
                std::process::exit(1)
            };
            let app_id = args.get(2).cloned().unwrap_or_default();
            // atrium --meta-versions check <binary id>...: can these builds be downloaded?
            if app_id == "check" {
                for b in &args[3..] {
                    match services::meta_download::build_info(&token, b) {
                        Ok((v, n, size)) => println!("{b}: version {v}, {n} files, {:.2} GB: downloadable", size as f64 / 1e9),
                        Err(e) => println!("{b}: {e}"),
                    }
                }
                return;
            }
            if app_id.is_empty() {
                // No app id: list what the account owns.
                for (id, name, _) in services::meta::owned(&token).unwrap_or_default() {
                    println!("{id}  {name}");
                }
                return;
            }
            let (name, builds) = services::meta_download::builds(&token, &app_id).unwrap_or_else(|e| {
                eprintln!("{e}");
                std::process::exit(1)
            });
            if cmd == "--meta-versions" {
                println!("{name}");
                for b in &builds {
                    println!("  {} (code {}, binary {})", b.version, b.version_code, b.binary_id);
                }
                return;
            }
            let (Some(want), Some(dir)) = (args.get(3), args.get(4)) else {
                eprintln!("usage: atrium --meta-download <appid> <version|latest> <folder>");
                std::process::exit(2)
            };
            let Some(b) = builds.iter().find(|b| want == "latest" || &b.version == want || &b.version_code.to_string() == want) else {
                eprintln!("no version {want}");
                std::process::exit(2)
            };
            // Further arguments: other versions' folders to copy unchanged pieces from.
            let seeds: Vec<std::path::PathBuf> = args[5..].iter().map(std::path::PathBuf::from).collect();
            let cancel = std::sync::atomic::AtomicBool::new(false);
            let r = services::meta_download::download(&token, &b.binary_id, std::path::Path::new(dir), &seeds, &cancel, &|phase, done, total| {
                eprint!("\r{phase}: {:.1}/{:.1} MB   ", done as f64 / 1e6, total as f64 / 1e6);
            });
            match r {
                Ok(i) => println!("\n{name} {} -> {dir}\\{} {}", b.version, i.launch_file, i.launch_args),
                Err(e) => {
                    eprintln!("\n{e}");
                    std::process::exit(1)
                }
            }
        }
        // Started at sign-in (autostart): straight to the tray.
        Some("--tray") => ui::run(true),
        _ => ui::run(false),
    }
}
