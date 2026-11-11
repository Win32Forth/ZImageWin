#ifndef ZIMAGEWIN_GRID_H
#define ZIMAGEWIN_GRID_H

#include "app.h"

#define ZI_WM_THUMB (WM_APP + 2)

typedef struct ZiThumbNote {
    LONG generation;
    int index;
    int failed;
    int w;
    int h;
    HBITMAP bitmap;
} ZiThumbNote;

#define ZI_WM_LIST (WM_APP + 5)

typedef struct ZiListNote {
    LONG generation;
    int failed;
    int capped;
    int count;
    ZiFile *files;
} ZiListNote;

void zi_grid_load(ZiApp *app);
void zi_grid_shutdown(ZiApp *app);
int zi_grid_active(const ZiApp *app);
/* Keeps helper order. file_dir is left unchanged. The caller frees nothing; this takes files. */
void zi_grid_install(ZiApp *app, ZiFile *files, int count);
/* Drops the grid and file_dir so the next Photos walk reads the folder again. */
void zi_grid_clear(ZiApp *app);
void zi_grid_paint(ZiApp *app, HDC hdc);
void zi_grid_thumb(ZiApp *app, ZiThumbNote *note);
void zi_grid_accept(ZiApp *app, ZiListNote *note);
int zi_grid_click(ZiApp *app, int x, int y);
void zi_grid_wheel(ZiApp *app, int delta);
/* Places the Folder-view scrollbar. Hidden outside the file grid. */
void zi_grid_sync_bar(ZiApp *app);
/* Handles WM_VSCROLL for the file-grid bar. Returns 0 for any other bar. */
int zi_grid_on_vscroll(ZiApp *app, HWND bar, WPARAM wparam);
int zi_grid_preview_hit(ZiApp *app, int x, int y);
void zi_grid_preview_drag(ZiApp *app, int x);
int zi_grid_index_at(ZiApp *app, int x, int y);
void zi_grid_select_only(ZiApp *app, int index);

#endif
