/* Disk folder actions. Helper bodies are built here.
   Pass helper_send 0 unless a later confirmed run asks for the catalog.
   This file does not call the live helper when helper_send is 0. */
#include "folders.h"
#include "features.h"
#include "grid.h"
#include "helper.h"

#include <wincodec.h>
#include <oleauto.h>
#include <shlobj.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_modal;
static int g_merge_scaled;

static int zi_bad_char(wchar_t ch) {
    return ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' ||
           ch == L'<' || ch == L'>' || ch == L'|';
}

int zi_folder_bad_name(const wchar_t *name) {
    const wchar_t *p;

    if (!name || !name[0] || wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) {
        return 1;
    }
    for (p = name; *p; p++) {
        if (*p < 32 || zi_bad_char(*p)) {
            return 1;
        }
    }
    return 0;
}

int zi_folder_reserved(const wchar_t *relative) {
    if (!relative || !relative[0]) {
        return 0;
    }
    if (_wcsicmp(relative, L"Photos") == 0 || _wcsicmp(relative, L"Photos/_Removed") == 0 ||
        _wcsicmp(relative, L"Photos/_Duplicates") == 0 || _wcsicmp(relative, L"Photos/_Review") == 0 ||
        _wcsicmp(relative, L"Photos\\_Removed") == 0 || _wcsicmp(relative, L"Photos\\_Duplicates") == 0 ||
        _wcsicmp(relative, L"Photos\\_Review") == 0) {
        return 1;
    }
    return 0;
}

static int zi_reserved_leaf(const wchar_t *name) {
    return name && (_wcsicmp(name, L"_Removed") == 0 || _wcsicmp(name, L"_Duplicates") == 0 ||
                    _wcsicmp(name, L"_Review") == 0);
}

static int zi_join(wchar_t *dest, size_t cap, const wchar_t *dir, const wchar_t *name) {
    size_t n;

    if (!dest || !dir || !name || cap < 4) {
        return 0;
    }
    n = wcslen(dir);
    if (n + wcslen(name) + 2 >= cap) {
        return 0;
    }
    wcscpy_s(dest, cap, dir);
    if (n > 0 && dest[n - 1] != L'\\' && dest[n - 1] != L'/') {
        wcscat_s(dest, cap, L"\\");
    }
    wcscat_s(dest, cap, name);
    return 1;
}

static void zi_stem_ext(const wchar_t *name, wchar_t *stem, size_t stem_cap, wchar_t *ext, size_t ext_cap) {
    const wchar_t *base = wcsrchr(name, L'\\');
    const wchar_t *dot;

    base = (base && base[1]) ? base + 1 : name;
    wcscpy_s(stem, stem_cap, base);
    ext[0] = L'\0';
    dot = wcsrchr(stem, L'.');
    if (dot && dot != stem) {
        wcscpy_s(ext, ext_cap, dot);
        ((wchar_t *)dot)[0] = L'\0';
    }
}

static int zi_exists_file(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);

    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static int zi_is_reparse(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);

    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static int zi_parent(const wchar_t *path, wchar_t *dest, size_t cap) {
    const wchar_t *slash;

    if (!path || !zi_join(dest, cap, path, L"")) {
        return 0;
    }
    if (dest[0] && (dest[wcslen(dest) - 1] == L'\\' || dest[wcslen(dest) - 1] == L'/')) {
        dest[wcslen(dest) - 1] = L'\0';
    }
    slash = wcsrchr(dest, L'\\');
    if (!slash) {
        slash = wcsrchr(dest, L'/');
    }
    if (!slash) {
        return 0;
    }
    if (slash == dest) {
        return 0;
    }
    dest[slash - dest] = L'\0';
    return dest[0] != L'\0';
}

static int zi_unique_path(const wchar_t *dir, const wchar_t *name, wchar_t *dest, size_t cap) {
    wchar_t stem[MAX_PATH];
    wchar_t ext[64];
    wchar_t leaf[MAX_PATH];
    int attempt;

    zi_stem_ext(name, stem, MAX_PATH, ext, 64);
    for (attempt = 0; attempt < 999; attempt++) {
        if (attempt == 0) {
            wcscpy_s(leaf, MAX_PATH, name);
        } else if (swprintf_s(leaf, MAX_PATH, L"%s-%d%s", stem, attempt + 1, ext) < 0) {
            return 0;
        }
        if (!zi_join(dest, cap, dir, leaf)) {
            return 0;
        }
        if (!zi_exists_file(dest) && GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES) {
            return 1;
        }
    }
    return 0;
}

static int zi_make_dir(const wchar_t *path) {
    int result;

    if (!path || !path[0]) {
        return 0;
    }
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        return (GetFileAttributesW(path) & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    result = SHCreateDirectoryExW(NULL, path, NULL);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

static int zi_same_dir(const wchar_t *path, const wchar_t *dir) {
    wchar_t parent[MAX_PATH];

    if (!zi_parent(path, parent, MAX_PATH)) {
        return 0;
    }
    return _wcsicmp(parent, dir) == 0;
}

static void zi_note(ZiApp *app, const wchar_t *text) {
    if (!app || !text) {
        return;
    }
    wcscpy_s(app->notice, 192, text);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

int zi_disk_move_marked(ZiApp *app, const wchar_t *folder_name, int subfolder) {
    wchar_t current[MAX_PATH];
    wchar_t dest_dir[MAX_PATH];
    wchar_t dest[MAX_PATH];
    int i;
    int moved = 0;
    int skipped = 0;

    if (!app || zi_folder_bad_name(folder_name) || zi_reserved_leaf(folder_name)) {
        zi_note(app, L"That folder name cannot be used.");
        return 0;
    }
    if (!zi_current_folder(app, current, MAX_PATH) || !app->root[0]) {
        zi_note(app, L"Open a folder on disk first.");
        return 0;
    }
    if (subfolder) {
        if (!zi_join(dest_dir, MAX_PATH, current, folder_name)) {
            return 0;
        }
    } else if (!zi_join(dest_dir, MAX_PATH, app->root, folder_name)) {
        return 0;
    }
    if (!zi_make_dir(dest_dir)) {
        zi_note(app, L"That folder could not be created.");
        return 0;
    }
    for (i = 0; i < app->file_count; i++) {
        const wchar_t *leaf;

        if (!app->file_mark[i] || !app->files[i].path[0] || zi_is_reparse(app->files[i].path)) {
            continue;
        }
        if (zi_same_dir(app->files[i].path, dest_dir)) {
            skipped++;
            continue;
        }
        leaf = wcsrchr(app->files[i].path, L'\\');
        leaf = (leaf && leaf[1]) ? leaf + 1 : app->files[i].name;
        if (!zi_unique_path(dest_dir, leaf, dest, MAX_PATH)) {
            skipped++;
            continue;
        }
        if (MoveFileW(app->files[i].path, dest)) {
            moved++;
        } else {
            skipped++;
        }
    }
    if (moved > 0) {
        swprintf_s(app->notice, 192, L"Moved %d file%s.", moved, moved == 1 ? L"" : L"s");
    } else if (skipped > 0) {
        zi_note(app, L"Those files stayed where they are.");
    } else {
        zi_note(app, L"Select the files to move.");
    }
    if (moved > 0) {
        zi_grid_load(app);
    }
    return moved > 0;
}

static int zi_encode_tiff(HBITMAP bitmap, const wchar_t *path, int lzw) {
    IWICImagingFactory *factory = NULL;
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

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&factory)) ||
        !factory) {
        return 0;
    }
    if (FAILED(factory->lpVtbl->CreateBitmapFromHBITMAP(factory, bitmap, NULL, WICBitmapIgnoreAlpha, &wic)) || !wic) {
        factory->lpVtbl->Release(factory);
        return 0;
    }
    if (FAILED(wic->lpVtbl->GetSize(wic, &w, &h)) || w == 0 || h == 0) {
        goto done;
    }
    if (FAILED(factory->lpVtbl->CreateStream(factory, &stream)) ||
        FAILED(stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE))) {
        goto done;
    }
    if (FAILED(factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatTiff, NULL, &encoder)) ||
        FAILED(encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, &props))) {
        DeleteFileW(path);
        goto done;
    }
    ZeroMemory(&option, sizeof option);
    option.dwType = PROPBAG2_TYPE_DATA;
    VariantInit(&setting);
    if (lzw) {
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
    if (factory) {
        factory->lpVtbl->Release(factory);
    }
    return ok;
}

