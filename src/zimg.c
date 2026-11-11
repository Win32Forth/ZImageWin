/* fileRef .zimg. Schema 1, app version 1.0.0, the same envelope as ZImgRaw.swift.
   History JSON uses the Mac HistoryLog names. Dust and scratch repairs are addInpaint
   commands. The original is not embedded. */
#include "zimg.h"

#include "blake3.h"
#include "tone.h"
#include "touchup.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZI_LOG_CAP 160
#define ZI_JSON_CAP (32u * 1024u * 1024u)

typedef struct ZiBuf {
    char *data;
    size_t len;
    size_t cap;
    int err;
} ZiBuf;

typedef struct ZiJs {
    const char *p;
    const char *end;
    int err;
} ZiJs;

typedef struct ZiSerialSlot {
    wchar_t path[MAX_PATH];
    long serial;
    int used;
} ZiSerialSlot;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static ZiSerialSlot g_serials[64];

static BOOL CALLBACK zi_lock_init(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void zi_lock(void) {
    InitOnceExecuteOnce(&g_once, zi_lock_init, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void zi_unlock(void) {
    LeaveCriticalSection(&g_lock);
}

static int zi_claim(const wchar_t *path, long serial) {
    int i;
    int slot = -1;

    if (!path || !path[0] || serial <= 0) {
        return 1;
    }
    for (i = 0; i < 64; i++) {
        if (!g_serials[i].used) {
            if (slot < 0) {
                slot = i;
            }
            continue;
        }
        if (_wcsicmp(g_serials[i].path, path) == 0) {
            if (serial < g_serials[i].serial) {
                return 0;
            }
            g_serials[i].serial = serial;
            return 1;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    g_serials[slot].used = 1;
    wcscpy_s(g_serials[slot].path, MAX_PATH, path);
    g_serials[slot].serial = serial;
    return 1;
}

static int zi_close(double a, double b) {
    double d = a - b;

    if (d < 0) {
        d = -d;
    }
    return d <= 0.0005;
}

static int zi_identity(const ZiTone *tone) {
    ZiTone bare;

    zi_tone_identity(&bare);
    return zi_tone_same(tone, &bare);
}

static void zi_clear_tone_fields(ZiTone *tone) {
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
    tone->has_skin = 0;
    tone->skin_preset = 0;
    tone->skin_delta_a = 0;
    tone->skin_delta_b = 0;
    tone->spectrum_low = 0;
    tone->spectrum_mid = 0.5;
    tone->spectrum_high = 1;
}

static void zi_clear_geo_fields(ZiTone *tone) {
    tone->straighten = 0;
    tone->vertical = 0;
    tone->horizontal = 0;
    tone->sharpness = 0;
    tone->k1 = 0;
    tone->k2 = 0;
    tone->fov = 0;
    tone->lens = 0;
    tone->has_corners = 0;
    memset(tone->corner_x, 0, sizeof tone->corner_x);
    memset(tone->corner_y, 0, sizeof tone->corner_y);
    tone->turns = 0;
}

static int zi_tone_part(const ZiTone *a, const ZiTone *b) {
    return zi_close(a->exposure, b->exposure) && zi_close(a->brightness, b->brightness) &&
           zi_close(a->contrast, b->contrast) && zi_close(a->highlights, b->highlights) &&
           zi_close(a->shadows, b->shadows) && zi_close(a->whites, b->whites) &&
           zi_close(a->blacks, b->blacks) && zi_close(a->texture, b->texture) &&
           zi_close(a->clarity, b->clarity) && zi_close(a->dehaze, b->dehaze) &&
           zi_close(a->vibrance, b->vibrance) && zi_close(a->saturation, b->saturation) &&
           zi_close(a->temperature, b->temperature) && zi_close(a->tint, b->tint) &&
           zi_close(a->spectrum_low, b->spectrum_low) && zi_close(a->spectrum_mid, b->spectrum_mid) &&
           zi_close(a->spectrum_high, b->spectrum_high) && a->has_skin == b->has_skin &&
           (!a->has_skin || (a->skin_preset == b->skin_preset && zi_close(a->skin_delta_a, b->skin_delta_a) &&
                             zi_close(a->skin_delta_b, b->skin_delta_b)));
}

static int zi_geo_part(const ZiTone *a, const ZiTone *b) {
    int i;

    if (!zi_close(a->straighten, b->straighten) || !zi_close(a->vertical, b->vertical) ||
        !zi_close(a->horizontal, b->horizontal) || !zi_close(a->sharpness, b->sharpness) ||
        !zi_close(a->k1, b->k1) || !zi_close(a->k2, b->k2) || !zi_close(a->fov, b->fov) ||
        a->lens != b->lens || a->has_corners != b->has_corners || (a->turns & 3) != (b->turns & 3)) {
        return 0;
    }
    if (!a->has_corners) {
        return 1;
    }
    for (i = 0; i < 4; i++) {
        if (!zi_close(a->corner_x[i], b->corner_x[i]) || !zi_close(a->corner_y[i], b->corner_y[i])) {
            return 0;
        }
    }
    return 1;
}

static int zi_crop_part(const ZiTone *a, const ZiTone *b) {
    if (a->has_crop != b->has_crop) {
        return 0;
    }
    if (!a->has_crop) {
        return 1;
    }
    return zi_close(a->crop_x, b->crop_x) && zi_close(a->crop_y, b->crop_y) &&
           zi_close(a->crop_w, b->crop_w) && zi_close(a->crop_h, b->crop_h);
}

static int buf_add(ZiBuf *buf, const void *bytes, size_t n) {
    if (!buf || buf->err) {
        return 0;
    }
    if (buf->len + n + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 256;
        char *next;

        while (cap < buf->len + n + 1) {
            if (cap > ZI_JSON_CAP) {
                buf->err = 1;
                return 0;
            }
            cap *= 2;
        }
        next = (char *)realloc(buf->data, cap);
        if (!next) {
            buf->err = 1;
            return 0;
        }
        buf->data = next;
        buf->cap = cap;
    }
    if (n && bytes) {
        memcpy(buf->data + buf->len, bytes, n);
    }
    buf->len += n;
    buf->data[buf->len] = '\0';
    return 1;
}

static void buf_raw(ZiBuf *buf, const char *text) {
    if (text) {
        buf_add(buf, text, strlen(text));
    }
}

static void buf_num(ZiBuf *buf, double value) {
    char text[64];

    if (value != value || value > 1.0e300 || value < -1.0e300) {
        value = 0;
    }
    sprintf_s(text, sizeof text, "%.17g", value);
    buf_raw(buf, text);
}

static void buf_json(ZiBuf *buf, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");

    buf_raw(buf, "\"");
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[3];

            esc[0] = '\\';
            esc[1] = (char)*p;
            esc[2] = '\0';
            buf_raw(buf, esc);
        } else if (*p < 0x20) {
            char esc[8];

            sprintf_s(esc, sizeof esc, "\\u%04x", *p);
            buf_raw(buf, esc);
        } else {
            buf_add(buf, p, 1);
        }
    }
    buf_raw(buf, "\"");
}

static void buf_key(ZiBuf *buf, int *first, const char *key) {
    if (!*first) {
        buf_raw(buf, ",");
    }
    *first = 0;
    buf_raw(buf, "\"");
    buf_raw(buf, key);
    buf_raw(buf, "\":");
}

static int zi_wide_utf8(const wchar_t *text, char *dest, size_t cap) {
    if (!dest || cap == 0) {
        return 0;
    }
    dest[0] = '\0';
    if (!text || !text[0]) {
        return 1;
    }
    return WideCharToMultiByte(CP_UTF8, 0, text, -1, dest, (int)cap, NULL, NULL) > 0;
}

static void zi_utf8_wide(const char *text, wchar_t *dest, size_t cap) {
    if (!dest || cap == 0) {
        return;
    }
    dest[0] = L'\0';
    if (!text || !text[0]) {
        return;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, dest, (int)cap) <= 0) {
        dest[0] = L'\0';
    }
}

static const char *zi_uti(const wchar_t *path) {
    const wchar_t *dot = path ? wcsrchr(path, L'.') : NULL;

    if (!dot) {
        return "public.image";
    }
    if (_wcsicmp(dot, L".jpg") == 0 || _wcsicmp(dot, L".jpeg") == 0) {
        return "public.jpeg";
    }
    if (_wcsicmp(dot, L".png") == 0) {
        return "public.png";
    }
    if (_wcsicmp(dot, L".tif") == 0 || _wcsicmp(dot, L".tiff") == 0) {
        return "public.tiff";
    }
    if (_wcsicmp(dot, L".heic") == 0 || _wcsicmp(dot, L".heif") == 0) {
        return "public.heic";
    }
    if (_wcsicmp(dot, L".webp") == 0) {
        return "org.webmproject.webp";
    }
    if (_wcsicmp(dot, L".gif") == 0) {
        return "com.compuserve.gif";
    }
    if (_wcsicmp(dot, L".bmp") == 0) {
        return "com.microsoft.bmp";
    }
    if (_wcsicmp(dot, L".dng") == 0) {
        return "com.adobe.raw-image";
    }
    return "public.image";
}

static void buf_point(ZiBuf *buf, double x, double y) {
    buf_raw(buf, "{\"x\":");
    buf_num(buf, x);
    buf_raw(buf, ",\"y\":");
    buf_num(buf, y);
    buf_raw(buf, "}");
}

static void buf_tone_body(ZiBuf *buf, const ZiTone *tone) {
    int first = 1;

    buf_raw(buf, "{");
    buf_key(buf, &first, "blacks");
    buf_num(buf, tone->blacks);
    buf_key(buf, &first, "brightness");
    buf_num(buf, tone->brightness);
    buf_key(buf, &first, "clarity");
    buf_num(buf, tone->clarity);
    buf_key(buf, &first, "contrast");
    buf_num(buf, tone->contrast);
    buf_key(buf, &first, "dehaze");
    buf_num(buf, tone->dehaze);
    buf_key(buf, &first, "exposureEV");
    buf_num(buf, tone->exposure);
    buf_key(buf, &first, "highlights");
    buf_num(buf, tone->highlights);
    buf_key(buf, &first, "saturation");
    buf_num(buf, tone->saturation);
    buf_key(buf, &first, "shadows");
    buf_num(buf, tone->shadows);
    if (tone->has_skin) {
        double L = 0;
        double a = 0;
        double b = 0;
        int skin = 1;

        zi_skin_lab(tone->skin_preset, &L, &a, &b);
        buf_key(buf, &first, "skinGrade");
        buf_raw(buf, "{");
        buf_key(buf, &skin, "deltaA");
        buf_num(buf, tone->skin_delta_a);
        buf_key(buf, &skin, "deltaB");
        buf_num(buf, tone->skin_delta_b);
        buf_key(buf, &skin, "presetID");
        buf_json(buf, zi_skin_preset_id(tone->skin_preset));
        buf_key(buf, &skin, "sample");
        buf_raw(buf, "{\"L\":");
        buf_num(buf, L);
        buf_raw(buf, ",\"a\":");
        buf_num(buf, a - tone->skin_delta_a);
        buf_raw(buf, ",\"b\":");
        buf_num(buf, b - tone->skin_delta_b);
        buf_raw(buf, "}");
        buf_key(buf, &skin, "target");
        buf_raw(buf, "{\"L\":");
        buf_num(buf, L);
        buf_raw(buf, ",\"a\":");
        buf_num(buf, a);
        buf_raw(buf, ",\"b\":");
        buf_num(buf, b);
        buf_raw(buf, "}}");
    }
    buf_key(buf, &first, "spectrumHigh");
    buf_num(buf, tone->spectrum_high);
    buf_key(buf, &first, "spectrumLow");
    buf_num(buf, tone->spectrum_low);
    buf_key(buf, &first, "spectrumMid");
    buf_num(buf, tone->spectrum_mid);
    buf_key(buf, &first, "temperature");
    buf_num(buf, tone->temperature);
    buf_key(buf, &first, "texture");
    buf_num(buf, tone->texture);
    buf_key(buf, &first, "tint");
    buf_num(buf, tone->tint);
    buf_key(buf, &first, "vibrance");
    buf_num(buf, tone->vibrance);
    buf_key(buf, &first, "whites");
    buf_num(buf, tone->whites);
    buf_raw(buf, "}");
}

static void buf_lens(ZiBuf *buf, const ZiTone *tone) {
    int first = 1;

    buf_raw(buf, "{");
    if (!zi_close(tone->fov, 0)) {
        buf_key(buf, &first, "cylindricalFOV");
        buf_num(buf, tone->fov);
    }
    buf_key(buf, &first, "k1");
    buf_num(buf, tone->k1);
    buf_key(buf, &first, "k2");
    buf_num(buf, tone->k2);
    buf_key(buf, &first, "profileID");
    buf_json(buf, zi_lens_profile_id(tone->lens));
    buf_raw(buf, "}");
}

static void buf_geometry(ZiBuf *buf, const ZiTone *tone) {
    int first = 1;
    char turns[16];

    buf_raw(buf, "{");
    if (tone->has_corners) {
        int corner = 1;

        buf_key(buf, &first, "corners");
        buf_raw(buf, "{");
        buf_key(buf, &corner, "bottomLeft");
        buf_point(buf, tone->corner_x[3], tone->corner_y[3]);
        buf_key(buf, &corner, "bottomRight");
        buf_point(buf, tone->corner_x[2], tone->corner_y[2]);
        buf_key(buf, &corner, "topLeft");
        buf_point(buf, tone->corner_x[0], tone->corner_y[0]);
        buf_key(buf, &corner, "topRight");
        buf_point(buf, tone->corner_x[1], tone->corner_y[1]);
        buf_raw(buf, "}");
    }
    buf_key(buf, &first, "horizontalKeystone");
    buf_num(buf, tone->horizontal);
    buf_key(buf, &first, "lens");
    buf_lens(buf, tone);
    buf_key(buf, &first, "quarterTurns");
    sprintf_s(turns, sizeof turns, "%d", tone->turns & 3);
    buf_raw(buf, turns);
    buf_key(buf, &first, "sharpness");
    buf_num(buf, tone->sharpness);
    buf_key(buf, &first, "straightenDegrees");
    buf_num(buf, tone->straighten);
    buf_key(buf, &first, "verticalKeystone");
    buf_num(buf, tone->vertical);
    buf_raw(buf, "}");
}

static void buf_crop(ZiBuf *buf, const ZiTone *tone) {
    buf_raw(buf, "{\"height\":");
    buf_num(buf, tone->crop_h);
    buf_raw(buf, ",\"width\":");
    buf_num(buf, tone->crop_w);
    buf_raw(buf, ",\"x\":");
    buf_num(buf, tone->crop_x);
    buf_raw(buf, ",\"y\":");
    buf_num(buf, tone->crop_y);
    buf_raw(buf, "}");
}

static void buf_inpaint(ZiBuf *buf, const ZiTouchOp *op) {
    unsigned char *png = NULL;
    size_t png_len = 0;
    char *b64 = NULL;

    buf_raw(buf, "{\"featherPx\":");
    buf_num(buf, op->feather);
    buf_raw(buf, ",\"id\":");
    buf_json(buf, op->id);
    buf_raw(buf, ",\"maskPNG\":");
    if (op->mask && zi_touch_mask_png(op->mask, &png, &png_len)) {
        b64 = zi_touch_base64(png, png_len);
    }
    free(png);
    buf_json(buf, b64 ? b64 : "");
    free(b64);
    buf_raw(buf, ",\"modelID\":");
    buf_json(buf, op->model);
    buf_raw(buf, ",\"strength\":");
    buf_num(buf, op->strength);
    buf_raw(buf, "}");
}

static void buf_inpaints(ZiBuf *buf, const ZiTouchDoc *doc) {
    int i;
    int n = 0;

    if (doc) {
        n = doc->count;
    }
    if (n < 0) {
        n = 0;
    }
    if (n > ZI_TOUCH_CAP) {
        n = ZI_TOUCH_CAP;
    }
    buf_raw(buf, "[");
    for (i = 0; i < n; i++) {
        if (i) {
            buf_raw(buf, ",");
        }
        buf_inpaint(buf, &doc->ops[i]);
    }
    buf_raw(buf, "]");
}

static void buf_edit(ZiBuf *buf, const ZiTone *tone, const ZiTouchDoc *doc) {
    int first = 1;

    buf_raw(buf, "{");
    if (tone->has_crop) {
        buf_key(buf, &first, "crop");
        buf_crop(buf, tone);
    }
    buf_key(buf, &first, "geometry");
    buf_geometry(buf, tone);
    buf_key(buf, &first, "inpaints");
    buf_inpaints(buf, doc);
    buf_key(buf, &first, "tone");
    buf_tone_body(buf, tone);
    buf_raw(buf, "}");
}

static void buf_label(ZiBuf *buf, const wchar_t *label) {
    char utf8[256];
    const wchar_t *text = (label && label[0]) ? label : L"Edit";

    if (!zi_wide_utf8(text, utf8, sizeof utf8)) {
        strcpy_s(utf8, sizeof utf8, "Edit");
    }
    buf_json(buf, utf8);
}

static void buf_set_part(ZiBuf *buf, const char *name, const ZiTone *tone, const wchar_t *label, int geometry) {
    int first = 1;

    buf_raw(buf, "{\"");
    buf_raw(buf, name);
    buf_raw(buf, "\":{");
    buf_key(buf, &first, "_0");
    if (geometry) {
        buf_geometry(buf, tone);
    } else {
        buf_tone_body(buf, tone);
    }
    buf_key(buf, &first, "label");
    buf_label(buf, label);
    buf_raw(buf, "}}");
}

static void buf_command(ZiBuf *buf, const ZiTone *prev, const ZiTone *next, const wchar_t *label,
                        const ZiTouchDoc *prev_doc, const ZiTouchDoc *next_doc) {
    int tone = !zi_tone_part(prev, next);
    int geo = !zi_geo_part(prev, next);
    int crop = !zi_crop_part(prev, next);
    int same = zi_touch_doc_same(prev_doc, next_doc);
    int added = 0;
    int strength_index = -1;

    if (!same && !tone && !geo && !crop && zi_touch_one_added(prev_doc, next_doc) && next_doc && next_doc->count > 0) {
        buf_raw(buf, "{\"addInpaint\":");
        buf_inpaint(buf, &next_doc->ops[next_doc->count - 1]);
        buf_raw(buf, "}");
        return;
    }
    if (!same && !tone && !geo && !crop && zi_touch_one_strength(prev_doc, next_doc, &strength_index) &&
        next_doc && strength_index >= 0 && strength_index < next_doc->count) {
        const ZiTouchOp *op = &next_doc->ops[strength_index];

        buf_raw(buf, "{\"setInpaintStrength\":{\"id\":");
        buf_json(buf, op->id);
        buf_raw(buf, ",\"label\":");
        buf_label(buf, label);
        buf_raw(buf, ",\"strength\":");
        buf_num(buf, op->strength);
        buf_raw(buf, "}}");
        return;
    }
    added = same;
    if (added && tone && !geo && !crop) {
        buf_set_part(buf, "setTone", next, label, 0);
    } else if (added && geo && !tone && !crop) {
        buf_set_part(buf, "setGeometry", next, label, 1);
    } else if (added && crop && !tone && !geo && next->has_crop) {
        buf_raw(buf, "{\"setCrop\":");
        buf_crop(buf, next);
        buf_raw(buf, "}");
    } else if (added && tone && geo && !crop) {
        int first = 1;

        buf_raw(buf, "{\"pasteSettings\":{");
        buf_key(buf, &first, "geometry");
        buf_geometry(buf, next);
        buf_key(buf, &first, "tone");
        buf_tone_body(buf, next);
        buf_raw(buf, "}}");
    } else {
        buf_raw(buf, "{\"keyframe\":");
        buf_edit(buf, next, next_doc);
        buf_raw(buf, "}");
    }
}

static int zi_step_extra(const ZiHistStep *hist, const ZiTouchDoc *docs) {
    if (!hist) {
        return 0;
    }
    if (!zi_identity(&hist[0].tone)) {
        return 1;
    }
    return docs && zi_touch_doc_active(&docs[0]);
}

static int buf_history(ZiBuf *buf, const ZiHistStep *hist, const ZiTouchDoc *docs, int len, int pos) {
    int i;
    int extra = 0;
    char cursor[32];

    if (len < 1) {
        return 0;
    }
    if (pos < 0) {
        pos = 0;
    }
    if (pos >= len) {
        pos = len - 1;
    }
    buf_raw(buf, "{\"commands\":[");
    if (zi_step_extra(hist, docs)) {
        buf_raw(buf, "{\"keyframe\":");
        buf_edit(buf, &hist[0].tone, docs ? &docs[0] : NULL);
        buf_raw(buf, "}");
        extra = 1;
    }
    for (i = 1; i < len; i++) {
        if (extra || i > 1) {
            buf_raw(buf, ",");
        }
        buf_command(buf, &hist[i - 1].tone, &hist[i].tone, hist[i].label,
                    docs ? &docs[i - 1] : NULL, docs ? &docs[i] : NULL);
    }
    sprintf_s(cursor, sizeof cursor, "],\"cursor\":%d}", pos + extra);
    buf_raw(buf, cursor);
    return !buf->err;
}

static void js_ws(ZiJs *js) {
    while (js->p < js->end) {
        char c = *js->p;

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        js->p++;
    }
}

static int js_eat(ZiJs *js, char expect) {
    js_ws(js);
    if (js->p >= js->end || *js->p != expect) {
        js->err = 1;
        return 0;
    }
    js->p++;
    return 1;
}

static int js_hex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static unsigned js_hex4(ZiJs *js) {
    unsigned value = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int digit;

        if (js->p >= js->end) {
            js->err = 1;
            return 0;
        }
        digit = js_hex(*js->p++);
        if (digit < 0) {
            js->err = 1;
            return 0;
        }
        value = (value << 4) | (unsigned)digit;
    }
    return value;
}

static void js_utf8(char *dest, size_t cap, size_t *used, unsigned code) {
    unsigned char bytes[4];
    int n = 0;
    int i;

    if (code > 0x10FFFF) {
        code = 0xFFFD;
    }
    if (code < 0x80) {
        bytes[0] = (unsigned char)code;
        n = 1;
    } else if (code < 0x800) {
        bytes[0] = (unsigned char)(0xC0 | (code >> 6));
        bytes[1] = (unsigned char)(0x80 | (code & 0x3F));
        n = 2;
    } else if (code < 0x10000) {
        bytes[0] = (unsigned char)(0xE0 | (code >> 12));
        bytes[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (code & 0x3F));
        n = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0 | (code >> 18));
        bytes[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (code & 0x3F));
        n = 4;
    }
    for (i = 0; i < n; i++) {
        if (dest && *used + 1 < cap) {
            dest[*used] = (char)bytes[i];
        }
        (*used)++;
    }
}

