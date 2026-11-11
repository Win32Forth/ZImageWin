/* CPU grade for the open photograph. Slider ranges and order follow the Mac app. */
#include "tone.h"
#include "touchup.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZI_PI 3.14159265358979323846

static const ZiSliderSpec k_sliders[ZI_SLIDER_COUNT] = {
    { L"Shadows", 0, 0, 1, 1, 0, 0 },
    { L"Midtones", 0, 0, 1, 1, 0.5, 0 },
    { L"Highlights", 0, 0, 1, 1, 1, 0 },
    { L"Exposure", 1, -5, 5, 1, 0, 1 },
    { L"Brightness", 1, -1, 1, 1, 0, 0 },
    { L"Contrast", 1, -1, 1, 1, 0, 0 },
    { L"Highlights", 1, -1, 1, 1, 0, 0 },
    { L"Shadows", 1, -1, 1, 1, 0, 0 },
    { L"Whites", 1, -1, 1, 1, 0, 0 },
    { L"Blacks", 1, -1, 1, 1, 0, 0 },
    { L"Texture", 1, -1, 1, 1, 0, 0 },
    { L"Clarity", 1, -1, 1, 1, 0, 0 },
    { L"Dehaze", 1, 0, 1, 0, 0, 0 },
    { L"Vibrance", 1, -1, 1, 1, 0, 0 },
    { L"Saturation", 1, -1, 1, 1, 0, 0 },
    { L"Temperature", 2, 2000, 50000, 0, 6500, 2 },
    { L"Tint", 2, -150, 150, 1, 0, 3 },
    { L"Straighten", 3, -45, 45, 1, 0, 4 },
    { L"Vertical", 3, -1, 1, 1, 0, 0 },
    { L"Horizontal", 3, -1, 1, 1, 0, 0 },
    { L"Sharpness", 3, -1, 1, 1, 0, 0 },
    { L"k1", -1, -0.50, 0.20, 0, 0, 0 },
    { L"k2", -1, -0.20, 0.20, 0, 0, 0 },
    { L"Top left X", -1, -0.5, 1.5, 1, 0.5, 5 },
    { L"Top left Y", -1, -0.5, 1.5, 1, 0.5, 5 },
    { L"Top right X", -1, -0.5, 1.5, 1, 0.5, 5 },
    { L"Top right Y", -1, -0.5, 1.5, 1, 0.5, 5 },
    { L"Bottom right X", -1, -0.5, 1.5, 1, 0.5, 5 },
    { L"Bottom right Y", -1, -0.5, 1.5, 1, 0.5, 5 },
    { L"Bottom left X", -1, -0.5, 1.5, 1, 0.5, 5 },
    { L"Bottom left Y", -1, -0.5, 1.5, 1, 0.5, 5 }
};

void zi_tone_identity(ZiTone *tone) {
    memset(tone, 0, sizeof *tone);
    tone->temperature = 6500;
    tone->spectrum_mid = 0.5;
    tone->spectrum_high = 1;
}

static int zi_near(double a, double b) {
    double d = a - b;

    if (d < 0) {
        d = -d;
    }
    return d <= 0.0005;
}

static int zi_corners_same(const ZiTone *a, const ZiTone *b) {
    int i;

    if (a->has_corners != b->has_corners) {
        return 0;
    }
    if (!a->has_corners) {
        return 1;
    }
    for (i = 0; i < 4; i++) {
        if (!zi_near(a->corner_x[i], b->corner_x[i]) || !zi_near(a->corner_y[i], b->corner_y[i])) {
            return 0;
        }
    }
    return 1;
}

int zi_tone_same(const ZiTone *a, const ZiTone *b) {
    return zi_near(a->exposure, b->exposure) &&
           zi_near(a->brightness, b->brightness) &&
           zi_near(a->contrast, b->contrast) &&
           zi_near(a->highlights, b->highlights) &&
           zi_near(a->shadows, b->shadows) &&
           zi_near(a->whites, b->whites) &&
           zi_near(a->blacks, b->blacks) &&
           zi_near(a->texture, b->texture) &&
           zi_near(a->clarity, b->clarity) &&
           zi_near(a->dehaze, b->dehaze) &&
           zi_near(a->vibrance, b->vibrance) &&
           zi_near(a->saturation, b->saturation) &&
           zi_near(a->temperature, b->temperature) &&
           zi_near(a->tint, b->tint) &&
           zi_near(a->spectrum_low, b->spectrum_low) &&
           zi_near(a->spectrum_mid, b->spectrum_mid) &&
           zi_near(a->spectrum_high, b->spectrum_high) &&
           zi_near(a->straighten, b->straighten) &&
           zi_near(a->vertical, b->vertical) &&
           zi_near(a->horizontal, b->horizontal) &&
           zi_near(a->sharpness, b->sharpness) &&
           zi_near(a->k1, b->k1) &&
           zi_near(a->k2, b->k2) &&
           zi_near(a->fov, b->fov) &&
           a->lens == b->lens &&
           zi_corners_same(a, b) &&
           (a->turns & 3) == (b->turns & 3) &&
           a->has_crop == b->has_crop &&
           (!a->has_crop || (zi_near(a->crop_x, b->crop_x) &&
                             zi_near(a->crop_y, b->crop_y) &&
                             zi_near(a->crop_w, b->crop_w) &&
                             zi_near(a->crop_h, b->crop_h))) &&
           a->has_skin == b->has_skin &&
           (!a->has_skin || (a->skin_preset == b->skin_preset &&
                             zi_near(a->skin_delta_a, b->skin_delta_a) &&
                             zi_near(a->skin_delta_b, b->skin_delta_b)));
}

static int zi_crop_active(const ZiTone *tone) {
    if (!tone->has_crop || tone->crop_w <= 0.0005 || tone->crop_h <= 0.0005) {
        return 0;
    }
    if (tone->crop_x <= 0.0005 && tone->crop_y <= 0.0005 &&
        tone->crop_w >= 0.9995 && tone->crop_h >= 0.9995) {
        return 0;
    }
    return 1;
}

void zi_norm_clamp(double *x, double *y, double *w, double *h) {
    double max_x;
    double max_y;

    if (*w < 0) {
        *x += *w;
        *w = -*w;
    }
    if (*h < 0) {
        *y += *h;
        *h = -*h;
    }
    max_x = *x + *w;
    if (max_x < 0) {
        max_x = 0;
    }
    if (max_x > 1) {
        max_x = 1;
    }
    max_y = *y + *h;
    if (max_y < 0) {
        max_y = 0;
    }
    if (max_y > 1) {
        max_y = 1;
    }
    if (*x < 0) {
        *x = 0;
    }
    if (*x > 1) {
        *x = 1;
    }
    if (*y < 0) {
        *y = 0;
    }
    if (*y > 1) {
        *y = 1;
    }
    *w = max_x - *x;
    if (*w < 0) {
        *w = 0;
    }
    *h = max_y - *y;
    if (*h < 0) {
        *h = 0;
    }
}

int zi_norm_usable(double width, double height, int pixel_w, int pixel_h) {
    if (pixel_w < 1 || pixel_h < 1) {
        return 0;
    }
    return width * (double)pixel_w >= 2.0 && height * (double)pixel_h >= 2.0;
}

void zi_crop_assign(ZiTone *tone, double x, double y, double w, double h) {
    zi_norm_clamp(&x, &y, &w, &h);
    if (tone->has_crop) {
        double bx = tone->crop_x;
        double by = tone->crop_y;
        double bw = tone->crop_w;
        double bh = tone->crop_h;

        zi_norm_clamp(&bx, &by, &bw, &bh);
        x = bx + x * bw;
        y = by + y * bh;
        w *= bw;
        h *= bh;
        zi_norm_clamp(&x, &y, &w, &h);
    }
    if (w <= 0.0005 || h <= 0.0005 ||
        (x <= 0.0005 && y <= 0.0005 && w >= 0.9995 && h >= 0.9995)) {
        tone->has_crop = 0;
        tone->crop_x = 0;
        tone->crop_y = 0;
        tone->crop_w = 0;
        tone->crop_h = 0;
        return;
    }
    tone->has_crop = 1;
    tone->crop_x = x;
    tone->crop_y = y;
    tone->crop_w = w;
    tone->crop_h = h;
}

static int zi_active(double value) {
    return value > 0.0000001 || value < -0.0000001;
}

static int zi_lens_active(const ZiTone *tone) {
    return zi_active(tone->k1) || zi_active(tone->k2) || tone->fov > 0.01;
}

static int zi_corner_at(const ZiTone *tone, int index, double x, double y) {
    return zi_near(tone->corner_x[index], x) && zi_near(tone->corner_y[index], y);
}

static int zi_corners_unit(const ZiTone *tone) {
    return zi_corner_at(tone, 0, 0, 0) &&
           zi_corner_at(tone, 1, 1, 0) &&
           zi_corner_at(tone, 2, 1, 1) &&
           zi_corner_at(tone, 3, 0, 1);
}

static int zi_corners_warp(const ZiTone *tone) {
    return tone->has_corners && !zi_corners_unit(tone);
}

int zi_spectrum_identity(const ZiTone *tone) {
    return tone->spectrum_low <= 0.0005 && tone->spectrum_high >= 0.9995 &&
           zi_near(tone->spectrum_mid, 0.5);
}

int zi_tone_needs_pipeline(const ZiTone *tone) {
    if (zi_active(tone->exposure) || zi_active(tone->brightness) || zi_active(tone->contrast) ||
        zi_active(tone->highlights) || zi_active(tone->shadows) || zi_active(tone->whites) ||
        zi_active(tone->blacks) || zi_active(tone->texture) || zi_active(tone->clarity) ||
        tone->dehaze > 0.0000001 || zi_active(tone->vibrance) || zi_active(tone->saturation) ||
        !zi_near(tone->temperature, 6500) || zi_active(tone->tint) ||
        tone->spectrum_low > 0.0005 || tone->spectrum_high < 0.9995 ||
        !zi_near(tone->spectrum_mid, 0.5) || zi_active(tone->straighten) ||
        zi_active(tone->vertical) || zi_active(tone->horizontal) || zi_active(tone->sharpness) ||
        zi_lens_active(tone) || zi_corners_warp(tone) ||
        zi_crop_active(tone) ||
        (tone->has_skin && (zi_active(tone->skin_delta_a) || zi_active(tone->skin_delta_b)))) {
        return 1;
    }
    return 0;
}

const ZiSliderSpec *zi_slider_spec(int index) {
    if (index < 0 || index >= ZI_SLIDER_COUNT) {
        return &k_sliders[0];
    }
    return &k_sliders[index];
}

