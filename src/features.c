#include "features.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double zi_clamp_d(double value, double low, double high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static double zi_srgb_to_linear(double c) {
    c = zi_clamp_d(c, 0, 1);
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

void zi_curve_linear(ZiCurve *curve) {
    if (!curve) {
        return;
    }
    curve->count = 2;
    curve->points[0].x = 0;
    curve->points[0].y = 0;
    curve->points[1].x = 1;
    curve->points[1].y = 1;
}

void zi_curves_identity(ZiCurves *curves) {
    if (!curves) {
        return;
    }
    zi_curve_linear(&curves->master);
    zi_curve_linear(&curves->red);
    zi_curve_linear(&curves->green);
    zi_curve_linear(&curves->blue);
}

int zi_curve_is_linear(const ZiCurve *curve) {
    int i;

    if (!curve || curve->count < 2) {
        return 1;
    }
    for (i = 0; i < curve->count; i++) {
        double d = curve->points[i].y - curve->points[i].x;

        if (d < 0) {
            d = -d;
        }
        if (d > 0.0001) {
            return 0;
        }
    }
    return 1;
}

int zi_curves_linear(const ZiCurves *curves) {
    if (!curves) {
        return 1;
    }
    return zi_curve_is_linear(&curves->master) && zi_curve_is_linear(&curves->red) &&
           zi_curve_is_linear(&curves->green) && zi_curve_is_linear(&curves->blue);
}

static int zi_curve_near(const ZiCurve *a, const ZiCurve *b) {
    int i;

    if (a->count != b->count) {
        return 0;
    }
    for (i = 0; i < a->count; i++) {
        double dx = a->points[i].x - b->points[i].x;
        double dy = a->points[i].y - b->points[i].y;

        if (dx < 0) {
            dx = -dx;
        }
        if (dy < 0) {
            dy = -dy;
        }
        if (dx > 0.0005 || dy > 0.0005) {
            return 0;
        }
    }
    return 1;
}

int zi_curves_same(const ZiCurves *a, const ZiCurves *b) {
    if (!a || !b) {
        return a == b;
    }
    return zi_curve_near(&a->master, &b->master) && zi_curve_near(&a->red, &b->red) &&
           zi_curve_near(&a->green, &b->green) && zi_curve_near(&a->blue, &b->blue);
}

void zi_curve_sanitize(ZiCurve *curve) {
    ZiCurvePoint kept[ZI_CURVE_MAX + 4];
    int n = 0;
    int i;
    int raw;

    if (!curve) {
        return;
    }
    raw = curve->count;
    if (raw < 0) {
        raw = 0;
    }
    if (raw > ZI_CURVE_MAX) {
        raw = ZI_CURVE_MAX;
    }
    for (i = 0; i < raw; i++) {
        int j = i;

        while (j > 0 && curve->points[j].x < curve->points[j - 1].x) {
            ZiCurvePoint swap = curve->points[j];

            curve->points[j] = curve->points[j - 1];
            curve->points[j - 1] = swap;
            j--;
        }
    }
    for (i = 0; i < raw && n < ZI_CURVE_MAX + 4; i++) {
        ZiCurvePoint point;

        point.x = zi_clamp_d(curve->points[i].x, 0, 1);
        point.y = zi_clamp_d(curve->points[i].y, 0, 1);
        if (n > 0 && point.x - kept[n - 1].x < 0.01) {
            continue;
        }
        kept[n++] = point;
    }
    if (n == 0 || kept[0].x != 0) {
        ZiCurvePoint end;

        end.x = 0;
        end.y = n > 0 ? kept[0].y : 0;
        if (n < ZI_CURVE_MAX + 4) {
            memmove(&kept[1], &kept[0], (size_t)n * sizeof kept[0]);
            kept[0] = end;
            n++;
        }
    } else {
        kept[0].x = 0;
    }
    if (n == 0 || kept[n - 1].x != 1) {
        if (n < ZI_CURVE_MAX + 4) {
            kept[n].x = 1;
            kept[n].y = n > 0 ? kept[n - 1].y : 1;
            n++;
        }
    } else {
        kept[n - 1].x = 1;
    }
    while (n > ZI_CURVE_MAX) {
        int drop = 1;
        double closest = 1e300;

        for (i = 1; i < n - 1; i++) {
            double spacing = kept[i].x - kept[i - 1].x;

            if (spacing < closest) {
                closest = spacing;
                drop = i;
            }
        }
        memmove(&kept[drop], &kept[drop + 1], (size_t)(n - drop - 1) * sizeof kept[0]);
        n--;
    }
    if (n < 2) {
        zi_curve_linear(curve);
        return;
    }
    curve->count = n;
    for (i = 0; i < n; i++) {
        curve->points[i] = kept[i];
    }
}