static int js_text(ZiJs *js, char *dest, size_t cap) {
    size_t used = 0;

    js_ws(js);
    if (js->p >= js->end || *js->p != '"') {
        js->err = 1;
        return 0;
    }
    js->p++;
    while (js->p < js->end) {
        unsigned char c = (unsigned char)*js->p++;

        if (c == '"') {
            if (dest && cap > 0) {
                dest[used < cap ? used : cap - 1] = '\0';
            }
            return 1;
        }
        if (c == '\\') {
            unsigned code;

            if (js->p >= js->end) {
                js->err = 1;
                return 0;
            }
            c = (unsigned char)*js->p++;
            if (c == 'u') {
                code = js_hex4(js);
                if (js->err) {
                    return 0;
                }
                if (code >= 0xD800 && code <= 0xDBFF && js->p + 6 <= js->end && js->p[0] == '\\' && js->p[1] == 'u') {
                    unsigned low;

                    js->p += 2;
                    low = js_hex4(js);
                    if (!js->err && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                    }
                }
                js_utf8(dest, cap, &used, code);
                continue;
            }
            if (c == 'n') {
                c = '\n';
            } else if (c == 'r') {
                c = '\r';
            } else if (c == 't') {
                c = '\t';
            } else if (c == 'b') {
                c = '\b';
            } else if (c == 'f') {
                c = '\f';
            }
        }
        if (dest && used + 1 < cap) {
            dest[used] = (char)c;
        }
        used++;
    }
    js->err = 1;
    return 0;
}

