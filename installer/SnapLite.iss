#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#define MyAppName "Snap-Lite"
#define MyAppPublisher "Elias"
#define MyAppExeName "SnapLite.exe"

[Setup]
AppId={{7A9D3E9D-8A2D-4F81-9F2A-6E9D5A6A2D31}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\Snap-Lite
DefaultGroupName=Snap-Lite
DisableProgramGroupPage=yes
DisableDirPage=no
OutputDir=..\dist
OutputBaseFilename=SnapLite-Setup-{#MyAppVersion}
SetupIconFile=..\build\SnapLite.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
CloseApplications=yes
RestartApplications=no
AppMutex=Local\SnapLiteSingleInstance
VersionInfoVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoDescription=Snap-Lite Windows Installer

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项："; Flags: unchecked

[Files]
Source: "..\package\SnapLite.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Snap-Lite"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\Snap-Lite"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 Snap-Lite"; Flags: nowait postinstall skipifsilent
