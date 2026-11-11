/* Edit view, Date column, export, and print. Modeled on the Mac editor in ZImage/ZImager.
   Grades are stored as a .zimg document. The original file is not modified. */
#include "editor.h"
#include "grid.h"
#include "library.h"
#include "tone.h"
#include "touchup.h"
#include "zimg.h"
#include "blake3.h"

#include <wincodec.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <oleauto.h>
#include <objbase.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG g_editor_alive = 1;
static volatile LONG g_photo_generation;
static volatile LONG g_fact_generation;
static volatile LONG g_grade_serial;
static volatile LONG g_grade_inflight;
static volatile LONG g_auto_serial;
static volatile LONG g_auto_inflight;
static volatile LONG g_drop_serial;
static volatile LONG g_drop_inflight;
static volatile LONG g_touch_inflight;

/* Spot clicks wait here while one removal runs. The count includes the one in progress. */
#define ZI_SPOT_Q ZI_TOUCH_CAP

typedef struct ZiSpotClick {
    LONG generation;
    int x;
    int y;
    int radius;
} ZiSpotClick;

static ZiSpotClick g_spot_q[ZI_SPOT_Q];
static int g_spot_q_len;
static int g_spot_running;

static void zi_grade_request(ZiApp *app);
static void zi_editor_refresh(ZiApp *app);
static int zi_photo_dest(const ZiApp *app, RECT *dest);
static int zi_view_point(const ZiApp *app, int x, int y, int *px, int *py);
static void zi_drop_begin(ZiApp *app, int x, int y);
static void zi_edits_note(ZiApp *app);
static void zi_queue_commit(const wchar_t *path, const ZiTone *tone, const wchar_t *label);
static void zi_queue_hash(ZiApp *app);

static int g_loading;
static int g_hold;
static int g_closed;
static ZiTouchDoc g_live;
static ZiTouchDoc g_steps[ZI_HIST];
static float g_amount_before;
static int g_amount_which;
static int g_doc_wait;
static wchar_t g_doc_path[MAX_PATH];
static unsigned char g_doc_digest[32];
static unsigned long long g_doc_size;
static ZiHistStep g_doc_hist[ZI_HIST];
static ZiTouchDoc g_doc_docs[ZI_HIST];
static int g_doc_len;
static int g_doc_pos;
static int g_export_format;

#define ZI_TURN_CAP 256
#define ZI_VIEW_EDGE 2560
#define ZI_EXPORT_EDGE 8192
/* 6928*6928 stays under the tone renderer's 48e6 pixel cap. */
#define ZI_GRADE_EDGE 6928
#define ZI_ZOOM_MAX 4.0
#define ZI_SQRT2 1.41421356237

typedef struct ZiMemo {
    wchar_t path[MAX_PATH];
    ZiTone tone;
} ZiMemo;

static ZiMemo g_memo[ZI_TURN_CAP];
static int g_memo_count;

static const wchar_t *const k_videos[] = {
    L"mov", L"mp4", L"m4v", L"m4p", L"avi", L"mkv", L"mpg", L"mpeg", L"mpe", L"m2v",
    L"wmv", L"flv", L"webm", L"3gp", L"3g2", L"mts", L"m2ts", L"ts", L"vob", L"ogv", L"asf", L"divx"
};

