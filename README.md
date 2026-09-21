# Yip

[![ci](https://github.com/Fury7425/Yip/actions/workflows/ci.yml/badge.svg)](https://github.com/Fury7425/Yip/actions/workflows/ci.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![platform: Windows 10 2004+ | x64 · ARM64](https://img.shields.io/badge/platform-Windows%2010%202004%2B%20%7C%20x64%20%C2%B7%20ARM64-lightgrey)

**A small, fast audio recorder for Windows.** Press a hotkey, talk, press it
again. Yip is built from native parts only (WinUI 3 on top, Rust and WASAPI
underneath), and its capture path never allocates, locks or blocks.

No AI, no cloud, no telemetry. Yip makes no network calls at all.

---

## Features

- **One-key recording.** `Ctrl+Alt+R` starts and stops a take from anywhere.
  You can rebind it in Settings.
- **Recording pill.** A small indicator at the top or bottom of the screen
  shows the recording light, elapsed time and a live meter. Tap it for Pause
  and Stop. It can shrink to a single dot and lets clicks pass through.
- **Four formats.** WAV (16/24-bit or 32-bit float), FLAC (16/24),
  MP3 (128–320 kbps), M4A/AAC (96–192 kbps). All of them use encoders that
  ship with Windows, so nothing extra is bundled.
- **Built-in playback.** Click a recording to play it. The transport has
  play/pause, a scrubber and a level meter, and seeking while you drag is
  smooth. The same lock-free engine as capture runs it in reverse.
- **Pause without gaps.** A paused take keeps the device stream open, so
  resuming is instant and the clock doesn't count the paused time.
- **Metering you can trust.** A logarithmic meter with peak hold, a clip
  lamp, and counters for overruns and dropped frames.
- **Lives in the tray.** Closing the window keeps Yip running. Recording and
  the hotkey keep working, and the tray icon or relaunching the exe brings the
  window back. Only one copy of Yip runs at a time.
- **Feels like part of Windows.** Mica and acrylic backgrounds, light and dark
  themes, keyboard shortcuts throughout.

### Keyboard shortcuts

| Keys          | Action                        |
|---------------|-------------------------------|
| `Ctrl+Alt+R`  | Start / stop recording (works from any app, rebindable) |
| `Ctrl+R`      | Start / stop recording (in the window) |
| `Ctrl+P`      | Play / pause the selected recording |
| `Ctrl+F`      | Search recordings              |
| `F5`          | Refresh the recordings list    |

### Where things go

| What          | Location                               |
|---------------|----------------------------------------|
| Recordings    | `%USERPROFILE%\Music\Yip` (you can change this in Settings) |
| Settings      | `%LOCALAPPDATA%\Yip\settings.json`     |

Uninstalling leaves both of these in place.

## Install

Download `yip-setup-<version>-x64.exe` from the latest
[CI run](https://github.com/Fury7425/Yip/actions/workflows/ci.yml) (artifact
`yip-setup-x64`) and run it. It installs for the current user only, so no
admin prompt appears. Yip is self-contained, so you don't need to install the
Windows App SDK runtime.

> **Defender or SmartScreen may block the installer.** The build is unsigned
> and brand new, so it has no download reputation yet.
> [installer/README.md](installer/README.md) explains why this happens and
> what to do.

**Requirements:** Windows 10 2004 (build 19041) or later. Windows 11 x64 is
the main target, and ARM64 is also supported. The *N* and *KN* editions of
Windows can only record WAV unless the
[Media Feature Pack](https://support.microsoft.com/windows/media-feature-pack-list-for-windows-n-editions-c1c6fffa-d052-8338-7a79-a4bb980a700a)
is installed.

## Build from source

### Prerequisites

- **Visual Studio 2022** with the *Desktop development with C++* workload and
  the **Windows 11 SDK 10.0.22621+**
- **Rust** stable 1.85+ via [rustup](https://rustup.rs). `rust-toolchain.toml`
  pins it. Add `aarch64-pc-windows-msvc` if you want the ARM64 build.
- **CMake** 3.27+
- *(optional)* [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`iscc` on
  `PATH`) to build the installer

NuGet restores the Windows App SDK and C++/WinRT packages during the build.

### Build

```powershell
cmake --preset release
cmake --build --preset release
```

This produces `build/release/yip-app/Release/yip-app.exe`, a self-contained
app you can run directly.

| Preset          | Output                                   |
|-----------------|------------------------------------------|
| `debug`         | x64 debug build                          |
| `release`       | x64 release build                        |
| `release-arm64` | ARM64 release, cross-compiled on x64     |

### Installer

```powershell
iscc /DYipSourceDir=..\build\release\yip-app\Release /DYipVersion=0.1.0 installer\yip.iss
```

The installer is written to `installer/out/yip-setup-<version>-<arch>.exe`.

### Checks

These are the same gates CI runs:

```powershell
cargo fmt --check
cargo clippy --all-targets --all-features -- -D warnings
cargo test --release
```

## How it works

```
┌──────────────────────────────┐
│ yip-app  (WinUI 3, C++/WinRT)│  MainWindow · pill · tray · hotkey · settings
└──────────────┬───────────────┘
               │  plain C ABI (audio_core.h, generated by cbindgen)
┌──────────────▼───────────────┐
│ audio-core  (Rust staticlib) │
│                              │
│  capture ─▶ SPSC ring ─▶ writer ─▶ WAV (hound) / FLAC·MP3·M4A (Media Foundation)
│  render  ◀─ SPSC ring ◀─ decoder ◀─ Media Foundation source reader
└──────────────┬───────────────┘
               │
            WASAPI (event-driven, MMCSS "Pro Audio")
```

- **The realtime threads don't allocate, lock, block or log.** Capture and
  render talk to the file side through a lock-free `rtrb` ring buffer and
  atomics, and nothing else.
- **The UI never waits on audio.** Meter and playback state come back from a
  single lock-free snapshot call (`rec_meter` / `play_state`). Recording start
  and stop are pushed to the UI through a callback. The app uses 0% CPU when
  idle because no timers run until something is happening.
- **Errors show up early.** A format the encoder refuses fails at
  `rec_start`, and a file that can't be played fails at `play_start`. You
  never get a silent take or silent playback.
- **Nothing but plain C crosses the FFI boundary.** Every Rust entry point
  catches panics, checks for null and validates UTF-8.

| Directory     | Language            | Builds                       |
|---------------|---------------------|------------------------------|
| `audio-core/` | Rust 2024           | `audio_core.lib` (staticlib) |
| `yip-app/`    | C++20 / WinRT / XAML| `yip-app.exe` (unpackaged)   |
| `installer/`  | Inno Setup          | `yip-setup-*.exe`            |

[CLAUDE.md](CLAUDE.md) covers the full architecture rules, conventions and
design-token system.

## Continuous integration

[GitHub Actions](.github/workflows/ci.yml) runs these jobs on every push and
pull request:

| Job            | What it does |
|----------------|--------------|
| `rust`         | fmt, clippy (`-D warnings`), tests. Publishes `audio_core.lib` and the header. |
| `native`       | Full CMake/MSBuild x64 build and publishes `yip-app-x64` |
| `native-arm64` | ARM64 cross-build and publishes `yip-app-arm64` |
| `installer`    | Inno Setup package and publishes `yip-setup-x64` |
| `format-cpp`   | `clang-format` check (informational for now) |

## Status

| Milestone | Status |
|-----------|--------|
| M1–M3: FFI skeleton, WASAPI capture, main window | ✅ Done |
| M4: recording indicator pill | ✅ Done |
| M5–M6: VST3 processing | ⏸ Removed for now |
| M7: polish (hotkey, tray, formats, playback, installer, ARM64) | ⏳ Nearly done: clippy sweep, clang-format gate, WPR profiling left |

## License

[MIT](LICENSE) © 2026 Yip contributors.