static double zi_clamp(double value, double low, double high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

double zi_slider_get(const ZiTone *tone, int index) {
    switch (index) {
    case ZI_SL_SPEC_LOW: return tone->spectrum_low;
    case ZI_SL_SPEC_MID: return tone->spectrum_mid;
    case ZI_SL_SPEC_HIGH: return tone->spectrum_high;
    case ZI_SL_EXPOSURE: return tone->exposure;
    case ZI_SL_BRIGHTNESS: return tone->brightness;
    case ZI_SL_CONTRAST: return tone->contrast;
    case ZI_SL_HIGHLIGHTS: return tone->highlights;
    case ZI_SL_SHADOWS: return tone->shadows;
    case ZI_SL_WHITES: return tone->whites;
    case ZI_SL_BLACKS: return tone->blacks;
    case ZI_SL_TEXTURE: return tone->texture;
    case ZI_SL_CLARITY: return tone->clarity;
    case ZI_SL_DEHAZE: return tone->dehaze;
    case ZI_SL_VIBRANCE: return tone->vibrance;
    case ZI_SL_SATURATION: return tone->saturation;
    case ZI_SL_TEMPERATURE: return tone->temperature;
    case ZI_SL_TINT: return tone->tint;
    case ZI_SL_STRAIGHTEN: return tone->straighten;
    case ZI_SL_VERTICAL: return tone->vertical;
    case ZI_SL_HORIZONTAL: return tone->horizontal;
    case ZI_SL_SHARPNESS: return tone->sharpness;
    case ZI_SL_K1: return tone->k1;
    case ZI_SL_K2: return tone->k2;
    case ZI_SL_CTL_X: return tone->corner_x[0];
    case ZI_SL_CTL_Y: return tone->corner_y[0];
    case ZI_SL_CTR_X: return tone->corner_x[1];
    case ZI_SL_CTR_Y: return tone->corner_y[1];
    case ZI_SL_CBR_X: return tone->corner_x[2];
    case ZI_SL_CBR_Y: return tone->corner_y[2];
    case ZI_SL_CBL_X: return tone->corner_x[3];
    case ZI_SL_CBL_Y: return tone->corner_y[3];
    default: return 0;
    }
}

void zi_slider_set(ZiTone *tone, int index, double value) {
    const ZiSliderSpec *spec = zi_slider_spec(index);
    const double gap = 0.02;

    value = zi_clamp(value, spec->min_value, spec->max_value);
    switch (index) {
    case ZI_SL_SPEC_LOW:
        tone->spectrum_low = zi_clamp(value, 0, tone->spectrum_mid - gap);
        break;
    case ZI_SL_SPEC_MID:
        tone->spectrum_mid = zi_clamp(value, tone->spectrum_low + gap, tone->spectrum_high - gap);
        break;
    case ZI_SL_SPEC_HIGH:
        tone->spectrum_high = zi_clamp(value, tone->spectrum_mid + gap, 1);
        break;
    case ZI_SL_EXPOSURE: tone->exposure = value; break;
    case ZI_SL_BRIGHTNESS: tone->brightness = value; break;
    case ZI_SL_CONTRAST: tone->contrast = value; break;
    case ZI_SL_HIGHLIGHTS: tone->highlights = value; break;
    case ZI_SL_SHADOWS: tone->shadows = value; break;
    case ZI_SL_WHITES: tone->whites = value; break;
    case ZI_SL_BLACKS: tone->blacks = value; break;
    case ZI_SL_TEXTURE: tone->texture = value; break;
    case ZI_SL_CLARITY: tone->clarity = value; break;
    case ZI_SL_DEHAZE: tone->dehaze = value; break;
    case ZI_SL_VIBRANCE: tone->vibrance = value; break;
    case ZI_SL_SATURATION: tone->saturation = value; break;
    case ZI_SL_TEMPERATURE: tone->temperature = value; break;
    case ZI_SL_TINT: tone->tint = value; break;
    case ZI_SL_STRAIGHTEN: tone->straighten = value; break;
    case ZI_SL_VERTICAL: tone->vertical = value; break;
    case ZI_SL_HORIZONTAL: tone->horizontal = value; break;
    case ZI_SL_SHARPNESS: tone->sharpness = value; break;
    case ZI_SL_K1: tone->k1 = value; break;
    case ZI_SL_K2: tone->k2 = value; break;
    case ZI_SL_CTL_X: tone->corner_x[0] = value; break;
    case ZI_SL_CTL_Y: tone->corner_y[0] = value; break;
    case ZI_SL_CTR_X: tone->corner_x[1] = value; break;
    case ZI_SL_CTR_Y: tone->corner_y[1] = value; break;
    case ZI_SL_CBR_X: tone->corner_x[2] = value; break;
    case ZI_SL_CBR_Y: tone->corner_y[2] = value; break;
    case ZI_SL_CBL_X: tone->corner_x[3] = value; break;
    case ZI_SL_CBL_Y: tone->corner_y[3] = value; break;
    default: break;
    }
}

void zi_slider_format(const ZiTone *tone, int index, wchar_t *dest, size_t cap) {
    double value = zi_slider_get(tone, index);
    const ZiSliderSpec *spec = zi_slider_spec(index);

    if (cap == 0) {
        return;
    }
    dest[0] = L'\0';
    switch (spec->readout) {
    case 1: swprintf_s(dest, cap, L"%+.1f", value); break;
    case 2: swprintf_s(dest, cap, L"%.0f K", value); break;
    case 3: swprintf_s(dest, cap, L"%+.0f", value); break;
    case 4: swprintf_s(dest, cap, L"%+.1f\u00B0", value); break;
    case 5: swprintf_s(dest, cap, L"%.2f", value); break;
    default: swprintf_s(dest, cap, L"%+.2f", value); break;
    }
}

void zi_slider_geometry(const ZiApp *app, const RECT *row, RECT *label, RECT *track, RECT *value) {
    int label_w = zi_px(app, 108);
    int value_w = zi_px(app, 64);
    int gap = zi_px(app, 8);

    *label = *row;
    label->left += zi_px(app, 16);
    label->right = label->left + label_w;
    *value = *row;
    value->right -= zi_px(app, 12);
    value->left = value->right - value_w;
    *track = *row;
    track->left = label->right + gap;
    track->right = value->left - gap;
    if (track->right < track->left + 8) {
        track->right = track->left + 8;
    }
}

int zi_color_body_dip(const ZiApp *app) {
    int height = ZI_COLOR_SLIDERS_DIP + ZI_COLOR_MODE_GAP + ZI_COLOR_MODE_H;

    if (app && app->eyedropper_skin) {
        height += ZI_COLOR_MENU_GAP + ZI_COLOR_MENU_H;
        height += ZI_COLOR_SWATCH_GAP + ZI_COLOR_SWATCH_H;
    }
    height += ZI_COLOR_HINT_GAP + ZI_COLOR_HINT_H + ZI_COLOR_BOTTOM;
    return height;
}

typedef struct ZiSkinPreset {
    const wchar_t *label;
    double L;
    double a;
    double b;
    COLORREF color;
} ZiSkinPreset;

/* a* and b* are the Mac preset targets. The swatch is the sRGB hex from SkinPresets.swift. */
static const char *const k_skin_ids[ZI_SKIN_COUNT] = {
    "light", "lightGolden", "mediumGolden", "mediumOlive", "mediumWarm", "mediumDeep", "deep"
};

static const ZiSkinPreset k_skin[ZI_SKIN_COUNT] = {
    { L"Caucasian", 61.2, 11.0, 14.7, RGB(0xB0, 0x8C, 0x7A) },
    { L"Asian", 61.7, 10.0, 16.9, RGB(0xB1, 0x8E, 0x78) },
    { L"Southeast Asian", 56.4, 10.0, 19.0, RGB(0xA4, 0x80, 0x67) },
    { L"Middle Eastern", 58.8, 10.6, 16.9, RGB(0xAA, 0x86, 0x70) },
    { L"Hispanic", 58.0, 12.0, 17.5, RGB(0xAA, 0x83, 0x6D) },
    { L"South Asian", 52.7, 10.4, 17.9, RGB(0x9A, 0x77, 0x60) },
    { L"African", 39.6, 10.2, 14.4, RGB(0x75, 0x57, 0x46) }
};

int zi_skin_count(void) {
    return ZI_SKIN_COUNT;
}

const wchar_t *zi_skin_label(int index) {
    if (index < 0 || index >= ZI_SKIN_COUNT) {
        return L"Skin";
    }
    return k_skin[index].label;
}

const char *zi_skin_preset_id(int index) {
    if (index < 0 || index >= ZI_SKIN_COUNT) {
        return k_skin_ids[0];
    }
    return k_skin_ids[index];
}

int zi_skin_preset_index(const char *preset_id) {
    int i;

    if (!preset_id || !preset_id[0]) {
        return 0;
    }
    for (i = 0; i < ZI_SKIN_COUNT; i++) {
        if (strcmp(preset_id, k_skin_ids[i]) == 0) {
            return i;
        }
    }
    return 0;
}

void zi_skin_lab(int index, double *L, double *a, double *b) {
    if (index < 0 || index >= ZI_SKIN_COUNT) {
        index = 0;
    }
    if (L) {
        *L = k_skin[index].L;
    }
    if (a) {
        *a = k_skin[index].a;
    }
    if (b) {
        *b = k_skin[index].b;
    }
}

COLORREF zi_skin_color(int index) {
    if (index < 0 || index >= ZI_SKIN_COUNT) {
        return RGB(176, 140, 122);
    }
    return k_skin[index].color;
}

typedef struct ZiLensProfile {
    const wchar_t *name;
    double k1;
    double k2;
    double fov;
} ZiLensProfile;

static const ZiLensProfile k_lenses[ZI_LENS_COUNT] = {
    { L"None", 0, 0, 0 },
    { L"Super-wide adapter, mild", -0.15, 0.05, 0 },
    { L"Super-wide adapter, strong", -0.28, 0.12, 0 },
    { L"Panoramic adapter, 120\u00B0", 0, 0, 120 },
    { L"Panoramic adapter, 180\u00B0", 0, 0, 180 }
};

static const char *const k_lens_ids[ZI_LENS_COUNT] = {
    "none", "superwide-mild", "superwide-strong", "pano-120", "pano-180"
};

int zi_lens_count(void) {
    return ZI_LENS_COUNT;
}

const char *zi_lens_profile_id(int index) {
    if (index < 0 || index >= ZI_LENS_COUNT) {
        return k_lens_ids[0];
    }
    return k_lens_ids[index];
}

int zi_lens_profile_index(const char *profile_id) {
    int i;

    if (!profile_id || !profile_id[0]) {
        return 0;
    }
    for (i = 0; i < ZI_LENS_COUNT; i++) {
        if (strcmp(profile_id, k_lens_ids[i]) == 0) {
            return i;
        }
    }
    return 0;
}

const wchar_t *zi_lens_label(int index) {
    if (index < 0 || index >= ZI_LENS_COUNT) {
        return k_lenses[0].name;
    }
    return k_lenses[index].name;
}

void zi_lens_copy(ZiTone *tone, int index) {
    if (!tone || index < 0 || index >= ZI_LENS_COUNT) {
        return;
    }
    tone->lens = index;
    tone->k1 = k_lenses[index].k1;
    tone->k2 = k_lenses[index].k2;
    tone->fov = k_lenses[index].fov;
}

void zi_horizon_delta(double x0, double y0, double x1, double y1, int *quarter_turns, double *degrees) {
    double dx = x1 - x0;
    double dy = y1 - y0;
    double tilt;
    double needed;
    int turns = 0;

    if (quarter_turns) {
        *quarter_turns = 0;
    }
    if (degrees) {
        *degrees = 0;
    }
    if (dx == 0 && dy == 0) {
        return;
    }
    tilt = atan2(dy, dx) * 180.0 / ZI_PI;
    while (tilt > 90.0) {
        tilt -= 180.0;
    }
    while (tilt <= -90.0) {
        tilt += 180.0;
    }
    needed = tilt;
    if (needed > 45.0) {
        turns = 1;
        needed -= 90.0;
    } else if (needed < -45.0) {
        turns = -1;
        needed += 90.0;
    }
    if (needed > 45.0) {
        needed = 45.0;
    }
    if (needed < -45.0) {
        needed = -45.0;
    }
    if (quarter_turns) {
        *quarter_turns = turns;
    }
    if (degrees) {
        *degrees = needed;
    }
}

int zi_geometry_body_dip(const ZiApp *app) {
    int height = ZI_GEO_TOP + ZI_GEO_ROW + ZI_GEO_GAP + ZI_GEO_HINT;

    height += 4 * ZI_GEO_ROW;
    height += ZI_GEO_GAP + ZI_GEO_ROW;
    if (app && app->edit.has_corners) {
        height += 4 * (ZI_GEO_TITLE + 2 * ZI_GEO_ROW);
    }
    height += ZI_GEO_GAP + ZI_GEO_MENU;
    height += 2 * ZI_GEO_ROW + ZI_GEO_BOTTOM;
    return height;
}

int zi_section_body_dip(const ZiApp *app, int section) {
    int count = 0;
    int i;

    if (section == 0) {
        return 112;
    }
    if (section == 2) {
        return zi_color_body_dip(app);
    }
    if (section == 3) {
        return zi_geometry_body_dip(app);
    }
    if (section == 4) {
        if (app && app->touch_body_dip > 0) {
            return app->touch_body_dip;
        }
        return 184;
    }
    if (section == 5) {
        if (app->hist_len < 1) {
            return 36;
        }
        return 8 + app->hist_len * 26;
    }
    for (i = 0; i < ZI_SLIDER_COUNT; i++) {
        if (k_sliders[i].section == section) {
            count++;
        }
    }
    return 8 + count * 28;
}

static double zi_srgb_to_linear(double c) {
    if (c <= 0.04045) {
        return c / 12.92;
    }
    return pow((c + 0.055) / 1.055, 2.4);
}

static double zi_linear_to_srgb(double c) {
    if (c <= 0) {
        return 0;
    }
    if (c >= 1) {
        return 1;
    }
    if (c <= 0.0031308) {
        return 12.92 * c;
    }
    return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

static int zi_byte(double srgb) {
    if (srgb <= 0) {
        return 0;
    }
    if (srgb >= 1) {
        return 255;
    }
    return (int)(srgb * 255.0 + 0.5);
}

static double zi_smooth(double e0, double e1, double x) {
    double t;

    if (x <= e0) {
        return 0;
    }
    if (x >= e1) {
        return 1;
    }
    t = (x - e0) / (e1 - e0);
    return t * t * (3.0 - 2.0 * t);
}

static void zi_read_base(HBITMAP src, float **pixels, int *width, int *height) {
    BITMAP bm;
    int x;
    int y;
    float *pix;
    size_t count;

    *pixels = NULL;
    *width = 0;
    *height = 0;
    if (!src || GetObject(src, sizeof bm, &bm) != (int)sizeof bm || !bm.bmBits) {
        return;
    }
    if (bm.bmBitsPixel != 32 || bm.bmWidth < 1 || bm.bmHeight < 1 || bm.bmWidth > 20000 || bm.bmHeight > 20000) {
        return;
    }
    count = (size_t)bm.bmWidth * (size_t)bm.bmHeight;
    if (count > 48000000) {
        return;
    }
    pix = (float *)malloc(count * 3 * sizeof(float));
    if (!pix) {
        return;
    }
    for (y = 0; y < bm.bmHeight; y++) {
        const BYTE *row = (const BYTE *)bm.bmBits + (size_t)y * (size_t)bm.bmWidthBytes;
        for (x = 0; x < bm.bmWidth; x++) {
            float *dst = pix + ((size_t)y * (size_t)bm.bmWidth + (size_t)x) * 3;
            dst[2] = (float)zi_srgb_to_linear(row[x * 4 + 0] / 255.0);
            dst[1] = (float)zi_srgb_to_linear(row[x * 4 + 1] / 255.0);
            dst[0] = (float)zi_srgb_to_linear(row[x * 4 + 2] / 255.0);
        }
    }
    *pixels = pix;
    *width = bm.bmWidth;
    *height = bm.bmHeight;
}

static void zi_sample(const float *src, int w, int h, double x, double y, float *r, float *g, float *b) {
    int x0;
    int y0;
    int x1;
    int y1;
    double tx;
    double ty;
    const float *p00;
    const float *p10;
    const float *p01;
    const float *p11;

    if (x < 0 || y < 0 || x > (double)w - 1.0 || y > (double)h - 1.0) {
        *r = 0;
        *g = 0;
        *b = 0;
        return;
    }
    x0 = (int)x;
    y0 = (int)y;
    x1 = x0 + 1 < w ? x0 + 1 : w - 1;
    y1 = y0 + 1 < h ? y0 + 1 : h - 1;
    tx = x - x0;
    ty = y - y0;
    p00 = src + ((size_t)y0 * (size_t)w + (size_t)x0) * 3;
    p10 = src + ((size_t)y0 * (size_t)w + (size_t)x1) * 3;
    p01 = src + ((size_t)y1 * (size_t)w + (size_t)x0) * 3;
    p11 = src + ((size_t)y1 * (size_t)w + (size_t)x1) * 3;
    *r = (float)((p00[0] * (1 - tx) + p10[0] * tx) * (1 - ty) + (p01[0] * (1 - tx) + p11[0] * tx) * ty);
    *g = (float)((p00[1] * (1 - tx) + p10[1] * tx) * (1 - ty) + (p01[1] * (1 - tx) + p11[1] * tx) * ty);
    *b = (float)((p00[2] * (1 - tx) + p10[2] * tx) * (1 - ty) + (p01[2] * (1 - tx) + p11[2] * tx) * ty);
}

static float *zi_keystone(const float *src, int w, int h, double vertical, double horizontal) {
    float *dst;
    int x;
    int y;
    double k = 0.5 * vertical;
    double hk = 0.5 * horizontal;

    dst = (float *)malloc((size_t)w * (size_t)h * 3 * sizeof(float));
    if (!dst) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            double nx = ((double)x + 0.5) / (double)w - 0.5;
            double ny = ((double)y + 0.5) / (double)h - 0.5;
            double denom = 1.0 - k * ny - hk * nx;
            double weight;
            double sx;
            double sy;
            float *out = dst + ((size_t)y * (size_t)w + (size_t)x) * 3;

            if (denom > -0.000001 && denom < 0.000001) {
                out[0] = out[1] = out[2] = 0;
                continue;
            }
            weight = 1.0 / denom;
            sx = (0.5 + nx * weight) * (double)w - 0.5;
            sy = (0.5 + ny * weight) * (double)h - 0.5;
            zi_sample(src, w, h, sx, sy, &out[0], &out[1], &out[2]);
        }
    }
    return dst;
}

