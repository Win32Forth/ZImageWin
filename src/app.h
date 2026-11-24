/* Window state for the ZImager shell. One window, as in ZImager/App/ZImageApp.swift. */
#ifndef ZIMAGEWIN_APP_H
#define ZIMAGEWIN_APP_H

#include <windows.h>

#include "palette.h"
#include "features.h"

#define ZI_MAX_FOLDERS 400
#define ZI_MAX_FILES   500
#define ZI_NAME        260
#define ZI_CTRL_CAP    320
#define ZI_SECTIONS    8
#define ZI_SEC_SPECTRUM 0
#define ZI_SEC_TONE 1
#define ZI_SEC_CURVE 2
#define ZI_SEC_COLOR 3
#define ZI_SEC_GEOMETRY 4
#define ZI_SEC_TOUCHUP 5
#define ZI_SEC_VIGNETTE 6
#define ZI_SEC_HISTORY 7

enum {
    ZI_ID_NONE = 0,
    ZI_ID_MENU,
    ZI_ID_OPEN_FILE,
    ZI_ID_OPEN_FOLDER,
    ZI_ID_SOURCE_DISK,
    ZI_ID_SOURCE_SERVER,
    ZI_ID_UNDO,
    ZI_ID_REDO,
    ZI_ID_COPY,
    ZI_ID_PASTE,
    ZI_ID_EVENTS,
    ZI_ID_REORG,
    ZI_ID_SUBFOLDER,
    ZI_ID_VIEW_FOLDER,
    ZI_ID_VIEW_IMAGE,
    ZI_ID_ROT_LEFT,
    ZI_ID_ROT_RIGHT,
    ZI_ID_FOLDER_PASTE,
    ZI_ID_FOLDER_EXPORT,
    ZI_ID_FOLDER_PRINT,
    ZI_ID_SELECT,
    ZI_ID_CROP,
    ZI_ID_EXPORT,
    ZI_ID_PRINT,
    ZI_ID_ZOOM,
    ZI_ID_REMOVE,
    ZI_ID_LIB,
    ZI_ID_SPLIT,
    ZI_ID_SECTION,
    ZI_ID_TREE,
    ZI_ID_ZOOM_TRACK,
    ZI_ID_SLIDER,
    ZI_ID_SPECTRUM,
    ZI_ID_HISTORY,
    ZI_ID_AUTO,
    ZI_ID_RESET,
    ZI_ID_COLOR_WHITE,
    ZI_ID_COLOR_SKIN,
    ZI_ID_EYEDROPPER,
    ZI_ID_SKIN_MENU,
    ZI_ID_SWATCH,
    ZI_ID_COLOR_HINT,
    ZI_ID_HORIZON,
    ZI_ID_CORNER_PIN,
    ZI_ID_LENS_MENU,
    ZI_ID_GEO_HINT,
    ZI_ID_CORNER_TITLE,
    ZI_ID_DUST,
    ZI_ID_SCRATCHES,
    ZI_ID_SPOT,
    ZI_ID_SCRATCH,
    ZI_ID_TOUCH_SIZE,
    ZI_ID_DUST_AMOUNT,
    ZI_ID_SCRATCH_AMOUNT,
    ZI_ID_TOUCH_HINT,
    ZI_ID_FLATTEN,
    ZI_ID_REDEYE,
    ZI_ID_CURVE_GRAPH,
    ZI_ID_CURVE_RGB,
    ZI_ID_CURVE_RED,
    ZI_ID_CURVE_GREEN,
    ZI_ID_CURVE_BLUE,
    ZI_ID_CURVE_LINEAR,
    ZI_ID_CURVE_MATTE,
    ZI_ID_CURVE_MOODY,
    ZI_ID_CURVE_PUNCH,
    ZI_ID_CURVE_LOFI,
    ZI_ID_FIND,
    ZI_ID_CLEAR_FILTER,
    ZI_ID_PAGE_FIRST,
    ZI_ID_PAGE_PREV,
    ZI_ID_PAGE_NEXT,
    ZI_ID_PAGE_LAST,
    ZI_ID_MOVE_GO,
    ZI_ID_MOVE_SUB,
    ZI_ID_MERGE_H,
    ZI_ID_MERGE_V,
    ZI_ID_MERGE_OK,
    ZI_ID_MERGE_CANCEL,
    ZI_ID_ORGANIZE,
    ZI_ID_CLEAN_DATE,
    ZI_ID_RESCAN,
    ZI_ID_BUILD_PREV,
    ZI_ID_DELETE_ALL,
    ZI_ID_FOLD_DATE,
    ZI_ID_FOLD_MOVE,
    ZI_ID_FOLD_MERGE,
    ZI_ID_FOLD_BUILD,
    ZI_ID_MOVE_NAME,
    ZI_ID_MERGE_FIT,
    ZI_ID_MERGE_FULL
};