int zi_write_tiff_file(const wchar_t *path, HBITMAP dib) {
    if (!path || !path[0] || !dib) {
        return 0;
    }
    if (zi_encode_tiff(dib, path, 1)) {
        return 1;
    }
    return zi_encode_tiff(dib, path, 0);
}

static int zi_replace_ext(const wchar_t *path, const wchar_t *ext, wchar_t *dest, size_t cap) {
    wchar_t parent[MAX_PATH];
    wchar_t stem[MAX_PATH];
    wchar_t old_ext[64];
    wchar_t leaf[MAX_PATH];

    if (!zi_parent(path, parent, MAX_PATH)) {
        return 0;
    }
    zi_stem_ext(path, stem, MAX_PATH, old_ext, 64);
    if (swprintf_s(leaf, MAX_PATH, L"%s%s", stem, ext) < 0) {
        return 0;
    }
    return zi_join(dest, cap, parent, leaf);
}

int zi_flatten_path(const wchar_t *photo_path, wchar_t *dest, size_t cap) {
    return zi_replace_ext(photo_path, L".zflat", dest, cap);
}

int zi_flatten_tiff(const wchar_t *photo_path, HBITMAP dib, int duplicate, wchar_t *written, size_t cap) {
    wchar_t dest[MAX_PATH];
    wchar_t parent[MAX_PATH];
    wchar_t stem[MAX_PATH];
    wchar_t ext[64];
    wchar_t leaf[MAX_PATH];
    int attempt;

    if (written && cap > 0) {
        written[0] = L'\0';
    }
    if (!photo_path || !dib) {
        return 0;
    }
    if (!duplicate) {
        if (!zi_flatten_path(photo_path, dest, MAX_PATH)) {
            return 0;
        }
        if (!zi_write_tiff_file(dest, dib)) {
            return 0;
        }
        if (written && cap > 0) {
            wcscpy_s(written, cap, dest);
        }
        return 1;
    }
    if (!zi_parent(photo_path, parent, MAX_PATH)) {
        return 0;
    }
    zi_stem_ext(photo_path, stem, MAX_PATH, ext, 64);
    for (attempt = 1; attempt < 999; attempt++) {
        if (swprintf_s(leaf, MAX_PATH, L"%s-%d.tiff", stem, attempt + 1) < 0) {
            return 0;
        }
        if (!zi_join(dest, MAX_PATH, parent, leaf)) {
            return 0;
        }
        if (GetFileAttributesW(dest) != INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (!zi_write_tiff_file(dest, dib)) {
            return 0;
        }
        if (written && cap > 0) {
            wcscpy_s(written, cap, dest);
        }
        return 1;
    }
    return 0;
}

int zi_revert_flat(const wchar_t *photo_path) {
    wchar_t dest[MAX_PATH];

    if (!zi_flatten_path(photo_path, dest, MAX_PATH)) {
        return 0;
    }
    if (!zi_exists_file(dest)) {
        return 0;
    }
    return DeleteFileW(dest) ? 1 : 0;
}

int zi_organize_dest(const wchar_t *capture, wchar_t *dest, size_t cap) {
    int year;
    int month;
    int day;
    int i;

    if (!capture || !dest || cap < 24) {
        return 0;
    }
    if (wcslen(capture) < 10) {
        return 0;
    }
    for (i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (capture[i] != L':' && capture[i] != L'-') {
                return 0;
            }
        } else if (capture[i] < L'0' || capture[i] > L'9') {
            return 0;
        }
    }
    year = (capture[0] - L'0') * 1000 + (capture[1] - L'0') * 100 + (capture[2] - L'0') * 10 + (capture[3] - L'0');
    month = (capture[5] - L'0') * 10 + (capture[6] - L'0');
    day = (capture[8] - L'0') * 10 + (capture[9] - L'0');
    if (year < 1900 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    return swprintf_s(dest, cap, L"Photos\\%04d\\%04d-%02d-%02d", year, year, month, day) > 0;
}

static int zi_variant_date(PROPVARIANT *value, wchar_t *dest, size_t cap) {
    if (!value || !dest || cap < 12) {
        return 0;
    }
    if (value->vt == VT_LPWSTR && value->pwszVal && value->pwszVal[0]) {
        wcscpy_s(dest, cap, value->pwszVal);
        return 1;
    }
    if (value->vt == VT_BSTR && value->bstrVal && value->bstrVal[0]) {
        wcscpy_s(dest, cap, value->bstrVal);
        return 1;
    }
    return 0;
}

static int zi_file_capture(const wchar_t *path, wchar_t *dest, size_t cap) {
    static const wchar_t *queries[] = {
        L"/app1/ifd/exif/{ushort=36867}",
        L"/app1/ifd/exif/{ushort=36868}",
        L"/app1/ifd/{ushort=306}"
    };
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICMetadataQueryReader *meta = NULL;
    int i;
    int ok = 0;

    dest[0] = L'\0';
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&factory)) ||
        !factory) {
        return 0;
    }
    if (FAILED(factory->lpVtbl->CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &decoder)) ||
        !decoder || FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame)) || !frame ||
        FAILED(frame->lpVtbl->GetMetadataQueryReader(frame, &meta)) || !meta) {
        goto done;
    }
    for (i = 0; i < 3; i++) {
        PROPVARIANT value;

        PropVariantInit(&value);
        if (SUCCEEDED(meta->lpVtbl->GetMetadataByName(meta, queries[i], &value)) && zi_variant_date(&value, dest, cap)) {
            ok = 1;
            PropVariantClear(&value);
            break;
        }
        PropVariantClear(&value);
    }
done:
    if (meta) {
        meta->lpVtbl->Release(meta);
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
    return ok;
}

static unsigned long long zi_file_bytes(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA info;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
        return 0;
    }
    return ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
}

static int zi_dir_has_size(const wchar_t *dir, unsigned long long bytes) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    int hit = 0;

    if (!bytes || !zi_join(pattern, MAX_PATH, dir, L"*")) {
        return 0;
    }
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        unsigned long long size;

        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            continue;
        }
        size = ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        if (size == bytes) {
            hit = 1;
            break;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return hit;
}