static float *zi_turn(const float *src, int w, int h, int quarters, int *out_w, int *out_h) {
    int q = quarters & 3;
    int dw = (q == 1 || q == 3) ? h : w;
    int dh = (q == 1 || q == 3) ? w : h;
    float *dst;
    int x;
    int y;

    dst = (float *)malloc((size_t)dw * (size_t)dh * 3 * sizeof(float));
    if (!dst) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int dx = x;
            int dy = y;
            const float *sp = src + ((size_t)y * (size_t)w + (size_t)x) * 3;
            float *dp;

            if (q == 1) {
                dx = h - 1 - y;
                dy = x;
            } else if (q == 2) {
                dx = w - 1 - x;
                dy = h - 1 - y;
            } else if (q == 3) {
                dx = y;
                dy = w - 1 - x;
            }
            dp = dst + ((size_t)dy * (size_t)dw + (size_t)dx) * 3;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
        }
    }
    *out_w = dw;
    *out_h = dh;
    return dst;
}

static int zi_inscribed(int w, int h, double radians, int *out_w, int *out_h) {
    double ang = fabs(radians);
    double sin_a;
    double cos_a;
    double ow;
    double oh;
    int width_longer = w >= h;
    double side_long = width_longer ? (double)w : (double)h;
    double side_short = width_longer ? (double)h : (double)w;

    if (ang < 0.00001) {
        *out_w = w;
        *out_h = h;
        return 1;
    }
    sin_a = sin(ang);
    cos_a = cos(ang);
    if (side_short <= 2.0 * sin_a * cos_a * side_long || fabs(sin_a - cos_a) < 0.00001) {
        double x = 0.5 * side_short;
        if (width_longer) {
            ow = x / sin_a;
            oh = x / cos_a;
        } else {
            ow = x / cos_a;
            oh = x / sin_a;
        }
    } else {
        double cos2 = cos_a * cos_a - sin_a * sin_a;
        ow = ((double)w * cos_a - (double)h * sin_a) / cos2;
        oh = ((double)h * cos_a - (double)w * sin_a) / cos2;
    }
    if (ow < 1 || oh < 1 || ow > 30000 || oh > 30000) {
        return 0;
    }
    *out_w = (int)(ow + 0.5);
    *out_h = (int)(oh + 0.5);
    return *out_w >= 1 && *out_h >= 1;
}

static float *zi_straighten(const float *src, int w, int h, double degrees, int *out_w, int *out_h) {
    double radians = degrees * ZI_PI / 180.0;
    int dw;
    int dh;
    float *dst;
    double c;
    double s;
    int x;
    int y;

    if (!zi_inscribed(w, h, radians, &dw, &dh)) {
        return NULL;
    }
    dst = (float *)malloc((size_t)dw * (size_t)dh * 3 * sizeof(float));
    if (!dst) {
        return NULL;
    }
    c = cos(radians);
    s = sin(radians);
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            double ox = (double)x - (double)dw * 0.5;
            double oy = (double)y - (double)dh * 0.5;
            double sx = ox * c - oy * s + (double)w * 0.5;
            double sy = ox * s + oy * c + (double)h * 0.5;
            float *out = dst + ((size_t)y * (size_t)dw + (size_t)x) * 3;

            zi_sample(src, w, h, sx, sy, &out[0], &out[1], &out[2]);
        }
    }
    *out_w = dw;
    *out_h = dh;
    return dst;
}

static float *zi_copy_pix(const float *src, int w, int h) {
    size_t bytes = (size_t)w * (size_t)h * 3 * sizeof(float);
    float *dst = (float *)malloc(bytes);

    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, bytes);
    return dst;
}

