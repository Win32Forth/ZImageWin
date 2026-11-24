/* Folder grid. The catalog keeps light names. Thumbnails decode for the page on screen. */
#include "grid.h"
#include "editor.h"
#include "blake3.h"

#include <wincodec.h>
#include <oleauto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG g_grid_generation;
static volatile LONG g_grid_alive = 1;
static HWND g_vscroll;
static WNDPROC g_vscroll_proc;

static const wchar_t *const k_stills[] = {
    L"jpg", L"jpeg", L"png", L"tif", L"tiff", L"heic", L"heif", L"webp", L"gif", L"bmp",
    L"dng", L"raf", L"arw", L"cr2", L"nef", L"orf", L"rw2", L"raw", L"x3f", L"pdf"
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

/* Name, path, and video flag. No thumbnail lives in the catalog. */
typedef struct ZiCatItem {
    wchar_t name[ZI_NAME];
    wchar_t path[MAX_PATH];
    int video;
} ZiCatItem;

typedef struct ZiCatPack {
    int still_count;
    int video_count;
    ZiCatItem items[1];
} ZiCatPack;

static ZiCatPack *g_pack;
static ZiCatItem *g_stills;
static ZiCatItem *g_videos;
static int g_still_count;
static int g_video_count;
static int g_catalog_capped;
static int g_have_catalog;

static int zi_file_name_less(const void *a, const void *b) {
    const ZiFile *fa = (const ZiFile *)a;
    const ZiFile *fb = (const ZiFile *)b;

    return _wcsicmp(fa->name, fb->name);
}

static int zi_cat_name_less(const void *a, const void *b) {
    const ZiCatItem *fa = (const ZiCatItem *)a;
    const ZiCatItem *fb = (const ZiCatItem *)b;

    return _wcsicmp(fa->name, fb->name);
}

static int zi_cat_ptr_less(const void *a, const void *b) {
    const ZiCatItem *fa = *(const ZiCatItem *const *)a;
    const ZiCatItem *fb = *(const ZiCatItem *const *)b;

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
    volatile LONG next;
    volatile LONG workers;
    wchar_t path[ZI_MAX_FILES][MAX_PATH];
} ZiGridJob;

static DWORD WINAPI zi_grid_thread(LPVOID param) {
    ZiGridJob *job = (ZiGridJob *)param;
    IWICImagingFactory *factory = NULL;

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        if (InterlockedDecrement(&job->workers) == 0) {
            free(job);
        }
        return 0;
    }
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
    for (;;) {
        int i = (int)InterlockedIncrement(&job->next) - 1;
        ZiThumbNote *note;
        HBITMAP bitmap = NULL;
        int w = 0;
        int h = 0;

        if (i >= job->count || !g_grid_alive || job->generation != g_grid_generation) {
            break;
        }
        if (!factory || !job->path[i][0]) {
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
    if (InterlockedDecrement(&job->workers) == 0) {
        free(job);
    }
    return 0;
}

typedef struct ZiListBuild {
    LONG generation;
    int nested;
    int capped;
    int oom;
    wchar_t root[MAX_PATH];
    ZiCatItem *stills;
    int still_count;
    int still_cap;
    ZiCatItem *videos;
    int video_count;
    int video_cap;
} ZiListBuild;

static int zi_cat_reserve(ZiCatItem **items, int *cap, int count) {
    int next;
    ZiCatItem *grown;

    if (count < *cap) {
        return 1;
    }
    next = *cap > 0 ? *cap * 2 : 256;
    if (next <= count) {
        next = count + 1;
    }
    if (next > ZI_CATALOG_CAP) {
        next = ZI_CATALOG_CAP;
    }
    if (count >= next) {
        return 0;
    }
    grown = (ZiCatItem *)realloc(*items, (size_t)next * sizeof *grown);
    if (!grown) {
        return 0;
    }
    *items = grown;
    *cap = next;
    return 1;
}

static int zi_grid_add(ZiListBuild *build, const wchar_t *dir, const wchar_t *name, int video) {
    ZiCatItem *slot;
    wchar_t relative[ZI_NAME];
    size_t root_len;

    if (build->still_count + build->video_count >= ZI_CATALOG_CAP) {
        build->capped = 1;
        return 1;
    }
    /* C:\ keeps its slash. That slash is the separator, not part of the prefix. */
    root_len = wcslen(build->root);
    if (root_len > 0 && (build->root[root_len - 1] == L'\\' || build->root[root_len - 1] == L'/')) {
        root_len--;
    }
    if (root_len > 0 &&
        _wcsnicmp(dir, build->root, root_len) == 0 &&
        (dir[root_len] == L'\\' || dir[root_len] == L'/' || dir[root_len] == L'\0')) {
        const wchar_t *rest = dir + root_len;

        if (*rest == L'\\' || *rest == L'/') {
            rest++;
        }
        if (rest[0] == L'\0') {
            if (wcscpy_s(relative, ZI_NAME, name) != 0) {
                return 0;
            }
        } else if (swprintf_s(relative, ZI_NAME, L"%s\\%s", rest, name) < 0) {
            return 0;
        }
    } else if (wcscpy_s(relative, ZI_NAME, name) != 0) {
        return 0;
    }
    if (wcslen(dir) + wcslen(name) + 2 >= MAX_PATH) {
        return 0;
    }
    if (video) {
        if (!zi_cat_reserve(&build->videos, &build->video_cap, build->video_count)) {
            build->oom = 1;
            return 1;
        }
        slot = &build->videos[build->video_count];
    } else {
        if (!zi_cat_reserve(&build->stills, &build->still_cap, build->still_count)) {
            build->oom = 1;
            return 1;
        }
        slot = &build->stills[build->still_count];
    }
    if (wcscpy_s(slot->name, ZI_NAME, relative) != 0) {
        return 0;
    }
    if (wcscpy_s(slot->path, MAX_PATH, dir) != 0 ||
        wcscat_s(slot->path, MAX_PATH, L"\\") != 0 ||
        wcscat_s(slot->path, MAX_PATH, name) != 0) {
        return 0;
    }
    slot->video = video;
    if (video) {
        build->video_count++;
    } else {
        build->still_count++;
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
    HANDLE threads[4];
    int i;
    int n;
    int made = 0;
    int stills = 0;

    job = (ZiGridJob *)calloc(1, sizeof *job);
    if (!job) {
        return;
    }
    job->hwnd = app->hwnd;
    job->generation = app->file_generation;
    job->next = 0;
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
    for (n = 0; n < 4; n++) {
        threads[n] = CreateThread(NULL, 0, zi_grid_thread, job, CREATE_SUSPENDED, NULL);
        if (threads[n]) {
            made++;
        }
    }
    if (!made) {
        free(job);
        return;
    }
    job->workers = made;
    for (n = 0; n < 4; n++) {
        if (!threads[n]) {
            continue;
        }
        ResumeThread(threads[n]);
        CloseHandle(threads[n]);
    }
}

static int zi_page_count_from(int filtered, int stills, int videos, int matches) {
    int total;

    if (filtered) {
        total = matches;
    } else if (stills <= 0 && videos > 0) {
        return 1;
    } else {
        total = stills;
    }
    if (total <= 0) {
        return 0;
    }
    return (total + ZI_PAGE_SIZE - 1) / ZI_PAGE_SIZE;
}

static void zi_file_from_cat(ZiFile *file, const ZiCatItem *item) {
    ZeroMemory(file, sizeof *file);
    file->video = item->video;
    wcscpy_s(file->name, ZI_NAME, item->name);
    wcscpy_s(file->path, MAX_PATH, item->path);
}

static int zi_notice_owned(const wchar_t *text) {
    return wcscmp(text, L"Reading subfolders.") == 0 ||
           wcscmp(text, L"The folder shows the first 200 photographs.") == 0 ||
           wcscmp(text, L"The folder shows the first 20000 names.") == 0 ||
           wcscmp(text, L"The folder shows the first 300 videos.") == 0 ||
           wcscmp(text, L"The folder shows the first 20000 names and the first 300 videos.") == 0 ||
           wcscmp(text, L"No photos match that name.") == 0;
}

static void zi_grid_apply_notice(ZiApp *app) {
    int cut_videos = !app->file_filtered && g_video_count > ZI_VIDEO_PAGE;

    if (app->file_filtered && app->file_match_total <= 0) {
        wcscpy_s(app->notice, 192, L"No photos match that name.");
        return;
    }
    if (g_catalog_capped && cut_videos) {
        wcscpy_s(app->notice, 192, L"The folder shows the first 20000 names and the first 300 videos.");
        return;
    }
    if (g_catalog_capped) {
        wcscpy_s(app->notice, 192, L"The folder shows the first 20000 names.");
        return;
    }
    if (cut_videos) {
        wcscpy_s(app->notice, 192, L"The folder shows the first 300 videos.");
        return;
    }
    if (zi_notice_owned(app->notice)) {
        app->notice[0] = L'\0';
    }
}

/* Page 0 mixes the first stills with videos, then sorts that page by name. Later pages are stills. */
static void zi_grid_show_page(ZiApp *app) {
    ZiPageSlice slice;
    ZiCatItem **matched = NULL;
    int match_total = 0;
    int pages;
    int video_n = 0;
    int i;
    int n = 0;

    app->file_generation = InterlockedIncrement(&g_grid_generation);
    zi_grid_free_files(app);
    if (!g_have_catalog) {
        app->file_still_total = 0;
        app->file_video_total = 0;
        app->file_match_total = 0;
        app->file_page = 0;
        return;
    }
    app->file_still_total = g_still_count;
    app->file_video_total = g_video_count;
    app->file_filtered = app->name_filter[0] ? 1 : 0;
    app->file_match_total = 0;
    if (app->file_filtered && g_still_count + g_video_count > 0) {
        matched = (ZiCatItem **)malloc((size_t)(g_still_count + g_video_count) * sizeof *matched);
        if (!matched) {
            wcscpy_s(app->notice, 192, L"That folder could not be read.");
            return;
        }
        for (i = 0; i < g_still_count; i++) {
            if (zi_name_match(g_stills[i].name, app->name_filter)) {
                matched[match_total++] = &g_stills[i];
            }
        }
        for (i = 0; i < g_video_count; i++) {
            if (zi_name_match(g_videos[i].name, app->name_filter)) {
                matched[match_total++] = &g_videos[i];
            }
        }
        if (match_total > 1) {
            qsort(matched, (size_t)match_total, sizeof *matched, zi_cat_ptr_less);
        }
    }
    app->file_match_total = app->file_filtered ? match_total : 0;
    pages = zi_page_count_from(app->file_filtered, app->file_still_total, app->file_video_total, app->file_match_total);
    if (pages <= 0 || app->file_page < 0) {
        app->file_page = 0;
    } else if (app->file_page >= pages) {
        app->file_page = pages - 1;
    }
    zi_page_slice(app->file_page, app->file_still_total, app->file_filtered, app->file_match_total, &slice);
    if (app->file_filtered) {
        for (i = 0; i < slice.filtered_count && n < ZI_MAX_FILES; i++) {
            zi_file_from_cat(&app->files[n], matched[slice.filtered_begin + i]);
            n++;
        }
    } else if (slice.include_videos) {
        video_n = g_video_count;
        if (video_n > ZI_VIDEO_PAGE) {
            video_n = ZI_VIDEO_PAGE;
        }
        for (i = 0; i < slice.still_count && n < ZI_MAX_FILES; i++) {
            zi_file_from_cat(&app->files[n], &g_stills[slice.still_begin + i]);
            n++;
        }
        for (i = 0; i < video_n && n < ZI_MAX_FILES; i++) {
            zi_file_from_cat(&app->files[n], &g_videos[i]);
            n++;
        }
        if (n > 1) {
            qsort(app->files, (size_t)n, sizeof app->files[0], zi_file_name_less);
        }
    } else {
        for (i = 0; i < slice.still_count && n < ZI_MAX_FILES; i++) {
            zi_file_from_cat(&app->files[n], &g_stills[slice.still_begin + i]);
            n++;
        }
    }
    app->file_count = n;
    free(matched);
    zi_grid_apply_notice(app);
    zi_grid_start_thumbs(app);
}

static void zi_build_free(ZiListBuild *build) {
    free(build->stills);
    free(build->videos);
    build->stills = NULL;
    build->videos = NULL;
    build->still_cap = 0;
    build->video_cap = 0;
}

static void zi_cat_sort_build(ZiListBuild *build) {
    if (build->still_count > 1) {
        qsort(build->stills, (size_t)build->still_count, sizeof(ZiCatItem), zi_cat_name_less);
    }
    if (build->video_count > 1) {
        qsort(build->videos, (size_t)build->video_count, sizeof(ZiCatItem), zi_cat_name_less);
    }
}

static ZiCatPack *zi_cat_pack(const ZiListBuild *build) {
    size_t n = (size_t)build->still_count + (size_t)build->video_count;
    size_t bytes = sizeof(ZiCatPack);
    ZiCatPack *pack;

    if (n > 1) {
        bytes += (n - 1) * sizeof(ZiCatItem);
    }
    pack = (ZiCatPack *)malloc(bytes);
    if (!pack) {
        return NULL;
    }
    pack->still_count = build->still_count;
    pack->video_count = build->video_count;
    if (build->still_count > 0) {
        memcpy(pack->items, build->stills, (size_t)build->still_count * sizeof(ZiCatItem));
    }
    if (build->video_count > 0) {
        memcpy(pack->items + build->still_count, build->videos, (size_t)build->video_count * sizeof(ZiCatItem));
    }
    return pack;
}

static void zi_catalog_drop(void) {
    free(g_pack);
    g_pack = NULL;
    g_stills = NULL;
    g_videos = NULL;
    g_still_count = 0;
    g_video_count = 0;
    g_catalog_capped = 0;
    g_have_catalog = 0;
}

static void zi_grid_clear_counts(ZiApp *app) {
    app->file_page = 0;
    app->file_still_total = 0;
    app->file_video_total = 0;
    app->file_match_total = 0;
    app->file_filtered = 0;
}

static void zi_grid_adopt(ZiApp *app, ZiCatPack *pack, int capped, int failed) {
    app->file_reading = 0;
    if (failed || !pack) {
        free(pack);
        zi_catalog_drop();
        zi_grid_free_files(app);
        zi_grid_clear_counts(app);
        wcscpy_s(app->notice, 192, L"That folder could not be read.");
        return;
    }
    zi_catalog_drop();
    g_pack = pack;
    g_still_count = pack->still_count;
    g_video_count = pack->video_count;
    if (g_still_count < 0) {
        g_still_count = 0;
    }
    if (g_video_count < 0) {
        g_video_count = 0;
    }
    if (g_still_count > ZI_CATALOG_CAP) {
        g_still_count = ZI_CATALOG_CAP;
    }
    if (g_still_count + g_video_count > ZI_CATALOG_CAP) {
        g_video_count = ZI_CATALOG_CAP - g_still_count;
    }
    g_stills = g_pack->items;
    g_videos = g_pack->items + g_still_count;
    g_catalog_capped = capped;
    g_have_catalog = 1;
    app->file_page = 0;
    zi_grid_show_page(app);
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
    ZiCatPack *pack = NULL;
    int result;
    int capped;
    int failed;

    ZeroMemory(&build, sizeof build);
    build.generation = job->generation;
    build.nested = job->nested;
    wcscpy_s(build.root, MAX_PATH, job->dir);
    result = zi_grid_collect(&build, job->dir, 0);
    if (!g_grid_alive || job->generation != g_grid_generation) {
        zi_build_free(&build);
        free(job);
        return 0;
    }
    capped = build.capped;
    failed = result < 0 || build.oom;
    if (!failed) {
        zi_cat_sort_build(&build);
        pack = zi_cat_pack(&build);
        if (!pack) {
            failed = 1;
        }
    }
    zi_build_free(&build);
    if (!g_grid_alive || job->generation != g_grid_generation) {
        free(pack);
        free(job);
        return 0;
    }
    note = (ZiListNote *)calloc(1, sizeof *note);
    if (!note) {
        free(pack);
        free(job);
        return 0;
    }
    note->generation = job->generation;
    note->failed = failed;
    note->capped = capped;
    note->count = pack ? pack->still_count + pack->video_count : 0;
    /* One heap block of light names. The UI adopts it; a dropped window frees it. */
    note->files = failed ? NULL : (ZiFile *)pack;
    if (failed) {
        free(pack);
    }
    if (!PostMessageW(job->hwnd, ZI_WM_LIST, 0, (LPARAM)note)) {
        free(note->files);
        free(note);
    }
    free(job);
    return 0;
}

static void zi_grid_list(ZiApp *app, const wchar_t *directory, int nested) {
    ZiListBuild build;
    ZiListJob *job;
    ZiCatPack *pack;
    HANDLE thread;
    int result;
    int capped;

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
    result = zi_grid_collect(&build, directory, 0);
    capped = build.capped;
    if (result < 0 || build.oom) {
        zi_build_free(&build);
        zi_grid_adopt(app, NULL, 0, 1);
        return;
    }
    zi_cat_sort_build(&build);
    pack = zi_cat_pack(&build);
    zi_build_free(&build);
    if (!pack) {
        zi_grid_adopt(app, NULL, 0, 1);
        return;
    }
    zi_grid_adopt(app, pack, capped, 0);
}

void zi_grid_install(ZiApp *app, ZiFile *files, int count) {
    LONG generation = InterlockedIncrement(&g_grid_generation);
    int i;

    app->file_generation = generation;
    zi_catalog_drop();
    zi_grid_free_files(app);
    app->file_reading = 0;
    zi_grid_clear_counts(app);
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
    app->file_still_total = count;
    free(files);
    zi_grid_start_thumbs(app);
}

void zi_grid_clear(ZiApp *app) {
    LONG generation = InterlockedIncrement(&g_grid_generation);

    app->file_generation = generation;
    zi_catalog_drop();
    zi_grid_free_files(app);
    app->file_dir[0] = L'\0';
    app->file_reading = 0;
    zi_grid_clear_counts(app);
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
        zi_catalog_drop();
        zi_grid_free_files(app);
        app->file_dir[0] = L'\0';
        app->file_reading = 0;
        zi_grid_clear_counts(app);
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
    zi_catalog_drop();
    zi_grid_free_files(app);
    app->notice[0] = L'\0';
    app->build_note[0] = L'\0';
    zi_grid_clear_counts(app);
    wcscpy_s(app->file_dir, MAX_PATH, dir);
    zi_grid_list(app, dir, nested);
}

void zi_grid_accept(ZiApp *app, ZiListNote *note) {
    ZiCatPack *pack;

    if (!note) {
        return;
    }
    pack = (ZiCatPack *)note->files;
    note->files = NULL;
    if (!app || note->generation != app->file_generation) {
        free(pack);
        free(note);
        return;
    }
    if (note->failed) {
        free(pack);
        pack = NULL;
    }
    zi_grid_adopt(app, pack, note->capped, note->failed);
    free(note);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

void zi_grid_shutdown(ZiApp *app) {
    g_grid_alive = 0;
    InterlockedIncrement(&g_grid_generation);
    zi_catalog_drop();
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

int zi_grid_header(const ZiApp *app) {
    int base = zi_px(app, 44);
    int notice = app->notice[0] ? zi_px(app, 36) : 0;

    /* Server Photos and Review keep a tool row above the preview slider. */
    if (app->source_server && zi_showing_folder(app) && !app->events_open && !app->reorg_open &&
        app->library_section != 2) {
        base = zi_px(app, 84);
    }
    return base + notice;
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
    int tool = 0;
    int y;
    int h = zi_px(app, 26);
    int label_w = zi_px(app, 100);
    int track_w = zi_px(app, 140);
    int value_w = zi_px(app, 56);
    SIZE size;
    HFONT old;

    if (app->source_server && zi_showing_folder(app) && !app->events_open && !app->reorg_open &&
        app->library_section != 2) {
        tool = zi_px(app, 46);
    }
    y = canvas.top + (tool ? tool : zi_px(app, 8));
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
        title->top = y;
        title->right = canvas.right - pad;
        title->bottom = y + h;
        if (app->filter_field.left > title->left + zi_px(app, 24)) {
            title->right = app->filter_field.left - zi_px(app, 8);
        }
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
    show = g_vscroll && zi_grid_active(app) && !app->merge_open;
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
    if (!app->source_server || app->library_section == 2 || app->events_open || app->reorg_open) {
        DrawTextW(hdc, base, -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (app->notice[0]) {
        RECT note = canvas;

        note.left += zi_px(app, 16);
        note.right -= zi_px(app, 16);
        note.bottom = canvas.top + zi_grid_header(app);
        note.top = note.bottom - zi_px(app, 36);
        SetTextColor(hdc, ZI_INK);
        DrawTextW(hdc, app->notice, -1, &note, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    saved = SaveDC(hdc);
    clip = canvas;
    clip.top = canvas.top + zi_grid_header(app);
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
            } else if (wcscmp(app->notice, L"That folder could not be read.") == 0) {
                empty = app->notice;
            } else if (g_have_catalog && app->file_filtered) {
                empty = L"No photos match that name.";
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

static void zi_grid_invalidate(ZiApp *app) {
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

void zi_grid_set_filter(ZiApp *app, const wchar_t *filter) {
    if (!app) {
        return;
    }
    if (!filter) {
        filter = L"";
    }
    wcsncpy_s(app->name_filter, ZI_NAME, filter, _TRUNCATE);
    app->file_filtered = app->name_filter[0] ? 1 : 0;
    app->file_page = 0;
    if (g_have_catalog) {
        zi_grid_show_page(app);
    }
    zi_grid_invalidate(app);
}

void zi_grid_set_page(ZiApp *app, int page) {
    int pages;

    if (!app || !g_have_catalog) {
        return;
    }
    pages = zi_grid_page_count(app);
    if (pages <= 0) {
        return;
    }
    if (page < 0) {
        page = 0;
    }
    if (page >= pages) {
        page = pages - 1;
    }
    app->file_page = page;
    zi_grid_show_page(app);
    zi_grid_invalidate(app);
}

int zi_grid_still_total(const ZiApp *app) {
    return app ? app->file_still_total : 0;
}

int zi_grid_video_total(const ZiApp *app) {
    return app ? app->file_video_total : 0;
}

int zi_grid_page_count(const ZiApp *app) {
    if (!app || !g_have_catalog) {
        return 0;
    }
    return zi_page_count_from(app->file_filtered, app->file_still_total, app->file_video_total,
                              app->file_match_total);
}

static const unsigned long long k_preview_bytes = 200ull * 1024ull * 1024ull;

typedef struct ZiPrevRec {
    wchar_t name[MAX_PATH];
    unsigned long long size;
    FILETIME when;
} ZiPrevRec;

static int zi_preview_older(const void *a, const void *b) {
    const ZiPrevRec *fa = (const ZiPrevRec *)a;
    const ZiPrevRec *fb = (const ZiPrevRec *)b;

    return CompareFileTime(&fa->when, &fb->when);
}

static int zi_preview_folder(wchar_t *dest, size_t cap) {
    wchar_t temp[MAX_PATH];
    wchar_t parent[MAX_PATH];

    if (!GetTempPathW(MAX_PATH, temp)) {
        return 0;
    }
    if (swprintf_s(parent, MAX_PATH, L"%sZImageWin", temp) < 0) {
        return 0;
    }
    CreateDirectoryW(parent, NULL);
    if (swprintf_s(dest, cap, L"%s\\previews", parent) < 0) {
        return 0;
    }
    CreateDirectoryW(dest, NULL);
    return 1;
}

static void zi_preview_evict(const wchar_t *folder) {
    WIN32_FIND_DATAW found;
    HANDLE search;
    wchar_t pattern[MAX_PATH];
    ZiPrevRec *recs = NULL;
    int count = 0;
    int cap = 0;
    int i;
    unsigned long long total = 0;

    if (swprintf_s(pattern, MAX_PATH, L"%s\\*.jpg", folder) < 0) {
        return;
    }
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        free(recs);
        return;
    }
    do {
        ZiPrevRec *grown;
        unsigned long long bytes;

        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (count >= cap) {
            int next = cap > 0 ? cap * 2 : 64;

            grown = (ZiPrevRec *)realloc(recs, (size_t)next * sizeof *grown);
            if (!grown) {
                break;
            }
            recs = grown;
            cap = next;
        }
        if (wcscpy_s(recs[count].name, MAX_PATH, found.cFileName) != 0) {
            continue;
        }
        bytes = ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        recs[count].size = bytes;
        recs[count].when = found.ftLastWriteTime;
        total += bytes;
        count++;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (count > 1) {
        qsort(recs, (size_t)count, sizeof *recs, zi_preview_older);
    }
    for (i = 0; total > k_preview_bytes && i < count - 1; i++) {
        wchar_t path[MAX_PATH];

        if (swprintf_s(path, MAX_PATH, L"%s\\%s", folder, recs[i].name) < 0) {
            continue;
        }
        if (DeleteFileW(path)) {
            if (total >= recs[i].size) {
                total -= recs[i].size;
            } else {
                total = 0;
            }
        }
    }
    free(recs);
}

static int zi_preview_leaf(const wchar_t *path, wchar_t *leaf, size_t cap) {
    unsigned char digest[32];
    char hex[65];
    int i;

    if (!path || !path[0] || cap < 70) {
        return 0;
    }
    if (!zi_blake3_hash(path, wcslen(path) * sizeof(wchar_t), digest)) {
        return 0;
    }
    zi_blake3_hex(digest, hex);
    for (i = 0; i < 64; i++) {
        leaf[i] = (wchar_t)hex[i];
    }
    leaf[64] = L'\0';
    if (wcscat_s(leaf, cap, L".jpg") != 0) {
        return 0;
    }
    return 1;
}

static int zi_save_thumb_jpeg(IWICImagingFactory *factory, HBITMAP bitmap, const wchar_t *dest) {
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
        FAILED(stream->lpVtbl->InitializeFromFilename(stream, dest, GENERIC_WRITE))) {
        goto done;
    }
    if (FAILED(factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatJpeg, NULL, &encoder)) ||
        FAILED(encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &props))) {
        DeleteFileW(dest);
        goto done;
    }
    ZeroMemory(&option, sizeof option);
    option.pstrName = L"ImageQuality";
    option.dwType = PROPBAG2_TYPE_DATA;
    VariantInit(&setting);
    setting.vt = VT_R4;
    setting.fltVal = 0.7f;
    if (props) {
        props->lpVtbl->Write(props, 1, &option, &setting);
    }
    VariantClear(&setting);
    if (FAILED(frame->lpVtbl->Initialize(frame, props)) || FAILED(frame->lpVtbl->SetSize(frame, w, h))) {
        DeleteFileW(dest);
        goto done;
    }
    frame->lpVtbl->SetPixelFormat(frame, &format);
    if (FAILED(frame->lpVtbl->WriteSource(frame, (IWICBitmapSource *)wic, NULL)) ||
        FAILED(frame->lpVtbl->Commit(frame)) || FAILED(encoder->lpVtbl->Commit(encoder))) {
        DeleteFileW(dest);
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

static void zi_preview_result(ZiApp *app, int photos, int built, int missed) {
    const wchar_t *noun;

    if (photos <= 0) {
        wcscpy_s(app->build_note, 192, L"This folder has no photos.");
        return;
    }
    if (built <= 0 && missed <= 0) {
        wcscpy_s(app->build_note, 192, L"Every photo in this folder already has a preview.");
        return;
    }
    if (built <= 0) {
        wcscpy_s(app->build_note, 192, L"No preview could be read.");
        return;
    }
    noun = built == 1 ? L"preview" : L"previews";
    if (missed <= 0) {
        swprintf_s(app->build_note, 192, L"Built %d %s.", built, noun);
        return;
    }
    if (missed == 1) {
        swprintf_s(app->build_note, 192, L"Built %d %s. 1 photo could not be read.", built, noun);
        return;
    }
    swprintf_s(app->build_note, 192, L"Built %d %s. %d photos could not be read.", built, noun, missed);
}

void zi_grid_build_previews(ZiApp *app) {
    wchar_t folder[MAX_PATH];
    IWICImagingFactory *factory = NULL;
    HRESULT com;
    int balance;
    int built = 0;
    int missed = 0;
    int i;

    if (!app) {
        return;
    }
    if (app->source_server) {
        wcscpy_s(app->build_note, 192, L"Server previews stay on the NAS.");
        zi_grid_invalidate(app);
        return;
    }
    if (app->file_filtered || app->name_filter[0]) {
        wcscpy_s(app->build_note, 192, L"Clear the name filter before building previews.");
        zi_grid_invalidate(app);
        return;
    }
    if (!g_have_catalog || g_still_count <= 0) {
        wcscpy_s(app->build_note, 192, L"This folder has no photos.");
        zi_grid_invalidate(app);
        return;
    }
    if (!zi_preview_folder(folder, MAX_PATH)) {
        wcscpy_s(app->build_note, 192, L"No preview could be read.");
        zi_grid_invalidate(app);
        return;
    }
    com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    balance = com == S_OK || com == S_FALSE;
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                     (void **)&factory);
    for (i = 0; factory && i < g_still_count; i++) {
        wchar_t leaf[80];
        wchar_t dest[MAX_PATH];
        HBITMAP bitmap;
        DWORD attr;
        int tw = 0;
        int th = 0;

        if (g_stills[i].video || !g_stills[i].path[0]) {
            continue;
        }
        if (!zi_preview_leaf(g_stills[i].path, leaf, 80) ||
            swprintf_s(dest, MAX_PATH, L"%s\\%s", folder, leaf) < 0) {
            missed++;
            continue;
        }
        attr = GetFileAttributesW(dest);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        bitmap = zi_decode_thumb(factory, g_stills[i].path, &tw, &th);
        if (!bitmap || tw < 1 || th < 1 || !zi_save_thumb_jpeg(factory, bitmap, dest)) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            missed++;
            continue;
        }
        DeleteObject(bitmap);
        built++;
    }
    if (!factory) {
        missed = g_still_count;
    }
    zi_preview_evict(folder);
    if (factory) {
        factory->lpVtbl->Release(factory);
    }
    if (balance) {
        CoUninitialize();
    }
    zi_preview_result(app, g_still_count, built, missed);
    zi_grid_invalidate(app);
}
