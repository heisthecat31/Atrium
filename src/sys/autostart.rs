//! Start Atrium when the user signs in. Windows: the per-user Run key (no admin). Linux: an XDG
//! autostart entry. The setting lives there, not in settings.json, so it can't drift.
#[cfg(windows)]
const RUN: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";

fn command() -> String {
    let exe = std::env::current_exe().unwrap_or_default();
    format!("\"{}\"", exe.display())
}

pub fn enabled() -> bool {
    #[cfg(windows)]
    {
        use winreg::{enums::HKEY_CURRENT_USER, RegKey};
        return RegKey::predef(HKEY_CURRENT_USER).open_subkey(RUN).and_then(|k| k.get_value::<String, _>("Atrium")).is_ok();
    }
    #[cfg(not(windows))]
    entry().exists()
}

pub fn set(on: bool) -> bool {
    #[cfg(windows)]
    {
        use winreg::{enums::HKEY_CURRENT_USER, RegKey};
        let Ok((k, _)) = RegKey::predef(HKEY_CURRENT_USER).create_subkey(RUN) else { return false };
        return if on { k.set_value("Atrium", &command()).is_ok() } else { k.delete_value("Atrium").is_ok() || !enabled() };
    }
    #[cfg(not(windows))]
    {
        let f = entry();
        if on {
            let _ = std::fs::create_dir_all(f.parent().unwrap());
            std::fs::write(&f, format!("[Desktop Entry]\nType=Application\nName=Atrium\nExec={}\nX-GNOME-Autostart-enabled=true\n", command())).is_ok()
        } else {
            std::fs::remove_file(&f).is_ok() || !f.exists()
        }
    }
}

#[cfg(not(windows))]
fn entry() -> std::path::PathBuf {
    std::env::var("XDG_CONFIG_HOME").map(std::path::PathBuf::from).unwrap_or_else(|_| super::home().join(".config")).join("autostart/atrium.desktop")
}