/* Brown–Conrady, then the cylindrical x remap. The frame size stays put. */
static float *zi_lens(const float *src, int w, int h, double k1, double k2, double fov) {
    float *dst;
    double scale;
    double half;
    int x;
    int y;

    if (w < 2 || h < 2) {
        return NULL;
    }
    scale = (w > h ? (double)w : (double)h) / 2.0;
    half = fov * ZI_PI / 180.0 / 2.0;
    if (half < 0) {
        half = 0;
    }
    if (half > ZI_PI / 2.0) {
        half = ZI_PI / 2.0;
    }
    dst = (float *)malloc((size_t)w * (size_t)h * 3 * sizeof(float));
    if (!dst) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            double gx = (double)x + 0.5;
            double gy = (double)y + 0.5;
            double nx = (gx - (double)w / 2.0) / scale;
            double ny = (gy - (double)h / 2.0) / scale;
            double radius2;
            double factor;
            double sample_x;
            double sample_y;
            float *out = dst + ((size_t)y * (size_t)w + (size_t)x) * 3;

            if (half > 0.01) {
                double x_edge = (gx - (double)w / 2.0) / ((double)w / 2.0);
                double theta;
                double src_edge;

                if (x_edge < -1.0) {
                    x_edge = -1.0;
                }
                if (x_edge > 1.0) {
                    x_edge = 1.0;
                }
                theta = x_edge * half;
                src_edge = sin(theta) / sin(half);
                nx = src_edge * ((double)w / 2.0) / scale;
            }
            radius2 = nx * nx + ny * ny;
            factor = 1.0 + k1 * radius2 + k2 * radius2 * radius2;
            sample_x = (double)w / 2.0 + nx * factor * scale;
            sample_y = (double)h / 2.0 + ny * factor * scale;
            zi_sample(src, w, h, sample_x - 0.5, sample_y - 0.5, &out[0], &out[1], &out[2]);
        }
    }
    return dst;
}

static int zi_solve8(double rows[8][9]) {
    int col;
    int row;
    int i;
    int j;

    for (col = 0; col < 8; col++) {
        int pivot = col;
        double max_abs = rows[col][col] < 0 ? -rows[col][col] : rows[col][col];

        for (row = col + 1; row < 8; row++) {
            double value = rows[row][col] < 0 ? -rows[row][col] : rows[row][col];

            if (value > max_abs) {
                max_abs = value;
                pivot = row;
            }
        }
        if (max_abs < 1e-12) {
            return 0;
        }
        if (pivot != col) {
            for (j = col; j < 9; j++) {
                double tmp = rows[col][j];

                rows[col][j] = rows[pivot][j];
                rows[pivot][j] = tmp;
            }
        }
        {
            double div = rows[col][col];

            for (j = col; j < 9; j++) {
                rows[col][j] /= div;
            }
        }
        for (i = 0; i < 8; i++) {
            double factor;

            if (i == col) {
                continue;
            }
            factor = rows[i][col];
            if (factor == 0) {
                continue;
            }
            for (j = col; j < 9; j++) {
                rows[i][j] -= factor * rows[col][j];
            }
        }
    }
    return 1;
}

static int zi_floor_to_int(double value) {
    int whole = (int)value;

    if ((double)whole > value) {
        whole--;
    }
    return whole;
}

static int zi_ceil_to_int(double value) {
    int whole = (int)value;

    if ((double)whole < value) {
        whole++;
    }
    return whole;
}

/* Maps the source corners onto the pin quad. The output is that quad's box. */
static float *zi_perspective(const float *src, int w, int h, const ZiTone *tone, int *out_w, int *out_h) {
    double source_x[4];
    double source_y[4];
    double dest_x[4];
    double dest_y[4];
    double rows[8][9];
    double coeff[8];
    double min_x;
    double min_y;
    double max_x;
    double max_y;
    int origin_x;
    int origin_y;
    int limit_x;
    int limit_y;
    int dw;
    int dh;
    int i;
    int x;
    int y;
    float *dst;

    *out_w = w;
    *out_h = h;
    if (w < 2 || h < 2) {
        return zi_copy_pix(src, w, h);
    }
    source_x[0] = 0;
    source_y[0] = 0;
    source_x[1] = (double)w;
    source_y[1] = 0;
    source_x[2] = (double)w;
    source_y[2] = (double)h;
    source_x[3] = 0;
    source_y[3] = (double)h;
    min_x = max_x = tone->corner_x[0] * (double)w;
    min_y = max_y = tone->corner_y[0] * (double)h;
    for (i = 0; i < 4; i++) {
        dest_x[i] = tone->corner_x[i] * (double)w;
        dest_y[i] = tone->corner_y[i] * (double)h;
        if (dest_x[i] < min_x) {
            min_x = dest_x[i];
        }
        if (dest_x[i] > max_x) {
            max_x = dest_x[i];
        }
        if (dest_y[i] < min_y) {
            min_y = dest_y[i];
        }
        if (dest_y[i] > max_y) {
            max_y = dest_y[i];
        }
    }
    origin_x = zi_floor_to_int(min_x);
    origin_y = zi_floor_to_int(min_y);
    limit_x = zi_ceil_to_int(max_x);
    limit_y = zi_ceil_to_int(max_y);
    dw = limit_x - origin_x;
    dh = limit_y - origin_y;
    if (dw < 1 || dh < 1 || dw > 30000 || dh > 30000 || (double)dw * (double)dh > 64000000.0) {
        return zi_copy_pix(src, w, h);
    }
    memset(rows, 0, sizeof rows);
    for (i = 0; i < 4; i++) {
        int row = i * 2;
        double u = dest_x[i];
        double v = dest_y[i];
        double sx = source_x[i];
        double sy = source_y[i];

        rows[row][0] = u;
        rows[row][1] = v;
        rows[row][2] = 1;
        rows[row][6] = -u * sx;
        rows[row][7] = -v * sx;
        rows[row][8] = sx;
        rows[row + 1][3] = u;
        rows[row + 1][4] = v;
        rows[row + 1][5] = 1;
        rows[row + 1][6] = -u * sy;
        rows[row + 1][7] = -v * sy;
        rows[row + 1][8] = sy;
    }
    if (!zi_solve8(rows)) {
        return zi_copy_pix(src, w, h);
    }
    for (i = 0; i < 8; i++) {
        coeff[i] = rows[i][8];
    }
    dst = (float *)malloc((size_t)dw * (size_t)dh * 3 * sizeof(float));
    if (!dst) {
        return NULL;
    }
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            double px = (double)origin_x + (double)x + 0.5;
            double py = (double)origin_y + (double)y + 0.5;
            double den = coeff[6] * px + coeff[7] * py + 1.0;
            double sx;
            double sy;
            float *out = dst + ((size_t)y * (size_t)dw + (size_t)x) * 3;

            if (den > -1e-8 && den < 1e-8) {
                out[0] = out[1] = out[2] = 0;
                continue;
            }
            sx = (coeff[0] * px + coeff[1] * py + coeff[2]) / den;
            sy = (coeff[3] * px + coeff[4] * py + coeff[5]) / den;
            zi_sample(src, w, h, sx - 0.5, sy - 0.5, &out[0], &out[1], &out[2]);
        }
    }
    *out_w = dw;
    *out_h = dh;
    return dst;
}

static void zi_box_h(const float *src, float *dst, int w, int h, int radius) {
    int span = radius * 2 + 1;
    int y;
    int x;

    for (y = 0; y < h; y++) {
        const float *row = src + (size_t)y * (size_t)w * 3;
        float *out = dst + (size_t)y * (size_t)w * 3;
        double sum0 = 0;
        double sum1 = 0;
        double sum2 = 0;

        for (x = -radius; x <= radius; x++) {
            int sx = x < 0 ? 0 : (x >= w ? w - 1 : x);
            sum0 += row[(size_t)sx * 3];
            sum1 += row[(size_t)sx * 3 + 1];
            sum2 += row[(size_t)sx * 3 + 2];
        }
        for (x = 0; x < w; x++) {
            int remove = x - radius;
            int add = x + radius + 1;

            out[(size_t)x * 3] = (float)(sum0 / span);
            out[(size_t)x * 3 + 1] = (float)(sum1 / span);
            out[(size_t)x * 3 + 2] = (float)(sum2 / span);
            if (remove < 0) {
                remove = 0;
            }
            if (remove >= w) {
                remove = w - 1;
            }
            if (add < 0) {
                add = 0;
            }
            if (add >= w) {
                add = w - 1;
            }
            sum0 += row[(size_t)add * 3] - row[(size_t)remove * 3];
            sum1 += row[(size_t)add * 3 + 1] - row[(size_t)remove * 3 + 1];
            sum2 += row[(size_t)add * 3 + 2] - row[(size_t)remove * 3 + 2];
        }
    }
}

static void zi_box_v(const float *src, float *dst, int w, int h, int radius) {
    int span = radius * 2 + 1;
    int x;
    int y;

    for (x = 0; x < w; x++) {
        double sum0 = 0;
        double sum1 = 0;
        double sum2 = 0;

        for (y = -radius; y <= radius; y++) {
            int sy = y < 0 ? 0 : (y >= h ? h - 1 : y);
            const float *p = src + ((size_t)sy * (size_t)w + (size_t)x) * 3;
            sum0 += p[0];
            sum1 += p[1];
            sum2 += p[2];
        }
        for (y = 0; y < h; y++) {
            int remove = y - radius;
            int add = y + radius + 1;
            float *out = dst + ((size_t)y * (size_t)w + (size_t)x) * 3;
            const float *rp;
            const float *ap;

            out[0] = (float)(sum0 / span);
            out[1] = (float)(sum1 / span);
            out[2] = (float)(sum2 / span);
            if (remove < 0) {
                remove = 0;
            }
            if (remove >= h) {
                remove = h - 1;
            }
            if (add < 0) {
                add = 0;
            }
            if (add >= h) {
                add = h - 1;
            }
            rp = src + ((size_t)remove * (size_t)w + (size_t)x) * 3;
            ap = src + ((size_t)add * (size_t)w + (size_t)x) * 3;
            sum0 += ap[0] - rp[0];
            sum1 += ap[1] - rp[1];
            sum2 += ap[2] - rp[2];
        }
    }
}

static void zi_blur(float *pix, float *tmp, int w, int h, int radius) {
    int pass;

    if (radius < 1) {
        return;
    }
    if (radius > 128) {
        radius = 128;
    }
    for (pass = 0; pass < 3; pass++) {
        zi_box_h(pix, tmp, w, h, radius);
        zi_box_v(tmp, pix, w, h, radius);
    }
}

