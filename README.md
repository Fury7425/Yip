# Yip

[![ci](https://github.com/OWNER/REPO/actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)

Native Windows audio recorder. Minimalist UI, lock-free WASAPI capture, offline VST3 post-processing.

## Build prerequisites

- Visual Studio 2022 with **Desktop development with C++** + **Windows 11 SDK 10.0.22621+**
- Rust stable 1.80+ via [rustup](https://rustup.rs) (target `x86_64-pc-windows-msvc`)
- CMake 3.27+
- Windows App SDK 1.6+ runtime ([download](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads))
- vcpkg (manifest mode, configured via `VCPKG_ROOT`)

## Build

```powershell
cmake --preset release
cmake --build --preset release
```

Output: `build/release/yip-app/yip-app.exe`

## Layout

| Dir          | Lang           | Output                          |
|--------------|----------------|---------------------------------|
| `audio-core` | Rust 2024      | `audio_core.lib` (staticlib)    |
| `vst-host`   | C++20          | `vst_host.lib` (static)         |
| `yip-app`    | C++/WinRT XAML | `yip-app.exe` (unpackaged)      |

## Continuous integration

GitHub Actions ([ci.yml](.github/workflows/ci.yml)) runs on every push and PR:

1. **`rust` job** — `cargo fmt --check`, `cargo clippy --all-targets --all-features -D warnings`, `cargo test --release`. Publishes `audio_core.lib` + `audio_core.h` as artifacts.
2. **`native` job** — depends on `rust`. Runs `cmake --preset release` + `cmake --build --preset release` (which msbuild-restores NuGet, builds `vst-host`, then builds `yip-app.vcxproj`). Publishes `yip-app` as a self-contained artifact (`WindowsAppSDKSelfContained=true`, no runtime install required).
3. **`format-cpp` job** — `clang-format --dry-run -Werror` against `yip-app/` + `vst-host/`.

NuGet packages and the cargo target dir are cached between runs.

## Milestones

See [CLAUDE.md](CLAUDE.md). Current: M1–M3 complete; M4 (indicator), M5–M6 (VST), M7 (polish) pending.

## License

[MIT](LICENSE). © 2026 Yip contributors.

The Steinberg VST3 SDK is **not** included in this repository. Wiring it in at
M5 requires signing the free Steinberg VST3 SDK developer agreement —
permissive MIT licensing of the Yip host is compatible with this path.
