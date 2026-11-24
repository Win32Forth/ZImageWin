# ZImager 1.8.6 for Windows

ZImager 1.8.6 is the app. It matches the Mac marketing version in the sibling `ZImage` tree. One window. Disk is the editor. Server is the library. The original photograph is left where it is.

Assembly identity is `Win32Forth.ZImager` 1.8.6.0. The installers are `dist\ZImager-1.8.6-arm64.exe` and `dist\ZImager-1.8.6-x64.exe`. A `.zimg` file stays schema 1 and writes `appVersion` `1.0.0`, the same envelope the Mac writes. Grades are linear sRGB on the CPU. Windows 10 and Windows 11 are the two targets, on ARM64 and on x64. Both builds compile the same C sources.

The NAS helper stays the Go program already installed with the library. This tree does not build it and the installer does not ship it.

## Do not use yet

Reorganize, Review, and Duplicates can lose photos. The Events and Reorganize buttons are hidden. Leave Review and Duplicates alone. Do not press Start, Pause, Continue, Find duplicates, or Delete all duplicates.

Server Move, Organize, Clean, Rescan, and Rename show a notice and leave the library where it is. Delete ALL, on `Photos/_Removed` only, asks "Are you sure, all _Removed photos will be deleted from the server?" Yes deletes the files in that folder through the share and leaves the folder. That button was not pressed in this build's tests.

## Install

Use the installer that matches the PC:

- `dist\ZImager-1.8.6-arm64.exe` on ARM64 Windows
- `dist\ZImager-1.8.6-x64.exe` on x64 Windows

Each installer is per user. It does not ask for an administrator password. ZImager.exe lands in `%LOCALAPPDATA%\Programs\ZImager-ARM64` or `%LOCALAPPDATA%\Programs\ZImager-x64`, with a Start menu shortcut named ZImager ARM64 or ZImager x64. The desktop icon task is offered and left unchecked.

Uninstall removes the program. Edits, the helper pin, and the last folder stay in `%LOCALAPPDATA%\ZImageWin`. The 1.0.0 installers remain in `dist` for the previous release.

## Build

The Visual Studio C tools build the executables. Inno Setup 6 builds the installers.

```bat
build.bat
build-x64.bat
build-test.bat
build-test-x64.bat
Install\build-installers.bat
```

`build.bat` writes `build\arm64\ZImager.exe`. `build-x64.bat` writes `build\x64\ZImager.exe`. The two test scripts compile the grade checks in `src\features.c` and print `ok`. File properties say 1.8.6. The embedded manifest says 1.8.6.0. `Install\build-installers.bat` reads those two programs and writes the 1.8.6 installers.

## Changes since 1.0.0

These are the Mac 1.7.1 through 1.8.6 additions now in this Windows build. The sections below describe how they work here.

1.7.1. Each repair is its own history step. Spot clicks queue while one removal runs. The shared repair list holds 64 entries. The canvas says how many spots remain, including the one running. Undo, redo, reset, and opening another photograph clear the queue.

1.7.2. Double-click a video in Disk folder view or Server Photos and it plays on Image. Play, pause, and scrub sit on the picture. Tone, the spectrum, Temperature, Tint, and Auto grade the frames. The grade stays in memory for the session and is not written into the movie, a `.zimg`, or a folder JPEG. Playback is silent. Export writes a new MPEG-4 of the graded frames and leaves the original file. Geometry, Spot, Scratch, Sharpness, and the eyedropper stay off while a video is open. Right-click Open still uses the shell player. Play on the menu opens the in-app player. Horizon scales the two clicks by the on-screen width and height. Scratch fill continues the picture from both sides of the stroke. The Size slider runs from 8 to 200 preview pixels and defaults to 48. Double-click restores 48.

1.7.5, shipped here with 1.8.0. Tone ends with a hairline, then Texture, Clarity, Details, and Sharpness. Details uses radius coefficient 8. Sharpness is stored with geometry and shown at the end of Tone. X offset and Y offset sit under Horizontal. Positive X moves the adjusted photograph right. Positive Y moves it up. The uncovered band is clear. Auto still aims the middle at mid gray, leaves Contrast alone when raising it would darken that middle, and can raise Exposure by as much as 3 stops. Auto writes only Exposure, Contrast, Highlights, Shadows, Whites, and Blacks.

