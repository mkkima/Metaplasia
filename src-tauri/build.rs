fn main() {
    println!("cargo:rerun-if-env-changed=METAPLASIA_UPDATE_CHANNEL");
    println!("cargo:rerun-if-env-changed=METAPLASIA_UPDATE_PUBLIC_KEY");
    println!("cargo:rerun-if-changed=development-update-public-key.txt");
    println!("cargo:rerun-if-changed=icons/icon.ico");
    println!("cargo:rerun-if-changed=icons/icon.png");

    let channel =
        std::env::var("METAPLASIA_UPDATE_CHANNEL").unwrap_or_else(|_| "stable".to_owned());
    if channel == "development" && std::env::var_os("METAPLASIA_UPDATE_PUBLIC_KEY").is_none() {
        let public_key = std::fs::read_to_string("development-update-public-key.txt")
            .expect("could not read the committed development update public key");
        let public_key = public_key.trim();
        let public_key_bytes = public_key.as_bytes();
        assert!(
            public_key_bytes.len() == 44
                && public_key_bytes[43] == b'='
                && public_key_bytes[..43]
                    .iter()
                    .copied()
                    .all(|byte| byte.is_ascii_alphanumeric() || byte == b'+' || byte == b'/'),
            "the committed development update public key is not canonical Base64"
        );
        println!("cargo:rustc-env=METAPLASIA_UPDATE_PUBLIC_KEY={public_key}");
    }

    tauri_build::build();
}
