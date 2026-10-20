//! Sealing tokens at rest. Windows: DPAPI bound to the current user. Elsewhere the settings file
//! itself is 0600 and the value is stored as-is (prefixed so it's never mistaken for sealed data).

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

fn b64(data: &[u8]) -> String {
    let mut s = String::new();
    for c in data.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            if i <= c.len() {
                s.push(B64[(n >> (18 - 6 * i) & 63) as usize] as char);
            } else {
                s.push('=');
            }
        }
    }
    s
}

fn unb64(s: &str) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    let mut buf = 0u32;
    let mut bits = 0;
    for ch in s.bytes().filter(|&b| b != b'=') {
        let v = B64.iter().position(|&x| x == ch)? as u32;
        buf = buf << 6 | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((buf >> bits) as u8);
            buf &= (1 << bits) - 1;
        }
    }
    Some(out)
}

#[cfg(windows)]
pub fn seal(plain: &str) -> Option<String> {
    use windows_sys::Win32::Security::Cryptography::*;
    unsafe {
        let input = CRYPT_INTEGER_BLOB { cbData: plain.len() as u32, pbData: plain.as_ptr() as *mut u8 };
        let mut out = CRYPT_INTEGER_BLOB { cbData: 0, pbData: std::ptr::null_mut() };
        if CryptProtectData(&input, std::ptr::null(), std::ptr::null(), std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut out) == 0 {
            return None;
        }
        let bytes = std::slice::from_raw_parts(out.pbData, out.cbData as usize).to_vec();
        windows_sys::Win32::Foundation::LocalFree(out.pbData as _);
        Some(format!("dpapi:{}", b64(&bytes)))
    }
}

#[cfg(windows)]
pub fn unseal(sealed: &str) -> Option<String> {
    use windows_sys::Win32::Security::Cryptography::*;
    let data = unb64(sealed.strip_prefix("dpapi:")?)?;
    unsafe {
        let input = CRYPT_INTEGER_BLOB { cbData: data.len() as u32, pbData: data.as_ptr() as *mut u8 };
        let mut out = CRYPT_INTEGER_BLOB { cbData: 0, pbData: std::ptr::null_mut() };
        if CryptUnprotectData(&input, std::ptr::null_mut(), std::ptr::null(), std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut out) == 0 {
            return None;
        }
        let s = String::from_utf8(std::slice::from_raw_parts(out.pbData, out.cbData as usize).to_vec()).ok();
        windows_sys::Win32::Foundation::LocalFree(out.pbData as _);
        s
    }
}

#[cfg(not(windows))]
pub fn seal(plain: &str) -> Option<String> {
    Some(format!("plain:{}", b64(plain.as_bytes())))
}

#[cfg(not(windows))]
pub fn unseal(sealed: &str) -> Option<String> {
    String::from_utf8(unb64(sealed.strip_prefix("plain:")?)?).ok()
}
