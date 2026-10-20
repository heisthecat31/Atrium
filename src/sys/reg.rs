//! Windows registry reads.
use winreg::enums::*;
use winreg::RegKey;

fn open(path: &str) -> Option<RegKey> {
    let (hive, rest) = path.split_once('\\')?;
    let root = match hive.to_ascii_uppercase().as_str() {
        "HKLM" | "HKEY_LOCAL_MACHINE" => RegKey::predef(HKEY_LOCAL_MACHINE),
        "HKCU" | "HKEY_CURRENT_USER" => RegKey::predef(HKEY_CURRENT_USER),
        _ => return None,
    };
    root.open_subkey_with_flags(rest, KEY_READ).ok()
}

pub fn string(path: &str, name: &str) -> Option<String> {
    open(path)?.get_value::<String, _>(name).ok().filter(|s| !s.is_empty())
}

/// Value names of a key (OpenXR's AvailableRuntimes keeps the manifest paths as names).
pub fn value_names(path: &str) -> Vec<String> {
    open(path).map(|k| k.enum_values().filter_map(|v| v.ok().map(|(n, _)| n)).collect()).unwrap_or_default()
}

/// Every subkey's string values.
pub fn subkeys(path: &str) -> Vec<std::collections::HashMap<String, String>> {
    let Some(k) = open(path) else { return vec![] };
    k.enum_keys()
        .filter_map(|n| n.ok())
        .filter_map(|n| k.open_subkey_with_flags(&n, KEY_READ).ok())
        .map(|sk| sk.enum_values().filter_map(|v| v.ok()).filter_map(|(n, _)| sk.get_value::<String, _>(&n).ok().map(|s| (n, s))).collect())
        .collect()
}

pub fn set_user_env(name: &str, value: Option<&str>) -> bool {
    let Ok((env, _)) = RegKey::predef(HKEY_CURRENT_USER).create_subkey("Environment") else { return false };
    match value {
        Some(v) => env.set_value(name, &v).is_ok(),
        None => env.delete_value(name).is_ok() || env.get_raw_value(name).is_err(),
    }
}