static int js_skip(ZiJs *js, int depth);

static int js_key(ZiJs *js, int *first, char *dest, size_t cap) {
    js_ws(js);
    if (js->err) {
        return 0;
    }
    if (js->p < js->end && *js->p == '}') {
        js->p++;
        return 0;
    }
    if (!*first) {
        if (!js_eat(js, ',')) {
            return 0;
        }
    }
    *first = 0;
    if (!js_text(js, dest, cap) || !js_eat(js, ':')) {
        js->err = 1;
        return 0;
    }
    return 1;
}

static int js_item(ZiJs *js, int *first) {
    js_ws(js);
    if (js->err) {
        return 0;
    }
    if (js->p < js->end && *js->p == ']') {
        js->p++;
        return 0;
    }
    if (!*first) {
        if (!js_eat(js, ',')) {
            return 0;
        }
    }
    *first = 0;
    return 1;
}

static int js_null(ZiJs *js) {
    js_ws(js);
    if (js->p + 4 <= js->end && memcmp(js->p, "null", 4) == 0) {
        js->p += 4;
        return 1;
    }
    return 0;
}

static int js_double(ZiJs *js, double *out) {
    const char *start;
    char text[128];
    size_t n;
    char *end = NULL;

    js_ws(js);
    start = js->p;
    if (js->p < js->end && (*js->p == '-' || *js->p == '+')) {
        js->p++;
    }
    if (js->p >= js->end || *js->p < '0' || *js->p > '9') {
        js->err = 1;
        return 0;
    }
    while (js->p < js->end && *js->p >= '0' && *js->p <= '9') {
        js->p++;
    }
    if (js->p < js->end && *js->p == '.') {
        js->p++;
        while (js->p < js->end && *js->p >= '0' && *js->p <= '9') {
            js->p++;
        }
    }
    if (js->p < js->end && (*js->p == 'e' || *js->p == 'E')) {
        js->p++;
        if (js->p < js->end && (*js->p == '+' || *js->p == '-')) {
            js->p++;
        }
        while (js->p < js->end && *js->p >= '0' && *js->p <= '9') {
            js->p++;
        }
    }
    n = (size_t)(js->p - start);
    if (n == 0 || n >= sizeof text) {
        js->err = 1;
        return 0;
    }
    memcpy(text, start, n);
    text[n] = '\0';
    *out = strtod(text, &end);
    if (end == text) {
        js->err = 1;
        return 0;
    }
    return 1;
}

static int js_skip(ZiJs *js, int depth) {
    js_ws(js);
    if (depth > 32 || js->p >= js->end) {
        js->err = 1;
        return 0;
    }
    if (*js->p == '"') {
        return js_text(js, NULL, 0);
    }
    if (*js->p == '{') {
        int first = 1;
        char key[80];

        if (!js_eat(js, '{')) {
            return 0;
        }
        while (js_key(js, &first, key, sizeof key)) {
            if (!js_skip(js, depth + 1)) {
                return 0;
            }
        }
        return !js->err;
    }
    if (*js->p == '[') {
        int first = 1;

        if (!js_eat(js, '[')) {
            return 0;
        }
        while (js_item(js, &first)) {
            if (!js_skip(js, depth + 1)) {
                return 0;
            }
        }
        return !js->err;
    }
    if (*js->p == 't' && js->p + 4 <= js->end && memcmp(js->p, "true", 4) == 0) {
        js->p += 4;
        return 1;
    }
    if (*js->p == 'f' && js->p + 5 <= js->end && memcmp(js->p, "false", 5) == 0) {
        js->p += 5;
        return 1;
    }
    if (*js->p == 'n' && js->p + 4 <= js->end && memcmp(js->p, "null", 4) == 0) {
        js->p += 4;
        return 1;
    }
    {
        double ignored = 0;
        return js_double(js, &ignored);
    }
}

static int js_point(ZiJs *js, double *x, double *y) {
    int first = 1;
    char key[16];

    *x = 0;
    *y = 0;
    if (js_null(js)) {
        return 1;
    }
    if (!js_eat(js, '{')) {
        return 0;
    }
    while (js_key(js, &first, key, sizeof key)) {
        double value = 0;

        if (js_null(js)) {
            continue;
        }
        if (!js_double(js, &value)) {
            return 0;
        }
        if (strcmp(key, "x") == 0) {
            *x = value;
        } else if (strcmp(key, "y") == 0) {
            *y = value;
        }
    }
    return !js->err;
}

static int js_tone(ZiJs *js, ZiTone *tone) {
    int first = 1;
    char key[40];

    zi_clear_tone_fields(tone);
    if (!js_eat(js, '{')) {
        return 0;
    }
    while (js_key(js, &first, key, sizeof key)) {
        double value = 0;

        if (strcmp(key, "skinGrade") == 0) {
            int inner = 1;
            char child[40];
            char preset[40];
            double sample_a = 0;
            double sample_b = 0;
            double target_a = 0;
            double target_b = 0;
            double delta_a = 0;
            double delta_b = 0;
            int have_a = 0;
            int have_b = 0;

            preset[0] = '\0';
            if (js_null(js)) {
                continue;
            }
            if (!js_eat(js, '{')) {
                return 0;
            }
            while (js_key(js, &inner, child, sizeof child)) {
                if (strcmp(child, "presetID") == 0) {
                    if (!js_text(js, preset, sizeof preset)) {
                        return 0;
                    }
                } else if (strcmp(child, "deltaA") == 0) {
                    if (!js_double(js, &delta_a)) {
                        return 0;
                    }
                    have_a = 1;
                } else if (strcmp(child, "deltaB") == 0) {
                    if (!js_double(js, &delta_b)) {
                        return 0;
                    }
                    have_b = 1;
                } else if (strcmp(child, "sample") == 0 || strcmp(child, "target") == 0) {
                    double x = 0;
                    double y = 0;
                    double L = 0;
                    int point = 1;
                    char axis[8];
                    int target = strcmp(child, "target") == 0;

                    if (js_null(js)) {
                        continue;
                    }
                    if (!js_eat(js, '{')) {
                        return 0;
                    }
                    while (js_key(js, &point, axis, sizeof axis)) {
                        double axis_value = 0;

                        if (!js_double(js, &axis_value)) {
                            return 0;
                        }
                        if (strcmp(axis, "L") == 0) {
                            L = axis_value;
                        } else if (strcmp(axis, "a") == 0) {
                            x = axis_value;
                        } else if (strcmp(axis, "b") == 0) {
                            y = axis_value;
                        }
                        (void)L;
                    }
                    if (js->err) {
                        return 0;
                    }
                    if (target) {
                        target_a = x;
                        target_b = y;
                    } else {
                        sample_a = x;
                        sample_b = y;
                    }
                } else if (!js_skip(js, 1)) {
                    return 0;
                }
            }
            if (js->err) {
                return 0;
            }
            tone->has_skin = 1;
            tone->skin_preset = zi_skin_preset_index(preset);
            tone->skin_delta_a = have_a ? delta_a : target_a - sample_a;
            tone->skin_delta_b = have_b ? delta_b : target_b - sample_b;
            continue;
        }
        if (js_null(js)) {
            continue;
        }
        if (!js_double(js, &value)) {
            return 0;
        }
        if (strcmp(key, "exposureEV") == 0) {
            tone->exposure = value;
        } else if (strcmp(key, "brightness") == 0) {
            tone->brightness = value;
        } else if (strcmp(key, "contrast") == 0) {
            tone->contrast = value;
        } else if (strcmp(key, "highlights") == 0) {
            tone->highlights = value;
        } else if (strcmp(key, "shadows") == 0) {
            tone->shadows = value;
        } else if (strcmp(key, "whites") == 0) {
            tone->whites = value;
        } else if (strcmp(key, "blacks") == 0) {
            tone->blacks = value;
        } else if (strcmp(key, "texture") == 0) {
            tone->texture = value;
        } else if (strcmp(key, "clarity") == 0) {
            tone->clarity = value;
        } else if (strcmp(key, "dehaze") == 0) {
            tone->dehaze = value;
        } else if (strcmp(key, "vibrance") == 0) {
            tone->vibrance = value;
        } else if (strcmp(key, "saturation") == 0) {
            tone->saturation = value;
        } else if (strcmp(key, "temperature") == 0) {
            tone->temperature = value;
        } else if (strcmp(key, "tint") == 0) {
            tone->tint = value;
        } else if (strcmp(key, "spectrumLow") == 0) {
            tone->spectrum_low = value;
        } else if (strcmp(key, "spectrumMid") == 0) {
            tone->spectrum_mid = value;
        } else if (strcmp(key, "spectrumHigh") == 0) {
            tone->spectrum_high = value;
        }
    }
    return !js->err;
}

