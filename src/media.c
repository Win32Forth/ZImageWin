/* Video playback, X3F embedded JPEG, and PDF embedded JPEG pages.
   media.c does not include initguid.h. Media Foundation GUIDs come from mfuuid.lib.
   Graded playback shows the picture and stays silent. Export writes a new MPEG-4 of
   the graded frames and does not change the original file. */
#include "media.h"
#include "tone.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

/* The SDK helpers are C++ inline methods. Width and the ratio numerator are the high half. */
static HRESULT zi_mf_get_pair(IMFMediaType *type, REFGUID key, UINT32 *high, UINT32 *low) {
    UINT64 packed = 0;
    HRESULT hr;

    if (!type || !high || !low) {
        return E_POINTER;
    }
    hr = type->lpVtbl->GetUINT64(type, key, &packed);
    if (FAILED(hr)) {
        return hr;
    }
    *high = (UINT32)(packed >> 32);
    *low = (UINT32)(packed & 0xffffffffu);
    return S_OK;
}

static HRESULT zi_mf_set_pair(IMFMediaType *type, REFGUID key, UINT32 high, UINT32 low) {
    UINT64 packed = ((UINT64)high << 32) | (UINT64)low;

    if (!type) {
        return E_POINTER;
    }
    return type->lpVtbl->SetUINT64(type, key, packed);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZI_VIDEO_TIMER 3
#define ZI_GRADE_SLOTS 48
#define ZI_X3F_BUDGET (128u << 20)
#define ZI_X3F_EDGE 20000
#define ZI_X3F_PIXELS 16000000
#define ZI_PDF_CAP (48u << 20)

static const wchar_t *const k_videos[] = {
    L"mov", L"mp4", L"m4v", L"m4p", L"avi", L"mkv", L"mpg", L"mpeg", L"mpe", L"m2v",
    L"wmv", L"flv", L"webm", L"3gp", L"3g2", L"mts", L"m2ts", L"ts", L"vob", L"ogv", L"asf", L"divx"
};

static INIT_ONCE g_media_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_media_lock;
static IMFSourceReader *g_reader;
static HBITMAP g_raw;
static HBITMAP g_show;
static int g_frame_w;
static int g_frame_h;
static int g_playing;
static int g_mf_started;
static LONGLONG g_duration;
static LONGLONG g_position;
static LARGE_INTEGER g_clock_freq;
static LARGE_INTEGER g_clock_start;
static LONGLONG g_clock_media;
static wchar_t g_open_path[MAX_PATH];
static RECT g_bar;
static int g_bar_ready;
static ZiTone g_grades[ZI_GRADE_SLOTS];
static wchar_t g_grade_paths[ZI_GRADE_SLOTS][MAX_PATH];
static int g_grade_used[ZI_GRADE_SLOTS];
static int g_grade_next;

static INIT_ONCE g_pdf_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_pdf_lock;
static wchar_t g_pdf_path[MAX_PATH];
static int *g_pdf_pages;
static unsigned char **g_pdf_jpegs;
static size_t *g_pdf_lengths;
static int g_pdf_count;

static BOOL CALLBACK zi_media_init_lock(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_media_lock);
    return TRUE;
}

static void zi_media_lock(void) {
    InitOnceExecuteOnce(&g_media_once, zi_media_init_lock, NULL, NULL);
    EnterCriticalSection(&g_media_lock);
}

static void zi_media_unlock(void) {
    LeaveCriticalSection(&g_media_lock);
}

static BOOL CALLBACK zi_pdf_init_lock(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_pdf_lock);
    return TRUE;
}

static void zi_pdf_lock(void) {
    InitOnceExecuteOnce(&g_pdf_once, zi_pdf_init_lock, NULL, NULL);
    EnterCriticalSection(&g_pdf_lock);
}

static void zi_pdf_unlock(void) {
    LeaveCriticalSection(&g_pdf_lock);
}

static int zi_ext_is(const wchar_t *path, const wchar_t *ext) {
    const wchar_t *dot = path ? wcsrchr(path, L'.') : NULL;

    return dot && dot[1] && _wcsicmp(dot + 1, ext) == 0;
}

int zi_media_is_video(const wchar_t *path) {
    int i;

    if (!path) {
        return 0;
    }
    for (i = 0; i < (int)(sizeof k_videos / sizeof k_videos[0]); i++) {
        if (zi_ext_is(path, k_videos[i])) {
            return 1;
        }
    }
    return 0;
}

static void zi_notice(ZiApp *app, const wchar_t *text) {
    if (!app) {
        return;
    }
    if (!text) {
        app->notice[0] = L'\0';
        return;
    }
    wcscpy_s(app->notice, 192, text);
}

static void zi_grade_store(const wchar_t *path, const ZiTone *tone) {
    int i;
    int slot = -1;

    if (!path || !path[0] || !tone) {
        return;
    }
    for (i = 0; i < ZI_GRADE_SLOTS; i++) {
        if (g_grade_used[i] && _wcsicmp(g_grade_paths[i], path) == 0) {
            g_grades[i] = *tone;
            return;
        }
    }
    for (i = 0; i < ZI_GRADE_SLOTS; i++) {
        if (!g_grade_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = g_grade_next++ % ZI_GRADE_SLOTS;
    }
    wcscpy_s(g_grade_paths[slot], MAX_PATH, path);
    g_grades[slot] = *tone;
    g_grade_used[slot] = 1;
}

static int zi_grade_load(const wchar_t *path, ZiTone *tone) {
    int i;

    for (i = 0; i < ZI_GRADE_SLOTS; i++) {
        if (g_grade_used[i] && _wcsicmp(g_grade_paths[i], path) == 0) {
            *tone = g_grades[i];
            return 1;
        }
    }
    return 0;
}

static void zi_release_bitmap(HBITMAP *bitmap) {
    if (bitmap && *bitmap) {
        DeleteObject(*bitmap);
        *bitmap = NULL;
    }
}

static HBITMAP zi_dib_from_bgra(const unsigned char *data, int width, int height, int stride) {
    BITMAPINFO info;
    HBITMAP bitmap;
    void *bits = NULL;
    int y;
    unsigned char *dest;

    if (!data || width < 1 || height < 1) {
        return NULL;
    }
    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return NULL;
    }
    dest = (unsigned char *)bits;
    for (y = 0; y < height; y++) {
        const unsigned char *src;
        int row = stride >= 0 ? y : (height - 1 - y);

        src = data + (size_t)row * (size_t)(stride >= 0 ? stride : -stride);
        memcpy(dest + (size_t)y * (size_t)width * 4u, src, (size_t)width * 4u);
    }
    return bitmap;
}

