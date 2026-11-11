/* Library actions. The helper moves the files. This window confirms first. */
#ifndef ZIMAGEWIN_LIBRARY_H
#define ZIMAGEWIN_LIBRARY_H

#include "app.h"

#define ZI_WM_LIBRARY (WM_APP + 11)

void zi_library_startup(HWND hwnd);
void zi_library_shutdown(void);
void zi_library_layout(ZiApp *app);
void zi_library_paint_center(ZiApp *app, HDC hdc);
void zi_library_paint_side(ZiApp *app, HDC hdc);
int zi_library_hittest(const ZiApp *app, int x, int y, int *extra);
void zi_library_click(ZiApp *app, int id, int extra);
void zi_library_wheel(ZiApp *app, int delta);
void zi_library_on_timer(ZiApp *app);
void zi_library_on_note(ZiApp *app, void *note);
/* GET /v1/duplicates/scan for the toolbar line. Does not start a scan. */
void zi_library_refresh_pass(ZiApp *app);
void zi_library_clear_pass(ZiApp *app);
int zi_library_date_visible(const ZiApp *app);
int zi_library_clean_ready(const ZiApp *app);
int zi_library_can_remove(const ZiApp *app, int index);
void zi_library_show_photos(ZiApp *app);
void zi_library_open_section(ZiApp *app, int section);
void zi_library_toggle_events(ZiApp *app);
void zi_library_toggle_reorg(ZiApp *app);
void zi_library_clean(ZiApp *app);
void zi_library_remove(ZiApp *app, int index);

#endif