static int zi_save_preview_jpeg(HBITMAP bitmap, const wchar_t *path) {
    IWICImagingFactory *factory = NULL;
    IWICBitmap *wic = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapSource *source = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    UINT w = 0;
    UINT h = 0;
    UINT nw;
    UINT nh;
    int ok = 0;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&factory)) ||
        !factory) {
        return 0;
    }
    if (FAILED(factory->lpVtbl->CreateBitmapFromHBITMAP(factory, bitmap, NULL, WICBitmapIgnoreAlpha, &wic)) || !wic ||
        FAILED(wic->lpVtbl->GetSize(wic, &w, &h)) || w == 0 || h == 0) {
        goto done;
    }
    source = (IWICBitmapSource *)wic;
    nw = w;
    nh = h;
    if (w > 250 || h > 250) {
        if (w >= h) {
            nw = 250;
            nh = h * 250 / w;
        } else {
            nh = 250;
            nw = w * 250 / h;
        }
        if (nw < 1) {
            nw = 1;
        }
        if (nh < 1) {
            nh = 1;
        }
        if (SUCCEEDED(factory->lpVtbl->CreateBitmapScaler(factory, &scaler)) && scaler &&
            SUCCEEDED(scaler->lpVtbl->Initialize(scaler, source, nw, nh, WICBitmapInterpolationModeFant))) {
            source = (IWICBitmapSource *)scaler;
        }
    }
    if (FAILED(factory->lpVtbl->CreateStream(factory, &stream)) ||
        FAILED(stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE)) ||
        FAILED(factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatJpeg, NULL, &encoder)) ||
        FAILED(encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, NULL)) ||
        FAILED(frame->lpVtbl->Initialize(frame, NULL)) || FAILED(frame->lpVtbl->SetSize(frame, nw, nh))) {
        DeleteFileW(path);
        goto done;
    }
    frame->lpVtbl->SetPixelFormat(frame, &format);
    if (FAILED(frame->lpVtbl->WriteSource(frame, source, NULL)) || FAILED(frame->lpVtbl->Commit(frame)) ||
        FAILED(encoder->lpVtbl->Commit(encoder))) {
        DeleteFileW(path);
        goto done;
    }
    ok = 1;
done:
    if (frame) {
        frame->lpVtbl->Release(frame);
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
    if (factory) {
        factory->lpVtbl->Release(factory);
    }
    return ok;
}

static HBITMAP zi_load_bitmap(const wchar_t *path, int *out_w, int *out_h);

int zi_disk_organize(ZiApp *app) {
    wchar_t review[MAX_PATH];
    wchar_t dup_dir[MAX_PATH];
    int i;
    int moved = 0;

    if (!app || !app->root[0]) {
        return 0;
    }
    if (!zi_join(review, MAX_PATH, app->root, L"Photos\\_Review") ||
        !zi_join(dup_dir, MAX_PATH, app->root, L"Photos\\_Duplicates")) {
        return 0;
    }
    zi_make_dir(review);
    zi_make_dir(dup_dir);
    for (i = 0; i < app->file_count; i++) {
        wchar_t capture[80];
        wchar_t relative[MAX_PATH];
        wchar_t dest_dir[MAX_PATH];
        wchar_t dest[MAX_PATH];
        const wchar_t *leaf;
        unsigned long long bytes;

        if (app->files[i].video || !app->files[i].path[0] || zi_is_reparse(app->files[i].path)) {
            continue;
        }
        capture[0] = L'\0';
        if (app->files[i].capture[0]) {
            wcscpy_s(capture, 80, app->files[i].capture);
        }
        if (!capture[0]) {
            zi_file_capture(app->files[i].path, capture, 80);
        }
        if (!zi_organize_dest(capture, relative, MAX_PATH)) {
            wcscpy_s(dest_dir, MAX_PATH, review);
        } else if (!zi_join(dest_dir, MAX_PATH, app->root, relative)) {
            continue;
        }
        if (zi_same_dir(app->files[i].path, dest_dir)) {
            continue;
        }
        if (!zi_make_dir(dest_dir)) {
            continue;
        }
        leaf = wcsrchr(app->files[i].path, L'\\');
        leaf = (leaf && leaf[1]) ? leaf + 1 : app->files[i].name;
        bytes = zi_file_bytes(app->files[i].path);
        if (zi_dir_has_size(dest_dir, bytes)) {
            wchar_t preview[MAX_PATH];
            wchar_t stem[MAX_PATH];
            wchar_t ext[64];
            wchar_t jpeg_name[MAX_PATH];
            HBITMAP bitmap;
            int bw = 0;
            int bh = 0;

            zi_stem_ext(leaf, stem, MAX_PATH, ext, 64);
            if (swprintf_s(jpeg_name, MAX_PATH, L"%s.jpg", stem) > 0 &&
                zi_unique_path(dup_dir, jpeg_name, preview, MAX_PATH)) {
                bitmap = zi_load_bitmap(app->files[i].path, &bw, &bh);
                if (bitmap) {
                    zi_save_preview_jpeg(bitmap, preview);
                    DeleteObject(bitmap);
                }
            }
        }
        if (!zi_unique_path(dest_dir, leaf, dest, MAX_PATH)) {
            continue;
        }
        if (MoveFileW(app->files[i].path, dest)) {
            moved++;
        }
    }
    if (moved > 0) {
        swprintf_s(app->notice, 192, L"Organized %d photo%s.", moved, moved == 1 ? L"" : L"s");
        zi_grid_load(app);
    } else {
        zi_note(app, L"No photo on this page needed a new folder.");
    }
    return 1;
}

static int zi_utf8(const wchar_t *wide, char *dest, size_t cap) {
    int n;

    if (!wide || !dest || cap < 2) {
        return 0;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, dest, (int)cap, NULL, NULL);
    return n > 0;
}

static int zi_json_escape(char *dest, size_t cap, const char *src) {
    size_t n = 0;

    if (!dest || !src || cap < 2) {
        return 0;
    }
    while (*src) {
        if (*src == '"' || *src == '\\') {
            if (n + 2 >= cap) {
                return 0;
            }
            dest[n++] = '\\';
        }
        if (n + 1 >= cap) {
            return 0;
        }
        dest[n++] = *src++;
    }
    dest[n] = '\0';
    return 1;
}

int zi_json_rename(char *dest, size_t cap, const char *from, const char *to) {
    char left[1024];
    char right[1024];

    if (!zi_json_escape(left, sizeof left, from ? from : "") || !zi_json_escape(right, sizeof right, to ? to : "")) {
        return 0;
    }
    return sprintf_s(dest, cap, "{\"from\":\"%s\",\"to\":\"%s\"}", left, right) > 0;
}

int zi_json_confirm(char *dest, size_t cap, const char *capture_yyyy_mm_dd) {
    char text[64];

    if (!zi_json_escape(text, sizeof text, capture_yyyy_mm_dd ? capture_yyyy_mm_dd : "")) {
        return 0;
    }
    return sprintf_s(dest, cap, "{\"captureTime\":\"%s\"}", text) > 0;
}