enum {
    ZI_CMD_OPEN_FILE = 1001,
    ZI_CMD_OPEN_FOLDER,
    ZI_CMD_SAVE,
    ZI_CMD_SAVE_AS,
    ZI_CMD_EXIT,
    ZI_CMD_FOLDER_VIEW,
    ZI_CMD_IMAGE_VIEW,
    ZI_CMD_DISK,
    ZI_CMD_SERVER,
    ZI_CMD_SUBFOLDER,
    ZI_CMD_CHOOSE_LIBRARY,
    ZI_CMD_PAIR,
    ZI_CMD_CLEAN,
    ZI_CMD_UNDO,
    ZI_CMD_REDO,
    ZI_CMD_COPY,
    ZI_CMD_PASTE,
    ZI_CMD_ROT_LEFT,
    ZI_CMD_ROT_RIGHT,
    ZI_CMD_FLIP_H,
    ZI_CMD_FLIP_V,
    ZI_CMD_CTX_EDIT = 2101,
    ZI_CMD_CTX_REVEAL,
    ZI_CMD_CTX_OPEN,
    ZI_CMD_CTX_COPY,
    ZI_CMD_CTX_PASTE,
    ZI_CMD_CTX_SIZE,
    ZI_CMD_CTX_REMOVE,
    ZI_CMD_CTX_RENAME,
    ZI_CMD_CTX_PLAY,
    ZI_CMD_CTX_APP0 = 2200
};

#define ZI_CTX_APPS 24

#define ZI_HIST 48
#define ZI_SPECTRUM_BINS 256

typedef struct ZiCtrl {
    int id;
    int extra;
    int enabled;
    int selected;
    RECT rc;
    const wchar_t *label;
} ZiCtrl;

/* One directory in the sidebar. Children stay in this list after the folder is collapsed. */
typedef struct ZiFolder {
    wchar_t name[ZI_NAME];
    int depth;
    int parent;
    int expanded;
    int loaded;
} ZiFolder;

/* One still or video in the folder grid. The thumbnail arrives after the name. */
typedef struct ZiFile {
    wchar_t name[ZI_NAME];
    wchar_t path[MAX_PATH];
    int video;
    int failed;
    int thumb_w;
    int thumb_h;
    HBITMAP thumb;
    /* Helper identity. Empty until a library list or a path lookup fills it. */
    char uid[64];
    int pinned;
    int keeper;
    int parked;
    wchar_t capture[40];
} ZiFile;

/* Slider positions for the open photograph. Identity leaves the file unchanged. */
typedef struct ZiTone {
    double exposure;
    double brightness;
    double contrast;
    double highlights;
    double shadows;
    double whites;
    double blacks;
    double texture;
    double clarity;
    double dehaze;
    double vibrance;
    double saturation;
    double temperature;
    double tint;
    double spectrum_low;
    double spectrum_mid;
    double spectrum_high;
    double straighten;
    double vertical;
    double horizontal;
    double sharpness;
    /* Lens coefficients. lens is a profile index. fov is degrees, or 0. */
    double k1;
    double k2;
    double fov;
    int lens;
    /* Corner pin, top-left y down. While this is on, keystone is not applied. */
    int has_corners;
    double corner_x[4];
    double corner_y[4];
    int turns;
    /* Crop of the geometry output. Origin is top-left and y grows downward. */
    int has_crop;
    double crop_x;
    double crop_y;
    double crop_w;
    double crop_h;
    /* Shifts a* and b* after dehaze. Inactive when has_skin is 0. Linear sRGB Lab. */
    int has_skin;
    int skin_preset;
    double skin_delta_a;
    double skin_delta_b;
    /* 1.8.x grade. Missing .zimg keys stay at identity. */
    double details;
    double offset_x;
    double offset_y;
    int flip_h;
    int flip_v;
    double vignette_amount;
    double vignette_mid;
    double vignette_round;
    double vignette_feather;
    double vignette_x;
    double vignette_y;
    ZiCurves curves;
} ZiTone;

typedef struct ZiHistStep {
    ZiTone tone;
    wchar_t label[40];
} ZiHistStep;

typedef struct ZiRegions {
    RECT toolbar;
    RECT sidebar;
    RECT split;
    RECT center;
    RECT viewbar;
    RECT editorbar;
    RECT canvas;
    RECT right;
} ZiRegions;

