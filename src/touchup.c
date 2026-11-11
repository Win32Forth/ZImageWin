/* Dust and scratch repair, matching ZImager Touchup.swift.
   Samples are linear RGB, three floats, row 0 at the top. Object removal is absent. */
#include "touchup.h"

#include <wincodec.h>
#include <objbase.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZI_TOUCH_CACHE 4

typedef struct ZiBlob {
    int *pixels;
    int count;
    int cap;
    float peak;
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    float sum_x;
    float sum_y;
    float sum_xx;
    float sum_yy;
    float sum_xy;
} ZiBlob;

typedef struct ZiTouchSlot {
    int used;
    unsigned long long sig;
    int w;
    int h;
    char model[40];
    float *pix;
} ZiTouchSlot;

static INIT_ONCE g_touch_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_touch_lock;
static ZiTouchSlot g_slots[ZI_TOUCH_CACHE];
static int g_slot_next;

static BOOL CALLBACK zi_touch_init(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_touch_lock);
    return TRUE;
}

static void zi_touch_lock(void) {
    InitOnceExecuteOnce(&g_touch_once, zi_touch_init, NULL, NULL);
    EnterCriticalSection(&g_touch_lock);
}

static void zi_touch_unlock(void) {
    LeaveCriticalSection(&g_touch_lock);
}

static int zi_round_f(float value) {
    return (int)floorf(value + (value >= 0.0f ? 0.5f : -0.5f));
}

static int zi_pixels_ok(int w, int h) {
    if (w < 2 || h < 2 || w > 20000 || h > 20000) {
        return 0;
    }
    if ((size_t)w > (SIZE_MAX / 3) / (size_t)h) {
        return 0;
    }
    return 1;
}

static float zi_at(const float *pix, int index, int channel) {
    return pix[(size_t)index * 3 + (size_t)channel];
}

static void zi_put(float *pix, int index, float r, float g, float b) {
    pix[(size_t)index * 3] = r;
    pix[(size_t)index * 3 + 1] = g;
    pix[(size_t)index * 3 + 2] = b;
}

void zi_touch_doc_clear(ZiTouchDoc *doc) {
    if (doc) {
        memset(doc, 0, sizeof *doc);
    }
}

static void zi_mask_release(ZiTouchMask *mask) {
    if (mask && InterlockedDecrement((LONG *)&mask->refs) == 0) {
        free(mask->bytes);
        free(mask);
    }
}

void zi_touch_doc_free(ZiTouchDoc *doc) {
    int i;

    if (!doc) {
        return;
    }
    for (i = 0; i < doc->count && i < ZI_TOUCH_CAP; i++) {
        zi_mask_release(doc->ops[i].mask);
        doc->ops[i].mask = NULL;
    }
    memset(doc, 0, sizeof *doc);
}

void zi_touch_doc_copy(ZiTouchDoc *dest, const ZiTouchDoc *src) {
    ZiTouchDoc tmp;
    int i;
    int count;

    if (!dest) {
        return;
    }
    memset(&tmp, 0, sizeof tmp);
    if (src) {
        count = src->count;
        if (count < 0) {
            count = 0;
        }
        if (count > ZI_TOUCH_CAP) {
            count = ZI_TOUCH_CAP;
        }
        tmp.count = count;
        for (i = 0; i < count; i++) {
            tmp.ops[i] = src->ops[i];
            if (tmp.ops[i].mask) {
                InterlockedIncrement((LONG *)&tmp.ops[i].mask->refs);
            }
        }
    }
    if (dest != src) {
        zi_touch_doc_free(dest);
        *dest = tmp;
    }
}

void zi_touch_doc_move(ZiTouchDoc *dest, ZiTouchDoc *src) {
    if (!dest || !src || dest == src) {
        return;
    }
    zi_touch_doc_free(dest);
    *dest = *src;
    zi_touch_doc_clear(src);
}

static int zi_op_same(const ZiTouchOp *a, const ZiTouchOp *b, int strength) {
    size_t n;

    if (strcmp(a->id, b->id) != 0 || strcmp(a->model, b->model) != 0) {
        return 0;
    }
    if (a->mask == b->mask) {
        /* Shared mask. */
    } else if (!a->mask || !b->mask || a->mask->w != b->mask->w || a->mask->h != b->mask->h) {
        return 0;
    } else {
        n = (size_t)a->mask->w * (size_t)a->mask->h;
        if (memcmp(a->mask->bytes, b->mask->bytes, n) != 0) {
            return 0;
        }
    }
    if (fabsf(a->feather - b->feather) > 0.01f) {
        return 0;
    }
    if (strength && fabsf(a->strength - b->strength) > 0.0001f) {
        return 0;
    }
    return 1;
}

int zi_touch_doc_same(const ZiTouchDoc *a, const ZiTouchDoc *b) {
    int i;
    int ca = a ? a->count : 0;
    int cb = b ? b->count : 0;

    if (ca != cb) {
        return 0;
    }
    for (i = 0; i < ca; i++) {
        if (!zi_op_same(&a->ops[i], &b->ops[i], 1)) {
            return 0;
        }
    }
    return 1;
}

int zi_touch_doc_visible(const ZiTouchDoc *doc) {
    int i;

    if (!doc) {
        return 0;
    }
    for (i = 0; i < doc->count; i++) {
        if (doc->ops[i].mask && doc->ops[i].strength > 0.001f) {
            return 1;
        }
    }
    return 0;
}

int zi_touch_doc_active(const ZiTouchDoc *doc) {
    return doc && doc->count > 0;
}

void zi_touch_new_id(char dest[40]) {
    GUID id;
    static volatile LONG g_seq;

    if (!dest) {
        return;
    }
    if (CoCreateGuid(&id) == S_OK) {
        sprintf_s(dest, 40, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  id.Data1, id.Data2, id.Data3,
                  id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
                  id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
        return;
    }
    sprintf_s(dest, 40, "TOUCH-%08lX", (unsigned long)InterlockedIncrement(&g_seq));
}

int zi_touch_make(ZiTouchOp *op, const char *model, unsigned char *bytes, int w, int h) {
    ZiTouchMask *mask;

    if (!op) {
        free(bytes);
        return 0;
    }
    memset(op, 0, sizeof *op);
    if (!bytes || w < 1 || h < 1 || !model) {
        free(bytes);
        return 0;
    }
    mask = (ZiTouchMask *)calloc(1, sizeof *mask);
    if (!mask) {
        free(bytes);
        return 0;
    }
    mask->refs = 1;
    mask->w = w;
    mask->h = h;
    mask->bytes = bytes;
    zi_touch_new_id(op->id);
    strncpy_s(op->model, 40, model, _TRUNCATE);
    op->feather = 2.0f;
    op->strength = 1.0f;
    op->mask = mask;
    return 1;
}

int zi_touch_last_index(const ZiTouchDoc *doc, const char *model) {
    int i;
    int found = -1;

    if (!doc || !model) {
        return -1;
    }
    for (i = 0; i < doc->count; i++) {
        if (strcmp(doc->ops[i].model, model) == 0) {
            found = i;
        }
    }
    return found;
}

float zi_touch_last_strength(const ZiTouchDoc *doc, const char *model) {
    int index = zi_touch_last_index(doc, model);

    if (index < 0) {
        return 1.0f;
    }
    return doc->ops[index].strength;
}

void zi_touch_set_strength(ZiTouchDoc *doc, const char *model, float strength) {
    int index = zi_touch_last_index(doc, model);

    if (index < 0) {
        return;
    }
    if (strength < 0.0f) {
        strength = 0.0f;
    }
    if (strength > 1.0f) {
        strength = 1.0f;
    }
    doc->ops[index].strength = strength;
}

int zi_touch_one_added(const ZiTouchDoc *prev, const ZiTouchDoc *next) {
    int i;
    int pc = prev ? prev->count : 0;

    if (!next || next->count != pc + 1) {
        return 0;
    }
    for (i = 0; i < pc; i++) {
        if (!zi_op_same(&prev->ops[i], &next->ops[i], 1)) {
            return 0;
        }
    }
    return next->ops[pc].mask != NULL;
}

int zi_touch_one_strength(const ZiTouchDoc *prev, const ZiTouchDoc *next, int *index) {
    int i;
    int changed = -1;

    if (index) {
        *index = -1;
    }
    if (!prev || !next || prev->count != next->count) {
        return 0;
    }
    for (i = 0; i < prev->count; i++) {
        if (!zi_op_same(&prev->ops[i], &next->ops[i], 0)) {
            return 0;
        }
        if (fabsf(prev->ops[i].strength - next->ops[i].strength) > 0.0001f) {
            if (changed >= 0) {
                return 0;
            }
            changed = i;
        }
    }
    if (changed < 0) {
        return 0;
    }
    if (index) {
        *index = changed;
    }
    return 1;
}

static void zi_blobs_free(ZiBlob *blobs, int count) {
    int i;

    if (!blobs) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(blobs[i].pixels);
    }
    free(blobs);
}

static int zi_blob_add(ZiBlob *blob, int index) {
    if (blob->count >= blob->cap) {
        int next = blob->cap < 64 ? 64 : blob->cap * 2;
        int *grown = (int *)realloc(blob->pixels, (size_t)next * sizeof(int));

        if (!grown) {
            return 0;
        }
        blob->pixels = grown;
        blob->cap = next;
    }
    blob->pixels[blob->count++] = index;
    return 1;
}

