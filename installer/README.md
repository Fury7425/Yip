# Installer

`yip.iss` is an [Inno Setup](https://jrsoftware.org/isinfo.php) 6 script. It
packages the already-built output of `cmake --build --preset release` — the
installer does not build anything itself.

```powershell
cmake --preset release
cmake --build --preset release
iscc /DYipSourceDir=..\build\release\yip-app\Release /DYipVersion=0.1.0 installer\yip.iss
```

The result lands in `installer/out/yip-setup-<version>-<arch>.exe`.

Notes:

- Per-user install by default (`PrivilegesRequired=lowest`), so no UAC prompt.
  Yip never needs elevation: WASAPI capture, the settings file under
  `%LOCALAPPDATA%\Yip`, and `RegisterHotKey` all work unelevated.
- Yip is self-contained, so there is no Windows App SDK runtime prerequisite.
- Uninstall removes `{app}` only. Recordings and `settings.json` live outside
  it and are deliberately left in place.

## "This file contains a virus" — Defender blocking the installer

Windows Defender blocks a freshly built `yip-setup-*.exe` with *"Operation did
not complete because the file contains a virus or potentially unwanted
software."* Yip does not, and nothing in this repo reaches the network. The
block is a reputation-and-heuristics decision, not a detection of anything Yip
actually does:

1. The binary is **unsigned**. Nothing vouches for who built it.
2. It has **no download reputation** — SmartScreen has never seen this exact
   file before, because CI produced it minutes ago.
3. Inno Setup's self-extracting stub is a shape a lot of real malware also
   uses, so generic signatures (`Wacatac`, `Sabsik`) fire on it readily.

What this repo does about it: `yip-app.exe` now carries a proper `VERSIONINFO`
resource ([yip-app/yip-app.rc](../yip-app/yip-app.rc)) and the setup binary
carries matching `VersionInfo*` metadata. Blank publisher metadata is one of
the signals weighed against an unsigned binary, and a blank Properties page was
wrong regardless. **This lowers the odds; it does not fix the problem.**

The actual fix is an Authenticode signature:

```powershell
iscc /DYipSourceDir=... /DYipVersion=0.1.0 `
     /DYipSignTool=yipsign `
     "/Syipsign=signtool.exe sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /f cert.pfx /p $env:CERT_PW $f" `
     installer\yip.iss
```

- An **EV code-signing certificate** gets SmartScreen reputation immediately.
- A standard **OV certificate** builds reputation over downloads and weeks.
- A **self-signed** certificate does nothing here — it is not a trusted chain.

Buying a certificate is a decision for whoever publishes Yip; the build is
wired for one either way, and stays unsigned when `YipSignTool` is not passed.

If Defender flags a build you trust, report it as a false positive at
<https://www.microsoft.com/en-us/wdsi/filesubmission> — that is what gets the
signature corrected for everyone, and it usually turns around in a day or two.
Check Windows Security → Protection history first to see the detection name;
a *named* threat and a generic `Wacatac.B!ml` are different conversations.

To run a build in the meantime, skip the installer: the `yip-app-x64` CI
artifact is the self-contained app folder, so `yip-app.exe` runs from wherever
it is unzipped.
