/* Grade math shared with the headless test. No window types. */
#ifndef ZIMAGEWIN_FEATURES_H
#define ZIMAGEWIN_FEATURES_H

#include <stddef.h>
#include <wchar.h>

#define ZI_CURVE_MAX 8
#define ZI_PAGE_SIZE 200
#define ZI_VIDEO_PAGE 300
#define ZI_CATALOG_CAP 20000

typedef struct ZiCurvePoint {
    double x;
    double y;
} ZiCurvePoint;

typedef struct ZiCurve {
    int count;
    ZiCurvePoint points[ZI_CURVE_MAX];
} ZiCurve;

typedef struct ZiCurves {
    ZiCurve master;
    ZiCurve red;
    ZiCurve green;
    ZiCurve blue;
} ZiCurves;

void zi_curve_linear(ZiCurve *curve);
void zi_curves_identity(ZiCurves *curves);
int zi_curve_is_linear(const ZiCurve *curve);
int zi_curves_linear(const ZiCurves *curves);
int zi_curves_same(const ZiCurves *a, const ZiCurves *b);
void zi_curve_sanitize(ZiCurve *curve);
double zi_curve_eval(const ZiCurve *curve, double x);
/* 0 linear, 1 matte, 2 film moody, 3 punch, 4 lo-fi. Sets the master curve and clears R, G, and B. */
void zi_curve_preset(ZiCurves *curves, int preset);
int zi_curve_matches_preset(const ZiCurves *curves, int preset);
void zi_curve_move(ZiCurve *curve, int index, double x, double y);
int zi_curve_insert(ZiCurve *curve, double x);
void zi_curve_remove(ZiCurve *curve, int index);
void zi_curve_reset_end(ZiCurve *curve, int index);
/* Display-tone lookup after spectrum: master, then the matching channel. Linear curves are skipped. */
void zi_curve_map_linear(const ZiCurves *curves, double *r, double *g, double *b);

double zi_vignette_coverage(double x, double y, double frame_w, double frame_h,
                            double midpoint, double roundness, double feather,
                            double center_x, double center_y);
double zi_vignette_shade(double sample, double amount, double coverage);
/* Top-down pixel. Positive center Y moves the clear center up the screen. */
double zi_vignette_coverage_px(int x, int y, int width, int height,
                               double midpoint, double roundness, double feather,
                               double center_x, double center_y);

void zi_horizon_measure(double x0, double y0, double x1, double y1,
                        double pixel_width, double pixel_height,
                        int *quarter_turns, double *degrees);

int zi_wrap_turns(int turns);
void zi_flip_swap_if_odd(int *flip_h, int *flip_v, int delta);

typedef struct ZiNormRect {
    double x;
    double y;
    double w;
    double h;
} ZiNormRect;

void zi_crop_rotate(ZiNormRect *rect, int quarter_turns);
void zi_crop_mirror_h(ZiNormRect *rect);
void zi_crop_mirror_v(ZiNormRect *rect);

typedef struct ZiAutoLevels {
    double black;
    double shadow;
    double mid;
    double highlight;
    double white;
} ZiAutoLevels;

typedef struct ZiAutoGrade {
    double exposure;
    double contrast;
    double highlights;
    double shadows;
    double whites;
    double blacks;
} ZiAutoGrade;

double zi_contrast_input(double slider);
void zi_auto_from_levels(const ZiAutoLevels *levels, ZiAutoGrade *grade);

int zi_name_match(const wchar_t *name, const wchar_t *filter);

typedef struct ZiPageSlice {
    int still_begin;
    int still_count;
    int include_videos;
    int filtered_begin;
    int filtered_count;
} ZiPageSlice;

void zi_page_slice(int page, int still_total, int filtered, int match_total, ZiPageSlice *out);

/* attempt 0 is left-right.tiff. attempt 1 is left-right-2.tiff, through -999. Returns 0 past that. */
int zi_merge_filename(wchar_t *dest, size_t cap, const wchar_t *left, const wchar_t *right, int attempt);

int zi_is_pupil_red(double red, double green, double blue);
int zi_is_pupil_fringe(double red, double green, double blue);
int zi_redeye_reject(int count, int box_w, int box_h);
int zi_scratch_spacing(int radius);
int zi_chosen_page(const int *pages, int count, int index);

#endif