static void zi_detail(float *pix, float *tmp, float *scratch, int w, int h, double amount, double radius) {
    size_t count;
    size_t i;
    int rad;

    if (!zi_active(amount) || radius < 0.5) {
        return;
    }
    rad = (int)(radius + 0.5);
    if (rad < 1) {
        return;
    }
    count = (size_t)w * (size_t)h * 3;
    memcpy(scratch, pix, count * sizeof(float));
    if (amount > 0) {
        zi_blur(scratch, tmp, w, h, rad);
        for (i = 0; i < count; i++) {
            pix[i] = (float)(pix[i] + amount * (pix[i] - scratch[i]));
        }
    } else {
        double t = -amount;
        int wide = (int)(radius * t + 0.5);
        if (wide < 1) {
            wide = 1;
        }
        zi_blur(scratch, tmp, w, h, wide);
        for (i = 0; i < count; i++) {
            pix[i] = (float)(pix[i] * (1.0 - t) + scratch[i] * t);
        }
    }
}

/* Encoded sRGB, extended above 1 so exposure headroom survives the tone pass. */
static double zi_encode(double linear) {
    if (linear <= 0) {
        return 0;
    }
    if (linear <= 0.0031308) {
        return 12.92 * linear;
    }
    return 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
}

static double zi_decode(double encoded) {
    if (encoded <= 0) {
        return 0;
    }
    if (encoded <= 0.04045) {
        return encoded / 12.92;
    }
    return pow((encoded + 0.055) / 1.055, 2.4);
}

/* Full-slider moves in encoded sRGB. A smaller slider is the same fraction. */
static const double k_end_span = 0.35;
static const double k_knot_span = 0.20;
static const double k_bright_span = 0.35;

/* Contrast keeps black, mid gray, and white. Midtone slope is 1 + 0.5 * slider. */
static double zi_contrast_curve(double x, double contrast) {
    double amount = 0.5 * contrast;
    double end_slope;
    double u;

    if (x <= 0) {
        return 0;
    }
    end_slope = 1.0 - 2.0 * amount;
    if (x >= 1.0) {
        return 1.0 + (x - 1.0) * end_slope;
    }
    u = x - 0.5;
    return 0.5 + u * (1.0 + amount * (1.0 - 4.0 * u * u));
}

/* Smooth tone curve. Knots sit at encoded 0.25 and 0.75. Identity is a straight line. */
static double zi_curve(double x, double y_black, double y_white) {
    double xs[5] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    double ys[5];
    double slope[4];
    double tangent[5];
    double m0;
    double m1;
    double h;
    double t;
    double t2;
    double t3;
    double a;
    double b;
    double hypot2;
    int i;
    int seg;

    if (x <= 0) {
        return 0;
    }
    if (x >= 1.0) {
        return x;
    }
    ys[0] = 0;
    ys[1] = y_black;
    ys[2] = 0.5;
    ys[3] = y_white;
    ys[4] = 1.0;
    for (i = 0; i < 4; i++) {
        slope[i] = (ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i]);
    }
    tangent[0] = slope[0];
    tangent[4] = slope[3];
    for (i = 1; i <= 3; i++) {
        if (slope[i - 1] * slope[i] <= 0) {
            tangent[i] = 0;
        } else {
            tangent[i] = (slope[i - 1] + slope[i]) * 0.5;
        }
    }
    seg = 0;
    if (x > 0.25) {
        seg = 1;
    }
    if (x > 0.5) {
        seg = 2;
    }
    if (x > 0.75) {
        seg = 3;
    }
    m0 = tangent[seg];
    m1 = tangent[seg + 1];
    if (slope[seg] > -1.0e-12 && slope[seg] < 1.0e-12) {
        m0 = 0;
        m1 = 0;
    } else {
        a = m0 / slope[seg];
        b = m1 / slope[seg];
        hypot2 = a * a + b * b;
        if (hypot2 > 9.0) {
            double scale = 3.0 / sqrt(hypot2);
            m0 = scale * a * slope[seg];
            m1 = scale * b * slope[seg];
        }
    }
    h = xs[seg + 1] - xs[seg];
    t = (x - xs[seg]) / h;
    t2 = t * t;
    t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * ys[seg]
        + (t3 - 2.0 * t2 + t) * h * m0
        + (-2.0 * t3 + 3.0 * t2) * ys[seg + 1]
        + (t3 - t2) * h * m1;
}

static int zi_finite(double value) {
    return value == value && value < 1.0e30 && value > -1.0e30;
}

static double zi_cbrt(double value) {
    if (value < 0) {
        return -pow(-value, 1.0 / 3.0);
    }
    return pow(value, 1.0 / 3.0);
}

/* Linear sRGB to XYZ D65. White is the matrix applied to (1, 1, 1), so white round-trips. */
static void zi_srgb_xyz(double r, double g, double b, double *x, double *y, double *z) {
    *x = 0.4124564390896922 * r + 0.357576077643909 * g + 0.1804374834215243 * b;
    *y = 0.21267285140562253 * r + 0.715152155287818 * g + 0.07217499330655958 * b;
    *z = 0.01933389558232931 * r + 0.119192025881303 * g + 0.9503040785363679 * b;
}

static void zi_xyz_srgb(double x, double y, double z, double *r, double *g, double *b) {
    *r = 3.2404541621141045 * x - 1.5371385127977166 * y - 0.498531409556016 * z;
    *g = -0.9692660305051868 * x + 1.8760108454466942 * y + 0.04155601753034984 * z;
    *b = 0.05564343095911469 * x - 0.2040259135167538 * y + 1.0572251882231791 * z;
}

static double zi_lab_pivot(double t) {
    const double epsilon = 216.0 / 24389.0;
    const double kappa = 24389.0 / 27.0;

    if (t > epsilon) {
        return zi_cbrt(t);
    }
    return (kappa * t + 16.0) / 116.0;
}

static double zi_lab_inverse(double t) {
    const double delta = 6.0 / 29.0;

    if (t > delta) {
        return t * t * t;
    }
    return 3.0 * delta * delta * (t - 4.0 / 29.0);
}

static void zi_linear_lab(double r, double g, double b, double *L, double *a, double *lab_b) {
    double x;
    double y;
    double z;
    double wx;
    double wy;
    double wz;
    double fx;
    double fy;
    double fz;

    zi_srgb_xyz(r, g, b, &x, &y, &z);
    zi_srgb_xyz(1, 1, 1, &wx, &wy, &wz);
    fx = zi_lab_pivot(x / wx);
    fy = zi_lab_pivot(y / wy);
    fz = zi_lab_pivot(z / wz);
    *L = 116.0 * fy - 16.0;
    *a = 500.0 * (fx - fy);
    *lab_b = 200.0 * (fy - fz);
}

static void zi_lab_linear(double L, double a, double lab_b, double *r, double *g, double *b) {
    double fy = (L + 16.0) / 116.0;
    double fx = fy + a / 500.0;
    double fz = fy - lab_b / 200.0;
    double wx;
    double wy;
    double wz;
    double x;
    double y;
    double z;

    zi_srgb_xyz(1, 1, 1, &wx, &wy, &wz);
    x = wx * zi_lab_inverse(fx);
    y = wy * zi_lab_inverse(fy);
    z = wz * zi_lab_inverse(fz);
    zi_xyz_srgb(x, y, z, r, g, b);
}

static double zi_chroma(double r, double g, double b) {
    double L;
    double a;
    double lab_b;

    zi_linear_lab(r, g, b, &L, &a, &lab_b);
    return hypot(a, lab_b);
}

static void zi_kelvin_rgb(double kelvin, double *r, double *g, double *b) {
    double temp = zi_clamp(kelvin, 1000, 40000) / 100.0;

    if (temp <= 66.0) {
        *r = 1;
        *g = zi_clamp(0.3900815787690196 * log(temp) - 0.6318414437886275, 0, 1);
    } else {
        *r = zi_clamp(1.292936186062745 * pow(temp - 60.0, -0.1332047592), 0, 1);
        *g = zi_clamp(1.129890860895294 * pow(temp - 60.0, -0.0755148492), 0, 1);
    }
    if (temp >= 66.0) {
        *b = 1;
    } else if (temp <= 19.0) {
        *b = 0;
    } else {
        *b = zi_clamp(0.5432067891101961 * log(temp - 10.0) - 1.19625408914, 0, 1);
    }
    if (*r < 0.02) {
        *r = 0.02;
    }
    if (*g < 0.02) {
        *g = 0.02;
    }
    if (*b < 0.02) {
        *b = 0.02;
    }
}

static void zi_temp_gains(double temperature, double tint, double *gain_r, double *gain_g, double *gain_b) {
    double nr;
    double ng;
    double nb;
    double tr;
    double tg;
    double tb;

    zi_kelvin_rgb(6500, &nr, &ng, &nb);
    zi_kelvin_rgb(temperature, &tr, &tg, &tb);
    tg = zi_clamp(tg * (1.0 - tint / 500.0), 0.02, 4);
    *gain_r = nr / tr;
    *gain_g = ng / tg;
    *gain_b = nb / tb;
}

/* 32³ trilinear cube, red fastest, matching CIColorCube. Built for this grade only. */
static void zi_apply_skin(float *pix, size_t count, double delta_a, double delta_b) {
    enum { N = 32 };
    float *cube;
    const double last = (double)(N - 1);
    int red;
    int green;
    int blue;
    size_t i;

    cube = (float *)malloc((size_t)N * (size_t)N * (size_t)N * 3 * sizeof(float));
    if (!cube) {
        return;
    }
    for (blue = 0; blue < N; blue++) {
        for (green = 0; green < N; green++) {
            for (red = 0; red < N; red++) {
                double r;
                double g;
                double b;
                double L;
                double a;
                double lab_b;
                size_t slot = (((size_t)blue * N + (size_t)green) * N + (size_t)red) * 3;

                zi_linear_lab((double)red / last, (double)green / last, (double)blue / last, &L, &a, &lab_b);
                zi_lab_linear(L, a + delta_a, lab_b + delta_b, &r, &g, &b);
                cube[slot] = (float)zi_clamp(r, 0, 1);
                cube[slot + 1] = (float)zi_clamp(g, 0, 1);
                cube[slot + 2] = (float)zi_clamp(b, 0, 1);
            }
        }
    }
    for (i = 0; i < count; i++) {
        double sr = zi_clamp(pix[i * 3], 0, 1) * last;
        double sg = zi_clamp(pix[i * 3 + 1], 0, 1) * last;
        double sb = zi_clamp(pix[i * 3 + 2], 0, 1) * last;
        int r0 = (int)sr;
        int g0 = (int)sg;
        int b0 = (int)sb;
        double fr;
        double fg;
        double fb;
        int channel;

        if (r0 >= N - 1) {
            r0 = N - 2;
            fr = 1;
        } else {
            fr = sr - r0;
        }
        if (g0 >= N - 1) {
            g0 = N - 2;
            fg = 1;
        } else {
            fg = sg - g0;
        }
        if (b0 >= N - 1) {
            b0 = N - 2;
            fb = 1;
        } else {
            fb = sb - b0;
        }
        for (channel = 0; channel < 3; channel++) {
            float c000 = cube[(((size_t)b0 * N + (size_t)g0) * N + (size_t)r0) * 3 + channel];
            float c100 = cube[(((size_t)b0 * N + (size_t)g0) * N + (size_t)(r0 + 1)) * 3 + channel];
            float c010 = cube[(((size_t)b0 * N + (size_t)(g0 + 1)) * N + (size_t)r0) * 3 + channel];
            float c110 = cube[(((size_t)b0 * N + (size_t)(g0 + 1)) * N + (size_t)(r0 + 1)) * 3 + channel];
            float c001 = cube[(((size_t)(b0 + 1) * N + (size_t)g0) * N + (size_t)r0) * 3 + channel];
            float c101 = cube[(((size_t)(b0 + 1) * N + (size_t)g0) * N + (size_t)(r0 + 1)) * 3 + channel];
            float c011 = cube[(((size_t)(b0 + 1) * N + (size_t)(g0 + 1)) * N + (size_t)r0) * 3 + channel];
            float c111 = cube[(((size_t)(b0 + 1) * N + (size_t)(g0 + 1)) * N + (size_t)(r0 + 1)) * 3 + channel];
            double c00 = c000 + (c100 - c000) * fr;
            double c10 = c010 + (c110 - c010) * fr;
            double c01 = c001 + (c101 - c001) * fr;
            double c11 = c011 + (c111 - c011) * fr;
            double c0 = c00 + (c10 - c00) * fg;
            double c1 = c01 + (c11 - c01) * fg;

            pix[i * 3 + channel] = (float)(c0 + (c1 - c0) * fb);
        }
    }
    free(cube);
}

