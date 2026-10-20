// Windows: embed the Atrium icon and file metadata in atrium.exe.
fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        let mut res = winresource::WindowsResource::new();
        res.set_icon("assets/atrium.ico");
        res.set("ProductName", "Atrium");
        res.set("FileDescription", "Atrium");
        let _ = res.compile();
    }
    println!("cargo:rerun-if-changed=assets/atrium.ico");
}