int zi_json_rescan_page(char *dest, size_t cap, const char *prefix, const char **paths, int count) {
    char escaped[1024];
    size_t used;
    int i;

    if (!dest || cap < 32 || !zi_json_escape(escaped, sizeof escaped, prefix ? prefix : "")) {
        return 0;
    }
    used = (size_t)sprintf_s(dest, cap, "{\"prefix\":\"%s\",\"paths\":[", escaped);
    if ((int)used < 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        char item[1024];
        size_t need;

        if (!zi_json_escape(item, sizeof item, paths && paths[i] ? paths[i] : "")) {
            return 0;
        }
        need = strlen(item) + 4;
        if (used + need + 3 >= cap) {
            return 0;
        }
        if (i > 0) {
            dest[used++] = ',';
        }
        dest[used++] = '"';
        memcpy(dest + used, item, strlen(item));
        used += strlen(item);
        dest[used++] = '"';
        dest[used] = '\0';
    }
    if (used + 3 >= cap) {
        return 0;
    }
    dest[used++] = ']';
    dest[used++] = '}';
    dest[used] = '\0';
    return 1;
}

static int zi_catalog_suffix(const wchar_t *path, char *dest, size_t cap) {
    const wchar_t *mark = wcsstr(path, L"\\Photos\\");
    wchar_t relative[MAX_PATH];
    wchar_t *p;

    if (!mark) {
        mark = wcsstr(path, L"/Photos/");
    }
    if (!mark) {
        return 0;
    }
    wcscpy_s(relative, MAX_PATH, mark + 1);
    for (p = relative; *p; p++) {
        if (*p == L'\\') {
            *p = L'/';
        }
    }
    return zi_utf8(relative, dest, cap);
}

static int zi_helper_rename(ZiApp *app, const char *from, const char *to, const wchar_t *route) {
    char body[2048];
    char *reply = NULL;
    DWORD status = 0;
    int ok;
    int answer;

    if (!zi_json_rename(body, sizeof body, from, to)) {
        return 0;
    }
    ok = zi_helper_call(L"POST", route, body, 180000, &status, &reply);
    free(reply);
    reply = NULL;
    if (ok && status >= 200 && status < 300) {
        return 1;
    }
    if (ok || status != 0 || g_modal || !app || !app->hwnd) {
        return 0;
    }
    g_modal = 1;
    answer = MessageBoxW(app->hwnd, L"The helper did not answer. Try the catalog rename again?", L"ZImager",
                         MB_YESNO | MB_ICONWARNING);
    g_modal = 0;
    if (answer != IDYES) {
        return 0;
    }
    ok = zi_helper_call(L"POST", route, body, 180000, &status, &reply);
    free(reply);
    return ok && status >= 200 && status < 300;
}

static int zi_rename_disk(const wchar_t *path, const wchar_t *new_name) {
    wchar_t parent[MAX_PATH];
    wchar_t dest[MAX_PATH];
    wchar_t temp[MAX_PATH];
    wchar_t leaf[MAX_PATH];

    if (zi_folder_bad_name(new_name) || !zi_parent(path, parent, MAX_PATH)) {
        return 0;
    }
    if (!zi_join(dest, MAX_PATH, parent, new_name)) {
        return 0;
    }
    if (_wcsicmp(path, dest) == 0) {
        if (wcscmp(path, dest) == 0) {
            return 1;
        }
        swprintf_s(leaf, MAX_PATH, L"%s.zrenametmp", new_name);
        if (!zi_join(temp, MAX_PATH, parent, leaf)) {
            return 0;
        }
        if (!MoveFileW(path, temp)) {
            return 0;
        }
        if (!MoveFileW(temp, dest)) {
            MoveFileW(temp, path);
            return 0;
        }
        return 1;
    }
    if (GetFileAttributesW(dest) != INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    return MoveFileW(path, dest) ? 1 : 0;
}

int zi_rename_file(ZiApp *app, const wchar_t *path, const wchar_t *new_name, int helper_send) {
    char from[1024];
    char to[1024];
    wchar_t parent[MAX_PATH];
    wchar_t dest[MAX_PATH];
    wchar_t old_leaf[MAX_PATH];
    const wchar_t *leaf;
    int disk_ok;

    if (!path || zi_folder_bad_name(new_name)) {
        zi_note(app, L"That name cannot be used.");
        return 0;
    }
    leaf = wcsrchr(path, L'\\');
    leaf = (leaf && leaf[1]) ? leaf + 1 : path;
    wcscpy_s(old_leaf, MAX_PATH, leaf);
    disk_ok = zi_rename_disk(path, new_name);
    if (!disk_ok) {
        zi_note(app, L"That name is already used.");
        return 0;
    }
    if (!helper_send) {
        zi_note(app, L"Renamed the file.");
        return 1;
    }
    if (!zi_parent(path, parent, MAX_PATH) || !zi_join(dest, MAX_PATH, parent, new_name)) {
        return 0;
    }
    if (!zi_catalog_suffix(path, from, sizeof from)) {
        zi_note(app, L"Renamed the file.");
        return 1;
    }
    if (!zi_catalog_suffix(dest, to, sizeof to)) {
        zi_rename_disk(dest, old_leaf);
        return 0;
    }
    if (!zi_helper_rename(app, from, to, L"/v1/files/rename")) {
        zi_rename_disk(dest, old_leaf);
        zi_note(app, L"The catalog name was put back.");
        return 0;
    }
    zi_note(app, L"Renamed the file.");
    return 1;
}

int zi_rename_folder(ZiApp *app, const wchar_t *path, const wchar_t *new_name, int helper_send) {
    char from[1024];
    char to[1024];
    wchar_t parent[MAX_PATH];
    wchar_t dest[MAX_PATH];
    wchar_t old_leaf[MAX_PATH];
    wchar_t relative[MAX_PATH];
    const wchar_t *leaf;

    if (!path || zi_folder_bad_name(new_name)) {
        zi_note(app, L"That name cannot be used.");
        return 0;
    }
    leaf = wcsrchr(path, L'\\');
    leaf = (leaf && leaf[1]) ? leaf + 1 : path;
    if (_wcsicmp(leaf, L"Photos") == 0 || zi_reserved_leaf(leaf)) {
        zi_note(app, L"That folder keeps its name.");
        return 0;
    }
    wcscpy_s(old_leaf, MAX_PATH, leaf);
    if (!zi_parent(path, parent, MAX_PATH)) {
        return 0;
    }
    swprintf_s(relative, MAX_PATH, L"Photos\\%s", leaf);
    if (zi_folder_reserved(relative) || zi_folder_reserved(leaf)) {
        zi_note(app, L"That folder keeps its name.");
        return 0;
    }
    if (!zi_rename_disk(path, new_name)) {
        zi_note(app, L"That name is already used.");
        return 0;
    }
    if (!zi_join(dest, MAX_PATH, parent, new_name)) {
        return 0;
    }
    if (!helper_send) {
        zi_note(app, L"Renamed the folder.");
        return 1;
    }
    if (!zi_catalog_suffix(path, from, sizeof from) || !zi_catalog_suffix(dest, to, sizeof to)) {
        zi_note(app, L"Renamed the folder.");
        return 1;
    }
    if (!zi_helper_rename(app, from, to, L"/v1/folders/rename")) {
        zi_rename_disk(dest, old_leaf);
        zi_note(app, L"The folder was put back.");
        return 0;
    }
    zi_note(app, L"Renamed the folder.");
    return 1;
}

static int zi_junk_name(const wchar_t *name) {
    if (!name || !name[0]) {
        return 0;
    }
    if (_wcsicmp(name, L"Thumbs.db") == 0 || _wcsicmp(name, L"ehthumbs.db") == 0 ||
        _wcsicmp(name, L"desktop.ini") == 0 || _wcsicmp(name, L".DS_Store") == 0) {
        return 1;
    }
    return name[0] == L'.' && name[1] == L'_';
}

static int zi_clean_walk(const wchar_t *dir, const wchar_t *root, int *removed) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;

    if (zi_is_reparse(dir) && _wcsicmp(dir, root) != 0) {
        return 1;
    }
    if (!zi_join(pattern, MAX_PATH, dir, L"*")) {
        return 0;
    }
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return 1;
    }
    do {
        wchar_t child[MAX_PATH];

        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) {
            continue;
        }
        if (!zi_join(child, MAX_PATH, dir, found.cFileName)) {
            continue;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            zi_clean_walk(child, root, removed);
            if (_wcsicmp(child, root) != 0 && RemoveDirectoryW(child)) {
                (*removed)++;
            }
        } else if (zi_junk_name(found.cFileName) && DeleteFileW(child)) {
            (*removed)++;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return 1;
}

