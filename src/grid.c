/* Folder grid. Names come from the selected directory. Thumbnails are decoded off the UI thread. */
#include "grid.h"
#include "editor.h"

#include <wincodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG g_grid_generation;
static volatile LONG g_grid_alive = 1;
static HWND g_vscroll;
static WNDPROC g_vscroll_proc;

static const wchar_t *const k_stills[] = {
    L"jpg", L"jpeg", L"png", L"tif", L"tiff", L"heic", L"heif", L"webp", L"gif", L"bmp",
    L"dng", L"raf", L"arw", L"cr2", L"nef", L"orf", L"rw2", L"raw"
};

static const wchar_t *const k_videos[] = {
    L"mov", L"mp4", L"m4v", L"m4p", L"avi", L"mkv", L"mpg", L"mpeg", L"mpe", L"m2v",
    L"wmv", L"flv", L"webm", L"3gp", L"3g2", L"mts", L"m2ts", L"ts", L"vob", L"ogv", L"asf", L"divx"
};

static int zi_has_ext(const wchar_t *name, const wchar_t *const *exts, int count) {
    const wchar_t *dot = wcsrchr(name, L'.');
    int i;

    if (!dot || dot[1] == L'\0') {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (_wcsicmp(dot + 1, exts[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int zi_file_less(const void *a, const void *b) {
    const ZiFile *fa = (const ZiFile *)a;
    const ZiFile *fb = (const ZiFile *)b;

    if (fa->video != fb->video) {
        return fa->video - fb->video;
    }
    return _wcsicmp(fa->name, fb->name);
}

static void zi_grid_free_files(ZiApp *app) {
    int i;

    for (i = 0; i < app->file_count; i++) {
        if (app->files[i].thumb) {
            DeleteObject(app->files[i].thumb);
            app->files[i].thumb = NULL;
        }
    }
    app->file_count = 0;
    app->file_selected = -1;
    app->file_anchor = -1;
    app->file_scroll = 0;
    memset(app->file_mark, 0, sizeof app->file_mark);
}

static int zi_grid_orientation(IWICBitmapFrameDecode *frame) {
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

static HBITMAP zi_grid_dib(IWICImagingFactory *factory, IWICBitmapSource *source, UINT w, UINT h) {
    IWICFormatConverter *conv = NULL;
    BITMAPINFO info;
    void *bits = NULL;
    HBITMAP dib = NULL;
    UINT stride;
    HRESULT hr;

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

static HBITMAP zi_decode_thumb(IWICImagingFactory *factory, const wchar_t *path, int *out_w, int *out_h) {
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *thumb = NULL;
    IWICBitmapSource *current = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapFlipRotator *rotator = NULL;
    HBITMAP dib = NULL;
    UINT w = 0;
    UINT h = 0;
    UINT tw;
    UINT th;
    int orientation;
    WICBitmapTransformOptions turn = WICBitmapTransformRotate0;

    if (FAILED(factory->lpVtbl->CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame))) {
        goto done;
    }
    orientation = zi_grid_orientation(frame);
    if (FAILED(frame->lpVtbl->GetThumbnail(frame, &thumb)) || !thumb) {
        current = (IWICBitmapSource *)frame;
        frame->lpVtbl->AddRef(frame);
    } else {
        current = thumb;
        thumb = NULL;
    }
    if (FAILED(current->lpVtbl->GetSize(current, &w, &h)) || w == 0 || h == 0) {
        goto done;
    }
    tw = w;
    th = h;
    if (w > 250 || h > 250) {
        if (w >= h) {
            tw = 250;
            th = (UINT)((250ull * h) / w);
        } else {
            th = 250;
            tw = (UINT)((250ull * w) / h);
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
    switch (orientation) {
    case 2: turn = WICBitmapTransformFlipHorizontal; break;
    case 3: turn = WICBitmapTransformRotate180; break;
    case 4: turn = WICBitmapTransformFlipVertical; break;
    case 5: turn = WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal; break;
    case 6: turn = WICBitmapTransformRotate90; break;
    case 7: turn = WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal; break;
    case 8: turn = WICBitmapTransformRotate270; break;
    default: turn = WICBitmapTransformRotate0; break;
    }
    if (turn != WICBitmapTransformRotate0) {
        if (FAILED(factory->lpVtbl->CreateBitmapFlipRotator(factory, &rotator)) ||
            FAILED(rotator->lpVtbl->Initialize(rotator, current, turn))) {
            goto done;
        }
        current->lpVtbl->Release(current);
        current = (IWICBitmapSource *)rotator;
        rotator = NULL;
        if (FAILED(current->lpVtbl->GetSize(current, &tw, &th)) || tw == 0 || th == 0) {
            goto done;
        }
    }
    dib = zi_grid_dib(factory, current, tw, th);
    if (dib) {
        *out_w = (int)tw;
        *out_h = (int)th;
    }
done:
    if (current) current->lpVtbl->Release(current);
    if (rotator) rotator->lpVtbl->Release(rotator);
    if (scaler) scaler->lpVtbl->Release(scaler);
    if (thumb) thumb->lpVtbl->Release(thumb);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    return dib;
}

typedef struct ZiGridJob {
    HWND hwnd;
    LONG generation;
    int count;
    wchar_t path[ZI_MAX_FILES][MAX_PATH];
} ZiGridJob;

static DWORD WINAPI zi_grid_thread(LPVOID param) {
    ZiGridJob *job = (ZiGridJob *)param;
    IWICImagingFactory *factory = NULL;
    int i;

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        free(job);
        return 0;
    }
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
    for (i = 0; factory && i < job->count; i++) {
        ZiThumbNote *note;
        HBITMAP bitmap = NULL;
        int w = 0;
        int h = 0;

        if (!g_grid_alive || job->generation != g_grid_generation) {
            break;
        }
        if (!job->path[i][0]) {
            continue;
        }
        bitmap = zi_decode_thumb(factory, job->path[i], &w, &h);
        if (!g_grid_alive || job->generation != g_grid_generation) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            break;
        }
        note = (ZiThumbNote *)calloc(1, sizeof *note);
        if (!note) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            break;
        }
        note->generation = job->generation;
        note->index = i;
        note->failed = bitmap ? 0 : 1;
        note->w = w;
        note->h = h;
        note->bitmap = bitmap;
        if (!PostMessageW(job->hwnd, ZI_WM_THUMB, 0, (LPARAM)note)) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            free(note);
        }
    }
    if (factory) {
        factory->lpVtbl->Release(factory);
    }
    CoUninitialize();
    free(job);
    return 0;
}

typedef struct ZiListBuild {
    LONG generation;
    int nested;
    int capped;
    int count;
    wchar_t root[MAX_PATH];
    ZiFile *files;
} ZiListBuild;

static int zi_grid_add(ZiListBuild *build, const wchar_t *dir, const wchar_t *name, int video) {
    ZiFile *file;
    wchar_t relative[ZI_NAME];
    size_t root_len;

    if (build->count >= ZI_MAX_FILES) {
        build->capped = 1;
        return 1;
    }
    root_len = wcslen(build->root);
    if (_wcsnicmp(dir, build->root, root_len) == 0 && (dir[root_len] == L'\\' || dir[root_len] == L'\0')) {
        if (dir[root_len] == L'\0') {
            wcscpy_s(relative, ZI_NAME, name);
        } else if (swprintf_s(relative, ZI_NAME, L"%s\\%s", dir + root_len + 1, name) < 0) {
            return 0;
        }
    } else {
        wcscpy_s(relative, ZI_NAME, name);
    }
    if (wcslen(dir) + wcslen(name) + 2 >= MAX_PATH) {
        return 0;
    }
    file = &build->files[build->count];
    ZeroMemory(file, sizeof *file);
    file->video = video;
    wcscpy_s(file->name, ZI_NAME, relative);
    wcscpy_s(file->path, MAX_PATH, dir);
    wcscat_s(file->path, MAX_PATH, L"\\");
    wcscat_s(file->path, MAX_PATH, name);
    build->count++;
    if (build->count >= ZI_MAX_FILES) {
        build->capped = 1;
        return 1;
    }
    return 0;
}

/* Returns 1 when the walk should stop, 0 when it finished, and -1 when the top folder cannot be read. */
static int zi_grid_collect(ZiListBuild *build, const wchar_t *directory, int depth) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    int stop = 0;

    if (!g_grid_alive || build->generation != g_grid_generation) {
        return 1;
    }
    if (depth > 40 || wcslen(directory) + 3 >= MAX_PATH) {
        return depth == 0 ? -1 : 0;
    }
    swprintf_s(pattern, MAX_PATH, L"%s\\*", directory);
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return depth == 0 ? -1 : 0;
    }
    do {
        wchar_t child[MAX_PATH];

        if (!g_grid_alive || build->generation != g_grid_generation) {
            stop = 1;
            break;
        }
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) {
            continue;
        }
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 || found.cFileName[0] == L'.') {
            continue;
        }
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!build->nested) {
                continue;
            }
            if (wcslen(directory) + wcslen(found.cFileName) + 2 >= MAX_PATH) {
                continue;
            }
            wcscpy_s(child, MAX_PATH, directory);
            wcscat_s(child, MAX_PATH, L"\\");
            wcscat_s(child, MAX_PATH, found.cFileName);
            if (zi_grid_collect(build, child, depth + 1) != 0) {
                stop = 1;
                break;
            }
            continue;
        }
        if (zi_has_ext(found.cFileName, k_videos, (int)(sizeof k_videos / sizeof k_videos[0]))) {
            stop = zi_grid_add(build, directory, found.cFileName, 1);
        } else if (zi_has_ext(found.cFileName, k_stills, (int)(sizeof k_stills / sizeof k_stills[0]))) {
            stop = zi_grid_add(build, directory, found.cFileName, 0);
        }
    } while (!stop && FindNextFileW(search, &found));
    FindClose(search);
    return stop ? 1 : 0;
}