static int zi_components(const unsigned char *mask, int w, int h, const float *response, ZiBlob **out, int *count) {
    size_t n = (size_t)w * (size_t)h;
    unsigned char *seen;
    ZiBlob *blobs = NULL;
    int blob_count = 0;
    int blob_cap = 0;
    size_t start;

    *out = NULL;
    *count = 0;
    seen = (unsigned char *)calloc(n, 1);
    if (!seen) {
        return 0;
    }
    for (start = 0; start < n; start++) {
        int *queue;
        int qcap;
        int qn;
        int head;
        ZiBlob blob;

        if (mask[start] == 0 || seen[start]) {
            continue;
        }
        memset(&blob, 0, sizeof blob);
        blob.min_x = w;
        blob.min_y = h;
        qcap = 64;
        queue = (int *)malloc((size_t)qcap * sizeof(int));
        if (!queue) {
            free(blob.pixels);
            free(seen);
            zi_blobs_free(blobs, blob_count);
            return 0;
        }
        queue[0] = (int)start;
        qn = 1;
        head = 0;
        seen[start] = 1;
        while (head < qn) {
            int index = queue[head++];
            int x = index % w;
            int y = index / w;
            int neighbors[4];
            int k;
            float fx;
            float fy;

            if (!zi_blob_add(&blob, index)) {
                free(queue);
                free(blob.pixels);
                free(seen);
                zi_blobs_free(blobs, blob_count);
                return 0;
            }
            if (response && blob.peak < response[index]) {
                blob.peak = response[index];
            }
            if (x < blob.min_x) {
                blob.min_x = x;
            }
            if (x > blob.max_x) {
                blob.max_x = x;
            }
            if (y < blob.min_y) {
                blob.min_y = y;
            }
            if (y > blob.max_y) {
                blob.max_y = y;
            }
            fx = (float)x;
            fy = (float)y;
            blob.sum_x += fx;
            blob.sum_y += fy;
            blob.sum_xx += fx * fx;
            blob.sum_yy += fy * fy;
            blob.sum_xy += fx * fy;
            neighbors[0] = x > 0 ? index - 1 : -1;
            neighbors[1] = x + 1 < w ? index + 1 : -1;
            neighbors[2] = y > 0 ? index - w : -1;
            neighbors[3] = y + 1 < h ? index + w : -1;
            for (k = 0; k < 4; k++) {
                int next = neighbors[k];

                if (next < 0 || mask[next] == 0 || seen[next]) {
                    continue;
                }
                seen[next] = 1;
                if (qn >= qcap) {
                    int ncap = qcap * 2;
                    int *grown = (int *)realloc(queue, (size_t)ncap * sizeof(int));

                    if (!grown) {
                        free(queue);
                        free(blob.pixels);
                        free(seen);
                        zi_blobs_free(blobs, blob_count);
                        return 0;
                    }
                    queue = grown;
                    qcap = ncap;
                }
                queue[qn++] = next;
            }
        }
        free(queue);
        if (blob_count >= blob_cap) {
            int ncap = blob_cap < 16 ? 16 : blob_cap * 2;
            ZiBlob *grown = (ZiBlob *)realloc(blobs, (size_t)ncap * sizeof(ZiBlob));

            if (!grown) {
                free(blob.pixels);
                free(seen);
                zi_blobs_free(blobs, blob_count);
                return 0;
            }
            blobs = grown;
            blob_cap = ncap;
        }
        blobs[blob_count++] = blob;
    }
    free(seen);
    *out = blobs;
    *count = blob_count;
    return 1;
}

static void zi_rank_line(const float *src, int src_stride, float *dst, int dst_stride, int n, int radius,
                         int minimum, int *dq) {
    int head = 0;
    int tail = 0;
    int last = -1;
    int x;

    if (n < 1) {
        return;
    }
    if (!dq || radius < 1) {
        for (x = 0; x < n; x++) {
            dst[(size_t)x * (size_t)dst_stride] = src[(size_t)x * (size_t)src_stride];
        }
        return;
    }
    for (x = 0; x < n; x++) {
        int left = x - radius;
        int right = x + radius;
        float front;

        if (left < 0) {
            left = 0;
        }
        if (right >= n) {
            right = n - 1;
        }
        while (head < tail && dq[head] < left) {
            head++;
        }
        while (last < right) {
            float value;

            last++;
            value = src[(size_t)last * (size_t)src_stride];
            if (minimum) {
                while (head < tail && src[(size_t)dq[tail - 1] * (size_t)src_stride] >= value) {
                    tail--;
                }
            } else {
                while (head < tail && src[(size_t)dq[tail - 1] * (size_t)src_stride] <= value) {
                    tail--;
                }
            }
            dq[tail++] = last;
        }
        front = src[(size_t)dq[head] * (size_t)src_stride];
        dst[(size_t)x * (size_t)dst_stride] = front;
    }
}

static float *zi_rank(const float *src, int w, int h, int radius, int minimum) {
    float *horizontal;
    float *output;
    int *dq;
    int span;
    int y;
    int x;
    size_t n = (size_t)w * (size_t)h;

    horizontal = (float *)malloc(n * sizeof(float));
    output = (float *)malloc(n * sizeof(float));
    span = w > h ? w : h;
    dq = (int *)malloc((size_t)span * sizeof(int));
    if (!horizontal || !output || !dq) {
        free(horizontal);
        free(output);
        free(dq);
        return NULL;
    }
    for (y = 0; y < h; y++) {
        zi_rank_line(src + (size_t)y * (size_t)w, 1, horizontal + (size_t)y * (size_t)w, 1, w, radius, minimum, dq);
    }
    for (x = 0; x < w; x++) {
        zi_rank_line(horizontal + x, w, output + x, w, h, radius, minimum, dq);
    }
    free(dq);
    free(horizontal);
    return output;
}

static float *zi_opening(const float *src, int w, int h, int radius) {
    float *eroded = zi_rank(src, w, h, radius, 1);
    float *opened;

    if (!eroded) {
        return NULL;
    }
    opened = zi_rank(eroded, w, h, radius, 0);
    free(eroded);
    return opened;
}

static float *zi_closing(const float *src, int w, int h, int radius) {
    float *dilated = zi_rank(src, w, h, radius, 0);
    float *closed;

    if (!dilated) {
        return NULL;
    }
    closed = zi_rank(dilated, w, h, radius, 1);
    free(dilated);
    return closed;
}

static float *zi_box(const float *src, int w, int h, int radius) {
    float *horizontal;
    float *output;
    int y;
    int x;
    size_t n = (size_t)w * (size_t)h;

    horizontal = (float *)malloc(n * sizeof(float));
    output = (float *)malloc(n * sizeof(float));
    if (!horizontal || !output) {
        free(horizontal);
        free(output);
        return NULL;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int sample;
            int end = x + radius;
            float sum = 0;
            float count = 0;

            if (end >= w) {
                end = w - 1;
            }
            for (sample = x - radius > 0 ? x - radius : 0; sample <= end; sample++) {
                sum += src[(size_t)y * (size_t)w + (size_t)sample];
                count += 1;
            }
            horizontal[(size_t)y * (size_t)w + (size_t)x] = sum / (count > 0 ? count : 1);
        }
    }
    for (x = 0; x < w; x++) {
        for (y = 0; y < h; y++) {
            int sample;
            int end = y + radius;
            float sum = 0;
            float count = 0;

            if (end >= h) {
                end = h - 1;
            }
            for (sample = y - radius > 0 ? y - radius : 0; sample <= end; sample++) {
                sum += horizontal[(size_t)sample * (size_t)w + (size_t)x];
                count += 1;
            }
            output[(size_t)y * (size_t)w + (size_t)x] = sum / (count > 0 ? count : 1);
        }
    }
    free(horizontal);
    return output;
}

static float *zi_luma(const float *rgb, int w, int h) {
    size_t n = (size_t)w * (size_t)h;
    float *luma = (float *)malloc(n * sizeof(float));
    size_t i;

    if (!luma) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        float linear = 0.2126f * fmaxf(zi_at(rgb, (int)i, 0), 0) +
                       0.7152f * fmaxf(zi_at(rgb, (int)i, 1), 0) +
                       0.0722f * fmaxf(zi_at(rgb, (int)i, 2), 0);
        luma[i] = powf(linear, 0.45f);
    }
    return luma;
}

static float zi_mean_sigma(const float *values, size_t count, float factor) {
    size_t index;
    int seen = 0;
    float mean = 0;
    float moment = 0;

    for (index = 0; index < count; index += 3) {
        float value = values[index];
        float delta;

        seen++;
        delta = value - mean;
        mean += delta / (float)seen;
        moment += delta * (value - mean);
    }
    if (seen <= 1) {
        return 0.055f;
    }
    return mean + factor * sqrtf(moment / (float)(seen - 1));
}

static int zi_blob_peak_desc(const void *va, const void *vb) {
    float a = ((const ZiBlob *)va)->peak;
    float b = ((const ZiBlob *)vb)->peak;

    if (a < b) {
        return 1;
    }
    if (a > b) {
        return -1;
    }
    return 0;
}

static unsigned char *zi_dilate_mask(const unsigned char *mask, int w, int h, int radius) {
    size_t n = (size_t)w * (size_t)h;
    unsigned char *output = (unsigned char *)calloc(n, 1);
    int y;

    if (!output) {
        return NULL;
    }
    if (radius <= 0) {
        memcpy(output, mask, n);
        return output;
    }
    for (y = 0; y < h; y++) {
        int x;

        for (x = 0; x < w; x++) {
            int y0 = y - radius;
            int y1 = y + radius;
            int x0 = x - radius;
            int x1 = x + radius;
            int yy;
            int on = 0;

            if (y0 < 0) {
                y0 = 0;
            }
            if (x0 < 0) {
                x0 = 0;
            }
            if (y1 >= h) {
                y1 = h - 1;
            }
            if (x1 >= w) {
                x1 = w - 1;
            }
            for (yy = y0; yy <= y1 && !on; yy++) {
                int xx;

                for (xx = x0; xx <= x1; xx++) {
                    if (mask[(size_t)yy * (size_t)w + (size_t)xx]) {
                        on = 1;
                        break;
                    }
                }
            }
            if (on) {
                output[(size_t)y * (size_t)w + (size_t)x] = 255;
            }
        }
    }
    return output;
}

