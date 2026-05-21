use std::env;
use std::path::PathBuf;

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let out_dir = PathBuf::from(&crate_dir).join("include");
    std::fs::create_dir_all(&out_dir).expect("create include dir");

    let header_path = out_dir.join("audio_core.h");

    let config = cbindgen::Config::from_file(PathBuf::from(&crate_dir).join("cbindgen.toml"))
        .expect("read cbindgen.toml");

    match cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
    {
        Ok(bindings) => {
            bindings.write_to_file(&header_path);
            println!("cargo:rerun-if-changed=src");
            println!("cargo:rerun-if-changed=cbindgen.toml");
            println!("cargo:rerun-if-changed=build.rs");
            println!(
                "cargo:warning=audio_core.h written to {}",
                header_path.display()
            );
        }
        Err(err) => {
            // Soft-fail in non-default builds (e.g. docs) — header is regenerated next build.
            println!("cargo:warning=cbindgen failed: {err}");
        }
    }
}