static void zi_grade_pixels(float *pix, float *tmp, float *scratch, int w, int h, const ZiTone *tone) {
    size_t count = (size_t)w * (size_t)h;
    size_t i;
    double exposure = pow(2.0, tone->exposure);
    double saturation = 1.0 + tone->saturation;
    double y_black = zi_clamp(0.25 + k_knot_span * tone->blacks, 0.02, 0.48);
    double y_white = zi_clamp(0.75 + k_knot_span * tone->whites, 0.52, 0.98);
    double gain_r = 1;
    double gain_g = 1;
    double gain_b = 1;
    double span;
    double gamma = 1;
    double long_edge = w > h ? (double)w : (double)h;
    int use_ends = zi_active(tone->shadows) || zi_active(tone->highlights);
    int use_curve = zi_active(tone->whites) || zi_active(tone->blacks);
    int use_contrast = zi_active(tone->brightness) || zi_active(tone->contrast);
    int use_tone = use_ends || use_curve || use_contrast;
    int use_sat = zi_active(tone->saturation);
    int use_temp = !zi_near(tone->temperature, 6500) || zi_active(tone->tint);
    int use_spectrum = tone->spectrum_low > 0.0005 || tone->spectrum_high < 0.9995 || !zi_near(tone->spectrum_mid, 0.5);

    if (use_temp) {
        zi_temp_gains(tone->temperature, tone->tint, &gain_r, &gain_g, &gain_b);
    }
    if (use_spectrum) {
        span = tone->spectrum_high - tone->spectrum_low;
        if (span > 0.0001) {
            double placed = zi_clamp((tone->spectrum_mid - tone->spectrum_low) / span, 0.05, 0.95);
            if (!zi_near(placed, 0.5)) {
                gamma = zi_clamp(log(0.5) / log(placed), 0.15, 6);
            }
        }
    }
    for (i = 0; i < count; i++) {
        double r = pix[i * 3] * exposure;
        double g = pix[i * 3 + 1] * exposure;
        double b = pix[i * 3 + 2] * exposure;

        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;
        if (use_tone) {
            r = zi_encode(r);
            g = zi_encode(g);
            b = zi_encode(b);
            if (use_ends) {
                double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                double delta = 0;
                if (zi_active(tone->shadows)) {
                    delta += tone->shadows * k_end_span * (1.0 - zi_smooth(0.0, 0.50, luma));
                }
                if (zi_active(tone->highlights)) {
                    delta += tone->highlights * k_end_span * zi_smooth(0.50, 1.0, luma);
                }
                r += delta;
                g += delta;
                b += delta;
                if (r < 0) r = 0;
                if (g < 0) g = 0;
                if (b < 0) b = 0;
            }
            if (use_curve) {
                r = zi_curve(r, y_black, y_white);
                g = zi_curve(g, y_black, y_white);
                b = zi_curve(b, y_black, y_white);
            }
            if (use_contrast) {
                double bright = tone->brightness * k_bright_span;
                r = zi_contrast_curve(r, tone->contrast) + bright;
                g = zi_contrast_curve(g, tone->contrast) + bright;
                b = zi_contrast_curve(b, tone->contrast) + bright;
                if (r < 0) r = 0;
                if (g < 0) g = 0;
                if (b < 0) b = 0;
            }
            r = zi_decode(r);
            g = zi_decode(g);
            b = zi_decode(b);
        }
        if (use_sat) {
            double gray = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            r = gray + (r - gray) * saturation;
            g = gray + (g - gray) * saturation;
            b = gray + (b - gray) * saturation;
        }
        if (zi_active(tone->vibrance)) {
            double maxc = r > g ? r : g;
            double minc = r < g ? r : g;
            double sat;
            double boost;
            double gray;
            if (b > maxc) maxc = b;
            if (b < minc) minc = b;
            sat = maxc > 0.000001 ? (maxc - minc) / maxc : 0;
            if (sat < 0) sat = 0;
            if (sat > 1) sat = 1;
            boost = tone->vibrance >= 0 ? tone->vibrance * (1.0 - sat) : tone->vibrance * sat;
            gray = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            r = gray + (r - gray) * (1.0 + boost);
            g = gray + (g - gray) * (1.0 + boost);
            b = gray + (b - gray) * (1.0 + boost);
        }
        if (use_temp) {
            r *= gain_r;
            g *= gain_g;
            b *= gain_b;
        }
        if (use_spectrum) {
            double low = tone->spectrum_low;
            double high = tone->spectrum_high;
            r = zi_linear_to_srgb(zi_clamp(r, 0, 1));
            g = zi_linear_to_srgb(zi_clamp(g, 0, 1));
            b = zi_linear_to_srgb(zi_clamp(b, 0, 1));
            r = r <= low ? 0 : (r >= high ? 1 : (r - low) / (high - low));
            g = g <= low ? 0 : (g >= high ? 1 : (g - low) / (high - low));
            b = b <= low ? 0 : (b >= high ? 1 : (b - low) / (high - low));
            if (!zi_near(gamma, 1)) {
                r = pow(zi_clamp(r, 0, 1), gamma);
                g = pow(zi_clamp(g, 0, 1), gamma);
                b = pow(zi_clamp(b, 0, 1), gamma);
            }
            r = zi_srgb_to_linear(r);
            g = zi_srgb_to_linear(g);
            b = zi_srgb_to_linear(b);
        }
        pix[i * 3] = (float)r;
        pix[i * 3 + 1] = (float)g;
        pix[i * 3 + 2] = (float)b;
    }
    zi_detail(pix, tmp, scratch, w, h, tone->texture, 1.5 * long_edge / 2560.0);
    zi_detail(pix, tmp, scratch, w, h, tone->clarity, 30.0 * long_edge / 2560.0);
    if (tone->dehaze > 0.0000001) {
        int rad = (int)(32.0 * long_edge / 2560.0 + 0.5);
        if (rad < 1) {
            rad = 1;
        }
        for (i = 0; i < count; i++) {
            float veil = pix[i * 3];
            if (pix[i * 3 + 1] < veil) veil = pix[i * 3 + 1];
            if (pix[i * 3 + 2] < veil) veil = pix[i * 3 + 2];
            tmp[i * 3] = tmp[i * 3 + 1] = tmp[i * 3 + 2] = veil;
        }
        zi_blur(tmp, scratch, w, h, rad);
        for (i = 0; i < count; i++) {
            pix[i * 3] = (float)(pix[i * 3] - tone->dehaze * tmp[i * 3]);
            pix[i * 3 + 1] = (float)(pix[i * 3 + 1] - tone->dehaze * tmp[i * 3]);
            pix[i * 3 + 2] = (float)(pix[i * 3 + 2] - tone->dehaze * tmp[i * 3]);
            if (pix[i * 3] < 0) pix[i * 3] = 0;
            if (pix[i * 3 + 1] < 0) pix[i * 3 + 1] = 0;
            if (pix[i * 3 + 2] < 0) pix[i * 3 + 2] = 0;
        }
    }
    if (tone->has_skin && (zi_active(tone->skin_delta_a) || zi_active(tone->skin_delta_b))) {
        zi_apply_skin(pix, count, tone->skin_delta_a, tone->skin_delta_b);
    }
}

static HBITMAP zi_write_dib(const float *pix, int w, int h) {
    BITMAPINFO info;
    BYTE *bits = NULL;
    HBITMAP dib;
    int x;
    int y;
    int stride;

    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!dib || !bits) {
        if (dib) {
            DeleteObject(dib);
        }
        return NULL;
    }
    stride = w * 4;
    for (y = 0; y < h; y++) {
        BYTE *row = bits + (size_t)y * (size_t)stride;
        for (x = 0; x < w; x++) {
            const float *p = pix + ((size_t)y * (size_t)w + (size_t)x) * 3;
            row[x * 4 + 0] = (BYTE)zi_byte(zi_linear_to_srgb(p[2]));
            row[x * 4 + 1] = (BYTE)zi_byte(zi_linear_to_srgb(p[1]));
            row[x * 4 + 2] = (BYTE)zi_byte(zi_linear_to_srgb(p[0]));
            row[x * 4 + 3] = 255;
        }
    }
    return dib;
}

/* Copies the integral crop. Returns 1 with a new buffer, 0 when unchanged, -1 on allocation failure. */
static int zi_crop_copy(const float *pix, int w, int h, const ZiTone *tone,
                        float **out, int *out_w, int *out_h) {
    int x0;
    int y0;
    int x1;
    int y1;
    int cw;
    int ch;
    int y;
    float *dest;

    *out = NULL;
    *out_w = w;
    *out_h = h;
    if (!zi_crop_active(tone) || w < 1 || h < 1) {
        return 0;
    }
    x0 = (int)floor(tone->crop_x * (double)w);
    y0 = (int)floor(tone->crop_y * (double)h);
    x1 = (int)ceil((tone->crop_x + tone->crop_w) * (double)w);
    y1 = (int)ceil((tone->crop_y + tone->crop_h) * (double)h);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > w) {
        x1 = w;
    }
    if (y1 > h) {
        y1 = h;
    }
    cw = x1 - x0;
    ch = y1 - y0;
    if (cw < 1 || ch < 1 || (x0 == 0 && y0 == 0 && cw == w && ch == h)) {
        return 0;
    }
    dest = (float *)malloc((size_t)cw * (size_t)ch * 3 * sizeof(float));
    if (!dest) {
        return -1;
    }
    for (y = 0; y < ch; y++) {
        memcpy(dest + ((size_t)y * (size_t)cw * 3),
               pix + (((size_t)(y0 + y) * (size_t)w + (size_t)x0) * 3),
               (size_t)cw * 3 * sizeof(float));
    }
    *out = dest;
    *out_w = cw;
    *out_h = ch;
    return 1;
}

