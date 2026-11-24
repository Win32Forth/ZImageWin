/* Disk folder actions. Helper bodies are built here.
   Pass helper_send 0 unless the user clicked a server action.
   Do not call the live helper from a test. */
#ifndef ZIMAGEWIN_FOLDERS_H
#define ZIMAGEWIN_FOLDERS_H

#include "app.h"

int zi_folder_bad_name(const wchar_t *name);
int zi_folder_reserved(const wchar_t *relative);

/* Unchecked creates the named folder under the tree root. Checked creates it inside the open folder.
   A file already in the destination stays. Same names become name-2. Refuses _Removed, _Duplicates, and _Review.
   Does not call the helper. */
int zi_disk_move_marked(ZiApp *app, const wchar_t *folder_name, int subfolder);

/* 8-bit TIFF in that folder, {first}-{second}.tiff, then -2 through -999.
   vertical stacks the second photo under the first. shift is pixels on the second photo.
   Overlap blends. A gap stays black. A slide past the edge copies the nearest pixels. Originals stay. */
int zi_merge_write(const wchar_t *directory, const wchar_t *left_path, const wchar_t *right_path,
                   int vertical, int shift_x, int shift_y, wchar_t *written, size_t written_cap);

/* Renames on disk. Capitalization-only is allowed. A taken name is refused.
   When helper_send is 1 and the file is under Photos, POST /v1/files/rename
   {"from","to"} with a 180000 ms timeout. A second try is asked once.
   Photos/_Removed, Photos/_Duplicates, and Photos/_Review keep their names. */
int zi_rename_file(ZiApp *app, const wchar_t *path, const wchar_t *new_name, int helper_send);
int zi_rename_folder(ZiApp *app, const wchar_t *path, const wchar_t *new_name, int helper_send);

/* Thumbs.db, ehthumbs.db, desktop.ini, .DS_Store, and AppleDouble ._ files, then empty directories.
   A reparse point is left in place and is not followed. Photos itself stays.
   allow_server must be 1 before a server path is walked. */
int zi_clean_junk(const wchar_t *root, int allow_server);

/* Deletes files under folder, including subfolders, and leaves the folder. */
int zi_delete_tree_files(const wchar_t *folder);

int zi_write_tiff_file(const wchar_t *path, HBITMAP dib);
/* photo.jpg -> photo.zflat beside it. duplicate writes a -2.tiff and does not replace the flat. */
int zi_flatten_tiff(const wchar_t *photo_path, HBITMAP dib, int duplicate, wchar_t *written, size_t cap);
int zi_flatten_path(const wchar_t *photo_path, wchar_t *dest, size_t cap);
int zi_revert_flat(const wchar_t *photo_path);

/* No date returns 0. A dated photo returns Photos/YYYY/YYYY-MM-DD. */
int zi_organize_dest(const wchar_t *capture, wchar_t *dest, size_t cap);

int zi_json_rescan_page(char *dest, size_t cap, const char *prefix, const char **paths, int count);
int zi_json_rename(char *dest, size_t cap, const char *from, const char *to);
int zi_json_confirm(char *dest, size_t cap, const char *capture_yyyy_mm_dd);

/* Exactly two marked stills. vertical stacks the second photo. Shift starts at zero. */
int zi_merge_arm(ZiApp *app, int vertical);
void zi_merge_cancel(ZiApp *app);
void zi_merge_paint(ZiApp *app, HDC hdc);
/* 1 when both photos of the open join have been read. */
int zi_merge_ready(void);
int zi_merge_dragging(void);
/* Arrow keys move the second photo one pixel. Returns 1 when the key was used. */
int zi_merge_key(ZiApp *app, int vk);
/* down starts a click. alt zooms out. space drags the whole picture. */
int zi_merge_pointer(ZiApp *app, int x, int y, int down, int alt, int space);
/* Writes the TIFF and leaves the originals. */
int zi_merge_accept(ZiApp *app);
/* Fit, or one image pixel per point. A second 100% click recenters. */
void zi_merge_fit(ZiApp *app, int full);
/* Moves the stills on this page into year and day folders under the tree root. */
int zi_disk_organize(ZiApp *app);

#endif