static void zi_curve_tangents(const ZiCurve *curve, double *slope) {
    double delta[ZI_CURVE_MAX];
    int count = curve->count;
    int i;

    if (count < 2) {
        return;
    }
    for (i = 0; i < count - 1; i++) {
        double span = curve->points[i + 1].x - curve->points[i].x;

        delta[i] = span > 1e-8 ? (curve->points[i + 1].y - curve->points[i].y) / span : 0;
    }
    slope[0] = delta[0];
    slope[count - 1] = delta[count - 2];
    if (count > 2) {
        for (i = 1; i < count - 1; i++) {
            if (delta[i - 1] * delta[i] <= 0) {
                slope[i] = 0;
            } else {
                slope[i] = (delta[i - 1] + delta[i]) / 2.0;
            }
        }
    }
    for (i = 0; i < count - 1; i++) {
        double a;
        double b;
        double magnitude;

        if (fabs(delta[i]) <= 1e-8) {
            slope[i] = 0;
            slope[i + 1] = 0;
            continue;
        }
        a = slope[i] / delta[i];
        b = slope[i + 1] / delta[i];
        magnitude = a * a + b * b;
        if (magnitude > 9) {
            double scale = 3.0 / sqrt(magnitude);

            slope[i] = scale * a * delta[i];
            slope[i + 1] = scale * b * delta[i];
        }
    }
}

double zi_curve_eval(const ZiCurve *curve, double x) {
    double slope[ZI_CURVE_MAX];
    int i;
    double span;
    double t;
    double t2;
    double t3;
    const ZiCurvePoint *left;
    const ZiCurvePoint *right;

    if (!curve || curve->count < 2) {
        return x;
    }
    zi_curve_tangents(curve, slope);
    if (x <= curve->points[0].x) {
        return curve->points[0].y + slope[0] * (x - curve->points[0].x);
    }
    if (x >= curve->points[curve->count - 1].x) {
        int last = curve->count - 1;

        return curve->points[last].y + slope[last] * (x - curve->points[last].x);
    }
    i = 0;
    for (; i < curve->count - 1; i++) {
        if (x >= curve->points[i].x && x <= curve->points[i + 1].x) {
            break;
        }
    }
    if (i >= curve->count - 1) {
        i = curve->count - 2;
    }
    left = &curve->points[i];
    right = &curve->points[i + 1];
    span = right->x - left->x;
    if (span <= 1e-8) {
        return left->y;
    }
    t = (x - left->x) / span;
    t2 = t * t;
    t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * left->y +
           (t3 - 2 * t2 + t) * span * slope[i] +
           (-2 * t3 + 3 * t2) * right->y +
           (t3 - t2) * span * slope[i + 1];
}

static void zi_curve_set(ZiCurve *curve, const double *xy, int pairs) {
    int i;

    curve->count = pairs;
    for (i = 0; i < pairs && i < ZI_CURVE_MAX; i++) {
        curve->points[i].x = xy[i * 2];
        curve->points[i].y = xy[i * 2 + 1];
    }
    zi_curve_sanitize(curve);
}

void zi_curve_preset(ZiCurves *curves, int preset) {
    static const double k_matte[] = { 0, 0.09, 0.22, 0.26, 0.50, 0.50, 0.78, 0.72, 1, 0.88 };
    static const double k_moody[] = { 0, 0.04, 0.20, 0.14, 0.48, 0.46, 0.76, 0.80, 1, 0.93 };
    static const double k_punch[] = { 0, 0, 0.22, 0.12, 0.50, 0.50, 0.78, 0.88, 1, 1 };
    static const double k_lofi[] = { 0, 0.14, 0.28, 0.32, 0.55, 0.55, 0.80, 0.74, 1, 0.84 };

    if (!curves) {
        return;
    }
    zi_curves_identity(curves);
    if (preset == 1) {
        zi_curve_set(&curves->master, k_matte, 5);
    } else if (preset == 2) {
        zi_curve_set(&curves->master, k_moody, 5);
    } else if (preset == 3) {
        zi_curve_set(&curves->master, k_punch, 5);
    } else if (preset == 4) {
        zi_curve_set(&curves->master, k_lofi, 5);
    }
}