typedef struct ZiApp {
    HWND hwnd;
    UINT dpi;
    HFONT font;
    HFONT font_header;
    HFONT font_hint;
    HBRUSH panel;
    HBRUSH canvas_brush;
    HBRUSH list_brush;
    HBRUSH list_on;
    HBRUSH button_brush;
    HBRUSH select_brush;
    HBRUSH white;
    HPEN hair;
    HPEN muted_pen;
    HPEN tool_pen;
    HPEN ink_pen;

    int source_server;
    int disk_folder;
    int server_folder;
    int subfolders;
    int tree_width;
    int library_width;
    int library_section;
    int section_open[ZI_SECTIONS];
    int folder_scroll;
    int inspector_scroll;
    int inspector_content;
    int folder_count;
    int folder_selected;
    int drag_split;
    int drag_anchor_x;
    int drag_anchor_width;

    wchar_t root[MAX_PATH];
    wchar_t opened[MAX_PATH];
    wchar_t library_root[MAX_PATH];
    wchar_t library_scan[MAX_PATH];
    wchar_t notice[192];
    wchar_t helper_host[64];
    wchar_t helper_line[192];
    /* Mac DuplicateCopy.passLine. Empty hides the toolbar status. */
    wchar_t pass_line[192];
    int helper_online;
    int helper_safe;
    int tree_root_open;
    int folders_are_library;
    int file_count;
    int file_selected;
    int file_scroll;
    LONG file_generation;
    wchar_t file_dir[MAX_PATH];
    ZiFolder folders[ZI_MAX_FOLDERS];
    ZiFile files[ZI_MAX_FILES];

    ZiRegions regions;
    ZiCtrl ctrls[ZI_CTRL_CAP];
    int ctrl_count;

    /* Folder grid. preview_dip is the cell long edge, from 80 to 480. */
    int preview_dip;
    int file_nested;
    int file_reading;
    int file_mark[ZI_MAX_FILES];
    int file_anchor;
    RECT preview_track;

    /* Open photograph. photo_base is EXIF-upright. photo includes the quarter-turns. */
    HBITMAP photo_base;
    HBITMAP photo;
    int photo_w;
    int photo_h;
    int photo_turns;
    int photo_failed;
    int photo_busy;
    int pending_fit;
    LONG photo_generation;
    wchar_t photo_path[MAX_PATH];

    double zoom_mag;
    double zoom_min;
    double zoom_off_x;
    double zoom_off_y;
    int zoom_tool;
    int select_tool;
    int eyedropper_tool;
    int horizon_tool;
    int horizon_have;
    double horizon_vx;
    double horizon_vy;
    double horizon_sx;
    double horizon_sy;
    /* 0 is White. 1 is Skin. skin_preset is the menu and swatch selection. */
    int eyedropper_skin;
    int skin_preset;
    int drop_pending;
    int drop_x;
    int drop_y;
    int has_marquee;
    double marquee_x;
    double marquee_y;
    double marquee_w;
    double marquee_h;
    int drag_crop;
    int crop_kind;
    int crop_handle;
    double crop_start_x;
    double crop_start_y;
    double crop_start_w;
    double crop_start_h;
    POINT crop_down;
    int space_down;
    int drag_pan;
    int drag_zoom;
    int drag_preview;
    int drag_slider;
    int slider_index;
    int drag_spectrum;
    double spectrum_origin;
    int spectrum_down_x;
    /* Display luminance before the Spectrum handles. The graph stays put while a handle moves. */
    int spectrum_bins[ZI_SPECTRUM_BINS];
    int spectrum_ready;
    ZiTone spectrum_source;
    POINT drag_pt;
    double drag_off_x;
    double drag_off_y;
    wchar_t zoom_label[16];

    ZiTone edit;
    LONG grade_serial;
    LONG auto_serial;
    ZiHistStep hist[ZI_HIST];
    int hist_len;
    int hist_pos;
    ZiTone copied;
    int has_copied;

    wchar_t fact_lines[8][260];
    int fact_count;
    int fact_ready;
    LONG fact_generation;
    wchar_t fact_path[MAX_PATH];

    /* Touchup section height in dip, measured from the hint. 0 until the first layout. */
    int touch_body_dip;
    int touch_hint_px;
    /* Spot and Scratch brush. 0 means 48. The circle is this many client pixels across. */
    int touch_size;
    int spot_tool;
    int scratch_tool;
    int redeye_tool;
    int touch_busy;
    int scratch_drag;
    int scratch_count;
    int scratch_x[4096];
    int scratch_y[4096];
    int amount_drag;
    int hover_x;
    int hover_y;
    int hover_on;

    /* Library panels. Events and Reorganize replace the folder grid. */
    int events_open;
    int reorg_open;
    int date_busy;
    int removal_busy;
    int clean_busy;
    int dup_busy;
    int event_selected;
    wchar_t grid_note[320];

    /* 1.8.x folder, curve, video, and flatten state. Zero means off or the first page. */
    int curve_channel;
    wchar_t name_filter[ZI_NAME];
    int file_page;
    int file_still_total;
    int file_match_total;
    int file_video_total;
    int file_filtered;
    int fold_date;
    int fold_move;
    int fold_merge;
    int fold_build;
    int flatten_busy;
    int export_busy;
    int video_open;
    int pdf_picking;
    int merge_open;
    int merge_vertical;
    int merge_shift_x;
    int merge_shift_y;
    int merge_a;
    int merge_b;
    wchar_t move_name[ZI_NAME];
    int move_sub;
} ZiApp;

static inline int zi_px(const ZiApp *app, int dip) {
    return MulDiv(dip, (int)app->dpi, 96);
}

static inline int zi_showing_folder(const ZiApp *app) {
    return app->source_server ? app->server_folder : app->disk_folder;
}

int zi_run(HINSTANCE instance);
void zi_layout_now(ZiApp *app);
/* index -1 is the tree root. A folder that is still unread keeps its chevron. */
int zi_folder_has_chevron(const ZiApp *app, int index);
/* The folder whose stills belong in the grid. The Photos root uses index -1. */
int zi_current_folder(const ZiApp *app, wchar_t *dest, size_t cap);

#endif