static int js_geometry(ZiJs *js, ZiTone *tone) {
    int first = 1;
    char key[40];

    zi_clear_geo_fields(tone);
    if (!js_eat(js, '{')) {
        return 0;
    }
    while (js_key(js, &first, key, sizeof key)) {
        if (strcmp(key, "corners") == 0) {
            int inner = 1;
            char child[24];

            if (js_null(js)) {
                tone->has_corners = 0;
                continue;
            }
            if (!js_eat(js, '{')) {
                return 0;
            }
            tone->has_corners = 1;
            while (js_key(js, &inner, child, sizeof child)) {
                double x = 0;
                double y = 0;
                int index = -1;

                if (!js_point(js, &x, &y)) {
                    return 0;
                }
                if (strcmp(child, "topLeft") == 0) {
                    index = 0;
                } else if (strcmp(child, "topRight") == 0) {
                    index = 1;
                } else if (strcmp(child, "bottomRight") == 0) {
                    index = 2;
                } else if (strcmp(child, "bottomLeft") == 0) {
                    index = 3;
                }
                if (index >= 0) {
                    tone->corner_x[index] = x;
                    tone->corner_y[index] = y;
                }
            }
            if (js->err) {
                return 0;
            }
        } else if (strcmp(key, "lens") == 0) {
            int inner = 1;
            char child[40];

            if (js_null(js)) {
                continue;
            }
            if (!js_eat(js, '{')) {
                return 0;
            }
            tone->fov = 0;
            while (js_key(js, &inner, child, sizeof child)) {
                if (strcmp(child, "profileID") == 0) {
                    char profile[40];

                    if (!js_text(js, profile, sizeof profile)) {
                        return 0;
                    }
                    tone->lens = zi_lens_profile_index(profile);
                } else if (strcmp(child, "cylindricalFOV") == 0) {
                    double value = 0;

                    if (js_null(js)) {
                        tone->fov = 0;
                    } else if (!js_double(js, &value)) {
                        return 0;
                    } else {
                        tone->fov = value;
                    }
                } else if (strcmp(child, "k1") == 0 || strcmp(child, "k2") == 0) {
                    double value = 0;

                    if (!js_double(js, &value)) {
                        return 0;
                    }
                    if (child[1] == '1') {
                        tone->k1 = value;
                    } else {
                        tone->k2 = value;
                    }
                } else if (!js_skip(js, 1)) {
                    return 0;
                }
            }
            if (js->err) {
                return 0;
            }
        } else if (js_null(js)) {
            continue;
        } else {
            double value = 0;

            if (!js_double(js, &value)) {
                return 0;
            }
            if (strcmp(key, "quarterTurns") == 0) {
                int turns = (int)value;
                turns %= 4;
                if (turns < 0) {
                    turns += 4;
                }
                tone->turns = turns;
            } else if (strcmp(key, "straightenDegrees") == 0) {
                tone->straighten = value;
            } else if (strcmp(key, "verticalKeystone") == 0) {
                tone->vertical = value;
            } else if (strcmp(key, "horizontalKeystone") == 0) {
                tone->horizontal = value;
            } else if (strcmp(key, "sharpness") == 0) {
                tone->sharpness = value;
            }
        }
    }
    return !js->err;
}

static int js_crop(ZiJs *js, ZiTone *tone) {
    int first = 1;
    char key[16];

    if (js_null(js)) {
        tone->has_crop = 0;
        return 1;
    }
    if (!js_eat(js, '{')) {
        return 0;
    }
    tone->has_crop = 1;
    tone->crop_x = 0;
    tone->crop_y = 0;
    tone->crop_w = 0;
    tone->crop_h = 0;
    while (js_key(js, &first, key, sizeof key)) {
        double value = 0;

        if (!js_double(js, &value)) {
            return 0;
        }
        if (strcmp(key, "x") == 0) {
            tone->crop_x = value;
        } else if (strcmp(key, "y") == 0) {
            tone->crop_y = value;
        } else if (strcmp(key, "width") == 0) {
            tone->crop_w = value;
        } else if (strcmp(key, "height") == 0) {
            tone->crop_h = value;
        }
    }
    if (tone->crop_w <= 0.0005 || tone->crop_h <= 0.0005) {
        tone->has_crop = 0;
    }
    return !js->err;
}

static char *js_text_alloc(ZiJs *js) {
    size_t cap = 256;
    size_t used = 0;
    char *dest = (char *)malloc(cap);

    if (!dest) {
        js->err = 1;
        return NULL;
    }
    js_ws(js);
    if (js->p >= js->end || *js->p != '"') {
        js->err = 1;
        free(dest);
        return NULL;
    }
    js->p++;
    while (js->p < js->end) {
        unsigned char c = (unsigned char)*js->p++;

        if (used + 8 >= cap) {
            size_t next = cap * 2;
            char *grown;

            if (next > ZI_JSON_CAP) {
                js->err = 1;
                free(dest);
                return NULL;
            }
            grown = (char *)realloc(dest, next);
            if (!grown) {
                js->err = 1;
                free(dest);
                return NULL;
            }
            dest = grown;
            cap = next;
        }
        if (c == '"') {
            dest[used] = '\0';
            return dest;
        }
        if (c == '\\') {
            unsigned code;

            if (js->p >= js->end) {
                js->err = 1;
                free(dest);
                return NULL;
            }
            c = (unsigned char)*js->p++;
            if (c == 'u') {
                code = js_hex4(js);
                if (js->err) {
                    free(dest);
                    return NULL;
                }
                if (code >= 0xD800 && code <= 0xDBFF && js->p + 6 <= js->end && js->p[0] == '\\' && js->p[1] == 'u') {
                    unsigned low;

                    js->p += 2;
                    low = js_hex4(js);
                    if (!js->err && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                    }
                }
                js_utf8(dest, cap, &used, code);
                continue;
            }
            if (c == 'n') {
                c = '\n';
            } else if (c == 'r') {
                c = '\r';
            } else if (c == 't') {
                c = '\t';
            } else if (c == 'b') {
                c = '\b';
            } else if (c == 'f') {
                c = '\f';
            }
        }
        dest[used++] = (char)c;
    }
    js->err = 1;
    free(dest);
    return NULL;
}

static void zi_op_release(ZiTouchOp *op) {
    ZiTouchDoc tmp;

    if (!op || !op->mask) {
        return;
    }
    memset(&tmp, 0, sizeof tmp);
    tmp.count = 1;
    tmp.ops[0] = *op;
    zi_touch_doc_free(&tmp);
    memset(op, 0, sizeof *op);
}

static int js_inpaint_op(ZiJs *js, ZiTouchOp *op, int *have) {
    int first = 1;
    char key[24];
    char id[40];
    char model[40];
    double feather = 2;
    double strength = 1;
    char *png_text = NULL;

    memset(op, 0, sizeof *op);
    id[0] = '\0';
    model[0] = '\0';
    *have = 0;
    if (!js_eat(js, '{')) {
        return 0;
    }
    while (js_key(js, &first, key, sizeof key)) {
        if (strcmp(key, "id") == 0) {
            if (!js_text(js, id, sizeof id)) {
                free(png_text);
                return 0;
            }
        } else if (strcmp(key, "modelID") == 0) {
            if (!js_text(js, model, sizeof model)) {
                free(png_text);
                return 0;
            }
        } else if (strcmp(key, "featherPx") == 0) {
            if (!js_double(js, &feather)) {
                free(png_text);
                return 0;
            }
        } else if (strcmp(key, "strength") == 0) {
            if (!js_double(js, &strength)) {
                free(png_text);
                return 0;
            }
        } else if (strcmp(key, "maskPNG") == 0) {
            free(png_text);
            png_text = js_text_alloc(js);
            if (!png_text) {
                return 0;
            }
        } else if (!js_skip(js, 1)) {
            free(png_text);
            return 0;
        }
    }
    if (js->err) {
        free(png_text);
        return 0;
    }
    if (model[0] && png_text && png_text[0]) {
        unsigned char *png = NULL;
        size_t png_len = 0;
        ZiTouchMask *mask = NULL;

        if (zi_touch_base64_decode(png_text, &png, &png_len) && png &&
            zi_touch_mask_from_png(png, png_len, &mask) && mask) {
            strncpy_s(op->id, 40, id, _TRUNCATE);
            strncpy_s(op->model, 40, model, _TRUNCATE);
            op->feather = (float)feather;
            if (strength < 0) {
                strength = 0;
            }
            if (strength > 1) {
                strength = 1;
            }
            op->strength = (float)strength;
            op->mask = mask;
            *have = 1;
        }
        free(png);
    }
    free(png_text);
    return 1;
}

static int js_inpaints(ZiJs *js, ZiTouchDoc *doc) {
    int item = 1;

    if (js_null(js)) {
        if (doc) {
            zi_touch_doc_free(doc);
        }
        return 1;
    }
    if (!js_eat(js, '[')) {
        return 0;
    }
    if (doc) {
        zi_touch_doc_free(doc);
    }
    while (js_item(js, &item)) {
        ZiTouchOp op;
        int have = 0;

        if (!js_inpaint_op(js, &op, &have)) {
            return 0;
        }
        if (have && doc && doc->count < ZI_TOUCH_CAP) {
            doc->ops[doc->count++] = op;
        } else if (have) {
            zi_op_release(&op);
        }
    }
    return !js->err;
}

static int js_edit(ZiJs *js, ZiTone *tone, ZiTouchDoc *doc) {
    int first = 1;
    char key[24];
    ZiTone parsed;

    zi_tone_identity(&parsed);
    if (!js_eat(js, '{')) {
        return 0;
    }
    if (doc) {
        zi_touch_doc_free(doc);
    }
    while (js_key(js, &first, key, sizeof key)) {
        if (strcmp(key, "tone") == 0) {
            if (!js_tone(js, &parsed)) {
                return 0;
            }
        } else if (strcmp(key, "geometry") == 0) {
            if (!js_geometry(js, &parsed)) {
                return 0;
            }
        } else if (strcmp(key, "crop") == 0) {
            if (!js_crop(js, &parsed)) {
                return 0;
            }
        } else if (strcmp(key, "inpaints") == 0) {
            if (!js_inpaints(js, doc)) {
                return 0;
            }
        } else if (!js_skip(js, 1)) {
            return 0;
        }
    }
    if (js->err) {
        return 0;
    }
    *tone = parsed;
    return 1;
}

static void zi_copy_label(wchar_t *dest, const char *utf8) {
    zi_utf8_wide(utf8 && utf8[0] ? utf8 : "Edit", dest, 40);
}

