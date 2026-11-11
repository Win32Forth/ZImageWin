; ZImager per-user installer. One build for ARM64, one for x64.
; iscc /DArch=arm64 Install\ZImager.iss
; iscc /DArch=x64 Install\ZImager.iss

#ifndef Arch
  #define Arch "arm64"
#endif

#if Arch == "x64"
  #define AppArch "x64"
  #define ArchTag "x64"
  #define ArchAllowed "x64compatible"
  #define AppGuid "{{BC5807BF-4A81-4C94-877D-E3CC380DE8B4}"
#else
  #define AppArch "ARM64"
  #define ArchTag "arm64"
  #define ArchAllowed "arm64"
  #define AppGuid "{{BF2BA587-533F-4E8B-A312-3700E5577FDA}"
#endif

#define AppVersion "1.0.0"

[Setup]
AppId={#AppGuid}
AppName=ZImager {#AppArch}
AppVersion={#AppVersion}
AppVerName=ZImager {#AppVersion} {#AppArch}
AppPublisher=Win32Forth
AppComments=ZImager {#AppArch}. Photo editor for Windows.
DefaultDirName={localappdata}\Programs\ZImager-{#AppArch}
DefaultGroupName=ZImager
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=ZImager-{#AppVersion}-{#ArchTag}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed={#ArchAllowed}
ArchitecturesInstallIn64BitMode={#ArchAllowed}
PrivilegesRequired=lowest
UninstallDisplayName=ZImager {#AppVersion} {#AppArch}
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Files]
Source: "..\build\{#ArchTag}\ZImager.exe"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Icons]
Name: "{autoprograms}\ZImager {#AppArch}"; Filename: "{app}\ZImager.exe"
Name: "{autodesktop}\ZImager {#AppArch}"; Filename: "{app}\ZImager.exe"; Tasks: desktopicon

[Messages]
FinishedLabel=ZImager {#AppArch} is installed. Uninstall removes this program and leaves edits, the helper pin, and folder memory in the ZImageWin folder under Local AppData.