int zi_curve_matches_preset(const ZiCurves *curves, int preset) {
    ZiCurves sample;

    if (!curves || !zi_curve_is_linear(&curves->red) || !zi_curve_is_linear(&curves->green) ||
        !zi_curve_is_linear(&curves->blue)) {
        return 0;
    }
    zi_curve_preset(&sample, preset);
    return zi_curve_near(&curves->master, &sample.master);
}

void zi_curve_move(ZiCurve *curve, int index, double x, double y) {
    if (!curve || index < 0 || index >= curve->count) {
        return;
    }
    y = zi_clamp_d(y, 0, 1);
    if (index == 0) {
        curve->points[0].x = 0;
        curve->points[0].y = y;
    } else if (index == curve->count - 1) {
        curve->points[index].x = 1;
        curve->points[index].y = y;
    } else {
        double left = curve->points[index - 1].x + 0.02;
        double right = curve->points[index + 1].x - 0.02;

        curve->points[index].x = zi_clamp_d(x, left, right);
        curve->points[index].y = y;
    }
    zi_curve_sanitize(curve);
}

int zi_curve_insert(ZiCurve *curve, double x) {
    int i;
    double y;

    if (!curve || curve->count >= ZI_CURVE_MAX) {
        return -1;
    }
    x = zi_clamp_d(x, 0.02, 1 - 0.02);
    for (i = 0; i < curve->count; i++) {
        double d = curve->points[i].x - x;

        if (d < 0) {
            d = -d;
        }
        if (d < 0.02) {
            return -1;
        }
    }
    y = zi_clamp_d(zi_curve_eval(curve, x), 0, 1);
    curve->points[curve->count].x = x;
    curve->points[curve->count].y = y;
    curve->count++;
    zi_curve_sanitize(curve);
    for (i = 0; i < curve->count; i++) {
        double d = curve->points[i].x - x;

        if (d < 0) {
            d = -d;
        }
        if (d < 0.0001) {
            return i;
        }
    }
    return -1;
}

void zi_curve_remove(ZiCurve *curve, int index) {
    if (!curve || index <= 0 || index >= curve->count - 1) {
        return;
    }
    memmove(&curve->points[index], &curve->points[index + 1],
            (size_t)(curve->count - index - 1) * sizeof curve->points[0]);
    curve->count--;
    zi_curve_sanitize(curve);
}

void zi_curve_reset_end(ZiCurve *curve, int index) {
    if (!curve) {
        return;
    }
    if (index == 0) {
        zi_curve_move(curve, 0, 0, 0);
    } else if (index == curve->count - 1) {
        zi_curve_move(curve, index, 1, 1);
    }
}

void zi_curve_map_linear(const ZiCurves *curves, double *r, double *g, double *b) {
    double er;
    double eg;
    double eb;

    if (!curves || !r || !g || !b || zi_curves_linear(curves)) {
        return;
    }
    er = zi_linear_to_srgb(*r);
    eg = zi_linear_to_srgb(*g);
    eb = zi_linear_to_srgb(*b);
    er = zi_curve_eval(&curves->red, zi_curve_eval(&curves->master, er));
    eg = zi_curve_eval(&curves->green, zi_curve_eval(&curves->master, eg));
    eb = zi_curve_eval(&curves->blue, zi_curve_eval(&curves->master, eb));
    *r = zi_srgb_to_linear(er);
    *g = zi_srgb_to_linear(eg);
    *b = zi_srgb_to_linear(eb);
}