static float *zi_down_long(const float *src, int w, int h, int edge, int *out_w, int *out_h) {
    int long_side = w > h ? w : h;
    int dw;
    int dh;
    int x;
    int y;
    float *dst;

    if (w < 1 || h < 1) {
        return NULL;
    }
    if (long_side <= edge) {
        dw = w;
        dh = h;
    } else {
        double scale = (double)edge / (double)long_side;

        dw = (int)((double)w * scale + 0.5);
        dh = (int)((double)h * scale + 0.5);
        if (dw < 1) {
            dw = 1;
        }
        if (dh < 1) {
            dh = 1;
        }
    }
    dst = (float *)malloc((size_t)dw * (size_t)dh * 3 * sizeof(float));
    if (!dst) {
        return NULL;
    }
    if (dw == w && dh == h) {
        memcpy(dst, src, (size_t)dw * (size_t)dh * 3 * sizeof(float));
    } else {
        for (y = 0; y < dh; y++) {
            for (x = 0; x < dw; x++) {
                float *out = dst + ((size_t)y * (size_t)dw + (size_t)x) * 3;
                double sx = ((double)x + 0.5) * (double)w / (double)dw - 0.5;
                double sy = ((double)y + 0.5) * (double)h / (double)dh - 0.5;

                zi_sample(src, w, h, sx, sy, &out[0], &out[1], &out[2]);
            }
        }
    }
    *out_w = dw;
    *out_h = dh;
    return dst;
}

/* 256 bins of display luminance. Spectrum is held at identity so a handle drag does not move the graph. */
static void zi_measure_spectrum(const float *pix, int w, int h, const ZiTone *tone, int bins[ZI_SPECTRUM_BINS]) {
    ZiTone plain;
    float *scaled;
    float *tmp;
    float *scratch;
    int sw = 0;
    int sh = 0;
    int i;
    int count;

    memset(bins, 0, sizeof(int) * ZI_SPECTRUM_BINS);
    plain = *tone;
    plain.spectrum_low = 0;
    plain.spectrum_mid = 0.5;
    plain.spectrum_high = 1;
    scaled = zi_down_long(pix, w, h, 160, &sw, &sh);
    if (!scaled || sw < 1 || sh < 1) {
        free(scaled);
        return;
    }
    count = sw * sh;
    tmp = (float *)malloc((size_t)count * 3 * sizeof(float));
    scratch = (float *)malloc((size_t)count * 3 * sizeof(float));
    if (!tmp || !scratch) {
        free(scaled);
        free(tmp);
        free(scratch);
        return;
    }
    zi_grade_pixels(scaled, tmp, scratch, sw, sh, &plain);
    for (i = 0; i < count; i++) {
        int r = zi_byte(zi_linear_to_srgb(scaled[i * 3]));
        int g = zi_byte(zi_linear_to_srgb(scaled[i * 3 + 1]));
        int b = zi_byte(zi_linear_to_srgb(scaled[i * 3 + 2]));
        int luma = (54 * r + 183 * g + 19 * b) >> 8;

        if (luma < 0) {
            luma = 0;
        }
        if (luma > 255) {
            luma = 255;
        }
        bins[luma]++;
    }
    free(scaled);
    free(tmp);
    free(scratch);
}

void zi_spectrum_count_dib(HBITMAP dib, int bins[ZI_SPECTRUM_BINS]) {
    BITMAP bm;
    int step;
    int x;
    int y;
    int long_side;

    memset(bins, 0, sizeof(int) * ZI_SPECTRUM_BINS);
    if (!dib || GetObject(dib, sizeof bm, &bm) != (int)sizeof bm || !bm.bmBits) {
        return;
    }
    if (bm.bmBitsPixel != 32 || bm.bmWidth < 1 || bm.bmHeight < 1) {
        return;
    }
    long_side = bm.bmWidth > bm.bmHeight ? bm.bmWidth : bm.bmHeight;
    step = long_side / 160;
    if (step < 1) {
        step = 1;
    }
    for (y = 0; y < bm.bmHeight; y += step) {
        const BYTE *row = (const BYTE *)bm.bmBits + (size_t)y * (size_t)bm.bmWidthBytes;

        for (x = 0; x < bm.bmWidth; x += step) {
            int b = row[x * 4];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];
            int luma = (54 * r + 183 * g + 19 * b) >> 8;

            if (luma < 0) {
                luma = 0;
            }
            if (luma > 255) {
                luma = 255;
            }
            bins[luma]++;
        }
    }
}

static HBITMAP zi_tone_render_ex(HBITMAP base, const ZiTone *tone, const ZiTouchDoc *touch, int *out_w, int *out_h,
                                 int *bins, float **stage) {
    float *pix = NULL;
    float *tmp = NULL;
    float *scratch = NULL;
    float *replaced;
    int w = 0;
    int h = 0;
    HBITMAP dib = NULL;
    size_t count;

    if (out_w) {
        *out_w = 0;
    }
    if (out_h) {
        *out_h = 0;
    }
    if (bins) {
        memset(bins, 0, sizeof(int) * ZI_SPECTRUM_BINS);
    }
    zi_read_base(base, &pix, &w, &h);
    if (!pix) {
        return NULL;
    }
    if (zi_lens_active(tone)) {
        replaced = zi_lens(pix, w, h, tone->k1, tone->k2, tone->fov);
        if (!replaced) {
            free(pix);
            return NULL;
        }
        free(pix);
        pix = replaced;
    }
    if (tone->has_corners) {
        if (zi_corners_warp(tone)) {
            int dw = w;
            int dh = h;

            replaced = zi_perspective(pix, w, h, tone, &dw, &dh);
            if (!replaced) {
                free(pix);
                return NULL;
            }
            free(pix);
            pix = replaced;
            w = dw;
            h = dh;
        }
    } else if (zi_active(tone->vertical) || zi_active(tone->horizontal)) {
        replaced = zi_keystone(pix, w, h, tone->vertical, tone->horizontal);
        if (!replaced) {
            free(pix);
            return NULL;
        }
        free(pix);
        pix = replaced;
    }
    if ((tone->turns & 3) != 0) {
        int dw = w;
        int dh = h;
        replaced = zi_turn(pix, w, h, tone->turns, &dw, &dh);
        if (!replaced) {
            free(pix);
            return NULL;
        }
        free(pix);
        pix = replaced;
        w = dw;
        h = dh;
    }
    if (zi_active(tone->straighten)) {
        int dw = w;
        int dh = h;
        replaced = zi_straighten(pix, w, h, tone->straighten, &dw, &dh);
        if (!replaced) {
            free(pix);
            return NULL;
        }
        free(pix);
        pix = replaced;
        w = dw;
        h = dh;
    }
    count = (size_t)w * (size_t)h;
    tmp = (float *)malloc(count * 3 * sizeof(float));
    scratch = (float *)malloc(count * 3 * sizeof(float));
    if (!tmp || !scratch) {
        free(pix);
        free(tmp);
        free(scratch);
        return NULL;
    }
    if (zi_active(tone->sharpness)) {
        double long_edge = w > h ? (double)w : (double)h;

        zi_detail(pix, tmp, scratch, w, h, tone->sharpness, long_edge / 2560.0);
    }
    {
        float *cropped = NULL;
        int cw = w;
        int ch = h;
        int crop_status = zi_crop_copy(pix, w, h, tone, &cropped, &cw, &ch);

        if (crop_status < 0) {
            free(pix);
            free(tmp);
            free(scratch);
            return NULL;
        }
        if (crop_status > 0) {
            free(pix);
            pix = cropped;
            w = cw;
            h = ch;
            free(tmp);
            free(scratch);
            count = (size_t)w * (size_t)h;
            tmp = (float *)malloc(count * 3 * sizeof(float));
            scratch = (float *)malloc(count * 3 * sizeof(float));
            if (!tmp || !scratch) {
                free(pix);
                free(tmp);
                free(scratch);
                return NULL;
            }
        }
    }
    if (touch) {
        zi_touch_apply(pix, w, h, touch);
    }
    if (stage) {
        *stage = pix;
        free(tmp);
        free(scratch);
        if (out_w) {
            *out_w = w;
        }
        if (out_h) {
            *out_h = h;
        }
        return NULL;
    }
    if (bins && !zi_spectrum_identity(tone)) {
        zi_measure_spectrum(pix, w, h, tone, bins);
    }
    zi_grade_pixels(pix, tmp, scratch, w, h, tone);
    dib = zi_write_dib(pix, w, h);
    free(pix);
    free(tmp);
    free(scratch);
    if (dib) {
        if (out_w) {
            *out_w = w;
        }
        if (out_h) {
            *out_h = h;
        }
    }
    return dib;
}

HBITMAP zi_tone_render(HBITMAP base, const ZiTone *tone, const ZiTouchDoc *touch, int *out_w, int *out_h, int *bins) {
    return zi_tone_render_ex(base, tone, touch, out_w, out_h, bins, NULL);
}

float *zi_tone_stage_rgb(HBITMAP base, const ZiTone *tone, const ZiTouchDoc *touch, int *out_w, int *out_h) {
    float *pix = NULL;

    zi_tone_render_ex(base, tone, touch, out_w, out_h, NULL, &pix);
    return pix;
}

/* Geometry is already in the base bitmap, so Auto can count those bytes. */
static int zi_auto_base_ready(const ZiTone *tone) {
    return (tone->turns & 3) == 0 &&
           !zi_active(tone->straighten) &&
           !zi_active(tone->vertical) &&
           !zi_active(tone->horizontal) &&
           !zi_active(tone->sharpness) &&
           !zi_lens_active(tone) &&
           !zi_corners_warp(tone) &&
           !zi_crop_active(tone);
}

static void zi_auto_clear_grade(ZiTone *tone) {
    tone->exposure = 0;
    tone->brightness = 0;
    tone->contrast = 0;
    tone->highlights = 0;
    tone->shadows = 0;
    tone->whites = 0;
    tone->blacks = 0;
    tone->texture = 0;
    tone->clarity = 0;
    tone->dehaze = 0;
    tone->vibrance = 0;
    tone->saturation = 0;
    tone->temperature = 6500;
    tone->tint = 0;
    tone->spectrum_low = 0;
    tone->spectrum_mid = 0.5;
    tone->spectrum_high = 1;
    tone->has_skin = 0;
    tone->skin_preset = 0;
    tone->skin_delta_a = 0;
    tone->skin_delta_b = 0;
}

static int zi_auto_histogram(HBITMAP dib, int histogram[256]) {
    BITMAP bm;
    int x;
    int y;
    int total = 0;

    memset(histogram, 0, sizeof(int) * 256);
    if (!dib || GetObject(dib, sizeof bm, &bm) != (int)sizeof bm || !bm.bmBits) {
        return 0;
    }
    if (bm.bmBitsPixel != 32 || bm.bmWidth < 2 || bm.bmHeight < 2) {
        return 0;
    }
    for (y = 0; y < bm.bmHeight; y++) {
        const BYTE *row = (const BYTE *)bm.bmBits + (size_t)y * (size_t)bm.bmWidthBytes;

        for (x = 0; x < bm.bmWidth; x++) {
            int b;
            int g;
            int r;
            int luma;

            if (row[x * 4 + 3] == 0) {
                continue;
            }
            b = row[x * 4];
            g = row[x * 4 + 1];
            r = row[x * 4 + 2];
            luma = (54 * r + 183 * g + 19 * b) >> 8;
            if (luma < 0) {
                luma = 0;
            }
            if (luma > 255) {
                luma = 255;
            }
            histogram[luma]++;
            total++;
        }
    }
    return total;
}