static int zi_mask_any(const unsigned char *mask, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (mask[i] == 255) {
            return 1;
        }
    }
    return 0;
}

static unsigned char *zi_paint(ZiBlob *blobs, int count, int w, int h) {
    size_t n = (size_t)w * (size_t)h;
    unsigned char *mask = (unsigned char *)calloc(n, 1);
    unsigned char *grown;
    int i;

    if (!mask) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        int p;

        for (p = 0; p < blobs[i].count; p++) {
            mask[blobs[i].pixels[p]] = 255;
        }
    }
    grown = zi_dilate_mask(mask, w, h, 1);
    free(mask);
    if (!grown) {
        return NULL;
    }
    if (!zi_mask_any(grown, n)) {
        free(grown);
        return NULL;
    }
    return grown;
}

int zi_touch_dust_mask(const float *rgb, int w, int h, unsigned char **mask) {
    float *luma;
    float *response;
    float *guard = NULL;
    float *gradient = NULL;
    unsigned char *candidate = NULL;
    ZiBlob *blobs = NULL;
    int blob_count = 0;
    int radius;
    float scale;
    int max_area;
    int max_span;
    float threshold;
    size_t n;
    int y;
    int kept = 0;

    if (mask) {
        *mask = NULL;
    }
    if (!rgb || !mask || !zi_pixels_ok(w, h)) {
        return -1;
    }
    n = (size_t)w * (size_t)h;
    luma = zi_luma(rgb, w, h);
    response = (float *)calloc(n, sizeof(float));
    if (!luma || !response) {
        free(luma);
        free(response);
        return -1;
    }
    scale = (float)(w > h ? w : h) / 2560.0f;
    max_area = (int)(90.0f * scale * scale);
    if (max_area < 30) {
        max_area = 30;
    }
    max_span = (int)(18.0f * scale);
    if (max_span < 10) {
        max_span = 10;
    }
    for (radius = 2; radius <= 4; radius += 2) {
        float *opened = zi_opening(luma, w, h, radius);
        float *closed = zi_closing(luma, w, h, radius);
        size_t i;

        if (!opened || !closed) {
            free(opened);
            free(closed);
            free(luma);
            free(response);
            free(guard);
            return -1;
        }
        if (radius == 2) {
            guard = opened;
            opened = NULL;
        }
        for (i = 0; i < n; i++) {
            float bright = luma[i] - (opened ? opened[i] : guard[i]);
            float dark = closed[i] - luma[i];
            float best = bright > dark ? bright : dark;

            if (response[i] < best) {
                response[i] = best;
            }
        }
        free(opened);
        free(closed);
    }
    free(luma);
    gradient = (float *)calloc(n, sizeof(float));
    candidate = (unsigned char *)calloc(n, 1);
    if (!gradient || !candidate || !guard) {
        free(gradient);
        free(candidate);
        free(guard);
        free(response);
        return -1;
    }
    if (w > 2 && h > 2) {
        for (y = 1; y < h - 1; y++) {
            int x;

            for (x = 1; x < w - 1; x++) {
                size_t index = (size_t)y * (size_t)w + (size_t)x;

                gradient[index] = fabsf(guard[index + 1] - guard[index - 1]) +
                                  fabsf(guard[index + (size_t)w] - guard[index - (size_t)w]);
            }
        }
    }
    free(guard);
    for (y = 0; y < (int)n; y++) {
        if (gradient[y] > 0.18f) {
            response[y] = 0;
        }
    }
    free(gradient);
    threshold = zi_mean_sigma(response, n, 4.8f);
    if (threshold < 0.055f) {
        threshold = 0.055f;
    }
    for (y = 1; y < h - 1; y++) {
        int x;

        for (x = 1; x < w - 1; x++) {
            size_t index = (size_t)y * (size_t)w + (size_t)x;
            float value = response[index];

            if (value < threshold) {
                continue;
            }
            if (value < response[index - 1] || value < response[index + 1] ||
                value < response[index - (size_t)w] || value < response[index + (size_t)w]) {
                continue;
            }
            candidate[index] = 255;
        }
    }
    if (!zi_components(candidate, w, h, response, &blobs, &blob_count)) {
        free(candidate);
        free(response);
        return -1;
    }
    free(candidate);
    for (y = 0; y < blob_count; y++) {
        int span = blobs[y].max_x - blobs[y].min_x;
        int other = blobs[y].max_y - blobs[y].min_y;
        int short_side;
        float aspect;
        int drop = 0;

        if (other > span) {
            short_side = span;
            span = other;
        } else {
            short_side = other;
        }
        span += 1;
        short_side += 1;
        aspect = (float)span / (float)(short_side > 1 ? short_side : 1);
        if (blobs[y].count > max_area || span > max_span) {
            drop = 1;
        }
        if (aspect > 3.5f && blobs[y].count > 6) {
            drop = 1;
        }
        if (blobs[y].count == 1 && blobs[y].peak < threshold * 1.25f) {
            drop = 1;
        }
        if (drop) {
            free(blobs[y].pixels);
            blobs[y].pixels = NULL;
            blobs[y].count = 0;
        }
    }
    {
        int write = 0;

        for (y = 0; y < blob_count; y++) {
            if (blobs[y].count > 0) {
                if (write != y) {
                    blobs[write] = blobs[y];
                }
                write++;
            }
        }
        kept = write;
    }
    if (kept > 400) {
        qsort(blobs, (size_t)kept, sizeof(ZiBlob), zi_blob_peak_desc);
        for (y = 400; y < kept; y++) {
            free(blobs[y].pixels);
            blobs[y].pixels = NULL;
        }
        kept = 400;
    }
    free(response);
    if (kept < 1) {
        free(blobs);
        return 0;
    }
    *mask = zi_paint(blobs, kept, w, h);
    zi_blobs_free(blobs, kept);
    if (!*mask) {
        return -1;
    }
    return 1;
}

static float zi_sample_one(const float *luma, int w, int h, int x, int y, float dx, float dy, int distance) {
    int sx = zi_round_f((float)x + dx * (float)distance);
    int sy = zi_round_f((float)y + dy * (float)distance);

    if (sx < 0) {
        sx = 0;
    }
    if (sy < 0) {
        sy = 0;
    }
    if (sx >= w) {
        sx = w - 1;
    }
    if (sy >= h) {
        sy = h - 1;
    }
    return luma[(size_t)sy * (size_t)w + (size_t)sx];
}

static float zi_median3(float a, float b, float c) {
    if (a > b) {
        if (b > c) {
            return b;
        }
        return a > c ? c : a;
    }
    if (a > c) {
        return a;
    }
    return b > c ? c : b;
}

static void zi_elongation(const ZiBlob *blob, float *ratio, float *major, float *thickness) {
    float count = (float)(blob->count > 1 ? blob->count : 1);
    float mean_x = blob->sum_x / count;
    float mean_y = blob->sum_y / count;
    float cxx = blob->sum_xx / count - mean_x * mean_x;
    float cyy = blob->sum_yy / count - mean_y * mean_y;
    float cxy = blob->sum_xy / count - mean_x * mean_y;
    float half;
    float root;
    float minor;

    if (cxx < 0) {
        cxx = 0;
    }
    if (cyy < 0) {
        cyy = 0;
    }
    half = (cxx + cyy) * 0.5f;
    root = sqrtf(fmaxf(0, half * half - (cxx * cyy - cxy * cxy)));
    *major = half + root;
    minor = half - root;
    if (minor < 0) {
        minor = 0;
    }
    *thickness = minor;
    *ratio = *major / (minor > 0.25f ? minor : 0.25f);
}

static float *zi_downsample(const float *src, int w, int h, int factor, int dw, int dh) {
    float *output = (float *)malloc((size_t)dw * (size_t)dh * sizeof(float));
    int y;

    if (!output) {
        return NULL;
    }
    for (y = 0; y < dh; y++) {
        int x;

        for (x = 0; x < dw; x++) {
            float sum = 0;
            float count = 0;
            int dy;
            int dx;

            for (dy = 0; dy < factor; dy++) {
                for (dx = 0; dx < factor; dx++) {
                    int sx = x * factor + dx;
                    int sy = y * factor + dy;

                    if (sx >= w) {
                        sx = w - 1;
                    }
                    if (sy >= h) {
                        sy = h - 1;
                    }
                    sum += src[(size_t)sy * (size_t)w + (size_t)sx];
                    count += 1;
                }
            }
            output[(size_t)y * (size_t)dw + (size_t)x] = sum / (count > 0 ? count : 1);
        }
    }
    return output;
}