double zi_vignette_coverage(double x, double y, double frame_w, double frame_h,
                            double midpoint, double roundness, double feather,
                            double center_x, double center_y) {
    double mid;
    double round;
    double soft;
    double half_w;
    double half_h;
    double shift_x;
    double shift_y;
    double cx;
    double cy;
    double dx;
    double dy;
    double ellipse;
    double rect;
    double short_half;
    double circle;
    double dist;
    double start;
    double band;
    double t;

    if (frame_w <= 1 || frame_h <= 1) {
        return 0;
    }
    mid = zi_clamp_d(midpoint, 0, 1);
    round = zi_clamp_d(roundness, -1, 1);
    soft = zi_clamp_d(feather, 0, 1);
    half_w = frame_w / 2.0;
    half_h = frame_h / 2.0;
    shift_x = zi_clamp_d(center_x, -1, 1);
    shift_y = zi_clamp_d(center_y, -1, 1);
    cx = frame_w / 2.0 + shift_x * half_w;
    cy = frame_h / 2.0 + shift_y * half_h;
    dx = (x - cx) / half_w;
    dy = (y - cy) / half_h;
    ellipse = sqrt(dx * dx + dy * dy);
    rect = fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy);
    short_half = half_w < half_h ? half_w : half_h;
    circle = sqrt(((x - cx) / short_half) * ((x - cx) / short_half) +
                  ((y - cy) / short_half) * ((y - cy) / short_half));
    dist = round >= 0 ? ellipse + (circle - ellipse) * round : ellipse + (rect - ellipse) * (-round);
    start = mid * 1.15;
    band = soft * 1.2;
    if (band < 0.02) {
        band = 0.02;
    }
    t = zi_clamp_d((dist - start) / band, 0, 1);
    return t * t * (3.0 - 2.0 * t);
}

double zi_vignette_shade(double sample, double amount, double coverage) {
    if (amount < 0) {
        double out = sample * (1.0 + amount * coverage);

        return out < 0 ? 0 : out;
    }
    return sample + (1.0 - (sample < 1.0 ? sample : 1.0)) * amount * coverage;
}

double zi_vignette_coverage_px(int x, int y, int width, int height,
                               double midpoint, double roundness, double feather,
                               double center_x, double center_y) {
    double y_up;

    if (width <= 1 || height <= 1) {
        return 0;
    }
    y_up = (double)(height - 1 - y) + 0.5;
    return zi_vignette_coverage((double)x + 0.5, y_up, (double)width, (double)height,
                                midpoint, roundness, feather, center_x, center_y);
}

void zi_horizon_measure(double x0, double y0, double x1, double y1,
                        double pixel_width, double pixel_height,
                        int *quarter_turns, double *degrees) {
    double width = pixel_width > 0 ? pixel_width : 1;
    double height = pixel_height > 0 ? pixel_height : 1;
    double dx = (x1 - x0) * width;
    double dy = (y1 - y0) * height;
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
    tilt = atan2(dy, dx) * 180.0 / M_PI;
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
    needed = zi_clamp_d(needed, -45.0, 45.0);
    if (quarter_turns) {
        *quarter_turns = turns;
    }
    if (degrees) {
        *degrees = needed;
    }
}

int zi_wrap_turns(int turns) {
    int remainder = turns % 4;

    if (remainder < 0) {
        remainder += 4;
    }
    return remainder;
}

void zi_flip_swap_if_odd(int *flip_h, int *flip_v, int delta) {
    int steps;

    if (!flip_h || !flip_v) {
        return;
    }
    steps = zi_wrap_turns(delta);
    if (steps % 2 == 1) {
        int swap = *flip_h;

        *flip_h = *flip_v;
        *flip_v = swap;
    }
}

static void zi_rect_clamp(ZiNormRect *rect) {
    double max_x;
    double max_y;

    if (rect->w < 0) {
        rect->x += rect->w;
        rect->w = -rect->w;
    }
    if (rect->h < 0) {
        rect->y += rect->h;
        rect->h = -rect->h;
    }
    if (rect->x < 0) {
        rect->w += rect->x;
        rect->x = 0;
    }
    if (rect->y < 0) {
        rect->h += rect->y;
        rect->y = 0;
    }
    max_x = rect->x + rect->w;
    max_y = rect->y + rect->h;
    if (max_x > 1) {
        rect->w -= max_x - 1;
    }
    if (max_y > 1) {
        rect->h -= max_y - 1;
    }
    if (rect->w < 0) {
        rect->w = 0;
    }
    if (rect->h < 0) {
        rect->h = 0;
    }
}

