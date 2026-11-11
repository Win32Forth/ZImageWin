# ZImager for Windows

Version 1.0.0. One window. Disk is the editor. Server is the library. The original photograph is left where it is.

The layout and the `.zimg` envelope follow the Mac app in the sibling `ZImage` tree. Grades on Windows are linear sRGB on the CPU. Assembly identity is `Win32Forth.ZImager` 1.0.0.0. Windows 10 and Windows 11 are the two targets, on ARM64 and on x64. Both builds compile the same C sources.

The NAS helper stays the Go program already installed with the library. This tree does not build it and the installer does not ship it.

## Install

Use the installer that matches the PC:

- `dist\ZImager-1.0.0-arm64.exe` on ARM64 Windows
- `dist\ZImager-1.0.0-x64.exe` on x64 Windows

Each installer is per user. It does not ask for an administrator password. ZImager.exe lands in `%LOCALAPPDATA%\Programs\ZImager-ARM64` or `%LOCALAPPDATA%\Programs\ZImager-x64`, with a Start menu shortcut named ZImager ARM64 or ZImager x64. The desktop icon task is offered and left unchecked.

Uninstall removes the program. Edits, the helper pin, and the last folder stay in `%LOCALAPPDATA%\ZImageWin`.

## Build

The Visual Studio C tools build the executables. Inno Setup 6 builds the installers.

```bat
build.bat
build-x64.bat
Install\build-installers.bat
```

`build.bat` writes `build\arm64\ZImager.exe`. `build-x64.bat` writes `build\x64\ZImager.exe`. The installer script reads those two files. File properties and the embedded manifest both say 1.0.0.

## The window

The toolbar switches Disk and Server, and Folder and Image. The menus are File, Edit, and View.

File: Open File (Ctrl+O), Open Folder, Save (Ctrl+S), Save As (Ctrl+Shift+S), Exit.

Edit: Undo (Ctrl+Z), Redo (Ctrl+Y), Copy (Ctrl+C), Paste (Ctrl+V).

View: Folder View (Ctrl+F), Edit View (Ctrl+E), Disk, Server, Sub-folder. The toolbar button for Edit View says Image.

The smallest window is 1100 by 700. The inspector is the column on the right. Its sections are Spectrum, Tone, Color, Geometry, Touchup, and History.

## Disk

Open Folder sets the root of an indented folder tree. A chevron opens the next level. Choosing a name lists that folder's stills and videos. Hidden names, names that start with a dot, and links are skipped. The root and the selected folder are saved in `%LOCALAPPDATA%\ZImageWin\folder.ini` and opened again at the next launch. If that folder is gone, the window says "The last folder could not be opened."

Folder view keeps up to 200 files. The Preview slider changes the drawn tile size from 80 to 480 dip, starting at 160. A tile's long side is at most 250 pixels, and a smaller picture stays at its own size. Videos wear a Video label and open with Windows. In Image view, Left and Right move to the previous or next still. Those keys stay put in Folder view.

Sub-folder, while it is on, also lists stills and videos in child folders. Until a folder is opened, the Disk column says "Open a folder to see its subfolders."

Right-click a thumbnail. A still offers Edit Image, Show in Explorer, Open In, Copy settings, Paste settings, and a size line. A video offers Open, Show in Explorer, Open With, the same copy and paste lines, and the size line. Paste keeps each destination crop. When the clicked file is already marked, paste applies to every marked still. Ctrl+right-click toggles the mark and leaves the menu closed. Image-view right-click zooms out, and is set aside while Select is armed.

Double-click a still, or choose Image, and that photograph opens in Image view.

## Image

A photograph fits the window when it opens. Zoom and pan stay until the next photograph. Straighten, crop, quarter-turns, keystone, Horizon, corner pin, lens, export, a loaded grade, and touchup keep the current zoom.

The grade is linear sRGB. Exposure is a linear EV scale. Shadows, Highlights, Whites, Blacks, Brightness, and Contrast run in encoded sRGB and convert back to linear. Saturation, vibrance, temperature, the spectrum handles, texture, clarity, dehaze, and the skin adjustment stay linear after decode. A slider left at 0 leaves the picture alone. History records one step per mouse-up. Copy and paste carry the adjustment. Paste keeps the destination crop.

Switching back to Folder view rebuilds the open photo's tile from the adjusted picture. Other tiles stay turn-only. Quarter-turns rotate a tile immediately.

### Spectrum

The graph is display luminance, black on the left and white on the right. Shadows, Midtones, and Highlights are the three handles. The graph is the picture before those handles, so it stays put while a handle moves. The ranges outside Shadows and Highlights are dimmed. Double-click returns Shadows to black, Highlights to white, and Midtones to the middle of those two. The graph is empty until a photograph is open.

### Tone

Exposure, Brightness, Contrast, Highlights, Shadows, Whites, Blacks, Texture, Clarity, Dehaze, Vibrance, and Saturation. Auto writes Exposure, Contrast, Highlights, Shadows, Whites, and Blacks from the picture before the current tone, and records one History step named Auto. A second press does not stack. Reset returns to the start of History and leaves every later step, so Redo brings them back one at a time. Auto and Reset stay available while the section is collapsed.