int zi_clean_junk(const wchar_t *root, int allow_server) {
    int removed = 0;

    if (!root || !root[0]) {
        return 0;
    }
    if ((root[0] == L'\\' && root[1] == L'\\') && !allow_server) {
        return 0;
    }
    if (zi_is_reparse(root)) {
        return 0;
    }
    zi_clean_walk(root, root, &removed);
    return 1;
}

static int zi_delete_walk(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;

    if (zi_is_reparse(dir)) {
        return 1;
    }
    if (!zi_join(pattern, MAX_PATH, dir, L"*")) {
        return 0;
    }
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return 1;
    }
    do {
        wchar_t child[MAX_PATH];

        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) {
            continue;
        }
        if (!zi_join(child, MAX_PATH, dir, found.cFileName)) {
            continue;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            zi_delete_walk(child);
            RemoveDirectoryW(child);
        } else {
            DeleteFileW(child);
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return 1;
}

int zi_delete_tree_files(const wchar_t *folder) {
    if (!folder || !folder[0] || zi_is_reparse(folder)) {
        return 0;
    }
    return zi_delete_walk(folder);
}

typedef struct ZiPix {
    int w;
    int h;
    unsigned char *b;
} ZiPix;

static void zi_pix_free(ZiPix *pix) {
    if (pix) {
        free(pix->b);
        pix->b = NULL;
        pix->w = 0;
        pix->h = 0;
    }
}

static int zi_load_pix(const wchar_t *path, ZiPix *pix) {
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0;
    UINT h = 0;
    int y;
    int ok = 0;

    zi_pix_free(pix);
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&factory)) ||
        !factory) {
        return 0;
    }
    if (FAILED(factory->lpVtbl->CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                          &decoder)) ||
        !decoder || FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame)) || !frame ||
        FAILED(factory->lpVtbl->CreateFormatConverter(factory, &conv)) || !conv) {
        goto done;
    }
    if (FAILED(conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->lpVtbl->GetSize(conv, &w, &h)) || w < 1 || h < 1 || w > 20000 || h > 20000) {
        goto done;
    }
    pix->b = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    if (!pix->b || FAILED(conv->lpVtbl->CopyPixels(conv, NULL, w * 4, w * h * 4, pix->b))) {
        goto done;
    }
    pix->w = (int)w;
    pix->h = (int)h;
    for (y = 0; y < pix->h; y++) {
        int x;

        for (x = 0; x < pix->w; x++) {
            pix->b[((size_t)y * (size_t)pix->w + (size_t)x) * 4u + 3u] = 255;
        }
    }
    ok = 1;
done:
    if (!ok) {
        zi_pix_free(pix);
    }
    if (conv) {
        conv->lpVtbl->Release(conv);
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
    return ok;
}

static HBITMAP zi_load_bitmap(const wchar_t *path, int *out_w, int *out_h) {
    ZiPix pix;
    BITMAPINFO info;
    HBITMAP bitmap;
    void *bits = NULL;

    ZeroMemory(&pix, sizeof pix);
    if (out_w) {
        *out_w = 0;
    }
    if (out_h) {
        *out_h = 0;
    }
    if (!zi_load_pix(path, &pix)) {
        return NULL;
    }
    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = pix.w;
    info.bmiHeader.biHeight = -pix.h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bitmap && bits) {
        memcpy(bits, pix.b, (size_t)pix.w * (size_t)pix.h * 4u);
        if (out_w) {
            *out_w = pix.w;
        }
        if (out_h) {
            *out_h = pix.h;
        }
    } else if (bitmap) {
        DeleteObject(bitmap);
        bitmap = NULL;
    }
    zi_pix_free(&pix);
    return bitmap;
}

static int zi_scale_pix(const ZiPix *src, ZiPix *dst, int dw, int dh) {
    int y;

    zi_pix_free(dst);
    if (!src || !src->b || dw < 1 || dh < 1) {
        return 0;
    }
    dst->b = (unsigned char *)malloc((size_t)dw * (size_t)dh * 4u);
    if (!dst->b) {
        return 0;
    }
    dst->w = dw;
    dst->h = dh;
    for (y = 0; y < dh; y++) {
        int x;
        int sy = y * src->h / dh;

        if (sy >= src->h) {
            sy = src->h - 1;
        }
        for (x = 0; x < dw; x++) {
            int sx = x * src->w / dw;
            unsigned char *out;
            const unsigned char *in;

            if (sx >= src->w) {
                sx = src->w - 1;
            }
            out = dst->b + ((size_t)y * (size_t)dw + (size_t)x) * 4u;
            in = src->b + ((size_t)sy * (size_t)src->w + (size_t)sx) * 4u;
            memcpy(out, in, 4);
        }
    }
    return 1;
}

static void zi_fit_budget(ZiPix *left, ZiPix *right, int vertical) {
    double along;
    double cross;
    double pixels;
    double scale;
    ZiPix scaled;

    if (!left->b || !right->b) {
        return;
    }
    if (vertical) {
        along = (double)left->h + (double)right->h;
        cross = left->w > right->w ? (double)left->w : (double)right->w;
    } else {
        along = (double)left->w + (double)right->w;
        cross = left->h > right->h ? (double)left->h : (double)right->h;
    }
    pixels = along * cross;
    if (pixels <= 40000000.0) {
        return;
    }
    scale = sqrt(40000000.0 / pixels);
    if (scale < 0.05) {
        scale = 0.05;
    }
    ZeroMemory(&scaled, sizeof scaled);
    if (zi_scale_pix(left, &scaled, (int)(left->w * scale), (int)(left->h * scale))) {
        zi_pix_free(left);
        *left = scaled;
    }
    ZeroMemory(&scaled, sizeof scaled);
    if (zi_scale_pix(right, &scaled, (int)(right->w * scale), (int)(right->h * scale))) {
        zi_pix_free(right);
        *right = scaled;
    }
    g_merge_scaled = 1;
}