void zi_crop_rotate(ZiNormRect *rect, int quarter_turns) {
    int turns;
    double px[4];
    double py[4];
    int i;
    double min_x;
    double min_y;
    double max_x;
    double max_y;

    if (!rect) {
        return;
    }
    turns = zi_wrap_turns(quarter_turns);
    if (turns == 0) {
        return;
    }
    zi_rect_clamp(rect);
    px[0] = rect->x;
    py[0] = rect->y;
    px[1] = rect->x + rect->w;
    py[1] = rect->y;
    px[2] = rect->x + rect->w;
    py[2] = rect->y + rect->h;
    px[3] = rect->x;
    py[3] = rect->y + rect->h;
    for (i = 0; i < turns; i++) {
        int k;

        for (k = 0; k < 4; k++) {
            double nx = 1.0 - py[k];
            double ny = px[k];

            px[k] = nx;
            py[k] = ny;
        }
    }
    min_x = px[0];
    min_y = py[0];
    max_x = px[0];
    max_y = py[0];
    for (i = 1; i < 4; i++) {
        if (px[i] < min_x) {
            min_x = px[i];
        }
        if (py[i] < min_y) {
            min_y = py[i];
        }
        if (px[i] > max_x) {
            max_x = px[i];
        }
        if (py[i] > max_y) {
            max_y = py[i];
        }
    }
    rect->x = min_x;
    rect->y = min_y;
    rect->w = max_x - min_x;
    rect->h = max_y - min_y;
}

void zi_crop_mirror_h(ZiNormRect *rect) {
    if (!rect) {
        return;
    }
    zi_rect_clamp(rect);
    rect->x = 1.0 - rect->x - rect->w;
}

void zi_crop_mirror_v(ZiNormRect *rect) {
    if (!rect) {
        return;
    }
    zi_rect_clamp(rect);
    rect->y = 1.0 - rect->y - rect->h;
}

double zi_contrast_input(double slider) {
    if (slider >= 0) {
        return 1.0 + 3.0 * slider;
    }
    return 1.0 + 0.75 * slider;
}

void zi_auto_from_levels(const ZiAutoLevels *levels, ZiAutoGrade *grade) {
    double target;
    double current;
    double ev;
    double span;
    double opened;
    double closed;
    double proposed;
    double gained;
    double out;
    double highlights = 0;
    double shadows = 0;
    double whites;
    double blacks;

    if (!levels || !grade) {
        return;
    }
    target = zi_srgb_to_linear(0.45);
    current = zi_srgb_to_linear(levels->mid);
    if (current < 0.002) {
        current = 0.002;
    }
    ev = log(target / current) / log(2.0);
    if (ev > 0) {
        double ceiling = zi_srgb_to_linear(0.92);
        double bright = zi_srgb_to_linear(levels->highlight);
        double room;

        if (bright < 0.002) {
            bright = 0.002;
        }
        room = log(ceiling / bright) / log(2.0);
        if (room < 0) {
            room = 0;
        }
        if (ev > room) {
            ev = room;
        }
    }
    ev = zi_clamp_d(ev, -1.5, 3.0);
    span = levels->white - levels->black;
    opened = span < 0.55 ? zi_clamp_d((0.75 - span) * 0.8, 0, 0.45) : 0;
    closed = span > 0.92 ? zi_clamp_d((0.92 - span) * 1.5, -0.35, 0) : 0;
    proposed = opened != 0 ? opened : closed;
    if (proposed > 0) {
        gained = zi_clamp_d(zi_srgb_to_linear(levels->mid) * pow(2.0, ev), 0, 1);
        out = (gained - 0.5) * zi_contrast_input(proposed) + 0.5;
        if (out < gained - 0.0001) {
            proposed = 0;
        }
    }
    if (levels->white >= 0.985) {
        highlights = -(levels->white - 0.96) * 10.0;
    }
    if (levels->shadow < 0.16 && levels->black < 0.04) {
        shadows = (0.18 - levels->shadow) * 2.0;
    }
    if (levels->white < 0.90) {
        whites = (0.97 - levels->white) * 2.0;
    } else if (levels->white >= 0.995) {
        whites = -0.15;
    } else {
        whites = 0;
    }
    if (levels->mid < 0.45) {
        blacks = (levels->black < 0.005 && levels->shadow < 0.06) ? 0.12 : 0;
    } else if (levels->black > 0.06 && levels->white > levels->black + 0.4) {
        blacks = -(levels->black - 0.02) * 4.0;
    } else if (levels->black < 0.005 && levels->shadow < 0.06) {
        blacks = 0.12;
    } else {
        blacks = 0;
    }
    grade->exposure = ev;
    grade->contrast = zi_clamp_d(proposed, -0.45, 0.5);
    grade->highlights = zi_clamp_d(highlights, -0.8, 0);
    grade->shadows = zi_clamp_d(shadows, 0, 0.55);
    grade->whites = zi_clamp_d(whites, -0.2, 0.7);
    grade->blacks = zi_clamp_d(blacks, -0.75, 0.15);
}