static int js_apply_command(ZiJs *js, ZiTone *tone, ZiTouchDoc *doc, wchar_t *label) {
    int first = 1;
    char key[40];

    wcscpy_s(label, 40, L"Edit");
    if (!js_eat(js, '{')) {
        return 0;
    }
    while (js_key(js, &first, key, sizeof key)) {
        if (strcmp(key, "keyframe") == 0) {
            if (!js_edit(js, tone, doc)) {
                return 0;
            }
            wcscpy_s(label, 40, L"Earlier edits");
        } else if (strcmp(key, "addInpaint") == 0) {
            ZiTouchOp op;
            int have = 0;

            if (!js_inpaint_op(js, &op, &have)) {
                return 0;
            }
            if (have && doc && doc->count < ZI_TOUCH_CAP) {
                doc->ops[doc->count++] = op;
                if (strcmp(op.model, ZI_TOUCH_SCRATCH) == 0) {
                    wcscpy_s(label, 40, L"Scratches");
                } else {
                    wcscpy_s(label, 40, L"Dust");
                }
            } else if (have) {
                zi_op_release(&op);
            }
        } else if (strcmp(key, "setInpaintStrength") == 0) {
            int inner = 1;
            char child[16];
            char id[40];
            char text[160];
            double strength = 1;

            id[0] = '\0';
            text[0] = '\0';
            if (!js_eat(js, '{')) {
                return 0;
            }
            while (js_key(js, &inner, child, sizeof child)) {
                if (strcmp(child, "id") == 0) {
                    if (!js_text(js, id, sizeof id)) {
                        return 0;
                    }
                } else if (strcmp(child, "label") == 0) {
                    if (!js_text(js, text, sizeof text)) {
                        return 0;
                    }
                } else if (strcmp(child, "strength") == 0) {
                    if (!js_double(js, &strength)) {
                        return 0;
                    }
                } else if (!js_skip(js, 1)) {
                    return 0;
                }
            }
            if (js->err) {
                return 0;
            }
            if (doc && id[0]) {
                int i;

                if (strength < 0) {
                    strength = 0;
                }
                if (strength > 1) {
                    strength = 1;
                }
                for (i = 0; i < doc->count; i++) {
                    if (strcmp(doc->ops[i].id, id) == 0) {
                        doc->ops[i].strength = (float)strength;
                    }
                }
            }
            if (text[0]) {
                zi_copy_label(label, text);
            }
        } else if (strcmp(key, "setTone") == 0 || strcmp(key, "setGeometry") == 0) {
            int inner = 1;
            char child[16];
            int geometry = key[3] == 'G';
            ZiTone part = *tone;

            if (!js_eat(js, '{')) {
                return 0;
            }
            if (geometry) {
                zi_clear_geo_fields(&part);
            } else {
                zi_clear_tone_fields(&part);
            }
            while (js_key(js, &inner, child, sizeof child)) {
                if (strcmp(child, "_0") == 0) {
                    if (geometry) {
                        if (!js_geometry(js, &part)) {
                            return 0;
                        }
                    } else if (!js_tone(js, &part)) {
                        return 0;
                    }
                } else if (strcmp(child, "label") == 0) {
                    char text[160];

                    if (!js_text(js, text, sizeof text)) {
                        return 0;
                    }
                    zi_copy_label(label, text);
                } else if (!js_skip(js, 1)) {
                    return 0;
                }
            }
            if (js->err) {
                return 0;
            }
            if (geometry) {
                tone->straighten = part.straighten;
                tone->vertical = part.vertical;
                tone->horizontal = part.horizontal;
                tone->sharpness = part.sharpness;
                tone->k1 = part.k1;
                tone->k2 = part.k2;
                tone->fov = part.fov;
                tone->lens = part.lens;
                tone->has_corners = part.has_corners;
                memcpy(tone->corner_x, part.corner_x, sizeof tone->corner_x);
                memcpy(tone->corner_y, part.corner_y, sizeof tone->corner_y);
                tone->turns = part.turns;
            } else {
                zi_clear_tone_fields(tone);
                tone->exposure = part.exposure;
                tone->brightness = part.brightness;
                tone->contrast = part.contrast;
                tone->highlights = part.highlights;
                tone->shadows = part.shadows;
                tone->whites = part.whites;
                tone->blacks = part.blacks;
                tone->texture = part.texture;
                tone->clarity = part.clarity;
                tone->dehaze = part.dehaze;
                tone->vibrance = part.vibrance;
                tone->saturation = part.saturation;
                tone->temperature = part.temperature;
                tone->tint = part.tint;
                tone->has_skin = part.has_skin;
                tone->skin_preset = part.skin_preset;
                tone->skin_delta_a = part.skin_delta_a;
                tone->skin_delta_b = part.skin_delta_b;
                tone->spectrum_low = part.spectrum_low;
                tone->spectrum_mid = part.spectrum_mid;
                tone->spectrum_high = part.spectrum_high;
            }
        } else if (strcmp(key, "setCrop") == 0) {
            if (!js_crop(js, tone)) {
                return 0;
            }
            wcscpy_s(label, 40, L"Crop");
        } else if (strcmp(key, "pasteSettings") == 0) {
            int inner = 1;
            char child[24];
            ZiTone part = *tone;

            if (!js_eat(js, '{')) {
                return 0;
            }
            zi_clear_tone_fields(&part);
            zi_clear_geo_fields(&part);
            while (js_key(js, &inner, child, sizeof child)) {
                if (strcmp(child, "tone") == 0) {
                    if (!js_tone(js, &part)) {
                        return 0;
                    }
                } else if (strcmp(child, "geometry") == 0) {
                    if (!js_geometry(js, &part)) {
                        return 0;
                    }
                } else if (!js_skip(js, 1)) {
                    return 0;
                }
            }
            if (js->err) {
                return 0;
            }
            {
                int has_crop = tone->has_crop;
                double x = tone->crop_x;
                double y = tone->crop_y;
                double w = tone->crop_w;
                double h = tone->crop_h;

                *tone = part;
                tone->has_crop = has_crop;
                tone->crop_x = x;
                tone->crop_y = y;
                tone->crop_w = w;
                tone->crop_h = h;
            }
            wcscpy_s(label, 40, L"Paste settings");
        } else if (!js_skip(js, 1)) {
            return 0;
        }
    }
    return !js->err;
}

static void zi_hist_parse_fail(ZiHistStep *states, ZiTouchDoc *log, ZiTouchDoc *running) {
    int i;

    zi_touch_doc_free(running);
    if (log) {
        for (i = 0; i < ZI_LOG_CAP; i++) {
            zi_touch_doc_free(&log[i]);
        }
        free(log);
    }
    free(states);
}

static int js_history(ZiJs *js, ZiHistStep *hist, int cap, int *len, int *pos, ZiTouchDoc **out_docs) {
    int first = 1;
    char key[24];
    ZiHistStep *states;
    ZiTouchDoc *log;
    int count = 1;
    int cursor = 0;
    int dropped = 0;
    ZiTone running;
    ZiTouchDoc running_doc;

    if (out_docs) {
        *out_docs = NULL;
    }
    if (cap < 1 || !out_docs) {
        return 0;
    }
    states = (ZiHistStep *)calloc((size_t)ZI_LOG_CAP, sizeof(ZiHistStep));
    log = (ZiTouchDoc *)calloc((size_t)ZI_LOG_CAP, sizeof(ZiTouchDoc));
    if (!states || !log) {
        zi_hist_parse_fail(states, log, NULL);
        return 0;
    }
    zi_tone_identity(&running);
    zi_touch_doc_clear(&running_doc);
    states[0].tone = running;
    wcscpy_s(states[0].label, 40, L"Open");
    if (!js_eat(js, '{')) {
        zi_hist_parse_fail(states, log, &running_doc);
        return 0;
    }
    while (js_key(js, &first, key, sizeof key)) {
        if (strcmp(key, "cursor") == 0) {
            double value = 0;

            if (!js_double(js, &value)) {
                zi_hist_parse_fail(states, log, &running_doc);
                return 0;
            }
            cursor = (int)value;
        } else if (strcmp(key, "commands") == 0) {
            int item = 1;

            if (!js_eat(js, '[')) {
                zi_hist_parse_fail(states, log, &running_doc);
                return 0;
            }
            while (js_item(js, &item)) {
                wchar_t label[40];

                if (!js_apply_command(js, &running, &running_doc, label)) {
                    zi_hist_parse_fail(states, log, &running_doc);
                    return 0;
                }
                if (count < ZI_LOG_CAP) {
                    states[count].tone = running;
                    wcscpy_s(states[count].label, 40, label);
                    zi_touch_doc_copy(&log[count], &running_doc);
                    count++;
                } else {
                    memmove(states, states + 1, (size_t)(ZI_LOG_CAP - 1) * sizeof(ZiHistStep));
                    states[ZI_LOG_CAP - 1].tone = running;
                    wcscpy_s(states[ZI_LOG_CAP - 1].label, 40, label);
                    zi_touch_doc_free(&log[0]);
                    memmove(log, log + 1, (size_t)(ZI_LOG_CAP - 1) * sizeof(ZiTouchDoc));
                    zi_touch_doc_clear(&log[ZI_LOG_CAP - 1]);
                    zi_touch_doc_copy(&log[ZI_LOG_CAP - 1], &running_doc);
                    dropped++;
                }
            }
            if (js->err) {
                zi_hist_parse_fail(states, log, &running_doc);
                return 0;
            }
        } else if (!js_skip(js, 1)) {
            zi_hist_parse_fail(states, log, &running_doc);
            return 0;
        }
    }
    if (js->err) {
        zi_hist_parse_fail(states, log, &running_doc);
        return 0;
    }
    if (cursor < 0) {
        cursor = 0;
    }
    if (dropped > 0) {
        cursor -= dropped;
    }
    if (cursor > count - 1) {
        cursor = count - 1;
    }
    if (cursor < 0) {
        cursor = 0;
    }
    {
        int start = 0;
        int i;
        int kept;
        int any = 0;
        ZiTouchDoc *kept_docs = NULL;

        if (count > cap) {
            start = cursor - (cap - 1);
            if (start < 0) {
                start = 0;
            }
            if (start + cap > count) {
                start = count - cap;
            }
        }
        kept = count - start;
        if (kept > cap) {
            kept = cap;
        }
        for (i = 0; i < kept; i++) {
            hist[i].tone = states[start + i].tone;
            if (i == 0 && start == 0 && dropped == 0) {
                wcscpy_s(hist[i].label, 40, L"Open");
            } else if (i == 0) {
                wcscpy_s(hist[i].label, 40, L"Earlier edits");
            } else {
                wcscpy_s(hist[i].label, 40, states[start + i].label);
            }
            if (zi_touch_doc_active(&log[start + i])) {
                any = 1;
            }
        }
        if (any) {
            kept_docs = (ZiTouchDoc *)calloc((size_t)ZI_HIST, sizeof(ZiTouchDoc));
            if (!kept_docs) {
                zi_hist_parse_fail(states, log, &running_doc);
                return 0;
            }
            for (i = 0; i < kept; i++) {
                zi_touch_doc_move(&kept_docs[i], &log[start + i]);
            }
        }
        *len = kept;
        *pos = cursor - start;
        if (*pos < 0) {
            *pos = 0;
        }
        if (*pos >= kept) {
            *pos = kept - 1;
        }
        *out_docs = kept_docs;
    }
    zi_touch_doc_free(&running_doc);
    zi_hist_parse_fail(NULL, log, NULL);
    free(states);
    return 1;
}