static HBITMAP zi_grade_bitmap(HBITMAP raw, const ZiTone *tone, int *out_w, int *out_h) {
    ZiTone use;
    HBITMAP graded;
    int w = 0;
    int h = 0;

    if (!raw || !tone) {
        return NULL;
    }
    use = *tone;
    use.turns = 0;
    use.flip_h = 0;
    use.flip_v = 0;
    use.offset_x = 0;
    use.offset_y = 0;
    use.straighten = 0;
    use.vertical = 0;
    use.horizontal = 0;
    use.sharpness = 0;
    use.has_crop = 0;
    use.crop_w = 0;
    use.crop_h = 0;
    use.has_corners = 0;
    use.k1 = 0;
    use.k2 = 0;
    use.fov = 0;
    use.lens = 0;
    use.has_skin = 0;
    if (!zi_tone_needs_pipeline(&use)) {
        return NULL;
    }
    graded = zi_tone_render(raw, &use, NULL, &w, &h, NULL);
    if (!graded || w < 1 || h < 1) {
        if (graded) {
            DeleteObject(graded);
        }
        return NULL;
    }
    if (out_w) {
        *out_w = w;
    }
    if (out_h) {
        *out_h = h;
    }
    return graded;
}

static void zi_show_current(const ZiTone *tone) {
    HBITMAP graded;
    int w = 0;
    int h = 0;
    BITMAP info;

    zi_release_bitmap(&g_show);
    if (!g_raw) {
        g_frame_w = 0;
        g_frame_h = 0;
        return;
    }
    graded = zi_grade_bitmap(g_raw, tone, &w, &h);
    if (graded) {
        g_show = graded;
        g_frame_w = w;
        g_frame_h = h;
        return;
    }
    ZeroMemory(&info, sizeof info);
    if (GetObject(g_raw, sizeof info, &info) == sizeof info) {
        g_frame_w = info.bmWidth;
        g_frame_h = info.bmHeight < 0 ? -info.bmHeight : info.bmHeight;
    }
}

static int zi_pull_frame(HBITMAP *out, LONGLONG *time, int *ended) {
    IMFSample *sample = NULL;
    IMFMediaBuffer *buffer = NULL;
    IMFMediaType *type = NULL;
    DWORD flags = 0;
    LONGLONG stamp = 0;
    BYTE *data = NULL;
    DWORD max_len = 0;
    DWORD cur_len = 0;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 stride_u = 0;
    int stride;
    HRESULT hr;
    HBITMAP bitmap = NULL;

    if (ended) {
        *ended = 0;
    }
    if (!g_reader || !out) {
        return 0;
    }
    *out = NULL;
    hr = g_reader->lpVtbl->ReadSample(g_reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, &stamp, &sample);
    if (FAILED(hr)) {
        return 0;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        if (ended) {
            *ended = 1;
        }
        if (sample) {
            sample->lpVtbl->Release(sample);
        }
        return 1;
    }
    if (!sample) {
        return 1;
    }
    if (FAILED(g_reader->lpVtbl->GetCurrentMediaType(g_reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)) ||
        !type || FAILED(zi_mf_get_pair(type, &MF_MT_FRAME_SIZE, &width, &height)) || width < 1 || height < 1) {
        if (type) {
            type->lpVtbl->Release(type);
        }
        sample->lpVtbl->Release(sample);
        return 0;
    }
    stride = (int)width * 4;
    if (SUCCEEDED(type->lpVtbl->GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &stride_u))) {
        stride = (int)stride_u;
    }
    type->lpVtbl->Release(type);
    if (FAILED(sample->lpVtbl->ConvertToContiguousBuffer(sample, &buffer)) || !buffer ||
        FAILED(buffer->lpVtbl->Lock(buffer, &data, &max_len, &cur_len)) || !data) {
        if (buffer) {
            buffer->lpVtbl->Release(buffer);
        }
        sample->lpVtbl->Release(sample);
        return 0;
    }
    bitmap = zi_dib_from_bgra(data, (int)width, (int)height, stride);
    buffer->lpVtbl->Unlock(buffer);
    buffer->lpVtbl->Release(buffer);
    sample->lpVtbl->Release(sample);
    if (!bitmap) {
        return 0;
    }
    *out = bitmap;
    if (time) {
        *time = stamp;
    }
    g_position = stamp;
    return 1;
}