static void zi_grid_start_thumbs(ZiApp *app) {
    ZiGridJob *job;
    HANDLE thread;
    int i;
    int stills = 0;

    job = (ZiGridJob *)calloc(1, sizeof *job);
    if (!job) {
        return;
    }
    job->hwnd = app->hwnd;
    job->generation = app->file_generation;
    for (i = 0; i < app->file_count; i++) {
        if (app->files[i].video) {
            job->path[i][0] = L'\0';
            continue;
        }
        wcscpy_s(job->path[i], MAX_PATH, app->files[i].path);
        stills++;
    }
    job->count = app->file_count;
    if (!stills) {
        free(job);
        return;
    }
    thread = CreateThread(NULL, 0, zi_grid_thread, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        free(job);
    }
}

static void zi_grid_take(ZiApp *app, ZiFile *files, int count, int capped, int failed) {
    int i;

    zi_grid_free_files(app);
    app->file_reading = 0;
    if (failed) {
        wcscpy_s(app->notice, 192, L"That folder could not be read.");
        free(files);
        return;
    }
    if (count > ZI_MAX_FILES) {
        count = ZI_MAX_FILES;
    }
    for (i = 0; i < count; i++) {
        app->files[i] = files[i];
    }
    app->file_count = count;
    free(files);
    if (capped) {
        wcscpy_s(app->notice, 192, L"The folder shows the first 200 photographs.");
    } else if (wcscmp(app->notice, L"Reading subfolders.") == 0) {
        app->notice[0] = L'\0';
    }
    if (app->file_count > 1) {
        qsort(app->files, (size_t)app->file_count, sizeof app->files[0], zi_file_less);
    }
    zi_grid_start_thumbs(app);
}

