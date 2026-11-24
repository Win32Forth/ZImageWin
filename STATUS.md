# ZImageWin status

Newest first. The Mac log is `C:\VSCodeProjects\ZImage\STATUS.md`.

## 2026-11-23 — ZImager 1.8.6.1

- Status: done
- Scope: Windows file version, product version, and manifest are 1.8.6.1. The folder column matches the Mac stack. Tone order is Auto, Reset, Flatten. Help, About ZImager reads the running program. `.zimg` `appVersion` stays `1.0.0`. The schema stays 1.
- Notes: `build.bat` wrote `build\arm64\ZImager.exe`. `build-x64.bat` wrote `build\x64\ZImager.exe`. Both file versions and product versions read 1.8.6.1. `build-test.bat` and `build-test-x64.bat` printed `ok`. `Install\build-installers.bat` wrote `dist\ZImager-1.8.6.1-arm64.exe` (2360016 bytes, SHA-256 `f023b21bc0f1b4c7a4508196c544a2cbadfe04fe9cf7989d1f8b48abbf2dd169`) and `dist\ZImager-1.8.6.1-x64.exe` (2423252 bytes, SHA-256 `d438a8156570072a4f751e26ed6d2cb3d354b510845469e30600ba81efc914eb`). The 1.0.0 and 1.8.6 installers stay in `dist`. The window was not launched for this stamp. No helper route that changes the library was sent. The NAS binary was left where it is.

The Folder column scrolls on its own. A Photos folder with more than one page opens with the page panel, then Preview, Date, Move, Merge, and Build previews. Review keeps Preview, Date, and Set date. The disclosure is at the right of the title, with a short space before the first letter. Open points down. Collapsed points up. Find, Clear filter, Rescan, and Delete ALL sit on the center header. A merge preview replaces the grid. Fit and 100% sit on the black join. Server Move, Organize, Clean, Rescan, and Rename still say the action is left for a confirmed run. Delete ALL was not pressed.

## 2026-11-23 — ZImager 1.8.6

- Status: done
- Scope: Windows marketing version 1.8.6, matching the Mac app. Manifest and file version are 1.8.6.0. Product version is 1.8.6. `.zimg` `appVersion` stays `1.0.0`. The schema stays 1.
- Notes: `build.bat` wrote `build\arm64\ZImager.exe`. `build-x64.bat` wrote `build\x64\ZImager.exe`. Both file versions read 1.8.6.0, product version 1.8.6. `build-test.bat` and `build-test-x64.bat` printed `ok`. `Install\build-installers.bat` wrote `dist\ZImager-1.8.6-arm64.exe` (2354562 bytes, SHA-256 `4fe5d59e99c530d8bedc531996341b4e4223db2367f981d88c5c82507663deb8`) and `dist\ZImager-1.8.6-x64.exe` (2416795 bytes, SHA-256 `5e237618c5469cfae6eadff161513ed975e66d7470484c49db66f4ca487806eb`). The 1.0.0 installers stay in `dist` for that release. The window was not launched for this stamp. No helper route that changes the library was sent. The NAS binary was left where it is.

The inspector sections are Spectrum, Tone, Curve, Color, Geometry, Touchup, Vignette, and History. Tone ends with a hairline, then Texture, Clarity, Details, and Sharpness. Flatten sits with Auto and Reset. View adds Rotate Counterclockwise, Rotate Clockwise, Flip Left/Right, and Flip Top/Bottom. Events and Reorganize stay hidden.

Folder view pages a heap catalog: page 0 is the first 200 stills plus at most 300 videos, and later pages are 200 stills. Find searches the whole catalog. Four threads decode the page. Disk Build previews writes the temporary JPEG cache. Server Build previews says the previews stay on the NAS. Date, Move, Merge, and Build previews fold from the right column. Disk Move, Organize of this page, Clean on a local drive, Merge, and Rename run on this PC. Server Move, Organize, Clean, Rescan, and Rename say the action is left for a confirmed run. Delete ALL on `Photos\_Removed` is wired and was not pressed. Disk Organize does not walk later pages and does not confirm a catalog date.

Spot and Scratch are one tool. Scratch fill uses both sides of the stroke. Size runs from 8 to 200 and defaults to 48. Red Eye is the third Touchup button. Curve and Vignette persist in the `.zimg`. A video plays graded frames with no soundtrack, and export writes a new MPEG-4. An X3F opens the embedded JPEG. A PDF lists JPEG pages and opens the clicked page. Horizon uses the on-screen width and height. Grades stay linear sRGB. DNG export and the Sigma sensor converter are absent. Object removal stays hidden.

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
