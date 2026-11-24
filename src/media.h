/* Video playback, X3F embedded JPEG, and PDF embedded JPEG pages.
   This header stays free of Media Foundation includes so app.c can use it.
   app.c already includes initguid.h. media.c must not. */
#ifndef ZIMAGEWIN_MEDIA_H
#define ZIMAGEWIN_MEDIA_H

#include "app.h"

int zi_media_is_video(const wchar_t *path);
/* Image tab. The grade stays in memory for the session and is not written into the movie. */
int zi_media_open(ZiApp *app, const wchar_t *path);
void zi_media_close(ZiApp *app);
void zi_media_toggle(ZiApp *app);
void zi_media_seek(ZiApp *app, double fraction);
int zi_media_playing(const ZiApp *app);
int zi_media_paint(ZiApp *app, HDC hdc);
void zi_media_on_grade(ZiApp *app);
/* A new MPEG-4. The original file is not changed. */
int zi_media_export(ZiApp *app, const wchar_t *dest);
void zi_media_shutdown(void);
/* Timer id 3. Advances a playing movie. */
void zi_media_on_timer(ZiApp *app);
/* Play, pause, and the scrub bar on the picture. Returns 1 when the click was used. */
int zi_media_click(ZiApp *app, int x, int y);

/* Copies the best embedded JPEG. Does not develop the sensor.
   On success with a preview, notice is
   "Opened the embedded preview. Sigma Photo Pro color is not applied."
   A file under 64 KB, or no JPEG, sets
   "Opened the embedded preview. The Sigma converter is not on this PC."
   when a notice buffer is provided. Returns 1 when jpeg is set. */
int zi_x3f_embedded_jpeg(const wchar_t *path, unsigned char **jpeg, size_t *len,
                         wchar_t *notice, size_t notice_cap);

/* pages[i] is that thumbnail's own 1-based page number. Never a constant page. */
int zi_pdf_list_pages(const wchar_t *path, int **pages, int *count);
int zi_pdf_page_jpeg(const wchar_t *path, int page, unsigned char **jpeg, size_t *len);

#endif
