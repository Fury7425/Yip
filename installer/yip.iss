; Inno Setup script for Yip.
;
; Yip ships unpackaged and self-contained (WindowsAppSDKSelfContained=true), so
; the installer is a plain file copy plus shortcuts — no Windows App SDK
; runtime prerequisite, no MSIX, no elevation.
;
; Build:
;   iscc /DYipSourceDir=..\build\release\yip-app\Release /DYipVersion=0.1.0 yip.iss

#ifndef YipSourceDir
  #define YipSourceDir "..\build\release\yip-app\Release"
#endif
#ifndef YipVersion
  #define YipVersion "0.1.0"
#endif
#ifndef YipArch
  #define YipArch "x64"
#endif

[Setup]
AppId={{B7E4A5C1-2D93-4A6E-9F31-7C0A5E8D2B44}
AppName=Yip
AppVersion={#YipVersion}
AppPublisher=Yip
AppCopyright=Copyright (C) Yip contributors
DefaultDirName={autopf}\Yip
DefaultGroupName=Yip
UninstallDisplayIcon={app}\yip-app.exe
OutputDir=.\out
OutputBaseFilename=yip-setup-{#YipVersion}-{#YipArch}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
; Per-user install: no admin prompt, and the recorder never needs elevation.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; Win10 2004 — matches WindowsTargetPlatformMinVersion 10.0.19041.0.
MinVersion=10.0.19041
#if YipArch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "startupicon"; Description: "Start Yip when I sign in"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
; Everything MSBuild emitted: the exe, the self-contained Windows App SDK
; binaries, and the XAML resource files. Nothing else is required at runtime.
Source: "{#YipSourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; \
    Excludes: "*.pdb,*.lib,*.exp,*.ilk,obj\*"

[Icons]
Name: "{group}\Yip"; Filename: "{app}\yip-app.exe"
Name: "{group}\Uninstall Yip"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Yip"; Filename: "{app}\yip-app.exe"; Tasks: desktopicon
Name: "{userstartup}\Yip"; Filename: "{app}\yip-app.exe"; Tasks: startupicon

[Run]
Filename: "{app}\yip-app.exe"; Description: "Launch Yip"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Leave recordings and settings alone — they live outside {app} on purpose.
Type: filesandordirs; Name: "{app}"