typedef struct ZiListJob {
    HWND hwnd;
    LONG generation;
    int nested;
    wchar_t dir[MAX_PATH];
} ZiListJob;

static DWORD WINAPI zi_list_thread(LPVOID param) {
    ZiListJob *job = (ZiListJob *)param;
    ZiListBuild build;
    ZiListNote *note;
    int result;

    ZeroMemory(&build, sizeof build);
    build.generation = job->generation;
    build.nested = job->nested;
    wcscpy_s(build.root, MAX_PATH, job->dir);
    build.files = (ZiFile *)calloc(ZI_MAX_FILES, sizeof(ZiFile));
    if (!build.files) {
        free(job);
        return 0;
    }
    result = zi_grid_collect(&build, job->dir, 0);
    if (!g_grid_alive || job->generation != g_grid_generation) {
        free(build.files);
        free(job);
        return 0;
    }
    note = (ZiListNote *)calloc(1, sizeof *note);
    if (!note) {
        free(build.files);
        free(job);
        return 0;
    }
    note->generation = job->generation;
    note->failed = result < 0;
    note->capped = build.capped;
    note->count = build.count;
    note->files = build.files;
    if (!PostMessageW(job->hwnd, ZI_WM_LIST, 0, (LPARAM)note)) {
        free(build.files);
        free(note);
    }
    free(job);
    return 0;
}