static int zi_reader_open(const wchar_t *path) {
    IMFAttributes *attr = NULL;
    IMFMediaType *type = NULL;
    IMFSourceReader *reader = NULL;
    PROPVARIANT duration;
    HRESULT hr;

    if (!g_mf_started) {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
            return 0;
        }
        g_mf_started = 1;
    }
    if (FAILED(MFCreateAttributes(&attr, 2)) || !attr) {
        return 0;
    }
    attr->lpVtbl->SetUINT32(attr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    hr = MFCreateSourceReaderFromURL(path, attr, &reader);
    attr->lpVtbl->Release(attr);
    if (FAILED(hr) || !reader) {
        return 0;
    }
    if (FAILED(MFCreateMediaType(&type)) || !type) {
        reader->lpVtbl->Release(reader);
        return 0;
    }
    type->lpVtbl->SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    type->lpVtbl->SetGUID(type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    hr = reader->lpVtbl->SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, type);
    type->lpVtbl->Release(type);
    if (FAILED(hr)) {
        reader->lpVtbl->Release(reader);
        return 0;
    }
    reader->lpVtbl->SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->lpVtbl->SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    PropVariantInit(&duration);
    g_duration = 0;
    if (SUCCEEDED(reader->lpVtbl->GetPresentationAttribute(reader, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8) {
        g_duration = (LONGLONG)duration.uhVal.QuadPart;
    }
    PropVariantClear(&duration);
    if (g_reader) {
        g_reader->lpVtbl->Release(g_reader);
    }
    g_reader = reader;
    return 1;
}

static void zi_stop_timer(ZiApp *app) {
    if (app && app->hwnd) {
        KillTimer(app->hwnd, ZI_VIDEO_TIMER);
    }
}

void zi_media_close(ZiApp *app) {
    zi_media_lock();
    g_playing = 0;
    zi_stop_timer(app);
    zi_release_bitmap(&g_raw);
    zi_release_bitmap(&g_show);
    g_frame_w = 0;
    g_frame_h = 0;
    g_position = 0;
    g_duration = 0;
    g_bar_ready = 0;
    g_open_path[0] = L'\0';
    if (g_reader) {
        g_reader->lpVtbl->Release(g_reader);
        g_reader = NULL;
    }
    zi_media_unlock();
    if (app) {
        app->video_open = 0;
    }
}

int zi_media_open(ZiApp *app, const wchar_t *path) {
    HBITMAP frame = NULL;
    int ended = 0;
    ZiTone tone;

    if (!app || !path || !path[0] || !zi_media_is_video(path)) {
        if (app) {
            zi_notice(app, L"This video could not be played on this PC.");
        }
        return 0;
    }
    zi_media_close(app);
    zi_tone_identity(&tone);
    zi_grade_load(path, &tone);
    if (!zi_reader_open(path) || !zi_pull_frame(&frame, NULL, &ended) || !frame) {
        zi_media_close(app);
        zi_notice(app, L"This video could not be played on this PC.");
        return 0;
    }
    g_raw = frame;
    zi_show_current(&tone);
    wcscpy_s(g_open_path, MAX_PATH, path);
    wcscpy_s(app->photo_path, MAX_PATH, path);
    wcscpy_s(app->opened, MAX_PATH, path);
    app->edit = tone;
    app->hist[0].tone = tone;
    wcscpy_s(app->hist[0].label, 40, L"Open");
    app->hist_len = 1;
    app->hist_pos = 0;
    app->photo_failed = 0;
    app->photo_busy = 0;
    app->video_open = 1;
    app->pdf_picking = 0;
    app->events_open = 0;
    app->reorg_open = 0;
    if (app->source_server) {
        app->server_folder = 0;
    } else {
        app->disk_folder = 0;
    }
    g_playing = 1;
    QueryPerformanceFrequency(&g_clock_freq);
    QueryPerformanceCounter(&g_clock_start);
    g_clock_media = g_position;
    if (app->hwnd) {
        SetTimer(app->hwnd, ZI_VIDEO_TIMER, 33, NULL);
    }
    zi_grade_store(path, &tone);
    return 1;
}

void zi_media_toggle(ZiApp *app) {
    if (!app || !app->video_open || !g_reader) {
        return;
    }
    g_playing = !g_playing;
    if (g_playing) {
        QueryPerformanceCounter(&g_clock_start);
        g_clock_media = g_position;
        if (app->hwnd) {
            SetTimer(app->hwnd, ZI_VIDEO_TIMER, 33, NULL);
        }
    }
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

void zi_media_seek(ZiApp *app, double fraction) {
    PROPVARIANT pos;
    HBITMAP frame = NULL;
    int ended = 0;

    if (!app || !app->video_open || !g_reader || g_duration <= 0) {
        return;
    }
    if (fraction < 0) {
        fraction = 0;
    }
    if (fraction > 1) {
        fraction = 1;
    }
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = (LONGLONG)(fraction * (double)g_duration);
    if (FAILED(g_reader->lpVtbl->SetCurrentPosition(g_reader, &GUID_NULL, &pos))) {
        PropVariantClear(&pos);
        return;
    }
    PropVariantClear(&pos);
    if (zi_pull_frame(&frame, NULL, &ended) && frame) {
        zi_release_bitmap(&g_raw);
        g_raw = frame;
        zi_show_current(&app->edit);
    }
    QueryPerformanceCounter(&g_clock_start);
    g_clock_media = g_position;
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

int zi_media_playing(const ZiApp *app) {
    return app && app->video_open && g_playing;
}

void zi_media_on_grade(ZiApp *app) {
    if (!app || !app->video_open) {
        return;
    }
    zi_grade_store(app->photo_path, &app->edit);
    zi_show_current(&app->edit);
}

void zi_media_on_timer(ZiApp *app) {
    LARGE_INTEGER now;
    LONGLONG elapsed;
    HBITMAP frame = NULL;
    LONGLONG stamp = 0;
    int ended = 0;
    int pulled = 0;

    if (!app || !app->video_open || !g_playing || !g_reader) {
        return;
    }
    if (g_clock_freq.QuadPart < 1) {
        return;
    }
    QueryPerformanceCounter(&now);
    elapsed = (now.QuadPart - g_clock_start.QuadPart) * 10000000LL / g_clock_freq.QuadPart;
    while (g_position < g_clock_media + elapsed) {
        if (!zi_pull_frame(&frame, &stamp, &ended)) {
            g_playing = 0;
            break;
        }
        if (ended) {
            g_playing = 0;
            if (frame) {
                DeleteObject(frame);
            }
            break;
        }
        if (!frame) {
            break;
        }
        zi_release_bitmap(&g_raw);
        g_raw = frame;
        frame = NULL;
        pulled = 1;
        if (g_position >= g_clock_media + elapsed) {
            break;
        }
    }
    if (pulled) {
        zi_show_current(&app->edit);
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

int zi_media_paint(ZiApp *app, HDC hdc) {
    RECT canvas;
    RECT dest;
    HBITMAP bitmap;
    int sw;
    int sh;
    int dw;
    int dh;
    int bw;
    int bh;
    HDC mem;
    HGDIOBJ old;
    double fit;
    double fraction;

    if (!app || !app->video_open) {
        return 0;
    }
    canvas = app->regions.canvas;
    bitmap = g_show ? g_show : g_raw;
    sw = g_frame_w;
    sh = g_frame_h;
    FillRect(hdc, &canvas, app->canvas_brush);
    if (bitmap && sw > 0 && sh > 0 && canvas.right > canvas.left && canvas.bottom > canvas.top + zi_px(app, 36)) {
        bw = canvas.right - canvas.left;
        bh = canvas.bottom - canvas.top - zi_px(app, 36);
        fit = (double)bw / (double)sw;
        if ((double)bh / (double)sh < fit) {
            fit = (double)bh / (double)sh;
        }
        dw = (int)((double)sw * fit);
        dh = (int)((double)sh * fit);
        if (dw < 1) {
            dw = 1;
        }
        if (dh < 1) {
            dh = 1;
        }
        dest.left = canvas.left + (bw - dw) / 2;
        dest.top = canvas.top + (bh - dh) / 2;
        dest.right = dest.left + dw;
        dest.bottom = dest.top + dh;
        mem = CreateCompatibleDC(hdc);
        old = SelectObject(mem, bitmap);
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchBlt(hdc, dest.left, dest.top, dw, dh, mem, 0, 0, sw, sh, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    g_bar = canvas;
    g_bar.top = canvas.bottom - zi_px(app, 28);
    g_bar_ready = 1;
    FillRect(hdc, &g_bar, app->panel);
    {
        RECT play = g_bar;
        RECT track;
        RECT head;
        int span;
        int x;

        play.right = play.left + zi_px(app, 72);
        SetTextColor(hdc, ZI_INK);
        SelectObject(hdc, app->font);
        DrawTextW(hdc, g_playing ? L"Pause" : L"Play", -1, &play, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        track = g_bar;
        track.left = play.right + zi_px(app, 8);
        track.right -= zi_px(app, 12);
        track.top += zi_px(app, 12);
        track.bottom = track.top + zi_px(app, 4);
        FillRect(hdc, &track, app->list_brush);
        fraction = g_duration > 0 ? (double)g_position / (double)g_duration : 0;
        if (fraction < 0) {
            fraction = 0;
        }
        if (fraction > 1) {
            fraction = 1;
        }
        span = track.right - track.left;
        x = track.left + (int)(fraction * (double)span);
        head.left = x - zi_px(app, 4);
        head.right = x + zi_px(app, 4);
        head.top = g_bar.top + zi_px(app, 6);
        head.bottom = g_bar.bottom - zi_px(app, 6);
        FillRect(hdc, &head, app->select_brush);
    }
    return 1;
}

int zi_media_click(ZiApp *app, int x, int y) {
    POINT pt;
    double fraction;
    int span;

    if (!app || !app->video_open || !g_bar_ready) {
        return 0;
    }
    pt.x = x;
    pt.y = y;
    if (!PtInRect(&g_bar, pt)) {
        return 0;
    }
    if (x < g_bar.left + zi_px(app, 72)) {
        zi_media_toggle(app);
        return 1;
    }
    span = (g_bar.right - zi_px(app, 12)) - (g_bar.left + zi_px(app, 80));
    if (span < 1) {
        span = 1;
    }
    fraction = (double)(x - (g_bar.left + zi_px(app, 80))) / (double)span;
    zi_media_seek(app, fraction);
    return 1;
}

static HBITMAP zi_sample_bitmap(IMFSourceReader *reader, IMFSample *sample) {
    IMFMediaBuffer *buffer = NULL;
    IMFMediaType *type = NULL;
    BYTE *data = NULL;
    DWORD max_len = 0;
    DWORD cur_len = 0;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 stride_u = 0;
    int stride;
    HBITMAP bitmap = NULL;

    if (!reader || !sample) {
        return NULL;
    }
    if (FAILED(reader->lpVtbl->GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)) ||
        !type || FAILED(zi_mf_get_pair(type, &MF_MT_FRAME_SIZE, &width, &height)) || width < 1 || height < 1) {
        if (type) {
            type->lpVtbl->Release(type);
        }
        return NULL;
    }
    stride = (int)width * 4;
    if (SUCCEEDED(type->lpVtbl->GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &stride_u))) {
        stride = (int)stride_u;
    }
    type->lpVtbl->Release(type);
    if (FAILED(sample->lpVtbl->ConvertToContiguousBuffer(sample, &buffer)) || !buffer ||
        FAILED(buffer->lpVtbl->Lock(buffer, &data, &max_len, &cur_len)) || !data) {
        if (buffer) {
            buffer->lpVtbl->Release(buffer);
        }
        return NULL;
    }
    bitmap = zi_dib_from_bgra(data, (int)width, (int)height, stride);
    buffer->lpVtbl->Unlock(buffer);
    buffer->lpVtbl->Release(buffer);
    return bitmap;
}

static HBITMAP zi_fit_even(HBITMAP src, int dw, int dh) {
    BITMAP info;
    BITMAPINFO bmi;
    HBITMAP dest;
    void *bits = NULL;
    HDC screen;
    HDC src_dc;
    HDC dst_dc;
    HGDIOBJ old_src;
    HGDIOBJ old_dst;
    int sh;

    if (!src || dw < 2 || dh < 2) {
        return NULL;
    }
    ZeroMemory(&info, sizeof info);
    if (GetObject(src, sizeof info, &info) != sizeof info || info.bmWidth < 1 || info.bmHeight == 0) {
        return NULL;
    }
    sh = info.bmHeight < 0 ? -info.bmHeight : info.bmHeight;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = dw;
    bmi.bmiHeader.biHeight = -dh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    dest = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dest || !bits) {
        if (dest) {
            DeleteObject(dest);
        }
        return NULL;
    }
    screen = GetDC(NULL);
    src_dc = CreateCompatibleDC(screen);
    dst_dc = CreateCompatibleDC(screen);
    old_src = SelectObject(src_dc, src);
    old_dst = SelectObject(dst_dc, dest);
    SetStretchBltMode(dst_dc, HALFTONE);
    StretchBlt(dst_dc, 0, 0, dw, dh, src_dc, 0, 0, info.bmWidth, sh, SRCCOPY);
    SelectObject(src_dc, old_src);
    SelectObject(dst_dc, old_dst);
    DeleteDC(src_dc);
    DeleteDC(dst_dc);
    ReleaseDC(NULL, screen);
    return dest;
}

static int zi_export_video(const wchar_t *source, const wchar_t *dest, const ZiTone *tone) {
    IMFSourceReader *reader = NULL;
    IMFAttributes *attr = NULL;
    IMFMediaType *out_type = NULL;
    IMFMediaType *in_type = NULL;
    IMFMediaType *native = NULL;
    IMFSinkWriter *writer = NULL;
    DWORD stream = 0;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 num = 30;
    UINT32 den = 1;
    LONGLONG stamp = 0;
    int wrote = 0;
    HRESULT hr;

    if (FAILED(MFCreateAttributes(&attr, 2)) || !attr) {
        return 0;
    }
    attr->lpVtbl->SetUINT32(attr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    hr = MFCreateSourceReaderFromURL(source, attr, &reader);
    attr->lpVtbl->Release(attr);
    if (FAILED(hr) || !reader) {
        return 0;
    }
    if (FAILED(MFCreateMediaType(&native)) || !native) {
        reader->lpVtbl->Release(reader);
        return 0;
    }
    native->lpVtbl->SetGUID(native, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    native->lpVtbl->SetGUID(native, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    hr = reader->lpVtbl->SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, native);
    native->lpVtbl->Release(native);
    native = NULL;
    if (FAILED(hr) || FAILED(reader->lpVtbl->GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &native)) ||
        !native || FAILED(zi_mf_get_pair(native, &MF_MT_FRAME_SIZE, &width, &height))) {
        if (native) {
            native->lpVtbl->Release(native);
        }
        reader->lpVtbl->Release(reader);
        return 0;
    }
    zi_mf_get_pair(native, &MF_MT_FRAME_RATE, &num, &den);
    native->lpVtbl->Release(native);
    if (width < 2 || height < 2 || num == 0 || den == 0) {
        reader->lpVtbl->Release(reader);
        return 0;
    }
    width &= ~1u;
    height &= ~1u;
    if (FAILED(MFCreateSinkWriterFromURL(dest, NULL, NULL, &writer)) || !writer) {
        reader->lpVtbl->Release(reader);
        return 0;
    }
    if (FAILED(MFCreateMediaType(&out_type)) || !out_type) {
        writer->lpVtbl->Release(writer);
        reader->lpVtbl->Release(reader);
        DeleteFileW(dest);
        return 0;
    }
    out_type->lpVtbl->SetGUID(out_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    out_type->lpVtbl->SetGUID(out_type, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    out_type->lpVtbl->SetUINT32(out_type, &MF_MT_AVG_BITRATE, 8000000);
    out_type->lpVtbl->SetUINT32(out_type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    zi_mf_set_pair(out_type, &MF_MT_FRAME_SIZE, width, height);
    zi_mf_set_pair(out_type, &MF_MT_FRAME_RATE, num, den);
    zi_mf_set_pair(out_type, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer->lpVtbl->AddStream(writer, out_type, &stream);
    out_type->lpVtbl->Release(out_type);
    if (FAILED(hr) || FAILED(MFCreateMediaType(&in_type)) || !in_type) {
        writer->lpVtbl->Release(writer);
        reader->lpVtbl->Release(reader);
        DeleteFileW(dest);
        return 0;
    }
    in_type->lpVtbl->SetGUID(in_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    in_type->lpVtbl->SetGUID(in_type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    zi_mf_set_pair(in_type, &MF_MT_FRAME_SIZE, width, height);
    zi_mf_set_pair(in_type, &MF_MT_FRAME_RATE, num, den);
    zi_mf_set_pair(in_type, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in_type->lpVtbl->SetUINT32(in_type, &MF_MT_DEFAULT_STRIDE, width * 4);
    in_type->lpVtbl->SetUINT32(in_type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = writer->lpVtbl->SetInputMediaType(writer, stream, in_type, NULL);
    in_type->lpVtbl->Release(in_type);
    if (FAILED(hr) || FAILED(writer->lpVtbl->BeginWriting(writer))) {
        writer->lpVtbl->Release(writer);
        reader->lpVtbl->Release(reader);
        DeleteFileW(dest);
        return 0;
    }
    for (;;) {
        IMFSample *sample = NULL;
        DWORD flags = 0;
        HBITMAP raw = NULL;
        HBITMAP graded = NULL;
        HBITMAP use;
        BITMAP info;
        IMFSample *out_sample = NULL;
        IMFMediaBuffer *out_buffer = NULL;
        BYTE *out_data = NULL;
        DWORD max_len = 0;
        DWORD cur_len = 0;
        int gw = 0;
        int gh = 0;

        hr = reader->lpVtbl->ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, &stamp, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) || !sample) {
            if (sample) {
                sample->lpVtbl->Release(sample);
            }
            break;
        }
        raw = zi_sample_bitmap(reader, sample);
        sample->lpVtbl->Release(sample);
        if (!raw) {
            break;
        }
        graded = zi_grade_bitmap(raw, tone, &gw, &gh);
        use = graded ? graded : raw;
        {
            HBITMAP even = zi_fit_even(use, (int)width, (int)height);

            zi_release_bitmap(&raw);
            zi_release_bitmap(&graded);
            if (!even) {
                break;
            }
            use = even;
        }
        ZeroMemory(&info, sizeof info);
        if (GetObject(use, sizeof info, &info) != sizeof info || info.bmBits == NULL) {
            DeleteObject(use);
            break;
        }
        if (FAILED(MFCreateSample(&out_sample)) || !out_sample ||
            FAILED(MFCreateMemoryBuffer((DWORD)width * height * 4, &out_buffer)) || !out_buffer) {
            if (out_sample) {
                out_sample->lpVtbl->Release(out_sample);
            }
            if (out_buffer) {
                out_buffer->lpVtbl->Release(out_buffer);
            }
            DeleteObject(use);
            break;
        }
        out_buffer->lpVtbl->Lock(out_buffer, &out_data, &max_len, &cur_len);
        if (out_data && info.bmBits) {
            memcpy(out_data, info.bmBits, (size_t)width * (size_t)height * 4u);
        }
        out_buffer->lpVtbl->Unlock(out_buffer);
        DeleteObject(use);
        out_buffer->lpVtbl->SetCurrentLength(out_buffer, width * height * 4);
        out_sample->lpVtbl->AddBuffer(out_sample, out_buffer);
        out_sample->lpVtbl->SetSampleTime(out_sample, stamp);
        out_sample->lpVtbl->SetSampleDuration(out_sample, 10000000LL * (LONGLONG)den / (LONGLONG)num);
        hr = writer->lpVtbl->WriteSample(writer, stream, out_sample);
        out_buffer->lpVtbl->Release(out_buffer);
        out_sample->lpVtbl->Release(out_sample);
        if (FAILED(hr)) {
            break;
        }
        wrote++;
    }
    if (wrote > 0 && SUCCEEDED(writer->lpVtbl->Finalize(writer))) {
        writer->lpVtbl->Release(writer);
        reader->lpVtbl->Release(reader);
        return 1;
    }
    writer->lpVtbl->Release(writer);
    reader->lpVtbl->Release(reader);
    DeleteFileW(dest);
    return 0;
}

int zi_media_export(ZiApp *app, const wchar_t *dest) {
    int was;
    int ok;
    ZiTone tone;

    if (!app || !app->video_open || !dest || !dest[0] || !g_open_path[0]) {
        return 0;
    }
    if (_wcsicmp(dest, g_open_path) == 0) {
        zi_notice(app, L"Export writes a new file. Choose a different name.");
        return 0;
    }
    tone = app->edit;
    was = g_playing;
    g_playing = 0;
    ok = zi_export_video(g_open_path, dest, &tone);
    g_playing = was;
    if (!ok) {
        zi_notice(app, L"That video could not be exported.");
    }
    return ok;
}

void zi_media_shutdown(void) {
    zi_media_close(NULL);
    zi_pdf_lock();
    free(g_pdf_pages);
    g_pdf_pages = NULL;
    if (g_pdf_jpegs) {
        int i;

        for (i = 0; i < g_pdf_count; i++) {
            free(g_pdf_jpegs[i]);
        }
    }
    free(g_pdf_jpegs);
    free(g_pdf_lengths);
    g_pdf_jpegs = NULL;
    g_pdf_lengths = NULL;
    g_pdf_count = 0;
    g_pdf_path[0] = L'\0';
    zi_pdf_unlock();
    if (g_mf_started) {
        MFShutdown();
        g_mf_started = 0;
    }
}

static unsigned char *zi_file_span(HANDLE file, unsigned long long offset, size_t count, unsigned long long file_size) {
    unsigned char *buf;
    LARGE_INTEGER pos;
    DWORD got = 0;

    if (count == 0 || count > ZI_X3F_BUDGET || offset > file_size || (unsigned long long)count > file_size - offset) {
        return NULL;
    }
    buf = (unsigned char *)malloc(count);
    if (!buf) {
        return NULL;
    }
    pos.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) || !ReadFile(file, buf, (DWORD)count, &got, NULL) || got != count) {
        free(buf);
        return NULL;
    }
    return buf;
}

static unsigned int zi_u32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int zi_jpeg_pixels(const unsigned char *data, size_t count) {
    size_t index = 2;

    if (!data || count < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return 0;
    }
    if (count > 256 * 1024) {
        count = 256 * 1024;
    }
    while (index + 3 < count) {
        unsigned int marker;
        unsigned int length;
        int width;
        int height;

        if (data[index] != 0xFF) {
            return 0;
        }
        index++;
        while (index < count && data[index] == 0xFF) {
            index++;
        }
        if (index >= count) {
            return 0;
        }
        marker = data[index++];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) {
            return 0;
        }
        if (index + 1 >= count) {
            return 0;
        }
        length = ((unsigned int)data[index] << 8) | data[index + 1];
        if (length < 2) {
            return 0;
        }
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (index + 6 >= count) {
                return 0;
            }
            height = ((int)data[index + 3] << 8) | data[index + 4];
            width = ((int)data[index + 5] << 8) | data[index + 6];
            if (width < 1 || height < 1 || width > ZI_X3F_EDGE || height > ZI_X3F_EDGE) {
                return 0;
            }
            if (height > ZI_X3F_PIXELS / width) {
                return 0;
            }
            return width * height;
        }
        if (index + length > count) {
            return 0;
        }
        index += length;
    }
    return 0;
}

static void zi_x3f_notice(wchar_t *notice, size_t cap, const wchar_t *text) {
    if (notice && cap > 0 && text) {
        wcscpy_s(notice, cap, text);
    }
}

int zi_x3f_embedded_jpeg(const wchar_t *path, unsigned char **jpeg, size_t *len, wchar_t *notice, size_t notice_cap) {
    HANDLE file;
    LARGE_INTEGER pos;
    unsigned long long file_size = 0;
    unsigned char tail[4];
    unsigned char header[12];
    unsigned char *table = NULL;
    unsigned int directory;
    unsigned int count;
    unsigned int i;
    DWORD got = 0;
    int best_pixels = -1;
    size_t best_len = 0;
    unsigned long long best_offset = 0;
    const wchar_t *missing_note = L"Opened the embedded preview. The Sigma converter is not on this PC.";
    const wchar_t *preview_note = L"Opened the embedded preview. Sigma Photo Pro color is not applied.";

    if (jpeg) {
        *jpeg = NULL;
    }
    if (len) {
        *len = 0;
    }
    if (!path || !jpeg || !len) {
        return 0;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    pos.QuadPart = 0;
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &pos) || pos.QuadPart < 4) {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        zi_x3f_notice(notice, notice_cap, missing_note);
        return 0;
    }
    file_size = (unsigned long long)pos.QuadPart;
    if (file_size < 65536ull) {
        CloseHandle(file);
        zi_x3f_notice(notice, notice_cap, missing_note);
        return 0;
    }
    pos.QuadPart = 0;
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) || !ReadFile(file, header, 4, &got, NULL) || got != 4 ||
        memcmp(header, "FOVb", 4) != 0) {
        CloseHandle(file);
        zi_x3f_notice(notice, notice_cap, missing_note);
        return 0;
    }
    pos.QuadPart = (LONGLONG)file_size - 4;
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) || !ReadFile(file, tail, 4, &got, NULL) || got != 4) {
        CloseHandle(file);
        zi_x3f_notice(notice, notice_cap, missing_note);
        return 0;
    }
    directory = zi_u32(tail);
    if ((unsigned long long)directory + 12ull > file_size) {
        CloseHandle(file);
        zi_x3f_notice(notice, notice_cap, missing_note);
        return 0;
    }
    pos.QuadPart = directory;
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) || !ReadFile(file, header, 12, &got, NULL) || got != 12 ||
        memcmp(header, "SECd", 4) != 0) {
        CloseHandle(file);
        zi_x3f_notice(notice, notice_cap, missing_note);
        return 0;
    }
    count = zi_u32(header + 8);
    if (count > 4096 || (unsigned long long)directory + 12ull + (unsigned long long)count * 12ull > file_size) {
        CloseHandle(file);
        zi_x3f_notice(notice, notice_cap, missing_note);
        return 0;
    }
    if (count > 0) {
        table = zi_file_span(file, (unsigned long long)directory + 12ull, (size_t)count * 12u, file_size);
        if (!table) {
            CloseHandle(file);
            zi_x3f_notice(notice, notice_cap, missing_note);
            return 0;
        }
    }
    for (i = 0; i < count; i++) {
        unsigned char *entry = table + (size_t)i * 12u;
        unsigned int offset = zi_u32(entry);
        unsigned int length = zi_u32(entry + 4);
        unsigned char peek[28];
        unsigned int image_type;
        unsigned int format;
        int pixels = 0;
        int seci = 0;
        unsigned long long payload;

        if (length < 4 || length > ZI_X3F_BUDGET || (unsigned long long)offset + length > file_size) {
            continue;
        }
        pos.QuadPart = offset;
        if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) || !ReadFile(file, peek, length < 28 ? length : 28, &got, NULL)) {
            continue;
        }
        if (got >= 2 && peek[0] == 0xFF && peek[1] == 0xD8) {
            unsigned char *chunk = zi_file_span(file, offset, length > 256 * 1024 ? 256 * 1024 : length, file_size);

            if (chunk) {
                pixels = zi_jpeg_pixels(chunk, length > 256 * 1024 ? 256 * 1024 : length);
                free(chunk);
            }
            payload = offset;
            seci = 0;
        } else if (got >= 28 && memcmp(peek, "SECi", 4) == 0) {
            unsigned int columns;
            unsigned int rows;

            image_type = zi_u32(peek + 8);
            format = zi_u32(peek + 12);
            if (image_type != 2 || format != 18) {
                continue;
            }
            columns = zi_u32(peek + 16);
            rows = zi_u32(peek + 20);
            if (columns > 0 && rows > 0 && columns <= ZI_X3F_EDGE && rows <= ZI_X3F_EDGE && rows <= ZI_X3F_PIXELS / columns) {
                pixels = (int)(columns * rows);
            }
            if (length <= 28) {
                continue;
            }
            payload = (unsigned long long)offset + 28ull;
            seci = 1;
            if (pixels == 0) {
                unsigned char *chunk = zi_file_span(file, payload, (size_t)(length - 28) > 256 * 1024 ? 256 * 1024 : (size_t)(length - 28),
                                                     file_size);
                if (chunk) {
                    pixels = zi_jpeg_pixels(chunk, (size_t)(length - 28) > 256 * 1024 ? 256 * 1024 : (size_t)(length - 28));
                    free(chunk);
                }
            }
        } else {
            continue;
        }
        if (pixels > ZI_X3F_PIXELS) {
            continue;
        }
        if (pixels > best_pixels || (pixels == best_pixels && (size_t)(seci ? length - 28 : length) > best_len)) {
            best_pixels = pixels;
            best_len = seci ? (size_t)length - 28u : (size_t)length;
            best_offset = seci ? (unsigned long long)offset + 28ull : offset;
            (void)payload;
        }
    }
    free(table);
    if (best_len < 2) {
        CloseHandle(file);
        zi_x3f_notice(notice, notice_cap, missing_note);
        return 0;
    }
    *jpeg = zi_file_span(file, best_offset, best_len, file_size);
    CloseHandle(file);
    if (!*jpeg || (*jpeg)[0] != 0xFF || (*jpeg)[1] != 0xD8) {
        free(*jpeg);
        *jpeg = NULL;
        zi_x3f_notice(notice, notice_cap, missing_note);
        return 0;
    }
    *len = best_len;
    zi_x3f_notice(notice, notice_cap, preview_note);
    return 1;
}