static float *zi_scratch_scores(const float *luma, int w, int h) {
    size_t n = (size_t)w * (size_t)h;
    float *scores = (float *)calloc(n, sizeof(float));
    float *blur;
    static const float k_dx[6] = { 1, 0.8660254f, 0.5f, 0, -0.5f, -0.8660254f };
    static const float k_dy[6] = { 0, 0.5f, 0.8660254f, 1, 0.8660254f, 0.5f };
    int margin = 16;
    int y;

    if (!scores) {
        return NULL;
    }
    blur = zi_box(luma, w, h, 2);
    if (!blur) {
        free(scores);
        return NULL;
    }
    if (w <= margin * 2 || h <= margin * 2) {
        free(blur);
        return scores;
    }
    for (y = margin; y < h - margin; y++) {
        int x;

        for (x = margin; x < w - margin; x++) {
            size_t index = (size_t)y * (size_t)w + (size_t)x;
            float center;
            float best = -1;
            float second = -1;
            int d;

            if (fabsf(luma[index] - blur[index]) < 0.035f) {
                continue;
            }
            center = luma[index];
            for (d = 0; d < 6; d++) {
                float along;
                float positive;
                float negative;
                float score;

                along = zi_median3(
                    zi_sample_one(luma, w, h, x, y, k_dx[d], k_dy[d], 3),
                    zi_sample_one(luma, w, h, x, y, k_dx[d], k_dy[d], 7),
                    zi_sample_one(luma, w, h, x, y, -k_dx[d], -k_dy[d], 7));
                positive = zi_median3(
                    zi_sample_one(luma, w, h, x, y, -k_dy[d], k_dx[d], 5),
                    zi_sample_one(luma, w, h, x, y, -k_dy[d], k_dx[d], 9),
                    zi_sample_one(luma, w, h, x, y, -k_dy[d], k_dx[d], 14));
                negative = zi_median3(
                    zi_sample_one(luma, w, h, x, y, k_dy[d], -k_dx[d], 5),
                    zi_sample_one(luma, w, h, x, y, k_dy[d], -k_dx[d], 9),
                    zi_sample_one(luma, w, h, x, y, k_dy[d], -k_dx[d], 14));
                if (fabsf(positive - negative) >= 0.12f) {
                    continue;
                }
                score = fabsf(center - (positive + negative) * 0.5f) - fabsf(center - along);
                if (score > best) {
                    second = best;
                    best = score;
                } else if (score > second) {
                    second = score;
                }
            }
            if (best > 0.05f && best > second + 0.015f) {
                scores[index] = best;
            }
        }
    }
    free(blur);
    return scores;
}

int zi_touch_scratch_mask(const float *rgb, int w, int h, unsigned char **mask) {
    float *luma;
    float *reduced = NULL;
    float *scores;
    unsigned char *candidate;
    unsigned char *full;
    unsigned char *grown;
    ZiBlob *blobs = NULL;
    int blob_count = 0;
    int factor;
    int dw;
    int dh;
    int min_count;
    float min_lambda;
    float threshold;
    int kept = 0;
    int i;
    size_t n;

    if (mask) {
        *mask = NULL;
    }
    if (!rgb || !mask || !zi_pixels_ok(w, h)) {
        return -1;
    }
    luma = zi_luma(rgb, w, h);
    if (!luma) {
        return -1;
    }
    factor = (w > h ? w : h) >= 700 ? 2 : 1;
    dw = w / factor;
    dh = h / factor;
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    if (factor == 1) {
        reduced = luma;
    } else {
        reduced = zi_downsample(luma, w, h, factor, dw, dh);
        if (!reduced) {
            free(luma);
            return -1;
        }
    }
    scores = zi_scratch_scores(reduced, dw, dh);
    if (factor != 1) {
        free(reduced);
    }
    free(luma);
    if (!scores) {
        return -1;
    }
    threshold = zi_mean_sigma(scores, (size_t)dw * (size_t)dh, 5.0f);
    if (threshold < 0.06f) {
        threshold = 0.06f;
    }
    candidate = (unsigned char *)calloc((size_t)dw * (size_t)dh, 1);
    if (!candidate) {
        free(scores);
        return -1;
    }
    for (i = 0; i < dw * dh; i++) {
        if (scores[i] >= threshold) {
            candidate[i] = 255;
        }
    }
    if (!zi_components(candidate, dw, dh, scores, &blobs, &blob_count)) {
        free(candidate);
        free(scores);
        return -1;
    }
    free(candidate);
    min_count = factor == 2 ? 8 : 14;
    min_lambda = factor == 2 ? 20.0f : 36.0f;
    for (i = 0; i < blob_count; i++) {
        float ratio = 0;
        float major = 0;
        float thickness = 0;
        int drop;

        zi_elongation(&blobs[i], &ratio, &major, &thickness);
        drop = blobs[i].count < min_count || major <= min_lambda || ratio < 6.0f || thickness > 4.0f;
        if (drop) {
            free(blobs[i].pixels);
            blobs[i].pixels = NULL;
            blobs[i].count = 0;
        }
    }
    {
        int write = 0;

        for (i = 0; i < blob_count; i++) {
            if (blobs[i].count > 0) {
                blobs[write++] = blobs[i];
            }
        }
        kept = write;
    }
    if (kept > 60) {
        qsort(blobs, (size_t)kept, sizeof(ZiBlob), zi_blob_peak_desc);
        for (i = 60; i < kept; i++) {
            free(blobs[i].pixels);
            blobs[i].pixels = NULL;
        }
        kept = 60;
    }
    free(scores);
    if (kept < 1) {
        free(blobs);
        return 0;
    }
    n = (size_t)w * (size_t)h;
    full = (unsigned char *)calloc(n, 1);
    if (!full) {
        zi_blobs_free(blobs, kept);
        return -1;
    }
    for (i = 0; i < kept; i++) {
        int p;

        for (p = 0; p < blobs[i].count; p++) {
            int index = blobs[i].pixels[p];
            int x = index % dw;
            int y = index / dw;
            int dy;
            int dx;

            for (dy = 0; dy < factor; dy++) {
                for (dx = 0; dx < factor; dx++) {
                    int fx = x * factor + dx;
                    int fy = y * factor + dy;

                    if (fx < w && fy < h) {
                        full[(size_t)fy * (size_t)w + (size_t)fx] = 255;
                    }
                }
            }
        }
    }
    zi_blobs_free(blobs, kept);
    grown = zi_dilate_mask(full, w, h, 1);
    free(full);
    if (!grown) {
        return -1;
    }
    if (!zi_mask_any(grown, n)) {
        free(grown);
        return 0;
    }
    *mask = grown;
    return 1;
}

static int zi_cmp_float(const void *va, const void *vb) {
    float a = *(const float *)va;
    float b = *(const float *)vb;

    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static int zi_float_add(float **data, int *count, int *cap, float value) {
    if (*count >= *cap) {
        int next = *cap < 64 ? 64 : *cap * 2;
        float *grown = (float *)realloc(*data, (size_t)next * sizeof(float));

        if (!grown) {
            return 0;
        }
        *data = grown;
        *cap = next;
    }
    (*data)[(*count)++] = value;
    return 1;
}

int zi_touch_spot_mask(const float *rgb, int w, int h, int x, int y, int radius, unsigned char **mask) {
    int outer;
    int step;
    float *reds = NULL;
    float *greens = NULL;
    float *blues = NULL;
    int samples = 0;
    int cap_r = 0;
    int cap_g = 0;
    int cap_b = 0;
    int sy;
    int sx;
    float median_r;
    float median_g;
    float median_b;
    float best_score = -1;
    int best_index;
    float seed_r;
    float seed_g;
    float seed_b;
    unsigned char *bits;
    int *queue;
    int qn;
    int qcap;
    int head;
    int count;
    int limit;
    float color_limit;
    float median_gate;
    long long reach;
    unsigned char *grown;
    size_t n;

    if (mask) {
        *mask = NULL;
    }
    if (!rgb || !mask || !zi_pixels_ok(w, h)) {
        return -1;
    }
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return 0;
    }
    if (radius < 1) {
        radius = 1;
    }
    outer = radius + (radius / 5 > 6 ? radius / 5 : 6);
    step = radius > 48 ? 3 : (radius > 16 ? 2 : 1);
    for (sy = y - outer > 0 ? y - outer : 0; sy <= (y + outer < h - 1 ? y + outer : h - 1); sy += step) {
        for (sx = x - outer > 0 ? x - outer : 0; sx <= (x + outer < w - 1 ? x + outer : w - 1); sx += step) {
            long long dx = (long long)sx - x;
            long long dy = (long long)sy - y;
            long long distance = dx * dx + dy * dy;
            int index;

            if (distance <= (long long)radius * radius || distance > (long long)outer * outer) {
                continue;
            }
            index = sy * w + sx;
            if (!zi_float_add(&reds, &samples, &cap_r, zi_at(rgb, index, 0))) {
                free(reds);
                free(greens);
                free(blues);
                return -1;
            }
            samples--;
            if (!zi_float_add(&greens, &samples, &cap_g, zi_at(rgb, index, 1))) {
                free(reds);
                free(greens);
                free(blues);
                return -1;
            }
            samples--;
            if (!zi_float_add(&blues, &samples, &cap_b, zi_at(rgb, index, 2))) {
                free(reds);
                free(greens);
                free(blues);
                return -1;
            }
        }
    }
    if (samples < 12) {
        int corner = w < h ? w : h;

        free(reds);
        free(greens);
        free(blues);
        reds = greens = blues = NULL;
        samples = 0;
        cap_r = 0;
        cap_g = 0;
        cap_b = 0;
        corner /= 2;
        if (corner > 8) {
            corner = 8;
        }
        if (corner < 1) {
            return 0;
        }
        for (sy = 0; sy < corner; sy++) {
            for (sx = 0; sx < corner; sx++) {
                int points[4];
                int p;

                points[0] = sy * w + sx;
                points[1] = sy * w + (w - 1 - sx);
                points[2] = (h - 1 - sy) * w + sx;
                points[3] = (h - 1 - sy) * w + (w - 1 - sx);
                for (p = 0; p < 4; p++) {
                    if (!zi_float_add(&reds, &samples, &cap_r, zi_at(rgb, points[p], 0))) {
                        free(reds);
                        free(greens);
                        free(blues);
                        return -1;
                    }
                    samples--;
                    if (!zi_float_add(&greens, &samples, &cap_g, zi_at(rgb, points[p], 1))) {
                        free(reds);
                        free(greens);
                        free(blues);
                        return -1;
                    }
                    samples--;
                    if (!zi_float_add(&blues, &samples, &cap_b, zi_at(rgb, points[p], 2))) {
                        free(reds);
                        free(greens);
                        free(blues);
                        return -1;
                    }
                }
            }
        }
    }
    if (samples < 1) {
        free(reds);
        free(greens);
        free(blues);
        return 0;
    }
    qsort(reds, (size_t)samples, sizeof(float), zi_cmp_float);
    qsort(greens, (size_t)samples, sizeof(float), zi_cmp_float);
    qsort(blues, (size_t)samples, sizeof(float), zi_cmp_float);
    median_r = reds[samples / 2];
    median_g = greens[samples / 2];
    median_b = blues[samples / 2];
    free(reds);
    free(greens);
    free(blues);
    best_index = y * w + x;
    for (sy = y - radius > 0 ? y - radius : 0; sy <= (y + radius < h - 1 ? y + radius : h - 1); sy++) {
        for (sx = x - radius > 0 ? x - radius : 0; sx <= (x + radius < w - 1 ? x + radius : w - 1); sx++) {
            long long dx = (long long)sx - x;
            long long dy = (long long)sy - y;
            int index;
            float dr;
            float dg;
            float db;
            float score;

            if (dx * dx + dy * dy > (long long)radius * radius) {
                continue;
            }
            index = sy * w + sx;
            dr = zi_at(rgb, index, 0) - median_r;
            dg = zi_at(rgb, index, 1) - median_g;
            db = zi_at(rgb, index, 2) - median_b;
            score = sqrtf(dr * dr + dg * dg + db * db);
            if (score > best_score) {
                best_score = score;
                best_index = index;
            }
        }
    }
    if (best_score <= 0.05f) {
        return 0;
    }
    seed_r = zi_at(rgb, best_index, 0);
    seed_g = zi_at(rgb, best_index, 1);
    seed_b = zi_at(rgb, best_index, 2);
    n = (size_t)w * (size_t)h;
    bits = (unsigned char *)calloc(n, 1);
    qcap = 256;
    queue = (int *)malloc((size_t)qcap * sizeof(int));
    if (!bits || !queue) {
        free(bits);
        free(queue);
        return -1;
    }
    bits[best_index] = 255;
    queue[0] = best_index;
    qn = 1;
    head = 0;
    count = 1;
    {
        long long area = (long long)radius * radius * 3;

        limit = 250000;
        if (area < limit) {
            limit = (int)area;
        }
        if (limit < 160) {
            limit = 160;
        }
    }
    color_limit = best_score * 0.85f;
    if (color_limit < 0.16f) {
        color_limit = 0.16f;
    }
    median_gate = best_score * 0.28f;
    if (median_gate < 0.045f) {
        median_gate = 0.045f;
    }
    reach = (long long)radius * radius;
    while (head < qn && count < limit) {
        int current = queue[head++];
        int cx = current % w;
        int cy = current / w;
        int k;

        for (k = 0; k < 4; k++) {
            int nx = cx + (k == 0 ? -1 : (k == 1 ? 1 : 0));
            int ny = cy + (k == 2 ? -1 : (k == 3 ? 1 : 0));
            int next;
            long long dx;
            long long dy;
            float dr;
            float dg;
            float db;
            float distance;
            float from_median;

            if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                continue;
            }
            next = ny * w + nx;
            if (bits[next]) {
                continue;
            }
            dx = (long long)nx - x;
            dy = (long long)ny - y;
            if (dx * dx + dy * dy > reach) {
                continue;
            }
            dr = zi_at(rgb, next, 0) - seed_r;
            dg = zi_at(rgb, next, 1) - seed_g;
            db = zi_at(rgb, next, 2) - seed_b;
            distance = sqrtf(dr * dr + dg * dg + db * db);
            dr = zi_at(rgb, next, 0) - median_r;
            dg = zi_at(rgb, next, 1) - median_g;
            db = zi_at(rgb, next, 2) - median_b;
            from_median = sqrtf(dr * dr + dg * dg + db * db);
            if (distance >= color_limit || from_median <= median_gate) {
                continue;
            }
            bits[next] = 255;
            count++;
            if (qn >= qcap) {
                int ncap = qcap * 2;
                int *grown_q = (int *)realloc(queue, (size_t)ncap * sizeof(int));

                if (!grown_q) {
                    free(queue);
                    free(bits);
                    return -1;
                }
                queue = grown_q;
                qcap = ncap;
            }
            queue[qn++] = next;
        }
    }
    free(queue);
    grown = zi_dilate_mask(bits, w, h, 1);
    free(bits);
    if (!grown) {
        return -1;
    }
    if (!zi_mask_any(grown, n)) {
        free(grown);
        return 0;
    }
    *mask = grown;
    return 1;
}