1.8.0. Spot and Scratch are one tool. A click repairs a spot. A drag repairs a scratch. Dust and Scratches stay separate buttons. Flatten writes `<stem>.zflat`, a TIFF, beside the photograph and keeps the original. Duplicate writes the first free `<stem>-2.tiff` through `<stem>-999.tiff` and keeps the edits. Flatten again asks first. Yes replaces the `.zflat` and clears the edit list. No restores the original and clears the edits. The Remove object button stays hidden. A PDF lists pages that contain a JPEG, and each thumbnail keeps that page's number. One click opens that page.

1.8.1. Red Eye sits to the right of Scratches. Click the red of each pupil. Each click is a history step titled Red eye. The black center is about 20% larger than the red that was found. A catchlight stays. A click on skin or lips does nothing. The repair is stored as `touchup-redeye-v1`. Find and Clear search the whole folder catalog. A folder of more than 200 stills pages 200 at a time. Page 0 also shows up to 300 videos. Videos stay out of the still count.

1.8.2. Folder view can move the selected files into a folder you name. On Disk, an unchecked sub-folder box creates that folder under the opened tree root. A checked box creates it inside the folder on screen. A file already there stays. The same name becomes name-2. `_Removed`, `_Duplicates`, and `_Review` are refused. On `Photos\_Removed`, Find and Clear are replaced by Delete ALL. Yes deletes every file under that folder, including subfolders, and leaves the folder. `Photos\_RemovedExtra` is left alone. Server Move says the action is left for a confirmed run.

1.8.3. Merge joins exactly two stills from the same folder into an 8-bit TIFF named `{first}-{second}.tiff`. The originals stay. Preview starts flush. Arrow keys move the second photo one pixel. Click zooms in. Alt-click zooms out and stops at fit. Space-drag pans. Fit and 100% are on the preview. Cancel writes nothing. After the preview closes, right-click Rename changes the clicked file's name in the same folder. The Events and Reorganize buttons are hidden. Keystone runs after turns and flips.

1.8.4. Four threads decode the page on screen. Disk previews are temporary JPEGs under `%TEMP%\ZImageWin\previews`, capped at 200 MB. Server previews stay on the NAS. The path beside Rescan ends with the still count. Date has Organize and Clean. Disk Organize asks, then reads the page on screen into year and day folders. A photo with no date goes to `Photos\_Review`. Clean on a local drive deletes thumbnail junk and folders that have nothing else. Photos itself stays. A reparse point is left in place. Right-click a folder in the tree and choose Rename. Photos and the three shelves keep their names. Disk rename stays on this PC. Server Organize, Clean, Rescan, and Rename say the action is left for a confirmed run.

1.8.5. Curve sits between Tone and Color. RGB, Red, Green, and Blue each have a curve. Linear, Matte, Film Moody, Punch, and Lo-fi set the RGB curve. Drag a handle. Click to add a point, up to eight. Double-click a middle handle to remove it. Double-click an end to put that corner back. Vignette sits under Touchup, with Amount, Midpoint, Roundness, Feather, X, and Y. Positive Y moves the clear center up. With no tool armed, a left drag pans, and the wheel zooms. Each folder panel folds from an arrow on its title. Build previews covers the open folder and skips videos. A Sigma X3F opens its embedded JPEG. The Sigma sensor converter is absent. X3I files are not opened. An older `.zimg` with no curve, vignette, or flip stays upright.

1.8.6. View has Flip Left/Right above Flip Top/Bottom, under the rotate commands. Each mirror is one history step, and the same command restores that edge. An odd turn swaps the two flags. A crop and a repair follow the photograph. Leaving Image for Folder returns to Zoom and releases a stay-clicked tool. Flatten is available for an open still, including an empty edit list, and stays off for a video, the PDF picker, and while Flatten or Export is writing. Automatic previews stop at the page on screen. Server Rescan is present as a client and does not call the helper in this build.

## The window

The toolbar switches Disk and Server, and Folder and Image. The menus are File, Edit, and View.

File: Open File (Ctrl+O), Open Folder, Save (Ctrl+S), Save As (Ctrl+Shift+S), Exit.