static void zi_grid_list(ZiApp *app, const wchar_t *directory, int nested) {
    ZiListBuild build;
    ZiListJob *job;
    HANDLE thread;
    int result;

    app->file_nested = nested;
    if (nested) {
        app->file_reading = 1;
        wcscpy_s(app->notice, 192, L"Reading subfolders.");
        job = (ZiListJob *)calloc(1, sizeof *job);
        if (!job) {
            app->file_reading = 0;
            wcscpy_s(app->notice, 192, L"That folder could not be read.");
            return;
        }
        job->hwnd = app->hwnd;
        job->generation = app->file_generation;
        job->nested = 1;
        wcscpy_s(job->dir, MAX_PATH, directory);
        thread = CreateThread(NULL, 0, zi_list_thread, job, 0, NULL);
        if (thread) {
            CloseHandle(thread);
        } else {
            free(job);
            app->file_reading = 0;
            wcscpy_s(app->notice, 192, L"That folder could not be read.");
        }
        return;
    }
    ZeroMemory(&build, sizeof build);
    build.generation = app->file_generation;
    build.nested = 0;
    wcscpy_s(build.root, MAX_PATH, directory);
    build.files = (ZiFile *)calloc(ZI_MAX_FILES, sizeof(ZiFile));
    if (!build.files) {
        wcscpy_s(app->notice, 192, L"That folder could not be read.");
        return;
    }
    result = zi_grid_collect(&build, directory, 0);
    zi_grid_take(app, build.files, build.count, build.capped, result < 0);
}

void zi_grid_install(ZiApp *app, ZiFile *files, int count) {
    LONG generation = InterlockedIncrement(&g_grid_generation);
    int i;

    app->file_generation = generation;
    zi_grid_free_files(app);
    app->file_reading = 0;
    if (!files || count < 0) {
        count = 0;
    }
    if (count > ZI_MAX_FILES) {
        count = ZI_MAX_FILES;
    }
    for (i = 0; i < count; i++) {
        app->files[i] = files[i];
        files[i].thumb = NULL;
    }
    app->file_count = count;
    free(files);
    zi_grid_start_thumbs(app);
}

void zi_grid_clear(ZiApp *app) {
    LONG generation = InterlockedIncrement(&g_grid_generation);

    app->file_generation = generation;
    zi_grid_free_files(app);
    app->file_dir[0] = L'\0';
    app->file_reading = 0;
}

void zi_grid_load(ZiApp *app) {
    wchar_t dir[MAX_PATH];
    LONG generation;
    int nested;

    /* Review, Duplicates, Events, and Reorganize own the grid until Photos is shown again. */
    if (app->events_open || app->reorg_open) {
        return;
    }
    if (app->source_server && app->library_section != 0) {
        return;
    }
    if (!app->hwnd || !zi_current_folder(app, dir, MAX_PATH)) {
        generation = InterlockedIncrement(&g_grid_generation);
        app->file_generation = generation;
        zi_grid_free_files(app);
        app->file_dir[0] = L'\0';
        app->file_reading = 0;
        return;
    }
    /* The Mac library lists every still under the selected folder. Photos 2022
       holds those pictures in child folders, so Server always walks them.
       Disk walks child folders only while Sub-folder is on. */
    nested = app->source_server || app->subfolders;
    if (wcscmp(app->file_dir, dir) == 0 && app->file_nested == nested) {
        return;
    }
    generation = InterlockedIncrement(&g_grid_generation);
    app->file_generation = generation;
    zi_grid_free_files(app);
    app->notice[0] = L'\0';
    wcscpy_s(app->file_dir, MAX_PATH, dir);
    zi_grid_list(app, dir, nested);
}

void zi_grid_accept(ZiApp *app, ZiListNote *note) {
    if (!note) {
        return;
    }
    if (!app || note->generation != app->file_generation) {
        free(note->files);
        free(note);
        return;
    }
    zi_grid_take(app, note->files, note->count, note->capped, note->failed);
    note->files = NULL;
    free(note);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

void zi_grid_shutdown(ZiApp *app) {
    g_grid_alive = 0;
    InterlockedIncrement(&g_grid_generation);
    zi_grid_free_files(app);
    if (g_vscroll) {
        DestroyWindow(g_vscroll);
        g_vscroll = NULL;
    }
}

int zi_grid_active(const ZiApp *app) {
    if (!zi_showing_folder(app) || app->events_open || app->reorg_open) {
        return 0;
    }
    if (app->source_server && app->library_section != 0) {
        return 1;
    }
    return app->file_dir[0] != L'\0';
}

static int zi_grid_dip(const ZiApp *app) {
    int dip = app->preview_dip;

    if (dip < 80) {
        dip = 80;
    }
    if (dip > 480) {
        dip = 480;
    }
    return dip;
}

static int zi_grid_header(const ZiApp *app) {
    return zi_px(app, 44);
}

static int zi_grid_cell_px(const ZiApp *app) {
    int cell = zi_px(app, zi_grid_dip(app));

    if (cell < 1) {
        cell = 1;
    }
    return cell;
}

static int zi_grid_bar_w(const ZiApp *app) {
    UINT dpi = app->dpi ? (UINT)app->dpi : 96;
    int w = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);

    if (w < 8) {
        w = 8;
    }
    return w;
}