static void zi_stamp_disk(unsigned char *mask, int w, int h, int x, int y, int radius) {
    int y0 = y - radius;
    int y1 = y + radius;
    int x0 = x - radius;
    int x1 = x + radius;
    int yy;
    long long limit = (long long)radius * radius + radius;

    if (y0 < 0) {
        y0 = 0;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y1 >= h) {
        y1 = h - 1;
    }
    if (x1 >= w) {
        x1 = w - 1;
    }
    for (yy = y0; yy <= y1; yy++) {
        int xx;

        for (xx = x0; xx <= x1; xx++) {
            long long dx = (long long)xx - x;
            long long dy = (long long)yy - y;

            if (dx * dx + dy * dy <= limit) {
                mask[(size_t)yy * (size_t)w + (size_t)xx] = 255;
            }
        }
    }
}

int zi_touch_stroke_mask(int w, int h, const int *xs, const int *ys, int count, int radius, unsigned char **mask) {
    int *rx;
    int *ry;
    int rn = 0;
    int i;
    float covered = 0;
    int spacing;
    unsigned char *bits;
    size_t n;

    if (mask) {
        *mask = NULL;
    }
    if (!mask || !xs || !ys || count < 2 || w < 2 || h < 2) {
        return 0;
    }
    if (radius < 1) {
        radius = 1;
    }
    rx = (int *)malloc((size_t)count * sizeof(int));
    ry = (int *)malloc((size_t)count * sizeof(int));
    if (!rx || !ry) {
        free(rx);
        free(ry);
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (xs[i] < 0 || ys[i] < 0 || xs[i] >= w || ys[i] >= h) {
            continue;
        }
        if (rn > 0 && rx[rn - 1] == xs[i] && ry[rn - 1] == ys[i]) {
            continue;
        }
        rx[rn] = xs[i];
        ry[rn] = ys[i];
        rn++;
    }
    if (rn < 2) {
        free(rx);
        free(ry);
        return 0;
    }
    n = (size_t)w * (size_t)h;
    bits = (unsigned char *)calloc(n, 1);
    if (!bits) {
        free(rx);
        free(ry);
        return -1;
    }
    spacing = radius / 2;
    if (spacing < 1) {
        spacing = 1;
    }
    for (i = 1; i < rn; i++) {
        float dx = (float)(rx[i] - rx[i - 1]);
        float dy = (float)(ry[i] - ry[i - 1]);
        float length = sqrtf(dx * dx + dy * dy);
        int steps;
        int step;

        if (length < 0.5f) {
            continue;
        }
        covered += length;
        steps = (int)ceilf(length / (float)spacing);
        if (steps < 1) {
            steps = 1;
        }
        for (step = 0; step <= steps; step++) {
            float t = (float)step / (float)steps;

            zi_stamp_disk(bits, w, h,
                          zi_round_f((float)rx[i - 1] + dx * t),
                          zi_round_f((float)ry[i - 1] + dy * t),
                          radius);
        }
    }
    free(rx);
    free(ry);
    if (covered < 2.0f || !zi_mask_any(bits, n)) {
        free(bits);
        return 0;
    }
    *mask = bits;
    return 1;
}

static float *zi_box_rgb(const float *samples, int w, int h, int radius) {
    size_t n = (size_t)w * (size_t)h;
    float *plane = (float *)malloc(n * sizeof(float));
    float *output = (float *)malloc(n * 3 * sizeof(float));
    int channel;

    if (!plane || !output) {
        free(plane);
        free(output);
        return NULL;
    }
    for (channel = 0; channel < 3; channel++) {
        float *blurred;
        size_t i;

        for (i = 0; i < n; i++) {
            plane[i] = samples[i * 3 + (size_t)channel];
        }
        blurred = zi_box(plane, w, h, radius);
        if (!blurred) {
            free(plane);
            free(output);
            return NULL;
        }
        for (i = 0; i < n; i++) {
            output[i * 3 + (size_t)channel] = blurred[i];
        }
        free(blurred);
    }
    free(plane);
    return output;
}

static float *zi_blur_mask(const unsigned char *mask, int w, int h, int radius) {
    size_t n = (size_t)w * (size_t)h;
    float *source = (float *)calloc(n, sizeof(float));
    float *blurred;
    size_t i;

    if (!source) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (mask[i]) {
            source[i] = 1.0f;
        }
    }
    blurred = zi_box(source, w, h, radius);
    free(source);
    return blurred;
}

static int *zi_boundary(const unsigned char *hole, const ZiBlob *blob, int w, int h, int *count) {
    int *found = (int *)malloc(240 * sizeof(int));
    int n = 0;
    int y0 = blob->min_y - 3;
    int y1 = blob->max_y + 3;
    int x0 = blob->min_x - 3;
    int x1 = blob->max_x + 3;
    int y;

    *count = 0;
    if (!found) {
        return NULL;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y1 >= h) {
        y1 = h - 1;
    }
    if (x1 >= w) {
        x1 = w - 1;
    }
    for (y = y0; y <= y1; y++) {
        int x;

        for (x = x0; x <= x1; x++) {
            int index = y * w + x;
            int adjacent = 0;
            int yy0 = y - 2;
            int yy1 = y + 2;
            int yy;

            if (hole[index]) {
                continue;
            }
            if (yy0 < 0) {
                yy0 = 0;
            }
            if (yy1 >= h) {
                yy1 = h - 1;
            }
            for (yy = yy0; yy <= yy1 && !adjacent; yy++) {
                int xx0 = x - 2;
                int xx1 = x + 2;
                int xx;

                if (xx0 < 0) {
                    xx0 = 0;
                }
                if (xx1 >= w) {
                    xx1 = w - 1;
                }
                for (xx = xx0; xx <= xx1; xx++) {
                    if (hole[yy * w + xx]) {
                        adjacent = 1;
                        break;
                    }
                }
            }
            if (!adjacent) {
                continue;
            }
            found[n++] = index;
            if (n == 240) {
                *count = n;
                return found;
            }
        }
    }
    *count = n;
    return found;
}