static int zi_token_at(const unsigned char *data, size_t n, size_t i, const char *token) {
    size_t len = strlen(token);

    if (i + len > n || memcmp(data + i, token, len) != 0) {
        return 0;
    }
    if (i > 0 && (data[i - 1] == (unsigned char)token[0] || (data[i - 1] >= 'a' && data[i - 1] <= 'z'))) {
        return 0;
    }
    return 1;
}

static void zi_pdf_free_cache(void) {
    int i;

    for (i = 0; i < g_pdf_count; i++) {
        free(g_pdf_jpegs[i]);
    }
    free(g_pdf_jpegs);
    free(g_pdf_lengths);
    free(g_pdf_pages);
    g_pdf_jpegs = NULL;
    g_pdf_lengths = NULL;
    g_pdf_pages = NULL;
    g_pdf_count = 0;
}

static int zi_pdf_load(const wchar_t *path) {
    HANDLE file;
    LARGE_INTEGER size;
    unsigned char *data = NULL;
    DWORD got = 0;
    size_t n;
    size_t i;
    typedef struct ZiPdfObj {
        int id;
        size_t body;
        size_t end;
        unsigned char *jpeg;
        size_t jpeg_len;
    } ZiPdfObj;
    ZiPdfObj *objs = NULL;
    int obj_count = 0;
    int obj_cap = 0;
    int page_ids[512];
    int page_count = 0;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart < 16 || (unsigned long long)size.QuadPart > ZI_PDF_CAP) {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        return 0;
    }
    n = (size_t)size.QuadPart;
    data = (unsigned char *)malloc(n);
    if (!data || !ReadFile(file, data, (DWORD)n, &got, NULL) || got != n) {
        free(data);
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    i = 0;
    while (i + 6 < n && obj_count < 8000) {
        size_t mark;
        int id = 0;
        size_t k;
        size_t body;
        size_t end;
        int stream_at = -1;
        size_t length = 0;
        int has_length = 0;

        if (data[i] < '0' || data[i] > '9') {
            i++;
            continue;
        }
        mark = i;
        while (i < n && data[i] >= '0' && data[i] <= '9') {
            id = id * 10 + (data[i] - '0');
            i++;
            if (i - mark > 9) {
                id = -1;
                break;
            }
        }
        if (id <= 0 || i + 6 >= n || data[i] != ' ' || data[i + 1] != '0' || data[i + 2] != ' ' ||
            memcmp(data + i + 3, "obj", 3) != 0) {
            i = mark + 1;
            continue;
        }
        body = i + 6;
        k = body;
        end = n;
        while (k + 6 < n) {
            if (stream_at < 0 && zi_token_at(data, n, k, "stream")) {
                size_t s = k + 6;
                size_t back;
                int dct = 0;

                stream_at = (int)k;
                if (s < n && data[s] == '\r') {
                    s++;
                }
                if (s < n && data[s] == '\n') {
                    s++;
                }
                back = body;
                while (back + 10 < (size_t)stream_at) {
                    if (memcmp(data + back, "/DCTDecode", 10) == 0) {
                        dct = 1;
                        break;
                    }
                    back++;
                }
                back = body;
                while (back + 8 < (size_t)stream_at) {
                    if (memcmp(data + back, "/Length", 7) == 0 && (data[back + 7] == ' ' || data[back + 7] == '\n' || data[back + 7] == '\r')) {
                        size_t p = back + 7;
                        size_t value = 0;
                        int digits = 0;

                        while (p < (size_t)stream_at && (data[p] == ' ' || data[p] == '\r' || data[p] == '\n')) {
                            p++;
                        }
                        while (p < (size_t)stream_at && data[p] >= '0' && data[p] <= '9') {
                            value = value * 10 + (size_t)(data[p] - '0');
                            p++;
                            digits++;
                        }
                        if (digits > 0) {
                            size_t q = p;

                            while (q < (size_t)stream_at && (data[q] == ' ' || data[q] == '\r' || data[q] == '\n')) {
                                q++;
                            }
                            /* "12 0 R" is an indirect length. A direct integer is the byte count. */
                            if (!(q < (size_t)stream_at && (data[q] == 'R' || (data[q] >= '0' && data[q] <= '9')))) {
                                length = value;
                                has_length = 1;
                            }
                        }
                        break;
                    }
                    back++;
                }
                if (dct) {
                    size_t jpeg_end = s;
                    size_t limit = has_length && s + length <= n ? s + length : n;

                    if (s + 1 < limit && data[s] == 0xFF && data[s + 1] == 0xD8) {
                        jpeg_end = s + 2;
                        while (jpeg_end + 1 < limit) {
                            if (data[jpeg_end] == 0xFF && data[jpeg_end + 1] == 0xD9) {
                                jpeg_end += 2;
                                break;
                            }
                            jpeg_end++;
                        }
                        if (obj_count >= obj_cap) {
                            int next_cap = obj_cap > 0 ? obj_cap * 2 : 64;
                            ZiPdfObj *grown = (ZiPdfObj *)realloc(objs, (size_t)next_cap * sizeof *grown);

                            if (!grown) {
                                break;
                            }
                            objs = grown;
                            obj_cap = next_cap;
                        }
                        ZeroMemory(&objs[obj_count], sizeof objs[0]);
                        objs[obj_count].id = id;
                        objs[obj_count].body = body;
                        objs[obj_count].jpeg_len = jpeg_end - s;
                        objs[obj_count].jpeg = (unsigned char *)malloc(objs[obj_count].jpeg_len);
                        if (objs[obj_count].jpeg) {
                            memcpy(objs[obj_count].jpeg, data + s, objs[obj_count].jpeg_len);
                            obj_count++;
                        }
                    }
                }
                if (has_length && s + length < n) {
                    k = s + length;
                    continue;
                }
            }
            if (zi_token_at(data, n, k, "endobj")) {
                end = k;
                break;
            }
            k++;
        }
        {
            size_t dict_end = stream_at >= 0 && (size_t)stream_at < end ? (size_t)stream_at : end;
            int is_page = 0;
            size_t p;
            int have = 0;
            int t;

            for (p = body; p + 10 < dict_end; p++) {
                if (memcmp(data + p, "/Type", 5) == 0) {
                    size_t q = p + 5;

                    while (q < dict_end && (data[q] == ' ' || data[q] == '\r' || data[q] == '\n' || data[q] == '/')) {
                        if (data[q] == '/' && q + 4 < dict_end && memcmp(data + q, "/Page", 5) == 0 &&
                            (q + 5 >= dict_end || data[q + 5] != 's')) {
                            is_page = 1;
                            break;
                        }
                        q++;
                    }
                }
                if (is_page) {
                    break;
                }
            }
            if (is_page && page_count < 512) {
                page_ids[page_count++] = id;
            }
            for (t = 0; t < obj_count; t++) {
                if (objs[t].id == id) {
                    objs[t].end = end;
                    if (objs[t].body == 0) {
                        objs[t].body = body;
                    }
                    have = 1;
                    break;
                }
            }
            if (!have) {
                if (obj_count >= obj_cap) {
                    int next_cap = obj_cap > 0 ? obj_cap * 2 : 64;
                    ZiPdfObj *grown = (ZiPdfObj *)realloc(objs, (size_t)next_cap * sizeof *grown);

                    if (!grown) {
                        break;
                    }
                    objs = grown;
                    obj_cap = next_cap;
                }
                ZeroMemory(&objs[obj_count], sizeof objs[0]);
                objs[obj_count].id = id;
                objs[obj_count].body = body;
                objs[obj_count].end = end;
                obj_count++;
            }
        }
        i = end > mark ? end : mark + 1;
    }
    zi_pdf_free_cache();
    if (page_count > 0) {
        g_pdf_pages = (int *)calloc((size_t)page_count, sizeof(int));
        g_pdf_jpegs = (unsigned char **)calloc((size_t)page_count, sizeof(unsigned char *));
        g_pdf_lengths = (size_t *)calloc((size_t)page_count, sizeof(size_t));
    }
    if (g_pdf_pages && g_pdf_jpegs && g_pdf_lengths) {
        int p;

        for (p = 0; p < page_count; p++) {
            int page_obj = -1;
            int r;
            size_t best = 0;
            unsigned char *best_jpeg = NULL;
            int refs[64];
            int ref_count = 0;
            size_t cursor;
            size_t end;

            for (r = 0; r < obj_count; r++) {
                if (objs[r].id == page_ids[p]) {
                    if (page_obj < 0 || !objs[r].jpeg) {
                        page_obj = r;
                    }
                    if (!objs[r].jpeg) {
                        break;
                    }
                }
            }
            if (page_obj < 0) {
                continue;
            }
            cursor = objs[page_obj].body;
            end = objs[page_obj].end;
            while (cursor + 4 < end && ref_count < 64) {
                if (data[cursor] >= '0' && data[cursor] <= '9') {
                    int ref = 0;
                    size_t q = cursor;

                    while (q < end && data[q] >= '0' && data[q] <= '9') {
                        ref = ref * 10 + (data[q] - '0');
                        q++;
                    }
                    if (ref > 0 && q + 4 < end && data[q] == ' ' && data[q + 1] == '0' && data[q + 2] == ' ' && data[q + 3] == 'R') {
                        refs[ref_count++] = ref;
                        cursor = q + 4;
                        continue;
                    }
                }
                cursor++;
            }
            {
                int base_count = ref_count;
                int r2;

                for (r2 = 0; r2 < base_count && ref_count < 64; r2++) {
                    int t;

                    for (t = 0; t < obj_count && ref_count < 64; t++) {
                        size_t c2;
                        size_t e2;

                        if (objs[t].id != refs[r2] || objs[t].jpeg) {
                            continue;
                        }
                        c2 = objs[t].body;
                        e2 = objs[t].end;
                        while (c2 + 4 < e2 && ref_count < 64) {
                            if (data[c2] >= '0' && data[c2] <= '9') {
                                int ref = 0;
                                size_t q = c2;

                                while (q < e2 && data[q] >= '0' && data[q] <= '9') {
                                    ref = ref * 10 + (data[q] - '0');
                                    q++;
                                }
                                if (ref > 0 && q + 4 < e2 && data[q] == ' ' && data[q + 1] == '0' &&
                                    data[q + 2] == ' ' && data[q + 3] == 'R') {
                                    int already = 0;
                                    int u;

                                    for (u = 0; u < ref_count; u++) {
                                        if (refs[u] == ref) {
                                            already = 1;
                                            break;
                                        }
                                    }
                                    if (!already) {
                                        refs[ref_count++] = ref;
                                    }
                                    c2 = q + 4;
                                    continue;
                                }
                            }
                            c2++;
                        }
                        break;
                    }
                }
            }
            for (r = 0; r < ref_count; r++) {
                int t;

                for (t = 0; t < obj_count; t++) {
                    if (objs[t].id == refs[r] && objs[t].jpeg && objs[t].jpeg_len > best) {
                        best = objs[t].jpeg_len;
                        best_jpeg = objs[t].jpeg;
                    }
                }
            }
            if (best_jpeg) {
                g_pdf_pages[g_pdf_count] = p + 1;
                g_pdf_lengths[g_pdf_count] = best;
                g_pdf_jpegs[g_pdf_count] = (unsigned char *)malloc(best);
                if (g_pdf_jpegs[g_pdf_count]) {
                    memcpy(g_pdf_jpegs[g_pdf_count], best_jpeg, best);
                    g_pdf_count++;
                }
            }
        }
    }
    for (i = 0; i < (size_t)obj_count; i++) {
        free(objs[i].jpeg);
    }
    free(objs);
    free(data);
    wcscpy_s(g_pdf_path, MAX_PATH, path);
    return g_pdf_count > 0;
}