static int zi_grid_cols(const ZiApp *app, const RECT *canvas) {
    int pad = zi_px(app, 16);
    int gap = zi_px(app, 12);
    int cell = zi_grid_cell_px(app);
    int width = (canvas->right - canvas->left) - pad * 2 - zi_grid_bar_w(app);
    int cols;

    if (width < cell) {
        width = cell;
    }
    cols = (width + gap) / (cell + gap);
    if (cols < 1) {
        cols = 1;
    }
    return cols;
}

static int zi_grid_stride(const ZiApp *app) {
    return zi_grid_cell_px(app) + zi_px(app, 44) + zi_px(app, 12);
}

static void zi_grid_header_layout(ZiApp *app, HDC hdc, RECT *track, RECT *value, RECT *title) {
    RECT canvas = app->regions.canvas;
    int pad = zi_px(app, 16);
    int x = canvas.left + pad;
    int y = canvas.top + zi_px(app, 8);
    int h = zi_px(app, 26);
    int label_w = zi_px(app, 100);
    int track_w = zi_px(app, 140);
    int value_w = zi_px(app, 56);
    SIZE size;
    HFONT old;

    if (hdc) {
        old = (HFONT)SelectObject(hdc, app->font);
        GetTextExtentPoint32W(hdc, L"Preview size", 12, &size);
        SelectObject(hdc, old);
        label_w = size.cx + zi_px(app, 8);
    }
    x += label_w;
    if (track) {
        track->left = x;
        track->top = y;
        track->right = x + track_w;
        track->bottom = y + h;
    }
    x += track_w + zi_px(app, 8);
    if (value) {
        value->left = x;
        value->top = y;
        value->right = x + value_w;
        value->bottom = y + h;
    }
    x += value_w + zi_px(app, 12);
    if (title) {
        title->left = x;
        title->top = canvas.top;
        title->right = canvas.right - pad;
        title->bottom = canvas.top + zi_grid_header(app);
    }
}

static void zi_grid_span(const ZiApp *app, int *content, int *view) {
    RECT canvas = app->regions.canvas;
    int cols = zi_grid_cols(app, &canvas);
    int rows = app->file_count == 0 ? 0 : (app->file_count + cols - 1) / cols;
    int height = zi_px(app, 16) + rows * zi_grid_stride(app) + zi_px(app, 16);
    int body = (canvas.bottom - canvas.top) - zi_grid_header(app);

    if (height < 1) {
        height = 1;
    }
    if (body < 1) {
        body = 1;
    }
    *content = height;
    *view = body;
}

static int zi_grid_max_scroll(const ZiApp *app) {
    int content;
    int view;
    int max_scroll;

    zi_grid_span(app, &content, &view);
    max_scroll = content - view;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    return max_scroll;
}

static void zi_grid_clamp(ZiApp *app) {
    int max_scroll = zi_grid_max_scroll(app);

    if (app->file_scroll > max_scroll) {
        app->file_scroll = max_scroll;
    }
    if (app->file_scroll < 0) {
        app->file_scroll = 0;
    }
}

static LRESULT CALLBACK zi_vscroll_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_MOUSEWHEEL) {
        SendMessageW(GetParent(hwnd), WM_MOUSEWHEEL, wparam, lparam);
        return 0;
    }
    return CallWindowProcW(g_vscroll_proc, hwnd, msg, wparam, lparam);
}

static void zi_move_bar(HWND bar, int x, int y, int w, int h, int show) {
    RECT now;

    if (!bar) {
        return;
    }
    if (!show || w < 1 || h < 1) {
        ShowWindow(bar, SW_HIDE);
        return;
    }
    GetWindowRect(bar, &now);
    MapWindowPoints(HWND_DESKTOP, GetParent(bar), (POINT *)&now, 2);
    if (now.left != x || now.top != y || now.right != x + w || now.bottom != y + h) {
        MoveWindow(bar, x, y, w, h, TRUE);
    }
    if (!IsWindowVisible(bar)) {
        ShowWindow(bar, SW_SHOW);
    }
}