### Color

Temperature, Tint, a White/Skin switch, an eyedropper, and in Skin the seven presets with swatches. The hint is "Click the image to sample." White samples an 11-pixel window after geometry and before the tone stack, then sets Temperature and Tint. Skin samples a 21-pixel window and shifts the sample toward the preset. History says White, or White, varied sample, or the preset name.

### Geometry

Straighten, Vertical, Horizontal, and Sharpness. Select draws a marquee. Crop nests that rectangle into the stored crop, after geometry and sharpness and before the tone stack, and records one History step named Crop. A full-frame crop stores no crop and adds no step. Undo, redo, and a history click clear the marquee.

Horizon is two clicks. The line's tilt is added to the current quarter-turn and Straighten, and one History step is named Horizon. Corner pin starts at the frame corners. Its eight sliders run from -0.5 to 1.5, and while it is on, Vertical and Horizontal wait. Lens offers None, two super-wide adapters, and panoramic 120° and 180°. Choosing one copies its k1, k2, and field of view. k1 and k2 can move after that. Lens runs before perspective. Sharpness runs on the picture before the crop.

### Touchup

Dust, Scratches, Spot, Scratch, and the size and amount sliders. The repair list holds 64 entries, shared by those four tools. The hint under the controls is 11-point Segoe UI. Dust says "Finds dust and blends it into the surrounding photo." Spot says "Cover a speck with the circle and click it. Click Spot again to leave." Scratch says "Drag the circle along a scratch. Click Scratch again to leave."

Spot clicks queue while one removal runs. The canvas says "1 spot remaining." or "N spots remaining.", counting the one in progress. Each finished spot is drawn as soon as that picture is ready, and later spots keep running. A full list says "The repair list is full." While Scratch is dragged with the mouse down, a gray round-cap swath the brush diameter wide follows the stroke and clears when the mouse goes up.

Repairs are stored in the `.zimg` as `addInpaint` (`touchup-dust-v1` and `touchup-scratch-v1`). The fill is the CPU repair in this program. Object removal is absent from 1.0.0.

### History

The log holds 48 steps. Undo, redo, and a click on a step return there.

## Save

Save (Ctrl+S) writes a schema-1 `.zimg` with `appVersion` 1.0.0. The original bytes are not embedded. The usual file is `%LOCALAPPDATA%\ZImageWin\edits\<digest>.zimg`. Save As (Ctrl+Shift+S) writes the `.zimg` you choose and remembers that path. A read-only photo folder asks with "The photo folder is read-only. Choose another folder, or use Save to keep the edits on this Mac."

Open File accepts a `.zimg`. The grade is applied when the original photograph's hash and size match. Load order is the remembered Save As path, then the local digest file, then a sibling `.zimg` of the same stem. Quarter-turns, tone, color, geometry, crop, and dust and scratch repairs survive a quit. Zoom and pan stay for the session.

## Export and print

The export list is JPEG, HEIC, TIFF, PNG, WebP, AVIF, JPEG XL, JPEG 2000, PSD, OpenEXR, BMP, GIF, TGA, PDF, and DNG.

This build writes JPEG at quality 0.90, 8-bit PNG, 8-bit TIFF (LZW when that encoder is present, otherwise uncompressed), BMP, GIF, TGA, and PDF. HEIC, WebP, and JPEG XL are written when Windows has that encoder. AVIF, JPEG 2000, PSD, OpenEXR, and DNG answer "<name> is not available on this PC." Export refuses a destination that is the source file. TIFF and PNG are 8-bit sRGB from the linear grade.

Print places one photograph on each page. Folder export uses the same formats for the marked stills. Export and print run the same grade as Image view.

## Server

Server reads the library on `\\192.168.1.147\BackupData\PhotoVideoAudio`. The helper answers TLS on `192.168.1.147` port 8742. The first answer pins the certificate. The pin is kept in `%LOCALAPPDATA%\ZImageWin\helper.ini`. Pair with Helper asks Windows to sign in to the share, then claims the pairing token. After a successful pair the token is stored with Windows data protection in `%LOCALAPPDATA%\ZImageWin\helper.auth`.

Photos is the folder grid of that share. It walks the selected folder and its children and keeps the first 200 stills and videos, the same cap as Disk. Review and Duplicates reuse that grid. Events and Reorganize are center panels. The date field and Set date sit at the bottom of the right column for Photos and Review.

While Server is selected, the toolbar shows the helper's current pass to the right of Server, for example "Making previews" or "Hashing previews". An empty phase hides the line. That read repeats every 10 seconds and does not start a scan. The health sentence stays in the center of an empty grid. Disk hides the pass line.

Clean, Set date, Remove from library, Reorganize Start, Trim, Restore, and Delete ask before files move. Plan, Pause, Find, and Pin do not ask. Find is the call that starts a duplicate scan. Remove from library is on the Server Photos context menu. The helper performs the file moves. This program does not open the catalog database.

## Limits of 1.0.0

Grades are linear sRGB on the CPU. TIFF and PNG are 8-bit. Object removal is absent. The NAS helper is the program already running on the library share.