int zi_pdf_list_pages(const wchar_t *path, int **pages, int *count) {
    int ok;

    if (pages) {
        *pages = NULL;
    }
    if (count) {
        *count = 0;
    }
    if (!path || !pages || !count) {
        return 0;
    }
    zi_pdf_lock();
    if (g_pdf_path[0] == 0 || _wcsicmp(g_pdf_path, path) != 0 || g_pdf_count < 1) {
        zi_pdf_free_cache();
        g_pdf_path[0] = L'\0';
        ok = zi_pdf_load(path);
    } else {
        ok = 1;
    }
    if (ok && g_pdf_count > 0) {
        *pages = (int *)malloc((size_t)g_pdf_count * sizeof(int));
        if (*pages) {
            memcpy(*pages, g_pdf_pages, (size_t)g_pdf_count * sizeof(int));
            *count = g_pdf_count;
        } else {
            ok = 0;
        }
    } else {
        ok = 0;
    }
    zi_pdf_unlock();
    return ok;
}

int zi_pdf_page_jpeg(const wchar_t *path, int page, unsigned char **jpeg, size_t *len) {
    int i;
    int ok = 0;

    if (jpeg) {
        *jpeg = NULL;
    }
    if (len) {
        *len = 0;
    }
    if (!path || page < 1 || !jpeg || !len) {
        return 0;
    }
    zi_pdf_lock();
    if (g_pdf_path[0] == 0 || _wcsicmp(g_pdf_path, path) != 0) {
        zi_pdf_free_cache();
        g_pdf_path[0] = L'\0';
        zi_pdf_load(path);
    }
    for (i = 0; i < g_pdf_count; i++) {
        if (g_pdf_pages[i] == page && g_pdf_jpegs[i] && g_pdf_lengths[i] > 1) {
            *jpeg = (unsigned char *)malloc(g_pdf_lengths[i]);
            if (*jpeg) {
                memcpy(*jpeg, g_pdf_jpegs[i], g_pdf_lengths[i]);
                *len = g_pdf_lengths[i];
                ok = 1;
            }
            break;
        }
    }
    zi_pdf_unlock();
    return ok;
}