Edit: Undo (Ctrl+Z), Redo (Ctrl+Y), Copy (Ctrl+C), Paste (Ctrl+V).

View: Folder View (Ctrl+F), Edit View (Ctrl+E), Rotate Counterclockwise, Rotate Clockwise, Flip Left/Right, Flip Top/Bottom, Disk, Server, Sub-folder. The toolbar button for Edit View says Image. Flip and rotate have no accelerator. The eyedropper stays a toolbar tool.

The smallest window is 1100 by 700. The inspector is the column on the right. Its sections are Spectrum, Tone, Curve, Color, Geometry, Touchup, Vignette, and History.

Leaving Image for Folder returns to Zoom and releases a stay-clicked tool.

## Disk

Open Folder sets the root of an indented folder tree. A chevron opens the next level. Choosing a name lists that folder. Hidden names, names that start with a dot, and links are skipped. The root and the selected folder are saved in `%LOCALAPPDATA%\ZImageWin\folder.ini` and opened again at the next launch. If that folder is gone, the window says "The last folder could not be opened."

The folder catalog holds up to 20000 names. The 20001st name stops the walk and the window says "The folder shows the first 20000 names." Page 0 shows the first 200 stills and at most 300 videos, sorted by name. Later pages show 200 stills at a time. Videos stay off those later pages and out of the still count. More than 300 videos says "The folder shows the first 300 videos." Stills include X3F and PDF. Find searches the whole catalog. A miss says "No photos match that name." Clear shows the folder again. When more than one page exists, First, Prev 200, Next 200, and Last sit with "Page N of M".

The Preview slider changes the drawn tile size from 80 to 480 dip, starting at 160. A tile's long side is at most 250 pixels. Four threads decode the page on screen. Automatic previews stop at that page. Build previews, under Merge, writes 250-pixel JPEGs under `%TEMP%\ZImageWin\previews` for the open folder, with a 200 MB cap. Videos are left out. A name filter has to be cleared first. On Server the same button says "Server previews stay on the NAS." and does not upload.

Sub-folder, while it is on, also lists stills and videos in child folders. Until a folder is opened, the Disk column says "Open a folder to see its subfolders."

The right column in Folder view stacks folding panels: Date, Move selected files, Merge two photos, and Build previews. Each panel starts open. Preview is the title above the picture.

Date has Organize and Clean. Organize on Disk asks "Organize the photos on this page into year and day folders?" Yes reads the page on screen. A photo with no date goes to `Photos\_Review` under the tree root. A dated photo goes to `Photos\YYYY\YYYY-MM-DD`. A photo already in that year and day folder stays. A moved photo that matches another file of the same size leaves a 250-pixel JPEG in `Photos\_Duplicates`. Later pages are left where they are. Clean on a local drive asks "Delete empty folders and junk files in this folder?" Yes deletes Thumbs.db, ehthumbs.db, desktop.ini, .DS_Store, and AppleDouble `._` files, then a folder that has nothing else. The starting folder stays. A reparse point is left in place. A UNC path says "Clean on the server is left for a confirmed run."

Move sits between Date and Merge. The caption is "Move selected files to specified Folder". The checkbox "Sub-folder of this folder" starts unchecked. Unchecked, the named folder is created under the opened tree root. Checked, it is created inside the folder on screen. A file already in the destination stays. The same name becomes name-2. The names `_Removed`, `_Duplicates`, and `_Review` are refused. The tree reloads. Server Move says "Move on the server is left for a confirmed run."

Merge joins exactly two stills from the same folder. Horizontal or Vertical. The first clicked marked still is left or top when the anchor and the selection are two different marked stills. Otherwise the two marked stills are used in index order. Any other count says "Select two stills from this folder." Preview starts flush. Arrow keys, including Shift-arrow, move the second photo one pixel. Click zooms in. Alt-click zooms out and stops at fit. Space-drag moves the whole picture. Fit and 100% are on the preview. 100% is one image pixel per point, centered on the seam, and a second click recenters. Changing Horizontal or Vertical returns to a flush join. Cancel writes nothing. An overlap is blended. A gap stays black. A slide past the edge fills the clear band from the nearest pixels. The file is an 8-bit TIFF named `{first}-{second}.tiff` in that folder. A taken name becomes `{name}-2.tiff` through `{name}-999.tiff`. The originals stay. A join that would pass about 40 million pixels is scaled, and the notice says so.