void zi_grid_sync_bar(ZiApp *app) {
    RECT canvas;
    SCROLLINFO si;
    int content;
    int view;
    int top;
    int height;
    int width;
    int show;

    if (!app || !app->hwnd) {
        return;
    }
    if (!g_vscroll) {
        g_vscroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | SBS_VERT,
                                     0, 0, 0, 0, app->hwnd, NULL, GetModuleHandleW(NULL), NULL);
        if (g_vscroll) {
            g_vscroll_proc = (WNDPROC)SetWindowLongPtrW(g_vscroll, GWLP_WNDPROC, (LONG_PTR)zi_vscroll_proc);
        }
    }
    show = g_vscroll && zi_grid_active(app);
    zi_grid_clamp(app);
    if (!show) {
        zi_move_bar(g_vscroll, 0, 0, 0, 0, 0);
        return;
    }
    canvas = app->regions.canvas;
    width = zi_grid_bar_w(app);
    top = canvas.top + zi_grid_header(app);
    height = canvas.bottom - top;
    zi_move_bar(g_vscroll, canvas.right - width, top, width, height, height > 0);
    zi_grid_span(app, &content, &view);
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = content - 1;
    si.nPage = (UINT)view;
    si.nPos = app->file_scroll;
    SetScrollInfo(g_vscroll, SB_CTL, &si, TRUE);
}

int zi_grid_on_vscroll(ZiApp *app, HWND bar, WPARAM wparam) {
    SCROLLINFO si;
    int pos;
    int code;
    int content;
    int view;
    int line;

    if (!app || bar != g_vscroll) {
        return 0;
    }
    code = LOWORD(wparam);
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    GetScrollInfo(bar, SB_CTL, &si);
    zi_grid_span(app, &content, &view);
    line = zi_grid_stride(app);
    pos = app->file_scroll;
    switch (code) {
    case SB_LINEUP:
        pos -= line;
        break;
    case SB_LINEDOWN:
        pos += line;
        break;
    case SB_PAGEUP:
        pos -= view;
        break;
    case SB_PAGEDOWN:
        pos += view;
        break;
    case SB_TOP:
        pos = 0;
        break;
    case SB_BOTTOM:
        pos = content;
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        /* HIWORD(wparam) stops at 16 bits. nTrackPos keeps the full offset. */
        pos = si.nTrackPos;
        break;
    default:
        return 1;
    }
    app->file_scroll = pos;
    zi_grid_clamp(app);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
    return 1;
}

static int zi_grid_cell(const ZiApp *app, int index, RECT *image, RECT *label) {
    RECT canvas = app->regions.canvas;
    int pad = zi_px(app, 16);
    int gap = zi_px(app, 12);
    int cell = zi_grid_cell_px(app);
    int name_h = zi_px(app, 44);
    int cols = zi_grid_cols(app, &canvas);
    int col = index % cols;
    int row = index / cols;
    int top = canvas.top + zi_grid_header(app) + pad - app->file_scroll;

    image->left = canvas.left + pad + col * (cell + gap);
    image->top = top + row * (cell + name_h + gap);
    image->right = image->left + cell;
    image->bottom = image->top + cell;
    label->left = image->left;
    label->right = image->right;
    label->top = image->bottom + zi_px(app, 4);
    label->bottom = label->top + name_h;
    return 1;
}