int zi_name_match(const wchar_t *name, const wchar_t *filter) {
    const wchar_t *n;
    const wchar_t *f;

    if (!filter || !filter[0]) {
        return 1;
    }
    if (!name) {
        return 0;
    }
    for (n = name; *n; n++) {
        const wchar_t *a = n;

        for (f = filter; *f; f++, a++) {
            if (!*a || towlower(*a) != towlower(*f)) {
                break;
            }
        }
        if (!*f) {
            return 1;
        }
    }
    return 0;
}

void zi_page_slice(int page, int still_total, int filtered, int match_total, ZiPageSlice *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (page < 0) {
        page = 0;
    }
    if (filtered) {
        int begin = page * ZI_PAGE_SIZE;

        if (match_total < 0) {
            match_total = 0;
        }
        if (begin > match_total) {
            begin = match_total;
        }
        out->filtered_begin = begin;
        out->filtered_count = match_total - begin;
        if (out->filtered_count > ZI_PAGE_SIZE) {
            out->filtered_count = ZI_PAGE_SIZE;
        }
        return;
    }
    if (still_total < 0) {
        still_total = 0;
    }
    out->still_begin = page * ZI_PAGE_SIZE;
    if (out->still_begin > still_total) {
        out->still_begin = still_total;
    }
    out->still_count = still_total - out->still_begin;
    if (out->still_count > ZI_PAGE_SIZE) {
        out->still_count = ZI_PAGE_SIZE;
    }
    out->include_videos = page == 0;
}

static void zi_stem(const wchar_t *name, wchar_t *dest, size_t cap) {
    const wchar_t *base = name ? name : L"";
    const wchar_t *slash = wcsrchr(base, L'\\');
    const wchar_t *slash2 = wcsrchr(base, L'/');
    const wchar_t *dot;
    size_t n;

    if (slash2 && (!slash || slash2 > slash)) {
        slash = slash2;
    }
    if (slash) {
        base = slash + 1;
    }
    dot = wcsrchr(base, L'.');
    n = dot && dot != base ? (size_t)(dot - base) : wcslen(base);
    if (n >= cap) {
        n = cap - 1;
    }
    if (n > 0) {
        memcpy(dest, base, n * sizeof(wchar_t));
    }
    dest[n] = L'\0';
}

int zi_merge_filename(wchar_t *dest, size_t cap, const wchar_t *left, const wchar_t *right, int attempt) {
    wchar_t a[260];
    wchar_t b[260];
    int suffix;

    if (!dest || cap < 8 || attempt < 0 || attempt > 998) {
        return 0;
    }
    zi_stem(left, a, 260);
    zi_stem(right, b, 260);
    if (!a[0] || !b[0]) {
        return 0;
    }
    if (attempt == 0) {
        return swprintf(dest, cap, L"%s-%s.tiff", a, b) > 0;
    }
    suffix = attempt + 1;
    return swprintf(dest, cap, L"%s-%s-%d.tiff", a, b, suffix) > 0;
}

int zi_is_pupil_red(double red, double green, double blue) {
    double other = green > blue ? green : blue;

    if (red <= 0.20 || other >= 0.36) {
        return 0;
    }
    if (red <= other * 1.7) {
        return 0;
    }
    return red - other > 0.12;
}

int zi_is_pupil_fringe(double red, double green, double blue) {
    double other = green > blue ? green : blue;

    if (red <= 0.15 || other >= 0.32) {
        return 0;
    }
    if (red <= other * 1.5) {
        return 0;
    }
    return red - other > 0.08;
}

int zi_redeye_reject(int count, int box_w, int box_h) {
    int lo;
    int hi;

    if (count < 8) {
        return 1;
    }
    lo = box_w < box_h ? box_w : box_h;
    hi = box_w > box_h ? box_w : box_h;
    if (lo < 1) {
        return 1;
    }
    return hi > lo * 3;
}

int zi_scratch_spacing(int radius) {
    int spacing = radius / 4;

    if (spacing < 1) {
        spacing = 1;
    }
    return spacing;
}

int zi_chosen_page(const int *pages, int count, int index) {
    if (!pages || index < 0 || index >= count) {
        return 0;
    }
    return pages[index];
}