static const unsigned char k_magic[8] = { 0x5A, 0x49, 0x4D, 0x47, 0x52, 0x41, 0x57, 0x01 };
static const unsigned char k_fixture[4] = { 0xFF, 0xD8, 0xFF, 0xD9 };

static int zi_log_cursor(const ZiHistStep *hist, int len, int pos, const ZiTouchDoc *docs) {
    if (len < 1) {
        return 0;
    }
    if (pos < 0) {
        pos = 0;
    }
    if (pos >= len) {
        pos = len - 1;
    }
    return pos + (zi_step_extra(hist, docs) ? 1 : 0);
}

static int zi_worth_writing(const ZiHistStep *hist, int len, const ZiTouchDoc *docs) {
    int i;
    ZiTone bare;

    if (len < 1) {
        return 0;
    }
    zi_tone_identity(&bare);
    for (i = 0; i < len; i++) {
        if (!zi_tone_same(&hist[i].tone, &bare)) {
            return 1;
        }
        if (docs && zi_touch_doc_active(&docs[i])) {
            return 1;
        }
    }
    return 0;
}

static void buf_u32(ZiBuf *buf, unsigned value) {
    char text[16];

    sprintf_s(text, sizeof text, "%u", value);
    buf_raw(buf, text);
}

static int buf_header(ZiBuf *buf, const char *blake, unsigned long long byte_size, const char *uti,
                      const char *path_utf8, unsigned cursor, unsigned history_len, unsigned preview_len) {
    int first = 1;
    int base = 1;
    int history = 1;
    int preview = 1;
    int section = 1;
    unsigned preview_at = history_len;
    unsigned masks_at = history_len + preview_len;
    char size_text[32];

    buf_raw(buf, "{");
    buf_key(buf, &first, "appVersion");
    buf_json(buf, "1.0.0");
    buf_key(buf, &first, "base");
    buf_raw(buf, "{");
    buf_key(buf, &base, "blake3");
    buf_json(buf, blake);
    buf_key(buf, &base, "byteSize");
    sprintf_s(size_text, sizeof size_text, "%llu", byte_size);
    buf_raw(buf, size_text);
    buf_key(buf, &base, "kind");
    buf_json(buf, "fileRef");
    buf_key(buf, &base, "pathHint");
    buf_json(buf, path_utf8 ? path_utf8 : "");
    buf_key(buf, &base, "uti");
    buf_json(buf, uti ? uti : "public.image");
    buf_raw(buf, "}");
    buf_key(buf, &first, "envelope");
    buf_raw(buf, "1");
    buf_key(buf, &first, "history");
    buf_raw(buf, "{");
    buf_key(buf, &history, "cursor");
    buf_u32(buf, cursor);
    buf_key(buf, &history, "depthCap");
    buf_raw(buf, "100");
    buf_key(buf, &history, "section");
    buf_json(buf, "history");
    buf_raw(buf, "}");
    buf_key(buf, &first, "preview");
    buf_raw(buf, "{");
    buf_key(buf, &preview, "longEdge");
    buf_raw(buf, "2048");
    buf_key(buf, &preview, "section");
    buf_json(buf, "preview");
    buf_key(buf, &preview, "uti");
    buf_json(buf, "public.jpeg");
    buf_raw(buf, "}");
    buf_key(buf, &first, "schemaVersion");
    buf_raw(buf, "1");
    buf_key(buf, &first, "sections");
    buf_raw(buf, "[{");
    buf_key(buf, &section, "codec");
    buf_json(buf, "none");
    buf_key(buf, &section, "length");
    buf_u32(buf, history_len);
    buf_key(buf, &section, "name");
    buf_json(buf, "history");
    buf_key(buf, &section, "offset");
    buf_raw(buf, "0");
    buf_raw(buf, "},{");
    section = 1;
    buf_key(buf, &section, "codec");
    buf_json(buf, "none");
    buf_key(buf, &section, "length");
    buf_u32(buf, preview_len);
    buf_key(buf, &section, "name");
    buf_json(buf, "preview");
    buf_key(buf, &section, "offset");
    buf_u32(buf, preview_at);
    buf_raw(buf, "},{");
    section = 1;
    buf_key(buf, &section, "codec");
    buf_json(buf, "none");
    buf_key(buf, &section, "length");
    buf_raw(buf, "4");
    buf_key(buf, &section, "name");
    buf_json(buf, "masks");
    buf_key(buf, &section, "offset");
    buf_u32(buf, masks_at);
    buf_raw(buf, "}]");
    buf_raw(buf, "}");
    return !buf->err;
}

static int zi_build_document(const wchar_t *photo, const unsigned char digest[32], unsigned long long byte_size,
                             const ZiHistStep *hist, int len, int pos, const ZiTouchDoc *docs,
                             const unsigned char *preview, size_t preview_len, unsigned char **out, size_t *out_len) {
    ZiBuf history;
    ZiBuf header;
    unsigned char *file;
    const unsigned char *jpeg;
    size_t jpeg_len;
    unsigned char masks[4] = { 0, 0, 0, 0 };
    char hex[65];
    char path_utf8[4096];
    size_t total;
    unsigned char *cursor;
    unsigned header_len;

    memset(&history, 0, sizeof history);
    memset(&header, 0, sizeof header);
    if (!out || !out_len || !digest || len < 1) {
        return 0;
    }
    *out = NULL;
    *out_len = 0;
    if (!buf_history(&history, hist, docs, len, pos)) {
        free(history.data);
        return 0;
    }
    jpeg = (preview && preview_len >= 4) ? preview : k_fixture;
    jpeg_len = (preview && preview_len >= 4) ? preview_len : 4;
    if (jpeg_len > 0x7fffffffu || history.len > 0x7fffffffu) {
        free(history.data);
        return 0;
    }
    zi_blake3_hex(digest, hex);
    path_utf8[0] = '\0';
    if (photo && photo[0]) {
        zi_wide_utf8(photo, path_utf8, sizeof path_utf8);
    }
    if (!buf_header(&header, hex, byte_size, zi_uti(photo), path_utf8,
                    (unsigned)zi_log_cursor(hist, len, pos, docs), (unsigned)history.len, (unsigned)jpeg_len)) {
        free(history.data);
        free(header.data);
        return 0;
    }
    header_len = (unsigned)header.len;
    total = 12u + header.len + history.len + jpeg_len + 4u;
    file = (unsigned char *)malloc(total);
    if (!file) {
        free(history.data);
        free(header.data);
        return 0;
    }
    cursor = file;
    memcpy(cursor, k_magic, 8);
    cursor += 8;
    cursor[0] = (unsigned char)header_len;
    cursor[1] = (unsigned char)(header_len >> 8);
    cursor[2] = (unsigned char)(header_len >> 16);
    cursor[3] = (unsigned char)(header_len >> 24);
    cursor += 4;
    memcpy(cursor, header.data, header.len);
    cursor += header.len;
    memcpy(cursor, history.data, history.len);
    cursor += history.len;
    memcpy(cursor, jpeg, jpeg_len);
    cursor += jpeg_len;
    memcpy(cursor, masks, 4);
    free(history.data);
    free(header.data);
    *out = file;
    *out_len = total;
    return 1;
}

static int zi_write_bytes(const wchar_t *path, const void *data, size_t n) {
    wchar_t temp[MAX_PATH];
    HANDLE file;
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;

    if (!path || !path[0] || !data || wcslen(path) + 5 >= MAX_PATH) {
        return 0;
    }
    swprintf_s(temp, MAX_PATH, L"%s.tmp", path);
    file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    while (done < n) {
        DWORD wrote = 0;
        DWORD chunk = (n - done) > 1u << 20 ? (1u << 20) : (DWORD)(n - done);

        if (!WriteFile(file, bytes + done, chunk, &wrote, NULL) || wrote == 0) {
            CloseHandle(file);
            DeleteFileW(temp);
            return 0;
        }
        done += wrote;
    }
    CloseHandle(file);
    if (!MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp);
        return 0;
    }
    return 1;
}

static unsigned zi_u32le(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int zi_read_span(HANDLE file, unsigned long long offset, size_t n, void *dest) {
    unsigned char *out = (unsigned char *)dest;
    size_t done = 0;
    LARGE_INTEGER place;

    place.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, place, NULL, FILE_BEGIN)) {
        return 0;
    }
    while (done < n) {
        DWORD got = 0;
        DWORD chunk = (n - done) > 1u << 20 ? (1u << 20) : (DWORD)(n - done);

        if (!ReadFile(file, out + done, chunk, &got, NULL) || got == 0) {
            return 0;
        }
        done += got;
    }
    return 1;
}

void zi_zimg_info_free(ZiZimgInfo *info) {
    if (!info) {
        return;
    }
    free(info->embedded);
    free(info->keep);
    if (info->touch) {
        ZiTouchDoc *docs = (ZiTouchDoc *)info->touch;
        int i;

        for (i = 0; i < ZI_HIST; i++) {
            zi_touch_doc_free(&docs[i]);
        }
        free(docs);
    }
    info->embedded = NULL;
    info->keep = NULL;
    info->touch = NULL;
    info->embedded_len = 0;
}

static int js_copy_text(ZiJs *js, char *dest, size_t cap) {
    if (js_null(js)) {
        if (dest && cap) {
            dest[0] = '\0';
        }
        return 1;
    }
    return js_text(js, dest, cap);
}

static int zi_parse_base(ZiJs *js, ZiZimgInfo *info) {
    int first = 1;
    char key[40];
    char text[128];

    if (!js_eat(js, '{')) {
        return 0;
    }
    while (js_key(js, &first, key, sizeof key)) {
        if (strcmp(key, "kind") == 0) {
            if (!js_copy_text(js, text, sizeof text)) {
                return 0;
            }
            if (strcmp(text, "fileRef") == 0) {
                info->kind = 0;
            } else if (strcmp(text, "catalogRef") == 0) {
                info->kind = 1;
            } else if (strcmp(text, "embedded") == 0) {
                info->kind = 2;
            } else {
                info->kind = -1;
            }
        } else if (strcmp(key, "blake3") == 0) {
            if (!js_copy_text(js, text, sizeof text)) {
                return 0;
            }
            info->has_digest = zi_blake3_from_hex(text, info->digest);
        } else if (strcmp(key, "byteSize") == 0) {
            double value = 0;

            if (!js_double(js, &value) || value < 0) {
                return 0;
            }
            info->byte_size = (unsigned long long)value;
        } else if (strcmp(key, "uti") == 0) {
            if (!js_copy_text(js, info->uti, sizeof info->uti)) {
                return 0;
            }
        } else if (strcmp(key, "pathHint") == 0) {
            char *path = (char *)malloc(8192);

            if (!path) {
                return 0;
            }
            if (!js_copy_text(js, path, 8192)) {
                free(path);
                return 0;
            }
            zi_utf8_wide(path, info->path_hint, ZI_ZIMG_HINT);
            free(path);
        } else if (strcmp(key, "relativePathHint") == 0) {
            char *path = (char *)malloc(8192);

            if (!path) {
                return 0;
            }
            if (!js_copy_text(js, path, 8192)) {
                free(path);
                return 0;
            }
            zi_utf8_wide(path, info->relative_hint, ZI_ZIMG_HINT);
            free(path);
        } else if (!js_skip(js, 1)) {
            return 0;
        }
    }
    return !js->err;
}