void zi_grid_paint(ZiApp *app, HDC hdc) {
    RECT canvas = app->regions.canvas;
    RECT title;
    RECT track;
    RECT value;
    RECT label_rc;
    RECT clip;
    HFONT old_font;
    int saved;
    int i;
    int thumb_x;
    int span;
    wchar_t points[16];
    double t;
    const wchar_t *base = wcsrchr(app->file_dir, L'\\');
    HGDIOBJ old_pen;

    zi_grid_clamp(app);
    base = (base && base[1]) ? base + 1 : app->file_dir;
    zi_grid_header_layout(app, hdc, &track, &value, &title);
    app->preview_track = track;
    old_font = (HFONT)SelectObject(hdc, app->font);
    SetTextColor(hdc, ZI_INK);
    SetBkMode(hdc, TRANSPARENT);
    label_rc.left = canvas.left + zi_px(app, 16);
    label_rc.top = track.top;
    label_rc.right = track.left;
    label_rc.bottom = track.bottom;
    DrawTextW(hdc, L"Preview size", -1, &label_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    t = ((double)zi_grid_dip(app) - 80.0) / (480.0 - 80.0);
    if (t < 0) {
        t = 0;
    }
    if (t > 1) {
        t = 1;
    }
    span = (track.right - track.left) - zi_px(app, 12);
    if (span < 1) {
        span = 1;
    }
    thumb_x = track.left + zi_px(app, 6) + (int)(t * span);
    old_pen = SelectObject(hdc, app->muted_pen);
    MoveToEx(hdc, track.left, (track.top + track.bottom) / 2, NULL);
    LineTo(hdc, track.right, (track.top + track.bottom) / 2);
    SelectObject(hdc, app->tool_pen);
    Ellipse(hdc, thumb_x - zi_px(app, 5), (track.top + track.bottom) / 2 - zi_px(app, 5),
            thumb_x + zi_px(app, 5), (track.top + track.bottom) / 2 + zi_px(app, 5));
    SelectObject(hdc, old_pen);
    swprintf_s(points, 16, L"%d pt", zi_grid_dip(app));
    DrawTextW(hdc, points, -1, &value, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    DrawTextW(hdc, base, -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    saved = SaveDC(hdc);
    clip = canvas;
    clip.top = title.bottom;
    clip.right -= zi_grid_bar_w(app);
    if (clip.right < clip.left) {
        clip.right = clip.left;
    }
    IntersectClipRect(hdc, clip.left, clip.top, clip.right, clip.bottom);
    if (app->file_count == 0) {
        RECT message = clip;
        InflateRect(&message, -zi_px(app, 24), -zi_px(app, 24));
        SetTextColor(hdc, ZI_MUTED);
        {
            const wchar_t *empty = L"No still images or videos in this folder.";

            if (app->file_reading) {
                empty = L"Reading subfolders.";
            } else if (app->grid_note[0]) {
                empty = app->grid_note;
            }
            DrawTextW(hdc, empty, -1, &message, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        }
    }
    for (i = 0; i < app->file_count; i++) {
        RECT image;
        RECT label;
        ZiFile *file = &app->files[i];
        HBRUSH well = app->list_brush;
        HGDIOBJ old_brush;
        HGDIOBJ cell_pen;

        zi_grid_cell(app, i, &image, &label);
        if (image.bottom < clip.top || image.top > canvas.bottom) {
            continue;
        }
        old_brush = SelectObject(hdc, well);
        cell_pen = SelectObject(hdc, app->hair);
        RoundRect(hdc, image.left, image.top, image.right, image.bottom, zi_px(app, 8), zi_px(app, 8));
        SelectObject(hdc, cell_pen);
        SelectObject(hdc, old_brush);
        if (file->thumb) {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old_bmp = SelectObject(mem, file->thumb);
            int dw;
            int dh;
            int x;
            int y;
            int side = image.right - image.left - zi_px(app, 8);

            if (file->thumb_w > 0 && file->thumb_h > 0 && side > 0) {
                dw = side;
                dh = (file->thumb_h * side) / file->thumb_w;
                if (dh > side) {
                    dh = side;
                    dw = (file->thumb_w * side) / file->thumb_h;
                }
                x = image.left + (image.right - image.left - dw) / 2;
                y = image.top + (image.bottom - image.top - dh) / 2;
                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, NULL);
                StretchBlt(hdc, x, y, dw, dh, mem, 0, 0, file->thumb_w, file->thumb_h, SRCCOPY);
            }
            SelectObject(mem, old_bmp);
            DeleteDC(mem);
        } else if (file->video) {
            SetTextColor(hdc, ZI_MUTED);
            DrawTextW(hdc, L"Video", -1, &image, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else if (file->failed) {
            SetTextColor(hdc, ZI_MUTED);
            DrawTextW(hdc, L"No preview", -1, &image, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (app->file_mark[i]) {
            HGDIOBJ old = SelectObject(hdc, app->tool_pen);
            HGDIOBJ brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, image.left + 1, image.top + 1, image.right - 1, image.bottom - 1);
            SelectObject(hdc, brush);
            SelectObject(hdc, old);
        }
        SetTextColor(hdc, ZI_INK);
        DrawTextW(hdc, file->name, -1, &label,
                  DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EXTERNALLEADING);
    }
    RestoreDC(hdc, saved);
    SelectObject(hdc, old_font);
}

void zi_grid_thumb(ZiApp *app, ZiThumbNote *note) {
    ZiFile *file;

    if (!note) {
        return;
    }
    if (note->generation != app->file_generation || note->index < 0 || note->index >= app->file_count) {
        if (note->bitmap) {
            DeleteObject(note->bitmap);
        }
        free(note);
        return;
    }
    file = &app->files[note->index];
    if (file->thumb) {
        DeleteObject(file->thumb);
    }
    file->thumb = note->bitmap;
    file->thumb_w = note->w;
    file->thumb_h = note->h;
    file->failed = note->failed;
    zi_editor_note_thumb(file);
    zi_editor_on_decoded_thumb(app, file);
    free(note);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

int zi_grid_click(ZiApp *app, int x, int y) {
    POINT pt;
    int i;

    if (!zi_grid_active(app)) {
        return 0;
    }
    pt.x = x;
    pt.y = y;
    if (!PtInRect(&app->regions.canvas, pt)) {
        return 0;
    }
    zi_grid_clamp(app);
    if (pt.y < app->regions.canvas.top + zi_grid_header(app)) {
        if (app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return 1;
    }
    {
        int index = -1;
        int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        int extend = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        for (i = 0; i < app->file_count; i++) {
            RECT image;
            RECT label;
            RECT hit;

            zi_grid_cell(app, i, &image, &label);
            hit = image;
            hit.bottom = label.bottom;
            if (PtInRect(&hit, pt)) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            memset(app->file_mark, 0, sizeof app->file_mark);
            app->file_selected = -1;
            app->file_anchor = -1;
        } else if (shift && app->file_anchor >= 0 && app->file_anchor < app->file_count) {
            int lo = app->file_anchor < index ? app->file_anchor : index;
            int hi = app->file_anchor > index ? app->file_anchor : index;
            int n;

            memset(app->file_mark, 0, sizeof app->file_mark);
            for (n = lo; n <= hi; n++) {
                app->file_mark[n] = 1;
            }
            app->file_selected = index;
        } else if (extend) {
            app->file_mark[index] = !app->file_mark[index];
            if (app->file_mark[index]) {
                app->file_selected = index;
                app->file_anchor = index;
            } else if (app->file_selected == index) {
                app->file_selected = -1;
                for (i = 0; i < app->file_count; i++) {
                    if (app->file_mark[i]) {
                        app->file_selected = i;
                        break;
                    }
                }
            }
        } else {
            memset(app->file_mark, 0, sizeof app->file_mark);
            app->file_mark[index] = 1;
            app->file_selected = index;
            app->file_anchor = index;
        }
    }
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
    return 1;
}

void zi_grid_wheel(ZiApp *app, int delta) {
    int step = delta > 0 ? -1 : 1;

    app->file_scroll += step * zi_grid_stride(app);
    zi_grid_clamp(app);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

int zi_grid_index_at(ZiApp *app, int x, int y) {
    POINT pt;
    int i;

    pt.x = x;
    pt.y = y;
    if (!zi_grid_active(app) || !PtInRect(&app->regions.canvas, pt)) {
        return -1;
    }
    if (y < app->regions.canvas.top + zi_grid_header(app)) {
        return -1;
    }
    for (i = 0; i < app->file_count; i++) {
        RECT image;
        RECT label;
        RECT hit;

        zi_grid_cell(app, i, &image, &label);
        hit = image;
        hit.bottom = label.bottom;
        if (PtInRect(&hit, pt)) {
            return i;
        }
    }
    return -1;
}

void zi_grid_select_only(ZiApp *app, int index) {
    RECT image;
    RECT label;
    int header;
    int top_limit;
    int bottom_limit;

    memset(app->file_mark, 0, sizeof app->file_mark);
    if (index < 0 || index >= app->file_count) {
        app->file_selected = -1;
        app->file_anchor = -1;
        return;
    }
    app->file_mark[index] = 1;
    app->file_selected = index;
    app->file_anchor = index;
    header = zi_grid_header(app);
    top_limit = app->regions.canvas.top + header;
    bottom_limit = app->regions.canvas.bottom;
    zi_grid_cell(app, index, &image, &label);
    if (image.top < top_limit) {
        app->file_scroll -= top_limit - image.top;
    } else if (label.bottom > bottom_limit) {
        app->file_scroll += label.bottom - bottom_limit;
    }
    zi_grid_clamp(app);
}

static void zi_grid_track(ZiApp *app, RECT *track) {
    HDC hdc = app->hwnd ? GetDC(app->hwnd) : NULL;

    zi_grid_header_layout(app, hdc, track, NULL, NULL);
    if (hdc) {
        ReleaseDC(app->hwnd, hdc);
    }
}

int zi_grid_preview_hit(ZiApp *app, int x, int y) {
    RECT track;
    POINT pt;

    if (!zi_grid_active(app)) {
        return 0;
    }
    pt.x = x;
    pt.y = y;
    zi_grid_track(app, &track);
    return PtInRect(&track, pt);
}

void zi_grid_preview_drag(ZiApp *app, int x) {
    RECT track;
    double t;
    int dip;
    int width;

    zi_grid_track(app, &track);
    width = track.right - track.left;
    if (width < 1) {
        return;
    }
    t = (double)(x - track.left) / (double)width;
    if (t < 0) {
        t = 0;
    }
    if (t > 1) {
        t = 1;
    }
    dip = 80 + (int)(t * 400.0 + 0.5);
    if (dip < 80) {
        dip = 80;
    }
    if (dip > 480) {
        dip = 480;
    }
    app->preview_dip = dip;
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}