typedef struct ZiRange {
    float r0;
    float r1;
    float g0;
    float g1;
    float b0;
    float b1;
} ZiRange;

static void zi_channel_range(const float *samples, const int *indexes, int count, ZiRange *range) {
    int i;

    range->r0 = 1.0f;
    range->r1 = 0.0f;
    range->g0 = 1.0f;
    range->g1 = 0.0f;
    range->b0 = 1.0f;
    range->b1 = 0.0f;
    for (i = 0; i < count; i++) {
        float r = zi_at(samples, indexes[i], 0);
        float g = zi_at(samples, indexes[i], 1);
        float b = zi_at(samples, indexes[i], 2);

        if (r < range->r0) {
            range->r0 = r;
        }
        if (r > range->r1) {
            range->r1 = r;
        }
        if (g < range->g0) {
            range->g0 = g;
        }
        if (g > range->g1) {
            range->g1 = g;
        }
        if (b < range->b0) {
            range->b0 = b;
        }
        if (b > range->b1) {
            range->b1 = b;
        }
    }
    range->r0 -= 0.06f;
    range->r1 += 0.06f;
    range->g0 -= 0.06f;
    range->g1 += 0.06f;
    range->b0 -= 0.06f;
    range->b1 += 0.06f;
}

static float zi_clamp_ch(float value, float lo, float hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static void zi_inverse_distance(const float *samples, int index, const int *boundary, int boundary_count, int w,
                                float *red, float *green, float *blue) {
    int x = index % w;
    int y = index / w;
    float weight = 0;
    int i;

    *red = 0;
    *green = 0;
    *blue = 0;
    for (i = 0; i < boundary_count; i++) {
        float dx = (float)(boundary[i] % w - x);
        float dy = (float)(boundary[i] / w - y);
        float value = 1.0f / (dx * dx + dy * dy + 0.35f);

        *red += zi_at(samples, boundary[i], 0) * value;
        *green += zi_at(samples, boundary[i], 1) * value;
        *blue += zi_at(samples, boundary[i], 2) * value;
        weight += value;
    }
    if (weight <= 0) {
        *red = zi_at(samples, index, 0);
        *green = zi_at(samples, index, 1);
        *blue = zi_at(samples, index, 2);
        return;
    }
    *red /= weight;
    *green /= weight;
    *blue /= weight;
}

static int zi_donor_offset(const unsigned char *hole, const ZiBlob *blob, const float *samples, const int *boundary,
                           int boundary_count, int w, int h, int *out_x, int *out_y) {
    int probes = blob->count < 100 ? blob->count : 100;
    int ring = boundary_count < 40 ? boundary_count : 40;
    float best = 1.0e30f;
    int found = 0;
    int oy;

    if (probes < 1 || ring < 1) {
        return 0;
    }
    for (oy = -30; oy <= 30; oy += 3) {
        int ox;

        for (ox = -30; ox <= 30; ox += 3) {
            int valid = 1;
            int p;
            float score = 0;
            float count = 0;

            if (ox == 0 && oy == 0) {
                continue;
            }
            for (p = 0; p < probes; p++) {
                int index = blob->pixels[p];
                int nx = index % w + ox;
                int ny = index / w + oy;

                if (nx < 0 || ny < 0 || nx >= w || ny >= h || hole[ny * w + nx]) {
                    valid = 0;
                    break;
                }
            }
            if (!valid) {
                continue;
            }
            for (p = 0; p < ring; p++) {
                int index = boundary[p];
                int nx = index % w + ox;
                int ny = index / w + oy;
                int source;

                if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                    continue;
                }
                source = ny * w + nx;
                score += fabsf(zi_at(samples, index, 0) - zi_at(samples, source, 0));
                score += fabsf(zi_at(samples, index, 1) - zi_at(samples, source, 1));
                score += fabsf(zi_at(samples, index, 2) - zi_at(samples, source, 2));
                count += 1;
            }
            if (count <= 0) {
                continue;
            }
            score = score / count + 0.002f * (float)(abs(ox) + abs(oy));
            if (score < best) {
                best = score;
                *out_x = ox;
                *out_y = oy;
                found = 1;
            }
        }
    }
    return found;
}

static void zi_fill_dust(float *filled, const float *samples, const float *high, const unsigned char *hole,
                         const ZiBlob *blob, int w, int h) {
    int boundary_count = 0;
    int *boundary = zi_boundary(hole, blob, w, h, &boundary_count);
    ZiRange range;
    int donor_x = 0;
    int donor_y = 0;
    int have_donor;
    int p;

    if (!boundary || boundary_count < 1) {
        free(boundary);
        return;
    }
    zi_channel_range(samples, boundary, boundary_count, &range);
    have_donor = zi_donor_offset(hole, blob, samples, boundary, boundary_count, w, h, &donor_x, &donor_y);
    for (p = 0; p < blob->count; p++) {
        int index = blob->pixels[p];
        float red;
        float green;
        float blue;

        zi_inverse_distance(samples, index, boundary, boundary_count, w, &red, &green, &blue);
        if (have_donor) {
            int x = index % w + donor_x;
            int y = index / w + donor_y;

            if (x >= 0 && y >= 0 && x < w && y < h) {
                int source = y * w + x;

                red += zi_at(high, source, 0);
                green += zi_at(high, source, 1);
                blue += zi_at(high, source, 2);
            }
        }
        zi_put(filled, index,
               zi_clamp_ch(red, range.r0, range.r1),
               zi_clamp_ch(green, range.g0, range.g1),
               zi_clamp_ch(blue, range.b0, range.b1));
    }
    free(boundary);
}

typedef struct ZiWalk {
    int found;
    int index;
    float distance;
} ZiWalk;

static ZiWalk zi_walk(const unsigned char *hole, int w, int h, int x, int y, float dx, float dy, int limit) {
    ZiWalk result;
    int step;

    result.found = 0;
    result.index = 0;
    result.distance = 0;
    if (limit < 1) {
        limit = 1;
    }
    for (step = 1; step <= limit; step++) {
        int nx = zi_round_f((float)x + dx * (float)step);
        int ny = zi_round_f((float)y + dy * (float)step);
        int index;

        if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
            return result;
        }
        index = ny * w + nx;
        if (hole[index] == 0) {
            result.found = 1;
            result.index = index;
            result.distance = (float)step;
            return result;
        }
    }
    return result;
}

static int zi_grain(const float *high, const unsigned char *hole, int w, int h, int index, float along_x, float along_y,
                    float *red, float *green, float *blue) {
    static const int distances[4] = {7, 11, 4, 0};
    int x = index % w;
    int y = index / w;
    int d;

    for (d = 0; d < 4; d++) {
        int nx = zi_round_f((float)x + along_x * (float)distances[d]);
        int ny = zi_round_f((float)y + along_y * (float)distances[d]);
        int sample;

        if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
            continue;
        }
        sample = ny * w + nx;
        if (hole[sample]) {
            continue;
        }
        *red = zi_at(high, sample, 0);
        *green = zi_at(high, sample, 1);
        *blue = zi_at(high, sample, 2);
        return 1;
    }
    return 0;
}

static void zi_fill_scratch(float *filled, const float *samples, const float *high, const unsigned char *hole,
                            const ZiBlob *blob, int w, int h) {
    int boundary_count = 0;
    int *boundary = zi_boundary(hole, blob, w, h, &boundary_count);
    ZiRange range;
    int span = blob->max_x - blob->min_x;
    int other = blob->max_y - blob->min_y;
    int limit;
    int p;
    static const float dirs[4][2] = {
        {1.0f, 0.0f},
        {0.70710678f, 0.70710678f},
        {0.0f, 1.0f},
        {-0.70710678f, 0.70710678f},
    };

    if (other > span) {
        span = other;
    }
    span += 8;
    if (span < 24) {
        span = 24;
    }
    if (span > 512) {
        span = 512;
    }
    limit = span;
    if (boundary && boundary_count > 0) {
        zi_channel_range(samples, boundary, boundary_count, &range);
    } else {
        zi_channel_range(samples, blob->pixels, blob->count, &range);
    }
    for (p = 0; p < blob->count; p++) {
        int index = blob->pixels[p];
        int x = index % w;
        int y = index / w;
        int best_score = 2147483647;
        ZiWalk positive = {0};
        ZiWalk negative = {0};
        float along_x = 1;
        float along_y = 0;
        float red = zi_at(samples, index, 0);
        float green = zi_at(samples, index, 1);
        float blue = zi_at(samples, index, 2);
        int d;

        for (d = 0; d < 4; d++) {
            ZiWalk forward = zi_walk(hole, w, h, x, y, dirs[d][0], dirs[d][1], limit);
            ZiWalk backward = zi_walk(hole, w, h, x, y, -dirs[d][0], -dirs[d][1], limit);
            int missing = (forward.found ? 0 : 1) + (backward.found ? 0 : 1);
            float distance = (forward.found ? forward.distance : (float)limit) +
                             (backward.found ? backward.distance : (float)limit);
            int score = missing * 10000 + (int)distance;

            if (score < best_score) {
                best_score = score;
                positive = forward;
                negative = backward;
                along_x = -dirs[d][1];
                along_y = dirs[d][0];
            }
        }
        if (positive.found && negative.found) {
            float t = positive.distance / (positive.distance + negative.distance);
            float gr;
            float gg;
            float gb;
            float or_;
            float og;
            float ob;

            red = zi_at(samples, positive.index, 0) * (1.0f - t) + zi_at(samples, negative.index, 0) * t;
            green = zi_at(samples, positive.index, 1) * (1.0f - t) + zi_at(samples, negative.index, 1) * t;
            blue = zi_at(samples, positive.index, 2) * (1.0f - t) + zi_at(samples, negative.index, 2) * t;
            if (zi_grain(high, hole, w, h, positive.index, along_x, along_y, &gr, &gg, &gb) &&
                zi_grain(high, hole, w, h, negative.index, along_x, along_y, &or_, &og, &ob)) {
                red += (gr + or_) * 0.5f;
                green += (gg + og) * 0.5f;
                blue += (gb + ob) * 0.5f;
            }
        } else if (positive.found || negative.found) {
            ZiWalk side = positive.found ? positive : negative;
            float gr;
            float gg;
            float gb;

            red = zi_at(samples, side.index, 0);
            green = zi_at(samples, side.index, 1);
            blue = zi_at(samples, side.index, 2);
            if (zi_grain(high, hole, w, h, side.index, along_x, along_y, &gr, &gg, &gb)) {
                red += gr;
                green += gg;
                blue += gb;
            }
        }
        zi_put(filled, index,
               zi_clamp_ch(red, range.r0, range.r1),
               zi_clamp_ch(green, range.g0, range.g1),
               zi_clamp_ch(blue, range.b0, range.b1));
    }
    free(boundary);
}

