/* Edit view. Grades are stored as a .zimg document. The photograph file is not rewritten. */
#ifndef ZIMAGEWIN_EDITOR_H
#define ZIMAGEWIN_EDITOR_H

#include "app.h"

#define ZI_WM_PHOTO (WM_APP + 3)
#define ZI_WM_FACTS (WM_APP + 4)
#define ZI_WM_GRADE (WM_APP + 6)
#define ZI_WM_AUTO (WM_APP + 7)
#define ZI_WM_DROP (WM_APP + 8)
#define ZI_WM_DIGEST (WM_APP + 9)
#define ZI_WM_TOUCH (WM_APP + 10)

typedef struct ZiPhotoNote {
    LONG generation;
    int failed;
    int w;
    int h;
    HBITMAP bitmap;
} ZiPhotoNote;

typedef struct ZiFactNote {
    LONG generation;
    wchar_t path[MAX_PATH];
    int count;
    wchar_t lines[8][260];
} ZiFactNote;

typedef struct ZiGradeNote {
    LONG serial;
    int w;
    int h;
    int has_bins;
    int bins[ZI_SPECTRUM_BINS];
    HBITMAP bitmap;
    int touch_count;
    ZiTone tone;
} ZiGradeNote;

typedef struct ZiAutoNote {
    LONG serial;
    int ok;
    double exposure;
    double contrast;
    double highlights;
    double shadows;
    double whites;
    double blacks;
} ZiAutoNote;

typedef struct ZiDropNote {
    LONG serial;
    int ok;
    int skin;
    int varied;
    int preset;
    double temperature;
    double tint;
    double delta_a;
    double delta_b;
} ZiDropNote;

typedef struct ZiTouchNote {
    LONG generation;
    int kind;
    int found;
    unsigned char *mask;
    int w;
    int h;
} ZiTouchNote;

typedef struct ZiDigestNote {
    LONG generation;
    int ok;
    wchar_t path[MAX_PATH];
    unsigned char digest[32];
    unsigned long long size;
} ZiDigestNote;

void zi_editor_shutdown(ZiApp *app);
void zi_editor_open_path(ZiApp *app, const wchar_t *path);
void zi_editor_open_selection(ZiApp *app);
int zi_editor_step(ZiApp *app, int delta);
void zi_editor_sync_details(ZiApp *app);
void zi_editor_on_photo(ZiApp *app, ZiPhotoNote *note);
void zi_editor_on_facts(ZiApp *app, ZiFactNote *note);
void zi_editor_on_grade(ZiApp *app, ZiGradeNote *note);
void zi_editor_on_auto(ZiApp *app, ZiAutoNote *note);
void zi_editor_on_drop(ZiApp *app, ZiDropNote *note);
void zi_editor_on_digest(ZiApp *app, ZiDigestNote *note);
void zi_editor_on_touch(ZiApp *app, ZiTouchNote *note);
void zi_editor_set_slider(ZiApp *app, int index, double value);
void zi_editor_slider_commit(ZiApp *app);
void zi_editor_slider_reset(ZiApp *app, int index);
/* 0 Shadows, 1 Midtones, 2 Highlights. Midtones returns to the middle of the other two. */
void zi_editor_spectrum_reset(ZiApp *app, int handle);
void zi_editor_history_select(ZiApp *app, int index);

int zi_editor_paint(ZiApp *app, HDC hdc);
void zi_editor_paint_details(ZiApp *app, HDC hdc);

int zi_editor_can_undo(const ZiApp *app);
int zi_editor_can_redo(const ZiApp *app);
int zi_editor_can_copy(const ZiApp *app);
int zi_editor_can_paste(const ZiApp *app);
int zi_editor_can_rotate(const ZiApp *app);
int zi_editor_can_export(const ZiApp *app);
int zi_editor_can_crop(const ZiApp *app);
int zi_editor_has_photo(const ZiApp *app);
int zi_editor_can_reset(const ZiApp *app);

void zi_editor_auto(ZiApp *app);
void zi_editor_reset(ZiApp *app);

void zi_editor_toggle_select(ZiApp *app);
void zi_editor_toggle_eyedropper(ZiApp *app);
void zi_editor_toggle_horizon(ZiApp *app);
void zi_editor_set_corner_pin(ZiApp *app, int enabled);
void zi_editor_choose_lens(ZiApp *app, int index);
void zi_editor_apply_crop(ZiApp *app);
void zi_editor_use_zoom(ZiApp *app);
void zi_editor_touch_dust(ZiApp *app);
void zi_editor_touch_scratches(ZiApp *app);
void zi_editor_toggle_spot(ZiApp *app);
void zi_editor_toggle_scratch(ZiApp *app);
void zi_editor_touch_size_set(ZiApp *app, int px, int commit);
void zi_editor_touch_amount_begin(ZiApp *app, int which);
void zi_editor_touch_amount_set(ZiApp *app, int which, float strength);
void zi_editor_touch_amount_end(ZiApp *app);
void zi_editor_touch_move(ZiApp *app, int x, int y);
void zi_editor_scratch_finish(ZiApp *app);
int zi_editor_has_dust(const ZiApp *app);
int zi_editor_has_scratch(const ZiApp *app);
float zi_editor_dust_strength(const ZiApp *app);
float zi_editor_scratch_strength(const ZiApp *app);

void zi_editor_undo(ZiApp *app);
void zi_editor_redo(ZiApp *app);
void zi_editor_copy(ZiApp *app);
void zi_editor_copy_path(ZiApp *app, const wchar_t *path);
void zi_editor_paste(ZiApp *app);
void zi_editor_paste_index(ZiApp *app, int index);
void zi_editor_rotate(ZiApp *app, int clockwise);
void zi_editor_export(ZiApp *app);
void zi_editor_save(ZiApp *app);
void zi_editor_save_as(ZiApp *app);
void zi_editor_open_zimg(ZiApp *app, const wchar_t *path);
void zi_editor_print(ZiApp *app);
void zi_editor_unavailable(ZiApp *app, int id);

void zi_editor_apply_fit(ZiApp *app);
void zi_editor_format_zoom(ZiApp *app);
double zi_editor_zoom_pos(const ZiApp *app);
void zi_editor_set_zoom_pos(ZiApp *app, double t);
int zi_editor_mouse_down(ZiApp *app, int x, int y, int right);
void zi_editor_mouse_move(ZiApp *app, int x, int y);
void zi_editor_crop_end(ZiApp *app);
HCURSOR zi_editor_cursor(const ZiApp *app, int x, int y);
void zi_editor_wheel(ZiApp *app, int x, int y, int delta);
void zi_editor_note_thumb(ZiFile *file);
/* The open photo's folder tile shows the adjusted picture, or the original thumbnail again. */
void zi_editor_match_folder_tile(ZiApp *app);
/* A newly decoded thumbnail is the original. Keep it, then match the open photo. */
void zi_editor_on_decoded_thumb(ZiApp *app, ZiFile *file);

#endif
