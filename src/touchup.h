/* Dust and scratch repair. Object removal is not part of this build.
   Masks are photo-top-down, 0 or 255. The Mac PNG on disk is stored upside down. */
#ifndef ZIMAGEWIN_TOUCHUP_H
#define ZIMAGEWIN_TOUCHUP_H

#include <stddef.h>

#define ZI_TOUCH_CAP 64
#define ZI_TOUCH_DUST "touchup-dust-v1"
#define ZI_TOUCH_SCRATCH "touchup-scratch-v1"

typedef struct ZiTouchMask {
    volatile long refs;
    int w;
    int h;
    unsigned char *bytes;
} ZiTouchMask;

typedef struct ZiTouchOp {
    char id[40];
    char model[40];
    float feather;
    float strength;
    ZiTouchMask *mask;
} ZiTouchOp;

typedef struct ZiTouchDoc {
    ZiTouchOp ops[ZI_TOUCH_CAP];
    int count;
} ZiTouchDoc;

void zi_touch_doc_clear(ZiTouchDoc *doc);
void zi_touch_doc_free(ZiTouchDoc *doc);
void zi_touch_doc_copy(ZiTouchDoc *dest, const ZiTouchDoc *src);
void zi_touch_doc_move(ZiTouchDoc *dest, ZiTouchDoc *src);
int zi_touch_doc_same(const ZiTouchDoc *a, const ZiTouchDoc *b);
int zi_touch_doc_visible(const ZiTouchDoc *doc);
int zi_touch_doc_active(const ZiTouchDoc *doc);

void zi_touch_new_id(char dest[40]);
int zi_touch_make(ZiTouchOp *op, const char *model, unsigned char *mask, int w, int h);
int zi_touch_last_index(const ZiTouchDoc *doc, const char *model);
float zi_touch_last_strength(const ZiTouchDoc *doc, const char *model);
void zi_touch_set_strength(ZiTouchDoc *doc, const char *model, float strength);

/* 1 when next is prev plus one new op at the end. */
int zi_touch_one_added(const ZiTouchDoc *prev, const ZiTouchDoc *next);
/* 1 when the masks match and exactly one strength changed. Writes the op index. */
int zi_touch_one_strength(const ZiTouchDoc *prev, const ZiTouchDoc *next, int *index);

/* RGB float, row 0 at the top, three channels. NULL doc is a no-op. */
void zi_touch_apply(float *rgb, int w, int h, const ZiTouchDoc *doc);

/* 1 mask, 0 nothing stood out, -1 the removal did not run. */
int zi_touch_dust_mask(const float *rgb, int w, int h, unsigned char **mask);
int zi_touch_scratch_mask(const float *rgb, int w, int h, unsigned char **mask);
int zi_touch_spot_mask(const float *rgb, int w, int h, int x, int y, int radius, unsigned char **mask);
int zi_touch_stroke_mask(int w, int h, const int *xs, const int *ys, int count, int radius, unsigned char **mask);

/* Mac PNG: row 0 is the bottom of the photo. The returned mask is top-down. */
int zi_touch_mask_png(const ZiTouchMask *mask, unsigned char **png, size_t *png_len);
int zi_touch_mask_from_png(const unsigned char *png, size_t png_len, ZiTouchMask **mask);
char *zi_touch_base64(const unsigned char *data, size_t len);
int zi_touch_base64_decode(const char *text, unsigned char **data, size_t *len);

void zi_touch_shutdown(void);

#endif