/* Feather radius follows the model: 3 for scratches, 1 for dust. Strength is applied later. */
static float *zi_repair(const float *samples, const unsigned char *mask, int w, int h, int scratch) {
    size_t n = (size_t)w * (size_t)h;
    unsigned char *hole = NULL;
    float *alpha = NULL;
    float *filled = NULL;
    float *low = NULL;
    float *high = NULL;
    float *output = NULL;
    ZiBlob *blobs = NULL;
    int blob_count = 0;
    size_t i;

    hole = zi_dilate_mask(mask, w, h, 1);
    alpha = zi_blur_mask(mask, w, h, scratch ? 3 : 1);
    filled = (float *)malloc(n * 3 * sizeof(float));
    if (!hole || !alpha || !filled) {
        goto fail;
    }
    memcpy(filled, samples, n * 3 * sizeof(float));
    if (scratch) {
        for (i = 0; i < n; i++) {
            if (mask[i]) {
                alpha[i] = 1.0f;
            }
        }
    }
    low = zi_box_rgb(samples, w, h, 2);
    high = (float *)malloc(n * 3 * sizeof(float));
    output = (float *)malloc(n * 3 * sizeof(float));
    if (!low || !high || !output) {
        goto fail;
    }
    for (i = 0; i < n; i++) {
        if (hole[i] == 0) {
            high[i * 3] = samples[i * 3] - low[i * 3];
            high[i * 3 + 1] = samples[i * 3 + 1] - low[i * 3 + 1];
            high[i * 3 + 2] = samples[i * 3 + 2] - low[i * 3 + 2];
        } else {
            high[i * 3] = 0;
            high[i * 3 + 1] = 0;
            high[i * 3 + 2] = 0;
        }
    }
    if (!zi_components(hole, w, h, alpha, &blobs, &blob_count)) {
        goto fail;
    }
    for (i = 0; i < (size_t)blob_count; i++) {
        if (blobs[i].count < 1) {
            continue;
        }
        if (scratch) {
            zi_fill_scratch(filled, samples, high, hole, &blobs[i], w, h);
        } else {
            zi_fill_dust(filled, samples, high, hole, &blobs[i], w, h);
        }
    }
    memcpy(output, samples, n * 3 * sizeof(float));
    for (i = 0; i < n; i++) {
        float amount = alpha[i];
        float keep;

        if (amount <= 0) {
            continue;
        }
        keep = 1.0f - amount;
        output[i * 3] = samples[i * 3] * keep + filled[i * 3] * amount;
        output[i * 3 + 1] = samples[i * 3 + 1] * keep + filled[i * 3 + 1] * amount;
        output[i * 3 + 2] = samples[i * 3 + 2] * keep + filled[i * 3 + 2] * amount;
    }
    free(hole);
    free(alpha);
    free(filled);
    free(low);
    free(high);
    zi_blobs_free(blobs, blob_count);
    return output;
fail:
    free(hole);
    free(alpha);
    free(filled);
    free(low);
    free(high);
    free(output);
    zi_blobs_free(blobs, blob_count);
    return NULL;
}

static unsigned char *zi_mask_scaled(const unsigned char *src, int sw, int sh, int dw, int dh) {
    size_t n = (size_t)dw * (size_t)dh;
    unsigned char *out = (unsigned char *)malloc(n);
    int y;

    if (!out || sw < 1 || sh < 1) {
        free(out);
        return NULL;
    }
    for (y = 0; y < dh; y++) {
        int sy = (int)(((long long)y * sh) / dh);
        int x;

        if (sy >= sh) {
            sy = sh - 1;
        }
        for (x = 0; x < dw; x++) {
            int sx = (int)(((long long)x * sw) / dw);
            unsigned char value;

            if (sx >= sw) {
                sx = sw - 1;
            }
            value = src[(size_t)sy * (size_t)sw + (size_t)sx];
            out[(size_t)y * (size_t)dw + (size_t)x] = value > 16 ? 255 : 0;
        }
    }
    return out;
}

static unsigned long long zi_fnv(unsigned long long hash, const void *data, size_t len) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t i;

    for (i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static void zi_dissolve(float *dst, const float *repaired, size_t n, float strength) {
    size_t i;

    if (strength >= 0.999f) {
        memcpy(dst, repaired, n * 3 * sizeof(float));
        return;
    }
    for (i = 0; i < n; i++) {
        float keep = 1.0f - strength;

        dst[i * 3] = dst[i * 3] * keep + repaired[i * 3] * strength;
        dst[i * 3 + 1] = dst[i * 3 + 1] * keep + repaired[i * 3 + 1] * strength;
        dst[i * 3 + 2] = dst[i * 3 + 2] * keep + repaired[i * 3 + 2] * strength;
    }
}

static int zi_slot_find(unsigned long long sig, int w, int h) {
    int i;

    for (i = 0; i < ZI_TOUCH_CACHE; i++) {
        if (g_slots[i].used && g_slots[i].sig == sig && g_slots[i].w == w && g_slots[i].h == h && g_slots[i].pix) {
            return i;
        }
    }
    return -1;
}

static float *zi_slot_copy(int slot, size_t n) {
    float *copy = (float *)malloc(n * 3 * sizeof(float));

    if (!copy) {
        return NULL;
    }
    memcpy(copy, g_slots[slot].pix, n * 3 * sizeof(float));
    return copy;
}

static void zi_slot_store(unsigned long long sig, int w, int h, const float *pix, size_t n) {
    int slot = g_slot_next % ZI_TOUCH_CACHE;
    float *copy = (float *)malloc(n * 3 * sizeof(float));

    if (!copy) {
        return;
    }
    memcpy(copy, pix, n * 3 * sizeof(float));
    free(g_slots[slot].pix);
    g_slots[slot].pix = copy;
    g_slots[slot].used = 1;
    g_slots[slot].sig = sig;
    g_slots[slot].w = w;
    g_slots[slot].h = h;
    strncpy_s(g_slots[slot].model, 40, "", _TRUNCATE);
    g_slot_next = (slot + 1) % ZI_TOUCH_CACHE;
}

void zi_touch_apply(float *rgb, int w, int h, const ZiTouchDoc *doc) {
    size_t n;
    int count;
    int index;
    int cacheable;
    unsigned long long chain = 14695981039346656037ull;
    int hashed = 0;

    if (!rgb || !doc || !zi_pixels_ok(w, h)) {
        return;
    }
    count = doc->count;
    if (count < 1) {
        return;
    }
    if (count > ZI_TOUCH_CAP) {
        count = ZI_TOUCH_CAP;
    }
    n = (size_t)w * (size_t)h;
    cacheable = n <= 2560ull * 2560ull;
    for (index = 0; index < count; index++) {
        const ZiTouchOp *op = &doc->ops[index];
        float strength;
        unsigned char *scaled = NULL;
        float *repaired = NULL;
        unsigned long long sig = 0;
        int scratch;

        if (!op->mask || !op->mask->bytes || op->mask->w < 1 || op->mask->h < 1) {
            continue;
        }
        strength = op->strength;
        if (strength < 0) {
            strength = 0;
        }
        if (strength > 1) {
            strength = 1;
        }
        if (strength <= 0.001f) {
            continue;
        }
        if (strcmp(op->model, ZI_TOUCH_DUST) != 0 && strcmp(op->model, ZI_TOUCH_SCRATCH) != 0) {
            continue;
        }
        if (cacheable) {
            if (!hashed) {
                chain = zi_fnv(chain, rgb, n * 3 * sizeof(float));
                hashed = 1;
            }
            sig = zi_fnv(chain, &w, sizeof w);
            sig = zi_fnv(sig, &h, sizeof h);
            sig = zi_fnv(sig, op->model, strlen(op->model));
            sig = zi_fnv(sig, op->mask->bytes, (size_t)op->mask->w * (size_t)op->mask->h);
            zi_touch_lock();
            {
                int slot = zi_slot_find(sig, w, h);

                if (slot >= 0) {
                    repaired = zi_slot_copy(slot, n);
                }
            }
            zi_touch_unlock();
        }
        if (!repaired) {
            scaled = zi_mask_scaled(op->mask->bytes, op->mask->w, op->mask->h, w, h);
            if (!scaled) {
                hashed = 0;
                chain = 14695981039346656037ull;
                continue;
            }
            scratch = strcmp(op->model, ZI_TOUCH_SCRATCH) == 0;
            repaired = zi_repair(rgb, scaled, w, h, scratch);
            free(scaled);
            if (!repaired) {
                hashed = 0;
                chain = 14695981039346656037ull;
                continue;
            }
            if (cacheable && hashed) {
                zi_touch_lock();
                zi_slot_store(sig, w, h, repaired, n);
                zi_touch_unlock();
            }
        }
        zi_dissolve(rgb, repaired, n, strength);
        free(repaired);
        if (hashed) {
            chain = zi_fnv(chain, &strength, sizeof strength);
            chain = zi_fnv(chain, &index, sizeof index);
        }
    }
}

static int zi_com_enter(int *leave) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    *leave = 0;
    if (hr == S_OK || hr == S_FALSE) {
        *leave = 1;
        return 1;
    }
    return hr == RPC_E_CHANGED_MODE;
}

