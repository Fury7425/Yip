# Yip — Windows Audio Recorder

Minimalist Windows-only audio recorder. Native feel, absolute efficiency, offline VST3 post-processing. No AI, no cloud, no telemetry.

## Stack (non-negotiable)

- **UI:** WinUI 3 + C++/WinRT, Windows App SDK 1.6+, unpackaged
- **Audio:** Rust staticlib via FFI — WASAPI event-driven via `windows` crate, `rtrb` SPSC ring, `hound` WAV
- **VST3:** C++ static lib wrapping Steinberg VST3 SDK 3.7+
- **Build:** CMake top-level orchestrates Cargo + MSBuild
- **Toolchain:** VS 2022, Rust 1.80+, vcpkg
- **Target:** Win11 x64 primary, ARM64 secondary

## Architecture Rules (immutable)

1. Audio thread = zero alloc, lock, block, log. SPSC ring + atomics only.
2. Layer separation: UI → Rust FFI → WASAPI. No shortcuts.
3. Threading: capture (Rust MMCSS Pro Audio), writer (Rust), UI (C++/WinRT), VST workers per job.
4. Native only — no JS/webview/Electron.
5. No GC in realtime path.
6. No network in v1.

## Directory Layout

```
Yip/
├── CMakeLists.txt
├── CMakePresets.json
├── CLAUDE.md
├── README.md
├── Cargo.toml                   ← workspace
├── audio-core/                  ← Rust staticlib
├── vst-host/                    ← C++ static lib
└── yip-app/                     ← WinUI 3 C++/WinRT app
```

## Coding Conventions

### Rust
- 2024 edition, `#![warn(clippy::pedantic)]`
- Custom `YipError` enum on public surface. No `anyhow` in public API.
- No `unwrap()` outside tests.
- No `unsafe` outside `ffi`/WASAPI; every block needs `// SAFETY:` comment.
- `#[repr(C)]` on FFI types; opaque handles via `*mut c_void`.

### C++
- C++20, `winrt::` namespace explicit.
- Smart pointers only (`winrt::com_ptr`, `std::unique_ptr`). No raw `new`/`delete`.
- `co_await` for async; no `.get()` on UI thread.
- VST host behind C ABI; no C++ types cross FFI.

### Naming
- Rust crates: `kebab-case` (`audio-core`)
- Rust mods/fns: `snake_case`
- C++ types: `PascalCase`, methods: `camelCase`, namespace: `yip::`
- XAML: `PascalCase` elements, `x:Name` in `PascalCase`

## Anti-patterns

- ❌ Allocation in audio capture callback
- ❌ `Mutex`/`RwLock` between audio + writer threads
- ❌ `println!`/`OutputDebugString` from audio thread
- ❌ Hardcoded paths/rates/devices
- ❌ TODO/FIXME without matching milestone box
- ❌ Suppressed warnings to ship
- ❌ Mixing `Result` + exceptions across FFI

## Build Verification

```powershell
cargo fmt --check
cargo clippy --all-targets --all-features -- -D warnings
cargo test --release
cmake --preset release
cmake --build --preset release
```

All green before milestone advance.