Right-click a thumbnail. A still offers Edit Image, Show in Explorer, Open In, Copy settings, Paste settings, and a size line. A video offers Open, Play, Show in Explorer, Open With, the same copy and paste lines, and the size line. Play opens the in-app player. Open and Open With stay with the shell. After a merge preview is closed, Rename changes the clicked file's name in the same folder. Rename is hidden while the preview is up. An empty name, `.`, `..`, a slash, a colon, or a name the file system will not take is refused. A capitalization-only change is allowed. Paste keeps each destination crop. When the clicked file is already marked, paste applies to every marked still. Ctrl+right-click toggles the mark and leaves the menu closed. Image-view right-click zooms out, and is set aside while Select is armed.

Right-click a folder in the tree and choose Rename. Photos, `Photos\_Removed`, `Photos\_Duplicates`, and `Photos\_Review` keep their names. A Disk folder is renamed on this PC. The helper is not called. Server Rename says "Rename on the server is left for a confirmed run."

When the folder is `Photos\_Removed`, Find and Clear are replaced by Delete ALL. `Photos\_RemovedExtra` is left alone.

Double-click a still, or choose Image, and that photograph opens in Image view. Double-click a video and it plays on Image. A Sigma X3F opens its embedded JPEG and says "Opened the embedded preview. Sigma Photo Pro color is not applied." A file under 64 KB, or one with no JPEG, says "Opened the embedded preview. The Sigma converter is not on this PC." An X3I says "This photograph could not be opened." A PDF lists pages that contain a JPEG. Each thumbnail keeps that page's number. One click opens that page.

## Image

A photograph fits the window when it opens. Zoom and pan stay until the next photograph. Straighten, crop, quarter-turns, keystone, Horizon, corner pin, lens, export, a loaded grade, and touchup keep the current zoom. With no tool armed, a left drag pans. The wheel zooms. Right-click zooms out.

The grade is linear sRGB. Exposure is a linear EV scale. Shadows, Highlights, Whites, Blacks, Brightness, and Contrast run in encoded sRGB and convert back to linear. Saturation, vibrance, temperature, the spectrum handles, texture, clarity, details, dehaze, the curve, vignette, and the skin adjustment stay in that pipeline. A slider left at 0 leaves that control alone. History records one step per mouse-up. Copy and paste carry the adjustment. Paste keeps the destination crop.

Switching back to Folder view rebuilds the open photo's tile from the adjusted picture. Other tiles stay turn-only. Quarter-turns rotate a tile immediately. An odd turn swaps Flip Left/Right with Flip Top/Bottom. A crop and a repair follow the photograph.

### Spectrum

The graph is display luminance, black on the left and white on the right. Shadows, Midtones, and Highlights are the three handles. The graph is the picture before those handles, so it stays put while a handle moves. The ranges outside Shadows and Highlights are dimmed. Double-click returns Shadows to black, Highlights to white, and Midtones to the middle of those two. The graph is empty until a photograph is open.

### Tone

Exposure, Brightness, Contrast, Highlights, Shadows, Whites, Blacks, Vibrance, and Saturation, then a hairline, then Texture, Clarity, Details, and Sharpness. Details uses radius coefficient 8. Sharpness is stored with geometry and shown here. Flatten sits to the left of Auto and Reset.

Flatten is available for an open still, including an empty edit list. It stays off for a video, the PDF page picker, and while Flatten or Export is writing. An empty library id asks before a duplicate TIFF. The first flatten asks with Yes, No, and Cancel. Yes writes `<stem>.zflat` beside the photograph, a TIFF, and keeps the edits. No writes the first free `<stem>-2.tiff` through `<stem>-999.tiff` and keeps the edits. Again asks "Flatten this photograph again?" Yes replaces the `.zflat` and clears the edit list. No restores the original and clears the edits. Cancel leaves both. The original file stays.

