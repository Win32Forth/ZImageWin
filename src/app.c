/* ZImager on Windows. The Mac app in ../ZImage/ZImager is the reference. */
#include <initguid.h>
#include "chrome.h"
#include "grid.h"
#include "editor.h"
#include "helper.h"
#include "library.h"
#include "tone.h"

#include <winnetwk.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static ZiApp g_app;

/* The tree that is not on screen. Disk and Server each keep their own folders. */
static ZiFolder g_kept_folders[ZI_MAX_FOLDERS];
static int g_kept_count;
static int g_kept_selected = -1;
static int g_kept_scroll;
static int g_kept_root_open;
static int g_kept_library;
static int g_kept_is_server;
static int g_tree_kept;

static HFONT zi_make_font(int points, int weight, UINT dpi) {
    LOGFONTW lf;

    ZeroMemory(&lf, sizeof lf);
    lf.lfHeight = -MulDiv(points, (int)dpi, 72);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

static void zi_rebuild_fonts(ZiApp *app) {
    if (app->font) {
        DeleteObject(app->font);
    }
    if (app->font_header) {
        DeleteObject(app->font_header);
    }
    if (app->font_hint) {
        DeleteObject(app->font_hint);
    }
    app->font = zi_make_font(13, FW_NORMAL, app->dpi);
    app->font_header = zi_make_font(15, FW_SEMIBOLD, app->dpi);
    app->font_hint = zi_make_font(11, FW_NORMAL, app->dpi);
}

static void zi_make_brushes(ZiApp *app) {
    app->panel = CreateSolidBrush(ZI_PANEL);
    app->canvas_brush = CreateSolidBrush(ZI_CANVAS);
    app->list_brush = CreateSolidBrush(ZI_LIST);
    app->list_on = CreateSolidBrush(ZI_LIST_ON);
    app->button_brush = CreateSolidBrush(ZI_BUTTON);
    app->select_brush = CreateSolidBrush(ZI_SELECT);
    app->white = CreateSolidBrush(RGB(255, 255, 255));
    app->hair = CreatePen(PS_SOLID, 1, ZI_HAIRLINE);
    app->muted_pen = CreatePen(PS_SOLID, 1, ZI_MUTED);
    app->tool_pen = CreatePen(PS_SOLID, 2, ZI_TOOL);
    app->ink_pen = CreatePen(PS_SOLID, 1, ZI_INK);
}

static void zi_free_gdi(ZiApp *app) {
    if (app->font) DeleteObject(app->font);
    if (app->font_header) DeleteObject(app->font_header);
    if (app->font_hint) DeleteObject(app->font_hint);
    if (app->panel) DeleteObject(app->panel);
    if (app->canvas_brush) DeleteObject(app->canvas_brush);
    if (app->list_brush) DeleteObject(app->list_brush);
    if (app->list_on) DeleteObject(app->list_on);
    if (app->button_brush) DeleteObject(app->button_brush);
    if (app->select_brush) DeleteObject(app->select_brush);
    if (app->white) DeleteObject(app->white);
    if (app->hair) DeleteObject(app->hair);
    if (app->muted_pen) DeleteObject(app->muted_pen);
    if (app->tool_pen) DeleteObject(app->tool_pen);
    if (app->ink_pen) DeleteObject(app->ink_pen);
    app->font = NULL;
    app->font_header = NULL;
    app->font_hint = NULL;
}

void zi_layout_now(ZiApp *app) {
    RECT rc;
    HDC hdc;

    if (!app->hwnd) {
        return;
    }
    hdc = GetDC(app->hwnd);
    GetClientRect(app->hwnd, &rc);
    zi_chrome_layout(app, hdc, &rc);
    ReleaseDC(app->hwnd, hdc);
}

static void zi_redraw(ZiApp *app) {
    zi_grid_load(app);
    zi_editor_sync_details(app);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

static void zi_set_title(ZiApp *app) {
    const wchar_t *base;
    wchar_t title[MAX_PATH];

    if (app->opened[0]) {
        base = wcsrchr(app->opened, L'\\');
        base = (base && base[1]) ? base + 1 : app->opened;
        wcscpy_s(title, MAX_PATH, base);
    } else {
        wcscpy_s(title, MAX_PATH, L"ZImager");
    }
    SetWindowTextW(app->hwnd, title);
}

static void zi_notice(ZiApp *app, const wchar_t *text) {
    wcscpy_s(app->notice, sizeof app->notice / sizeof app->notice[0], text);
    zi_redraw(app);
}

static int zi_name_less(const void *a, const void *b) {
    return _wcsicmp(((const ZiFolder *)a)->name, ((const ZiFolder *)b)->name);
}

static void zi_clear_folders(ZiApp *app) {
    app->folder_count = 0;
    app->folder_selected = -1;
    app->folder_scroll = 0;
}

static int zi_is_dir(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);

    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* Photos is the tree root on the Mac. A picked Photos folder is used as itself. */
static void zi_photos_dir(const wchar_t *library, wchar_t *dest, size_t cap) {
    size_t n;
    wchar_t nested[MAX_PATH];

    dest[0] = L'\0';
    if (!library || !library[0] || cap == 0) {
        return;
    }
    n = wcslen(library);
    while (n > 0 && (library[n - 1] == L'\\' || library[n - 1] == L'/')) {
        n--;
    }
    if (n >= 6 && _wcsnicmp(library + (n - 6), L"Photos", 6) == 0 &&
        (n == 6 || library[n - 7] == L'\\' || library[n - 7] == L'/')) {
        if (n >= cap) {
            n = cap - 1;
        }
        wcsncpy_s(dest, cap, library, n);
        return;
    }
    if (swprintf_s(nested, MAX_PATH, L"%.*s\\Photos", (int)n, library) > 0 && zi_is_dir(nested)) {
        wcscpy_s(dest, cap, nested);
        return;
    }
    wcsncpy_s(dest, cap, library, n >= cap ? cap - 1 : n);
}

static int zi_append_children(ZiApp *app, const wchar_t *directory, int parent, int depth) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    int begin;
    int added = 0;
    size_t length;

    length = wcslen(directory);
    if (length == 0 || length + 3 >= MAX_PATH) {
        return -1;
    }
    if (directory[length - 1] == L'\\' || directory[length - 1] == L'/') {
        swprintf_s(pattern, MAX_PATH, L"%s*", directory);
    } else {
        swprintf_s(pattern, MAX_PATH, L"%s\\*", directory);
    }
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return -1;
    }
    begin = app->folder_count;
    do {
        ZiFolder *row;

        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        if ((found.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            continue;
        }
        if (found.cFileName[0] == L'.') {
            continue;
        }
        if (app->folder_count >= ZI_MAX_FOLDERS) {
            wcscpy_s(app->notice, 192, L"The folder list shows the first 400 names.");
            break;
        }
        row = &app->folders[app->folder_count];
        ZeroMemory(row, sizeof *row);
        wcscpy_s(row->name, ZI_NAME, found.cFileName);
        row->depth = depth;
        row->parent = parent;
        app->folder_count++;
        added++;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (added > 1) {
        qsort(app->folders + begin, (size_t)added, sizeof app->folders[0], zi_name_less);
    }
    return added;
}

static int zi_node_dir(const ZiApp *app, int index, wchar_t *dest, size_t cap) {
    int chain[40];
    int count = 0;
    int cursor = index;

    if (app->source_server) {
        if (!app->library_scan[0]) {
            return 0;
        }
    } else if (!app->root[0]) {
        return 0;
    }
    while (cursor >= 0 && count < 40) {
        chain[count++] = cursor;
        cursor = app->folders[cursor].parent;
    }
    wcscpy_s(dest, cap, app->source_server ? app->library_scan : app->root);
    while (count > 0) {
        const wchar_t *name = app->folders[chain[--count]].name;

        if (wcslen(dest) + wcslen(name) + 2 >= cap) {
            return 0;
        }
        wcscat_s(dest, cap, L"\\");
        wcscat_s(dest, cap, name);
    }
    return 1;
}

int zi_current_folder(const ZiApp *app, wchar_t *dest, size_t cap) {
    const wchar_t *base = app->source_server ? app->library_scan : app->root;

    if (!base[0]) {
        return 0;
    }
    if (app->folder_selected < 0 || app->folder_selected >= app->folder_count) {
        wcscpy_s(dest, cap, base);
        return 1;
    }
    return zi_node_dir(app, app->folder_selected, dest, cap);
}

static void zi_list_folders(ZiApp *app) {
    zi_clear_folders(app);
    app->folders_are_library = 0;
    app->tree_root_open = 1;
    if (!app->root[0]) {
        app->tree_root_open = 0;
        return;
    }
    if (zi_append_children(app, app->root, -1, 0) < 0) {
        wcscpy_s(app->notice, 192, L"That folder could not be read.");
    }
}

static void zi_list_library(ZiApp *app) {
    wchar_t dir[MAX_PATH];

    zi_photos_dir(app->library_root, dir, MAX_PATH);
    wcscpy_s(app->library_scan, MAX_PATH, dir);
    zi_clear_folders(app);
    app->tree_root_open = 1;
    app->folders_are_library = 1;
    if (!dir[0] || zi_append_children(app, dir, -1, 0) < 0) {
        wcscpy_s(app->notice, 192, L"That folder could not be read.");
    }
}

static void zi_ensure_library_tree(ZiApp *app) {
    wchar_t dir[MAX_PATH];

    if (!app->library_root[0]) {
        return;
    }
    zi_photos_dir(app->library_root, dir, MAX_PATH);
    if (app->folders_are_library && wcscmp(app->library_scan, dir) == 0) {
        return;
    }
    zi_list_library(app);
}

static void zi_expand_folder(ZiApp *app, int index) {
    wchar_t dir[MAX_PATH];
    ZiFolder *row;

    if (index < 0 || index >= app->folder_count) {
        return;
    }
    row = &app->folders[index];
    row->expanded = 1;
    if (row->loaded) {
        return;
    }
    row->loaded = 1;
    if (!zi_node_dir(app, index, dir, MAX_PATH) || zi_append_children(app, dir, index, row->depth + 1) < 0) {
        wcscpy_s(app->notice, 192, L"That folder could not be read.");
    }
}

static int zi_folder_file(wchar_t *path, size_t cap) {
    wchar_t local[MAX_PATH];
    DWORD n;

    if (!path || cap < 16) {
        return 0;
    }
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return 0;
    }
    if (swprintf_s(path, cap, L"%ls\\ZImageWin", local) < 0) {
        return 0;
    }
    CreateDirectoryW(path, NULL);
    if (swprintf_s(path, cap, L"%ls\\ZImageWin\\folder.ini", local) < 0) {
        return 0;
    }
    return 1;
}

static void zi_trim_dir(wchar_t *path) {
    size_t n;

    if (!path) {
        return;
    }
    n = wcslen(path);
    while (n > 1 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) {
        if (n == 3 && path[1] == L':') {
            break;
        }
        path[--n] = L'\0';
    }
}

static int zi_chain_dir(const wchar_t *root, const ZiFolder *folders, int count, int index,
                        wchar_t *dest, size_t cap) {
    int chain[40];
    int links = 0;
    int cursor = index;

    if (!root || !root[0] || !dest || cap == 0) {
        return 0;
    }
    if (!folders || index < 0 || index >= count) {
        wcscpy_s(dest, cap, root);
        return 1;
    }
    while (cursor >= 0 && cursor < count && links < 40) {
        chain[links++] = cursor;
        cursor = folders[cursor].parent;
    }
    wcscpy_s(dest, cap, root);
    while (links > 0) {
        const wchar_t *name = folders[chain[--links]].name;

        if (wcslen(dest) + wcslen(name) + 2 >= cap) {
            wcscpy_s(dest, cap, root);
            return 1;
        }
        wcscat_s(dest, cap, L"\\");
        wcscat_s(dest, cap, name);
    }
    return 1;
}

static int zi_find_child(const ZiApp *app, int parent, const wchar_t *name) {
    int i;

    for (i = 0; i < app->folder_count; i++) {
        if (app->folders[i].parent == parent && _wcsicmp(app->folders[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void zi_reveal_selection(ZiApp *app) {
    int visible[ZI_MAX_FOLDERS];
    int shown = 0;
    int stack_parent[40];
    int stack_index[40];
    int depth = 0;
    int i;

    app->folder_scroll = 0;
    if (app->folder_selected < 0 || !app->tree_root_open) {
        return;
    }
    stack_parent[0] = -1;
    stack_index[0] = 0;
    depth = 1;
    while (depth > 0) {
        int parent = stack_parent[depth - 1];
        int index = stack_index[depth - 1];
        int found = 0;

        for (i = index; i < app->folder_count; i++) {
            if (app->folders[i].parent != parent) {
                continue;
            }
            if (parent < 0 && !app->tree_root_open) {
                continue;
            }
            if (shown >= ZI_MAX_FOLDERS) {
                return;
            }
            visible[shown++] = i;
            stack_index[depth - 1] = i + 1;
            found = 1;
            if (i == app->folder_selected) {
                app->folder_scroll = shown - 1;
                return;
            }
            if (app->folders[i].expanded && depth < 40 && shown < ZI_MAX_FOLDERS) {
                stack_parent[depth] = i;
                stack_index[depth] = 0;
                depth++;
            }
            break;
        }
        if (!found) {
            depth--;
        }
        if (shown >= ZI_MAX_FOLDERS) {
            return;
        }
    }
}

/* Opens each ancestor of the saved folder so that row is selected in the tree. */
static void zi_select_saved(ZiApp *app, const wchar_t *selection) {
    wchar_t rest[MAX_PATH];
    wchar_t *cursor;
    size_t root_len;
    int parent = -1;

    app->folder_selected = -1;
    app->tree_root_open = 1;
    if (!selection || !selection[0] || !app->root[0]) {
        return;
    }
    root_len = wcslen(app->root);
    if (_wcsicmp(selection, app->root) == 0) {
        return;
    }
    if (wcslen(selection) <= root_len || _wcsnicmp(selection, app->root, root_len) != 0) {
        return;
    }
    if (selection[root_len] != L'\\' && selection[root_len] != L'/') {
        return;
    }
    wcscpy_s(rest, MAX_PATH, selection + root_len + 1);
    cursor = rest;
    while (cursor[0]) {
        wchar_t *slash = wcschr(cursor, L'\\');
        int found;

        if (!slash) {
            slash = wcschr(cursor, L'/');
        }
        if (slash) {
            *slash = L'\0';
        }
        if (!cursor[0]) {
            break;
        }
        if (parent >= 0) {
            zi_expand_folder(app, parent);
        }
        found = zi_find_child(app, parent, cursor);
        if (found < 0) {
            app->folder_selected = parent;
            return;
        }
        parent = found;
        cursor = slash ? slash + 1 : cursor + wcslen(cursor);
    }
    app->folder_selected = parent;
    if (parent >= 0) {
        zi_expand_folder(app, parent);
    }
}

static void zi_remember_disk(ZiApp *app) {
    wchar_t path[MAX_PATH];
    wchar_t selection[MAX_PATH];
    const ZiFolder *folders;
    int count;
    int selected;
    FILE *file;

    if (!app->root[0] || !zi_is_dir(app->root) || !zi_folder_file(path, MAX_PATH)) {
        return;
    }
    if (!app->source_server) {
        folders = app->folders;
        count = app->folder_count;
        selected = app->folder_selected;
    } else if (g_tree_kept && !g_kept_is_server) {
        folders = g_kept_folders;
        count = g_kept_count;
        selected = g_kept_selected;
    } else {
        folders = NULL;
        count = 0;
        selected = -1;
    }
    if (!zi_chain_dir(app->root, folders, count, selected, selection, MAX_PATH)) {
        return;
    }
    if (_wfopen_s(&file, path, L"wt, ccs=UTF-8") != 0 || !file) {
        return;
    }
    fwprintf(file, L"root=%ls\nselection=%ls\n", app->root, selection);
    fclose(file);
}

static void zi_restore_disk_folder(ZiApp *app) {
    wchar_t path[MAX_PATH];
    wchar_t root[MAX_PATH];
    wchar_t selection[MAX_PATH];
    wchar_t line[MAX_PATH + 32];
    FILE *file;

    root[0] = L'\0';
    selection[0] = L'\0';
    if (!zi_folder_file(path, MAX_PATH)) {
        return;
    }
    if (_wfopen_s(&file, path, L"rt, ccs=UTF-8") != 0 || !file) {
        return;
    }
    while (fgetws(line, (int)(sizeof line / sizeof line[0]), file)) {
        size_t n = wcslen(line);

        while (n > 0 && (line[n - 1] == L'\n' || line[n - 1] == L'\r')) {
            line[--n] = L'\0';
        }
        if (wcsncmp(line, L"root=", 5) == 0 && wcslen(line + 5) < MAX_PATH) {
            wcscpy_s(root, MAX_PATH, line + 5);
        } else if (wcsncmp(line, L"selection=", 10) == 0 && wcslen(line + 10) < MAX_PATH) {
            wcscpy_s(selection, MAX_PATH, line + 10);
        }
    }
    fclose(file);
    zi_trim_dir(root);
    zi_trim_dir(selection);
    if (!root[0]) {
        return;
    }
    if (!zi_is_dir(root)) {
        wcscpy_s(app->notice, 192, L"The last folder could not be opened.");
        app->disk_folder = 1;
        zi_redraw(app);
        return;
    }
    wcscpy_s(app->root, MAX_PATH, root);
    app->source_server = 0;
    app->disk_folder = 1;
    app->notice[0] = L'\0';
    zi_list_folders(app);
    if (!zi_is_dir(selection)) {
        selection[0] = L'\0';
    }
    zi_select_saved(app, selection);
    zi_reveal_selection(app);
    zi_redraw(app);
}

int zi_folder_has_chevron(const ZiApp *app, int index) {
    int i;

    if (index < 0) {
        for (i = 0; i < app->folder_count; i++) {
            if (app->folders[i].parent < 0) {
                return 1;
            }
        }
        return 0;
    }
    if (index >= app->folder_count) {
        return 0;
    }
    if (!app->folders[index].loaded) {
        return 1;
    }
    for (i = 0; i < app->folder_count; i++) {
        if (app->folders[i].parent == index) {
            return 1;
        }
    }
    return 0;
}

static int zi_pick(HWND owner, wchar_t *dest, size_t cap, const wchar_t *title, int folder) {
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    DWORD options = 0;
    HRESULT hr;
    int ok = 0;
    static const COMDLG_FILTERSPEC k_types[] = {
        { L"Photographs", L"*.jpg;*.jpeg;*.png;*.tif;*.tiff;*.heic;*.heif;*.webp;*.gif;*.bmp;*.dng;*.raf;*.arw;*.cr2;*.nef;*.orf;*.rw2;*.raw;*.x3f;*.pdf;*.zimg" },
        { L"All files", L"*.*" }
    };

    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        return 0;
    }
    dlg->lpVtbl->GetOptions(dlg, &options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    options |= folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    dlg->lpVtbl->SetOptions(dlg, options);
    dlg->lpVtbl->SetTitle(dlg, title);
    if (!folder) {
        dlg->lpVtbl->SetFileTypes(dlg, 2, k_types);
    }
    hr = dlg->lpVtbl->Show(dlg, owner);
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

static void zi_swap_tree(ZiApp *app) {
    int i;
    int swap;
    ZiFolder row;

    for (i = 0; i < ZI_MAX_FOLDERS; i++) {
        row = app->folders[i];
        app->folders[i] = g_kept_folders[i];
        g_kept_folders[i] = row;
    }
    swap = app->folder_count;
    app->folder_count = g_kept_count;
    g_kept_count = swap;
    swap = app->folder_selected;
    app->folder_selected = g_kept_selected;
    g_kept_selected = swap;
    swap = app->folder_scroll;
    app->folder_scroll = g_kept_scroll;
    g_kept_scroll = swap;
    swap = app->tree_root_open;
    app->tree_root_open = g_kept_root_open;
    g_kept_root_open = swap;
    swap = app->folders_are_library;
    app->folders_are_library = g_kept_library;
    g_kept_library = swap;
    g_kept_is_server = !g_kept_is_server;
}

static void zi_store_tree(ZiApp *app, int server) {
    memcpy(g_kept_folders, app->folders, sizeof g_kept_folders);
    g_kept_count = app->folder_count;
    g_kept_selected = app->folder_selected;
    g_kept_scroll = app->folder_scroll;
    g_kept_root_open = app->tree_root_open;
    g_kept_library = app->folders_are_library;
    g_kept_is_server = server;
    g_tree_kept = 1;
}

/* Shows Disk or Server and brings that side's folder tree back. */
static void zi_show_source(ZiApp *app, int server) {
    if (app->source_server == server) {
        if (server) {
            zi_ensure_library_tree(app);
        }
        return;
    }
    if (g_tree_kept && g_kept_is_server == server) {
        zi_swap_tree(app);
    } else {
        zi_store_tree(app, app->source_server);
        zi_clear_folders(app);
        app->tree_root_open = 0;
        app->folders_are_library = 0;
    }
    app->source_server = server;
    if (server) {
        zi_ensure_library_tree(app);
    }
}

static int zi_path_is_zimg(const wchar_t *path) {
    const wchar_t *dot = wcsrchr(path, L'.');

    return dot && _wcsicmp(dot, L".zimg") == 0;
}

static void zi_open_file(ZiApp *app) {
    wchar_t path[MAX_PATH];

    path[0] = L'\0';
    app->notice[0] = L'\0';
    if (!zi_pick(app->hwnd, path, MAX_PATH, L"Open File", 0)) {
        return;
    }
    if (zi_path_is_zimg(path)) {
        zi_editor_open_zimg(app, path);
        return;
    }
    zi_show_source(app, 0);
    zi_editor_open_path(app, path);
}

static void zi_open_folder(ZiApp *app) {
    wchar_t path[MAX_PATH];

    path[0] = L'\0';
    app->notice[0] = L'\0';
    if (!zi_pick(app->hwnd, path, MAX_PATH, L"Open Folder", 1)) {
        return;
    }
    zi_show_source(app, 0);
    wcscpy_s(app->root, MAX_PATH, path);
    app->disk_folder = 1;
    zi_list_folders(app);
    zi_remember_disk(app);
    zi_set_title(app);
    zi_redraw(app);
}

static const wchar_t k_library_unc[] = L"\\\\192.168.1.147\\BackupData\\PhotoVideoAudio";

static int zi_adopt_share(ZiApp *app) {
    DWORD attr;

    if (app->library_root[0]) {
        return 1;
    }
    attr = GetFileAttributesW(k_library_unc);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return 0;
    }
    wcscpy_s(app->library_root, MAX_PATH, k_library_unc);
    return 1;
}

static void zi_prompt_share(ZiApp *app) {
    NETRESOURCEW resource;

    if (zi_adopt_share(app)) {
        return;
    }
    ZeroMemory(&resource, sizeof resource);
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = L"\\\\192.168.1.147\\BackupData";
    WNetAddConnection3W(app->hwnd, &resource, NULL, NULL, CONNECT_INTERACTIVE | CONNECT_PROMPT);
    zi_adopt_share(app);
}

static void zi_watch_server(ZiApp *app) {
    if (!app->hwnd) {
        return;
    }
    if (app->source_server) {
        SetTimer(app->hwnd, 1, 10000, NULL);
        zi_helper_refresh(app->hwnd);
        zi_library_refresh_pass(app);
    } else {
        KillTimer(app->hwnd, 1);
        zi_library_clear_pass(app);
    }
}

static void zi_choose_library(ZiApp *app) {
    app->notice[0] = L'\0';
    if (!zi_pick(app->hwnd, app->library_root, MAX_PATH, L"Choose Library Base Folder", 1)) {
        return;
    }
    zi_show_source(app, 1);
    app->server_folder = 1;
    app->library_section = 0;
    zi_list_library(app);
    zi_watch_server(app);
    zi_redraw(app);
}

static void zi_show_folder(ZiApp *app) {
    app->notice[0] = L'\0';
    if (app->source_server) {
        app->server_folder = 1;
    } else {
        app->disk_folder = 1;
    }
    zi_editor_match_folder_tile(app);
    zi_redraw(app);
}

static void zi_show_image(ZiApp *app) {
    app->notice[0] = L'\0';
    if (app->file_selected >= 0 && app->file_selected < app->file_count &&
        !app->files[app->file_selected].video) {
        zi_editor_open_selection(app);
        return;
    }
    if (app->source_server) {
        app->server_folder = 0;
    } else {
        app->disk_folder = 0;
    }
    zi_redraw(app);
}

static void zi_sync_menu(ZiApp *app, HMENU menu) {
    HMENU file = GetSubMenu(menu, 0);
    HMENU edit = GetSubMenu(menu, 1);
    HMENU view = GetSubMenu(menu, 2);
    int folder;
    UINT save_on;

    if (file) {
        save_on = app->photo_path[0] ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(file, ZI_CMD_SAVE, MF_BYCOMMAND | save_on);
        EnableMenuItem(file, ZI_CMD_SAVE_AS, MF_BYCOMMAND | save_on);
    }
    if (!edit || !view) {
        return;
    }
    EnableMenuItem(edit, ZI_CMD_UNDO, MF_BYCOMMAND | (zi_editor_can_undo(app) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(edit, ZI_CMD_REDO, MF_BYCOMMAND | (zi_editor_can_redo(app) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(edit, ZI_CMD_COPY, MF_BYCOMMAND | (zi_editor_can_copy(app) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(edit, ZI_CMD_PASTE, MF_BYCOMMAND | (zi_editor_can_paste(app) ? MF_ENABLED : MF_GRAYED));
    folder = zi_showing_folder(app);
    CheckMenuItem(view, ZI_CMD_FOLDER_VIEW, MF_BYCOMMAND | (folder ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(view, ZI_CMD_IMAGE_VIEW, MF_BYCOMMAND | (folder ? MF_UNCHECKED : MF_CHECKED));
    CheckMenuItem(view, ZI_CMD_DISK, MF_BYCOMMAND | (app->source_server ? MF_UNCHECKED : MF_CHECKED));
    CheckMenuItem(view, ZI_CMD_SERVER, MF_BYCOMMAND | (app->source_server ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(view, ZI_CMD_SUBFOLDER, MF_BYCOMMAND | (app->subfolders ? MF_CHECKED : MF_UNCHECKED));
}

static HMENU zi_popup_menu(const ZiApp *app) {
    HMENU menu = CreatePopupMenu();

    if (app->source_server) {
        AppendMenuW(menu, MF_STRING, ZI_CMD_CHOOSE_LIBRARY, L"Choose Library Base Folder");
        AppendMenuW(menu, MF_STRING, ZI_CMD_PAIR, L"Pair with Helper");
        AppendMenuW(menu, zi_library_clean_ready(app) ? MF_STRING : MF_STRING | MF_GRAYED,
                    ZI_CMD_CLEAN, L"Clean folder");
    } else {
        AppendMenuW(menu, MF_STRING, ZI_CMD_OPEN_FILE, L"Open File");
        AppendMenuW(menu, MF_STRING, ZI_CMD_OPEN_FOLDER, L"Open Folder");
    }
    return menu;
}

static void zi_show_popup(ZiApp *app) {
    HMENU menu;
    RECT rc;
    int i;
    int found = 0;

    zi_layout_now(app);
    for (i = 0; i < app->ctrl_count; i++) {
        if (app->ctrls[i].id == ZI_ID_MENU) {
            rc = app->ctrls[i].rc;
            found = 1;
            break;
        }
    }
    if (!found) {
        return;
    }
    menu = zi_popup_menu(app);
    ClientToScreen(app->hwnd, (POINT *)&rc.left);
    ClientToScreen(app->hwnd, (POINT *)&rc.right);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, rc.left, rc.bottom, 0, app->hwnd, NULL);
    DestroyMenu(menu);
}

static int zi_chevron_hit(const ZiApp *app, int extra, int x) {
    int depth = extra < 0 ? 0 : app->folders[extra].depth + 1;
    int left = app->regions.sidebar.left + zi_px(app, 8) + zi_px(app, 14) * depth;
    int right = left + zi_px(app, 16);

    return x >= left && x < right;
}

static void zi_on_tree_click(ZiApp *app, int extra, int x) {
    int chevron = zi_folder_has_chevron(app, extra) && zi_chevron_hit(app, extra, x);

    app->notice[0] = L'\0';
    if (extra < 0) {
        if (chevron) {
            app->tree_root_open = !app->tree_root_open;
        } else {
            app->folder_selected = -1;
            app->tree_root_open = 1;
            if (app->source_server) {
                zi_library_show_photos(app);
                app->server_folder = 1;
            } else {
                app->disk_folder = 1;
            }
        }
        if (!app->source_server) {
            zi_remember_disk(app);
        }
        zi_redraw(app);
        return;
    }
    if (chevron && app->folders[extra].expanded) {
        app->folders[extra].expanded = 0;
    } else if (chevron) {
        zi_expand_folder(app, extra);
    } else {
        app->folder_selected = extra;
        if (app->source_server) {
            zi_library_show_photos(app);
            app->server_folder = 1;
        } else {
            app->disk_folder = 1;
        }
        if (!app->folders[extra].expanded) {
            zi_expand_folder(app, extra);
        }
    }
    if (!app->source_server) {
        zi_remember_disk(app);
    }
    zi_redraw(app);
}

static void zi_pick_skin_preset(ZiApp *app) {
    HMENU menu;
    POINT pt;
    RECT rc = { 0 };
    int i;
    int cmd;
    int found = 0;

    if (!app->hwnd) {
        return;
    }
    for (i = 0; i < app->ctrl_count; i++) {
        if (app->ctrls[i].id == ZI_ID_SKIN_MENU) {
            rc = app->ctrls[i].rc;
            found = 1;
            break;
        }
    }
    if (!found) {
        return;
    }
    pt.x = rc.left;
    pt.y = rc.bottom;
    ClientToScreen(app->hwnd, &pt);
    menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    for (i = 0; i < zi_skin_count(); i++) {
        UINT flags = MF_STRING;

        if (i == app->skin_preset) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(menu, flags, (UINT_PTR)(i + 1), zi_skin_label(i));
    }
    SetForegroundWindow(app->hwnd);
    cmd = (int)TrackPopupMenu(menu,
                              TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                              pt.x, pt.y, 0, app->hwnd, NULL);
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= zi_skin_count()) {
        app->skin_preset = cmd - 1;
        zi_redraw(app);
    }
}

static void zi_pick_lens(ZiApp *app) {
    HMENU menu;
    POINT pt;
    RECT rc = { 0 };
    int i;
    int cmd;
    int found = 0;
    int current = app->edit.lens;

    if (!app->hwnd || !app->photo_path[0] || app->photo_failed) {
        return;
    }
    for (i = 0; i < app->ctrl_count; i++) {
        if (app->ctrls[i].id == ZI_ID_LENS_MENU) {
            rc = app->ctrls[i].rc;
            found = 1;
            break;
        }
    }
    if (!found) {
        return;
    }
    if (current < 0 || current >= zi_lens_count()) {
        current = 0;
    }
    pt.x = rc.left;
    pt.y = rc.bottom;
    ClientToScreen(app->hwnd, &pt);
    menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    for (i = 0; i < zi_lens_count(); i++) {
        UINT flags = MF_STRING;

        if (i == current) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(menu, flags, (UINT_PTR)(i + 1), zi_lens_label(i));
    }
    SetForegroundWindow(app->hwnd);
    cmd = (int)TrackPopupMenu(menu,
                              TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                              pt.x, pt.y, 0, app->hwnd, NULL);
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= zi_lens_count()) {
        zi_editor_choose_lens(app, cmd - 1);
    }
}

static void zi_on_click(ZiApp *app, int id, int extra, int x) {
    POINT pt;

    switch (id) {
    case ZI_ID_MENU:
        zi_show_popup(app);
        break;
    case ZI_ID_OPEN_FILE:
        zi_open_file(app);
        break;
    case ZI_ID_OPEN_FOLDER:
        zi_open_folder(app);
        break;
    case ZI_ID_SOURCE_DISK:
        app->notice[0] = L'\0';
        zi_show_source(app, 0);
        zi_watch_server(app);
        zi_redraw(app);
        break;
    case ZI_ID_SOURCE_SERVER:
        app->notice[0] = L'\0';
        zi_show_source(app, 1);
        zi_watch_server(app);
        zi_redraw(app);
        break;
    case ZI_ID_SUBFOLDER:
        app->subfolders = !app->subfolders;
        zi_redraw(app);
        break;
    case ZI_ID_VIEW_FOLDER:
        zi_show_folder(app);
        break;
    case ZI_ID_VIEW_IMAGE:
        zi_show_image(app);
        break;
    case ZI_ID_UNDO:
        zi_editor_undo(app);
        break;
    case ZI_ID_REDO:
        zi_editor_redo(app);
        break;
    case ZI_ID_COPY:
        zi_editor_copy(app);
        break;
    case ZI_ID_PASTE:
    case ZI_ID_FOLDER_PASTE:
        zi_editor_paste(app);
        break;
    case ZI_ID_ROT_LEFT:
        zi_editor_rotate(app, 0);
        break;
    case ZI_ID_ROT_RIGHT:
        zi_editor_rotate(app, 1);
        break;
    case ZI_ID_FOLDER_EXPORT:
    case ZI_ID_EXPORT:
        zi_editor_export(app);
        break;
    case ZI_ID_FOLDER_PRINT:
    case ZI_ID_PRINT:
        zi_editor_print(app);
        break;
    case ZI_ID_SELECT:
        zi_editor_toggle_select(app);
        break;
    case ZI_ID_CROP:
        zi_editor_apply_crop(app);
        break;
    case ZI_ID_DUST:
        zi_editor_touch_dust(app);
        break;
    case ZI_ID_SCRATCHES:
        zi_editor_touch_scratches(app);
        break;
    case ZI_ID_SPOT:
        zi_editor_toggle_spot(app);
        break;
    case ZI_ID_SCRATCH:
        zi_editor_toggle_scratch(app);
        break;
    case ZI_ID_ZOOM:
        zi_editor_use_zoom(app);
        break;
    case ZI_ID_LIB:
        zi_library_open_section(app, extra);
        zi_redraw(app);
        break;
    case ZI_ID_EVENTS:
        zi_library_toggle_events(app);
        zi_redraw(app);
        break;
    case ZI_ID_REORG:
        zi_library_toggle_reorg(app);
        zi_redraw(app);
        break;
    case ZI_ID_TREE:
        zi_on_tree_click(app, extra, x);
        break;
    case ZI_ID_SECTION:
        if (extra >= 0 && extra < ZI_SECTIONS) {
            app->section_open[extra] = !app->section_open[extra];
            zi_redraw(app);
        }
        break;
    case ZI_ID_HISTORY:
        zi_editor_history_select(app, extra);
        break;
    case ZI_ID_AUTO:
        zi_editor_auto(app);
        break;
    case ZI_ID_RESET:
        zi_editor_reset(app);
        break;
    case ZI_ID_COLOR_WHITE:
        app->eyedropper_skin = 0;
        zi_redraw(app);
        break;
    case ZI_ID_COLOR_SKIN:
        app->eyedropper_skin = 1;
        zi_redraw(app);
        break;
    case ZI_ID_EYEDROPPER:
        zi_editor_toggle_eyedropper(app);
        break;
    case ZI_ID_HORIZON:
        zi_editor_toggle_horizon(app);
        break;
    case ZI_ID_CORNER_PIN:
        zi_editor_set_corner_pin(app, !app->edit.has_corners);
        break;
    case ZI_ID_LENS_MENU:
        zi_pick_lens(app);
        break;
    case ZI_ID_SKIN_MENU:
        zi_pick_skin_preset(app);
        break;
    case ZI_ID_SWATCH:
        if (extra >= 0 && extra < zi_skin_count()) {
            app->skin_preset = extra;
            zi_redraw(app);
        }
        break;
    case ZI_ID_SPLIT:
        app->drag_split = 1;
        app->drag_anchor_width = app->source_server ? app->library_width : app->tree_width;
        GetCursorPos(&pt);
        app->drag_anchor_x = pt.x;
        SetCapture(app->hwnd);
        break;
    default:
        break;
    }
}

static void zi_menu_label(const wchar_t *src, wchar_t *dest, size_t cap) {
    size_t n = 0;

    if (!src) {
        src = L"";
    }
    while (*src && n + 2 < cap) {
        if (*src == L'&' && n + 3 < cap) {
            dest[n++] = L'&';
        }
        dest[n++] = *src++;
    }
    dest[n] = L'\0';
}

static int zi_paste_targets(const ZiApp *app, int index) {
    int i;
    int count = 0;

    if (!app->has_copied || index < 0 || index >= app->file_count) {
        return 0;
    }
    if (app->file_mark[index]) {
        for (i = 0; i < app->file_count; i++) {
            if (app->file_mark[i] && !app->files[i].video) {
                count++;
            }
        }
        return count;
    }
    return app->files[index].video ? 0 : 1;
}

static void zi_release_openers(IAssocHandler **handlers, int count) {
    int i;

    for (i = 0; i < count; i++) {
        if (handlers[i]) {
            handlers[i]->lpVtbl->Release(handlers[i]);
            handlers[i] = NULL;
        }
    }
}

static int zi_opener_known(IAssocHandler **handlers, int count, const wchar_t *exe) {
    int i;

    for (i = 0; i < count; i++) {
        LPWSTR name = NULL;

        if (SUCCEEDED(handlers[i]->lpVtbl->GetName(handlers[i], &name)) && name) {
            int same = _wcsicmp(name, exe) == 0;

            CoTaskMemFree(name);
            if (same) {
                return 1;
            }
        }
    }
    return 0;
}

static void zi_collect_openers(const wchar_t *path, IAssocHandler **handlers, int *count) {
    wchar_t self_path[MAX_PATH];
    const wchar_t *ext;
    int pass;

    *count = 0;
    self_path[0] = L'\0';
    GetModuleFileNameW(NULL, self_path, MAX_PATH);
    ext = wcsrchr(path, L'.');
    if (!ext || !ext[1]) {
        return;
    }
    for (pass = 0; pass < 2 && *count == 0; pass++) {
        IEnumAssocHandlers *enumerator = NULL;
        ASSOC_FILTER filter = pass == 0 ? ASSOC_FILTER_RECOMMENDED : ASSOC_FILTER_NONE;

        if (FAILED(SHAssocEnumHandlers(ext, filter, &enumerator)) || !enumerator) {
            continue;
        }
        for (;;) {
            IAssocHandler *handler = NULL;
            LPWSTR exe = NULL;
            ULONG fetched = 0;
            int skip;

            if (enumerator->lpVtbl->Next(enumerator, 1, &handler, &fetched) != S_OK || !handler) {
                break;
            }
            skip = 0;
            if (FAILED(handler->lpVtbl->GetName(handler, &exe)) || !exe || !exe[0]) {
                skip = 1;
            } else if (_wcsicmp(exe, self_path) == 0 || zi_opener_known(handlers, *count, exe)) {
                skip = 1;
            }
            if (exe) {
                CoTaskMemFree(exe);
            }
            if (skip || *count >= ZI_CTX_APPS) {
                handler->lpVtbl->Release(handler);
                if (*count >= ZI_CTX_APPS) {
                    break;
                }
                continue;
            }
            handlers[(*count)++] = handler;
        }
        enumerator->lpVtbl->Release(enumerator);
    }
}

static void zi_opener_name(IAssocHandler *handler, wchar_t *dest, size_t cap) {
    LPWSTR name = NULL;

    dest[0] = L'\0';
    if (SUCCEEDED(handler->lpVtbl->GetUIName(handler, &name)) && name && name[0]) {
        zi_menu_label(name, dest, cap);
        CoTaskMemFree(name);
        return;
    }
    if (name) {
        CoTaskMemFree(name);
        name = NULL;
    }
    if (SUCCEEDED(handler->lpVtbl->GetName(handler, &name)) && name && name[0]) {
        const wchar_t *base = wcsrchr(name, L'\\');

        base = (base && base[1]) ? base + 1 : name;
        zi_menu_label(base, dest, cap);
        CoTaskMemFree(name);
        return;
    }
    if (name) {
        CoTaskMemFree(name);
    }
    wcscpy_s(dest, cap, L"Application");
}

static HRESULT zi_item_data(HWND hwnd, const wchar_t *path, IDataObject **out) {
    PIDLIST_ABSOLUTE pidl = NULL;
    IShellFolder *folder = NULL;
    PCUITEMID_CHILD child = NULL;
    HRESULT hr;

    *out = NULL;
    hr = SHParseDisplayName(path, NULL, &pidl, 0, NULL);
    if (FAILED(hr) || !pidl) {
        return hr;
    }
    hr = SHBindToParent(pidl, &IID_IShellFolder, (void **)&folder, &child);
    if (SUCCEEDED(hr) && folder && child) {
        hr = folder->lpVtbl->GetUIObjectOf(folder, hwnd, 1, &child, &IID_IDataObject, NULL, (void **)out);
    }
    if (folder) {
        folder->lpVtbl->Release(folder);
    }
    CoTaskMemFree(pidl);
    return hr;
}

static void zi_open_with_exe(ZiApp *app, const wchar_t *exe, const wchar_t *path) {
    wchar_t params[MAX_PATH + 8];
    HINSTANCE opened;

    swprintf_s(params, MAX_PATH + 8, L"\"%s\"", path);
    opened = ShellExecuteW(app->hwnd, NULL, exe, params, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)opened <= 32) {
        zi_notice(app, L"That application could not open the file.");
    }
}

static void zi_run_opener(ZiApp *app, IAssocHandler *handler, const wchar_t *path) {
    IDataObject *data = NULL;
    HRESULT hr = E_FAIL;

    if (SUCCEEDED(zi_item_data(app->hwnd, path, &data)) && data) {
        hr = handler->lpVtbl->Invoke(handler, data);
        data->lpVtbl->Release(data);
    }
    if (FAILED(hr)) {
        LPWSTR exe = NULL;

        if (SUCCEEDED(handler->lpVtbl->GetName(handler, &exe)) && exe && exe[0]) {
            zi_open_with_exe(app, exe, path);
        } else {
            zi_notice(app, L"That application could not open the file.");
        }
        if (exe) {
            CoTaskMemFree(exe);
        }
    }
}

static void zi_show_in_explorer(ZiApp *app, const wchar_t *path) {
    PIDLIST_ABSOLUTE pidl = NULL;
    HRESULT hr;

    hr = SHParseDisplayName(path, NULL, &pidl, 0, NULL);
    if (SUCCEEDED(hr) && pidl) {
        hr = SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
        CoTaskMemFree(pidl);
    }
    if (FAILED(hr)) {
        zi_notice(app, L"That file could not be shown in Explorer.");
    }
}

static void zi_size_label(const wchar_t *path, wchar_t *dest, size_t cap) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    unsigned long long bytes;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
        wcscpy_s(dest, cap, L"Size unavailable");
        return;
    }
    bytes = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    swprintf_s(dest, cap, L"%.1f MB", (double)bytes / 1000000.0);
}

static void zi_show_file_menu(ZiApp *app, int index, int x, int y) {
    IAssocHandler *handlers[ZI_CTX_APPS];
    int handler_count = 0;
    int video;
    int i;
    int cmd;
    HMENU menu;
    HMENU open_in;
    POINT pt;
    const wchar_t *path;
    wchar_t label[160];
    wchar_t size_text[64];

    if (index < 0 || index >= app->file_count) {
        return;
    }
    path = app->files[index].path;
    video = app->files[index].video;
    zi_collect_openers(path, handlers, &handler_count);
    menu = CreatePopupMenu();
    open_in = CreatePopupMenu();
    if (!menu || !open_in) {
        if (menu) {
            DestroyMenu(menu);
        }
        if (open_in) {
            DestroyMenu(open_in);
        }
        zi_release_openers(handlers, handler_count);
        return;
    }
    if (video) {
        if (handler_count > 0) {
            wchar_t app_name[128];

            zi_opener_name(handlers[0], app_name, 128);
            swprintf_s(label, 160, L"Open with %s", app_name);
            AppendMenuW(menu, MF_STRING, ZI_CMD_CTX_OPEN, label);
        } else {
            AppendMenuW(menu, MF_STRING, ZI_CMD_CTX_OPEN, L"Open");
        }
    } else {
        AppendMenuW(menu, MF_STRING, ZI_CMD_CTX_EDIT, L"Edit Image");
    }
    AppendMenuW(menu, MF_STRING, ZI_CMD_CTX_REVEAL, L"Show in Explorer");
    if (!video) {
        AppendMenuW(open_in, MF_STRING, ZI_CMD_CTX_EDIT, L"ZImager");
    }
    if (handler_count == 0 && video) {
        AppendMenuW(open_in, MF_GRAYED | MF_STRING, ZI_CMD_CTX_SIZE, L"No application");
    }
    for (i = 0; i < handler_count; i++) {
        zi_opener_name(handlers[i], label, 160);
        AppendMenuW(open_in, MF_STRING, (UINT_PTR)(ZI_CMD_CTX_APP0 + i), label);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)open_in, video ? L"Open With" : L"Open In");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (video ? MF_GRAYED : MF_ENABLED), ZI_CMD_CTX_COPY, L"Copy settings");
    AppendMenuW(menu, MF_STRING | (zi_paste_targets(app, index) > 0 ? MF_ENABLED : MF_GRAYED),
                ZI_CMD_CTX_PASTE, L"Paste settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    zi_size_label(path, size_text, 64);
    AppendMenuW(menu, MF_GRAYED | MF_STRING, ZI_CMD_CTX_SIZE, size_text);
    if (app->source_server && app->library_section == 0 && !app->events_open && !app->reorg_open) {
        UINT flags = MF_STRING;

        if (!zi_library_can_remove(app, index)) {
            flags |= MF_GRAYED;
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, flags, ZI_CMD_CTX_REMOVE, L"Remove from library");
    }
    pt.x = x;
    pt.y = y;
    ClientToScreen(app->hwnd, &pt);
    SetForegroundWindow(app->hwnd);
    cmd = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                         pt.x, pt.y, 0, app->hwnd, NULL);
    PostMessageW(app->hwnd, WM_NULL, 0, 0);
    if (cmd == ZI_CMD_CTX_EDIT) {
        zi_grid_select_only(app, index);
        zi_editor_open_path(app, path);
    } else if (cmd == ZI_CMD_CTX_REVEAL) {
        zi_show_in_explorer(app, path);
    } else if (cmd == ZI_CMD_CTX_OPEN) {
        if (handler_count > 0) {
            zi_run_opener(app, handlers[0], path);
        } else {
            HINSTANCE opened = ShellExecuteW(app->hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);

            if ((INT_PTR)opened <= 32) {
                zi_notice(app, L"That file could not be opened.");
            }
        }
    } else if (cmd == ZI_CMD_CTX_COPY) {
        zi_editor_copy_path(app, path);
    } else if (cmd == ZI_CMD_CTX_PASTE) {
        zi_editor_paste_index(app, index);
    } else if (cmd == ZI_CMD_CTX_REMOVE) {
        zi_library_remove(app, index);
    } else if (cmd >= ZI_CMD_CTX_APP0 && cmd < ZI_CMD_CTX_APP0 + handler_count) {
        zi_run_opener(app, handlers[cmd - ZI_CMD_CTX_APP0], path);
    }
    DestroyMenu(menu);
    zi_release_openers(handlers, handler_count);
}

static void zi_on_command(ZiApp *app, int cmd) {
    switch (cmd) {
    case ZI_CMD_OPEN_FILE:
        zi_open_file(app);
        break;
    case ZI_CMD_OPEN_FOLDER:
        zi_open_folder(app);
        break;
    case ZI_CMD_SAVE:
        zi_editor_save(app);
        break;
    case ZI_CMD_SAVE_AS:
        zi_editor_save_as(app);
        break;
    case ZI_CMD_EXIT:
        DestroyWindow(app->hwnd);
        break;
    case ZI_CMD_FOLDER_VIEW:
        zi_show_folder(app);
        break;
    case ZI_CMD_IMAGE_VIEW:
        zi_show_image(app);
        break;
    case ZI_CMD_DISK:
        app->notice[0] = L'\0';
        zi_show_source(app, 0);
        zi_watch_server(app);
        zi_redraw(app);
        break;
    case ZI_CMD_SERVER:
        app->notice[0] = L'\0';
        zi_show_source(app, 1);
        zi_watch_server(app);
        zi_redraw(app);
        break;
    case ZI_CMD_SUBFOLDER:
        app->subfolders = !app->subfolders;
        zi_redraw(app);
        break;
    case ZI_CMD_CHOOSE_LIBRARY:
        zi_choose_library(app);
        break;
    case ZI_CMD_PAIR: {
        wchar_t message[192];

        zi_prompt_share(app);
        if (!app->library_root[0]) {
            zi_notice(app, L"Sign in to \\\\192.168.1.147\\BackupData, then choose the PhotoVideoAudio folder.");
            break;
        }
        zi_helper_pair(app->library_root, message, 192);
        zi_notice(app, message);
        zi_helper_refresh(app->hwnd);
        break;
    }
    case ZI_CMD_CLEAN:
        zi_library_clean(app);
        break;
    case ZI_CMD_UNDO:
        zi_editor_undo(app);
        break;
    case ZI_CMD_REDO:
        zi_editor_redo(app);
        break;
    case ZI_CMD_COPY:
        zi_editor_copy(app);
        break;
    case ZI_CMD_PASTE:
        zi_editor_paste(app);
        break;
    default:
        break;
    }
}

static void zi_zoom_drag(ZiApp *app, int x) {
    RECT rc;
    int i;
    int span;
    int origin;
    double t;

    for (i = 0; i < app->ctrl_count; i++) {
        if (app->ctrls[i].id != ZI_ID_ZOOM_TRACK) {
            continue;
        }
        rc = app->ctrls[i].rc;
        origin = rc.left + zi_px(app, 6);
        span = (rc.right - rc.left) - zi_px(app, 12);
        if (span < 1) {
            span = 1;
        }
        t = (double)(x - origin) / (double)span;
        zi_editor_set_zoom_pos(app, t);
        zi_editor_format_zoom(app);
        if (app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return;
    }
}

static void zi_spectrum_drag(ZiApp *app, int x) {
    int i;
    int width;
    double next;

    if (app->slider_index < 0 || app->slider_index > 2) {
        return;
    }
    for (i = 0; i < app->ctrl_count; i++) {
        if (app->ctrls[i].id != ZI_ID_SPECTRUM) {
            continue;
        }
        width = app->ctrls[i].rc.right - app->ctrls[i].rc.left;
        if (width < 2) {
            return;
        }
        next = app->spectrum_origin + (double)(x - app->spectrum_down_x) / (double)width;
        zi_editor_set_slider(app, app->slider_index, next);
        return;
    }
}

static int g_touch_drag;

static void zi_touch_slider_drag(ZiApp *app, int x) {
    int i;
    RECT label;
    RECT track;
    RECT value;
    double span;
    double t;

    if (!g_touch_drag) {
        return;
    }
    for (i = 0; i < app->ctrl_count; i++) {
        if (app->ctrls[i].id != g_touch_drag) {
            continue;
        }
        zi_slider_geometry(app, &app->ctrls[i].rc, &label, &track, &value);
        span = (double)(track.right - track.left);
        if (span < 1) {
            span = 1;
        }
        t = (double)(x - track.left) / span;
        if (t < 0) {
            t = 0;
        }
        if (t > 1) {
            t = 1;
        }
        if (g_touch_drag == ZI_ID_TOUCH_SIZE) {
            zi_editor_touch_size_set(app, 8 + (int)(t * 392.0 + 0.5), 0);
        } else if (g_touch_drag == ZI_ID_SCRATCH_AMOUNT) {
            zi_editor_touch_amount_set(app, 1, (float)t);
        } else {
            zi_editor_touch_amount_set(app, 0, (float)t);
        }
        return;
    }
}

static void zi_slider_drag(ZiApp *app, int x) {
    RECT label;
    RECT track;
    RECT value;
    const ZiSliderSpec *spec;
    int i;
    double span;
    double t;

    for (i = 0; i < app->ctrl_count; i++) {
        if (app->ctrls[i].id != ZI_ID_SLIDER || app->ctrls[i].extra != app->slider_index) {
            continue;
        }
        zi_slider_geometry(app, &app->ctrls[i].rc, &label, &track, &value);
        span = (double)(track.right - track.left);
        if (span < 1) {
            span = 1;
        }
        t = (double)(x - track.left) / span;
        if (t < 0) {
            t = 0;
        }
        if (t > 1) {
            t = 1;
        }
        spec = zi_slider_spec(app->slider_index);
        zi_editor_set_slider(app, app->slider_index, spec->min_value + t * (spec->max_value - spec->min_value));
        return;
    }
}

static void zi_release_drag(ZiApp *app) {
    int commit = app->drag_slider;
    int crop = app->drag_crop;
    int scratch = app->scratch_drag;
    int amount = app->amount_drag;

    if (!app->drag_split && !app->drag_pan && !app->drag_zoom && !app->drag_preview &&
        !app->drag_slider && !app->drag_spectrum && !crop && !scratch && !amount && !g_touch_drag) {
        return;
    }
    app->drag_split = 0;
    app->drag_pan = 0;
    app->drag_zoom = 0;
    app->drag_preview = 0;
    app->drag_slider = 0;
    app->drag_spectrum = 0;
    app->drag_crop = 0;
    if (commit) {
        zi_editor_slider_commit(app);
    }
    if (crop) {
        zi_editor_crop_end(app);
    }
    if (g_touch_drag == ZI_ID_DUST_AMOUNT || g_touch_drag == ZI_ID_SCRATCH_AMOUNT || amount) {
        zi_editor_touch_amount_end(app);
    }
    g_touch_drag = 0;
    if (scratch) {
        zi_editor_scratch_finish(app);
    }
    ReleaseCapture();
}

static void zi_drag_split(ZiApp *app) {
    POINT pt;
    int delta;
    int proposed;

    if (!app->drag_split) {
        return;
    }
    GetCursorPos(&pt);
    delta = MulDiv(pt.x - app->drag_anchor_x, 96, (int)app->dpi);
    proposed = app->drag_anchor_width + delta;
    if (proposed < ZI_SIDEBAR_MIN) {
        proposed = ZI_SIDEBAR_MIN;
    }
    if (app->source_server) {
        app->library_width = proposed;
    } else {
        app->tree_width = proposed;
    }
    zi_redraw(app);
}

static void zi_on_wheel(ZiApp *app, int delta, POINT pt) {
    int step = delta > 0 ? -1 : 1;
    int row;

    ScreenToClient(app->hwnd, &pt);
    zi_layout_now(app);
    row = zi_px(app, ZI_ROW_H);
    if (app->events_open && PtInRect(&app->regions.canvas, pt)) {
        zi_library_wheel(app, delta);
        return;
    }
    if (PtInRect(&app->regions.canvas, pt) && zi_grid_active(app)) {
        zi_grid_wheel(app, delta);
        return;
    }
    if (PtInRect(&app->regions.canvas, pt) && !zi_showing_folder(app)) {
        zi_editor_wheel(app, pt.x, pt.y, delta);
        return;
    }
    if (PtInRect(&app->regions.sidebar, pt)) {
        app->folder_scroll += step * 3;
        if (app->folder_scroll < 0) {
            app->folder_scroll = 0;
        }
        zi_redraw(app);
        return;
    }
    if (PtInRect(&app->regions.right, pt) && !zi_showing_folder(app)) {
        app->inspector_scroll += step * row;
        if (app->inspector_scroll < 0) {
            app->inspector_scroll = 0;
        }
        zi_redraw(app);
    }
}

static LRESULT CALLBACK zi_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    ZiApp *app = (ZiApp *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE:
        g_app.hwnd = hwnd;
        g_app.dpi = GetDpiForWindow(hwnd);
        if (g_app.dpi == 0) {
            g_app.dpi = 96;
        }
        zi_rebuild_fonts(&g_app);
        zi_make_brushes(&g_app);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&g_app);
        return TRUE;
    case WM_CREATE:
        zi_restore_disk_folder(&g_app);
        zi_set_title(&g_app);
        zi_helper_refresh(hwnd);
        zi_library_startup(hwnd);
        return 0;
    case WM_TIMER:
        if (wparam == 1 && app && app->source_server) {
            zi_helper_refresh(hwnd);
            zi_library_refresh_pass(app);
        } else if (wparam == 2 && app) {
            zi_library_on_timer(app);
        }
        return 0;
    case ZI_WM_HELPER: {
        ZiHelperNote *note = (ZiHelperNote *)lparam;
        if (note && app) {
            app->helper_online = note->online;
            app->helper_safe = note->safe;
            if (note->line[0]) {
                wcscpy_s(app->helper_line, 192, note->line);
            }
            if (note->online) {
                int had_library = app->library_root[0] != L'\0';

                zi_adopt_share(app);
                if (!had_library && app->library_root[0] && app->source_server) {
                    zi_list_library(app);
                }
            }
            zi_redraw(app);
        }
        free(note);
        return 0;
    }
    case ZI_WM_THUMB:
        if (app) {
            zi_grid_thumb(app, (ZiThumbNote *)lparam);
        } else if (lparam) {
            ZiThumbNote *note = (ZiThumbNote *)lparam;
            if (note->bitmap) {
                DeleteObject(note->bitmap);
            }
            free(note);
        }
        return 0;
    case ZI_WM_PHOTO:
        if (app) {
            zi_editor_on_photo(app, (ZiPhotoNote *)lparam);
        } else if (lparam) {
            ZiPhotoNote *note = (ZiPhotoNote *)lparam;
            if (note->bitmap) {
                DeleteObject(note->bitmap);
            }
            free(note);
        }
        return 0;
    case ZI_WM_GRADE:
        if (app) {
            zi_editor_on_grade(app, (ZiGradeNote *)lparam);
        } else if (lparam) {
            ZiGradeNote *note = (ZiGradeNote *)lparam;
            if (note->bitmap) {
                DeleteObject(note->bitmap);
            }
            free(note);
        }
        return 0;
    case ZI_WM_AUTO:
        if (app) {
            zi_editor_on_auto(app, (ZiAutoNote *)lparam);
        } else if (lparam) {
            free((void *)lparam);
        }
        return 0;
    case ZI_WM_DROP:
        if (app) {
            zi_editor_on_drop(app, (ZiDropNote *)lparam);
        } else if (lparam) {
            free((void *)lparam);
        }
        return 0;
    case ZI_WM_DIGEST:
        if (app) {
            zi_editor_on_digest(app, (ZiDigestNote *)lparam);
        } else if (lparam) {
            free((void *)lparam);
        }
        return 0;
    case ZI_WM_TOUCH:
        if (app) {
            zi_editor_on_touch(app, (ZiTouchNote *)lparam);
        } else if (lparam) {
            ZiTouchNote *note = (ZiTouchNote *)lparam;
            free(note->mask);
            free(note);
        }
        return 0;
    case ZI_WM_FACTS:
        if (app) {
            zi_editor_on_facts(app, (ZiFactNote *)lparam);
        } else if (lparam) {
            free((void *)lparam);
        }
        return 0;
    case ZI_WM_LIST:
        if (app) {
            zi_grid_accept(app, (ZiListNote *)lparam);
            zi_editor_sync_details(app);
        } else if (lparam) {
            ZiListNote *note = (ZiListNote *)lparam;
            free(note->files);
            free(note);
        }
        return 0;
    case ZI_WM_LIBRARY:
        zi_library_on_note(app, (void *)lparam);
        return 0;
    case WM_DPICHANGED: {
        RECT *suggested = (RECT *)lparam;
        g_app.dpi = HIWORD(wparam);
        zi_rebuild_fonts(&g_app);
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO:
        if (app && app->dpi) {
            MINMAXINFO *info = (MINMAXINFO *)lparam;
            RECT rc = { 0, 0, zi_px(app, ZI_MIN_W), zi_px(app, ZI_MIN_H) };
            AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, TRUE, 0, app->dpi);
            info->ptMinTrackSize.x = rc.right - rc.left;
            info->ptMinTrackSize.y = rc.bottom - rc.top;
        }
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        zi_chrome_paint(app ? app : &g_app, hdc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (app && LOWORD(lparam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            zi_layout_now(app);
            if (PtInRect(&app->regions.split, pt)) {
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                return TRUE;
            }
            if (!zi_showing_folder(app) && zi_spectrum_handle_at(app, pt.x, pt.y) >= 0) {
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                return TRUE;
            }
            if (!zi_showing_folder(app)) {
                HCURSOR cursor = zi_editor_cursor(app, pt.x, pt.y);

                if (cursor) {
                    SetCursor(cursor);
                    return TRUE;
                }
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (app) {
            int extra = 0;
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            int id;

            zi_layout_now(app);
            id = zi_library_hittest(app, x, y, &extra);
            if (id != 0) {
                zi_library_click(app, id, extra);
                return 0;
            }
            id = zi_chrome_hittest(app, x, y, &extra);
            if (id == ZI_ID_SPECTRUM) {
                int handle = zi_spectrum_handle_at(app, x, y);

                if (handle >= 0) {
                    app->drag_spectrum = 1;
                    app->drag_slider = 1;
                    app->slider_index = handle;
                    app->spectrum_origin = zi_slider_get(&app->edit, handle);
                    app->spectrum_down_x = x;
                    SetCapture(hwnd);
                }
                return 0;
            }
            if (id == ZI_ID_SLIDER) {
                app->drag_slider = 1;
                app->slider_index = extra;
                SetCapture(hwnd);
                zi_slider_drag(app, x);
                return 0;
            }
            if (id == ZI_ID_TOUCH_SIZE || id == ZI_ID_DUST_AMOUNT || id == ZI_ID_SCRATCH_AMOUNT) {
                g_touch_drag = id;
                if (id == ZI_ID_DUST_AMOUNT) {
                    zi_editor_touch_amount_begin(app, 0);
                } else if (id == ZI_ID_SCRATCH_AMOUNT) {
                    zi_editor_touch_amount_begin(app, 1);
                }
                SetCapture(hwnd);
                zi_touch_slider_drag(app, x);
                return 0;
            }
            if (id == ZI_ID_ZOOM_TRACK) {
                app->drag_zoom = 1;
                SetCapture(hwnd);
                zi_zoom_drag(app, x);
                return 0;
            }
            if (id == ZI_ID_NONE && zi_grid_preview_hit(app, x, y)) {
                app->drag_preview = 1;
                SetCapture(hwnd);
                zi_grid_preview_drag(app, x);
                return 0;
            }
            if (id == ZI_ID_NONE && zi_grid_click(app, x, y)) {
                zi_editor_sync_details(app);
                return 0;
            }
            if (id == ZI_ID_NONE && zi_editor_mouse_down(app, x, y, 0)) {
                return 0;
            }
            zi_on_click(app, id, extra, x);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (app) {
            int extra = 0;
            int index;
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            int id;

            zi_layout_now(app);
            id = zi_library_hittest(app, x, y, &extra);
            if (id != 0) {
                zi_library_click(app, id, extra);
                return 0;
            }
            id = zi_chrome_hittest(app, x, y, &extra);
            if (id == ZI_ID_SPECTRUM) {
                int handle = zi_spectrum_handle_at(app, x, y);

                if (handle >= 0) {
                    zi_editor_spectrum_reset(app, handle);
                }
                return 0;
            }
            if (id == ZI_ID_SLIDER) {
                zi_editor_slider_reset(app, extra);
                return 0;
            }
            if (id == ZI_ID_TOUCH_SIZE) {
                zi_editor_touch_size_set(app, 48, 1);
                return 0;
            }
            if (id == ZI_ID_DUST_AMOUNT || id == ZI_ID_SCRATCH_AMOUNT) {
                zi_editor_touch_amount_begin(app, id == ZI_ID_SCRATCH_AMOUNT);
                zi_editor_touch_amount_set(app, id == ZI_ID_SCRATCH_AMOUNT, 1.0f);
                zi_editor_touch_amount_end(app);
                return 0;
            }
            index = zi_grid_index_at(app, x, y);
            if (index >= 0) {
                zi_grid_select_only(app, index);
                zi_editor_open_path(app, app->files[index].path);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (app) {
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            int index;

            zi_layout_now(app);
            index = zi_grid_index_at(app, x, y);
            if (index >= 0 && (GetKeyState(VK_CONTROL) & 0x8000)) {
                zi_grid_click(app, x, y);
                zi_editor_sync_details(app);
                return 0;
            }
            if (index >= 0) {
                zi_show_file_menu(app, index, x, y);
                return 0;
            }
            zi_editor_mouse_down(app, x, y, 1);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app && (app->drag_pan || app->drag_crop || app->spot_tool || app->scratch_tool || app->scratch_drag)) {
            zi_editor_mouse_move(app, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        }
        if (app && g_touch_drag) {
            zi_touch_slider_drag(app, GET_X_LPARAM(lparam));
        } else if (app && app->drag_zoom) {
            zi_zoom_drag(app, GET_X_LPARAM(lparam));
        } else if (app && app->drag_preview) {
            zi_grid_preview_drag(app, GET_X_LPARAM(lparam));
        } else if (app && app->drag_spectrum) {
            zi_spectrum_drag(app, GET_X_LPARAM(lparam));
        } else if (app && app->drag_slider) {
            zi_slider_drag(app, GET_X_LPARAM(lparam));
        } else if (app && app->drag_split) {
            zi_drag_split(app);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
        if (app) {
            zi_release_drag(app);
        }
        return 0;
    case WM_KEYDOWN:
        if (app && wparam == VK_SPACE) {
            app->space_down = 1;
            return 0;
        }
        if (app && wparam == VK_LEFT) {
            zi_editor_step(app, -1);
            return 0;
        }
        if (app && wparam == VK_RIGHT) {
            zi_editor_step(app, 1);
            return 0;
        }
        break;
    case WM_KEYUP:
        if (app && wparam == VK_SPACE) {
            app->space_down = 0;
            if (app->drag_pan) {
                app->drag_pan = 0;
                if (!app->drag_split && !app->drag_zoom && !app->drag_preview && !app->drag_slider &&
                    !app->drag_spectrum && !app->drag_crop) {
                    ReleaseCapture();
                }
            }
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (app) {
            app->space_down = 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (app) {
            POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            zi_on_wheel(app, GET_WHEEL_DELTA_WPARAM(wparam), pt);
        }
        return 0;
    case WM_VSCROLL:
        if (app && (zi_grid_on_vscroll(app, (HWND)lparam, wparam) ||
                    zi_chrome_on_vscroll(app, (HWND)lparam, wparam))) {
            return 0;
        }
        break;
    case WM_INITMENU:
        if (app) {
            zi_sync_menu(app, (HMENU)wparam);
        }
        return 0;
    case WM_COMMAND:
        if (app) {
            zi_on_command(app, LOWORD(wparam));
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        zi_remember_disk(&g_app);
        zi_editor_shutdown(&g_app);
        zi_library_shutdown();
        zi_grid_shutdown(&g_app);
        zi_helper_shutdown();
        zi_free_gdi(&g_app);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HMENU zi_menu_bar(void) {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU edit = CreatePopupMenu();
    HMENU view = CreatePopupMenu();

    AppendMenuW(file, MF_STRING, ZI_CMD_OPEN_FILE, L"Open File\tCtrl+O");
    AppendMenuW(file, MF_STRING, ZI_CMD_OPEN_FOLDER, L"Open Folder");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ZI_CMD_SAVE, L"Save\tCtrl+S");
    AppendMenuW(file, MF_STRING, ZI_CMD_SAVE_AS, L"Save As\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ZI_CMD_EXIT, L"Exit");
    AppendMenuW(edit, MF_STRING, ZI_CMD_UNDO, L"Undo\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, ZI_CMD_REDO, L"Redo\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, ZI_CMD_COPY, L"Copy\tCtrl+C");
    AppendMenuW(edit, MF_STRING, ZI_CMD_PASTE, L"Paste\tCtrl+V");
    AppendMenuW(view, MF_STRING, ZI_CMD_FOLDER_VIEW, L"Folder View\tCtrl+F");
    AppendMenuW(view, MF_STRING, ZI_CMD_IMAGE_VIEW, L"Edit View\tCtrl+E");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, ZI_CMD_DISK, L"Disk");
    AppendMenuW(view, MF_STRING, ZI_CMD_SERVER, L"Server");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, ZI_CMD_SUBFOLDER, L"Sub-folder");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"View");
    return bar;
}

static HACCEL zi_accelerators(void) {
    ACCEL keys[] = {
        { FCONTROL | FVIRTKEY, 'O', ZI_CMD_OPEN_FILE },
        { FCONTROL | FVIRTKEY, 'S', ZI_CMD_SAVE },
        { FCONTROL | FSHIFT | FVIRTKEY, 'S', ZI_CMD_SAVE_AS },
        { FCONTROL | FVIRTKEY, 'F', ZI_CMD_FOLDER_VIEW },
        { FCONTROL | FVIRTKEY, 'E', ZI_CMD_IMAGE_VIEW },
        { FCONTROL | FVIRTKEY, 'Z', ZI_CMD_UNDO },
        { FCONTROL | FVIRTKEY, 'Y', ZI_CMD_REDO },
        { FCONTROL | FVIRTKEY, 'C', ZI_CMD_COPY },
        { FCONTROL | FVIRTKEY, 'V', ZI_CMD_PASTE }
    };
    return CreateAcceleratorTableW(keys, 9);
}

int zi_run(HINSTANCE instance) {
    WNDCLASSW wc;
    HWND hwnd;
    HACCEL accel;
    MSG msg;
    RECT rc;
    int i;
    UINT dpi;

    ZeroMemory(&g_app, sizeof g_app);
    zi_tone_identity(&g_app.edit);
    g_app.disk_folder = 1;
    g_app.server_folder = 1;
    g_app.tree_width = ZI_SIDEBAR_INIT;
    g_app.library_width = ZI_SIDEBAR_INIT;
    g_app.folder_selected = -1;
    g_app.file_selected = -1;
    g_app.file_anchor = -1;
    g_app.preview_dip = 160;
    g_app.zoom_mag = 1;
    g_app.zoom_min = 0.1;
    g_app.zoom_tool = 1;
    g_app.touch_size = 48;
    wcscpy_s(g_app.zoom_label, 16, L"1.00\u00D7");
    wcscpy_s(g_app.helper_host, 64, ZI_HELPER_HOST);
    wcscpy_s(g_app.helper_line, 192, L"192.168.1.147   Connecting...");
    zi_helper_init();
    for (i = 0; i < ZI_SECTIONS; i++) {
        g_app.section_open[i] = 1;
    }

    ZeroMemory(&wc, sizeof wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = zi_wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"ZImager";
    if (!RegisterClassW(&wc)) {
        return 1;
    }

    dpi = GetDpiForSystem();
    if (dpi == 0) {
        dpi = 96;
    }
    rc.left = 0;
    rc.top = 0;
    rc.right = MulDiv(1440, (int)dpi, 96);
    rc.bottom = MulDiv(900, (int)dpi, 96);
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);

    hwnd = CreateWindowExW(0, L"ZImager", L"ZImager", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top,
                            NULL, zi_menu_bar(), instance, NULL);
    if (!hwnd) {
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    accel = zi_accelerators();
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyAcceleratorTable(accel);
    return (int)msg.wParam;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int code;

    (void)previous;
    (void)command;
    (void)show;
    code = zi_run(instance);
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
    return code;
}