static int zi_parse_sections(ZiJs *js, unsigned *history_at, unsigned *history_n, unsigned *embedded_at,
                             unsigned *embedded_n, int *has_embedded) {
    int item = 1;

    *history_at = 0;
    *history_n = 0;
    *embedded_at = 0;
    *embedded_n = 0;
    *has_embedded = 0;
    if (!js_eat(js, '[')) {
        return 0;
    }
    while (js_item(js, &item)) {
        int first = 1;
        char key[24];
        char name[40];
        double offset = 0;
        double length = 0;
        int have_offset = 0;
        int have_length = 0;

        name[0] = '\0';
        if (!js_eat(js, '{')) {
            return 0;
        }
        while (js_key(js, &first, key, sizeof key)) {
            if (strcmp(key, "name") == 0) {
                if (!js_copy_text(js, name, sizeof name)) {
                    return 0;
                }
            } else if (strcmp(key, "offset") == 0) {
                if (!js_double(js, &offset)) {
                    return 0;
                }
                have_offset = 1;
            } else if (strcmp(key, "length") == 0) {
                if (!js_double(js, &length)) {
                    return 0;
                }
                have_length = 1;
            } else if (!js_skip(js, 1)) {
                return 0;
            }
        }
        if (js->err || !have_offset || !have_length || offset < 0 || length < 0) {
            return 0;
        }
        if (strcmp(name, "history") == 0) {
            *history_at = (unsigned)offset;
            *history_n = (unsigned)length;
        } else if (strcmp(name, "base-original") == 0) {
            *embedded_at = (unsigned)offset;
            *embedded_n = (unsigned)length;
            *has_embedded = 1;
        }
    }
    return !js->err;
}

int zi_zimg_parse(const wchar_t *path, ZiZimgInfo *info) {
    HANDLE file;
    unsigned char lead[12];
    unsigned header_len;
    char *header = NULL;
    LARGE_INTEGER file_size;
    unsigned long long payload_at;
    unsigned history_at = 0;
    unsigned history_n = 0;
    unsigned embedded_at = 0;
    unsigned embedded_n = 0;
    int has_embedded = 0;
    int schema = -1;
    ZiJs js;
    int first = 1;
    char key[32];
    char *history = NULL;
    int ok = 0;

    if (!path || !info) {
        return 0;
    }
    memset(info, 0, sizeof *info);
    info->kind = -1;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 12) {
        CloseHandle(file);
        return 0;
    }
    if (!zi_read_span(file, 0, 12, lead) || memcmp(lead, k_magic, 8) != 0) {
        CloseHandle(file);
        return 0;
    }
    header_len = zi_u32le(lead + 8);
    payload_at = 12ull + header_len;
    if (header_len < 2 || header_len > 8u * 1024u * 1024u || (unsigned long long)file_size.QuadPart < payload_at) {
        CloseHandle(file);
        return 0;
    }
    header = (char *)malloc((size_t)header_len + 1u);
    if (!header || !zi_read_span(file, 12, header_len, header)) {
        free(header);
        CloseHandle(file);
        return 0;
    }
    header[header_len] = '\0';
    js.p = header;
    js.end = header + header_len;
    js.err = 0;
    if (!js_eat(&js, '{')) {
        free(header);
        CloseHandle(file);
        return 0;
    }
    while (js_key(&js, &first, key, sizeof key)) {
        if (strcmp(key, "schemaVersion") == 0) {
            double value = 0;

            if (!js_double(&js, &value)) {
                break;
            }
            schema = (int)value;
        } else if (strcmp(key, "base") == 0) {
            if (!zi_parse_base(&js, info)) {
                break;
            }
        } else if (strcmp(key, "sections") == 0) {
            if (!zi_parse_sections(&js, &history_at, &history_n, &embedded_at, &embedded_n, &has_embedded)) {
                break;
            }
        } else if (!js_skip(&js, 1)) {
            break;
        }
    }
    free(header);
    if (js.err || schema != 1 || info->kind < 0) {
        CloseHandle(file);
        zi_zimg_info_free(info);
        memset(info, 0, sizeof *info);
        info->kind = -1;
        return 0;
    }
    if (history_n > 0) {
        unsigned long long at = payload_at + history_at;

        if (history_n > ZI_JSON_CAP || at + history_n > (unsigned long long)file_size.QuadPart) {
            CloseHandle(file);
            zi_zimg_info_free(info);
            memset(info, 0, sizeof *info);
            info->kind = -1;
            return 0;
        }
        history = (char *)malloc((size_t)history_n + 1u);
        if (!history || !zi_read_span(file, at, history_n, history)) {
            free(history);
            CloseHandle(file);
            zi_zimg_info_free(info);
            memset(info, 0, sizeof *info);
            info->kind = -1;
            return 0;
        }
        history[history_n] = '\0';
        {
            ZiTouchDoc *touch = NULL;

            ok = js_history(&(ZiJs){ history, history + history_n, 0 }, info->hist, ZI_HIST, &info->hist_len,
                            &info->hist_pos, &touch);
            info->touch = touch;
        }
        free(history);
        if (!ok) {
            CloseHandle(file);
            zi_zimg_info_free(info);
            memset(info, 0, sizeof *info);
            info->kind = -1;
            return 0;
        }
    } else {
        zi_tone_identity(&info->hist[0].tone);
        wcscpy_s(info->hist[0].label, 40, L"Open");
        info->hist_len = 1;
        info->hist_pos = 0;
    }
    if (has_embedded && embedded_n > 0 && embedded_n <= 64u * 1024u * 1024u) {
        unsigned long long at = payload_at + embedded_at;

        if (at + embedded_n <= (unsigned long long)file_size.QuadPart) {
            info->embedded = (unsigned char *)malloc(embedded_n);
            if (info->embedded && zi_read_span(file, at, embedded_n, info->embedded)) {
                info->embedded_len = embedded_n;
            } else {
                free(info->embedded);
                info->embedded = NULL;
            }
        }
    }
    CloseHandle(file);
    return 1;
}

int zi_zimg_edits_dir(wchar_t *dest, size_t cap) {
    wchar_t base[MAX_PATH];
    wchar_t root[MAX_PATH];
    DWORD n;
    DWORD attr;

    if (!dest || cap < 8) {
        return 0;
    }
    dest[0] = L'\0';
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return 0;
    }
    if (swprintf_s(root, MAX_PATH, L"%s\\ZImageWin", base) < 0) {
        return 0;
    }
    CreateDirectoryW(root, NULL);
    if (swprintf_s(dest, cap, L"%s\\edits", root) < 0) {
        return 0;
    }
    CreateDirectoryW(dest, NULL);
    attr = GetFileAttributesW(dest);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int zi_digest_name(const unsigned char digest[32], const wchar_t *suffix, wchar_t *dest, size_t cap) {
    wchar_t dir[MAX_PATH];
    char hex[65];
    wchar_t name[80];

    if (!zi_zimg_edits_dir(dir, MAX_PATH)) {
        return 0;
    }
    zi_blake3_hex(digest, hex);
    if (MultiByteToWideChar(CP_UTF8, 0, hex, -1, name, 80) <= 0) {
        return 0;
    }
    return swprintf_s(dest, cap, L"%s\\%s%s", dir, name, suffix) > 0;
}