static void zi_com_leave(int leave) {
    if (leave) {
        CoUninitialize();
    }
}

static int zi_factory(IWICImagingFactory **factory) {
    *factory = NULL;
    return SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                      &IID_IWICImagingFactory, (void **)factory)) &&
           *factory;
}

int zi_touch_mask_png(const ZiTouchMask *mask, unsigned char **png, size_t *png_len) {
    int leave = 0;
    IWICImagingFactory *factory = NULL;
    IStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    unsigned char *flipped = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppGray;
    STATSTG stat;
    LARGE_INTEGER zero;
    int w;
    int h;
    int stride;
    int y;
    ULONG got = 0;
    int ok = 0;

    if (png) {
        *png = NULL;
    }
    if (png_len) {
        *png_len = 0;
    }
    if (!mask || !mask->bytes || !png || !png_len || mask->w < 1 || mask->h < 1) {
        return 0;
    }
    if (!zi_com_enter(&leave) || !zi_factory(&factory)) {
        zi_com_leave(leave);
        return 0;
    }
    w = mask->w;
    h = mask->h;
    stride = (w + 3) & ~3;
    flipped = (unsigned char *)calloc((size_t)stride * (size_t)h, 1);
    if (!flipped || FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) || !stream ||
        FAILED(factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder)) || !encoder ||
        FAILED(encoder->lpVtbl->Initialize(encoder, stream, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, NULL)) || !frame) {
        goto done;
    }
    for (y = 0; y < h; y++) {
        memcpy(flipped + (size_t)y * (size_t)stride, mask->bytes + (size_t)(h - 1 - y) * (size_t)w, (size_t)w);
    }
    if (FAILED(frame->lpVtbl->Initialize(frame, NULL)) || FAILED(frame->lpVtbl->SetSize(frame, (UINT)w, (UINT)h))) {
        goto done;
    }
    if (FAILED(frame->lpVtbl->SetPixelFormat(frame, &format)) || !IsEqualGUID(&format, &GUID_WICPixelFormat8bppGray)) {
        goto done;
    }
    if (FAILED(frame->lpVtbl->WritePixels(frame, (UINT)h, (UINT)stride, (UINT)(stride * h), flipped)) ||
        FAILED(frame->lpVtbl->Commit(frame)) || FAILED(encoder->lpVtbl->Commit(encoder))) {
        goto done;
    }
    ZeroMemory(&stat, sizeof stat);
    if (FAILED(stream->lpVtbl->Stat(stream, &stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart <= 0 ||
        stat.cbSize.QuadPart > 32ll * 1024ll * 1024ll) {
        goto done;
    }
    *png = (unsigned char *)malloc((size_t)stat.cbSize.QuadPart);
    if (!*png) {
        goto done;
    }
    zero.QuadPart = 0;
    if (FAILED(stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL)) ||
        FAILED(stream->lpVtbl->Read(stream, *png, (ULONG)stat.cbSize.QuadPart, &got)) || got == 0) {
        free(*png);
        *png = NULL;
        goto done;
    }
    *png_len = got;
    ok = 1;
done:
    free(flipped);
    if (frame) {
        frame->lpVtbl->Release(frame);
    }
    if (encoder) {
        encoder->lpVtbl->Release(encoder);
    }
    if (stream) {
        stream->lpVtbl->Release(stream);
    }
    if (factory) {
        factory->lpVtbl->Release(factory);
    }
    zi_com_leave(leave);
    return ok;
}

int zi_touch_mask_from_png(const unsigned char *png, size_t png_len, ZiTouchMask **mask) {
    int leave = 0;
    HGLOBAL memory = NULL;
    void *locked = NULL;
    IStream *stream = NULL;
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    unsigned char *raw = NULL;
    ZiTouchMask *out = NULL;
    UINT w = 0;
    UINT h = 0;
    UINT stride;
    int y;
    int ok = 0;

    if (mask) {
        *mask = NULL;
    }
    if (!png || png_len < 8 || png_len > 32u * 1024u * 1024u || !mask) {
        return 0;
    }
    if (!zi_com_enter(&leave)) {
        return 0;
    }
    memory = GlobalAlloc(GMEM_MOVEABLE, png_len);
    if (!memory) {
        zi_com_leave(leave);
        return 0;
    }
    locked = GlobalLock(memory);
    if (!locked) {
        GlobalFree(memory);
        zi_com_leave(leave);
        return 0;
    }
    memcpy(locked, png, png_len);
    GlobalUnlock(memory);
    locked = NULL;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream)) || !stream) {
        GlobalFree(memory);
        zi_com_leave(leave);
        return 0;
    }
    memory = NULL;
    if (!zi_factory(&factory) ||
        FAILED(factory->lpVtbl->CreateDecoderFromStream(factory, stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        !decoder || FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame)) || !frame ||
        FAILED(frame->lpVtbl->GetSize(frame, &w, &h)) || w < 1 || h < 1 || w > 20000 || h > 20000) {
        goto done;
    }
    if ((size_t)w > (SIZE_MAX / (size_t)h)) {
        goto done;
    }
    stride = (w + 3u) & ~3u;
    raw = (unsigned char *)malloc((size_t)stride * (size_t)h);
    out = (ZiTouchMask *)calloc(1, sizeof *out);
    if (!raw || !out || FAILED(factory->lpVtbl->CreateFormatConverter(factory, &converter)) || !converter ||
        FAILED(converter->lpVtbl->Initialize(converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat8bppGray,
                                             WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->lpVtbl->CopyPixels(converter, NULL, stride, stride * h, raw))) {
        goto done;
    }
    out->bytes = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!out->bytes) {
        goto done;
    }
    out->refs = 1;
    out->w = (int)w;
    out->h = (int)h;
    for (y = 0; y < (int)h; y++) {
        int x;
        const unsigned char *row = raw + (size_t)((int)h - 1 - y) * stride;

        for (x = 0; x < (int)w; x++) {
            out->bytes[(size_t)y * w + (size_t)x] = row[x] > 16 ? 255 : 0;
        }
    }
    *mask = out;
    out = NULL;
    ok = 1;
done:
    if (out) {
        free(out->bytes);
        free(out);
    }
    free(raw);
    if (converter) {
        converter->lpVtbl->Release(converter);
    }
    if (frame) {
        frame->lpVtbl->Release(frame);
    }
    if (decoder) {
        decoder->lpVtbl->Release(decoder);
    }
    if (factory) {
        factory->lpVtbl->Release(factory);
    }
    if (stream) {
        stream->lpVtbl->Release(stream);
    }
    if (memory) {
        GlobalFree(memory);
    }
    zi_com_leave(leave);
    return ok;
}

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *zi_touch_base64(const unsigned char *data, size_t len) {
    size_t out_len;
    char *out;
    size_t i;
    size_t o = 0;

    if (!data && len) {
        return NULL;
    }
    out_len = ((len + 2) / 3) * 4;
    out = (char *)malloc(out_len + 1);
    if (!out) {
        return NULL;
    }
    for (i = 0; i < len; i += 3) {
        unsigned int value = (unsigned int)data[i] << 16;
        int remain = (int)(len - i);

        if (remain > 1) {
            value |= (unsigned int)data[i + 1] << 8;
        }
        if (remain > 2) {
            value |= data[i + 2];
        }
        out[o++] = k_b64[(value >> 18) & 63];
        out[o++] = k_b64[(value >> 12) & 63];
        out[o++] = remain > 1 ? k_b64[(value >> 6) & 63] : '=';
        out[o++] = remain > 2 ? k_b64[value & 63] : '=';
    }
    out[o] = 0;
    return out;
}

static int zi_b64_value(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

int zi_touch_base64_decode(const char *text, unsigned char **data, size_t *len) {
    size_t n;
    size_t i;
    size_t o = 0;
    int quad[4];
    int q = 0;
    unsigned char *out;

    if (data) {
        *data = NULL;
    }
    if (len) {
        *len = 0;
    }
    if (!text || !data || !len) {
        return 0;
    }
    n = strlen(text);
    out = (unsigned char *)malloc(n / 4 * 3 + 3);
    if (!out) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        int value;

        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            continue;
        }
        if (ch == '=') {
            value = 0;
        } else {
            value = zi_b64_value(ch);
            if (value < 0) {
                free(out);
                return 0;
            }
        }
        quad[q++] = ch == '=' ? -1 : value;
        if (q < 4) {
            continue;
        }
        if (quad[0] < 0 || quad[1] < 0) {
            free(out);
            return 0;
        }
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
        if (quad[2] >= 0) {
            out[o++] = (unsigned char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
        }
        if (quad[3] >= 0) {
            out[o++] = (unsigned char)(((quad[2] & 3) << 6) | quad[3]);
        }
        q = 0;
    }
    if (q != 0) {
        free(out);
        return 0;
    }
    *data = out;
    *len = o;
    return 1;
}

void zi_touch_shutdown(void) {
    int i;

    zi_touch_lock();
    for (i = 0; i < ZI_TOUCH_CACHE; i++) {
        free(g_slots[i].pix);
        g_slots[i].pix = NULL;
        g_slots[i].used = 0;
        g_slots[i].sig = 0;
    }
    g_slot_next = 0;
    zi_touch_unlock();
}