Auto writes Exposure, Contrast, Highlights, Shadows, Whites, and Blacks. It aims the middle at mid gray. Contrast is left alone when raising it would darken that middle. Exposure can rise by as much as 3 stops, held back by the 85th percentile. Blacks stay put while the middle is below the target. A second press does not stack. Reset returns to the start of History and leaves every later step, so Redo brings them back one at a time. Auto and Reset stay available while the section is collapsed. Reset also clears Details, the curves, and the vignette amount, and puts the vignette midpoint and feather back to 0.5. Flips and offsets stay.

### Curve

Curve sits between Tone and Color. The graph shows brightness. RGB shapes the whole picture. Red, Green, and Blue shape one channel after that. Linear, Matte, Film Moody, Punch, and Lo-fi set the RGB curve and clear the channel curves. Drag a handle. Click the curve to add a point, up to eight. Double-click a middle handle to remove it. Double-click an end to put that corner back. The curve is applied after the spectrum. History says Curve, Red curve, Green curve, or Blue curve.

### Color

Temperature, Tint, a White/Skin switch, an eyedropper, and in Skin the seven presets with swatches. The hint is "Click the image to sample." White samples an 11-pixel window after geometry and before the tone stack, then sets Temperature and Tint. Skin samples a 21-pixel window and shifts the sample toward the preset. History says White, or White, varied sample, or the preset name. The eyedropper stays off while a video is open.

### Geometry

Straighten, Vertical, Horizontal, X offset, and Y offset. Positive X moves the adjusted photograph right. Positive Y moves it up. The uncovered band is clear. Select draws a marquee. Crop nests that rectangle into the stored crop, after geometry and sharpness and before the tone stack, and records one History step named Crop. A full-frame crop stores no crop and adds no step. Undo, redo, and a history click clear the marquee.

Horizon is two clicks. The clicks are scaled by the on-screen width and height before the angle is measured. The line's tilt is added to the current quarter-turn and Straighten, and one History step is named Horizon. Corner pin starts at the frame corners. Its eight sliders run from -0.5 to 1.5, and while it is on, Vertical and Horizontal wait. Lens offers None, two super-wide adapters, and panoramic 120° and 180°. Choosing one copies its k1, k2, and field of view. k1 and k2 can move after that. Lens runs before perspective. Turns and flips run before keystone. Sharpness runs on the picture before the crop, and its slider is at the end of Tone. Geometry, Spot, Scratch, and the eyedropper stay off while a video is open. Sharpness is disabled then too.

Flip Left/Right mirrors the left and right edges. Flip Top/Bottom mirrors the top and bottom. The same command restores that edge. Each is one history step.

### Touchup

Dust, Scratches, and Red Eye share the first row. Spot is the full-width button under them. Spot and Scratch are one tool: a click repairs a spot, and a drag repairs a scratch. The automatic Dust and Scratches buttons stay separate. The repair list holds 64 entries. The hint under the controls is 11-point Segoe UI. Dust says "Finds dust and blends it into the surrounding photo." Spot says "Click a spot, or drag along a scratch. Click Spot again to leave." Red Eye says "Click the red in each eye. Click Red Eye again to leave."

The Size slider runs from 8 to 200 preview pixels. The default is 48. Double-click restores 48. Scratch fill continues the picture from both sides of the stroke. Disks along the stroke are about a quarter of the radius apart. A round end copies the near side. While the stroke is down, a gray swath follows it.

Spot clicks queue while one removal runs. The canvas says "1 spot remaining." or "N spots remaining.", counting the one in progress. Each finished spot is drawn as soon as that picture is ready. A full list says "The repair list is full." Undo, redo, reset, and opening another photograph clear the queue.

Red Eye sits to the right of Scratches. Click the red of each pupil. Each click is a history step titled "Red eye". The black center is about 20% larger than the red that was found. A catchlight stays. A click on skin or lips does nothing.

Repairs are stored in the `.zimg` as `addInpaint` (`touchup-dust-v1`, `touchup-scratch-v1`, and `touchup-redeye-v1`). The fill is the CPU repair in this program. The Remove object button stays hidden.

### Vignette

Vignette sits under Touchup. Amount, Midpoint, Roundness, Feather, X, and Y. Amount starts at 0, midpoint and feather at 0.5, and the center at 0, 0. Amount 0 leaves the picture alone. Positive Y moves the clear center up. Dragging the photograph moves the vignette center while this section is the one on screen.