static unsigned char *zi_at(ZiPix *pix, int x, int y) {
    if (!pix || !pix->b || x < 0 || y < 0 || x >= pix->w || y >= pix->h) {
        return NULL;
    }
    return pix->b + ((size_t)y * (size_t)pix->w + (size_t)x) * 4u;
}

static void zi_blit(ZiPix *canvas, const ZiPix *src, int ox, int oy) {
    int y;

    for (y = 0; y < src->h; y++) {
        int x;

        if (oy + y < 0 || oy + y >= canvas->h) {
            continue;
        }
        for (x = 0; x < src->w; x++) {
            unsigned char *out;
            const unsigned char *in;

            if (ox + x < 0 || ox + x >= canvas->w) {
                continue;
            }
            out = zi_at(canvas, ox + x, oy + y);
            in = src->b + ((size_t)y * (size_t)src->w + (size_t)x) * 4u;
            if (out && in[3] > 16) {
                memcpy(out, in, 4);
                out[3] = 255;
            }
        }
    }
}

static int zi_compose(const ZiPix *first, const ZiPix *second, int vertical, int shift_x, int shift_y, ZiPix *out) {
    int rel_x;
    int rel_y;
    int origin_x;
    int origin_y;
    int first_x;
    int first_y;
    int second_x;
    int second_y;
    int canvas_w;
    int canvas_h;
    int shared_left;
    int shared_top;
    int shared_w;
    int shared_h;
    int reach;
    int y;

    if (vertical) {
        rel_x = shift_x;
        rel_y = first->h + shift_y;
    } else {
        rel_x = first->w + shift_x;
        rel_y = shift_y;
    }
    origin_x = rel_x < 0 ? rel_x : 0;
    origin_y = rel_y < 0 ? rel_y : 0;
    first_x = -origin_x;
    first_y = -origin_y;
    second_x = rel_x - origin_x;
    second_y = rel_y - origin_y;
    canvas_w = first_x + first->w;
    if (second_x + second->w > canvas_w) {
        canvas_w = second_x + second->w;
    }
    canvas_h = first_y + first->h;
    if (second_y + second->h > canvas_h) {
        canvas_h = second_y + second->h;
    }
    if (canvas_w < 1 || canvas_h < 1 || canvas_w > 30000 || canvas_h > 30000) {
        return 0;
    }
    zi_pix_free(out);
    out->b = (unsigned char *)calloc((size_t)canvas_w * (size_t)canvas_h, 4u);
    if (!out->b) {
        return 0;
    }
    out->w = canvas_w;
    out->h = canvas_h;
    zi_blit(out, first, first_x, first_y);
    shared_left = first_x > second_x ? first_x : second_x;
    shared_top = first_y > second_y ? first_y : second_y;
    shared_w = (first_x + first->w < second_x + second->w ? first_x + first->w : second_x + second->w) - shared_left;
    shared_h = (first_y + first->h < second_y + second->h ? first_y + first->h : second_y + second->h) - shared_top;
    if (shared_w > 0 && shared_h > 0) {
        unsigned char *kept = (unsigned char *)malloc((size_t)shared_w * (size_t)shared_h * 4u);
        int row;

        if (kept) {
            for (row = 0; row < shared_h; row++) {
                memcpy(kept + (size_t)row * (size_t)shared_w * 4u, zi_at(out, shared_left, shared_top + row),
                       (size_t)shared_w * 4u);
            }
            zi_blit(out, second, second_x, second_y);
            for (row = 0; row < shared_h; row++) {
                int col;

                for (col = 0; col < shared_w; col++) {
                    unsigned char *dst = zi_at(out, shared_left + col, shared_top + row);
                    const unsigned char *a = kept + ((size_t)row * (size_t)shared_w + (size_t)col) * 4u;
                    float t;

                    if (!dst) {
                        continue;
                    }
                    if (vertical) {
                        t = shared_h == 1 ? 1.0f : (float)row / (float)(shared_h - 1);
                    } else {
                        t = shared_w == 1 ? 1.0f : (float)col / (float)(shared_w - 1);
                    }
                    dst[0] = (unsigned char)((float)a[0] * (1.0f - t) + (float)dst[0] * t + 0.5f);
                    dst[1] = (unsigned char)((float)a[1] * (1.0f - t) + (float)dst[1] * t + 0.5f);
                    dst[2] = (unsigned char)((float)a[2] * (1.0f - t) + (float)dst[2] * t + 0.5f);
                    dst[3] = 255;
                }
            }
            free(kept);
        } else {
            zi_blit(out, second, second_x, second_y);
        }
    } else {
        zi_blit(out, second, second_x, second_y);
    }
    reach = vertical ? (shift_x < 0 ? -shift_x : shift_x) : (shift_y < 0 ? -shift_y : shift_y);
    reach += 2;
    for (y = 0; y < canvas_h; y++) {
        int x;

        for (x = 0; x < canvas_w; x++) {
            unsigned char *pixel = zi_at(out, x, y);
            int filled = 0;
            int distance;

            if (!pixel || pixel[3] > 16) {
                continue;
            }
            for (distance = 1; distance <= reach && !filled; distance++) {
                int nx[2];
                int ny[2];
                int n;

                if (vertical) {
                    nx[0] = x - distance;
                    ny[0] = y;
                    nx[1] = x + distance;
                    ny[1] = y;
                } else {
                    nx[0] = x;
                    ny[0] = y - distance;
                    nx[1] = x;
                    ny[1] = y + distance;
                }
                for (n = 0; n < 2; n++) {
                    unsigned char *from = zi_at(out, nx[n], ny[n]);

                    if (from && from[3] > 16) {
                        pixel[0] = from[0];
                        pixel[1] = from[1];
                        pixel[2] = from[2];
                        pixel[3] = 255;
                        filled = 1;
                        break;
                    }
                }
            }
            if (!filled) {
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[3] = 255;
            }
        }
    }
    return 1;
}

static HBITMAP zi_pix_bitmap(const ZiPix *pix) {
    BITMAPINFO info;
    HBITMAP bitmap;
    void *bits = NULL;

    if (!pix || !pix->b) {
        return NULL;
    }
    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = pix->w;
    info.bmiHeader.biHeight = -pix->h;
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
    memcpy(bits, pix->b, (size_t)pix->w * (size_t)pix->h * 4u);
    return bitmap;
}