static int zi_is_video_name(const wchar_t *name) {
    const wchar_t *dot = wcsrchr(name, L'.');
    int i;

    if (!dot || dot[1] == L'\0') {
        return 0;
    }
    for (i = 0; i < (int)(sizeof k_videos / sizeof k_videos[0]); i++) {
        if (_wcsicmp(dot + 1, k_videos[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static ZiTone zi_edit_get(const wchar_t *path) {
    ZiTone tone;
    int i;

    zi_tone_identity(&tone);
    if (!path || !path[0]) {
        return tone;
    }
    for (i = 0; i < g_memo_count; i++) {
        if (_wcsicmp(g_memo[i].path, path) == 0) {
            return g_memo[i].tone;
        }
    }
    return tone;
}

static void zi_edit_set(const wchar_t *path, const ZiTone *tone) {
    int i;

    if (!path || !path[0] || !tone) {
        return;
    }
    for (i = 0; i < g_memo_count; i++) {
        if (_wcsicmp(g_memo[i].path, path) == 0) {
            g_memo[i].tone = *tone;
            g_memo[i].tone.turns &= 3;
            return;
        }
    }
    if (g_memo_count >= ZI_TURN_CAP) {
        i = 0;
    } else {
        i = g_memo_count++;
    }
    wcscpy_s(g_memo[i].path, MAX_PATH, path);
    g_memo[i].tone = *tone;
    g_memo[i].tone.turns &= 3;
}

static void zi_editor_notice(ZiApp *app, const wchar_t *text) {
    wcscpy_s(app->notice, 192, text);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

static void zi_editor_title(ZiApp *app) {
    const wchar_t *base;
    wchar_t title[MAX_PATH];

    if (!app->hwnd) {
        return;
    }
    if (app->photo_path[0]) {
        base = wcsrchr(app->photo_path, L'\\');
        base = (base && base[1]) ? base + 1 : app->photo_path;
        wcscpy_s(title, MAX_PATH, base);
    } else if (app->opened[0]) {
        base = wcsrchr(app->opened, L'\\');
        base = (base && base[1]) ? base + 1 : app->opened;
        wcscpy_s(title, MAX_PATH, base);
    } else {
        wcscpy_s(title, MAX_PATH, L"ZImager");
    }
    SetWindowTextW(app->hwnd, title);
}

static void zi_editor_show_image(ZiApp *app) {
    if (app->source_server) {
        app->server_folder = 0;
    } else {
        app->disk_folder = 0;
    }
}

static HBITMAP zi_dib_from_source(IWICImagingFactory *factory, IWICBitmapSource *source, UINT w, UINT h) {
    IWICFormatConverter *conv = NULL;
    BITMAPINFO info;
    void *bits = NULL;
    HBITMAP dib = NULL;
    UINT stride;
    HRESULT hr;

    if (w == 0 || h == 0 || w > 20000 || h > 20000) {
        return NULL;
    }
    if (FAILED(factory->lpVtbl->CreateFormatConverter(factory, &conv)) || !conv) {
        return NULL;
    }
    hr = conv->lpVtbl->Initialize(conv, source, &GUID_WICPixelFormat32bppBGRA,
                                  WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        conv->lpVtbl->Release(conv);
        return NULL;
    }
    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = (LONG)w;
    info.bmiHeader.biHeight = -(LONG)h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    stride = w * 4;
    if (!dib || !bits || FAILED(conv->lpVtbl->CopyPixels(conv, NULL, stride, stride * h, (BYTE *)bits))) {
        if (dib) {
            DeleteObject(dib);
            dib = NULL;
        }
    } else {
        BYTE *pixel = (BYTE *)bits;
        UINT n = stride * h;
        UINT i;

        for (i = 3; i < n; i += 4) {
            pixel[i] = 255;
        }
    }
    conv->lpVtbl->Release(conv);
    return dib;
}

static WICBitmapTransformOptions zi_exif_options(int orientation) {
    switch (orientation) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal;
    case 6: return WICBitmapTransformRotate90;
    case 7: return WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal;
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

static int zi_frame_orientation(IWICBitmapFrameDecode *frame) {
    IWICMetadataQueryReader *meta = NULL;
    PROPVARIANT value;
    int orientation = 1;

    PropVariantInit(&value);
    if (FAILED(frame->lpVtbl->GetMetadataQueryReader(frame, &meta)) || !meta) {
        return 1;
    }
    if (SUCCEEDED(meta->lpVtbl->GetMetadataByName(meta, L"/app1/ifd/{ushort=274}", &value))) {
        if (value.vt == VT_UI2) {
            orientation = value.uiVal;
        } else if (value.vt == VT_UI4) {
            orientation = (int)value.ulVal;
        }
    }
    PropVariantClear(&value);
    meta->lpVtbl->Release(meta);
    if (orientation < 1 || orientation > 8) {
        orientation = 1;
    }
    return orientation;
}

static HBITMAP zi_decode_source(IWICImagingFactory *factory, IWICBitmapSource *source, int orientation,
                                int max_edge, int turns, int *out_w, int *out_h) {
    IWICBitmapSource *current = source;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapFlipRotator *rotator = NULL;
    HBITMAP dib = NULL;
    UINT w = 0;
    UINT h = 0;
    UINT tw;
    UINT th;
    WICBitmapTransformOptions exif;
    WICBitmapTransformOptions user = WICBitmapTransformRotate0;

    source->lpVtbl->AddRef(source);
    if (FAILED(current->lpVtbl->GetSize(current, &w, &h)) || w == 0 || h == 0) {
        goto done;
    }
    tw = w;
    th = h;
    if (max_edge > 0 && ((int)w > max_edge || (int)h > max_edge)) {
        if (w >= h) {
            tw = (UINT)max_edge;
            th = (UINT)(((unsigned long long)max_edge * h) / w);
        } else {
            th = (UINT)max_edge;
            tw = (UINT)(((unsigned long long)max_edge * w) / h);
        }
        if (tw < 1) {
            tw = 1;
        }
        if (th < 1) {
            th = 1;
        }
        if (FAILED(factory->lpVtbl->CreateBitmapScaler(factory, &scaler)) ||
            FAILED(scaler->lpVtbl->Initialize(scaler, current, tw, th, WICBitmapInterpolationModeFant))) {
            goto done;
        }
        current->lpVtbl->Release(current);
        current = (IWICBitmapSource *)scaler;
        scaler = NULL;
    }
    exif = zi_exif_options(orientation);
    if (exif != WICBitmapTransformRotate0) {
        if (FAILED(factory->lpVtbl->CreateBitmapFlipRotator(factory, &rotator)) ||
            FAILED(rotator->lpVtbl->Initialize(rotator, current, exif))) {
            goto done;
        }
        current->lpVtbl->Release(current);
        current = (IWICBitmapSource *)rotator;
        rotator = NULL;
    }
    switch (turns & 3) {
    case 1: user = WICBitmapTransformRotate90; break;
    case 2: user = WICBitmapTransformRotate180; break;
    case 3: user = WICBitmapTransformRotate270; break;
    default: user = WICBitmapTransformRotate0; break;
    }
    if (user != WICBitmapTransformRotate0) {
        if (FAILED(factory->lpVtbl->CreateBitmapFlipRotator(factory, &rotator)) ||
            FAILED(rotator->lpVtbl->Initialize(rotator, current, user))) {
            goto done;
        }
        current->lpVtbl->Release(current);
        current = (IWICBitmapSource *)rotator;
        rotator = NULL;
    }
    if (FAILED(current->lpVtbl->GetSize(current, &tw, &th)) || tw == 0 || th == 0) {
        goto done;
    }
    dib = zi_dib_from_source(factory, current, tw, th);
    if (dib) {
        *out_w = (int)tw;
        *out_h = (int)th;
    }
done:
    if (current) {
        current->lpVtbl->Release(current);
    }
    if (rotator) {
        rotator->lpVtbl->Release(rotator);
    }
    if (scaler) {
        scaler->lpVtbl->Release(scaler);
    }
    return dib;
}

static HBITMAP zi_decode_file(IWICImagingFactory *factory, const wchar_t *path, int max_edge, int turns,
                              int *out_w, int *out_h) {
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    HBITMAP dib = NULL;
    int orientation;

    if (FAILED(factory->lpVtbl->CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame))) {
        goto done;
    }
    orientation = zi_frame_orientation(frame);
    dib = zi_decode_source(factory, (IWICBitmapSource *)frame, orientation, max_edge, turns, out_w, out_h);
done:
    if (frame) {
        frame->lpVtbl->Release(frame);
    }
    if (decoder) {
        decoder->lpVtbl->Release(decoder);
    }
    return dib;
}

static HBITMAP zi_copy_turned(HBITMAP src, int quarters) {
    BITMAP bm;
    BITMAPINFO info;
    BYTE *src_bits;
    BYTE *dst_bits = NULL;
    HBITMAP dst = NULL;
    int q = quarters & 3;
    int sw;
    int sh;
    int dw;
    int dh;
    int sx;
    int sy;
    int src_stride;
    int dst_stride;

    if (!src || GetObject(src, sizeof bm, &bm) != (int)sizeof bm || !bm.bmBits) {
        return NULL;
    }
    if (bm.bmBitsPixel != 32 || bm.bmWidth < 1 || bm.bmHeight < 1) {
        return NULL;
    }
    if (bm.bmWidth > 20000 || bm.bmHeight > 20000) {
        return NULL;
    }
    sw = bm.bmWidth;
    sh = bm.bmHeight;
    src_stride = bm.bmWidthBytes;
    src_bits = (BYTE *)bm.bmBits;
    if (q == 1 || q == 3) {
        dw = sh;
        dh = sw;
    } else {
        dw = sw;
        dh = sh;
    }
    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = dw;
    info.bmiHeader.biHeight = -dh;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    dst = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, (void **)&dst_bits, NULL, 0);
    if (!dst || !dst_bits) {
        if (dst) {
            DeleteObject(dst);
        }
        return NULL;
    }
    dst_stride = dw * 4;
    for (sy = 0; sy < sh; sy++) {
        for (sx = 0; sx < sw; sx++) {
            int dx;
            int dy;
            BYTE *sp;
            BYTE *dp;

            if (q == 1) {
                dx = sh - 1 - sy;
                dy = sx;
            } else if (q == 2) {
                dx = sw - 1 - sx;
                dy = sh - 1 - sy;
            } else if (q == 3) {
                dx = sy;
                dy = sw - 1 - sx;
            } else {
                dx = sx;
                dy = sy;
            }
            sp = src_bits + sy * src_stride + sx * 4;
            dp = dst_bits + dy * dst_stride + dx * 4;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = 255;
        }
    }
    return dst;
}

/* Folder tiles are capped at the same long edge as the Mac folder preview. */
#define ZI_TILE_EDGE 250
#define ZI_PLAIN_CAP 32

typedef struct ZiPlainTile {
    wchar_t path[MAX_PATH];
    HBITMAP thumb;
    int w;
    int h;
} ZiPlainTile;

static ZiPlainTile g_plain[ZI_PLAIN_CAP];
static int g_plain_count;
static wchar_t g_overlay_path[MAX_PATH];

static HBITMAP zi_scale_bitmap(HBITMAP src, int max_edge, int *out_w, int *out_h) {
    BITMAP bm;
    BITMAPINFO info;
    HDC src_dc;
    HDC dst_dc;
    HBITMAP dst;
    HGDIOBJ old_src;
    HGDIOBJ old_dst;
    void *bits = NULL;
    BYTE *pixel;
    int sw;
    int sh;
    int dw;
    int dh;
    int long_edge;
    int count;
    int i;

    if (!src || !out_w || !out_h || GetObject(src, sizeof bm, &bm) != (int)sizeof bm) {
        return NULL;
    }
    sw = bm.bmWidth;
    sh = bm.bmHeight;
    if (sw < 1 || sh < 1 || sw > 20000 || sh > 20000) {
        return NULL;
    }
    dw = sw;
    dh = sh;
    long_edge = sw > sh ? sw : sh;
    if (max_edge > 0 && long_edge > max_edge) {
        if (sw >= sh) {
            dw = max_edge;
            dh = (int)(((long long)sh * max_edge) / sw);
        } else {
            dh = max_edge;
            dw = (int)(((long long)sw * max_edge) / sh);
        }
        if (dw < 1) {
            dw = 1;
        }
        if (dh < 1) {
            dh = 1;
        }
    }
    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = dw;
    info.bmiHeader.biHeight = -dh;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    dst = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    src_dc = CreateCompatibleDC(NULL);
    dst_dc = CreateCompatibleDC(NULL);
    if (!dst || !bits || !src_dc || !dst_dc) {
        if (dst) {
            DeleteObject(dst);
        }
        if (src_dc) {
            DeleteDC(src_dc);
        }
        if (dst_dc) {
            DeleteDC(dst_dc);
        }
        return NULL;
    }
    old_src = SelectObject(src_dc, src);
    old_dst = SelectObject(dst_dc, dst);
    SetStretchBltMode(dst_dc, HALFTONE);
    SetBrushOrgEx(dst_dc, 0, 0, NULL);
    if (dw == sw && dh == sh) {
        BitBlt(dst_dc, 0, 0, dw, dh, src_dc, 0, 0, SRCCOPY);
    } else {
        StretchBlt(dst_dc, 0, 0, dw, dh, src_dc, 0, 0, sw, sh, SRCCOPY);
    }
    SelectObject(src_dc, old_src);
    SelectObject(dst_dc, old_dst);
    DeleteDC(src_dc);
    DeleteDC(dst_dc);
    pixel = (BYTE *)bits;
    count = dw * dh;
    for (i = 0; i < count; i++) {
        pixel[i * 4 + 3] = 255;
    }
    *out_w = dw;
    *out_h = dh;
    return dst;
}

static int zi_plain_find(const wchar_t *path) {
    int i;

    if (!path || !path[0]) {
        return -1;
    }
    for (i = 0; i < g_plain_count; i++) {
        if (_wcsicmp(g_plain[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static void zi_plain_store(const ZiFile *file) {
    HBITMAP copy;
    int index;
    int w = 0;
    int h = 0;

    if (!file || !file->path[0] || !file->thumb) {
        return;
    }
    copy = zi_scale_bitmap(file->thumb, 0, &w, &h);
    if (!copy) {
        return;
    }
    index = zi_plain_find(file->path);
    if (index < 0) {
        if (g_plain_count >= ZI_PLAIN_CAP) {
            if (g_plain[0].thumb) {
                DeleteObject(g_plain[0].thumb);
            }
            memmove(&g_plain[0], &g_plain[1], (ZI_PLAIN_CAP - 1) * sizeof g_plain[0]);
            g_plain_count = ZI_PLAIN_CAP - 1;
        }
        index = g_plain_count++;
        ZeroMemory(&g_plain[index], sizeof g_plain[index]);
    } else if (g_plain[index].thumb) {
        DeleteObject(g_plain[index].thumb);
    }
    wcscpy_s(g_plain[index].path, MAX_PATH, file->path);
    g_plain[index].thumb = copy;
    g_plain[index].w = w;
    g_plain[index].h = h;
}

static int zi_plain_restore(ZiFile *file) {
    HBITMAP copy;
    int index;
    int w = 0;
    int h = 0;

    if (!file) {
        return 0;
    }
    index = zi_plain_find(file->path);
    if (index < 0 || !g_plain[index].thumb) {
        return 0;
    }
    copy = zi_scale_bitmap(g_plain[index].thumb, 0, &w, &h);
    if (!copy) {
        return 0;
    }
    if (file->thumb) {
        DeleteObject(file->thumb);
    }
    file->thumb = copy;
    file->thumb_w = w;
    file->thumb_h = h;
    file->failed = 0;
    return 1;
}

static void zi_turn_plain(const wchar_t *path, int delta) {
    HBITMAP turned;
    BITMAP bm;
    int index;

    index = zi_plain_find(path);
    if (index < 0 || !g_plain[index].thumb || (delta & 3) == 0) {
        return;
    }
    turned = zi_copy_turned(g_plain[index].thumb, delta);
    if (!turned) {
        return;
    }
    DeleteObject(g_plain[index].thumb);
    g_plain[index].thumb = turned;
    if (GetObject(turned, sizeof bm, &bm) == (int)sizeof bm) {
        g_plain[index].w = bm.bmWidth;
        g_plain[index].h = bm.bmHeight;
    }
}

static void zi_plain_free_all(void) {
    int i;

    for (i = 0; i < g_plain_count; i++) {
        if (g_plain[i].thumb) {
            DeleteObject(g_plain[i].thumb);
            g_plain[i].thumb = NULL;
        }
    }
    g_plain_count = 0;
    g_overlay_path[0] = L'\0';
}

static void zi_editor_free_bitmaps(ZiApp *app) {
    if (app->photo) {
        DeleteObject(app->photo);
        app->photo = NULL;
    }
    if (app->photo_base) {
        DeleteObject(app->photo_base);
        app->photo_base = NULL;
    }
    app->photo_w = 0;
    app->photo_h = 0;
}

typedef struct ZiGradeJob {
    HWND hwnd;
    LONG serial;
    HBITMAP base;
    ZiTone tone;
    ZiTouchDoc doc;
} ZiGradeJob;

static DWORD WINAPI zi_grade_thread(LPVOID param) {
    ZiGradeJob *job = (ZiGradeJob *)param;
    ZiGradeNote *note;
    HBITMAP bitmap = NULL;
    int bins[ZI_SPECTRUM_BINS];
    int has_bins = 0;
    int w = 0;
    int h = 0;

    memset(bins, 0, sizeof bins);
    if (g_editor_alive && job->base) {
        bitmap = zi_tone_render(job->base, &job->tone, &job->doc, &w, &h, bins);
        has_bins = bitmap != NULL && !zi_spectrum_identity(&job->tone);
    }
    if (job->base) {
        DeleteObject(job->base);
        job->base = NULL;
    }
    InterlockedExchange(&g_grade_inflight, 0);
    if (!g_editor_alive || !job->hwnd) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        zi_touch_doc_free(&job->doc);
        free(job);
        return 0;
    }
    note = (ZiGradeNote *)calloc(1, sizeof *note);
    if (!note) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        zi_touch_doc_free(&job->doc);
        free(job);
        return 0;
    }
    note->serial = job->serial;
    note->w = w;
    note->h = h;
    note->has_bins = has_bins;
    note->touch_count = job->doc.count;
    note->tone = job->tone;
    if (has_bins) {
        memcpy(note->bins, bins, sizeof note->bins);
    }
    note->bitmap = bitmap;
    if (!PostMessageW(job->hwnd, ZI_WM_GRADE, 0, (LPARAM)note)) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        free(note);
    }
    zi_touch_doc_free(&job->doc);
    free(job);
    return 0;
}

typedef struct ZiAutoJob {
    HWND hwnd;
    LONG serial;
    HBITMAP base;
    ZiTone tone;
    ZiTouchDoc doc;
} ZiAutoJob;

static DWORD WINAPI zi_auto_thread(LPVOID param) {
    ZiAutoJob *job = (ZiAutoJob *)param;
    ZiAutoNote *note;
    ZiTone measured;
    int ok = 0;

    measured = job->tone;
    if (g_editor_alive && job->base && job->serial == g_auto_serial) {
        ok = zi_tone_auto(job->base, &measured, &job->doc);
    }
    if (job->base) {
        DeleteObject(job->base);
        job->base = NULL;
    }
    InterlockedExchange(&g_auto_inflight, 0);
    if (!g_editor_alive || !job->hwnd) {
        zi_touch_doc_free(&job->doc);
        free(job);
        return 0;
    }
    note = (ZiAutoNote *)calloc(1, sizeof *note);
    if (!note) {
        zi_touch_doc_free(&job->doc);
        free(job);
        return 0;
    }
    note->serial = job->serial;
    note->ok = ok;
    note->exposure = measured.exposure;
    note->contrast = measured.contrast;
    note->highlights = measured.highlights;
    note->shadows = measured.shadows;
    note->whites = measured.whites;
    note->blacks = measured.blacks;
    if (!PostMessageW(job->hwnd, ZI_WM_AUTO, 0, (LPARAM)note)) {
        free(note);
    }
    zi_touch_doc_free(&job->doc);
    free(job);
    return 0;
}

static void zi_clear_grade_fields(ZiTone *tone) {
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
    tone->skin_delta_a = 0;
    tone->skin_delta_b = 0;
}

/* Geometry and repairs. The tone stack is cleared so the sample is stage 3. */
static HBITMAP zi_stage3(HBITMAP base, const ZiTone *tone, const ZiTouchDoc *touch) {
    ZiTone geo = *tone;

    zi_clear_grade_fields(&geo);
    if (!zi_tone_needs_pipeline(&geo) && !zi_touch_doc_visible(touch)) {
        return zi_copy_turned(base, geo.turns);
    }
    return zi_tone_render(base, &geo, touch, NULL, NULL, NULL);
}

typedef struct ZiDropJob {
    HWND hwnd;
    LONG serial;
    HBITMAP base;
    ZiTone tone;
    int skin;
    int preset;
    int px;
    int py;
    int view_w;
    int view_h;
    ZiTouchDoc doc;
} ZiDropJob;

static DWORD WINAPI zi_drop_thread(LPVOID param) {
    ZiDropJob *job = (ZiDropJob *)param;
    ZiDropNote *note;
    HBITMAP stage = NULL;
    ZiWindowSample sample;
    BITMAP bm;
    int sx;
    int sy;
    int side;
    int ok = 0;
    double temperature = 6500;
    double tint = 0;
    double delta_a = 0;
    double delta_b = 0;
    int varied = 0;

    memset(&sample, 0, sizeof sample);
    if (g_editor_alive && job->base && job->serial == g_drop_serial) {
        stage = zi_stage3(job->base, &job->tone, &job->doc);
    }
    if (stage && GetObject(stage, sizeof bm, &bm) == (int)sizeof bm && bm.bmWidth > 0 && bm.bmHeight > 0) {
        sx = job->px;
        sy = job->py;
        if (job->view_w > 0 && bm.bmWidth != job->view_w) {
            sx = (int)((double)job->px * (double)bm.bmWidth / (double)job->view_w);
        }
        if (job->view_h > 0 && bm.bmHeight != job->view_h) {
            sy = (int)((double)job->py * (double)bm.bmHeight / (double)job->view_h);
        }
        if (sx < 0) {
            sx = 0;
        }
        if (sy < 0) {
            sy = 0;
        }
        if (sx >= bm.bmWidth) {
            sx = bm.bmWidth - 1;
        }
        if (sy >= bm.bmHeight) {
            sy = bm.bmHeight - 1;
        }
        side = job->skin ? 21 : 11;
        if (zi_tone_sample_window(stage, sx, sy, side, &sample) && sample.count > 0) {
            if (job->skin) {
                zi_skin_deltas(sample.median_r, sample.median_g, sample.median_b, job->preset, &delta_a, &delta_b);
            } else {
                zi_tone_neutralize(sample.mean_r, sample.mean_g, sample.mean_b, &temperature, &tint);
                varied = sample.chroma_rms > 3.0;
            }
            ok = 1;
        }
    }
    if (stage) {
        DeleteObject(stage);
    }
    if (job->base) {
        DeleteObject(job->base);
        job->base = NULL;
    }
    InterlockedExchange(&g_drop_inflight, 0);
    if (!g_editor_alive || !job->hwnd) {
        zi_touch_doc_free(&job->doc);
        free(job);
        return 0;
    }
    note = (ZiDropNote *)calloc(1, sizeof *note);
    if (!note) {
        zi_touch_doc_free(&job->doc);
        free(job);
        return 0;
    }
    note->serial = job->serial;
    note->ok = ok;
    note->skin = job->skin;
    note->varied = varied;
    note->preset = job->preset;
    note->temperature = temperature;
    note->tint = tint;
    note->delta_a = delta_a;
    note->delta_b = delta_b;
    if (!PostMessageW(job->hwnd, ZI_WM_DROP, 0, (LPARAM)note)) {
        free(note);
    }
    zi_touch_doc_free(&job->doc);
    free(job);
    return 0;
}

/* Repairs that finished while a picture was still drawing. Each one is shown in order. */
static ZiTone g_grade_tone;
static int g_grade_touches;
static ZiTouchDoc g_grade_q[ZI_TOUCH_CAP];
static ZiTone g_grade_q_tone[ZI_TOUCH_CAP];
static int g_grade_q_len;

static void zi_grade_q_clear(void) {
    int i;

    for (i = 0; i < g_grade_q_len; i++) {
        zi_touch_doc_free(&g_grade_q[i]);
    }
    g_grade_q_len = 0;
}

static int zi_grade_launch(ZiApp *app, const ZiTone *tone, const ZiTouchDoc *doc) {
    ZiGradeJob *job;
    HANDLE thread;
    HBITMAP base;
    LONG serial;

    if (!app || !app->hwnd || !app->photo_base || !app->photo_path[0] || !tone || !doc) {
        return 0;
    }
    if (InterlockedCompareExchange(&g_grade_inflight, 1, 0) != 0) {
        return 0;
    }
    serial = InterlockedIncrement(&g_grade_serial);
    app->grade_serial = serial;
    base = zi_copy_turned(app->photo_base, 0);
    if (!base) {
        InterlockedExchange(&g_grade_inflight, 0);
        return 0;
    }
    job = (ZiGradeJob *)calloc(1, sizeof *job);
    if (!job) {
        DeleteObject(base);
        InterlockedExchange(&g_grade_inflight, 0);
        return 0;
    }
    job->hwnd = app->hwnd;
    job->serial = serial;
    job->base = base;
    job->tone = *tone;
    zi_touch_doc_copy(&job->doc, doc);
    g_grade_tone = job->tone;
    g_grade_touches = job->doc.count;
    thread = CreateThread(NULL, 0, zi_grade_thread, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        return 1;
    }
    DeleteObject(base);
    zi_touch_doc_free(&job->doc);
    free(job);
    InterlockedExchange(&g_grade_inflight, 0);
    return 0;
}

static void zi_grade_pump(ZiApp *app) {
    ZiTone tone;
    ZiTouchDoc doc;

    if (!app || g_grade_q_len <= 0 || g_grade_inflight) {
        return;
    }
    tone = g_grade_q_tone[0];
    memset(&doc, 0, sizeof doc);
    zi_touch_doc_move(&doc, &g_grade_q[0]);
    g_grade_q_len--;
    if (g_grade_q_len > 0) {
        memmove(&g_grade_q[0], &g_grade_q[1], (size_t)g_grade_q_len * sizeof g_grade_q[0]);
        memmove(&g_grade_q_tone[0], &g_grade_q_tone[1], (size_t)g_grade_q_len * sizeof g_grade_q_tone[0]);
    }
    memset(&g_grade_q[g_grade_q_len], 0, sizeof g_grade_q[0]);
    zi_grade_launch(app, &tone, &doc);
    zi_touch_doc_free(&doc);
}

static void zi_grade_enqueue(ZiApp *app) {
    int latest;

    latest = g_grade_touches;
    if (g_grade_q_len > 0) {
        latest = g_grade_q[g_grade_q_len - 1].count;
    }
    if (!zi_tone_same(&g_grade_tone, &app->edit) || g_live.count <= latest) {
        zi_grade_q_clear();
        app->grade_serial = InterlockedIncrement(&g_grade_serial);
        return;
    }
    if (g_grade_q_len >= ZI_TOUCH_CAP) {
        zi_touch_doc_copy(&g_grade_q[ZI_TOUCH_CAP - 1], &g_live);
        g_grade_q_tone[ZI_TOUCH_CAP - 1] = app->edit;
        return;
    }
    zi_touch_doc_copy(&g_grade_q[g_grade_q_len], &g_live);
    g_grade_q_tone[g_grade_q_len] = app->edit;
    g_grade_q_len++;
}

static void zi_grade_request(ZiApp *app) {
    if (!app || !app->hwnd || !app->photo_base || !app->photo_path[0]) {
        return;
    }
    if (!zi_grade_launch(app, &app->edit, &g_live) && g_grade_inflight) {
        zi_grade_enqueue(app);
    }
}

static void zi_editor_rebuild(ZiApp *app) {
    BITMAP bm;
    HBITMAP turned;

    app->photo_turns = app->edit.turns & 3;
    if (zi_tone_needs_pipeline(&app->edit) || zi_touch_doc_visible(&g_live)) {
        zi_grade_request(app);
        return;
    }
    zi_grade_q_clear();
    app->grade_serial = InterlockedIncrement(&g_grade_serial);
    if (app->photo) {
        DeleteObject(app->photo);
        app->photo = NULL;
    }
    app->photo_w = 0;
    app->photo_h = 0;
    if (!app->photo_base) {
        return;
    }
    turned = zi_copy_turned(app->photo_base, app->photo_turns);
    if (!turned) {
        return;
    }
    app->photo = turned;
    if (GetObject(turned, sizeof bm, &bm) == (int)sizeof bm) {
        app->photo_w = bm.bmWidth;
        app->photo_h = bm.bmHeight;
    }
    zi_spectrum_count_dib(turned, app->spectrum_bins);
    app->spectrum_ready = 1;
    app->spectrum_source = app->edit;
    zi_editor_match_folder_tile(app);
}

static ZiFile *zi_file_by_path(ZiApp *app, const wchar_t *path) {
    int i;

    for (i = 0; i < app->file_count; i++) {
        if (_wcsicmp(app->files[i].path, path) == 0) {
            return &app->files[i];
        }
    }
    return NULL;
}

static void zi_turn_thumb(ZiFile *file, int delta) {
    HBITMAP turned;
    BITMAP bm;

    if (!file || (delta & 3) == 0) {
        return;
    }
    zi_turn_plain(file->path, delta);
    if (!file->thumb) {
        return;
    }
    turned = zi_copy_turned(file->thumb, delta);
    if (!turned) {
        return;
    }
    DeleteObject(file->thumb);
    file->thumb = turned;
    if (GetObject(turned, sizeof bm, &bm) == (int)sizeof bm) {
        file->thumb_w = bm.bmWidth;
        file->thumb_h = bm.bmHeight;
    }
}

void zi_editor_note_thumb(ZiFile *file) {
    ZiTone tone;
    int turns;
    HBITMAP turned;
    BITMAP bm;

    if (!file || !file->thumb) {
        return;
    }
    tone = zi_edit_get(file->path);
    turns = tone.turns & 3;
    if (turns == 0) {
        return;
    }
    turned = zi_copy_turned(file->thumb, turns);
    if (!turned) {
        return;
    }
    DeleteObject(file->thumb);
    file->thumb = turned;
    if (GetObject(turned, sizeof bm, &bm) == (int)sizeof bm) {
        file->thumb_w = bm.bmWidth;
        file->thumb_h = bm.bmHeight;
    }
}

void zi_editor_match_folder_tile(ZiApp *app) {
    ZiFile *file;
    HBITMAP scaled;
    int graded;
    int w = 0;
    int h = 0;

    if (!app || !zi_showing_folder(app) || !app->photo_path[0] || app->photo_failed) {
        return;
    }
    file = zi_file_by_path(app, app->photo_path);
    if (!file || file->video) {
        return;
    }
    graded = app->photo &&
             (zi_tone_needs_pipeline(&app->edit) || zi_touch_doc_visible(&g_live));
    if (!graded) {
        if (g_overlay_path[0] && _wcsicmp(g_overlay_path, file->path) == 0 && zi_plain_restore(file)) {
            g_overlay_path[0] = L'\0';
        }
        return;
    }
    /* The render in flight still shows the previous picture. Wait for it. */
    if (g_grade_inflight) {
        return;
    }
    if (!g_overlay_path[0] || _wcsicmp(g_overlay_path, file->path) != 0) {
        zi_plain_store(file);
    }
    scaled = zi_scale_bitmap(app->photo, ZI_TILE_EDGE, &w, &h);
    if (!scaled) {
        return;
    }
    if (file->thumb) {
        DeleteObject(file->thumb);
    }
    file->thumb = scaled;
    file->thumb_w = w;
    file->thumb_h = h;
    file->failed = 0;
    wcscpy_s(g_overlay_path, MAX_PATH, file->path);
}

void zi_editor_on_decoded_thumb(ZiApp *app, ZiFile *file) {
    if (!file) {
        return;
    }
    /* This bitmap is a new original. Do not keep it out of the plain cache. */
    if (g_overlay_path[0] && _wcsicmp(g_overlay_path, file->path) == 0) {
        g_overlay_path[0] = L'\0';
        zi_plain_store(file);
    } else if (zi_plain_find(file->path) >= 0) {
        zi_plain_store(file);
    }
    if (app && app->photo_path[0] && _wcsicmp(app->photo_path, file->path) == 0) {
        zi_editor_match_folder_tile(app);
    }
}

static void zi_docs_free_all(void) {
    int i;

    zi_touch_doc_free(&g_live);
    for (i = 0; i < ZI_HIST; i++) {
        zi_touch_doc_free(&g_steps[i]);
    }
}

static void zi_live_from_pos(const ZiApp *app) {
    if (!app || app->hist_pos < 0 || app->hist_pos >= app->hist_len || app->hist_pos >= ZI_HIST) {
        return;
    }
    zi_touch_doc_free(&g_live);
    zi_touch_doc_copy(&g_live, &g_steps[app->hist_pos]);
}

static void zi_hist_push(ZiApp *app, const ZiTone *tone, const wchar_t *label) {
    int i;

    if (!tone) {
        return;
    }
    if (app->hist_len > 0 && app->hist_pos >= 0 && app->hist_pos < app->hist_len &&
        zi_tone_same(&app->hist[app->hist_pos].tone, tone) &&
        zi_touch_doc_same(&g_live, &g_steps[app->hist_pos])) {
        return;
    }
    for (i = app->hist_pos + 1; i < app->hist_len && i < ZI_HIST; i++) {
        zi_touch_doc_free(&g_steps[i]);
    }
    if (app->hist_pos + 1 >= ZI_HIST) {
        zi_touch_doc_free(&g_steps[0]);
        memmove(&app->hist[0], &app->hist[1], (ZI_HIST - 1) * sizeof app->hist[0]);
        memmove(&g_steps[0], &g_steps[1], (ZI_HIST - 1) * sizeof g_steps[0]);
        zi_touch_doc_clear(&g_steps[ZI_HIST - 1]);
        app->hist_pos--;
        app->hist_len--;
        if (app->hist_pos < 0) {
            app->hist_pos = 0;
        }
        if (app->hist_len < 0) {
            app->hist_len = 0;
        }
    }
    app->hist_len = app->hist_pos + 1;
    app->hist[app->hist_len].tone = *tone;
    wcscpy_s(app->hist[app->hist_len].label, 40, label ? label : L"Edit");
    zi_touch_doc_free(&g_steps[app->hist_len]);
    zi_touch_doc_copy(&g_steps[app->hist_len], &g_live);
    app->hist_len++;
    app->hist_pos = app->hist_len - 1;
    zi_edits_note(app);
}

static void zi_apply_edit(ZiApp *app, const wchar_t *path, const ZiTone *tone) {
    ZiTone old;
    int delta;

    if (!path || !path[0] || !tone) {
        return;
    }
    old = zi_edit_get(path);
    delta = (tone->turns - old.turns) & 3;
    zi_edit_set(path, tone);
    zi_turn_thumb(zi_file_by_path(app, path), delta);
    if (app->photo_path[0] && _wcsicmp(app->photo_path, path) == 0) {
        app->edit = *tone;
        app->photo_turns = tone->turns & 3;
        app->horizon_have = 0;
        zi_editor_rebuild(app);
    }
    zi_editor_refresh(app);
}

static void zi_open_video(ZiApp *app, const wchar_t *path) {
    HINSTANCE opened = ShellExecuteW(app->hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);

    if ((INT_PTR)opened <= 32) {
        zi_editor_notice(app, L"That video could not be opened.");
    }
}

typedef struct ZiPhotoJob {
    HWND hwnd;
    LONG generation;
    wchar_t path[MAX_PATH];
} ZiPhotoJob;

static DWORD WINAPI zi_photo_thread(LPVOID param) {
    ZiPhotoJob *job = (ZiPhotoJob *)param;
    IWICImagingFactory *factory = NULL;
    ZiPhotoNote *note;
    HBITMAP bitmap = NULL;
    int w = 0;
    int h = 0;

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        free(job);
        return 0;
    }
    if (g_editor_alive && job->generation == g_photo_generation) {
        CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                         &IID_IWICImagingFactory, (void **)&factory);
        if (factory) {
            bitmap = zi_decode_file(factory, job->path, ZI_VIEW_EDGE, 0, &w, &h);
            factory->lpVtbl->Release(factory);
        }
    }
    if (!g_editor_alive || job->generation != g_photo_generation) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        CoUninitialize();
        free(job);
        return 0;
    }
    note = (ZiPhotoNote *)calloc(1, sizeof *note);
    if (!note) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        CoUninitialize();
        free(job);
        return 0;
    }
    note->generation = job->generation;
    note->failed = bitmap ? 0 : 1;
    note->w = w;
    note->h = h;
    note->bitmap = bitmap;
    if (!PostMessageW(job->hwnd, ZI_WM_PHOTO, 0, (LPARAM)note)) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        free(note);
    }
    CoUninitialize();
    free(job);
    return 0;
}

static void zi_editor_start_photo(ZiApp *app) {
    ZiPhotoJob *job;
    HANDLE thread;
    LONG generation;

    generation = InterlockedIncrement(&g_photo_generation);
    app->photo_generation = generation;
    if (!app->hwnd || !app->photo_path[0]) {
        return;
    }
    job = (ZiPhotoJob *)calloc(1, sizeof *job);
    if (!job) {
        app->photo_busy = 0;
        app->photo_failed = 1;
        return;
    }
    job->hwnd = app->hwnd;
    job->generation = generation;
    wcscpy_s(job->path, MAX_PATH, app->photo_path);
    thread = CreateThread(NULL, 0, zi_photo_thread, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        free(job);
        app->photo_busy = 0;
        app->photo_failed = 1;
    }
}

static void zi_clear_marquee(ZiApp *app) {
    int was_drag = app->drag_crop;

    app->has_marquee = 0;
    app->drag_crop = 0;
    if (was_drag && app->hwnd && !app->drag_split && !app->drag_pan && !app->drag_zoom &&
        !app->drag_preview && !app->drag_slider) {
        ReleaseCapture();
    }
}

static void zi_doc_stash_free(void) {
    int i;

    for (i = 0; i < ZI_HIST; i++) {
        zi_touch_doc_free(&g_doc_docs[i]);
    }
}

void zi_editor_open_path(ZiApp *app, const wchar_t *path) {
    ZiTone tone;
    const wchar_t *base;

    if (!path || !path[0]) {
        return;
    }
    base = wcsrchr(path, L'\\');
    base = (base && base[1]) ? base + 1 : path;
    if (zi_is_video_name(base)) {
        zi_open_video(app, path);
        return;
    }
    zi_editor_show_image(app);
    if (app->photo_path[0] && _wcsicmp(app->photo_path, path) == 0 &&
        (app->photo || app->photo_busy) && !app->photo_failed) {
        zi_editor_title(app);
        if (app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return;
    }
    if (g_hold && app->photo_path[0]) {
        zi_edits_note(app);
    }
    tone = zi_edit_get(path);
    zi_editor_free_bitmaps(app);
    wcscpy_s(app->photo_path, MAX_PATH, path);
    wcscpy_s(app->opened, MAX_PATH, path);
    app->edit = tone;
    app->photo_turns = tone.turns & 3;
    app->photo_failed = 0;
    app->photo_busy = 1;
    app->pending_fit = 1;
    app->zoom_mag = 1;
    app->zoom_min = 0.1;
    app->zoom_off_x = 0;
    app->zoom_off_y = 0;
    app->zoom_tool = 1;
    app->select_tool = 0;
    app->eyedropper_tool = 0;
    app->horizon_tool = 0;
    app->horizon_have = 0;
    app->spot_tool = 0;
    app->scratch_tool = 0;
    app->scratch_drag = 0;
    app->scratch_count = 0;
    app->hover_on = 0;
    app->touch_busy = 0;
    g_spot_q_len = 0;
    g_spot_running = 0;
    app->amount_drag = 0;
    app->drop_pending = 0;
    zi_docs_free_all();
    zi_grade_q_clear();
    zi_clear_marquee(app);
    app->grade_serial = InterlockedIncrement(&g_grade_serial);
    app->auto_serial = InterlockedIncrement(&g_auto_serial);
    InterlockedIncrement(&g_drop_serial);
    app->spectrum_ready = 0;
    app->hist[0].tone = tone;
    wcscpy_s(app->hist[0].label, 40, L"Open");
    app->hist_len = 1;
    app->hist_pos = 0;
    g_hold = 0;
    if (!g_doc_wait || !g_doc_path[0] || _wcsicmp(g_doc_path, path) != 0) {
        g_doc_wait = 0;
        zi_doc_stash_free();
    }
    app->notice[0] = L'\0';
    zi_editor_title(app);
    zi_editor_start_photo(app);
    zi_queue_hash(app);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

void zi_editor_open_selection(ZiApp *app) {
    if (app->file_selected < 0 || app->file_selected >= app->file_count) {
        return;
    }
    zi_editor_open_path(app, app->files[app->file_selected].path);
}

int zi_editor_step(ZiApp *app, int delta) {
    int stills[ZI_MAX_FILES];
    int n = 0;
    int pos = -1;
    int i;

    if (zi_showing_folder(app) || !app->photo_path[0]) {
        return 0;
    }
    for (i = 0; i < app->file_count; i++) {
        if (app->files[i].video) {
            continue;
        }
        if (_wcsicmp(app->files[i].path, app->photo_path) == 0) {
            pos = n;
        }
        stills[n++] = i;
    }
    if (pos < 0 || pos + delta < 0 || pos + delta >= n) {
        return 0;
    }
    i = stills[pos + delta];
    zi_layout_now(app);
    zi_grid_select_only(app, i);
    zi_editor_open_path(app, app->files[i].path);
    return 1;
}

void zi_editor_on_photo(ZiApp *app, ZiPhotoNote *note) {
    if (!note) {
        return;
    }
    if (!app || note->generation != app->photo_generation) {
        if (note->bitmap) {
            DeleteObject(note->bitmap);
        }
        free(note);
        return;
    }
    zi_editor_free_bitmaps(app);
    app->photo_busy = 0;
    app->photo_failed = note->failed || !note->bitmap;
    if (app->photo_failed) {
        app->spectrum_ready = 0;
    }
    if (note->bitmap && !app->photo_failed) {
        app->photo_base = note->bitmap;
        note->bitmap = NULL;
        app->pending_fit = 1;
        zi_editor_rebuild(app);
    }
    if (note->bitmap) {
        DeleteObject(note->bitmap);
    }
    free(note);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

static int zi_add_line(ZiFactNote *note, const wchar_t *line) {
    if (!line || !line[0] || note->count >= 8) {
        return 0;
    }
    wcscpy_s(note->lines[note->count], 260, line);
    note->count++;
    return 1;
}

static int zi_variant_text(const PROPVARIANT *value, wchar_t *dest, size_t cap) {
    if (value->vt == VT_LPWSTR && value->pwszVal && value->pwszVal[0]) {
        wcscpy_s(dest, cap, value->pwszVal);
        return 1;
    }
    if (value->vt == VT_BSTR && value->bstrVal && value->bstrVal[0]) {
        wcscpy_s(dest, cap, value->bstrVal);
        return 1;
    }
    if ((value->vt == VT_LPSTR || value->vt == VT_BSTR) && value->pszVal && value->pszVal[0]) {
        if (MultiByteToWideChar(CP_UTF8, 0, value->pszVal, -1, dest, (int)cap) > 0) {
            return 1;
        }
    }
    dest[0] = L'\0';
    return 0;
}

static void zi_add_date(ZiFactNote *note, IWICMetadataQueryReader *meta, const wchar_t *query, const wchar_t *label) {
    PROPVARIANT value;
    wchar_t text[128];
    wchar_t line[260];

    PropVariantInit(&value);
    if (SUCCEEDED(meta->lpVtbl->GetMetadataByName(meta, query, &value)) && zi_variant_text(&value, text, 128)) {
        swprintf_s(line, 260, L"%s %s", label, text);
        zi_add_line(note, line);
    }
    PropVariantClear(&value);
}

typedef struct ZiFactJob {
    HWND hwnd;
    LONG generation;
    int video;
    wchar_t path[MAX_PATH];
} ZiFactJob;

static DWORD WINAPI zi_fact_thread(LPVOID param) {
    ZiFactJob *job = (ZiFactJob *)param;
    ZiFactNote *note;
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICMetadataQueryReader *meta = NULL;
    WIN32_FILE_ATTRIBUTE_DATA info;
    const wchar_t *base;
    wchar_t line[260];
    int opened = 0;
    int has_date = 0;
    unsigned long long bytes = 0;
    UINT w = 0;
    UINT h = 0;

    note = (ZiFactNote *)calloc(1, sizeof *note);
    if (!note) {
        free(job);
        return 0;
    }
    note->generation = job->generation;
    wcscpy_s(note->path, MAX_PATH, job->path);
    base = wcsrchr(job->path, L'\\');
    base = (base && base[1]) ? base + 1 : job->path;
    zi_add_line(note, base);
    zi_add_line(note, job->path);
    if (GetFileAttributesExW(job->path, GetFileExInfoStandard, &info)) {
        bytes = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        swprintf_s(line, 260, L"%.1f MB", (double)bytes / 1000000.0);
        zi_add_line(note, line);
    }
    if (job->video) {
        zi_add_line(note, L"Video");
    } else if (SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                         &IID_IWICImagingFactory, (void **)&factory);
        if (factory &&
            SUCCEEDED(factory->lpVtbl->CreateDecoderFromFilename(factory, job->path, NULL, GENERIC_READ,
                                                                 WICDecodeMetadataCacheOnLoad, &decoder)) &&
            SUCCEEDED(decoder->lpVtbl->GetFrame(decoder, 0, &frame))) {
            opened = 1;
            if (SUCCEEDED(frame->lpVtbl->GetSize(frame, &w, &h)) && w > 0 && h > 0) {
                swprintf_s(line, 260, L"%u \u00D7 %u", w, h);
            } else {
                line[0] = L'\0';
            }
            if (SUCCEEDED(frame->lpVtbl->GetMetadataQueryReader(frame, &meta)) && meta) {
                int before = note->count;

                zi_add_date(note, meta, L"/app1/ifd/exif/{ushort=36867}", L"DateTimeOriginal");
                zi_add_date(note, meta, L"/app1/ifd/exif/{ushort=36868}", L"DateTimeDigitized");
                zi_add_date(note, meta, L"/app1/ifd/{ushort=306}", L"DateTime");
                has_date = note->count > before;
                meta->lpVtbl->Release(meta);
            }
            if (line[0]) {
                zi_add_line(note, line);
            }
            if (opened && !has_date) {
                zi_add_line(note, L"No DateTimeOriginal, DateTimeDigitized, or DateTime tag was in the file header.");
            }
        } else {
            zi_add_line(note, L"The file could not be read from the library folder.");
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
        CoUninitialize();
    }
    if (!g_editor_alive || job->generation != g_fact_generation ||
        !PostMessageW(job->hwnd, ZI_WM_FACTS, 0, (LPARAM)note)) {
        free(note);
    }
    free(job);
    return 0;
}

void zi_editor_sync_details(ZiApp *app) {
    const wchar_t *path = NULL;
    int video = 0;
    ZiFactJob *job;
    HANDLE thread;
    LONG generation;

    if (!zi_showing_folder(app)) {
        return;
    }
    if (app->file_selected >= 0 && app->file_selected < app->file_count) {
        path = app->files[app->file_selected].path;
        video = app->files[app->file_selected].video;
    }
    if (!path) {
        app->fact_path[0] = L'\0';
        app->fact_count = 0;
        app->fact_ready = 0;
        return;
    }
    if (wcscmp(app->fact_path, path) == 0) {
        return;
    }
    wcscpy_s(app->fact_path, MAX_PATH, path);
    app->fact_count = 0;
    app->fact_ready = 0;
    if (!app->hwnd) {
        return;
    }
    generation = InterlockedIncrement(&g_fact_generation);
    app->fact_generation = generation;
    job = (ZiFactJob *)calloc(1, sizeof *job);
    if (!job) {
        return;
    }
    job->hwnd = app->hwnd;
    job->generation = generation;
    job->video = video;
    wcscpy_s(job->path, MAX_PATH, path);
    thread = CreateThread(NULL, 0, zi_fact_thread, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        free(job);
    }
}

void zi_editor_on_facts(ZiApp *app, ZiFactNote *note) {
    int i;

    if (!note) {
        return;
    }
    if (!app || note->generation != app->fact_generation || _wcsicmp(note->path, app->fact_path) != 0) {
        free(note);
        return;
    }
    app->fact_count = note->count;
    if (app->fact_count > 8) {
        app->fact_count = 8;
    }
    for (i = 0; i < app->fact_count; i++) {
        wcscpy_s(app->fact_lines[i], 260, note->lines[i]);
    }
    app->fact_ready = 1;
    free(note);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

static int zi_marked_still(const ZiApp *app) {
    int i;

    for (i = 0; i < app->file_count; i++) {
        if (app->file_mark[i] && !app->files[i].video) {
            return 1;
        }
    }
    return 0;
}

int zi_editor_has_photo(const ZiApp *app) {
    return app->photo != NULL;
}

int zi_editor_can_reset(const ZiApp *app) {
    if (!app->photo || app->photo_failed || !app->photo_path[0] || app->hist_len < 1) {
        return 0;
    }
    if (app->hist_pos > 0 || app->has_marquee) {
        return 1;
    }
    return !zi_tone_same(&app->edit, &app->hist[0].tone);
}

int zi_editor_can_undo(const ZiApp *app) {
    return app->photo_path[0] && app->hist_pos > 0;
}

int zi_editor_can_redo(const ZiApp *app) {
    return app->photo_path[0] && app->hist_pos + 1 < app->hist_len;
}

int zi_editor_can_copy(const ZiApp *app) {
    if (!zi_showing_folder(app)) {
        return app->photo_path[0] != L'\0';
    }
    return app->file_selected >= 0 && app->file_selected < app->file_count &&
           !app->files[app->file_selected].video;
}

int zi_editor_can_paste(const ZiApp *app) {
    if (!app->has_copied) {
        return 0;
    }
    if (!zi_showing_folder(app)) {
        return app->photo_path[0] != L'\0';
    }
    return zi_marked_still(app);
}

int zi_editor_can_rotate(const ZiApp *app) {
    if (!zi_showing_folder(app)) {
        return app->photo_path[0] && !app->photo_failed;
    }
    return zi_marked_still(app);
}

int zi_editor_can_export(const ZiApp *app) {
    if (!zi_showing_folder(app)) {
        return app->photo_path[0] && !app->photo_failed;
    }
    return zi_marked_still(app);
}

static void zi_editor_refresh(ZiApp *app) {
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

void zi_editor_on_grade(ZiApp *app, ZiGradeNote *note) {
    LONG serial;
    HBITMAP bitmap;
    ZiTone rendered;
    int w;
    int h;
    int touch_count;
    int progress;
    int has_bins = 0;
    int grade_bins[ZI_SPECTRUM_BINS];

    if (!note) {
        return;
    }
    serial = note->serial;
    bitmap = note->bitmap;
    w = note->w;
    h = note->h;
    touch_count = note->touch_count;
    rendered = note->tone;
    has_bins = note->has_bins;
    if (has_bins) {
        memcpy(grade_bins, note->bins, sizeof grade_bins);
    }
    free(note);
    if (!app) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return;
    }
    /* A later spot may already be queued. This picture is still the repairs finished so far. */
    progress = app->photo_base && touch_count > 0 && touch_count < g_live.count &&
               zi_tone_same(&rendered, &app->edit);
    if ((serial != app->grade_serial && !progress) || !app->photo_base) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (app->photo_base && serial != app->grade_serial &&
            (zi_tone_needs_pipeline(&app->edit) || zi_touch_doc_visible(&g_live))) {
            zi_grade_request(app);
        }
        return;
    }
    if (!bitmap || w < 1 || h < 1) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (!app->photo) {
            app->photo_failed = 1;
        }
        zi_editor_notice(app, L"The adjustment could not be applied.");
        if (serial == app->grade_serial) {
            zi_grade_pump(app);
        }
        return;
    }
    if (app->photo) {
        DeleteObject(app->photo);
    }
    app->photo = bitmap;
    app->photo_w = w;
    app->photo_h = h;
    app->photo_failed = 0;
    if (app->spectrum_ready) {
        ZiTone now = app->edit;
        ZiTone prev = app->spectrum_source;

        now.spectrum_low = 0;
        now.spectrum_mid = 0.5;
        now.spectrum_high = 1;
        prev.spectrum_low = 0;
        prev.spectrum_mid = 0.5;
        prev.spectrum_high = 1;
        if (zi_tone_same(&now, &prev)) {
            has_bins = 0;
        }
    }
    if (zi_spectrum_identity(&app->edit)) {
        zi_spectrum_count_dib(bitmap, app->spectrum_bins);
        app->spectrum_ready = 1;
        app->spectrum_source = app->edit;
    } else if (has_bins) {
        memcpy(app->spectrum_bins, grade_bins, sizeof app->spectrum_bins);
        app->spectrum_ready = 1;
        app->spectrum_source = app->edit;
    }
    zi_editor_match_folder_tile(app);
    zi_editor_refresh(app);
    if (serial == app->grade_serial) {
        zi_grade_pump(app);
    }
}

void zi_editor_set_slider(ZiApp *app, int index, double value) {
    if (!app->photo_path[0] || app->photo_failed || index < 0 || index >= ZI_SLIDER_COUNT) {
        return;
    }
    zi_slider_set(&app->edit, index, value);
    app->photo_turns = app->edit.turns & 3;
    zi_editor_rebuild(app);
    zi_editor_refresh(app);
}

void zi_editor_slider_commit(ZiApp *app) {
    const ZiSliderSpec *spec;

    if (!app->photo_path[0] || app->hist_len < 1) {
        return;
    }
    if (app->slider_index < 0 || app->slider_index >= ZI_SLIDER_COUNT) {
        return;
    }
    if (app->hist_pos < 0 || app->hist_pos >= app->hist_len) {
        return;
    }
    if (zi_tone_same(&app->edit, &app->hist[app->hist_pos].tone)) {
        return;
    }
    spec = zi_slider_spec(app->slider_index);
    zi_edit_set(app->photo_path, &app->edit);
    if (app->slider_index >= ZI_SL_CTL_X && app->slider_index <= ZI_SL_CBL_Y) {
        zi_hist_push(app, &app->edit, L"Corners");
    } else {
        zi_hist_push(app, &app->edit, spec->label);
    }
    zi_editor_refresh(app);
}

void zi_editor_slider_reset(ZiApp *app, int index) {
    const ZiSliderSpec *spec;

    if (index < 0 || index >= ZI_SLIDER_COUNT || !app->photo_path[0] || app->photo_failed) {
        return;
    }
    spec = zi_slider_spec(index);
    if (!spec->reset) {
        return;
    }
    app->slider_index = index;
    zi_editor_set_slider(app, index, spec->reset_value);
    zi_editor_slider_commit(app);
}

void zi_editor_spectrum_reset(ZiApp *app, int handle) {
    double value;

    if (handle < 0 || handle > 2 || !app->photo_path[0] || app->photo_failed) {
        return;
    }
    if (handle == ZI_SL_SPEC_MID) {
        value = (app->edit.spectrum_low + app->edit.spectrum_high) * 0.5;
    } else if (handle == ZI_SL_SPEC_HIGH) {
        value = 1;
    } else {
        value = 0;
    }
    app->slider_index = handle;
    zi_editor_set_slider(app, handle, value);
    zi_editor_slider_commit(app);
}

static void zi_editor_commit_open(ZiApp *app) {
    zi_editor_slider_commit(app);
    if (app->hist_len < 1 || app->hist_pos < 0 || app->hist_pos >= app->hist_len) {
        return;
    }
    if (!zi_tone_same(&app->edit, &app->hist[app->hist_pos].tone)) {
        zi_hist_push(app, &app->edit, L"Edit");
    }
}

void zi_editor_auto(ZiApp *app) {
    ZiAutoJob *job;
    HANDLE thread;
    HBITMAP base;
    LONG serial;

    if (!app->photo || app->photo_failed || !app->photo_base || !app->photo_path[0] || !app->hwnd) {
        return;
    }
    zi_editor_commit_open(app);
    if (InterlockedCompareExchange(&g_auto_inflight, 1, 0) != 0) {
        return;
    }
    serial = app->auto_serial;
    base = zi_copy_turned(app->photo_base, 0);
    if (!base) {
        InterlockedExchange(&g_auto_inflight, 0);
        return;
    }
    job = (ZiAutoJob *)calloc(1, sizeof *job);
    if (!job) {
        DeleteObject(base);
        InterlockedExchange(&g_auto_inflight, 0);
        return;
    }
    job->hwnd = app->hwnd;
    job->serial = serial;
    job->base = base;
    job->tone = app->edit;
    zi_touch_doc_copy(&job->doc, &g_live);
    thread = CreateThread(NULL, 0, zi_auto_thread, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        DeleteObject(base);
        zi_touch_doc_free(&job->doc);
        free(job);
        InterlockedExchange(&g_auto_inflight, 0);
    }
}

void zi_editor_on_auto(ZiApp *app, ZiAutoNote *note) {
    ZiTone next;

    if (!note) {
        return;
    }
    if (!app || !note->ok || note->serial != app->auto_serial || !app->photo_path[0] || !app->photo_base) {
        free(note);
        return;
    }
    next = app->edit;
    next.exposure = note->exposure;
    next.contrast = note->contrast;
    next.highlights = note->highlights;
    next.shadows = note->shadows;
    next.whites = note->whites;
    next.blacks = note->blacks;
    free(note);
    if (zi_tone_same(&next, &app->edit)) {
        return;
    }
    zi_hist_push(app, &next, L"Auto");
    zi_apply_edit(app, app->photo_path, &next);
}

void zi_editor_on_drop(ZiApp *app, ZiDropNote *note) {
    ZiTone next;
    const wchar_t *label;
    int retry = 0;
    int x = 0;
    int y = 0;

    if (!note) {
        return;
    }
    if (!app || !note->ok || note->serial != g_drop_serial || !app->photo_path[0] || !app->photo_base) {
        if (app && app->drop_pending && note->serial != g_drop_serial) {
            retry = 1;
            x = app->drop_x;
            y = app->drop_y;
            app->drop_pending = 0;
        }
        free(note);
        if (retry) {
            zi_drop_begin(app, x, y);
        }
        return;
    }
    app->drop_pending = 0;
    next = app->edit;
    if (note->skin) {
        next.has_skin = 1;
        next.skin_preset = note->preset;
        next.skin_delta_a = note->delta_a;
        next.skin_delta_b = note->delta_b;
        label = zi_skin_label(note->preset);
    } else {
        next.temperature = note->temperature;
        next.tint = note->tint;
        label = note->varied ? L"White, varied sample" : L"White";
    }
    free(note);
    if (zi_tone_same(&next, &app->edit)) {
        zi_editor_refresh(app);
        return;
    }
    zi_hist_push(app, &next, label);
    zi_apply_edit(app, app->photo_path, &next);
}

void zi_editor_reset(ZiApp *app) {
    if (!zi_editor_can_reset(app)) {
        return;
    }
    zi_editor_commit_open(app);
    app->auto_serial = InterlockedIncrement(&g_auto_serial);
    zi_clear_marquee(app);
    if (app->hist_len < 1) {
        return;
    }
    if (app->hist_pos == 0 && zi_tone_same(&app->edit, &app->hist[0].tone) &&
        zi_touch_doc_same(&g_live, &g_steps[0])) {
        zi_editor_refresh(app);
        return;
    }
    app->hist_pos = 0;
    zi_live_from_pos(app);
    zi_apply_edit(app, app->photo_path, &app->hist[0].tone);
    zi_edits_note(app);
}

void zi_editor_history_select(ZiApp *app, int index) {
    if (!app->photo_path[0] || index < 0 || index >= app->hist_len || index == app->hist_pos) {
        return;
    }
    app->hist_pos = index;
    zi_live_from_pos(app);
    zi_clear_marquee(app);
    zi_apply_edit(app, app->photo_path, &app->hist[index].tone);
    zi_edits_note(app);
}

void zi_editor_undo(ZiApp *app) {
    if (!zi_editor_can_undo(app)) {
        return;
    }
    app->hist_pos--;
    zi_live_from_pos(app);
    zi_clear_marquee(app);
    zi_apply_edit(app, app->photo_path, &app->hist[app->hist_pos].tone);
    zi_edits_note(app);
}

void zi_editor_redo(ZiApp *app) {
    if (!zi_editor_can_redo(app)) {
        return;
    }
    app->hist_pos++;
    zi_live_from_pos(app);
    zi_clear_marquee(app);
    zi_apply_edit(app, app->photo_path, &app->hist[app->hist_pos].tone);
    zi_edits_note(app);
}

void zi_editor_copy_path(ZiApp *app, const wchar_t *path) {
    if (!path || !path[0]) {
        return;
    }
    if (app->photo_path[0] && _wcsicmp(app->photo_path, path) == 0) {
        app->copied = app->edit;
    } else {
        app->copied = zi_edit_get(path);
    }
    app->has_copied = 1;
    zi_editor_refresh(app);
}

void zi_editor_copy(ZiApp *app) {
    if (!zi_editor_can_copy(app)) {
        return;
    }
    if (!zi_showing_folder(app)) {
        zi_editor_copy_path(app, app->photo_path);
    } else {
        zi_editor_copy_path(app, app->files[app->file_selected].path);
    }
}

static ZiTone zi_paste_keeping_crop(const ZiTone *copied, const ZiTone *dest) {
    ZiTone next = *copied;

    next.has_crop = dest->has_crop;
    next.crop_x = dest->crop_x;
    next.crop_y = dest->crop_y;
    next.crop_w = dest->crop_w;
    next.crop_h = dest->crop_h;
    return next;
}

static void zi_paste_path(ZiApp *app, const wchar_t *path) {
    ZiTone current;
    ZiTone next;

    if (!path || !path[0] || !app->has_copied) {
        return;
    }
    current = zi_edit_get(path);
    next = zi_paste_keeping_crop(&app->copied, &current);
    if (zi_tone_same(&current, &next)) {
        return;
    }
    if (app->photo_path[0] && _wcsicmp(path, app->photo_path) == 0) {
        zi_hist_push(app, &next, L"Paste settings");
    } else {
        zi_queue_commit(path, &next, L"Paste settings");
    }
    zi_apply_edit(app, path, &next);
}

void zi_editor_paste(ZiApp *app) {
    int i;

    if (!zi_editor_can_paste(app)) {
        return;
    }
    if (!zi_showing_folder(app)) {
        ZiTone next = zi_paste_keeping_crop(&app->copied, &app->edit);

        if (!zi_tone_same(&app->edit, &next)) {
            zi_hist_push(app, &next, L"Paste settings");
            zi_apply_edit(app, app->photo_path, &next);
        }
    } else {
        for (i = 0; i < app->file_count; i++) {
            if (!app->file_mark[i] || app->files[i].video) {
                continue;
            }
            zi_paste_path(app, app->files[i].path);
        }
    }
    zi_editor_refresh(app);
}

void zi_editor_paste_index(ZiApp *app, int index) {
    if (!app->has_copied || index < 0 || index >= app->file_count) {
        return;
    }
    if (app->file_mark[index]) {
        zi_editor_paste(app);
        return;
    }
    if (app->files[index].video) {
        return;
    }
    zi_paste_path(app, app->files[index].path);
    zi_editor_refresh(app);
}

static void zi_rotate_path(ZiApp *app, const wchar_t *path, int clockwise, int record) {
    ZiTone next = zi_edit_get(path);

    next.turns = (next.turns + (clockwise ? 1 : 3)) & 3;
    if (record && app->photo_path[0] && path && _wcsicmp(path, app->photo_path) == 0) {
        zi_hist_push(app, &next, clockwise ? L"Rotate right" : L"Rotate left");
    } else if (record) {
        zi_queue_commit(path, &next, clockwise ? L"Rotate right" : L"Rotate left");
    }
    zi_apply_edit(app, path, &next);
}

void zi_editor_rotate(ZiApp *app, int clockwise) {
    int i;
    int any = 0;

    if (!zi_editor_can_rotate(app)) {
        zi_editor_notice(app, L"Select a still.");
        return;
    }
    if (!zi_showing_folder(app)) {
        zi_rotate_path(app, app->photo_path, clockwise, 1);
        zi_editor_refresh(app);
        return;
    }
    for (i = 0; i < app->file_count; i++) {
        if (app->file_mark[i] && !app->files[i].video) {
            zi_rotate_path(app, app->files[i].path, clockwise, 1);
            any = 1;
        }
    }
    if (!any) {
        zi_editor_notice(app, L"Select a still.");
        return;
    }
    zi_editor_refresh(app);
}

void zi_editor_unavailable(ZiApp *app, int id) {
    (void)app;
    (void)id;
}

static int zi_touch_size_px(const ZiApp *app) {
    int px = app ? app->touch_size : 48;

    if (px < 8 || px > 400) {
        return 48;
    }
    return px;
}

static int zi_brush_radius(const ZiApp *app) {
    RECT dest;
    int dw;
    double radius;
    int rounded;

    if (!app || !zi_photo_dest(app, &dest)) {
        return 1;
    }
    dw = dest.right - dest.left;
    if (dw < 1 || app->photo_w < 1) {
        return 1;
    }
    radius = ((double)zi_touch_size_px(app) * 0.5) * (double)app->photo_w / (double)dw;
    rounded = (int)(radius + 0.5);
    if (rounded < 1) {
        rounded = 1;
    }
    return rounded;
}

typedef struct ZiTouchJob {
    HWND hwnd;
    LONG generation;
    int kind;
    HBITMAP base;
    ZiTone tone;
    ZiTouchDoc doc;
    int x;
    int y;
    int radius;
    int count;
    int *xs;
    int *ys;
} ZiTouchJob;

static DWORD WINAPI zi_touch_thread(LPVOID param) {
    ZiTouchJob *job = (ZiTouchJob *)param;
    ZiTouchNote *note = NULL;
    float *rgb = NULL;
    unsigned char *mask = NULL;
    int w = 0;
    int h = 0;
    int found = -1;

    if (g_editor_alive && job->base && job->generation == g_photo_generation) {
        rgb = zi_tone_stage_rgb(job->base, &job->tone, &job->doc, &w, &h);
    }
    if (rgb && w > 1 && h > 1) {
        if (job->kind == 0) {
            found = zi_touch_dust_mask(rgb, w, h, &mask);
        } else if (job->kind == 1) {
            found = zi_touch_scratch_mask(rgb, w, h, &mask);
        } else if (job->kind == 2) {
            found = zi_touch_spot_mask(rgb, w, h, job->x, job->y, job->radius, &mask);
        } else {
            found = zi_touch_stroke_mask(w, h, job->xs, job->ys, job->count, job->radius, &mask);
        }
    }
    free(rgb);
    if (job->base) {
        DeleteObject(job->base);
        job->base = NULL;
    }
    zi_touch_doc_free(&job->doc);
    free(job->xs);
    free(job->ys);
    job->xs = NULL;
    job->ys = NULL;
    if (g_editor_alive && job->hwnd) {
        note = (ZiTouchNote *)calloc(1, sizeof *note);
    }
    if (note) {
        note->generation = job->generation;
        note->kind = job->kind;
        note->found = found;
        note->mask = mask;
        note->w = w;
        note->h = h;
        mask = NULL;
        if (!PostMessageW(job->hwnd, ZI_WM_TOUCH, 0, (LPARAM)note)) {
            free(note->mask);
            free(note);
            note = NULL;
        }
    }
    free(mask);
    free(job);
    /* The window clears this when it takes the note. A missed post must not leave it stuck. */
    if (!note) {
        InterlockedExchange(&g_touch_inflight, 0);
    }
    return 0;
}

static int zi_spots_remaining(void) {
    return g_spot_q_len + (g_spot_running ? 1 : 0);
}

static void zi_spot_notice(ZiApp *app) {
    int left = zi_spots_remaining();
    wchar_t text[64];

    if (!app || left <= 0) {
        return;
    }
    if (left == 1) {
        wcscpy_s(text, 64, L"1 spot remaining.");
    } else {
        swprintf_s(text, 64, L"%d spots remaining.", left);
    }
    zi_editor_notice(app, text);
}

static int zi_spot_room(void) {
    if (g_spot_q_len >= ZI_SPOT_Q) {
        return 0;
    }
    if (g_live.count + zi_spots_remaining() >= ZI_TOUCH_CAP) {
        return 0;
    }
    return 1;
}

static int zi_spot_enqueue(ZiApp *app, int x, int y, int radius) {
    ZiSpotClick *click;

    if (!app || !zi_spot_room()) {
        if (app) {
            zi_editor_notice(app, L"The repair list is full.");
        }
        return 0;
    }
    click = &g_spot_q[g_spot_q_len];
    click->generation = app->photo_generation;
    click->x = x;
    click->y = y;
    click->radius = radius;
    g_spot_q_len++;
    zi_spot_notice(app);
    return 1;
}

static int zi_spot_take(ZiSpotClick *out) {
    if (!out || g_spot_q_len <= 0) {
        return 0;
    }
    *out = g_spot_q[0];
    g_spot_q_len--;
    if (g_spot_q_len > 0) {
        memmove(&g_spot_q[0], &g_spot_q[1], (size_t)g_spot_q_len * sizeof g_spot_q[0]);
    }
    return 1;
}

static void zi_spot_put_front(ZiSpotClick click) {
    if (g_spot_q_len >= ZI_SPOT_Q) {
        return;
    }
    memmove(&g_spot_q[1], &g_spot_q[0], (size_t)g_spot_q_len * sizeof g_spot_q[0]);
    g_spot_q[0] = click;
    g_spot_q_len++;
}

static int zi_touch_begin(ZiApp *app, int kind, int x, int y, int radius, const int *xs, const int *ys, int count) {
    ZiTouchJob *job;
    HANDLE thread;
    HBITMAP base;
    const wchar_t *working;

    if (!app || !app->hwnd || !app->photo_base || !app->photo_path[0] || app->photo_failed) {
        return 0;
    }
    if (g_live.count >= ZI_TOUCH_CAP) {
        zi_editor_notice(app, L"The repair list is full.");
        return 0;
    }
    if (InterlockedCompareExchange(&g_touch_inflight, 1, 0) != 0) {
        return 0;
    }
    base = zi_copy_turned(app->photo_base, 0);
    if (!base) {
        InterlockedExchange(&g_touch_inflight, 0);
        zi_editor_notice(app, L"The removal did not run.");
        return 0;
    }
    job = (ZiTouchJob *)calloc(1, sizeof *job);
    if (!job) {
        DeleteObject(base);
        InterlockedExchange(&g_touch_inflight, 0);
        zi_editor_notice(app, L"The removal did not run.");
        return 0;
    }
    job->hwnd = app->hwnd;
    job->generation = app->photo_generation;
    job->kind = kind;
    job->base = base;
    job->tone = app->edit;
    job->x = x;
    job->y = y;
    job->radius = radius;
    zi_touch_doc_copy(&job->doc, &g_live);
    if (kind == 3 && xs && ys && count > 1) {
        job->xs = (int *)malloc((size_t)count * sizeof(int));
        job->ys = (int *)malloc((size_t)count * sizeof(int));
        if (!job->xs || !job->ys) {
            free(job->xs);
            free(job->ys);
            DeleteObject(base);
            zi_touch_doc_free(&job->doc);
            free(job);
            InterlockedExchange(&g_touch_inflight, 0);
            zi_editor_notice(app, L"The removal did not run.");
            return 0;
        }
        memcpy(job->xs, xs, (size_t)count * sizeof(int));
        memcpy(job->ys, ys, (size_t)count * sizeof(int));
        job->count = count;
    }
    thread = CreateThread(NULL, 0, zi_touch_thread, job, 0, NULL);
    if (!thread) {
        DeleteObject(base);
        zi_touch_doc_free(&job->doc);
        free(job->xs);
        free(job->ys);
        free(job);
        InterlockedExchange(&g_touch_inflight, 0);
        zi_editor_notice(app, L"The removal did not run.");
        return 0;
    }
    CloseHandle(thread);
    app->touch_busy = 1;
    if (kind == 2) {
        g_spot_running = 1;
        zi_spot_notice(app);
        return 1;
    }
    if (kind == 0) {
        working = L"Removing dust\u2026";
    } else if (kind == 1) {
        working = L"Removing scratches\u2026";
    } else {
        working = L"Removing the scratch\u2026";
    }
    zi_editor_notice(app, working);
    return 1;
}

static void zi_spot_pump(ZiApp *app) {
    ZiSpotClick click;

    if (!app || !g_editor_alive) {
        return;
    }
    while (zi_spot_take(&click)) {
        if (click.generation != app->photo_generation || app->photo_failed || !app->photo_base) {
            continue;
        }
        if (g_live.count >= ZI_TOUCH_CAP) {
            g_spot_q_len = 0;
            zi_editor_notice(app, L"The repair list is full.");
            return;
        }
        if (g_touch_inflight) {
            zi_spot_put_front(click);
            return;
        }
        if (zi_touch_begin(app, 2, click.x, click.y, click.radius, NULL, NULL, 0)) {
            return;
        }
    }
}

void zi_editor_on_touch(ZiApp *app, ZiTouchNote *note) {
    const wchar_t *empty = L"The removal did not run.";
    const char *model;
    const wchar_t *label;
    ZiTouchOp op;

    if (!note) {
        return;
    }
    if (!app || note->generation != app->photo_generation) {
        int kind = note->kind;

        free(note->mask);
        free(note);
        if (kind == 2) {
            g_spot_running = 0;
        }
        InterlockedExchange(&g_touch_inflight, 0);
        if (app && g_editor_alive) {
            zi_spot_pump(app);
            if (!g_touch_inflight) {
                app->touch_busy = 0;
            }
            zi_editor_refresh(app);
        }
        return;
    }
    if (note->kind == 2) {
        g_spot_running = 0;
    }
    InterlockedExchange(&g_touch_inflight, 0);
    app->touch_busy = 0;
    if (note->found == 1 && note->mask && note->w > 0 && note->h > 0) {
        if (g_live.count >= ZI_TOUCH_CAP) {
            free(note->mask);
            note->mask = NULL;
            g_spot_q_len = 0;
            zi_editor_notice(app, L"The repair list is full.");
        } else {
            if (note->kind == 1 || note->kind == 3) {
                model = ZI_TOUCH_SCRATCH;
                label = L"Scratches";
            } else {
                model = ZI_TOUCH_DUST;
                label = L"Dust";
            }
            if (zi_touch_make(&op, model, note->mask, note->w, note->h)) {
                note->mask = NULL;
                g_live.ops[g_live.count++] = op;
                zi_hist_push(app, &app->edit, label);
                zi_editor_rebuild(app);
                app->notice[0] = L'\0';
            } else {
                zi_editor_notice(app, L"The removal did not run.");
            }
        }
    } else if (note->found < 0) {
        free(note->mask);
        zi_editor_notice(app, L"The removal did not run.");
    } else {
        free(note->mask);
        if (note->kind == 0) {
            empty = L"No dust stood out from the surrounding photo. Spot covers a speck with the circle.";
        } else if (note->kind == 1) {
            empty = L"No scratch stood out from the surrounding photo. Drag the Scratch circle along one.";
        } else if (note->kind == 2) {
            empty = L"No speck stood out in that circle. Cover the spot and click it.";
        } else {
            empty = L"No scratch stood out under that stroke.";
        }
        zi_editor_notice(app, empty);
    }
    free(note);
    zi_spot_pump(app);
    zi_editor_refresh(app);
}

void zi_editor_touch_dust(ZiApp *app) {
    if (!app || app->touch_busy || zi_showing_folder(app)) {
        return;
    }
    zi_touch_begin(app, 0, 0, 0, 0, NULL, NULL, 0);
}

void zi_editor_touch_scratches(ZiApp *app) {
    if (!app || app->touch_busy || zi_showing_folder(app)) {
        return;
    }
    zi_touch_begin(app, 1, 0, 0, 0, NULL, NULL, 0);
}

void zi_editor_toggle_spot(ZiApp *app) {
    if (!app || zi_showing_folder(app) || !app->photo || app->photo_failed) {
        return;
    }
    if (app->spot_tool) {
        app->spot_tool = 0;
        app->zoom_tool = 1;
        app->hover_on = 0;
    } else {
        app->spot_tool = 1;
        app->scratch_tool = 0;
        app->scratch_drag = 0;
        app->zoom_tool = 0;
        app->select_tool = 0;
        app->eyedropper_tool = 0;
        app->horizon_tool = 0;
        app->horizon_have = 0;
        zi_clear_marquee(app);
    }
    zi_editor_refresh(app);
}

void zi_editor_toggle_scratch(ZiApp *app) {
    if (!app || zi_showing_folder(app) || !app->photo || app->photo_failed) {
        return;
    }
    if (app->scratch_tool) {
        app->scratch_tool = 0;
        app->scratch_drag = 0;
        app->scratch_count = 0;
        app->zoom_tool = 1;
        app->hover_on = 0;
    } else {
        app->scratch_tool = 1;
        app->spot_tool = 0;
        app->zoom_tool = 0;
        app->select_tool = 0;
        app->eyedropper_tool = 0;
        app->horizon_tool = 0;
        app->horizon_have = 0;
        zi_clear_marquee(app);
    }
    zi_editor_refresh(app);
}

void zi_editor_touch_size_set(ZiApp *app, int px, int commit) {
    (void)commit;
    if (!app) {
        return;
    }
    if (px < 8) {
        px = 8;
    }
    if (px > 400) {
        px = 400;
    }
    app->touch_size = px;
    zi_editor_refresh(app);
}

void zi_editor_touch_amount_begin(ZiApp *app, int which) {
    if (!app) {
        return;
    }
    g_amount_which = which;
    g_amount_before = which ? zi_touch_last_strength(&g_live, ZI_TOUCH_SCRATCH)
                            : zi_touch_last_strength(&g_live, ZI_TOUCH_DUST);
    app->amount_drag = 1;
}

void zi_editor_touch_amount_set(ZiApp *app, int which, float strength) {
    if (!app || !app->photo_path[0] || app->photo_failed) {
        return;
    }
    zi_touch_set_strength(&g_live, which ? ZI_TOUCH_SCRATCH : ZI_TOUCH_DUST, strength);
    zi_editor_rebuild(app);
    zi_editor_refresh(app);
}

void zi_editor_touch_amount_end(ZiApp *app) {
    float now;
    const wchar_t *label;

    if (!app) {
        return;
    }
    app->amount_drag = 0;
    now = g_amount_which ? zi_touch_last_strength(&g_live, ZI_TOUCH_SCRATCH)
                         : zi_touch_last_strength(&g_live, ZI_TOUCH_DUST);
    if (now > g_amount_before) {
        if (now - g_amount_before <= 0.0001f) {
            return;
        }
    } else if (g_amount_before - now <= 0.0001f) {
        return;
    }
    label = g_amount_which ? L"Scratch amount" : L"Dust amount";
    zi_hist_push(app, &app->edit, label);
    zi_editor_refresh(app);
}

void zi_editor_touch_move(ZiApp *app, int x, int y) {
    int px = 0;
    int py = 0;
    int on;

    if (!app || (!app->spot_tool && !app->scratch_tool)) {
        return;
    }
    on = zi_view_point(app, x, y, &px, &py);
    app->hover_on = on;
    app->hover_x = x;
    app->hover_y = y;
    if (app->scratch_drag && on && app->scratch_count < 4096) {
        if (app->scratch_count == 0 ||
            app->scratch_x[app->scratch_count - 1] != px ||
            app->scratch_y[app->scratch_count - 1] != py) {
            app->scratch_x[app->scratch_count] = px;
            app->scratch_y[app->scratch_count] = py;
            app->scratch_count++;
        }
    }
    zi_editor_refresh(app);
}

void zi_editor_scratch_finish(ZiApp *app) {
    int count;
    int radius;

    if (!app || !app->scratch_drag) {
        return;
    }
    app->scratch_drag = 0;
    count = app->scratch_count;
    if (count < 2) {
        app->scratch_count = 0;
        zi_editor_refresh(app);
        return;
    }
    radius = zi_brush_radius(app);
    zi_touch_begin(app, 3, 0, 0, radius, app->scratch_x, app->scratch_y, count);
    app->scratch_count = 0;
    zi_editor_refresh(app);
}

int zi_editor_has_dust(const ZiApp *app) {
    (void)app;
    return zi_touch_last_index(&g_live, ZI_TOUCH_DUST) >= 0;
}

int zi_editor_has_scratch(const ZiApp *app) {
    (void)app;
    return zi_touch_last_index(&g_live, ZI_TOUCH_SCRATCH) >= 0;
}

float zi_editor_dust_strength(const ZiApp *app) {
    (void)app;
    return zi_touch_last_strength(&g_live, ZI_TOUCH_DUST);
}

float zi_editor_scratch_strength(const ZiApp *app) {
    (void)app;
    return zi_touch_last_strength(&g_live, ZI_TOUCH_SCRATCH);
}

void zi_editor_apply_fit(ZiApp *app) {
    int cw;
    int ch;
    int view_long;
    int image_long;
    double raw;

    if (!app->pending_fit || !app->photo || app->photo_w < 1 || app->photo_h < 1 || app->dpi == 0) {
        return;
    }
    cw = app->regions.canvas.right - app->regions.canvas.left;
    ch = app->regions.canvas.bottom - app->regions.canvas.top;
    if (cw < 2 || ch < 2) {
        return;
    }
    view_long = cw > ch ? cw : ch;
    image_long = app->photo_w > app->photo_h ? app->photo_w : app->photo_h;
    raw = ((double)view_long * 96.0) / ((double)app->dpi * (double)image_long);
    if (raw < 0.1) {
        app->zoom_min = raw < 0.05 ? 0.05 : raw;
        app->zoom_mag = app->zoom_min;
    } else {
        app->zoom_min = 0.1;
        app->zoom_mag = raw > ZI_ZOOM_MAX ? ZI_ZOOM_MAX : raw;
    }
    app->zoom_off_x = 0;
    app->zoom_off_y = 0;
    app->pending_fit = 0;
}

void zi_editor_format_zoom(ZiApp *app) {
    double mag = app->zoom_mag;

    if (mag < 0.01) {
        mag = 1;
    }
    swprintf_s(app->zoom_label, 16, L"%.2f\u00D7", mag);
}

double zi_editor_zoom_pos(const ZiApp *app) {
    double low;
    double high;
    double mag;
    double span;
    double floor_mag = app->zoom_min > 0.01 ? app->zoom_min : 0.1;

    low = log(floor_mag);
    high = log(ZI_ZOOM_MAX);
    span = high - low;
    if (span <= 0) {
        return 0;
    }
    mag = app->zoom_mag;
    if (mag < floor_mag) {
        mag = floor_mag;
    }
    if (mag > ZI_ZOOM_MAX) {
        mag = ZI_ZOOM_MAX;
    }
    return (log(mag) - low) / span;
}

void zi_editor_set_zoom_pos(ZiApp *app, double t) {
    double low;
    double high;
    double floor_mag = app->zoom_min > 0.01 ? app->zoom_min : 0.1;

    if (t < 0) {
        t = 0;
    }
    if (t > 1) {
        t = 1;
    }
    low = log(floor_mag);
    high = log(ZI_ZOOM_MAX);
    app->zoom_mag = exp(low + (high - low) * t);
    app->pending_fit = 0;
}

static void zi_zoom_by(ZiApp *app, double factor, int x, int y) {
    RECT c = app->regions.canvas;
    double old;
    double new_mag;
    double applied;
    double ax;
    double ay;
    double vw;
    double vh;
    double dx;
    double dy;
    double floor_mag = app->zoom_min > 0.01 ? app->zoom_min : 0.1;

    if (!app->photo || factor <= 0 || app->zoom_mag <= 0 || app->dpi == 0) {
        return;
    }
    old = app->zoom_mag;
    new_mag = old * factor;
    if (new_mag < floor_mag) {
        new_mag = floor_mag;
    }
    if (new_mag > ZI_ZOOM_MAX) {
        new_mag = ZI_ZOOM_MAX;
    }
    applied = new_mag / old;
    vw = (c.right - c.left) * 96.0 / (double)app->dpi;
    vh = (c.bottom - c.top) * 96.0 / (double)app->dpi;
    ax = (x - c.left) * 96.0 / (double)app->dpi;
    ay = (y - c.top) * 96.0 / (double)app->dpi;
    dx = ax - vw / 2.0 - app->zoom_off_x;
    dy = ay - vh / 2.0 - app->zoom_off_y;
    app->zoom_off_x += dx - dx * applied;
    app->zoom_off_y += dy - dy * applied;
    app->zoom_mag = new_mag;
    app->pending_fit = 0;
}

static int zi_photo_dest(const ZiApp *app, RECT *dest) {
    RECT canvas;
    int view_w;
    int view_h;
    double scale;
    int dw;
    int dh;
    int ox;
    int oy;

    if (!app->photo || app->photo_w < 1 || app->photo_h < 1 || app->dpi == 0) {
        return 0;
    }
    canvas = app->regions.canvas;
    view_w = canvas.right - canvas.left;
    view_h = canvas.bottom - canvas.top;
    scale = app->zoom_mag * (double)app->dpi / 96.0;
    dw = (int)((double)app->photo_w * scale + 0.5);
    dh = (int)((double)app->photo_h * scale + 0.5);
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    ox = (int)(app->zoom_off_x * (double)app->dpi / 96.0);
    oy = (int)(app->zoom_off_y * (double)app->dpi / 96.0);
    dest->left = canvas.left + (view_w - dw) / 2 + ox;
    dest->top = canvas.top + (view_h - dh) / 2 + oy;
    dest->right = dest->left + dw;
    dest->bottom = dest->top + dh;
    return 1;
}

static void zi_marquee_edges(const ZiApp *app, const RECT *dest,
                             double *left, double *top, double *right, double *bottom) {
    double dw = (double)(dest->right - dest->left);
    double dh = (double)(dest->bottom - dest->top);

    *left = (double)dest->left + app->marquee_x * dw;
    *top = (double)dest->top + app->marquee_y * dh;
    *right = (double)dest->left + (app->marquee_x + app->marquee_w) * dw;
    *bottom = (double)dest->top + (app->marquee_y + app->marquee_h) * dh;
}

/* Corners before edges: top-left, top-right, bottom-right, bottom-left, top, right, bottom, left. */
static int zi_hit_handle(const ZiApp *app, int x, int y, const RECT *dest) {
    double left;
    double top;
    double right;
    double bottom;
    double mx;
    double my;
    double marks[8][2];
    double radius;
    int i;

    if (!app->has_marquee) {
        return -1;
    }
    zi_marquee_edges(app, dest, &left, &top, &right, &bottom);
    mx = (left + right) * 0.5;
    my = (top + bottom) * 0.5;
    marks[0][0] = left;
    marks[0][1] = top;
    marks[1][0] = right;
    marks[1][1] = top;
    marks[2][0] = right;
    marks[2][1] = bottom;
    marks[3][0] = left;
    marks[3][1] = bottom;
    marks[4][0] = mx;
    marks[4][1] = top;
    marks[5][0] = right;
    marks[5][1] = my;
    marks[6][0] = mx;
    marks[6][1] = bottom;
    marks[7][0] = left;
    marks[7][1] = my;
    radius = (double)zi_px(app, 14);
    for (i = 0; i < 8; i++) {
        double dx = (double)x - marks[i][0];
        double dy = (double)y - marks[i][1];

        if (dx * dx + dy * dy <= radius * radius) {
            return i;
        }
    }
    return -1;
}

static int zi_hit_marquee(const ZiApp *app, int x, int y, const RECT *dest) {
    double left;
    double top;
    double right;
    double bottom;

    if (!app->has_marquee) {
        return 0;
    }
    zi_marquee_edges(app, dest, &left, &top, &right, &bottom);
    return (double)x >= left && (double)x <= right && (double)y >= top && (double)y <= bottom;
}

static void zi_norm_point(const ZiApp *app, const RECT *dest, int x, int y, double *nx, double *ny) {
    double scale = app->zoom_mag * (double)app->dpi / 96.0;
    double iw = app->photo_w > 0 ? (double)app->photo_w : 1.0;
    double ih = app->photo_h > 0 ? (double)app->photo_h : 1.0;
    double ix;
    double iy;

    if (scale < 0.0000001) {
        scale = 1;
    }
    ix = ((double)x - (double)dest->left) / scale;
    iy = ((double)y - (double)dest->top) / scale;
    if (ix < 0) {
        ix = 0;
    }
    if (iy < 0) {
        iy = 0;
    }
    if (ix > iw) {
        ix = iw;
    }
    if (iy > ih) {
        iy = ih;
    }
    *nx = ix / iw;
    *ny = iy / ih;
}

static void zi_norm_delta(const ZiApp *app, int x0, int y0, int x1, int y1, double *dx, double *dy) {
    double scale = app->zoom_mag * (double)app->dpi / 96.0;
    double iw = app->photo_w > 0 ? (double)app->photo_w : 1.0;
    double ih = app->photo_h > 0 ? (double)app->photo_h : 1.0;

    if (scale < 0.0000001) {
        scale = 1;
    }
    *dx = ((double)x1 - (double)x0) / scale / iw;
    *dy = ((double)y1 - (double)y0) / scale / ih;
}

static void zi_store_marquee(ZiApp *app, double x, double y, double w, double h) {
    zi_norm_clamp(&x, &y, &w, &h);
    app->has_marquee = 1;
    app->marquee_x = x;
    app->marquee_y = y;
    app->marquee_w = w;
    app->marquee_h = h;
}

static void zi_crop_drag_to(ZiApp *app, int x, int y) {
    RECT dest;
    double sx;
    double sy;
    double cx;
    double cy;
    double dx;
    double dy;

    if (!app->drag_crop || !zi_photo_dest(app, &dest)) {
        return;
    }
    if (app->crop_kind == 0) {
        double left;
        double top;
        double w;
        double h;

        zi_norm_point(app, &dest, app->crop_down.x, app->crop_down.y, &sx, &sy);
        zi_norm_point(app, &dest, x, y, &cx, &cy);
        left = sx < cx ? sx : cx;
        top = sy < cy ? sy : cy;
        w = sx > cx ? sx - cx : cx - sx;
        h = sy > cy ? sy - cy : cy - sy;
        zi_store_marquee(app, left, top, w, h);
    } else if (app->crop_kind == 1) {
        double nx;
        double ny;
        double limit_x;
        double limit_y;

        zi_norm_delta(app, app->crop_down.x, app->crop_down.y, x, y, &dx, &dy);
        nx = app->crop_start_x + dx;
        ny = app->crop_start_y + dy;
        limit_x = 1.0 - app->crop_start_w;
        limit_y = 1.0 - app->crop_start_h;
        if (limit_x < 0) {
            limit_x = 0;
        }
        if (limit_y < 0) {
            limit_y = 0;
        }
        if (nx < 0) {
            nx = 0;
        }
        if (ny < 0) {
            ny = 0;
        }
        if (nx > limit_x) {
            nx = limit_x;
        }
        if (ny > limit_y) {
            ny = limit_y;
        }
        app->has_marquee = 1;
        app->marquee_x = nx;
        app->marquee_y = ny;
        app->marquee_w = app->crop_start_w;
        app->marquee_h = app->crop_start_h;
    } else {
        double left = app->crop_start_x;
        double right = app->crop_start_x + app->crop_start_w;
        double top = app->crop_start_y;
        double bottom = app->crop_start_y + app->crop_start_h;
        int handle = app->crop_handle;

        zi_norm_delta(app, app->crop_down.x, app->crop_down.y, x, y, &dx, &dy);
        if (handle == 0 || handle == 3 || handle == 7) {
            left += dx;
        } else if (handle == 1 || handle == 2 || handle == 5) {
            right += dx;
        }
        if (handle == 0 || handle == 1 || handle == 4) {
            top += dy;
        } else if (handle == 2 || handle == 3 || handle == 6) {
            bottom += dy;
        }
        zi_store_marquee(app, left, top, right - left, bottom - top);
    }
    zi_editor_refresh(app);
}

static void zi_crop_begin(ZiApp *app, int x, int y) {
    RECT dest;
    int handle;

    if (!zi_photo_dest(app, &dest)) {
        return;
    }
    app->crop_down.x = x;
    app->crop_down.y = y;
    if (app->has_marquee && (handle = zi_hit_handle(app, x, y, &dest)) >= 0) {
        app->crop_kind = 2;
        app->crop_handle = handle;
    } else if (zi_hit_marquee(app, x, y, &dest)) {
        app->crop_kind = 1;
    } else {
        app->crop_kind = 0;
    }
    if (app->crop_kind == 0) {
        app->crop_start_x = 0;
        app->crop_start_y = 0;
        app->crop_start_w = 0;
        app->crop_start_h = 0;
    } else {
        app->crop_start_x = app->marquee_x;
        app->crop_start_y = app->marquee_y;
        app->crop_start_w = app->marquee_w;
        app->crop_start_h = app->marquee_h;
    }
    app->drag_crop = 1;
    zi_crop_drag_to(app, x, y);
    if (app->hwnd) {
        SetCapture(app->hwnd);
    }
}

int zi_editor_can_crop(const ZiApp *app) {
    if (zi_showing_folder(app) || !app->has_marquee || !app->photo) {
        return 0;
    }
    return zi_norm_usable(app->marquee_w, app->marquee_h, app->photo_w, app->photo_h);
}

void zi_editor_use_zoom(ZiApp *app) {
    app->zoom_tool = 1;
    app->select_tool = 0;
    app->eyedropper_tool = 0;
    app->horizon_tool = 0;
    app->horizon_have = 0;
    app->spot_tool = 0;
    app->scratch_tool = 0;
    app->scratch_drag = 0;
    app->scratch_count = 0;
    app->hover_on = 0;
    zi_clear_marquee(app);
    zi_editor_refresh(app);
}

void zi_editor_toggle_select(ZiApp *app) {
    if (zi_showing_folder(app) || !app->photo) {
        return;
    }
    if (app->select_tool) {
        zi_editor_use_zoom(app);
        return;
    }
    app->select_tool = 1;
    app->zoom_tool = 0;
    app->eyedropper_tool = 0;
    app->horizon_tool = 0;
    app->horizon_have = 0;
    app->spot_tool = 0;
    app->scratch_tool = 0;
    app->scratch_drag = 0;
    app->hover_on = 0;
    app->has_marquee = 1;
    app->marquee_x = 0;
    app->marquee_y = 0;
    app->marquee_w = 1;
    app->marquee_h = 1;
    zi_editor_refresh(app);
}

void zi_editor_toggle_eyedropper(ZiApp *app) {
    if (zi_showing_folder(app) || !app->photo || app->photo_failed) {
        return;
    }
    if (app->eyedropper_tool) {
        zi_editor_use_zoom(app);
        return;
    }
    app->eyedropper_tool = 1;
    app->zoom_tool = 0;
    app->select_tool = 0;
    app->horizon_tool = 0;
    app->horizon_have = 0;
    app->spot_tool = 0;
    app->scratch_tool = 0;
    app->scratch_drag = 0;
    app->hover_on = 0;
    zi_clear_marquee(app);
    zi_editor_refresh(app);
}

void zi_editor_toggle_horizon(ZiApp *app) {
    if (zi_showing_folder(app) || !app->photo || app->photo_failed) {
        return;
    }
    if (app->horizon_tool) {
        zi_editor_use_zoom(app);
        return;
    }
    app->horizon_tool = 1;
    app->zoom_tool = 0;
    app->select_tool = 0;
    app->eyedropper_tool = 0;
    app->horizon_have = 0;
    app->spot_tool = 0;
    app->scratch_tool = 0;
    app->scratch_drag = 0;
    app->hover_on = 0;
    zi_clear_marquee(app);
    zi_editor_refresh(app);
}

void zi_editor_set_corner_pin(ZiApp *app, int enabled) {
    ZiTone next;

    if (!app->photo_path[0] || app->photo_failed) {
        return;
    }
    zi_editor_commit_open(app);
    next = app->edit;
    if (enabled) {
        if (next.has_corners) {
            return;
        }
        next.has_corners = 1;
        next.corner_x[0] = 0;
        next.corner_y[0] = 0;
        next.corner_x[1] = 1;
        next.corner_y[1] = 0;
        next.corner_x[2] = 1;
        next.corner_y[2] = 1;
        next.corner_x[3] = 0;
        next.corner_y[3] = 1;
    } else if (next.has_corners) {
        next.has_corners = 0;
    } else {
        return;
    }
    if (zi_tone_same(&app->edit, &next)) {
        return;
    }
    zi_hist_push(app, &next, L"Corners");
    zi_apply_edit(app, app->photo_path, &next);
}

void zi_editor_choose_lens(ZiApp *app, int index) {
    ZiTone next;

    if (!app->photo_path[0] || app->photo_failed || index < 0 || index >= zi_lens_count()) {
        return;
    }
    zi_editor_commit_open(app);
    next = app->edit;
    zi_lens_copy(&next, index);
    if (zi_tone_same(&app->edit, &next)) {
        return;
    }
    zi_hist_push(app, &next, zi_lens_label(index));
    zi_apply_edit(app, app->photo_path, &next);
}

void zi_editor_apply_crop(ZiApp *app) {
    ZiTone next;

    if (!zi_editor_can_crop(app) || !app->photo_path[0]) {
        return;
    }
    next = app->edit;
    zi_crop_assign(&next, app->marquee_x, app->marquee_y, app->marquee_w, app->marquee_h);
    if (zi_tone_same(&app->edit, &next)) {
        return;
    }
    zi_hist_push(app, &next, L"Crop");
    zi_apply_edit(app, app->photo_path, &next);
    app->select_tool = 0;
    app->zoom_tool = 1;
    app->eyedropper_tool = 0;
    app->horizon_tool = 0;
    app->horizon_have = 0;
    app->spot_tool = 0;
    app->scratch_tool = 0;
    zi_clear_marquee(app);
    zi_editor_refresh(app);
}

void zi_editor_crop_end(ZiApp *app) {
    if (!app->has_marquee ||
        !zi_norm_usable(app->marquee_w, app->marquee_h, app->photo_w, app->photo_h)) {
        app->has_marquee = 0;
    }
    app->drag_crop = 0;
    zi_editor_refresh(app);
}

HCURSOR zi_editor_cursor(const ZiApp *app, int x, int y) {
    POINT pt;
    RECT dest;
    int handle;

    pt.x = x;
    pt.y = y;
    if (zi_showing_folder(app) || !app->photo || !PtInRect(&app->regions.canvas, pt)) {
        return NULL;
    }
    if (app->eyedropper_tool || app->horizon_tool || app->spot_tool || app->scratch_tool) {
        return LoadCursor(NULL, IDC_CROSS);
    }
    if (!app->select_tool) {
        return NULL;
    }
    if (app->has_marquee && zi_photo_dest(app, &dest)) {
        handle = zi_hit_handle(app, x, y, &dest);
        if (handle == 5 || handle == 7) {
            return LoadCursor(NULL, IDC_SIZEWE);
        }
        if (handle == 4 || handle == 6) {
            return LoadCursor(NULL, IDC_SIZENS);
        }
    }
    return LoadCursor(NULL, IDC_CROSS);
}

static int zi_view_point(const ZiApp *app, int x, int y, int *px, int *py) {
    RECT dest;
    int dw;
    int dh;

    if (!zi_photo_dest(app, &dest)) {
        return 0;
    }
    if (x < dest.left || y < dest.top || x >= dest.right || y >= dest.bottom) {
        return 0;
    }
    dw = dest.right - dest.left;
    dh = dest.bottom - dest.top;
    if (dw < 1 || dh < 1 || app->photo_w < 1 || app->photo_h < 1) {
        return 0;
    }
    *px = (int)((double)(x - dest.left) * (double)app->photo_w / (double)dw);
    *py = (int)((double)(y - dest.top) * (double)app->photo_h / (double)dh);
    if (*px < 0) {
        *px = 0;
    }
    if (*py < 0) {
        *py = 0;
    }
    if (*px >= app->photo_w) {
        *px = app->photo_w - 1;
    }
    if (*py >= app->photo_h) {
        *py = app->photo_h - 1;
    }
    return 1;
}

static void zi_drop_begin(ZiApp *app, int x, int y) {
    ZiDropJob *job;
    HANDLE thread;
    HBITMAP base;
    LONG serial;
    int px;
    int py;

    if (!app->photo || app->photo_failed || !app->photo_base || !app->photo_path[0] || !app->hwnd) {
        return;
    }
    if (!zi_view_point(app, x, y, &px, &py)) {
        return;
    }
    zi_editor_commit_open(app);
    serial = InterlockedIncrement(&g_drop_serial);
    if (InterlockedCompareExchange(&g_drop_inflight, 1, 0) != 0) {
        app->drop_pending = 1;
        app->drop_x = x;
        app->drop_y = y;
        return;
    }
    app->drop_pending = 0;
    base = zi_copy_turned(app->photo_base, 0);
    if (!base) {
        InterlockedExchange(&g_drop_inflight, 0);
        return;
    }
    job = (ZiDropJob *)calloc(1, sizeof *job);
    if (!job) {
        DeleteObject(base);
        InterlockedExchange(&g_drop_inflight, 0);
        return;
    }
    job->hwnd = app->hwnd;
    job->serial = serial;
    job->base = base;
    job->tone = app->edit;
    job->skin = app->eyedropper_skin;
    job->preset = app->skin_preset;
    if (job->preset < 0 || job->preset >= ZI_SKIN_COUNT) {
        job->preset = 0;
    }
    job->px = px;
    job->py = py;
    job->view_w = app->photo_w;
    job->view_h = app->photo_h;
    zi_touch_doc_copy(&job->doc, &g_live);
    thread = CreateThread(NULL, 0, zi_drop_thread, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        DeleteObject(base);
        zi_touch_doc_free(&job->doc);
        free(job);
        InterlockedExchange(&g_drop_inflight, 0);
    }
}

static int zi_horizon_view(const ZiApp *app, int x, int y, double *vx, double *vy) {
    RECT dest;
    int dw;
    int dh;

    if (!zi_photo_dest(app, &dest)) {
        return 0;
    }
    if (x < dest.left || y < dest.top || x >= dest.right || y >= dest.bottom) {
        return 0;
    }
    dw = dest.right - dest.left;
    dh = dest.bottom - dest.top;
    if (dw < 1 || dh < 1) {
        return 0;
    }
    *vx = (double)(x - dest.left) / (double)dw;
    *vy = (double)(y - dest.top) / (double)dh;
    if (*vx < 0) {
        *vx = 0;
    }
    if (*vy < 0) {
        *vy = 0;
    }
    if (*vx > 1) {
        *vx = 1;
    }
    if (*vy > 1) {
        *vy = 1;
    }
    return 1;
}

static void zi_horizon_source(const ZiTone *tone, double vx, double vy, double *sx, double *sy) {
    if (tone->has_crop && tone->crop_w > 0.0005 && tone->crop_h > 0.0005) {
        *sx = tone->crop_x + vx * tone->crop_w;
        *sy = tone->crop_y + vy * tone->crop_h;
        return;
    }
    *sx = vx;
    *sy = vy;
}

static void zi_horizon_click(ZiApp *app, int x, int y) {
    double vx;
    double vy;
    double sx;
    double sy;
    ZiTone next;
    int quarter = 0;
    double degrees = 0;

    if (!app->photo || app->photo_failed || !app->photo_path[0]) {
        return;
    }
    if (!zi_horizon_view(app, x, y, &vx, &vy)) {
        return;
    }
    zi_horizon_source(&app->edit, vx, vy, &sx, &sy);
    if (!app->horizon_have) {
        app->horizon_have = 1;
        app->horizon_vx = vx;
        app->horizon_vy = vy;
        app->horizon_sx = sx;
        app->horizon_sy = sy;
        zi_editor_refresh(app);
        return;
    }
    next = app->edit;
    zi_horizon_delta(app->horizon_sx, app->horizon_sy, sx, sy, &quarter, &degrees);
    app->horizon_have = 0;
    next.turns = (next.turns + quarter + 4) & 3;
    next.straighten += degrees;
    if (next.straighten > 45) {
        next.straighten = 45;
    }
    if (next.straighten < -45) {
        next.straighten = -45;
    }
    if (zi_tone_same(&app->edit, &next)) {
        zi_editor_refresh(app);
        return;
    }
    zi_editor_commit_open(app);
    zi_hist_push(app, &next, L"Horizon");
    zi_apply_edit(app, app->photo_path, &next);
}

int zi_editor_mouse_down(ZiApp *app, int x, int y, int right) {
    POINT pt;

    pt.x = x;
    pt.y = y;
    if (zi_showing_folder(app) || !PtInRect(&app->regions.canvas, pt)) {
        return 0;
    }
    if (!app->photo) {
        return 1;
    }
    if (app->select_tool) {
        if (!right) {
            zi_crop_begin(app, x, y);
        }
        return 1;
    }
    if (app->eyedropper_tool && !app->space_down) {
        if (!right) {
            zi_drop_begin(app, x, y);
        }
        return 1;
    }
    if (app->horizon_tool && !app->space_down) {
        if (!right) {
            zi_horizon_click(app, x, y);
        }
        return 1;
    }
    if ((app->spot_tool || app->scratch_tool) && !app->space_down) {
        int px = 0;
        int py = 0;

        if (right) {
            return 1;
        }
        if (app->scratch_tool) {
            app->scratch_drag = 1;
            app->scratch_count = 0;
            if (zi_view_point(app, x, y, &px, &py)) {
                app->scratch_x[0] = px;
                app->scratch_y[0] = py;
                app->scratch_count = 1;
                app->hover_on = 1;
                app->hover_x = x;
                app->hover_y = y;
            }
            if (app->hwnd) {
                SetCapture(app->hwnd);
            }
            return 1;
        }
        if (zi_view_point(app, x, y, &px, &py)) {
            int radius = zi_brush_radius(app);

            if (g_touch_inflight || g_spot_q_len > 0) {
                if (zi_spot_enqueue(app, px, py, radius) && !g_touch_inflight) {
                    zi_spot_pump(app);
                }
            } else {
                zi_touch_begin(app, 2, px, py, radius, NULL, NULL, 0);
            }
        }
        return 1;
    }
    if (app->space_down && !right) {
        app->drag_pan = 1;
        app->drag_pt.x = x;
        app->drag_pt.y = y;
        app->drag_off_x = app->zoom_off_x;
        app->drag_off_y = app->zoom_off_y;
        SetCapture(app->hwnd);
        return 1;
    }
    if (app->eyedropper_tool) {
        return 1;
    }
    zi_zoom_by(app, right ? (1.0 / ZI_SQRT2) : ZI_SQRT2, x, y);
    zi_editor_refresh(app);
    return 1;
}

void zi_editor_mouse_move(ZiApp *app, int x, int y) {
    if (app->drag_crop) {
        zi_crop_drag_to(app, x, y);
        return;
    }
    if (app->spot_tool || app->scratch_tool || app->scratch_drag) {
        zi_editor_touch_move(app, x, y);
        if (!app->drag_pan) {
            return;
        }
    }
    if (!app->drag_pan || app->dpi == 0) {
        return;
    }
    app->zoom_off_x = app->drag_off_x + (x - app->drag_pt.x) * 96.0 / (double)app->dpi;
    app->zoom_off_y = app->drag_off_y + (y - app->drag_pt.y) * 96.0 / (double)app->dpi;
    zi_editor_refresh(app);
}

void zi_editor_wheel(ZiApp *app, int x, int y, int delta) {
    int steps = delta / 120;

    if (zi_showing_folder(app) || !app->photo) {
        return;
    }
    if (steps == 0) {
        steps = delta > 0 ? 1 : -1;
    }
    while (steps > 0) {
        zi_zoom_by(app, ZI_SQRT2, x, y);
        steps--;
    }
    while (steps < 0) {
        zi_zoom_by(app, 1.0 / ZI_SQRT2, x, y);
        steps++;
    }
    zi_editor_refresh(app);
}

static void zi_blit_fit(HDC hdc, HBITMAP bitmap, int bw, int bh, int x, int y, int box_w, int box_h) {
    HDC mem;
    HGDIOBJ old;
    int dw;
    int dh;
    int dx;
    int dy;

    if (!bitmap || bw < 1 || bh < 1 || box_w < 2 || box_h < 2) {
        return;
    }
    dw = box_w;
    dh = (bh * box_w) / bw;
    if (dh > box_h) {
        dh = box_h;
        dw = (bw * box_h) / bh;
    }
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    dx = x + (box_w - dw) / 2;
    dy = y + (box_h - dh) / 2;
    mem = CreateCompatibleDC(hdc);
    old = SelectObject(mem, bitmap);
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchBlt(hdc, dx, dy, dw, dh, mem, 0, 0, bw, bh, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

static int zi_wrap(HDC hdc, HFONT font, const wchar_t *text, RECT *rc, int draw) {
    RECT measure = *rc;
    HFONT old = (HFONT)SelectObject(hdc, font);
    int height = DrawTextW(hdc, text, -1, &measure,
                           DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EXTERNALLEADING | DT_CALCRECT);

    if (height < 1) {
        height = 1;
    }
    measure.left = rc->left;
    measure.right = rc->right;
    measure.top = rc->top;
    measure.bottom = rc->top + height;
    if (draw) {
        DrawTextW(hdc, text, -1, &measure, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EXTERNALLEADING);
    }
    SelectObject(hdc, old);
    return height;
}

void zi_editor_paint_details(ZiApp *app, HDC hdc) {
    RECT column = app->regions.right;
    RECT text;
    RECT box;
    ZiFile *file = NULL;
    int y;
    int i;
    int saved;
    int width;
    const wchar_t *placeholder = app->source_server
        ? L"Select a file to see its preview and the date stored for it."
        : L"Select a file to see its preview and the dates in the file.";

    FillRect(hdc, &column, app->panel);
    if (zi_library_date_visible(app)) {
        column.bottom -= zi_px(app, 120);
        if (column.bottom < column.top) {
            column.bottom = column.top;
        }
    }
    saved = SaveDC(hdc);
    IntersectClipRect(hdc, column.left, column.top, column.right, column.bottom);
    text = column;
    InflateRect(&text, -zi_px(app, ZI_PAD), -zi_px(app, ZI_PAD));
    width = text.right - text.left;
    y = text.top;
    if (app->file_selected >= 0 && app->file_selected < app->file_count) {
        file = &app->files[app->file_selected];
    }
    if (file && file->thumb && file->thumb_w > 0 && file->thumb_h > 0 && width > 8) {
        int max_h = zi_px(app, 180);
        int dh = max_h;
        int dw = (file->thumb_w * max_h) / file->thumb_h;

        if (dw > width) {
            dw = width;
            dh = (file->thumb_h * width) / file->thumb_w;
        }
        zi_blit_fit(hdc, file->thumb, file->thumb_w, file->thumb_h, text.left, y, width, dh > 0 ? dh : 1);
        y += (dh > 0 ? dh : 1) + zi_px(app, 8);
    } else if (file && !file->video && !file->failed) {
        box.left = text.left;
        box.right = text.right;
        box.top = y;
        box.bottom = y + zi_px(app, 120);
        FillRect(hdc, &box, app->list_brush);
        SetTextColor(hdc, ZI_INK);
        DrawTextW(hdc, L"Loading preview", -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        y = box.bottom + zi_px(app, 8);
    }
    box.left = text.left;
    box.right = text.right;
    box.top = y;
    box.bottom = y + zi_px(app, 24);
    SetTextColor(hdc, ZI_INK);
    SelectObject(hdc, app->font_header);
    DrawTextW(hdc, L"Date", -1, &box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    y = box.bottom + zi_px(app, 8);
    if (!file) {
        box.left = text.left;
        box.right = text.right;
        box.top = y;
        box.bottom = column.bottom - zi_px(app, ZI_PAD);
        SelectObject(hdc, app->font);
        DrawTextW(hdc, placeholder, -1, &box, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        RestoreDC(hdc, saved);
        return;
    }
    if (app->fact_ready && _wcsicmp(app->fact_path, file->path) == 0) {
        for (i = 0; i < app->fact_count; i++) {
            box.left = text.left;
            box.right = text.right;
            box.top = y;
            box.bottom = column.bottom;
            y += zi_wrap(hdc, app->font, app->fact_lines[i], &box, 1) + zi_px(app, 6);
        }
    } else {
        box.left = text.left;
        box.right = text.right;
        box.top = y;
        box.bottom = column.bottom;
        y += zi_wrap(hdc, app->font, file->name, &box, 1) + zi_px(app, 6);
        box.top = y;
        zi_wrap(hdc, app->font, file->path, &box, 1);
    }
    RestoreDC(hdc, saved);
}

static void zi_shade_band(HDC hdc, HDC src, const RECT *canvas, int left, int top, int right, int bottom) {
    RECT band;
    RECT out;
    BLENDFUNCTION blend;

    band.left = left;
    band.top = top;
    band.right = right;
    band.bottom = bottom;
    if (!IntersectRect(&out, &band, canvas)) {
        return;
    }
    if (out.right <= out.left || out.bottom <= out.top) {
        return;
    }
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 115;
    blend.AlphaFormat = 0;
    AlphaBlend(hdc, out.left, out.top, out.right - out.left, out.bottom - out.top, src, 0, 0, 1, 1, blend);
}

static void zi_paint_marquee(ZiApp *app, HDC hdc, const RECT *canvas, const RECT *dest) {
    double left;
    double top;
    double right;
    double bottom;
    int l;
    int t;
    int r;
    int b;
    int i;
    int handle;
    int black_w;
    int white_w;
    HDC shade_dc;
    HBITMAP shade;
    HGDIOBJ old_shade;
    HPEN black;
    HPEN white_pen;
    RECT box = { 0, 0, 1, 1 };
    double marks[8][2];

    if (!app->select_tool || !app->has_marquee) {
        return;
    }
    zi_marquee_edges(app, dest, &left, &top, &right, &bottom);
    l = (int)floor(left + 0.5);
    t = (int)floor(top + 0.5);
    r = (int)floor(right + 0.5);
    b = (int)floor(bottom + 0.5);
    shade_dc = CreateCompatibleDC(hdc);
    shade = shade_dc ? CreateCompatibleBitmap(hdc, 1, 1) : NULL;
    if (shade_dc && shade) {
        old_shade = SelectObject(shade_dc, shade);
        FillRect(shade_dc, &box, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetStretchBltMode(hdc, COLORONCOLOR);
        zi_shade_band(hdc, shade_dc, canvas, canvas->left, canvas->top, canvas->right, t);
        zi_shade_band(hdc, shade_dc, canvas, canvas->left, b, canvas->right, canvas->bottom);
        zi_shade_band(hdc, shade_dc, canvas, canvas->left, t, l, b);
        zi_shade_band(hdc, shade_dc, canvas, r, t, canvas->right, b);
        SelectObject(shade_dc, old_shade);
    }
    if (shade) {
        DeleteObject(shade);
    }
    if (shade_dc) {
        DeleteDC(shade_dc);
    }
    black_w = zi_px(app, 3);
    white_w = MulDiv(3, (int)app->dpi, 192);
    if (black_w < 1) {
        black_w = 1;
    }
    if (white_w < 1) {
        white_w = 1;
    }
    black = CreatePen(PS_SOLID, black_w, RGB(0, 0, 0));
    white_pen = CreatePen(PS_SOLID, white_w, RGB(255, 255, 255));
    if (black && white_pen && r > l && b > t) {
        SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        SelectObject(hdc, black);
        Rectangle(hdc, l, t, r, b);
        SelectObject(hdc, white_pen);
        Rectangle(hdc, l, t, r, b);
        SelectObject(hdc, GetStockObject(DC_PEN));
        DeleteObject(black);
        black = CreatePen(PS_SOLID, white_w, RGB(0, 0, 0));
    }
    handle = zi_px(app, 12);
    if (handle < 4) {
        handle = 4;
    }
    marks[0][0] = left;
    marks[0][1] = top;
    marks[1][0] = right;
    marks[1][1] = top;
    marks[2][0] = right;
    marks[2][1] = bottom;
    marks[3][0] = left;
    marks[3][1] = bottom;
    marks[4][0] = (left + right) * 0.5;
    marks[4][1] = top;
    marks[5][0] = right;
    marks[5][1] = (top + bottom) * 0.5;
    marks[6][0] = (left + right) * 0.5;
    marks[6][1] = bottom;
    marks[7][0] = left;
    marks[7][1] = (top + bottom) * 0.5;
    if (app->white && black) {
        SelectObject(hdc, app->white);
        SelectObject(hdc, black);
        for (i = 0; i < 8; i++) {
            int hx = (int)floor(marks[i][0] + 0.5);
            int hy = (int)floor(marks[i][1] + 0.5);
            int half = handle / 2;

            Rectangle(hdc, hx - half, hy - half, hx - half + handle, hy - half + handle);
        }
    }
    SelectObject(hdc, GetStockObject(DC_PEN));
    if (black) {
        DeleteObject(black);
    }
    if (white_pen) {
        DeleteObject(white_pen);
    }
}

/* Gray trail while Scratch is dragged: white 0.35 at 45% opacity, brush diameter wide, round caps.
   The points are photo pixels, drawn back in the view. The photograph is not changed. */
static void zi_paint_scratch_swath(ZiApp *app, HDC hdc, const RECT *canvas, const RECT *dest) {
    POINT *pts = NULL;
    BITMAPINFO info;
    BLENDFUNCTION blend;
    RECT box;
    RECT clip;
    HDC mem = NULL;
    HBITMAP dib = NULL;
    HGDIOBJ old_bmp = NULL;
    void *bits = NULL;
    unsigned int *pix;
    double radius;
    double limit;
    int count;
    int i;
    int dw;
    int dh;
    int diameter;
    int pad;
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;
    int bw;
    int bh;

    if (!app->scratch_tool || !app->scratch_drag || app->scratch_count < 2) {
        return;
    }
    if (app->photo_w < 1 || app->photo_h < 1) {
        return;
    }
    dw = dest->right - dest->left;
    dh = dest->bottom - dest->top;
    if (dw < 1 || dh < 1) {
        return;
    }
    count = app->scratch_count;
    if (count > 4096) {
        count = 4096;
    }
    diameter = zi_touch_size_px(app);
    if (diameter < 1) {
        diameter = 1;
    }
    pts = (POINT *)malloc((size_t)count * sizeof(POINT));
    if (!pts) {
        return;
    }
    for (i = 0; i < count; i++) {
        int vx = dest->left + (int)floor(((double)app->scratch_x[i] + 0.5) * (double)dw / (double)app->photo_w);
        int vy = dest->top + (int)floor(((double)app->scratch_y[i] + 0.5) * (double)dh / (double)app->photo_h);

        pts[i].x = vx;
        pts[i].y = vy;
        if (i == 0 || vx < min_x) {
            min_x = vx;
        }
        if (i == 0 || vy < min_y) {
            min_y = vy;
        }
        if (i == 0 || vx > max_x) {
            max_x = vx;
        }
        if (i == 0 || vy > max_y) {
            max_y = vy;
        }
    }
    pad = diameter / 2 + 2;
    box.left = min_x - pad;
    box.top = min_y - pad;
    box.right = max_x + pad + 1;
    box.bottom = max_y + pad + 1;
    if (!IntersectRect(&clip, &box, canvas)) {
        free(pts);
        return;
    }
    bw = clip.right - clip.left;
    bh = clip.bottom - clip.top;
    if (bw < 1 || bh < 1 || bw > 8192 || bh > 8192) {
        free(pts);
        return;
    }
    for (i = 0; i < count; i++) {
        pts[i].x -= clip.left;
        pts[i].y -= clip.top;
    }
    memset(&info, 0, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = bw;
    info.bmiHeader.biHeight = -bh;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(hdc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    mem = CreateCompatibleDC(hdc);
    if (!dib || !mem || !bits) {
        if (dib) {
            DeleteObject(dib);
        }
        if (mem) {
            DeleteDC(mem);
        }
        free(pts);
        return;
    }
    memset(bits, 0, (size_t)bw * (size_t)bh * 4u);
    pix = (unsigned int *)bits;
    radius = 0.5 * (double)diameter;
    limit = radius * radius;
    for (i = 1; i < count; i++) {
        double x0 = (double)pts[i - 1].x;
        double y0 = (double)pts[i - 1].y;
        double x1 = (double)pts[i].x;
        double y1 = (double)pts[i].y;
        double dx = x1 - x0;
        double dy = y1 - y0;
        double len2 = dx * dx + dy * dy;
        int left = (pts[i - 1].x < pts[i].x ? pts[i - 1].x : pts[i].x) - pad;
        int top = (pts[i - 1].y < pts[i].y ? pts[i - 1].y : pts[i].y) - pad;
        int right = (pts[i - 1].x > pts[i].x ? pts[i - 1].x : pts[i].x) + pad;
        int bottom = (pts[i - 1].y > pts[i].y ? pts[i - 1].y : pts[i].y) + pad;
        int yy;

        if (left < 0) {
            left = 0;
        }
        if (top < 0) {
            top = 0;
        }
        if (right >= bw) {
            right = bw - 1;
        }
        if (bottom >= bh) {
            bottom = bh - 1;
        }
        for (yy = top; yy <= bottom; yy++) {
            int xx;

            for (xx = left; xx <= right; xx++) {
                double cx = (double)xx + 0.5;
                double cy = (double)yy + 0.5;
                double t;
                double ox;
                double oy;

                if (len2 < 0.25) {
                    ox = cx - x0;
                    oy = cy - y0;
                } else {
                    t = ((cx - x0) * dx + (cy - y0) * dy) / len2;
                    if (t < 0.0) {
                        t = 0.0;
                    } else if (t > 1.0) {
                        t = 1.0;
                    }
                    ox = cx - (x0 + t * dx);
                    oy = cy - (y0 + t * dy);
                }
                if (ox * ox + oy * oy <= limit) {
                    pix[(size_t)yy * (size_t)bw + (size_t)xx] = 0x73282828u;
                }
            }
        }
    }
    old_bmp = SelectObject(mem, dib);
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(hdc, clip.left, clip.top, bw, bh, mem, 0, 0, bw, bh, blend);
    if (old_bmp) {
        SelectObject(mem, old_bmp);
    }
    DeleteObject(dib);
    DeleteDC(mem);
    free(pts);
}

int zi_editor_paint(ZiApp *app, HDC hdc) {
    RECT canvas;
    RECT dest;
    int saved;
    int dw;
    int dh;
    HDC mem;
    HGDIOBJ old;

    if (zi_showing_folder(app) || !app->photo_path[0]) {
        return 0;
    }
    canvas = app->regions.canvas;
    if (zi_photo_dest(app, &dest)) {
        dw = dest.right - dest.left;
        dh = dest.bottom - dest.top;
        saved = SaveDC(hdc);
        IntersectClipRect(hdc, canvas.left, canvas.top, canvas.right, canvas.bottom);
        mem = CreateCompatibleDC(hdc);
        old = SelectObject(mem, app->photo);
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchBlt(hdc, dest.left, dest.top, dw, dh, mem, 0, 0, app->photo_w, app->photo_h, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        zi_paint_marquee(app, hdc, &canvas, &dest);
        if (app->horizon_tool && app->horizon_have && dw > 0 && dh > 0) {
            int hx = dest.left + (int)(app->horizon_vx * (double)dw);
            int hy = dest.top + (int)(app->horizon_vy * (double)dh);
            int arm = zi_px(app, 8);
            HPEN dark = CreatePen(PS_SOLID, 3, RGB(20, 16, 12));
            HPEN light = CreatePen(PS_SOLID, 1, RGB(255, 252, 248));
            HGDIOBJ old_pen = NULL;

            if (arm < 4) {
                arm = 4;
            }
            if (dark) {
                old_pen = SelectObject(hdc, dark);
                MoveToEx(hdc, hx - arm, hy, NULL);
                LineTo(hdc, hx + arm, hy);
                MoveToEx(hdc, hx, hy - arm, NULL);
                LineTo(hdc, hx, hy + arm);
            }
            if (light) {
                SelectObject(hdc, light);
                MoveToEx(hdc, hx - arm, hy, NULL);
                LineTo(hdc, hx + arm, hy);
                MoveToEx(hdc, hx, hy - arm, NULL);
                LineTo(hdc, hx, hy + arm);
            }
            if (old_pen) {
                SelectObject(hdc, old_pen);
            }
            if (dark) {
                DeleteObject(dark);
            }
            if (light) {
                DeleteObject(light);
            }
        }
        zi_paint_scratch_swath(app, hdc, &canvas, &dest);
        if ((app->spot_tool || app->scratch_tool) && app->hover_on) {
            int diameter = zi_touch_size_px(app);
            int left = app->hover_x - diameter / 2;
            int top = app->hover_y - diameter / 2;
            HPEN dark = CreatePen(PS_SOLID, 2, RGB(20, 16, 12));
            HPEN light = CreatePen(PS_SOLID, 1, RGB(255, 252, 248));
            HGDIOBJ old_pen = NULL;
            HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

            if (dark) {
                old_pen = SelectObject(hdc, dark);
                Ellipse(hdc, left, top, left + diameter, top + diameter);
            }
            if (light) {
                SelectObject(hdc, light);
                Ellipse(hdc, left, top, left + diameter, top + diameter);
            }
            if (old_pen) {
                SelectObject(hdc, old_pen);
            }
            SelectObject(hdc, old_brush);
            if (dark) {
                DeleteObject(dark);
            }
            if (light) {
                DeleteObject(light);
            }
        }
        RestoreDC(hdc, saved);
    } else {
        const wchar_t *message = app->photo_failed
            ? L"This photograph could not be opened."
            : L"Opening this photograph.";
        RECT message_rc = canvas;

        InflateRect(&message_rc, -zi_px(app, 24), -zi_px(app, 24));
        SetTextColor(hdc, ZI_WELL_INK);
        SelectObject(hdc, app->font);
        DrawTextW(hdc, message, -1, &message_rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    }
    if (app->notice[0]) {
        RECT bar = canvas;
        RECT label;

        bar.bottom = bar.top + zi_px(app, 28);
        FillRect(hdc, &bar, app->panel);
        label = bar;
        InflateRect(&label, -zi_px(app, 12), 0);
        SetTextColor(hdc, ZI_INK);
        SelectObject(hdc, app->font);
        DrawTextW(hdc, app->notice, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    return 1;
}

static int zi_jpeg_memory(IWICImagingFactory *factory, HBITMAP bitmap, float quality, int long_edge,
                          unsigned char **out, size_t *out_len) {
    IWICBitmap *wic = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapSource *source = NULL;
    IStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *props = NULL;
    PROPBAG2 option;
    VARIANT setting;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    STATSTG stat;
    LARGE_INTEGER zero;
    UINT w = 0;
    UINT h = 0;
    UINT nw;
    UINT nh;
    ULONG got = 0;
    int ok = 0;

    if (out) {
        *out = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (!factory || !bitmap || !out || !out_len) {
        return 0;
    }
    if (FAILED(factory->lpVtbl->CreateBitmapFromHBITMAP(factory, bitmap, NULL, WICBitmapIgnoreAlpha, &wic)) || !wic) {
        goto done;
    }
    if (FAILED(wic->lpVtbl->GetSize(wic, &w, &h)) || w == 0 || h == 0) {
        goto done;
    }
    source = (IWICBitmapSource *)wic;
    nw = w;
    nh = h;
    if (long_edge > 0 && (w > (UINT)long_edge || h > (UINT)long_edge)) {
        if (w >= h) {
            nw = (UINT)long_edge;
            nh = (UINT)(((unsigned long long)h * (unsigned)long_edge) / w);
        } else {
            nh = (UINT)long_edge;
            nw = (UINT)(((unsigned long long)w * (unsigned)long_edge) / h);
        }
        if (nw < 1) {
            nw = 1;
        }
        if (nh < 1) {
            nh = 1;
        }
        if (FAILED(factory->lpVtbl->CreateBitmapScaler(factory, &scaler)) || !scaler ||
            FAILED(scaler->lpVtbl->Initialize(scaler, source, nw, nh, WICBitmapInterpolationModeFant))) {
            goto done;
        }
        source = (IWICBitmapSource *)scaler;
    }
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) || !stream) {
        goto done;
    }
    if (FAILED(factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatJpeg, NULL, &encoder)) ||
        FAILED(encoder->lpVtbl->Initialize(encoder, stream, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &props))) {
        goto done;
    }
    ZeroMemory(&option, sizeof option);
    option.pstrName = L"ImageQuality";
    option.dwType = PROPBAG2_TYPE_DATA;
    VariantInit(&setting);
    setting.vt = VT_R4;
    setting.fltVal = quality;
    if (props) {
        props->lpVtbl->Write(props, 1, &option, &setting);
    }
    VariantClear(&setting);
    if (FAILED(frame->lpVtbl->Initialize(frame, props)) || FAILED(frame->lpVtbl->SetSize(frame, nw, nh))) {
        goto done;
    }
    frame->lpVtbl->SetPixelFormat(frame, &format);
    if (FAILED(frame->lpVtbl->WriteSource(frame, source, NULL)) || FAILED(frame->lpVtbl->Commit(frame)) ||
        FAILED(encoder->lpVtbl->Commit(encoder))) {
        goto done;
    }
    ZeroMemory(&stat, sizeof stat);
    if (FAILED(stream->lpVtbl->Stat(stream, &stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart <= 0 ||
        stat.cbSize.QuadPart > 64ll * 1024ll * 1024ll) {
        goto done;
    }
    *out = (unsigned char *)malloc((size_t)stat.cbSize.QuadPart);
    if (!*out) {
        goto done;
    }
    zero.QuadPart = 0;
    if (FAILED(stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL)) ||
        FAILED(stream->lpVtbl->Read(stream, *out, (ULONG)stat.cbSize.QuadPart, &got)) || got == 0) {
        free(*out);
        *out = NULL;
        goto done;
    }
    *out_len = got;
    ok = 1;
done:
    if (frame) {
        frame->lpVtbl->Release(frame);
    }
    if (props) {
        props->lpVtbl->Release(props);
    }
    if (encoder) {
        encoder->lpVtbl->Release(encoder);
    }
    if (stream) {
        stream->lpVtbl->Release(stream);
    }
    if (scaler) {
        scaler->lpVtbl->Release(scaler);
    }
    if (wic) {
        wic->lpVtbl->Release(wic);
    }
    return ok;
}

static int zi_encode_wic(IWICImagingFactory *factory, HBITMAP bitmap, const wchar_t *path, const GUID *container,
                         float quality, int lossy, int tiff_lzw) {
    IWICBitmap *wic = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *props = NULL;
    PROPBAG2 option;
    VARIANT setting;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    UINT w = 0;
    UINT h = 0;
    int ok = 0;

    if (FAILED(factory->lpVtbl->CreateBitmapFromHBITMAP(factory, bitmap, NULL, WICBitmapIgnoreAlpha, &wic)) || !wic) {
        goto done;
    }
    if (FAILED(wic->lpVtbl->GetSize(wic, &w, &h)) || w == 0 || h == 0) {
        goto done;
    }
    if (FAILED(factory->lpVtbl->CreateStream(factory, &stream)) ||
        FAILED(stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE))) {
        goto done;
    }
    if (FAILED(factory->lpVtbl->CreateEncoder(factory, container, NULL, &encoder)) ||
        FAILED(encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &props))) {
        DeleteFileW(path);
        goto done;
    }
    ZeroMemory(&option, sizeof option);
    option.dwType = PROPBAG2_TYPE_DATA;
    VariantInit(&setting);
    if (lossy) {
        option.pstrName = L"ImageQuality";
        setting.vt = VT_R4;
        setting.fltVal = quality;
        if (props) {
            props->lpVtbl->Write(props, 1, &option, &setting);
        }
    }
    if (tiff_lzw) {
        option.pstrName = L"TiffCompressionMethod";
        setting.vt = VT_UI1;
        setting.bVal = WICTiffCompressionLZW;
        if (props) {
            props->lpVtbl->Write(props, 1, &option, &setting);
        }
    }
    VariantClear(&setting);
    if (FAILED(frame->lpVtbl->Initialize(frame, props)) || FAILED(frame->lpVtbl->SetSize(frame, w, h))) {
        DeleteFileW(path);
        goto done;
    }
    frame->lpVtbl->SetPixelFormat(frame, &format);
    if (FAILED(frame->lpVtbl->WriteSource(frame, (IWICBitmapSource *)wic, NULL)) ||
        FAILED(frame->lpVtbl->Commit(frame)) || FAILED(encoder->lpVtbl->Commit(encoder))) {
        DeleteFileW(path);
        goto done;
    }
    ok = 1;
done:
    if (frame) {
        frame->lpVtbl->Release(frame);
    }
    if (props) {
        props->lpVtbl->Release(props);
    }
    if (encoder) {
        encoder->lpVtbl->Release(encoder);
    }
    if (stream) {
        stream->lpVtbl->Release(stream);
    }
    if (wic) {
        wic->lpVtbl->Release(wic);
    }
    return ok;
}

static int zi_write_tga(IWICImagingFactory *factory, HBITMAP bitmap, const wchar_t *path) {
    IWICBitmap *wic = NULL;
    UINT w = 0;
    UINT h = 0;
    UINT stride;
    BYTE header[18];
    BYTE *pixels = NULL;
    HANDLE file;
    DWORD wrote = 0;
    int ok = 0;

    if (FAILED(factory->lpVtbl->CreateBitmapFromHBITMAP(factory, bitmap, NULL, WICBitmapIgnoreAlpha, &wic)) || !wic) {
        return 0;
    }
    if (FAILED(wic->lpVtbl->GetSize(wic, &w, &h)) || w == 0 || h == 0 || w > 65535 || h > 65535) {
        wic->lpVtbl->Release(wic);
        return 0;
    }
    stride = w * 3;
    pixels = (BYTE *)malloc((size_t)stride * h);
    if (!pixels || FAILED(wic->lpVtbl->CopyPixels(wic, NULL, stride, stride * h, pixels))) {
        free(pixels);
        wic->lpVtbl->Release(wic);
        return 0;
    }
    wic->lpVtbl->Release(wic);
    ZeroMemory(header, sizeof header);
    header[2] = 2;
    header[12] = (BYTE)(w & 0xff);
    header[13] = (BYTE)((w >> 8) & 0xff);
    header[14] = (BYTE)(h & 0xff);
    header[15] = (BYTE)((h >> 8) & 0xff);
    header[16] = 24;
    header[17] = 0x20;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        free(pixels);
        return 0;
    }
    if (WriteFile(file, header, sizeof header, &wrote, NULL) && wrote == sizeof header &&
        WriteFile(file, pixels, stride * h, &wrote, NULL) && wrote == stride * h) {
        ok = 1;
    }
    CloseHandle(file);
    free(pixels);
    if (!ok) {
        DeleteFileW(path);
    }
    return ok;
}

static int zi_write_pdf(IWICImagingFactory *factory, HBITMAP bitmap, const wchar_t *path) {
    BITMAP bm;
    unsigned char *jpeg = NULL;
    size_t jpeg_len = 0;
    unsigned char *pdf = NULL;
    size_t len = 0;
    size_t cap = 0;
    int off[5];
    int xref;
    int i;
    char text[320];
    char content[96];
    int content_len;
    int w;
    int h;
    HANDLE file;
    DWORD wrote = 0;
    int ok = 0;

    if (GetObject(bitmap, sizeof bm, &bm) != (int)sizeof bm) {
        return 0;
    }
    w = bm.bmWidth;
    h = bm.bmHeight;
    if (w < 1 || h < 1 || !zi_jpeg_memory(factory, bitmap, 0.9f, 0, &jpeg, &jpeg_len)) {
        return 0;
    }
    content_len = sprintf_s(content, sizeof content, "q\n%d 0 0 %d 0 0 cm\n/Im Do\nQ\n", w, h);
    if (content_len < 0) {
        free(jpeg);
        return 0;
    }
#define ZI_PDF_ADD(bytes, n)                                                          \
    do {                                                                              \
        size_t need_ = (size_t)(n);                                                   \
        while (len + need_ + 1 > cap) {                                               \
            size_t next_ = cap ? cap * 2 : 4096;                                      \
            unsigned char *grown_;                                                    \
            if (next_ < len + need_ + 1) {                                            \
                next_ = len + need_ + 1;                                              \
            }                                                                         \
            grown_ = (unsigned char *)realloc(pdf, next_);                            \
            if (!grown_) {                                                            \
                free(pdf);                                                            \
                free(jpeg);                                                           \
                return 0;                                                             \
            }                                                                         \
            pdf = grown_;                                                             \
            cap = next_;                                                              \
        }                                                                             \
        memcpy(pdf + len, (bytes), need_);                                            \
        len += need_;                                                                 \
    } while (0)

    ZI_PDF_ADD("%PDF-1.4\n", 9);
    off[0] = (int)len;
    ZI_PDF_ADD("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
               strlen("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"));
    off[1] = (int)len;
    ZI_PDF_ADD("2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n",
               strlen("2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n"));
    off[2] = (int)len;
    sprintf_s(text, sizeof text,
              "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
              "/Contents 4 0 R /Resources << /XObject << /Im 5 0 R >> >> >>\nendobj\n",
              w, h);
    ZI_PDF_ADD(text, strlen(text));
    off[3] = (int)len;
    sprintf_s(text, sizeof text, "4 0 obj\n<< /Length %d >>\nstream\n", content_len);
    ZI_PDF_ADD(text, strlen(text));
    ZI_PDF_ADD(content, (size_t)content_len);
    ZI_PDF_ADD("endstream\nendobj\n", strlen("endstream\nendobj\n"));
    off[4] = (int)len;
    sprintf_s(text, sizeof text,
              "5 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
              "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %zu >>\nstream\n",
              w, h, jpeg_len);
    ZI_PDF_ADD(text, strlen(text));
    ZI_PDF_ADD(jpeg, jpeg_len);
    ZI_PDF_ADD("\nendstream\nendobj\n", strlen("\nendstream\nendobj\n"));
    xref = (int)len;
    ZI_PDF_ADD("xref\n0 6\n0000000000 65535 f \n", strlen("xref\n0 6\n0000000000 65535 f \n"));
    for (i = 0; i < 5; i++) {
        sprintf_s(text, sizeof text, "%010d 00000 n \n", off[i]);
        ZI_PDF_ADD(text, strlen(text));
    }
    sprintf_s(text, sizeof text, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n", xref);
    ZI_PDF_ADD(text, strlen(text));
#undef ZI_PDF_ADD
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        free(pdf);
        free(jpeg);
        return 0;
    }
    if (WriteFile(file, pdf, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len) {
        ok = 1;
    }
    CloseHandle(file);
    free(pdf);
    free(jpeg);
    if (!ok) {
        DeleteFileW(path);
    }
    return ok;
}

static void zi_stem(const wchar_t *path, wchar_t *dest, size_t cap) {
    const wchar_t *base = wcsrchr(path, L'\\');
    wchar_t *dot;

    base = (base && base[1]) ? base + 1 : path;
    wcscpy_s(dest, cap, base);
    dot = wcsrchr(dest, L'.');
    if (dot && dot != dest) {
        *dot = L'\0';
    }
}

static const wchar_t *const k_fmt_name[] = {
    L"JPEG", L"HEIC", L"TIFF", L"PNG", L"WebP", L"AVIF", L"JPEG XL", L"JPEG 2000",
    L"PSD", L"OpenEXR", L"BMP", L"GIF", L"TGA", L"PDF", L"DNG"
};
static const wchar_t *const k_fmt_ext[] = {
    L"jpg", L"heic", L"tiff", L"png", L"webp", L"avif", L"jxl", L"jp2",
    L"psd", L"exr", L"bmp", L"gif", L"tga", L"pdf", L"dng"
};
static int g_probe_heic = -1;
static int g_probe_webp = -1;
static int g_probe_jxl = -1;

static int zi_probe_encoder(IWICImagingFactory *factory, const GUID *container, int *cache) {
    IWICBitmapEncoder *encoder = NULL;
    HRESULT hr;

    if (*cache >= 0) {
        return *cache;
    }
    hr = factory->lpVtbl->CreateEncoder(factory, container, NULL, &encoder);
    if (encoder) {
        encoder->lpVtbl->Release(encoder);
    }
    *cache = SUCCEEDED(hr) ? 1 : 0;
    return *cache;
}

static int zi_format_ready(IWICImagingFactory *factory, int format) {
    if (format < 0 || format > 14) {
        return 0;
    }
    if (format == 5 || format == 7 || format == 8 || format == 9 || format == 14) {
        return 0;
    }
    if (format == 1) {
        return factory && zi_probe_encoder(factory, &GUID_ContainerFormatHeif, &g_probe_heic);
    }
    if (format == 4) {
        return factory && zi_probe_encoder(factory, &GUID_ContainerFormatWebp, &g_probe_webp);
    }
    if (format == 6) {
        return factory && zi_probe_encoder(factory, &GUID_ContainerFormatJpegXL, &g_probe_jxl);
    }
    return 1;
}

static int zi_write_format(IWICImagingFactory *factory, HBITMAP bitmap, const wchar_t *path, int format) {
    int ok;

    switch (format) {
    case 0:
        return zi_encode_wic(factory, bitmap, path, &GUID_ContainerFormatJpeg, 0.9f, 1, 0);
    case 1:
        return zi_encode_wic(factory, bitmap, path, &GUID_ContainerFormatHeif, 0.8f, 1, 0);
    case 2:
        ok = zi_encode_wic(factory, bitmap, path, &GUID_ContainerFormatTiff, 0, 0, 1);
        if (ok) {
            return 1;
        }
        return zi_encode_wic(factory, bitmap, path, &GUID_ContainerFormatTiff, 0, 0, 0);
    case 3:
        return zi_encode_wic(factory, bitmap, path, &GUID_ContainerFormatPng, 0, 0, 0);
    case 4:
        return zi_encode_wic(factory, bitmap, path, &GUID_ContainerFormatWebp, 0.8f, 1, 0);
    case 6:
        return zi_encode_wic(factory, bitmap, path, &GUID_ContainerFormatJpegXL, 0.8f, 1, 0);
    case 10:
        return zi_encode_wic(factory, bitmap, path, &GUID_ContainerFormatBmp, 0, 0, 0);
    case 11:
        return zi_encode_wic(factory, bitmap, path, &GUID_ContainerFormatGif, 0, 0, 0);
    case 12:
        return zi_write_tga(factory, bitmap, path);
    case 13:
        return zi_write_pdf(factory, bitmap, path);
    default:
        return 0;
    }
}

typedef struct ZiExportEvents {
    IFileDialogEventsVtbl *lpVtbl;
    LONG refs;
} ZiExportEvents;

static HRESULT STDMETHODCALLTYPE zi_evt_query(IFileDialogEvents *self, REFIID riid, void **out) {
    if (!out) {
        return E_POINTER;
    }
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IFileDialogEvents)) {
        *out = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE zi_evt_add(IFileDialogEvents *self) {
    return (ULONG)InterlockedIncrement(&((ZiExportEvents *)self)->refs);
}

static ULONG STDMETHODCALLTYPE zi_evt_release(IFileDialogEvents *self) {
    return (ULONG)InterlockedDecrement(&((ZiExportEvents *)self)->refs);
}

static HRESULT STDMETHODCALLTYPE zi_evt_ok(IFileDialogEvents *self, IFileDialog *dialog) {
    (void)self;
    (void)dialog;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE zi_evt_folder_changing(IFileDialogEvents *self, IFileDialog *dialog, IShellItem *item) {
    (void)self;
    (void)dialog;
    (void)item;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE zi_evt_folder(IFileDialogEvents *self, IFileDialog *dialog) {
    (void)self;
    (void)dialog;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE zi_evt_selection(IFileDialogEvents *self, IFileDialog *dialog) {
    (void)self;
    (void)dialog;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE zi_evt_share(IFileDialogEvents *self, IFileDialog *dialog, IShellItem *item,
                                              FDE_SHAREVIOLATION_RESPONSE *response) {
    (void)self;
    (void)dialog;
    (void)item;
    if (response) {
        *response = FDESVR_DEFAULT;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE zi_evt_type(IFileDialogEvents *self, IFileDialog *dialog) {
    UINT index = 1;
    IFileSaveDialog *save = NULL;

    (void)self;
    if (!dialog || FAILED(dialog->lpVtbl->GetFileTypeIndex(dialog, &index))) {
        return S_OK;
    }
    if (index < 1 || index > 15) {
        return S_OK;
    }
    if (SUCCEEDED(dialog->lpVtbl->QueryInterface(dialog, &IID_IFileSaveDialog, (void **)&save)) && save) {
        save->lpVtbl->SetDefaultExtension(save, k_fmt_ext[index - 1]);
        save->lpVtbl->Release(save);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE zi_evt_overwrite(IFileDialogEvents *self, IFileDialog *dialog, IShellItem *item,
                                                  FDE_OVERWRITE_RESPONSE *response) {
    (void)self;
    (void)dialog;
    (void)item;
    if (response) {
        *response = FDEOR_DEFAULT;
    }
    return S_OK;
}

static IFileDialogEventsVtbl g_export_vtbl = {
    zi_evt_query, zi_evt_add, zi_evt_release, zi_evt_ok, zi_evt_folder_changing,
    zi_evt_folder, zi_evt_selection, zi_evt_share, zi_evt_type, zi_evt_overwrite
};

static void zi_force_ext(wchar_t *path, size_t cap, const wchar_t *ext) {
    wchar_t *slash;
    wchar_t *dot;

    if (!path || !ext || !ext[0]) {
        return;
    }
    slash = wcsrchr(path, L'\\');
    dot = wcsrchr(path, L'.');
    if (dot && (!slash || dot > slash)) {
        if (_wcsicmp(dot + 1, ext) == 0) {
            return;
        }
        if ((size_t)(dot - path) + 1 + wcslen(ext) >= cap) {
            return;
        }
        wcscpy_s(dot + 1, cap - (size_t)(dot + 1 - path), ext);
    } else if (wcslen(path) + 1 + wcslen(ext) < cap) {
        wcscat_s(path, cap, L".");
        wcscat_s(path, cap, ext);
    }
}

static int zi_pick_export(ZiApp *app, wchar_t *dest, size_t cap, const wchar_t *name, int *format) {
    IFileSaveDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    DWORD options = 0;
    DWORD cookie = 0;
    UINT index = 1;
    HRESULT hr;
    int ok = 0;
    ZiExportEvents events;
    static const COMDLG_FILTERSPEC k_types[] = {
        { L"JPEG", L"*.jpg" },
        { L"HEIC", L"*.heic" },
        { L"TIFF", L"*.tiff" },
        { L"PNG", L"*.png" },
        { L"WebP", L"*.webp" },
        { L"AVIF", L"*.avif" },
        { L"JPEG XL", L"*.jxl" },
        { L"JPEG 2000", L"*.jp2" },
        { L"PSD", L"*.psd" },
        { L"OpenEXR", L"*.exr" },
        { L"BMP", L"*.bmp" },
        { L"GIF", L"*.gif" },
        { L"TGA", L"*.tga" },
        { L"PDF", L"*.pdf" },
        { L"DNG", L"*.dng" }
    };

    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        return 0;
    }
    events.lpVtbl = &g_export_vtbl;
    events.refs = 1;
    dlg->lpVtbl->GetOptions(dlg, &options);
    dlg->lpVtbl->SetOptions(dlg, options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    dlg->lpVtbl->SetTitle(dlg, L"Export");
    dlg->lpVtbl->SetFileTypes(dlg, 15, k_types);
    dlg->lpVtbl->SetFileTypeIndex(dlg, (UINT)g_export_format + 1);
    dlg->lpVtbl->SetDefaultExtension(dlg, k_fmt_ext[g_export_format]);
    dlg->lpVtbl->SetFileName(dlg, name);
    dlg->lpVtbl->Advise(dlg, (IFileDialogEvents *)&events, &cookie);
    hr = dlg->lpVtbl->Show(dlg, app->hwnd);
    if (SUCCEEDED(hr)) {
        hr = dlg->lpVtbl->GetResult(dlg, &item);
    }
    if (SUCCEEDED(hr) && item) {
        hr = item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path);
    }
    if (SUCCEEDED(dlg->lpVtbl->GetFileTypeIndex(dlg, &index)) && index >= 1 && index <= 15) {
        g_export_format = (int)index - 1;
    }
    if (SUCCEEDED(hr) && path && wcslen(path) < cap) {
        wcscpy_s(dest, cap, path);
        zi_force_ext(dest, cap, k_fmt_ext[g_export_format]);
        ok = 1;
    }
    if (format) {
        *format = g_export_format;
    }
    if (cookie) {
        dlg->lpVtbl->Unadvise(dlg, cookie);
    }
    if (path) {
        CoTaskMemFree(path);
    }
    if (item) {
        item->lpVtbl->Release(item);
    }
    dlg->lpVtbl->Release(dlg);
    return ok;
}

static int zi_pick_format(ZiApp *app) {
    HMENU menu;
    POINT pt;
    int i;
    int cmd;

    menu = CreatePopupMenu();
    if (!menu) {
        return 0;
    }
    for (i = 0; i < 15; i++) {
        AppendMenuW(menu, MF_STRING | (i == g_export_format ? MF_CHECKED : 0), (UINT_PTR)(i + 1), k_fmt_name[i]);
    }
    GetCursorPos(&pt);
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, app->hwnd, NULL);
    DestroyMenu(menu);
    if (cmd < 1 || cmd > 15) {
        return 0;
    }
    g_export_format = cmd - 1;
    return 1;
}

static int zi_pick_folder(ZiApp *app, wchar_t *dest, size_t cap) {
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    DWORD options = 0;
    HRESULT hr;
    int ok = 0;

    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        return 0;
    }
    dlg->lpVtbl->GetOptions(dlg, &options);
    dlg->lpVtbl->SetOptions(dlg, options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_PICKFOLDERS);
    dlg->lpVtbl->SetTitle(dlg, L"Export");
    hr = dlg->lpVtbl->Show(dlg, app->hwnd);
    if (SUCCEEDED(hr)) {
        hr = dlg->lpVtbl->GetResult(dlg, &item);
    }
    if (SUCCEEDED(hr) && item) {
        hr = item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path);
    }
    if (SUCCEEDED(hr) && path && wcslen(path) < cap) {
        wcscpy_s(dest, cap, path);
        ok = 1;
    }
    if (path) {
        CoTaskMemFree(path);
    }
    if (item) {
        item->lpVtbl->Release(item);
    }
    dlg->lpVtbl->Release(dlg);
    return ok;
}

static int zi_export_dest(const wchar_t *folder, const wchar_t *source, const wchar_t *ext, wchar_t *dest, size_t cap) {
    wchar_t stem[ZI_NAME];

    zi_stem(source, stem, ZI_NAME);
    if (wcslen(folder) + wcslen(stem) + wcslen(ext) + 4 >= cap) {
        return 0;
    }
    swprintf_s(dest, cap, L"%s\\%s.%s", folder, stem, ext);
    if (_wcsicmp(dest, source) == 0) {
        if (wcslen(folder) + wcslen(stem) + wcslen(ext) + 14 >= cap) {
            return 0;
        }
        swprintf_s(dest, cap, L"%s\\%s exported.%s", folder, stem, ext);
    }
    if (_wcsicmp(dest, source) == 0) {
        return 0;
    }
    return 1;
}

static HBITMAP zi_load_graded(IWICImagingFactory *factory, const wchar_t *source, const ZiTone *tone,
                             const ZiTouchDoc *touch, int *out_w, int *out_h) {
    HBITMAP bitmap;
    HBITMAP graded;
    BITMAP bm;
    int edge = ZI_EXPORT_EDGE;
    int w = 0;
    int h = 0;
    int repair = zi_touch_doc_visible(touch);

    if (!tone) {
        return NULL;
    }
    if (zi_tone_needs_pipeline(tone) || repair) {
        edge = ZI_GRADE_EDGE;
    }
    bitmap = zi_decode_file(factory, source, edge, 0, &w, &h);
    if (!bitmap) {
        return NULL;
    }
    if (!zi_tone_needs_pipeline(tone) && !repair) {
        graded = zi_copy_turned(bitmap, tone->turns);
        DeleteObject(bitmap);
        if (!graded) {
            return NULL;
        }
        if (GetObject(graded, sizeof bm, &bm) == (int)sizeof bm) {
            w = bm.bmWidth;
            h = bm.bmHeight;
        }
        if (out_w) {
            *out_w = w;
        }
        if (out_h) {
            *out_h = h;
        }
        return graded;
    }
    graded = zi_tone_render(bitmap, tone, touch, out_w, out_h, NULL);
    DeleteObject(bitmap);
    return graded;
}

static int zi_export_one(IWICImagingFactory *factory, const wchar_t *source, const ZiTone *tone,
                        const ZiTouchDoc *touch, const wchar_t *dest, int format) {
    HBITMAP bitmap;
    int w = 0;
    int h = 0;
    int ok;

    if (_wcsicmp(dest, source) == 0) {
        return 0;
    }
    bitmap = zi_load_graded(factory, source, tone, touch, &w, &h);
    if (!bitmap) {
        return 0;
    }
    ok = zi_write_format(factory, bitmap, dest, format);
    DeleteObject(bitmap);
    return ok;
}

static int zi_count_targets(const ZiApp *app) {
    int i;
    int n = 0;

    if (!zi_showing_folder(app)) {
        return app->photo_path[0] ? 1 : 0;
    }
    for (i = 0; i < app->file_count; i++) {
        if (app->file_mark[i] && !app->files[i].video) {
            n++;
        }
    }
    return n;
}

void zi_editor_export(ZiApp *app) {
    IWICImagingFactory *factory = NULL;
    wchar_t dest[MAX_PATH];
    wchar_t folder[MAX_PATH];
    wchar_t suggested[ZI_NAME];
    HCURSOR previous;
    int count;
    int exported = 0;
    int failed = 0;
    int i;
    wchar_t message[128];

    count = zi_count_targets(app);
    if (count < 1) {
        zi_editor_notice(app, L"Select a still.");
        return;
    }
    if (count == 1) {
        const wchar_t *source = app->photo_path;
        ZiTone tone = app->edit;
        int format = g_export_format;

        if (zi_showing_folder(app)) {
            zi_tone_identity(&tone);
            for (i = 0; i < app->file_count; i++) {
                if (app->file_mark[i] && !app->files[i].video) {
                    source = app->files[i].path;
                    tone = zi_edit_get(source);
                    break;
                }
            }
        }
        zi_stem(source, suggested, ZI_NAME);
        wcscat_s(suggested, ZI_NAME, L" exported.");
        wcscat_s(suggested, ZI_NAME, k_fmt_ext[g_export_format]);
        if (!zi_pick_export(app, dest, MAX_PATH, suggested, &format)) {
            return;
        }
        if (_wcsicmp(dest, source) == 0) {
            zi_editor_notice(app, L"Export writes a new file. Choose a different name.");
            return;
        }
        previous = SetCursor(LoadCursor(NULL, IDC_WAIT));
        CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
        if (!zi_format_ready(factory, format)) {
            swprintf_s(message, 128, L"%s is not available on this PC.", k_fmt_name[format]);
            if (factory) {
                factory->lpVtbl->Release(factory);
            }
            SetCursor(previous);
            zi_editor_notice(app, message);
            return;
        }
        if (factory && zi_export_one(factory, source, &tone,
                                     (!zi_showing_folder(app) && source == app->photo_path) ? &g_live : NULL,
                                     dest, format)) {
            exported = 1;
        } else {
            failed = 1;
        }
        if (factory) {
            factory->lpVtbl->Release(factory);
        }
        SetCursor(previous);
    } else {
        if (!zi_pick_format(app)) {
            return;
        }
        CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
        if (!zi_format_ready(factory, g_export_format)) {
            swprintf_s(message, 128, L"%s is not available on this PC.", k_fmt_name[g_export_format]);
            if (factory) {
                factory->lpVtbl->Release(factory);
            }
            zi_editor_notice(app, message);
            return;
        }
        if (factory) {
            factory->lpVtbl->Release(factory);
            factory = NULL;
        }
        if (!zi_pick_folder(app, folder, MAX_PATH)) {
            return;
        }
        previous = SetCursor(LoadCursor(NULL, IDC_WAIT));
        CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
        for (i = 0; factory && i < app->file_count; i++) {
            ZiTone tone;

            if (!app->file_mark[i] || app->files[i].video) {
                continue;
            }
            tone = zi_edit_get(app->files[i].path);
            if (!zi_export_dest(folder, app->files[i].path, k_fmt_ext[g_export_format], dest, MAX_PATH) ||
                !zi_export_one(factory, app->files[i].path, &tone, NULL, dest, g_export_format)) {
                failed++;
            } else {
                exported++;
            }
        }
        if (!factory) {
            failed = count;
        }
        if (factory) {
            factory->lpVtbl->Release(factory);
        }
        SetCursor(previous);
    }
    if (exported > 0 && failed == 0) {
        swprintf_s(message, 128, exported == 1 ? L"Exported 1 photograph." : L"Exported %d photographs.", exported);
    } else if (exported > 0) {
        swprintf_s(message, 128, L"Exported %d photographs. %d could not be exported.", exported, failed);
    } else {
        wcscpy_s(message, 128, L"That photograph could not be exported.");
    }
    zi_editor_notice(app, message);
}

static int zi_print_bitmap(HDC printer, HBITMAP bitmap, int bw, int bh) {
    int page_w = GetDeviceCaps(printer, HORZRES);
    int page_h = GetDeviceCaps(printer, VERTRES);
    int margin_x = page_w / 40;
    int margin_y = page_h / 40;
    int box_w = page_w - margin_x * 2;
    int box_h = page_h - margin_y * 2;

    if (StartPage(printer) <= 0) {
        return 0;
    }
    zi_blit_fit(printer, bitmap, bw, bh, margin_x, margin_y, box_w, box_h);
    return EndPage(printer) > 0;
}

void zi_editor_print(ZiApp *app) {
    PRINTDLGW dialog;
    DOCINFOW document;
    IWICImagingFactory *factory = NULL;
    HCURSOR previous;
    int printed = 0;
    int failed = 0;
    int i;
    wchar_t message[128];

    if (zi_count_targets(app) < 1) {
        zi_editor_notice(app, L"Select a still.");
        return;
    }
    ZeroMemory(&dialog, sizeof dialog);
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = app->hwnd;
    dialog.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
    dialog.nCopies = 1;
    dialog.nFromPage = 1;
    dialog.nToPage = 1;
    dialog.nMinPage = 1;
    dialog.nMaxPage = 1;
    if (!PrintDlgW(&dialog)) {
        if (CommDlgExtendedError() != 0) {
            zi_editor_notice(app, L"Print could not start.");
        }
        return;
    }
    ZeroMemory(&document, sizeof document);
    document.cbSize = sizeof document;
    document.lpszDocName = L"ZImager";
    previous = SetCursor(LoadCursor(NULL, IDC_WAIT));
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
    if (factory && StartDocW(dialog.hDC, &document) > 0) {
        if (!zi_showing_folder(app)) {
            HBITMAP bitmap;
            int w = 0;
            int h = 0;

            bitmap = zi_load_graded(factory, app->photo_path, &app->edit, &g_live, &w, &h);
            if (bitmap && zi_print_bitmap(dialog.hDC, bitmap, w, h)) {
                printed = 1;
            } else {
                failed = 1;
            }
            if (bitmap) {
                DeleteObject(bitmap);
            }
        } else {
            for (i = 0; i < app->file_count; i++) {
                HBITMAP bitmap;
                ZiTone tone;
                int w = 0;
                int h = 0;

                if (!app->file_mark[i] || app->files[i].video) {
                    continue;
                }
                tone = zi_edit_get(app->files[i].path);
                bitmap = zi_load_graded(factory, app->files[i].path, &tone, NULL, &w, &h);
                if (bitmap && zi_print_bitmap(dialog.hDC, bitmap, w, h)) {
                    printed++;
                } else {
                    failed++;
                }
                if (bitmap) {
                    DeleteObject(bitmap);
                }
            }
        }
        EndDoc(dialog.hDC);
    } else {
        failed = 1;
    }
    if (factory) {
        factory->lpVtbl->Release(factory);
    }
    if (dialog.hDC) {
        DeleteDC(dialog.hDC);
    }
    if (dialog.hDevMode) {
        GlobalFree(dialog.hDevMode);
    }
    if (dialog.hDevNames) {
        GlobalFree(dialog.hDevNames);
    }
    SetCursor(previous);
    if (printed > 0 && failed == 0) {
        swprintf_s(message, 128, printed == 1 ? L"Printed 1 photograph." : L"Printed %d photographs.", printed);
    } else if (printed > 0) {
        swprintf_s(message, 128, L"Printed %d photographs. %d could not be printed.", printed, failed);
    } else {
        wcscpy_s(message, 128, L"That photograph could not be printed.");
    }
    zi_editor_notice(app, message);
}

typedef struct ZiJob {
    struct ZiJob *next;
    int kind;
    HANDLE done;
    int ok;
    HWND hwnd;
    LONG generation;
    long serial;
    wchar_t path[MAX_PATH];
    wchar_t dest[MAX_PATH];
    int remember;
    ZiHistStep *hist;
    ZiTouchDoc *docs;
    int len;
    int pos;
    unsigned char *preview;
    size_t preview_len;
    ZiTone tone;
    wchar_t label[40];
} ZiJob;

static INIT_ONCE g_work_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_work_lock;
static CONDITION_VARIABLE g_work_cv;
static HANDLE g_work_idle;
static ZiJob *g_job_head;
static ZiJob *g_job_tail;
static int g_work_started;
static long g_edit_serial;
static wchar_t g_cache_path[MAX_PATH];
static unsigned char g_cache_digest[32];
static unsigned long long g_cache_size;
static int g_cache_ok;

static BOOL CALLBACK zi_work_init(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_work_lock);
    InitializeConditionVariable(&g_work_cv);
    g_work_idle = CreateEventW(NULL, TRUE, TRUE, NULL);
    return TRUE;
}

static int zi_worker_digest(const wchar_t *path, unsigned char digest[32], unsigned long long *size) {
    WIN32_FILE_ATTRIBUTE_DATA info;

    if (g_cache_ok && _wcsicmp(g_cache_path, path) == 0 &&
        GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
        unsigned long long bytes = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;

        if (bytes == g_cache_size) {
            memcpy(digest, g_cache_digest, 32);
            *size = g_cache_size;
            return 1;
        }
    }
    if (!zi_blake3_file(path, digest, size)) {
        g_cache_ok = 0;
        return 0;
    }
    wcscpy_s(g_cache_path, MAX_PATH, path);
    memcpy(g_cache_digest, digest, 32);
    g_cache_size = *size;
    g_cache_ok = 1;
    return 1;
}

static void zi_job_release(ZiJob *job) {
    int i;

    if (!job) {
        return;
    }
    if (job->docs) {
        for (i = 0; i < ZI_HIST; i++) {
            zi_touch_doc_free(&job->docs[i]);
        }
        free(job->docs);
    }
    free(job->hist);
    free(job->preview);
    job->docs = NULL;
    job->hist = NULL;
    job->preview = NULL;
}

static DWORD WINAPI zi_worker(LPVOID param) {
    (void)param;
    for (;;) {
        ZiJob *job;

        EnterCriticalSection(&g_work_lock);
        while (!g_job_head) {
            if (g_work_idle) {
                SetEvent(g_work_idle);
            }
            SleepConditionVariableCS(&g_work_cv, &g_work_lock, INFINITE);
            if (g_work_idle) {
                ResetEvent(g_work_idle);
            }
        }
        job = g_job_head;
        g_job_head = job->next;
        if (!g_job_head) {
            g_job_tail = NULL;
        }
        LeaveCriticalSection(&g_work_lock);
        if (job->kind == 3) {
            ZiDigestNote *note = (ZiDigestNote *)calloc(1, sizeof *note);

            if (note) {
                note->generation = job->generation;
                wcscpy_s(note->path, MAX_PATH, job->path);
                note->ok = zi_worker_digest(job->path, note->digest, &note->size);
                if (!g_closed && job->hwnd && PostMessageW(job->hwnd, ZI_WM_DIGEST, 0, (LPARAM)note)) {
                    note = NULL;
                }
                free(note);
            }
        } else if (job->kind == 2) {
            job->ok = zi_zimg_commit_tone(job->path, &job->tone, job->label, job->serial);
        } else if (job->hist && job->len > 0) {
            unsigned char digest[32];
            unsigned long long size = 0;

            if (zi_worker_digest(job->path, digest, &size)) {
                if (job->kind == 4 && job->dest[0]) {
                    job->ok = zi_zimg_write_at(job->path, job->dest, digest, size, job->hist, job->len, job->pos,
                                               job->docs, job->preview, job->preview_len, job->remember, job->serial);
                } else {
                    job->ok = zi_zimg_write_hist(job->path, digest, size, job->hist, job->len, job->pos, job->docs,
                                                 job->preview, job->preview_len, job->serial);
                }
            }
        }
        zi_job_release(job);
        if (job->done) {
            SetEvent(job->done);
        } else {
            free(job);
        }
    }
}

static void zi_worker_ensure(void) {
    InitOnceExecuteOnce(&g_work_once, zi_work_init, NULL, NULL);
    EnterCriticalSection(&g_work_lock);
    if (!g_work_started) {
        HANDLE thread = CreateThread(NULL, 0, zi_worker, NULL, 0, NULL);

        if (thread) {
            CloseHandle(thread);
            g_work_started = 1;
        }
    }
    LeaveCriticalSection(&g_work_lock);
}

static void zi_job_submit(ZiJob *job) {
    zi_worker_ensure();
    EnterCriticalSection(&g_work_lock);
    job->next = NULL;
    if (g_job_tail) {
        g_job_tail->next = job;
    } else {
        g_job_head = job;
    }
    g_job_tail = job;
    if (g_work_idle) {
        ResetEvent(g_work_idle);
    }
    WakeConditionVariable(&g_work_cv);
    LeaveCriticalSection(&g_work_lock);
}

static int zi_preview_jpeg(HBITMAP bitmap, unsigned char **out, size_t *out_len) {
    IWICImagingFactory *factory = NULL;
    int ok = 0;

    *out = NULL;
    *out_len = 0;
    if (!bitmap) {
        return 0;
    }
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
    if (factory) {
        ok = zi_jpeg_memory(factory, bitmap, 0.85f, 2048, out, out_len);
        factory->lpVtbl->Release(factory);
    }
    return ok;
}

static int zi_hist_has_edit(const ZiApp *app) {
    ZiTone bare;
    int i;

    zi_tone_identity(&bare);
    for (i = 0; i < app->hist_len; i++) {
        if (!zi_tone_same(&app->hist[i].tone, &bare)) {
            return 1;
        }
        if (i < ZI_HIST && zi_touch_doc_active(&g_steps[i])) {
            return 1;
        }
    }
    return 0;
}

static ZiJob *zi_job_from_hist(ZiApp *app, int kind) {
    ZiJob *job;
    int i;

    if (!app || !app->photo_path[0] || app->hist_len < 1 || g_closed) {
        return NULL;
    }
    job = (ZiJob *)calloc(1, sizeof *job);
    if (!job) {
        return NULL;
    }
    job->kind = kind;
    job->serial = InterlockedIncrement(&g_edit_serial);
    job->generation = app->photo_generation;
    wcscpy_s(job->path, MAX_PATH, app->photo_path);
    job->hist = (ZiHistStep *)malloc(sizeof(ZiHistStep) * (size_t)ZI_HIST);
    if (!job->hist) {
        free(job);
        return NULL;
    }
    memcpy(job->hist, app->hist, sizeof(ZiHistStep) * (size_t)app->hist_len);
    job->docs = (ZiTouchDoc *)calloc((size_t)ZI_HIST, sizeof(ZiTouchDoc));
    if (!job->docs) {
        free(job->hist);
        free(job);
        return NULL;
    }
    for (i = 0; i < app->hist_len && i < ZI_HIST; i++) {
        zi_touch_doc_copy(&job->docs[i], &g_steps[i]);
    }
    job->len = app->hist_len;
    job->pos = app->hist_pos;
    if (app->photo) {
        zi_preview_jpeg(app->photo, &job->preview, &job->preview_len);
    }
    return job;
}

static void zi_edits_note(ZiApp *app) {
    ZiJob *job;

    if (g_loading || !zi_hist_has_edit(app)) {
        return;
    }
    g_hold = 1;
    job = zi_job_from_hist(app, 1);
    if (job) {
        zi_job_submit(job);
    }
}

static void zi_queue_commit(const wchar_t *path, const ZiTone *tone, const wchar_t *label) {
    ZiJob *job;

    if (!path || !path[0] || !tone || g_closed) {
        return;
    }
    job = (ZiJob *)calloc(1, sizeof *job);
    if (!job) {
        return;
    }
    job->kind = 2;
    job->serial = InterlockedIncrement(&g_edit_serial);
    wcscpy_s(job->path, MAX_PATH, path);
    job->tone = *tone;
    wcscpy_s(job->label, 40, label && label[0] ? label : L"Edit");
    zi_job_submit(job);
}

static void zi_queue_hash(ZiApp *app) {
    ZiJob *job;

    if (!app || !app->hwnd || !app->photo_path[0] || g_closed) {
        return;
    }
    job = (ZiJob *)calloc(1, sizeof *job);
    if (!job) {
        return;
    }
    job->kind = 3;
    job->hwnd = app->hwnd;
    job->generation = app->photo_generation;
    wcscpy_s(job->path, MAX_PATH, app->photo_path);
    zi_job_submit(job);
}

static int zi_save_wait(ZiApp *app, const wchar_t *dest, int remember) {
    ZiJob *job;
    HANDLE done;
    DWORD wait;
    int ok = 0;
    HCURSOR previous;

    job = zi_job_from_hist(app, dest && dest[0] ? 4 : 1);
    if (!job) {
        return 0;
    }
    if (dest && dest[0]) {
        wcscpy_s(job->dest, MAX_PATH, dest);
        job->remember = remember;
    }
    done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!done) {
        zi_job_release(job);
        free(job);
        return 0;
    }
    job->done = done;
    g_hold = 1;
    previous = SetCursor(LoadCursor(NULL, IDC_WAIT));
    zi_job_submit(job);
    wait = WaitForSingleObject(done, 180000);
    if (wait == WAIT_OBJECT_0) {
        ok = job->ok;
        zi_job_release(job);
        free(job);
    }
    CloseHandle(done);
    SetCursor(previous);
    return ok;
}

static void zi_install_hist(ZiApp *app, const ZiHistStep *hist, int len, int pos, const ZiTouchDoc *docs) {
    int i;

    if (!app || !hist || len < 1 || len > ZI_HIST) {
        return;
    }
    if (pos < 0) {
        pos = 0;
    }
    if (pos >= len) {
        pos = len - 1;
    }
    g_loading = 1;
    zi_docs_free_all();
    memcpy(app->hist, hist, sizeof(ZiHistStep) * (size_t)len);
    app->hist_len = len;
    app->hist_pos = pos;
    if (docs) {
        for (i = 0; i < len; i++) {
            zi_touch_doc_copy(&g_steps[i], &docs[i]);
        }
    }
    zi_live_from_pos(app);
    g_loading = 0;
    zi_apply_edit(app, app->photo_path, &app->hist[pos].tone);
}

void zi_editor_on_digest(ZiApp *app, ZiDigestNote *note) {
    if (!note) {
        return;
    }
    if (g_closed || !app || !note->ok || note->generation != app->photo_generation || !app->photo_path[0] ||
        _wcsicmp(note->path, app->photo_path) != 0) {
        free(note);
        return;
    }
    if (g_doc_wait && g_doc_path[0] && _wcsicmp(g_doc_path, note->path) == 0) {
        g_doc_wait = 0;
        if (g_doc_len > 0 && note->size == g_doc_size && memcmp(note->digest, g_doc_digest, 32) == 0) {
            g_hold = 1;
            zi_install_hist(app, g_doc_hist, g_doc_len, g_doc_pos, g_doc_docs);
        } else {
            zi_editor_notice(app, L"The photo on disk does not match this document. The edits were not applied.");
        }
        zi_doc_stash_free();
        free(note);
        return;
    }
    if (!g_hold) {
        ZiHistStep *loaded = (ZiHistStep *)calloc((size_t)ZI_HIST, sizeof(ZiHistStep));
        ZiTouchDoc *loaded_docs = (ZiTouchDoc *)calloc((size_t)ZI_HIST, sizeof(ZiTouchDoc));
        int len = 0;
        int pos = 0;
        ZiTone memo;
        ZiTone bare;

        zi_tone_identity(&bare);
        memo = zi_edit_get(note->path);
        if (loaded && loaded_docs &&
            zi_zimg_load_hist(note->path, note->digest, note->size, loaded, ZI_HIST, &len, &pos, loaded_docs)) {
            if (zi_tone_same(&memo, &bare) || zi_tone_same(&memo, &loaded[pos].tone)) {
                zi_install_hist(app, loaded, len, pos, loaded_docs);
            } else {
                g_hold = 1;
                zi_edits_note(app);
            }
        }
        if (loaded_docs) {
            for (len = 0; len < ZI_HIST; len++) {
                zi_touch_doc_free(&loaded_docs[len]);
            }
        }
        free(loaded_docs);
        free(loaded);
    }
    free(note);
}

static int zi_file_is_file(const wchar_t *path) {
    DWORD attr;

    if (!path || !path[0]) {
        return 0;
    }
    attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static int zi_locate_photo(ZiApp *app, wchar_t *dest, size_t cap) {
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    DWORD options = 0;
    HRESULT hr;
    int ok = 0;

    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        return 0;
    }
    dlg->lpVtbl->GetOptions(dlg, &options);
    dlg->lpVtbl->SetOptions(dlg, options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    dlg->lpVtbl->SetTitle(dlg, L"Locate the original photo");
    hr = dlg->lpVtbl->Show(dlg, app->hwnd);
    if (SUCCEEDED(hr)) {
        hr = dlg->lpVtbl->GetResult(dlg, &item);
    }
    if (SUCCEEDED(hr) && item) {
        hr = item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path);
    }
    if (SUCCEEDED(hr) && path && wcslen(path) < cap) {
        wcscpy_s(dest, cap, path);
        ok = 1;
    }
    if (path) {
        CoTaskMemFree(path);
    }
    if (item) {
        item->lpVtbl->Release(item);
    }
    dlg->lpVtbl->Release(dlg);
    return ok;
}

static int zi_library_photo(const ZiApp *app, const wchar_t *hint, wchar_t *dest, size_t cap) {
    const wchar_t *p;
    const wchar_t *tail = NULL;
    wchar_t root[MAX_PATH];
    wchar_t *slash;

    if (!hint || !hint[0]) {
        return 0;
    }
    if (zi_file_is_file(hint) && wcslen(hint) < cap) {
        wcscpy_s(dest, cap, hint);
        return 1;
    }
    for (p = hint; *p; p++) {
        if (_wcsnicmp(p, L"PhotoVideoAudio", 15) == 0) {
            p += 15;
            while (*p == L'/' || *p == L'\\') {
                p++;
            }
            tail = p;
            break;
        }
    }
    if (!tail || !tail[0]) {
        return 0;
    }
    root[0] = L'\0';
    if (app->library_root[0]) {
        wcscpy_s(root, MAX_PATH, app->library_root);
    } else if (GetFileAttributesW(L"\\\\192.168.1.147\\BackupData\\PhotoVideoAudio") != INVALID_FILE_ATTRIBUTES) {
        wcscpy_s(root, MAX_PATH, L"\\\\192.168.1.147\\BackupData\\PhotoVideoAudio");
    }
    if (!root[0] || swprintf_s(dest, cap, L"%s\\%s", root, tail) < 0) {
        return 0;
    }
    for (slash = dest; *slash; slash++) {
        if (*slash == L'/') {
            *slash = L'\\';
        }
    }
    return zi_file_is_file(dest);
}

static int zi_spill_embedded(const ZiZimgInfo *info, wchar_t *dest, size_t cap) {
    wchar_t folder[MAX_PATH];
    char hex[65];
    wchar_t name[80];
    const wchar_t *ext = L".bin";
    HANDLE file;
    const unsigned char *bytes;
    size_t left;
    DWORD wrote = 0;

    if (!info->embedded || info->embedded_len == 0 || !GetTempPathW(MAX_PATH, folder)) {
        return 0;
    }
    if (strcmp(info->uti, "public.jpeg") == 0) {
        ext = L".jpg";
    } else if (strcmp(info->uti, "public.png") == 0) {
        ext = L".png";
    } else if (strcmp(info->uti, "public.tiff") == 0) {
        ext = L".tiff";
    } else if (strcmp(info->uti, "public.heic") == 0) {
        ext = L".heic";
    }
    if (info->has_digest) {
        zi_blake3_hex(info->digest, hex);
        if (MultiByteToWideChar(CP_UTF8, 0, hex, -1, name, 80) <= 0) {
            wcscpy_s(name, 80, L"embedded");
        }
    } else {
        wcscpy_s(name, 80, L"embedded");
    }
    if (swprintf_s(dest, cap, L"%sZImageWin-%s%s", folder, name, ext) < 0) {
        return 0;
    }
    file = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    bytes = info->embedded;
    left = info->embedded_len;
    while (left > 0) {
        DWORD chunk = left > (1u << 20) ? (1u << 20) : (DWORD)left;

        if (!WriteFile(file, bytes, chunk, &wrote, NULL) || wrote == 0) {
            CloseHandle(file);
            DeleteFileW(dest);
            return 0;
        }
        bytes += wrote;
        left -= wrote;
    }
    CloseHandle(file);
    return 1;
}

void zi_editor_open_zimg(ZiApp *app, const wchar_t *path) {
    ZiZimgInfo info;
    wchar_t photo[MAX_PATH];

    photo[0] = L'\0';
    if (!path || !zi_zimg_parse(path, &info) || !info.has_digest) {
        zi_zimg_info_free(&info);
        zi_editor_notice(app, L"The edits could not be opened.");
        return;
    }
    if (info.kind == 2) {
        zi_spill_embedded(&info, photo, MAX_PATH);
    }
    if (!photo[0] && info.kind == 0) {
        zi_library_photo(app, info.path_hint, photo, MAX_PATH);
    }
    if (!photo[0]) {
        zi_library_photo(app, info.relative_hint[0] ? info.relative_hint : info.path_hint, photo, MAX_PATH);
    }
    if (!photo[0] && !zi_locate_photo(app, photo, MAX_PATH)) {
        zi_zimg_info_free(&info);
        zi_editor_notice(app, L"The original photo could not be found. Locate it to open the edits.");
        return;
    }
    if (info.hist_len < 1 || info.hist_len > ZI_HIST) {
        zi_zimg_info_free(&info);
        zi_editor_notice(app, L"The edits could not be opened.");
        return;
    }
    memcpy(g_doc_hist, info.hist, sizeof(ZiHistStep) * (size_t)info.hist_len);
    g_doc_len = info.hist_len;
    g_doc_pos = info.hist_pos;
    memcpy(g_doc_digest, info.digest, 32);
    g_doc_size = info.byte_size;
    wcscpy_s(g_doc_path, MAX_PATH, photo);
    zi_doc_stash_free();
    if (info.touch) {
        ZiTouchDoc *src = (ZiTouchDoc *)info.touch;
        int i;
        int n = info.hist_len;

        if (n > ZI_HIST) {
            n = ZI_HIST;
        }
        for (i = 0; i < n; i++) {
            zi_touch_doc_move(&g_doc_docs[i], &src[i]);
        }
        free(src);
        info.touch = NULL;
    }
    g_doc_wait = 1;
    zi_zimg_info_free(&info);
    if (app->photo_path[0] && _wcsicmp(app->photo_path, photo) == 0 && (app->photo || app->photo_busy) &&
        !app->photo_failed) {
        g_hold = 0;
        zi_queue_hash(app);
        return;
    }
    zi_editor_open_path(app, photo);
}

void zi_editor_save(ZiApp *app) {
    if (!app->photo_path[0]) {
        return;
    }
    if (!zi_hist_has_edit(app)) {
        zi_editor_notice(app, L"Edits saved.");
        return;
    }
    zi_editor_notice(app, zi_save_wait(app, NULL, 0) ? L"Edits saved." : L"The edits could not be saved.");
}

void zi_editor_save_as(ZiApp *app) {
    IFileSaveDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR picked = NULL;
    wchar_t dest[MAX_PATH];
    wchar_t suggested[ZI_NAME];
    DWORD options = 0;
    HRESULT hr;
    static const COMDLG_FILTERSPEC k_zimg = { L"ZImager edits", L"*.zimg" };

    if (!app->photo_path[0]) {
        return;
    }
    zi_stem(app->photo_path, suggested, ZI_NAME);
    wcscat_s(suggested, ZI_NAME, L".zimg");
    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        zi_editor_notice(app, L"The edits could not be saved.");
        return;
    }
    dlg->lpVtbl->GetOptions(dlg, &options);
    dlg->lpVtbl->SetOptions(dlg, options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    dlg->lpVtbl->SetTitle(dlg, L"The photo folder is read-only. Choose another folder, or use Save to keep the edits on this Mac.");
    dlg->lpVtbl->SetFileTypes(dlg, 1, &k_zimg);
    dlg->lpVtbl->SetDefaultExtension(dlg, L"zimg");
    dlg->lpVtbl->SetFileName(dlg, suggested);
    hr = dlg->lpVtbl->Show(dlg, app->hwnd);
    if (SUCCEEDED(hr)) {
        hr = dlg->lpVtbl->GetResult(dlg, &item);
    }
    if (SUCCEEDED(hr) && item) {
        hr = item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &picked);
    }
    dest[0] = L'\0';
    if (SUCCEEDED(hr) && picked && wcslen(picked) < MAX_PATH) {
        wcscpy_s(dest, MAX_PATH, picked);
        zi_force_ext(dest, MAX_PATH, L"zimg");
    }
    if (picked) {
        CoTaskMemFree(picked);
    }
    if (item) {
        item->lpVtbl->Release(item);
    }
    dlg->lpVtbl->Release(dlg);
    if (!dest[0]) {
        return;
    }
    if (!zi_hist_has_edit(app)) {
        zi_editor_notice(app, L"Edits saved.");
        return;
    }
    zi_editor_notice(app, zi_save_wait(app, dest, 1) ? L"Edits saved." : L"The edits could not be saved.");
}

void zi_editor_shutdown(ZiApp *app) {
    if (app && g_hold) {
        zi_edits_note(app);
    }
    if (g_work_started && g_work_idle) {
        WaitForSingleObject(g_work_idle, 600000);
    }
    g_closed = 1;
    g_editor_alive = 0;
    g_spot_q_len = 0;
    g_spot_running = 0;
    zi_grade_q_clear();
    InterlockedIncrement(&g_photo_generation);
    InterlockedIncrement(&g_fact_generation);
    InterlockedIncrement(&g_grade_serial);
    InterlockedIncrement(&g_auto_serial);
    InterlockedIncrement(&g_drop_serial);
    zi_docs_free_all();
    zi_editor_free_bitmaps(app);
    zi_plain_free_all();
    zi_touch_shutdown();
}
