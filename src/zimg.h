/* Mac .zimg envelope. fileRef only. The photograph is not embedded and the photo folder is not written. */
#ifndef ZIMAGEWIN_ZIMG_H
#define ZIMAGEWIN_ZIMG_H

#include "app.h"

#define ZI_ZIMG_HINT 1024

typedef struct ZiTouchDoc ZiTouchDoc;

typedef struct ZiZimgInfo {
    int kind;
    wchar_t path_hint[ZI_ZIMG_HINT];
    wchar_t relative_hint[ZI_ZIMG_HINT];
    char uti[80];
    unsigned char digest[32];
    int has_digest;
    unsigned long long byte_size;
    ZiHistStep hist[ZI_HIST];
    int hist_len;
    int hist_pos;
    unsigned char *embedded;
    size_t embedded_len;
    void *keep;
    /* Heap ZiTouchDoc[ZI_HIST]. NULL means no repairs were stored. */
    void *touch;
} ZiZimgInfo;

void zi_zimg_info_free(ZiZimgInfo *info);
int zi_zimg_parse(const wchar_t *path, ZiZimgInfo *info);
int zi_zimg_edits_dir(wchar_t *dest, size_t cap);

/* serial is a process-wide counter. An older serial does not replace a newer save. */
int zi_zimg_write_hist(const wchar_t *photo, const unsigned char digest[32], unsigned long long byte_size,
                       const ZiHistStep *hist, int len, int pos, const ZiTouchDoc *docs,
                       const unsigned char *preview, size_t preview_len, long serial);
int zi_zimg_write_at(const wchar_t *photo, const wchar_t *dest, const unsigned char digest[32],
                     unsigned long long byte_size, const ZiHistStep *hist, int len, int pos,
                     const ZiTouchDoc *docs, const unsigned char *preview, size_t preview_len,
                     int remember, long serial);
int zi_zimg_load_hist(const wchar_t *photo, const unsigned char digest[32], unsigned long long byte_size,
                      ZiHistStep *hist, int cap, int *len, int *pos, ZiTouchDoc *docs);
int zi_zimg_commit_tone(const wchar_t *photo, const ZiTone *tone, const wchar_t *label, long serial);

#endif
