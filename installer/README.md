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
