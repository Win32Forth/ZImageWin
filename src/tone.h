/* Adjustment math modeled on ZImager TonePipeline and GeometryPipeline.
   The working space here is linear sRGB. The Mac uses linear Display P3.
   The original photograph is not modified. */
#ifndef ZIMAGEWIN_TONE_H
#define ZIMAGEWIN_TONE_H

#include "app.h"

typedef struct ZiTouchDoc ZiTouchDoc;

enum {
    ZI_SL_SPEC_LOW = 0,
    ZI_SL_SPEC_MID,
    ZI_SL_SPEC_HIGH,
    ZI_SL_EXPOSURE,
    ZI_SL_BRIGHTNESS,
    ZI_SL_CONTRAST,
    ZI_SL_HIGHLIGHTS,
    ZI_SL_SHADOWS,
    ZI_SL_WHITES,
    ZI_SL_BLACKS,
    ZI_SL_TEXTURE,
    ZI_SL_CLARITY,
    ZI_SL_DEHAZE,
    ZI_SL_VIBRANCE,
    ZI_SL_SATURATION,
    ZI_SL_TEMPERATURE,
    ZI_SL_TINT,
    ZI_SL_STRAIGHTEN,
    ZI_SL_VERTICAL,
    ZI_SL_HORIZONTAL,
    ZI_SL_SHARPNESS,
    ZI_SL_K1,
    ZI_SL_K2,
    ZI_SL_CTL_X,
    ZI_SL_CTL_Y,
    ZI_SL_CTR_X,
    ZI_SL_CTR_Y,
    ZI_SL_CBR_X,
    ZI_SL_CBR_Y,
    ZI_SL_CBL_X,
    ZI_SL_CBL_Y,
    ZI_SLIDER_COUNT
};

typedef struct ZiSliderSpec {
    const wchar_t *label;
    int section;
    double min_value;
    double max_value;
    int reset;
    double reset_value;
    int readout;
} ZiSliderSpec;

void zi_tone_identity(ZiTone *tone);
int zi_tone_same(const ZiTone *a, const ZiTone *b);
int zi_tone_needs_pipeline(const ZiTone *tone);
int zi_spectrum_identity(const ZiTone *tone);
void zi_norm_clamp(double *x, double *y, double *w, double *h);
int zi_norm_usable(double width, double height, int pixel_w, int pixel_h);
/* Maps a marquee on the current picture into the stored crop. A full frame clears it. */
void zi_crop_assign(ZiTone *tone, double x, double y, double w, double h);
const ZiSliderSpec *zi_slider_spec(int index);
double zi_slider_get(const ZiTone *tone, int index);
void zi_slider_set(ZiTone *tone, int index, double value);
void zi_slider_format(const ZiTone *tone, int index, wchar_t *dest, size_t cap);
void zi_slider_geometry(const ZiApp *app, const RECT *row, RECT *label, RECT *track, RECT *value);
int zi_section_body_dip(const ZiApp *app, int section);

/* Lens, perspective, quarter-turns, straighten, sharpness, crop, then the tone stack.
   bins, when set, receives the display-luminance counts before the Spectrum handles. */
#define ZI_GEO_TOP 4
#define ZI_GEO_ROW 28
#define ZI_GEO_GAP 8
#define ZI_GEO_TITLE 16
#define ZI_GEO_MENU 26
#define ZI_GEO_HINT 20
#define ZI_GEO_BOTTOM 8
#define ZI_LENS_COUNT 5

int zi_geometry_body_dip(const ZiApp *app);
int zi_lens_count(void);
const wchar_t *zi_lens_label(int index);
const char *zi_lens_profile_id(int index);
int zi_lens_profile_index(const char *profile_id);
void zi_lens_copy(ZiTone *tone, int index);
/* Quarter-turn delta, clockwise positive, and the straighten delta in degrees. */
void zi_horizon_delta(double x0, double y0, double x1, double y1, int *quarter_turns, double *degrees);
/* Touch repair runs after the crop and before the tone stack. NULL skips it. */
HBITMAP zi_tone_render(HBITMAP base, const ZiTone *tone, const ZiTouchDoc *touch, int *out_w, int *out_h, int *bins);
/* Stops after that repair. The caller frees the RGB buffer. */
float *zi_tone_stage_rgb(HBITMAP base, const ZiTone *tone, const ZiTouchDoc *touch, int *out_w, int *out_h);
/* Counts a bitmap that is already the displayed picture, used when no grade is required. */
void zi_spectrum_count_dib(HBITMAP dib, int bins[ZI_SPECTRUM_BINS]);
/* Writes Exposure, Contrast, Highlights, Shadows, Whites, and Blacks from the picture
   before the tone stack. Other fields stay. Returns 0 when the picture has too few pixels. */
int zi_tone_auto(HBITMAP base, ZiTone *tone, const ZiTouchDoc *touch);

#define ZI_SKIN_COUNT 7
/* Color body below the section header. The slider block is the 4 DIP pad plus two 28 DIP rows. */
#define ZI_COLOR_SLIDERS_DIP 60
#define ZI_COLOR_MODE_GAP 8
#define ZI_COLOR_MODE_H 28
#define ZI_COLOR_MENU_GAP 8
#define ZI_COLOR_MENU_H 26
#define ZI_COLOR_SWATCH_GAP 6
#define ZI_COLOR_SWATCH 18
#define ZI_COLOR_SWATCH_H 22
#define ZI_COLOR_HINT_GAP 8
#define ZI_COLOR_HINT_H 20
#define ZI_COLOR_BOTTOM 8

int zi_color_body_dip(const ZiApp *app);
int zi_skin_count(void);
const wchar_t *zi_skin_label(int index);
const char *zi_skin_preset_id(int index);
int zi_skin_preset_index(const char *preset_id);
void zi_skin_lab(int index, double *L, double *a, double *b);
COLORREF zi_skin_color(int index);

typedef struct ZiWindowSample {
    int count;
    double mean_r;
    double mean_g;
    double mean_b;
    double median_r;
    double median_g;
    double median_b;
    double chroma_rms;
} ZiWindowSample;

/* Top-down BGRA. side is the window. An empty clip reads the top-left pixel. */
int zi_tone_sample_window(HBITMAP dib, int x, int y, int side, ZiWindowSample *sample);
/* Temperature and tint that bring the linear sample closest to neutral through the Kelvin gains. */
void zi_tone_neutralize(double r, double g, double b, double *temperature, double *tint);
void zi_skin_deltas(double r, double g, double b, int preset, double *delta_a, double *delta_b);

#endif