int zi_merge_write(const wchar_t *directory, const wchar_t *left_path, const wchar_t *right_path, int vertical,
                   int shift_x, int shift_y, wchar_t *written, size_t written_cap) {
    ZiPix left;
    ZiPix right;
    ZiPix canvas;
    HBITMAP bitmap;
    wchar_t name[MAX_PATH];
    wchar_t dest[MAX_PATH];
    int attempt;
    int ok = 0;

    if (written && written_cap > 0) {
        written[0] = L'\0';
    }
    ZeroMemory(&left, sizeof left);
    ZeroMemory(&right, sizeof right);
    ZeroMemory(&canvas, sizeof canvas);
    g_merge_scaled = 0;
    if (!directory || !left_path || !right_path || !zi_load_pix(left_path, &left) || !zi_load_pix(right_path, &right)) {
        zi_pix_free(&left);
        zi_pix_free(&right);
        return 0;
    }
    zi_fit_budget(&left, &right, vertical);
    if (!zi_compose(&left, &right, vertical, shift_x, shift_y, &canvas)) {
        zi_pix_free(&left);
        zi_pix_free(&right);
        return 0;
    }
    bitmap = zi_pix_bitmap(&canvas);
    zi_pix_free(&left);
    zi_pix_free(&right);
    zi_pix_free(&canvas);
    if (!bitmap) {
        return 0;
    }
    for (attempt = 0; attempt < 999; attempt++) {
        if (!zi_merge_filename(name, MAX_PATH, left_path, right_path, attempt)) {
            break;
        }
        if (!zi_join(dest, MAX_PATH, directory, name)) {
            break;
        }
        if (GetFileAttributesW(dest) != INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        ok = zi_write_tiff_file(dest, bitmap);
        if (ok && written && written_cap > 0) {
            wcscpy_s(written, written_cap, dest);
        }
        break;
    }
    DeleteObject(bitmap);
    return ok;
}

static ZiPix g_merge_left;
static ZiPix g_merge_right;
static wchar_t g_merge_dir[MAX_PATH];
static wchar_t g_merge_left_path[MAX_PATH];
static wchar_t g_merge_right_path[MAX_PATH];
static int g_merge_full;
static int g_merge_pan_x;
static int g_merge_pan_y;
static double g_merge_zoom;
static int g_merge_drag;
static int g_merge_drag_x;
static int g_merge_drag_y;
static int g_merge_drag_pan_x;
static int g_merge_drag_pan_y;

static void zi_merge_release(void) {
    zi_pix_free(&g_merge_left);
    zi_pix_free(&g_merge_right);
    g_merge_dir[0] = L'\0';
    g_merge_left_path[0] = L'\0';
    g_merge_right_path[0] = L'\0';
    g_merge_full = 0;
    g_merge_pan_x = 0;
    g_merge_pan_y = 0;
    g_merge_zoom = 0;
    g_merge_drag = 0;
}

static int zi_preview_edge(const ZiPix *src, ZiPix *dst) {
    int edge = src->w > src->h ? src->w : src->h;
    int dw;
    int dh;

    if (edge <= 1400) {
        return zi_scale_pix(src, dst, src->w, src->h);
    }
    if (src->w >= src->h) {
        dw = 1400;
        dh = src->h * 1400 / src->w;
    } else {
        dh = 1400;
        dw = src->w * 1400 / src->h;
    }
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    return zi_scale_pix(src, dst, dw, dh);
}

int zi_merge_arm(ZiApp *app, int vertical) {
    int first = -1;
    int second = -1;
    int i;
    ZiPix full_left;
    ZiPix full_right;
    wchar_t dir[MAX_PATH];

    if (!app || !zi_current_folder(app, dir, MAX_PATH)) {
        return 0;
    }
    if (app->file_anchor >= 0 && app->file_anchor < app->file_count && app->file_mark[app->file_anchor] &&
        !app->files[app->file_anchor].video && app->file_selected >= 0 && app->file_selected < app->file_count &&
        app->file_mark[app->file_selected] && !app->files[app->file_selected].video &&
        app->file_anchor != app->file_selected) {
        first = app->file_anchor;
        second = app->file_selected;
    } else {
        for (i = 0; i < app->file_count; i++) {
            if (!app->file_mark[i] || app->files[i].video) {
                continue;
            }
            if (first < 0) {
                first = i;
            } else if (second < 0) {
                second = i;
            } else {
                first = -1;
                break;
            }
        }
    }
    if (first < 0 || second < 0) {
        zi_note(app, L"Select two stills from this folder.");
        return 0;
    }
    ZeroMemory(&full_left, sizeof full_left);
    ZeroMemory(&full_right, sizeof full_right);
    if (!zi_load_pix(app->files[first].path, &full_left) || !zi_load_pix(app->files[second].path, &full_right)) {
        zi_pix_free(&full_left);
        zi_pix_free(&full_right);
        zi_note(app, L"Those photographs could not be read.");
        return 0;
    }
    zi_merge_release();
    if (!zi_preview_edge(&full_left, &g_merge_left) || !zi_preview_edge(&full_right, &g_merge_right)) {
        zi_pix_free(&full_left);
        zi_pix_free(&full_right);
        zi_merge_release();
        return 0;
    }
    zi_pix_free(&full_left);
    zi_pix_free(&full_right);
    wcscpy_s(g_merge_dir, MAX_PATH, dir);
    wcscpy_s(g_merge_left_path, MAX_PATH, app->files[first].path);
    wcscpy_s(g_merge_right_path, MAX_PATH, app->files[second].path);
    app->merge_open = 1;
    app->merge_vertical = vertical ? 1 : 0;
    app->merge_shift_x = 0;
    app->merge_shift_y = 0;
    app->merge_a = first;
    app->merge_b = second;
    g_merge_zoom = 0;
    zi_note(app, L"Arrow keys move the second photo. Click zooms in.");
    return 1;
}

void zi_merge_cancel(ZiApp *app) {
    zi_merge_release();
    if (app) {
        app->merge_open = 0;
        app->notice[0] = L'\0';
        if (app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
    }
}

static void zi_merge_place(const ZiApp *app, int *ox, int *oy, double *scale, int canvas_w, int canvas_h) {
    int rel_x;
    int rel_y;
    int origin_x;
    int origin_y;
    int picture_w;
    int picture_h;
    double fit;

    if (app->merge_vertical) {
        rel_x = app->merge_shift_x;
        rel_y = g_merge_left.h + app->merge_shift_y;
    } else {
        rel_x = g_merge_left.w + app->merge_shift_x;
        rel_y = app->merge_shift_y;
    }
    origin_x = rel_x < 0 ? rel_x : 0;
    origin_y = rel_y < 0 ? rel_y : 0;
    picture_w = -origin_x + g_merge_left.w;
    if (-origin_x + rel_x + g_merge_right.w > picture_w) {
        picture_w = -origin_x + rel_x + g_merge_right.w;
    }
    picture_h = -origin_y + g_merge_left.h;
    if (-origin_y + rel_y + g_merge_right.h > picture_h) {
        picture_h = -origin_y + rel_y + g_merge_right.h;
    }
    if (picture_w < 1) {
        picture_w = 1;
    }
    if (picture_h < 1) {
        picture_h = 1;
    }
    fit = (double)canvas_w / (double)picture_w;
    if ((double)canvas_h / (double)picture_h < fit) {
        fit = (double)canvas_h / (double)picture_h;
    }
    if (g_merge_full) {
        *scale = g_merge_zoom > 0 ? g_merge_zoom : 1.0;
    } else if (g_merge_zoom > fit) {
        *scale = g_merge_zoom;
    } else {
        *scale = fit;
    }
    *ox = (canvas_w - (int)(picture_w * *scale)) / 2 + g_merge_pan_x;
    *oy = (canvas_h - (int)(picture_h * *scale)) / 2 + g_merge_pan_y;
}

static void zi_merge_note(ZiApp *app, HDC hdc, const RECT *area, const wchar_t *text) {
    HFONT old = (HFONT)SelectObject(hdc, app->font);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, text, -1, (RECT *)area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);
}

int zi_merge_ready(void) {
    return g_merge_left.b != NULL && g_merge_right.b != NULL;
}

void zi_merge_paint(ZiApp *app, HDC hdc) {
    ZiPix canvas;
    HBITMAP bitmap;
    RECT area;
    RECT strip;
    int bar;
    int w;
    int h;
    int ox;
    int oy;
    double scale;
    HDC mem;
    HGDIOBJ old;

    if (!app || !app->merge_open || !hdc) {
        return;
    }
    area = app->regions.canvas;
    bar = zi_px(app, 44);
    strip = area;
    strip.bottom = strip.top + bar;
    if (strip.bottom > area.bottom) {
        strip.bottom = area.bottom;
    }
    FillRect(hdc, &strip, app->panel);
    area.top = strip.bottom;
    if (area.bottom <= area.top) {
        return;
    }
    FillRect(hdc, &area, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (!g_merge_left.b || !g_merge_right.b) {
        zi_merge_note(app, hdc, &area, L"Reading the two photos.");
        return;
    }
    w = area.right - area.left;
    h = area.bottom - area.top;
    if (w < 8 || h < 8) {
        return;
    }
    ZeroMemory(&canvas, sizeof canvas);
    if (!zi_compose(&g_merge_left, &g_merge_right, app->merge_vertical, app->merge_shift_x, app->merge_shift_y, &canvas)) {
        zi_merge_note(app, hdc, &area, L"The photos could not be read.");
        return;
    }
    bitmap = zi_pix_bitmap(&canvas);
    {
        int picture_w = canvas.w;
        int picture_h = canvas.h;

        zi_pix_free(&canvas);
        if (!bitmap || picture_w < 1 || picture_h < 1) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            return;
        }
        zi_merge_place(app, &ox, &oy, &scale, w, h);
        mem = CreateCompatibleDC(hdc);
        old = SelectObject(mem, bitmap);
        StretchBlt(hdc, area.left + ox, area.top + oy, (int)(picture_w * scale), (int)(picture_h * scale), mem, 0, 0,
                   picture_w, picture_h, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bitmap);
    }
}

int zi_merge_key(ZiApp *app, int vk) {
    if (!app || !app->merge_open) {
        return 0;
    }
    if (vk == VK_LEFT) {
        app->merge_shift_x -= 1;
    } else if (vk == VK_RIGHT) {
        app->merge_shift_x += 1;
    } else if (vk == VK_UP) {
        app->merge_shift_y -= 1;
    } else if (vk == VK_DOWN) {
        app->merge_shift_y += 1;
    } else {
        return 0;
    }
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
    return 1;
}

int zi_merge_dragging(void) {
    return g_merge_drag;
}

int zi_merge_pointer(ZiApp *app, int x, int y, int down, int alt, int space) {
    POINT pt;

    if (!app || !app->merge_open) {
        return 0;
    }
    pt.x = x;
    pt.y = y;
    if (!PtInRect(&app->regions.canvas, pt)) {
        return 0;
    }
    /* The Fit and 100% strip belongs to those buttons. */
    if (y < app->regions.canvas.top + zi_px(app, 44) && !g_merge_drag) {
        return down ? 1 : 0;
    }
    if (down && space) {
        g_merge_drag = 1;
        g_merge_drag_x = x;
        g_merge_drag_y = y;
        g_merge_drag_pan_x = g_merge_pan_x;
        g_merge_drag_pan_y = g_merge_pan_y;
        return 1;
    }
    if (!down && g_merge_drag) {
        g_merge_pan_x = g_merge_drag_pan_x + (x - g_merge_drag_x);
        g_merge_pan_y = g_merge_drag_pan_y + (y - g_merge_drag_y);
        g_merge_drag = 0;
        if (app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return 1;
    }
    if (g_merge_drag) {
        g_merge_pan_x = g_merge_drag_pan_x + (x - g_merge_drag_x);
        g_merge_pan_y = g_merge_drag_pan_y + (y - g_merge_drag_y);
        if (app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return 1;
    }
    if (!down) {
        return 0;
    }
    if (alt) {
        if (g_merge_zoom > 0) {
            g_merge_zoom /= 1.41421356237;
        }
        if (g_merge_zoom < 0.05) {
            g_merge_full = 0;
            g_merge_zoom = 0;
            g_merge_pan_x = 0;
            g_merge_pan_y = 0;
        }
    } else {
        int ox;
        int oy;
        double scale;
        int w = app->regions.canvas.right - app->regions.canvas.left;
        int h = app->regions.canvas.bottom - app->regions.canvas.top;

        zi_merge_place(app, &ox, &oy, &scale, w, h);
        g_merge_zoom = scale * 1.41421356237;
        if (g_merge_zoom > 8) {
            g_merge_zoom = 8;
        }
        g_merge_full = 1;
    }
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
    return 1;
}

int zi_merge_accept(ZiApp *app) {
    wchar_t written[MAX_PATH];
    int shift_x;
    int shift_y;
    int vertical;

    if (!app || !app->merge_open || !g_merge_left_path[0] || !g_merge_right_path[0]) {
        return 0;
    }
    vertical = app->merge_vertical;
    shift_x = app->merge_shift_x;
    shift_y = app->merge_shift_y;
    if (g_merge_left.w > 0 && g_merge_right.w > 0) {
        ZiPix probe_l;
        ZiPix probe_r;

        ZeroMemory(&probe_l, sizeof probe_l);
        ZeroMemory(&probe_r, sizeof probe_r);
        if (zi_load_pix(g_merge_left_path, &probe_l) && zi_load_pix(g_merge_right_path, &probe_r) && g_merge_left.w > 0) {
            shift_x = (int)((double)app->merge_shift_x * (double)probe_l.w / (double)g_merge_left.w);
            shift_y = (int)((double)app->merge_shift_y * (double)probe_l.h / (double)g_merge_left.h);
        }
        zi_pix_free(&probe_l);
        zi_pix_free(&probe_r);
    }
    if (!zi_merge_write(g_merge_dir, g_merge_left_path, g_merge_right_path, vertical, shift_x, shift_y, written,
                        MAX_PATH)) {
        zi_note(app, L"The merged photograph could not be written.");
        return 0;
    }
    zi_merge_release();
    app->merge_open = 0;
    if (g_merge_scaled) {
        zi_note(app, L"Wrote the merged TIFF. It was scaled to stay under 40 million pixels.");
    } else {
        zi_note(app, L"Wrote the merged TIFF.");
    }
    zi_grid_load(app);
    return 1;
}

void zi_merge_fit(ZiApp *app, int full) {
    if (!app || !app->merge_open) {
        return;
    }
    if (!full) {
        g_merge_full = 0;
        g_merge_zoom = 0;
        g_merge_pan_x = 0;
        g_merge_pan_y = 0;
    } else if (g_merge_full) {
        g_merge_pan_x = 0;
        g_merge_pan_y = 0;
        g_merge_zoom = 1;
    } else {
        g_merge_full = 1;
        g_merge_zoom = 1;
        g_merge_pan_x = 0;
        g_merge_pan_y = 0;
    }
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}