### History

The log holds 48 steps. Undo, redo, and a click on a step return there.

## Video

A double-click plays the video on Image. Play, pause, and a scrub track sit on the picture. Space toggles play while the main window has focus. Tone, the spectrum, Temperature, Tint, and Auto grade the frames on screen. The grade stays in memory for the session. Reopening the same video in that session restores it. The grade is not written into the movie, a `.zimg`, or a folder JPEG. Playback is silent. Export writes a new MPEG-4 of the graded frames and leaves the original file unchanged. Choosing the source path says "Export writes a new file. Choose a different name." A file this PC cannot play says "This video could not be played on this PC."

## Save

Save (Ctrl+S) writes a schema-1 `.zimg` with `appVersion` 1.0.0. The original bytes are not embedded. The usual file is `%LOCALAPPDATA%\ZImageWin\edits\<digest>.zimg`. Save As (Ctrl+Shift+S) writes the `.zimg` you choose and a sibling `.ref`, and remembers that path. A read-only photo folder asks with "The photo folder is read-only. Choose another folder, or use Save to keep the edits on this Mac."

Open File accepts a `.zimg`. The grade is applied when the original photograph's hash and size match. Load order is the remembered Save As path, then the local digest file, then a sibling `.zimg` of the same stem. Quarter-turns, flips, offsets, tone, curves, vignette, color, geometry, crop, and dust, scratch, and red-eye repairs survive a quit. An older file with no flip, curve, or vignette stays upright and unvignetted. Zoom and pan stay for the session.

## Export and print

The export list is JPEG, HEIC, TIFF, PNG, WebP, AVIF, JPEG XL, JPEG 2000, PSD, OpenEXR, BMP, GIF, TGA, PDF, and DNG.

This build writes JPEG at quality 0.90, 8-bit PNG, 8-bit TIFF (LZW when that encoder is present, otherwise uncompressed), BMP, GIF, TGA, and PDF. HEIC, WebP, and JPEG XL are written at quality 0.80 when Windows has that encoder. AVIF, JPEG 2000, PSD, OpenEXR, and DNG answer "<name> is not available on this PC." Export refuses a destination that is the source file. TIFF and PNG are 8-bit sRGB from the linear grade.

Print places one photograph on each page. Folder export uses the same formats for the marked stills. Export and print run the same grade as Image view. A video export uses the MPEG-4 writer above.

## Server

Server reads the library on `\\192.168.1.147\BackupData\PhotoVideoAudio`. The helper answers TLS on `192.168.1.147` port 8742. The first answer pins the certificate. The pin is kept in `%LOCALAPPDATA%\ZImageWin\helper.ini`. Pair with Helper asks Windows to sign in to the share, then claims the pairing token. After a successful pair the token is stored with Windows data protection in `%LOCALAPPDATA%\ZImageWin\helper.auth`.

Photos uses the same folder grid, paging, Find, and fold panels as Disk. Review and Duplicates still have code paths. Events and Reorganize are hidden. The date field and Set date sit at the bottom of the right column for Photos and Review. The path beside Rescan ends with the still count, as `(12345)`, once counting finishes. While the list is counting, the title says "Counting photos…".

While Server is selected, the toolbar shows the helper's current pass to the right of Server, for example "Making previews" or "Hashing previews". An empty phase hides the line. That read repeats every 10 seconds and does not start a scan. The health sentence stays in the center of an empty grid, for example "192.168.1.147   Helper online. Safe to reboot." Disk hides the pass line.

Organize, Clean, Rescan, Move, and Rename on Server show "… is left for a confirmed run." and do not call the helper. The client can build the page-rescan, rename, and confirm JSON. Those routes were not sent. Rescan does not show a progress bar because it does not start. This program does not open the catalog database.

## Limits

Grades are linear sRGB on the CPU. TIFF and PNG are 8-bit. There is no DNG writer. An X3F opens the embedded JPEG. The Sigma sensor converter is not in this build. Object removal is absent. Graded video playback is silent. Disk Organize reads the page on screen. A PDF page opens on one click of its thumbnail. The window was not launched for this source update. The grade checks in `src\features.c` were compiled and printed `ok` on ARM64 and on x64.
