# ZImageWin status

Newest first. The Mac log is `C:\VSCodeProjects\ZImage\STATUS.md`.

## 2026-11-10 — 1.0.0

Version 1.0.0 for ARM64 and x64. The manifest and the version resource are 1.0.0.0. `.zimg` files already wrote `appVersion` 1.0.0.

`README.md` describes the window: Disk and Server, Folder and Image, Spectrum, Tone, Color, Geometry, Touchup, and History, Save and Save As, export, print, and the helper on `192.168.1.147` port 8742. Grades are linear sRGB. TIFF and PNG are 8-bit. Object removal is absent. The helper binary stays on the NAS.

A successful pair stores the bearer with DPAPI in `%LOCALAPPDATA%\ZImageWin\helper.auth`. The 2026-11-09 note below recorded the earlier state, before that save existed.

Installers are per user, Inno Setup 6, same shape as EditForthWin. `Install\build-installers.bat` reads `build\arm64\ZImager.exe` and `build\x64\ZImager.exe` and writes `dist\ZImager-1.0.0-arm64.exe` and `dist\ZImager-1.0.0-x64.exe`. Uninstall leaves `%LOCALAPPDATA%\ZImageWin`.

## 2026-11-09 — DS423

The helper host is `192.168.1.147` port 8742. Server mode asks `GET /v1/health` over TLS and pins the certificate the first time, the same SPKI hash the Mac stores. An ARM64 probe on this PC got `192.168.1.147   Helper online. Safe to reboot.` The pin hash is in `%LOCALAPPDATA%\ZImageWin\helper.ini`. A second call matched that pin. The BackupData share still needs a Windows sign-in before the photograph list can be read. Pair with Helper asks for that sign-in. The bearer token is not stored yet.

## 2026-11-09

The project is a C Win32 shell of ZImager 1.7.0. ARM64 is the first build. `build-x64.bat` compiles the same sources for x64. Windows 10 and Windows 11 are the two targets, on both processors.

The window matches the Mac chrome: one window, Disk and Server, Folder and Image, cream panels, the `#5C5C5C` well, and the Spectrum, Tone, Color, Geometry, Touchup, and History titles. Open Folder lists the child directories. Open File records the path and shows Image.

Not started: decoding a still, the edit log, tone and geometry, the folder grid, export, and print.
