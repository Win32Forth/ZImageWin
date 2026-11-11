#ifndef ZIMAGEWIN_CHROME_H
#define ZIMAGEWIN_CHROME_H

#include "app.h"

void zi_chrome_layout(ZiApp *app, HDC hdc, const RECT *client);
void zi_chrome_paint(ZiApp *app, HDC hdc, const RECT *client);
/* Returns a control id. extra receives the tree or section index when those are hit. */
int zi_chrome_hittest(const ZiApp *app, int x, int y, int *extra);
/* 0 Shadows, 1 Midtones, 2 Highlights. -1 when the pointer is not on a handle. */
int zi_spectrum_handle_at(const ZiApp *app, int x, int y);
/* Handles WM_VSCROLL for the Disk and Server folder tree. Returns 0 for any other bar. */
int zi_chrome_on_vscroll(ZiApp *app, HWND bar, WPARAM wparam);

#endif