static int zi_read_ref(const unsigned char digest[32], wchar_t *dest, size_t cap) {
    wchar_t ref[MAX_PATH];
    HANDLE file;
    char bytes[4096];
    DWORD got = 0;
    const char *text;

    dest[0] = L'\0';
    if (!zi_digest_name(digest, L".ref", ref, MAX_PATH)) {
        return 0;
    }
    file = CreateFileW(ref, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (!ReadFile(file, bytes, sizeof bytes - 1, &got, NULL)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    bytes[got] = '\0';
    text = bytes;
    if (got >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text += 3;
    }
    zi_utf8_wide(text, dest, cap);
    return dest[0] != L'\0';
}

static int zi_write_ref(const unsigned char digest[32], const wchar_t *target) {
    wchar_t ref[MAX_PATH];
    char utf8[4096];

    if (!target || !target[0] || !zi_digest_name(digest, L".ref", ref, MAX_PATH)) {
        return 0;
    }
    if (!zi_wide_utf8(target, utf8, sizeof utf8)) {
        return 0;
    }
    return zi_write_bytes(ref, utf8, strlen(utf8));
}

static int zi_sibling_path(const wchar_t *photo, wchar_t *dest, size_t cap) {
    const wchar_t *slash;
    wchar_t *dot;

    if (!photo || !photo[0] || wcslen(photo) + 6 >= cap) {
        return 0;
    }
    wcscpy_s(dest, cap, photo);
    slash = wcsrchr(dest, L'\\');
    dot = wcsrchr(dest, L'.');
    if (dot && (!slash || dot > slash)) {
        wcscpy_s(dot, (size_t)(dest + cap - dot), L".zimg");
    } else if (wcslen(dest) + 5 < cap) {
        wcscat_s(dest, cap, L".zimg");
    } else {
        return 0;
    }
    return 1;
}

static int zi_active_dest(const unsigned char digest[32], wchar_t *dest, size_t cap) {
    if (zi_read_ref(digest, dest, cap)) {
        return 1;
    }
    return zi_digest_name(digest, L".zimg", dest, cap);
}

static int zi_hist_matches(const ZiZimgInfo *info, const unsigned char digest[32], unsigned long long byte_size) {
    if (!info || !info->has_digest || !digest) {
        return 0;
    }
    if (info->kind != 0 && info->kind != 1) {
        return 0;
    }
    return info->byte_size == byte_size && memcmp(info->digest, digest, 32) == 0 && info->hist_len > 0;
}

static void zi_docs_release(ZiTouchDoc *docs) {
    int i;

    if (!docs) {
        return;
    }
    for (i = 0; i < ZI_HIST; i++) {
        zi_touch_doc_free(&docs[i]);
    }
    free(docs);
}

static int zi_try_hist(const wchar_t *path, const unsigned char digest[32], unsigned long long byte_size,
                       ZiHistStep *hist, int cap, int *len, int *pos, ZiTouchDoc *docs) {
    ZiZimgInfo info;
    int i;
    int n;

    if (!path || !path[0] || !zi_zimg_parse(path, &info)) {
        return 0;
    }
    if (!zi_hist_matches(&info, digest, byte_size)) {
        zi_zimg_info_free(&info);
        return 0;
    }
    if (info.hist_len > cap) {
        info.hist_len = cap;
    }
    memcpy(hist, info.hist, (size_t)info.hist_len * sizeof(ZiHistStep));
    *len = info.hist_len;
    *pos = info.hist_pos;
    if (*pos < 0) {
        *pos = 0;
    }
    if (*pos >= *len) {
        *pos = *len - 1;
    }
    if (docs) {
        n = info.hist_len;
        if (n > ZI_HIST) {
            n = ZI_HIST;
        }
        for (i = 0; i < ZI_HIST; i++) {
            zi_touch_doc_free(&docs[i]);
        }
        if (info.touch) {
            ZiTouchDoc *src = (ZiTouchDoc *)info.touch;

            for (i = 0; i < n; i++) {
                zi_touch_doc_copy(&docs[i], &src[i]);
            }
        }
    }
    zi_zimg_info_free(&info);
    return 1;
}

int zi_zimg_load_hist(const wchar_t *photo, const unsigned char digest[32], unsigned long long byte_size,
                      ZiHistStep *hist, int cap, int *len, int *pos, ZiTouchDoc *docs) {
    wchar_t path[MAX_PATH];

    if (!hist || cap < 1 || !len || !pos || !digest) {
        return 0;
    }
    *len = 0;
    *pos = 0;
    if (zi_read_ref(digest, path, MAX_PATH) && zi_try_hist(path, digest, byte_size, hist, cap, len, pos, docs)) {
        return 1;
    }
    if (zi_digest_name(digest, L".zimg", path, MAX_PATH) &&
        zi_try_hist(path, digest, byte_size, hist, cap, len, pos, docs)) {
        return 1;
    }
    if (zi_sibling_path(photo, path, MAX_PATH) && zi_try_hist(path, digest, byte_size, hist, cap, len, pos, docs)) {
        return 1;
    }
    return 0;
}

static int zi_prepare_hist(const wchar_t *dest, const unsigned char digest[32], unsigned long long byte_size,
                           ZiHistStep *hist, int *len, int *pos, ZiTouchDoc *docs) {
    ZiZimgInfo info;
    ZiHistStep merged[ZI_HIST];
    ZiTouchDoc *merged_docs = NULL;
    ZiTone file_now;
    const ZiTouchDoc *file_doc = NULL;
    int keep;
    int add;
    int total;
    int start;
    int npos;
    int i;
    int out_n = 0;

    if (*len < 1 || !zi_worth_writing(hist, *len, docs)) {
        return 0;
    }
    if (!dest || !dest[0] || !zi_zimg_parse(dest, &info)) {
        return 1;
    }
    if (!zi_hist_matches(&info, digest, byte_size)) {
        zi_zimg_info_free(&info);
        return 1;
    }
    file_now = info.hist[info.hist_pos].tone;
    if (info.touch && info.hist_pos >= 0 && info.hist_pos < ZI_HIST) {
        file_doc = &((const ZiTouchDoc *)info.touch)[info.hist_pos];
    }
    if (zi_identity(&hist[0].tone) || !zi_tone_same(&hist[0].tone, &file_now) ||
        !zi_touch_doc_same(docs ? &docs[0] : NULL, file_doc)) {
        zi_zimg_info_free(&info);
        return 1;
    }
    if (*len <= 1) {
        zi_zimg_info_free(&info);
        return 0;
    }
    keep = info.hist_pos + 1;
    if (keep < 1) {
        keep = 1;
    }
    if (keep > info.hist_len) {
        keep = info.hist_len;
    }
    add = *len - 1;
    total = keep + add;
    start = total > ZI_HIST ? total - ZI_HIST : 0;
    npos = info.hist_pos + *pos - start;
    if (npos < 0) {
        npos = 0;
    }
    if (docs) {
        merged_docs = (ZiTouchDoc *)calloc((size_t)ZI_HIST, sizeof(ZiTouchDoc));
        if (!merged_docs) {
            zi_zimg_info_free(&info);
            return 0;
        }
    }
    for (i = 0; i < total - start && i < ZI_HIST; i++) {
        int src = start + i;

        if (src < keep) {
            merged[i] = info.hist[src];
            if (merged_docs && info.touch && src < ZI_HIST) {
                zi_touch_doc_copy(&merged_docs[i], &((const ZiTouchDoc *)info.touch)[src]);
            }
        } else {
            merged[i] = hist[src - keep + 1];
            if (merged_docs && src - keep + 1 < ZI_HIST) {
                zi_touch_doc_copy(&merged_docs[i], &docs[src - keep + 1]);
            }
        }
        out_n++;
    }
    zi_zimg_info_free(&info);
    *len = out_n;
    if (*len > ZI_HIST) {
        *len = ZI_HIST;
    }
    if (start > 0 && *len > 0) {
        wcscpy_s(merged[0].label, 40, L"Earlier edits");
    }
    if (npos >= *len) {
        npos = *len - 1;
    }
    memcpy(hist, merged, (size_t)(*len) * sizeof(ZiHistStep));
    *pos = npos;
    if (docs && merged_docs) {
        for (i = 0; i < ZI_HIST; i++) {
            if (i < *len) {
                zi_touch_doc_move(&docs[i], &merged_docs[i]);
            } else {
                zi_touch_doc_free(&docs[i]);
            }
        }
    }
    free(merged_docs);
    return 1;
}

static int zi_store_holding(const wchar_t *photo, const wchar_t *dest, int remember, const unsigned char digest[32],
                            unsigned long long byte_size, const ZiHistStep *hist, int len, int pos,
                            const ZiTouchDoc *docs, const unsigned char *preview, size_t preview_len, long serial) {
    ZiHistStep local[ZI_HIST];
    ZiTouchDoc *local_docs = NULL;
    unsigned char *file = NULL;
    size_t file_len = 0;
    int ok;
    int i;

    if (!photo || !photo[0] || !dest || !dest[0] || !digest || len < 1 || len > ZI_HIST) {
        return 0;
    }
    if (!zi_claim(photo, serial)) {
        return 1;
    }
    memcpy(local, hist, (size_t)len * sizeof(ZiHistStep));
    if (docs) {
        local_docs = (ZiTouchDoc *)calloc((size_t)ZI_HIST, sizeof(ZiTouchDoc));
        if (!local_docs) {
            return 0;
        }
        for (i = 0; i < len; i++) {
            zi_touch_doc_copy(&local_docs[i], &docs[i]);
        }
    }
    if (!zi_prepare_hist(dest, digest, byte_size, local, &len, &pos, local_docs)) {
        zi_docs_release(local_docs);
        return 1;
    }
    if (!zi_build_document(photo, digest, byte_size, local, len, pos, local_docs, preview, preview_len, &file,
                           &file_len)) {
        zi_docs_release(local_docs);
        return 0;
    }
    ok = zi_write_bytes(dest, file, file_len);
    free(file);
    zi_docs_release(local_docs);
    if (ok && remember) {
        zi_write_ref(digest, dest);
    }
    return ok;
}

int zi_zimg_write_at(const wchar_t *photo, const wchar_t *dest, const unsigned char digest[32],
                     unsigned long long byte_size, const ZiHistStep *hist, int len, int pos, const ZiTouchDoc *docs,
                     const unsigned char *preview, size_t preview_len, int remember, long serial) {
    int ok;

    zi_lock();
    ok = zi_store_holding(photo, dest, remember, digest, byte_size, hist, len, pos, docs, preview, preview_len, serial);
    zi_unlock();
    return ok;
}

int zi_zimg_write_hist(const wchar_t *photo, const unsigned char digest[32], unsigned long long byte_size,
                       const ZiHistStep *hist, int len, int pos, const ZiTouchDoc *docs,
                       const unsigned char *preview, size_t preview_len, long serial) {
    wchar_t dest[MAX_PATH];

    if (!digest || !zi_active_dest(digest, dest, MAX_PATH)) {
        return 0;
    }
    return zi_zimg_write_at(photo, dest, digest, byte_size, hist, len, pos, docs, preview, preview_len, 0, serial);
}

int zi_zimg_commit_tone(const wchar_t *photo, const ZiTone *tone, const wchar_t *label, long serial) {
    unsigned char digest[32];
    unsigned long long byte_size = 0;
    ZiHistStep hist[ZI_HIST];
    ZiTouchDoc *docs;
    int len = 0;
    int pos = 0;
    int ok = 0;

    if (!photo || !photo[0] || !tone || !zi_blake3_file(photo, digest, &byte_size)) {
        return 0;
    }
    docs = (ZiTouchDoc *)calloc((size_t)ZI_HIST, sizeof(ZiTouchDoc));
    if (!docs) {
        return 0;
    }
    zi_lock();
    if (!zi_claim(photo, serial)) {
        zi_docs_release(docs);
        zi_unlock();
        return 1;
    }
    if (!zi_zimg_load_hist(photo, digest, byte_size, hist, ZI_HIST, &len, &pos, docs)) {
        if (zi_identity(tone)) {
            zi_docs_release(docs);
            zi_unlock();
            return 1;
        }
        zi_tone_identity(&hist[0].tone);
        wcscpy_s(hist[0].label, 40, L"Open");
        hist[1].tone = *tone;
        wcscpy_s(hist[1].label, 40, label && label[0] ? label : L"Edit");
        len = 2;
        pos = 1;
    } else if (pos >= 0 && pos < len && zi_tone_same(&hist[pos].tone, tone)) {
        zi_docs_release(docs);
        zi_unlock();
        return 1;
    } else {
        len = pos + 1;
        if (len < 1) {
            len = 1;
        }
        if (len >= ZI_HIST) {
            memmove(&hist[0], &hist[1], (size_t)(ZI_HIST - 1) * sizeof hist[0]);
            zi_touch_doc_free(&docs[0]);
            memmove(docs, docs + 1, (size_t)(ZI_HIST - 1) * sizeof(ZiTouchDoc));
            zi_touch_doc_clear(&docs[ZI_HIST - 1]);
            wcscpy_s(hist[0].label, 40, L"Earlier edits");
            len = ZI_HIST - 1;
        }
        hist[len].tone = *tone;
        wcscpy_s(hist[len].label, 40, label && label[0] ? label : L"Edit");
        if (len > 0) {
            zi_touch_doc_copy(&docs[len], &docs[len - 1]);
        }
        len++;
        pos = len - 1;
    }
    {
        wchar_t dest[MAX_PATH];

        if (!zi_active_dest(digest, dest, MAX_PATH)) {
            zi_docs_release(docs);
            zi_unlock();
            return 0;
        }
        ok = zi_store_holding(photo, dest, 0, digest, byte_size, hist, len, pos, docs, k_fixture, 4, serial);
    }
    zi_docs_release(docs);
    zi_unlock();
    return ok;
}