static double zi_auto_percentile(double p, const int histogram[256], int total) {
    int target = (int)ceil((double)total * p);
    int seen = 0;
    int bin;

    if (target < 1) {
        target = 1;
    }
    for (bin = 0; bin < 256; bin++) {
        seen += histogram[bin];
        if (seen >= target) {
            return (double)bin / 255.0;
        }
    }
    return 1;
}

static double zi_auto_linear(double encoded) {
    double c = encoded;

    if (c < 0) {
        c = 0;
    }
    if (c > 1) {
        c = 1;
    }
    if (c <= 0.04045) {
        return c / 12.92;
    }
    return pow((c + 0.055) / 1.055, 2.4);
}

static void zi_auto_apply(ZiTone *tone, const int histogram[256], int total) {
    double black = zi_auto_percentile(0.005, histogram, total);
    double shadow = zi_auto_percentile(0.15, histogram, total);
    double mid = zi_auto_percentile(0.50, histogram, total);
    double white = zi_auto_percentile(0.995, histogram, total);
    double span = white - black;
    double exposure;
    double contrast;
    double highlights;
    double shadows;
    double whites;
    double blacks;
    double current;

    current = zi_auto_linear(mid);
    if (current < 0.002) {
        current = 0.002;
    }
    exposure = log2(zi_auto_linear(0.45) / current);
    if (span < 0.55) {
        contrast = (0.75 - span) * 0.8;
        if (contrast > 0.45) {
            contrast = 0.45;
        }
    } else if (span > 0.92) {
        contrast = (0.92 - span) * 1.5;
        if (contrast < -0.35) {
            contrast = -0.35;
        }
    } else {
        contrast = 0;
    }
    highlights = 0;
    if (white >= 0.985) {
        highlights = -(white - 0.96) * 10;
    }
    shadows = 0;
    if (shadow < 0.16 && black < 0.04) {
        shadows = (0.18 - shadow) * 2;
    }
    if (white < 0.90) {
        whites = (0.97 - white) * 2;
    } else if (white >= 0.995) {
        whites = -0.15;
    } else {
        whites = 0;
    }
    if (black > 0.06 && white > black + 0.4) {
        blacks = -(black - 0.02) * 4;
    } else if (black < 0.005 && shadow < 0.06) {
        blacks = 0.12;
    } else {
        blacks = 0;
    }
    tone->exposure = zi_clamp(exposure, -1.5, 1.5);
    tone->contrast = zi_clamp(contrast, -0.45, 0.5);
    tone->highlights = zi_clamp(highlights, -0.8, 0);
    tone->shadows = zi_clamp(shadows, 0, 0.55);
    tone->whites = zi_clamp(whites, -0.2, 0.7);
    tone->blacks = zi_clamp(blacks, -0.75, 0.15);
}

int zi_tone_auto(HBITMAP base, ZiTone *tone, const ZiTouchDoc *touch) {
    HBITMAP owned = NULL;
    HBITMAP measured;
    int histogram[256];
    int total;

    if (!base || !tone) {
        return 0;
    }
    if (!zi_auto_base_ready(tone) || zi_touch_doc_visible(touch)) {
        ZiTone shaped = *tone;

        zi_auto_clear_grade(&shaped);
        owned = zi_tone_render(base, &shaped, touch, NULL, NULL, NULL);
        if (!owned) {
            return 0;
        }
        measured = owned;
    } else {
        measured = base;
    }
    total = zi_auto_histogram(measured, histogram);
    if (owned) {
        DeleteObject(owned);
    }
    if (total < 32) {
        return 0;
    }
    zi_auto_apply(tone, histogram, total);
    return 1;
}

static void zi_sort_doubles(double *values, int count) {
    int i;

    for (i = 1; i < count; i++) {
        double key = values[i];
        int j = i - 1;

        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }
}

static double zi_middle(double *values, int count) {
    int mid;

    zi_sort_doubles(values, count);
    mid = count / 2;
    if (count % 2 == 1) {
        return values[mid];
    }
    return (values[mid - 1] + values[mid]) * 0.5;
}

int zi_tone_sample_window(HBITMAP dib, int x, int y, int side, ZiWindowSample *sample) {
    BITMAP bm;
    int half;
    int left;
    int top;
    int right;
    int bottom;
    int px;
    int py;
    int count = 0;
    double reds[32 * 32];
    double greens[32 * 32];
    double blues[32 * 32];
    double lab_a[32 * 32];
    double lab_b[32 * 32];
    double sum_r = 0;
    double sum_g = 0;
    double sum_b = 0;
    double mean_a;
    double mean_b;
    double sum = 0;
    int i;

    if (!sample) {
        return 0;
    }
    memset(sample, 0, sizeof *sample);
    if (!dib || side < 1 || GetObject(dib, sizeof bm, &bm) != (int)sizeof bm || !bm.bmBits) {
        return 0;
    }
    if (bm.bmBitsPixel != 32 || bm.bmWidth < 1 || bm.bmHeight < 1) {
        return 0;
    }
    if (side > 32) {
        side = 32;
    }
    half = side / 2;
    left = x - half;
    top = y - half;
    right = left + side;
    bottom = top + side;
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > bm.bmWidth) {
        right = bm.bmWidth;
    }
    if (bottom > bm.bmHeight) {
        bottom = bm.bmHeight;
    }
    if (right - left < 1 || bottom - top < 1) {
        left = 0;
        top = 0;
        right = 1;
        bottom = 1;
    }
    for (py = top; py < bottom; py++) {
        const BYTE *row = (const BYTE *)bm.bmBits + (size_t)py * (size_t)bm.bmWidthBytes;

        for (px = left; px < right && count < 32 * 32; px++) {
            double r = zi_srgb_to_linear(row[px * 4 + 2] / 255.0);
            double g = zi_srgb_to_linear(row[px * 4 + 1] / 255.0);
            double b = zi_srgb_to_linear(row[px * 4] / 255.0);
            double L;
            double a;
            double bb;

            reds[count] = r;
            greens[count] = g;
            blues[count] = b;
            zi_linear_lab(r, g, b, &L, &a, &bb);
            lab_a[count] = a;
            lab_b[count] = bb;
            sum_r += r;
            sum_g += g;
            sum_b += b;
            count++;
        }
    }
    if (count < 1) {
        return 0;
    }
    sample->count = count;
    sample->mean_r = sum_r / count;
    sample->mean_g = sum_g / count;
    sample->mean_b = sum_b / count;
    sample->median_r = zi_middle(reds, count);
    sample->median_g = zi_middle(greens, count);
    sample->median_b = zi_middle(blues, count);
    if (count <= 1) {
        sample->chroma_rms = 0;
        return 1;
    }
    mean_a = 0;
    mean_b = 0;
    for (i = 0; i < count; i++) {
        mean_a += lab_a[i];
        mean_b += lab_b[i];
    }
    mean_a /= count;
    mean_b /= count;
    for (i = 0; i < count; i++) {
        double da = lab_a[i] - mean_a;
        double db = lab_b[i] - mean_b;

        sum += da * da + db * db;
    }
    sample->chroma_rms = sqrt(sum / count);
    return 1;
}

void zi_tone_neutralize(double r, double g, double b, double *temperature, double *tint) {
    double temps[18];
    double tints[16];
    int tint_count = 0;
    int i;
    int j;
    double best_t = 6500;
    double best_g = 0;
    double best = 1e9;
    double span_t;
    double span_g;
    double step;

    if (!temperature || !tint) {
        return;
    }
    for (i = 0; i < 18; i++) {
        double t = (double)i / 17.0;

        temps[i] = exp(log(2000.0) + t * (log(50000.0) - log(2000.0)));
    }
    for (step = -150; step <= 150.0001; step += 25) {
        tints[tint_count++] = step;
    }
    for (i = 0; i < 18; i++) {
        for (j = 0; j < tint_count; j++) {
            double gr;
            double gg;
            double gb;
            double rr;
            double rg;
            double rb;
            double score;

            zi_temp_gains(temps[i], tints[j], &gr, &gg, &gb);
            rr = r * gr;
            rg = g * gg;
            rb = b * gb;
            if (!zi_finite(rr) || !zi_finite(rg) || !zi_finite(rb)) {
                score = 1e9;
            } else if (rr < 0 || rg < 0 || rb < 0) {
                score = 1e6;
            } else {
                score = zi_chroma(rr, rg, rb);
            }
            if (score < best) {
                best = score;
                best_t = temps[i];
                best_g = tints[j];
            }
        }
    }
    span_t = best_t * 0.18;
    span_g = 30;
    for (i = 0; i < 5 && best > 0.35; i++) {
        double low_t = best_t - span_t;
        double high_t = best_t + span_t;
        double low_g = best_g - span_g;
        double high_g = best_g + span_g;

        if (low_t < 2000) {
            low_t = 2000;
        }
        if (high_t > 50000) {
            high_t = 50000;
        }
        if (low_g < -150) {
            low_g = -150;
        }
        if (high_g > 150) {
            high_g = 150;
        }
        for (j = 0; j < 7; j++) {
            int k;

            for (k = 0; k < 7; k++) {
                double temp = low_t + (high_t - low_t) * (double)j / 6.0;
                double green = low_g + (high_g - low_g) * (double)k / 6.0;
                double gr;
                double gg;
                double gb;
                double rr;
                double rg;
                double rb;
                double score;

                zi_temp_gains(temp, green, &gr, &gg, &gb);
                rr = r * gr;
                rg = g * gg;
                rb = b * gb;
                if (!zi_finite(rr) || !zi_finite(rg) || !zi_finite(rb)) {
                    score = 1e9;
                } else if (rr < 0 || rg < 0 || rb < 0) {
                    score = 1e6;
                } else {
                    score = zi_chroma(rr, rg, rb);
                }
                if (score < best) {
                    best = score;
                    best_t = temp;
                    best_g = green;
                }
            }
        }
        span_t *= 0.45;
        span_g *= 0.45;
    }
    *temperature = best_t;
    *tint = best_g;
}

void zi_skin_deltas(double r, double g, double b, int preset, double *delta_a, double *delta_b) {
    double L;
    double a;
    double lab_b;

    if (!delta_a || !delta_b) {
        return;
    }
    if (preset < 0 || preset >= ZI_SKIN_COUNT) {
        preset = 0;
    }
    zi_linear_lab(r, g, b, &L, &a, &lab_b);
    *delta_a = k_skin[preset].a - a;
    *delta_b = k_skin[preset].b - lab_b;
}
