/* The one-window chrome from ZImager/UI/Chrome.swift. */
#include "chrome.h"
#include "editor.h"
#include "grid.h"
#include "library.h"
#include "tone.h"

#include <stdio.h>
#include <string.h>

static const wchar_t *const k_sections[ZI_SECTIONS] = {
    L"Spectrum", L"Tone", L"Color", L"Geometry", L"Touchup", L"History"
};

static const wchar_t *const k_library[3] = {
    L"Photos", L"Review", L"Duplicates"
};

static int zi_clamp_sidebar(int proposed, int available) {
    int room = available - 240;
    int cap;

    if (room < ZI_SIDEBAR_MIN) {
        room = ZI_SIDEBAR_MIN;
    }
    cap = room < ZI_SIDEBAR_MAX ? room : ZI_SIDEBAR_MAX;
    if (proposed < ZI_SIDEBAR_MIN) {
        proposed = ZI_SIDEBAR_MIN;
    }
    if (proposed > cap) {
        proposed = cap;
    }
    return proposed;
}

static void zi_add(ZiApp *app, int id, int extra, int enabled, int selected, RECT rc, const wchar_t *label) {
    ZiCtrl *ctrl;

    if (app->ctrl_count >= ZI_CTRL_CAP) {
        return;
    }
    ctrl = &app->ctrls[app->ctrl_count++];
    ctrl->id = id;
    ctrl->extra = extra;
    ctrl->enabled = enabled;
    ctrl->selected = selected;
    ctrl->rc = rc;
    ctrl->label = label;
}

static int zi_text_width(HDC hdc, HFONT font, const wchar_t *text) {
    SIZE size;
    HFONT old = (HFONT)SelectObject(hdc, font);

    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &size);
    SelectObject(hdc, old);
    return size.cx;
}

static RECT zi_row(int x, int y, int w, int h) {
    RECT rc;

    rc.left = x;
    rc.top = y;
    rc.right = x + w;
    rc.bottom = y + h;
    return rc;
}

static int zi_button_width(ZiApp *app, HDC hdc, const wchar_t *label) {
    return zi_text_width(hdc, app->font, label) + zi_px(app, 16);
}

static void zi_place_button(ZiApp *app, HDC hdc, int *x, int y, int h, int limit, int id, int enabled, int selected, const wchar_t *label) {
    int w = zi_button_width(app, hdc, label);
    int gap = zi_px(app, ZI_GAP);

    if (*x + w > limit) {
        return;
    }
    zi_add(app, id, 0, enabled, selected, zi_row(*x, y, w, h), label);
    *x += w + gap;
}

static const wchar_t *zi_base(const wchar_t *path) {
    const wchar_t *slash = wcsrchr(path, L'\\');

    if (!slash || slash[1] == L'\0') {
        return path;
    }
    return slash + 1;
}

static void zi_layout_toolbar(ZiApp *app, HDC hdc) {
    int y = app->regions.toolbar.top + zi_px(app, 10);
    int h = zi_px(app, 26);
    int x = app->regions.toolbar.left + zi_px(app, ZI_PAD);
    int source_x;
    int right_x;
    int showing_image = !zi_showing_folder(app);
    const wchar_t *left_labels[2];
    int left_ids[2];
    int left_count = 0;
    int i;

    zi_add(app, ZI_ID_MENU, 0, 1, 0, zi_row(x, y, zi_px(app, 28), h), NULL);
    x += zi_px(app, 28) + zi_px(app, 12);

    if (!app->source_server) {
        left_labels[0] = L"Open File";
        left_labels[1] = L"Open Folder";
        left_ids[0] = ZI_ID_OPEN_FILE;
        left_ids[1] = ZI_ID_OPEN_FOLDER;
        left_count = 2;
    }
    for (i = 0; i < left_count; i++) {
        zi_place_button(app, hdc, &x, y, h, app->regions.toolbar.right, left_ids[i], 1, 0, left_labels[i]);
    }

    source_x = app->regions.center.left + zi_px(app, 20);
    if (source_x < x) {
        source_x = x;
    }
    {
        int label_w = zi_text_width(hdc, app->font, L"Source");
        RECT label = zi_row(source_x, y, label_w, h);
        int seg_w = zi_px(app, 180);
        int half;

        zi_add(app, ZI_ID_NONE, 0, 0, 0, label, L"Source");
        source_x += label_w + zi_px(app, 12);
        half = seg_w / 2;
        zi_add(app, ZI_ID_SOURCE_DISK, 0, 1, !app->source_server,
               zi_row(source_x, y, half, h), L"Disk");
        zi_add(app, ZI_ID_SOURCE_SERVER, 0, 1, app->source_server,
               zi_row(source_x + half, y, seg_w - half, h), L"Server");
        source_x += seg_w;
    }

    if (!app->source_server || showing_image) {
        const wchar_t *names[] = { L"Undo", L"Redo", L"Copy settings", L"Paste settings" };
        int ids[] = { ZI_ID_UNDO, ZI_ID_REDO, ZI_ID_COPY, ZI_ID_PASTE };
        int widths[4];
        int total = 0;

        for (i = 0; i < 4; i++) {
            widths[i] = zi_button_width(app, hdc, names[i]);
            total += widths[i];
        }
        total += zi_px(app, ZI_GAP) * 3;
        right_x = app->regions.toolbar.right - zi_px(app, ZI_PAD) - total;
        if (app->source_server && app->pass_line[0] && source_x + zi_px(app, 24) < right_x) {
            zi_add(app, ZI_ID_NONE, 2, 0, 0,
                   zi_row(source_x + zi_px(app, 16), y, right_x - source_x - zi_px(app, 28), h),
                   app->pass_line);
        }
        {
            int enabled[4];

            enabled[0] = zi_editor_can_undo(app);
            enabled[1] = zi_editor_can_redo(app);
            enabled[2] = zi_editor_can_copy(app);
            enabled[3] = zi_editor_can_paste(app);
            for (i = 0; i < 4; i++) {
                zi_add(app, ids[i], 0, enabled[i], 0, zi_row(right_x, y, widths[i], h), names[i]);
                right_x += widths[i] + zi_px(app, ZI_GAP);
            }
        }
    } else {
        const wchar_t *names[] = { L"Events", L"Reorganize" };
        int ids[] = { ZI_ID_EVENTS, ZI_ID_REORG };
        int widths[2];
        int total = 0;

        for (i = 0; i < 2; i++) {
            widths[i] = zi_button_width(app, hdc, names[i]);
            total += widths[i];
        }
        total += zi_px(app, ZI_GAP);
        right_x = app->regions.toolbar.right - zi_px(app, ZI_PAD) - total;
        if (app->pass_line[0] && source_x + zi_px(app, 24) < right_x) {
            zi_add(app, ZI_ID_NONE, 2, 0, 0,
                   zi_row(source_x + zi_px(app, 16), y, right_x - source_x - zi_px(app, 28), h),
                   app->pass_line);
        }
        for (i = 0; i < 2; i++) {
            int selected = (ids[i] == ZI_ID_EVENTS && app->events_open)
                || (ids[i] == ZI_ID_REORG && app->reorg_open);

            zi_add(app, ids[i], 0, app->source_server, selected, zi_row(right_x, y, widths[i], h), names[i]);
            right_x += widths[i] + zi_px(app, ZI_GAP);
        }
    }
}

static void zi_layout_viewbar(ZiApp *app, HDC hdc) {
    RECT bar = app->regions.viewbar;
    int y = bar.top + zi_px(app, 8);
    int h = zi_px(app, 26);
    int x = bar.left + zi_px(app, ZI_PAD);
    int folder = zi_showing_folder(app);
    int right;
    int limit = bar.right - zi_px(app, ZI_PAD);
    int seg_w = zi_px(app, 160);
    int half = seg_w / 2;

    if (!app->source_server) {
        int box = zi_px(app, 14);
        int label_w = zi_text_width(hdc, app->font, L"Sub-folder");
        RECT rc = zi_row(x, y, label_w + zi_px(app, 8) + box, h);

        zi_add(app, ZI_ID_SUBFOLDER, 0, 1, app->subfolders, rc, L"Sub-folder");
        x = rc.right + zi_px(app, 12);
    }

    zi_add(app, ZI_ID_VIEW_FOLDER, 0, 1, folder, zi_row(x, y, half, h), L"Folder");
    zi_add(app, ZI_ID_VIEW_IMAGE, 0, 1, !folder, zi_row(x + half, y, seg_w - half, h), L"Image");
    x += seg_w + zi_px(app, 18);

    {
        int can_turn = zi_editor_can_rotate(app);
        int can_out = zi_editor_can_export(app);

        zi_place_button(app, hdc, &x, y, h, limit, ZI_ID_ROT_LEFT, can_turn, 0, L"Rotate left");
        zi_place_button(app, hdc, &x, y, h, limit, ZI_ID_ROT_RIGHT, can_turn, 0, L"Rotate right");
        if (folder) {
            zi_place_button(app, hdc, &x, y, h, limit, ZI_ID_FOLDER_PASTE, zi_editor_can_paste(app), 0, L"Paste settings");
            zi_place_button(app, hdc, &x, y, h, limit, ZI_ID_FOLDER_EXPORT, can_out, 0, L"Export");
            zi_place_button(app, hdc, &x, y, h, limit, ZI_ID_FOLDER_PRINT, can_out, 0, L"Print");
        } else {
            const wchar_t *names[] = { L"Select", L"Crop", L"Export", L"Print" };
            int ids[] = { ZI_ID_SELECT, ZI_ID_CROP, ZI_ID_EXPORT, ZI_ID_PRINT };
            int enabled[] = { zi_editor_has_photo(app), zi_editor_can_crop(app), can_out, can_out };
            int selected[] = { app->select_tool, 0, 0, 0 };
            int widths[4];
            int total = 0;
            int i;

            for (i = 0; i < 4; i++) {
                widths[i] = zi_button_width(app, hdc, names[i]);
                total += widths[i] + zi_px(app, ZI_GAP);
            }
            right = bar.right - zi_px(app, ZI_PAD) - total + zi_px(app, ZI_GAP);
            if (right < x) {
                right = x;
            }
            for (i = 0; i < 4; i++) {
                zi_add(app, ids[i], 0, enabled[i], selected[i], zi_row(right, y, widths[i], h), names[i]);
                right += widths[i] + zi_px(app, ZI_GAP);
            }
        }
    }
}

static void zi_layout_editorbar(ZiApp *app, HDC hdc) {
    RECT bar = app->regions.editorbar;
    int y = bar.top + zi_px(app, 8);
    int h = zi_px(app, 26);
    int x = bar.left + zi_px(app, ZI_PAD);

    if (bar.bottom <= bar.top) {
        return;
    }
    zi_editor_apply_fit(app);
    zi_editor_format_zoom(app);
    zi_place_button(app, hdc, &x, y, h, bar.right - zi_px(app, 80), ZI_ID_ZOOM, 1, app->zoom_tool, L"Zoom");
    zi_add(app, ZI_ID_ZOOM_TRACK, 0, zi_editor_has_photo(app), 0,
           zi_row(x, y + zi_px(app, 4), zi_px(app, 180), zi_px(app, 18)), NULL);
    zi_add(app, ZI_ID_NONE, 1, 0, 0,
           zi_row(x + zi_px(app, 188), y, zi_px(app, 72), h), app->zoom_label);
}

static void zi_walk_folders(const ZiApp *app, int parent, int *out, int *count, int cap) {
    int i;

    for (i = 0; i < app->folder_count && *count < cap; i++) {
        if (app->folders[i].parent != parent) {
            continue;
        }
        if (parent < 0 && !app->tree_root_open) {
            continue;
        }
        out[(*count)++] = i;
        if (app->folders[i].expanded) {
            zi_walk_folders(app, i, out, count, cap);
        }
    }
}

static HWND g_tree_scroll;
static WNDPROC g_tree_proc;
static int g_tree_vis = 1;
static int g_tree_shown;

static int zi_vscroll_w(const ZiApp *app) {
    UINT dpi = app->dpi ? (UINT)app->dpi : 96;
    int w = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);

    if (w < 8) {
        w = 8;
    }
    return w;
}

static LRESULT CALLBACK zi_tree_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_MOUSEWHEEL) {
        SendMessageW(GetParent(hwnd), WM_MOUSEWHEEL, wparam, lparam);
        return 0;
    }
    return CallWindowProcW(g_tree_proc, hwnd, msg, wparam, lparam);
}

static void zi_move_tree_bar(HWND bar, int x, int y, int w, int h, int show) {
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

static void zi_tree_place(ZiApp *app, int x, int y, int w, int h, int show, int vis, int shown) {
    SCROLLINFO si;
    int limit;

    g_tree_vis = vis > 0 ? vis : 1;
    g_tree_shown = shown > 0 ? shown : 0;
    if (!app->hwnd) {
        return;
    }
    if (!g_tree_scroll) {
        g_tree_scroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | SBS_VERT,
                                         0, 0, 0, 0, app->hwnd, NULL, GetModuleHandleW(NULL), NULL);
        if (g_tree_scroll) {
            g_tree_proc = (WNDPROC)SetWindowLongPtrW(g_tree_scroll, GWLP_WNDPROC, (LONG_PTR)zi_tree_proc);
        }
    }
    if (!g_tree_scroll) {
        return;
    }
    zi_move_tree_bar(g_tree_scroll, x, y, w, h, show);
    if (!show) {
        return;
    }
    limit = g_tree_shown - 1;
    if (limit < 0) {
        limit = 0;
    }
    ZeroMemory(&si, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = limit;
    si.nPage = (UINT)g_tree_vis;
    si.nPos = app->folder_scroll;
    SetScrollInfo(g_tree_scroll, SB_CTL, &si, TRUE);
}

int zi_chrome_on_vscroll(ZiApp *app, HWND bar, WPARAM wparam) {
    SCROLLINFO si;
    int pos;
    int code;
    int page;
    int limit;

    if (!app || bar != g_tree_scroll) {
        return 0;
    }
    code = LOWORD(wparam);
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    GetScrollInfo(bar, SB_CTL, &si);
    page = g_tree_vis > 0 ? g_tree_vis : 1;
    pos = app->folder_scroll;
    switch (code) {
    case SB_LINEUP:
        pos -= 1;
        break;
    case SB_LINEDOWN:
        pos += 1;
        break;
    case SB_PAGEUP:
        pos -= page;
        break;
    case SB_PAGEDOWN:
        pos += page;
        break;
    case SB_TOP:
        pos = 0;
        break;
    case SB_BOTTOM:
        pos = g_tree_shown;
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        pos = si.nTrackPos;
        break;
    default:
        return 1;
    }
    limit = g_tree_shown - page;
    if (limit < 0) {
        limit = 0;
    }
    if (pos < 0) {
        pos = 0;
    }
    if (pos > limit) {
        pos = limit;
    }
    app->folder_scroll = pos;
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
    return 1;
}

static void zi_layout_sidebar(ZiApp *app) {
    RECT side = app->regions.sidebar;
    int row = zi_px(app, ZI_ROW_H);
    int y = side.top;
    int vis;
    int i;
    int first;
    int visible[ZI_MAX_FOLDERS];
    int shown = 0;
    int list_top;
    int list_w;
    int bar_w;
    int show_bar;

    if (app->source_server) {
        for (i = 0; i < 3; i++) {
            zi_add(app, ZI_ID_LIB, i, 1, app->library_section == i,
                   zi_row(side.left, y, side.right - side.left, row), k_library[i]);
            y += row;
        }
        y += zi_px(app, 6);
        if (app->library_scan[0]) {
            zi_add(app, ZI_ID_TREE, -1, 1, app->folder_selected < 0,
                   zi_row(side.left, y, side.right - side.left, row), zi_base(app->library_scan));
            y += row;
        }
    } else if (app->root[0]) {
        zi_add(app, ZI_ID_TREE, -1, 1, app->folder_selected < 0,
               zi_row(side.left, y, side.right - side.left, row), zi_base(app->root));
        y += row;
    }
    list_top = y;
    list_w = side.right - side.left;
    bar_w = zi_vscroll_w(app);
    show_bar = (side.bottom - list_top) >= zi_px(app, 24) && list_w > bar_w + zi_px(app, 48);
    if (show_bar) {
        list_w -= bar_w;
    }
    zi_walk_folders(app, -1, visible, &shown, ZI_MAX_FOLDERS);
    first = app->folder_scroll;
    if (first < 0) {
        first = 0;
    }
    vis = (side.bottom - y) / row;
    if (vis < 1) {
        vis = 1;
    }
    if (first > shown - vis) {
        first = shown - vis;
    }
    if (first < 0) {
        first = 0;
    }
    app->folder_scroll = first;
    for (i = 0; i < vis && first + i < shown; i++) {
        int index = visible[first + i];

        zi_add(app, ZI_ID_TREE, index, 1, app->folder_selected == index,
               zi_row(side.left, y, list_w, row), app->folders[index].name);
        y += row;
    }
    zi_tree_place(app, side.right - bar_w, list_top, bar_w, side.bottom - list_top, show_bar, vis, shown);
}

static void zi_layout_geometry(ZiApp *app, const RECT *column, int body_top, int enabled) {
    int inset = zi_px(app, 16);
    int row_h = zi_px(app, ZI_GEO_ROW);
    int left = column->left + inset;
    int right = column->right - inset;
    int width;
    int cursor = body_top + zi_px(app, ZI_GEO_TOP);
    int button_w = zi_px(app, 92);
    int s;
    int n;
    static const wchar_t *k_corner_title[4] = {
        L"Top left", L"Top right", L"Bottom right", L"Bottom left"
    };
    const wchar_t *hint = app->horizon_have
        ? L"Click the other end of the horizon."
        : L"Two clicks level a horizon.";

    if (right < left + zi_px(app, 40)) {
        right = left + zi_px(app, 40);
    }
    width = right - left;
    if (button_w > width) {
        button_w = width;
    }
    zi_add(app, ZI_ID_HORIZON, 0, enabled, app->horizon_tool,
           zi_row(right - button_w, cursor, button_w, row_h), L"Horizon");
    cursor += row_h + zi_px(app, ZI_GEO_GAP);
    zi_add(app, ZI_ID_GEO_HINT, 0, 1, 0,
           zi_row(left, cursor, width, zi_px(app, ZI_GEO_HINT)), hint);
    cursor += zi_px(app, ZI_GEO_HINT);
    for (s = 0; s < ZI_SLIDER_COUNT; s++) {
        const ZiSliderSpec *spec = zi_slider_spec(s);

        if (spec->section != 3) {
            continue;
        }
        zi_add(app, ZI_ID_SLIDER, s, enabled, 0,
               zi_row(column->left, cursor, column->right - column->left, row_h), spec->label);
        cursor += row_h;
    }
    cursor += zi_px(app, ZI_GEO_GAP);
    zi_add(app, ZI_ID_CORNER_PIN, 0, enabled, app->edit.has_corners,
           zi_row(left, cursor, width, row_h), L"Corner pin");
    cursor += row_h;
    if (app->edit.has_corners) {
        int title_h = zi_px(app, ZI_GEO_TITLE);

        for (n = 0; n < 4; n++) {
            int x_index = ZI_SL_CTL_X + n * 2;

            zi_add(app, ZI_ID_CORNER_TITLE, n, 1, 0,
                   zi_row(left, cursor, width, title_h), k_corner_title[n]);
            cursor += title_h;
            zi_add(app, ZI_ID_SLIDER, x_index, enabled, 0,
                   zi_row(column->left, cursor, column->right - column->left, row_h),
                   zi_slider_spec(x_index)->label);
            cursor += row_h;
            zi_add(app, ZI_ID_SLIDER, x_index + 1, enabled, 0,
                   zi_row(column->left, cursor, column->right - column->left, row_h),
                   zi_slider_spec(x_index + 1)->label);
            cursor += row_h;
        }
    }
    cursor += zi_px(app, ZI_GEO_GAP);
    zi_add(app, ZI_ID_LENS_MENU, 0, enabled, 0,
           zi_row(left, cursor, width, zi_px(app, ZI_GEO_MENU)),
           zi_lens_label(app->edit.lens));
    cursor += zi_px(app, ZI_GEO_MENU);
    zi_add(app, ZI_ID_SLIDER, ZI_SL_K1, enabled, 0,
           zi_row(column->left, cursor, column->right - column->left, row_h), L"k1");
    cursor += row_h;
    zi_add(app, ZI_ID_SLIDER, ZI_SL_K2, enabled, 0,
           zi_row(column->left, cursor, column->right - column->left, row_h), L"k2");
}

static const wchar_t *zi_touch_hint(const ZiApp *app) {
    if (app->spot_tool) {
        return L"Cover a speck with the circle and click it. Click Spot again to leave.";
    }
    if (app->scratch_tool) {
        return L"Drag the circle along a scratch. Click Scratch again to leave.";
    }
    return L"Finds dust and blends it into the surrounding photo.";
}

/* Pixels from the section body top to the hint. Keep this in step with zi_layout_touchup. */
static int zi_touch_hint_top(const ZiApp *app) {
    return zi_px(app, 4)
        + zi_px(app, 26) + zi_px(app, 6)
        + zi_px(app, 26) + zi_px(app, 6)
        + zi_px(app, 28) + zi_px(app, 28) + zi_px(app, 28)
        + zi_px(app, 4);
}

static int zi_dip_for_px(const ZiApp *app, int px) {
    int dpi = app->dpi > 0 ? (int)app->dpi : 96;
    int dip;

    if (px < 1) {
        px = 1;
    }
    dip = MulDiv(px, 96, dpi);
    if (dip < 1) {
        dip = 1;
    }
    while (zi_px(app, dip) < px && dip < 4000) {
        dip++;
    }
    return dip;
}

static int zi_measure_wrap(HDC hdc, HFONT font, const wchar_t *text, int width) {
    RECT rc;
    HFONT old;
    int height;

    if (!hdc || !font || !text || width < 1) {
        return 0;
    }
    rc.left = 0;
    rc.top = 0;
    rc.right = width;
    rc.bottom = 0;
    old = (HFONT)SelectObject(hdc, font);
    height = DrawTextW(hdc, text, -1, &rc,
                        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EXTERNALLEADING | DT_CALCRECT);
    SelectObject(hdc, old);
    if (height < 1) {
        height = 1;
    }
    return height;
}

/* Measures the hint, then records a section body tall enough for one line or two. */
static void zi_measure_touch_body(ZiApp *app, HDC hdc) {
    RECT column = app->regions.right;
    HFONT font = app->font_hint ? app->font_hint : app->font;
    int inset = zi_px(app, 16);
    int width = (column.right - inset) - (column.left + inset);
    int hint_px;
    int total;

    if (width < zi_px(app, 40)) {
        width = zi_px(app, 40);
    }
    hint_px = zi_measure_wrap(hdc, font, zi_touch_hint(app), width);
    if (hint_px < zi_px(app, 14)) {
        hint_px = zi_px(app, 14);
    }
    app->touch_hint_px = hint_px;
    total = zi_touch_hint_top(app) + hint_px + zi_px(app, 8);
    app->touch_body_dip = zi_dip_for_px(app, total);
}

static void zi_layout_touchup(ZiApp *app, const RECT *column, int body_top) {
    int inset = zi_px(app, 16);
    int gap = zi_px(app, 6);
    int row_h = zi_px(app, 26);
    int slider_h = zi_px(app, 28);
    int left = column->left + inset;
    int right = column->right - inset;
    int width;
    int half;
    int cursor = body_top + zi_px(app, 4);
    int ready = app->photo && !app->photo_failed;
    int busy = app->touch_busy;
    int hint_px = app->touch_hint_px > 0 ? app->touch_hint_px : zi_px(app, 32);
    const wchar_t *hint = zi_touch_hint(app);

    if (right < left + zi_px(app, 40)) {
        right = left + zi_px(app, 40);
    }
    width = right - left;
    half = (width - gap) / 2;
    if (half < zi_px(app, 40)) {
        half = zi_px(app, 40);
    }
    zi_add(app, ZI_ID_DUST, 0, ready && !busy, 0, zi_row(left, cursor, half, row_h), L"Dust");
    zi_add(app, ZI_ID_SCRATCHES, 0, ready && !busy, 0,
           zi_row(left + half + gap, cursor, width - half - gap, row_h), L"Scratches");
    cursor += row_h + gap;
    zi_add(app, ZI_ID_SPOT, 0, ready && (!busy || app->spot_tool), app->spot_tool, zi_row(left, cursor, half, row_h), L"Spot");
    zi_add(app, ZI_ID_SCRATCH, 0, ready && !busy, app->scratch_tool,
           zi_row(left + half + gap, cursor, width - half - gap, row_h), L"Scratch");
    cursor += row_h + gap;
    zi_add(app, ZI_ID_TOUCH_SIZE, 0, ready, 0,
           zi_row(column->left, cursor, column->right - column->left, slider_h), L"Size");
    cursor += slider_h;
    zi_add(app, ZI_ID_DUST_AMOUNT, 0, ready && !busy && zi_editor_has_dust(app), 0,
           zi_row(column->left, cursor, column->right - column->left, slider_h), L"Dust");
    cursor += slider_h;
    zi_add(app, ZI_ID_SCRATCH_AMOUNT, 0, ready && !busy && zi_editor_has_scratch(app), 0,
           zi_row(column->left, cursor, column->right - column->left, slider_h), L"Scratches");
    cursor += slider_h + zi_px(app, 4);
    zi_add(app, ZI_ID_TOUCH_HINT, 0, 1, 0, zi_row(left, cursor, width, hint_px), hint);
}

static void zi_layout_inspector(ZiApp *app, HDC hdc) {
    RECT column = app->regions.right;
    int header = zi_px(app, ZI_SECTION_H);
    int view_h = column.bottom - column.top;
    int content = 0;
    int y;
    int i;
    int enabled = app->photo_path[0] && !app->photo_failed;

    if (zi_showing_folder(app)) {
        app->inspector_content = view_h;
        app->inspector_scroll = 0;
        return;
    }
    zi_measure_touch_body(app, hdc);
    for (i = 0; i < ZI_SECTIONS; i++) {
        content += header;
        if (app->section_open[i]) {
            content += zi_px(app, zi_section_body_dip(app, i));
        }
    }
    app->inspector_content = content;
    if (app->inspector_scroll > content - view_h) {
        app->inspector_scroll = content - view_h;
    }
    if (app->inspector_scroll < 0) {
        app->inspector_scroll = 0;
    }
    y = column.top - app->inspector_scroll;
    for (i = 0; i < ZI_SECTIONS; i++) {
        int body;
        int body_top;
        int cursor;
        int s;

        zi_add(app, ZI_ID_SECTION, i, 1, app->section_open[i],
               zi_row(column.left, y, column.right - column.left, header), k_sections[i]);
        if (i == 1) {
            int button_h = zi_px(app, 22);
            int gap = zi_px(app, 6);
            int top = y + (header - button_h) / 2;
            RECT auto_rc;
            RECT reset_rc;

            reset_rc.right = column.right - zi_px(app, 28);
            reset_rc.left = reset_rc.right - zi_px(app, 62);
            reset_rc.top = top;
            reset_rc.bottom = top + button_h;
            auto_rc.right = reset_rc.left - gap;
            auto_rc.left = auto_rc.right - zi_px(app, 54);
            auto_rc.top = top;
            auto_rc.bottom = top + button_h;
            zi_add(app, ZI_ID_AUTO, 0, app->photo && !app->photo_failed, 0, auto_rc, L"Auto");
            zi_add(app, ZI_ID_RESET, 0, zi_editor_can_reset(app), 0, reset_rc, L"Reset");
        }
        y += header;
        if (!app->section_open[i]) {
            continue;
        }
        body = zi_px(app, zi_section_body_dip(app, i));
        body_top = y;
        y = body_top + body;
        if (i == 5 && app->hist_len < 1) {
            continue;
        }
        if (i == 4) {
            zi_layout_touchup(app, &column, body_top);
            continue;
        }
        if (i == 0) {
            RECT well;
            int inset = zi_px(app, 16);

            well.left = column.left + inset;
            well.right = column.right - inset;
            well.top = body_top + zi_px(app, 4);
            well.bottom = well.top + zi_px(app, 104);
            if (well.right < well.left + 8) {
                well.right = well.left + 8;
            }
            zi_add(app, ZI_ID_SPECTRUM, 0, enabled, 0, well, NULL);
            continue;
        }
        if (i == 5) {
            int row_h = zi_px(app, 26);

            cursor = body_top + zi_px(app, 4);
            for (s = 0; s < app->hist_len; s++) {
                zi_add(app, ZI_ID_HISTORY, s, 1, s == app->hist_pos,
                       zi_row(column.left, cursor, column.right - column.left, row_h),
                       app->hist[s].label);
                cursor += row_h;
            }
            continue;
        }
        if (i == 3) {
            zi_layout_geometry(app, &column, body_top, enabled);
            continue;
        }
        cursor = body_top + zi_px(app, 4);
        for (s = 0; s < ZI_SLIDER_COUNT; s++) {
            const ZiSliderSpec *spec = zi_slider_spec(s);
            int row_h = zi_px(app, 28);

            if (spec->section != i) {
                continue;
            }
            zi_add(app, ZI_ID_SLIDER, s, enabled, 0,
                   zi_row(column.left, cursor, column.right - column.left, row_h), spec->label);
            cursor += row_h;
        }
        if (i == 2) {
            int inset = zi_px(app, 16);
            int gap = zi_px(app, ZI_COLOR_MODE_GAP);
            int row_h = zi_px(app, ZI_COLOR_MODE_H);
            int drop_w = zi_px(app, 108);
            RECT row;
            RECT drop;
            RECT seg;
            int half;

            cursor += gap;
            row.left = column.left + inset;
            row.right = column.right - inset;
            row.top = cursor;
            row.bottom = cursor + row_h;
            if (row.right < row.left + zi_px(app, 40)) {
                row.right = row.left + zi_px(app, 40);
            }
            drop = row;
            drop.left = row.right - drop_w;
            if (drop.left < row.left + zi_px(app, 96)) {
                drop.left = row.left + zi_px(app, 96);
            }
            seg = row;
            seg.right = drop.left - zi_px(app, 8);
            half = (seg.right - seg.left) / 2;
            zi_add(app, ZI_ID_COLOR_WHITE, 0, 1, !app->eyedropper_skin,
                   zi_row(seg.left, seg.top, half, row_h), L"White");
            zi_add(app, ZI_ID_COLOR_SKIN, 0, 1, app->eyedropper_skin,
                   zi_row(seg.left + half, seg.top, seg.right - seg.left - half, row_h), L"Skin");
            zi_add(app, ZI_ID_EYEDROPPER, 0, enabled, app->eyedropper_tool, drop, L"Eyedropper");
            cursor += row_h;
            if (app->eyedropper_skin) {
                int menu_h = zi_px(app, ZI_COLOR_MENU_H);
                int swatch = zi_px(app, ZI_COLOR_SWATCH);
                int swatch_gap = zi_px(app, ZI_COLOR_SWATCH_GAP);
                int n;
                const wchar_t *preset = zi_skin_label(app->skin_preset);

                cursor += zi_px(app, ZI_COLOR_MENU_GAP);
                zi_add(app, ZI_ID_SKIN_MENU, 0, 1, 0,
                       zi_row(row.left, cursor, row.right - row.left, menu_h), preset);
                cursor += menu_h + zi_px(app, ZI_COLOR_SWATCH_GAP);
                for (n = 0; n < zi_skin_count(); n++) {
                    int left = row.left + n * (swatch + swatch_gap);

                    if (left + swatch > row.right) {
                        break;
                    }
                    zi_add(app, ZI_ID_SWATCH, n, 1, n == app->skin_preset,
                           zi_row(left, cursor, swatch, swatch), zi_skin_label(n));
                }
                cursor += zi_px(app, ZI_COLOR_SWATCH_H);
            }
            cursor += zi_px(app, ZI_COLOR_HINT_GAP);
            zi_add(app, ZI_ID_COLOR_HINT, 0, 1, 0,
                   zi_row(row.left, cursor, row.right - row.left, zi_px(app, ZI_COLOR_HINT_H)),
                   L"Click the image to sample.");
        }
    }
}

void zi_chrome_layout(ZiApp *app, HDC hdc, const RECT *client) {
    int toolbar = zi_px(app, ZI_TOOLBAR_H);
    int viewbar = zi_px(app, ZI_VIEWBAR_H);
    int editor = 0;
    int split = zi_px(app, ZI_SPLIT_W);
    int width = client->right - client->left;
    int available = MulDiv(width, 96, (int)app->dpi);
    int side_dip = app->source_server ? app->library_width : app->tree_width;
    int side;
    int right_dip = zi_showing_folder(app) ? ZI_DETAILS_W : ZI_INSPECTOR_W;
    int right;
    int top;

    side_dip = zi_clamp_sidebar(side_dip, available);
    if (app->source_server) {
        app->library_width = side_dip;
    } else {
        app->tree_width = side_dip;
    }
    side = zi_px(app, side_dip);
    right = zi_px(app, right_dip);
    if (!zi_showing_folder(app)) {
        editor = viewbar;
    }
    if (side + split + right > width - zi_px(app, 80)) {
        right = width - side - split - zi_px(app, 80);
        if (right < zi_px(app, 160)) {
            right = zi_px(app, 160);
        }
    }

    app->ctrl_count = 0;
    top = client->top + toolbar;
    app->regions.toolbar = zi_row(client->left, client->top, width, toolbar);
    app->regions.right = zi_row(client->right - right, top, right, client->bottom - top);
    app->regions.sidebar = zi_row(client->left, top, side, client->bottom - top);
    app->regions.split = zi_row(client->left + side, top, split, client->bottom - top);
    app->regions.center = zi_row(app->regions.split.right, top,
                                  app->regions.right.left - app->regions.split.right,
                                  client->bottom - top);
    app->regions.viewbar = zi_row(app->regions.center.left, app->regions.center.top,
                                   app->regions.center.right - app->regions.center.left, viewbar);
    app->regions.editorbar = zi_row(app->regions.center.left, app->regions.viewbar.bottom,
                                     app->regions.center.right - app->regions.center.left, editor);
    app->regions.canvas = zi_row(app->regions.center.left, app->regions.editorbar.bottom,
                                  app->regions.center.right - app->regions.center.left,
                                  app->regions.center.bottom - app->regions.editorbar.bottom);

    zi_add(app, ZI_ID_SPLIT, 0, 1, 0, app->regions.split, NULL);
    zi_layout_toolbar(app, hdc);
    zi_layout_viewbar(app, hdc);
    zi_layout_editorbar(app, hdc);
    zi_layout_sidebar(app);
    zi_layout_inspector(app, hdc);
    zi_library_layout(app);
    zi_grid_sync_bar(app);
}

static void zi_text(HDC hdc, HFONT font, COLORREF color, RECT rc, UINT format, const wchar_t *text) {
    HFONT old = (HFONT)SelectObject(hdc, font);

    SetTextColor(hdc, color);
    DrawTextW(hdc, text, -1, &rc, format | DT_NOPREFIX);
    SelectObject(hdc, old);
}

static void zi_paint_button(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    HBRUSH fill = ctrl->selected ? app->select_brush : app->button_brush;
    HPEN pen = app->muted_pen;
    HGDIOBJ old_brush = SelectObject(hdc, fill);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    int radius = zi_px(app, 8);
    RECT text = ctrl->rc;

    RoundRect(hdc, ctrl->rc.left, ctrl->rc.top, ctrl->rc.right, ctrl->rc.bottom, radius, radius);
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
    if (ctrl->label) {
        InflateRect(&text, -zi_px(app, 4), 0);
        zi_text(hdc, app->font, ctrl->enabled ? ZI_INK : ZI_MUTED, text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, ctrl->label);
    }
}

static void zi_paint_menu_button(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    int mid_y = (ctrl->rc.top + ctrl->rc.bottom) / 2;
    int left = ctrl->rc.left + zi_px(app, 7);
    int right = ctrl->rc.right - zi_px(app, 7);
    int gap = zi_px(app, 4);
    HGDIOBJ old;
    int i;

    zi_paint_button(app, hdc, ctrl);
    old = SelectObject(hdc, app->ink_pen);
    for (i = -1; i <= 1; i++) {
        int y = mid_y + i * gap;
        MoveToEx(hdc, left, y, NULL);
        LineTo(hdc, right, y);
    }
    SelectObject(hdc, old);
}

static void zi_paint_checkbox(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    int box = zi_px(app, 14);
    RECT mark = ctrl->rc;
    RECT label = ctrl->rc;
    HGDIOBJ old_brush;
    HGDIOBJ old_pen;

    label.right -= box + zi_px(app, 6);
    zi_text(hdc, app->font, ZI_INK, label, DT_LEFT | DT_VCENTER | DT_SINGLELINE, ctrl->label);
    mark.left = ctrl->rc.right - box;
    mark.right = ctrl->rc.right;
    mark.top = ctrl->rc.top + (ctrl->rc.bottom - ctrl->rc.top - box) / 2;
    mark.bottom = mark.top + box;
    old_brush = SelectObject(hdc, app->white);
    old_pen = SelectObject(hdc, app->ink_pen);
    Rectangle(hdc, mark.left, mark.top, mark.right, mark.bottom);
    if (ctrl->selected) {
        SelectObject(hdc, app->tool_pen);
        MoveToEx(hdc, mark.left + zi_px(app, 3), mark.top + box / 2, NULL);
        LineTo(hdc, mark.left + box / 2 - 1, mark.bottom - zi_px(app, 3));
        LineTo(hdc, mark.right - zi_px(app, 3), mark.top + zi_px(app, 3));
    }
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
}

static void zi_paint_zoom_track(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    int y = (ctrl->rc.top + ctrl->rc.bottom) / 2;
    int span = (ctrl->rc.right - ctrl->rc.left) - zi_px(app, 12);
    int thumb;
    double t = zi_editor_zoom_pos(app);
    HGDIOBJ old = SelectObject(hdc, app->muted_pen);

    if (span < 1) {
        span = 1;
    }
    if (t < 0) {
        t = 0;
    }
    if (t > 1) {
        t = 1;
    }
    thumb = ctrl->rc.left + zi_px(app, 6) + (int)(t * span);
    MoveToEx(hdc, ctrl->rc.left, y, NULL);
    LineTo(hdc, ctrl->rc.right, y);
    SelectObject(hdc, ctrl->enabled ? app->tool_pen : app->muted_pen);
    Ellipse(hdc, thumb - zi_px(app, 5), y - zi_px(app, 5), thumb + zi_px(app, 5), y + zi_px(app, 5));
    SelectObject(hdc, old);
}

static void zi_center_message(const ZiApp *app, wchar_t *buf, size_t cap) {
    const wchar_t *lead = L"No photograph";

    if (app->source_server && app->server_folder) {
        if (app->library_section == 1) {
            lead = app->grid_note[0]
                ? app->grid_note
                : L"No photographs are parked for review.";
        } else if (app->library_section == 2) {
            lead = app->grid_note[0]
                ? app->grid_note
                : L"This list shows copies gathered into Photos/_Duplicates. Find duplicates looks for matches. Start gathers the extra copies into this list.";
        } else if (app->library_scan[0] || app->library_root[0]) {
            const wchar_t *name = app->library_scan[0] ? zi_base(app->library_scan) : zi_base(app->library_root);

            if (app->folder_selected >= 0 && app->folder_selected < app->folder_count) {
                name = app->folders[app->folder_selected].name;
            }
            swprintf_s(buf, cap, L"%s\nStills and videos are not listed yet.", name);
            lead = NULL;
        } else if (app->helper_online) {
            swprintf_s(buf, cap,
                       L"DS423 is online at 192.168.1.147.\nOpen \\\\192.168.1.147\\BackupData\\PhotoVideoAudio to list photographs.");
            lead = NULL;
        } else {
            lead = app->helper_line[0] ? app->helper_line : L"Connecting to 192.168.1.147.";
        }
    } else if (!app->source_server && app->disk_folder) {
        if (app->root[0]) {
            const wchar_t *name = (app->folder_selected >= 0 && app->folder_selected < app->folder_count)
                ? app->folders[app->folder_selected].name
                : zi_base(app->root);
            swprintf_s(buf, cap, L"%s\nStills and videos are not listed yet.", name);
            lead = NULL;
        } else {
            lead = L"Open a folder to list its stills and videos.";
        }
    } else if (app->opened[0]) {
        lead = zi_base(app->opened);
    }

    if (lead) {
        wcscpy_s(buf, cap, lead);
    }
    if (app->notice[0]) {
        size_t used = wcslen(buf);
        if (used + 2 < cap) {
            buf[used] = L'\n';
            buf[used + 1] = L'\0';
            wcsncat_s(buf, cap, app->notice, _TRUNCATE);
        }
    }
}

static void zi_paint_message(ZiApp *app, HDC hdc, RECT bounds, COLORREF color) {
    wchar_t buf[640];
    RECT measure;
    HFONT old;
    TEXTMETRICW tm;
    int height;
    int pad = zi_px(app, 24);
    int width = (bounds.right - bounds.left) - pad * 2;
    UINT format = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EXTERNALLEADING;

    zi_center_message(app, buf, sizeof buf / sizeof buf[0]);
    if (width < zi_px(app, 120)) {
        pad = zi_px(app, 8);
        width = (bounds.right - bounds.left) - pad * 2;
    }
    if (width < 1) {
        pad = 0;
        width = bounds.right - bounds.left;
    }
    old = (HFONT)SelectObject(hdc, app->font);
    GetTextMetricsW(hdc, &tm);
    measure.left = bounds.left + pad;
    measure.right = measure.left + width;
    measure.top = bounds.top;
    measure.bottom = bounds.bottom;
    height = DrawTextW(hdc, buf, -1, &measure, format | DT_CALCRECT);
    if (height < tm.tmHeight) {
        height = tm.tmHeight;
    }
    /* DrawText clips a descender that lands on the measured bottom. */
    height += tm.tmDescent;
    measure.left = bounds.left + pad;
    measure.right = measure.left + width;
    if (height >= bounds.bottom - bounds.top - zi_px(app, 16)) {
        measure.top = bounds.top + zi_px(app, 8);
        measure.bottom = bounds.bottom - zi_px(app, 8);
    } else {
        measure.top = bounds.top + ((bounds.bottom - bounds.top) - height) / 2;
        measure.bottom = measure.top + height;
    }
    SetTextColor(hdc, color);
    DrawTextW(hdc, buf, -1, &measure, format);
    SelectObject(hdc, old);
}

static void zi_paint_chevron(ZiApp *app, HDC hdc, int x, int mid, int open) {
    int s = zi_px(app, 4);
    HGDIOBJ old = SelectObject(hdc, app->muted_pen);

    if (open) {
        MoveToEx(hdc, x, mid - s / 2, NULL);
        LineTo(hdc, x + s, mid - s / 2);
        LineTo(hdc, x + s / 2, mid + s / 2);
        LineTo(hdc, x, mid - s / 2);
    } else {
        MoveToEx(hdc, x, mid - s, NULL);
        LineTo(hdc, x + s, mid);
        LineTo(hdc, x, mid + s);
        LineTo(hdc, x, mid - s);
    }
    SelectObject(hdc, old);
}

static void zi_paint_sidebar(ZiApp *app, HDC hdc) {
    int i;
    int rule = 0;

    FillRect(hdc, &app->regions.sidebar, app->list_brush);
    for (i = 0; i < app->ctrl_count; i++) {
        ZiCtrl *ctrl = &app->ctrls[i];
        RECT text;
        HBRUSH fill;
        int depth = 0;

        if (ctrl->id != ZI_ID_TREE && ctrl->id != ZI_ID_LIB) {
            continue;
        }
        if (ctrl->rc.bottom < app->regions.sidebar.top || ctrl->rc.top > app->regions.sidebar.bottom) {
            continue;
        }
        if (app->source_server && ctrl->id == ZI_ID_TREE && !rule) {
            HGDIOBJ old = SelectObject(hdc, app->hair);
            int y = ctrl->rc.top - zi_px(app, 3);

            MoveToEx(hdc, app->regions.sidebar.left + zi_px(app, 8), y, NULL);
            LineTo(hdc, app->regions.sidebar.right - zi_px(app, 8), y);
            SelectObject(hdc, old);
            rule = 1;
        }
        fill = ctrl->selected ? app->list_on : app->list_brush;
        FillRect(hdc, &ctrl->rc, fill);
        if (ctrl->id == ZI_ID_TREE && ctrl->extra >= 0) {
            depth = app->folders[ctrl->extra].depth + 1;
        }
        text = ctrl->rc;
        text.left += zi_px(app, 8) + zi_px(app, 14) * depth;
        text.right -= zi_px(app, 8);
        if (ctrl->id == ZI_ID_TREE && zi_folder_has_chevron(app, ctrl->extra)) {
            int open = ctrl->extra < 0 ? app->tree_root_open : app->folders[ctrl->extra].expanded;
            int mid = (ctrl->rc.top + ctrl->rc.bottom) / 2;

            zi_paint_chevron(app, hdc, text.left, mid, open);
        }
        if (ctrl->id == ZI_ID_TREE) {
            text.left += zi_px(app, 16);
        }
        zi_text(hdc, app->font, ZI_INK, text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, ctrl->label);
    }
    if (!app->source_server && !app->root[0]) {
        RECT text = app->regions.sidebar;

        InflateRect(&text, -zi_px(app, 12), -zi_px(app, 12));
        if (g_tree_scroll && IsWindowVisible(g_tree_scroll)) {
            text.right -= zi_vscroll_w(app);
        }
        zi_text(hdc, app->font, ZI_MUTED, text, DT_LEFT | DT_TOP | DT_WORDBREAK,
                L"Open a folder to see its subfolders.");
    }
}

static void zi_paint_touch_slider(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    RECT label;
    RECT track;
    RECT value;
    wchar_t text[32];
    double t = 0;
    int thumb;
    int mid;
    COLORREF color = ctrl->enabled ? ZI_INK : ZI_MUTED;
    HGDIOBJ old_brush;
    HGDIOBJ old_pen;

    zi_slider_geometry(app, &ctrl->rc, &label, &track, &value);
    zi_text(hdc, app->font, color, label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, ctrl->label);
    if (ctrl->id == ZI_ID_TOUCH_SIZE) {
        swprintf_s(text, 32, L"%d px", app->touch_size >= 8 && app->touch_size <= 400 ? app->touch_size : 48);
        t = ((app->touch_size >= 8 && app->touch_size <= 400 ? app->touch_size : 48) - 8) / 392.0;
    } else if (ctrl->id == ZI_ID_SCRATCH_AMOUNT) {
        t = zi_editor_scratch_strength(app);
        swprintf_s(text, 32, L"%d%%", (int)(t * 100.0 + 0.5));
    } else {
        t = zi_editor_dust_strength(app);
        swprintf_s(text, 32, L"%d%%", (int)(t * 100.0 + 0.5));
    }
    if (t < 0) {
        t = 0;
    }
    if (t > 1) {
        t = 1;
    }
    zi_text(hdc, app->font, color, value, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, text);
    mid = (track.top + track.bottom) / 2;
    old_pen = SelectObject(hdc, app->muted_pen);
    MoveToEx(hdc, track.left, mid, NULL);
    LineTo(hdc, track.right, mid);
    thumb = track.left + (int)(t * (double)(track.right - track.left));
    old_brush = SelectObject(hdc, ctrl->enabled ? app->select_brush : app->button_brush);
    SelectObject(hdc, ctrl->enabled ? app->tool_pen : app->muted_pen);
    Ellipse(hdc, thumb - zi_px(app, 5), mid - zi_px(app, 5), thumb + zi_px(app, 5), mid + zi_px(app, 5));
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
}

static void zi_paint_slider(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    RECT label;
    RECT track;
    RECT value;
    wchar_t text[32];
    const ZiSliderSpec *spec = zi_slider_spec(ctrl->extra);
    double span;
    double t = 0;
    int thumb;
    int mid;
    COLORREF color = ctrl->enabled ? ZI_INK : ZI_MUTED;
    HGDIOBJ old_brush;
    HGDIOBJ old_pen;

    zi_slider_geometry(app, &ctrl->rc, &label, &track, &value);
    zi_text(hdc, app->font, color, label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, ctrl->label);
    zi_slider_format(&app->edit, ctrl->extra, text, 32);
    zi_text(hdc, app->font, color, value, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, text);
    mid = (track.top + track.bottom) / 2;
    old_pen = SelectObject(hdc, app->muted_pen);
    MoveToEx(hdc, track.left, mid, NULL);
    LineTo(hdc, track.right, mid);
    span = spec->max_value - spec->min_value;
    if (span > 0) {
        t = (zi_slider_get(&app->edit, ctrl->extra) - spec->min_value) / span;
    }
    if (t < 0) {
        t = 0;
    }
    if (t > 1) {
        t = 1;
    }
    thumb = track.left + (int)(t * (double)(track.right - track.left));
    old_brush = SelectObject(hdc, ctrl->enabled ? app->select_brush : app->button_brush);
    SelectObject(hdc, ctrl->enabled ? app->tool_pen : app->muted_pen);
    Ellipse(hdc, thumb - zi_px(app, 5), mid - zi_px(app, 5), thumb + zi_px(app, 5), mid + zi_px(app, 5));
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
}

static void zi_paint_history_row(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    RECT text = ctrl->rc;
    COLORREF color = ZI_INK;

    if (ctrl->selected) {
        FillRect(hdc, &ctrl->rc, app->select_brush);
    }
    if (ctrl->extra > app->hist_pos) {
        color = ZI_MUTED;
    }
    text.left += zi_px(app, 16);
    text.right -= zi_px(app, 12);
    zi_text(hdc, app->font, color, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, ctrl->label);
}

static int zi_spectrum_graph(const ZiApp *app, const RECT *frame, RECT *graph) {
    int height = frame->bottom - frame->top;
    int graph_h = height - zi_px(app, 16);
    int min_h = zi_px(app, 48);

    if (graph_h < min_h) {
        graph_h = min_h;
    }
    if (graph_h > height) {
        graph_h = height;
    }
    if (graph_h < 1) {
        graph_h = 1;
    }
    *graph = *frame;
    graph->bottom = frame->top + graph_h;
    return graph->right - graph->left;
}

static int zi_spectrum_x(const RECT *graph, int width, double position) {
    if (position < 0) {
        position = 0;
    }
    if (position > 1) {
        position = 1;
    }
    if (width < 1) {
        width = 1;
    }
    return graph->left + (int)(position * (double)width);
}

static void zi_spectrum_shade(HDC hdc, HDC src, int left, int top, int right, int bottom) {
    BLENDFUNCTION blend;

    if (!src || right <= left || bottom <= top) {
        return;
    }
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 41;
    blend.AlphaFormat = 0;
    AlphaBlend(hdc, left, top, right - left, bottom - top, src, 0, 0, 1, 1, blend);
}

static void zi_paint_spectrum(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    RECT graph;
    POINT shape[258];
    POINT tri[3];
    double positions[3];
    COLORREF fills[3];
    HBRUSH well_brush;
    HBRUSH hist_brush;
    HGDIOBJ old_brush;
    HGDIOBJ old_pen;
    HDC shade_dc;
    HBITMAP shade;
    HGDIOBJ old_shade;
    HRGN clip;
    HPEN base_pen;
    RECT box = { 0, 0, 1, 1 };
    int width;
    int radius;
    int saved;
    int peak;
    int ceiling;
    int i;
    int low_x;
    int high_x;
    int half;
    int drop;

    width = zi_spectrum_graph(app, &ctrl->rc, &graph);
    if (width < 1) {
        width = 1;
    }
    radius = zi_px(app, 8);
    well_brush = CreateSolidBrush(RGB(253, 252, 250));
    if (well_brush) {
        old_brush = SelectObject(hdc, well_brush);
        old_pen = SelectObject(hdc, app->hair);
        RoundRect(hdc, graph.left, graph.top, graph.right, graph.bottom, radius, radius);
        SelectObject(hdc, old_pen);
        SelectObject(hdc, old_brush);
        DeleteObject(well_brush);
    }

    saved = SaveDC(hdc);
    clip = CreateRoundRectRgn(graph.left, graph.top, graph.right + 1, graph.bottom + 1, radius, radius);
    if (clip) {
        ExtSelectClipRgn(hdc, clip, RGN_AND);
        DeleteObject(clip);
    }
    if (app->spectrum_ready && width > 1) {
        peak = 1;
        for (i = 0; i < ZI_SPECTRUM_BINS; i++) {
            if (app->spectrum_bins[i] > peak) {
                peak = app->spectrum_bins[i];
            }
        }
        ceiling = (graph.bottom - graph.top) - zi_px(app, 6);
        if (ceiling < 1) {
            ceiling = 1;
        }
        shape[0].x = graph.left;
        shape[0].y = graph.bottom - 1;
        for (i = 0; i < ZI_SPECTRUM_BINS; i++) {
            shape[i + 1].x = graph.left + (int)((double)i / 255.0 * (double)width);
            shape[i + 1].y = (graph.bottom - 1) -
                (int)((double)app->spectrum_bins[i] / (double)peak * (double)ceiling);
        }
        shape[257].x = graph.right;
        shape[257].y = graph.bottom - 1;
        hist_brush = CreateSolidBrush(RGB(80, 76, 71));
        if (hist_brush) {
            int fill_mode = SetPolyFillMode(hdc, WINDING);

            old_brush = SelectObject(hdc, hist_brush);
            old_pen = SelectObject(hdc, GetStockObject(NULL_PEN));
            Polygon(hdc, shape, 258);
            SelectObject(hdc, old_pen);
            SelectObject(hdc, old_brush);
            SetPolyFillMode(hdc, fill_mode);
            DeleteObject(hist_brush);
        }
    }
    low_x = zi_spectrum_x(&graph, width, app->edit.spectrum_low);
    high_x = zi_spectrum_x(&graph, width, app->edit.spectrum_high);
    shade_dc = CreateCompatibleDC(hdc);
    shade = shade_dc ? CreateCompatibleBitmap(hdc, 1, 1) : NULL;
    if (shade_dc && shade) {
        old_shade = SelectObject(shade_dc, shade);
        FillRect(shade_dc, &box, (HBRUSH)GetStockObject(BLACK_BRUSH));
        zi_spectrum_shade(hdc, shade_dc, graph.left, graph.top, low_x, graph.bottom);
        zi_spectrum_shade(hdc, shade_dc, high_x, graph.top, graph.right, graph.bottom);
        SelectObject(shade_dc, old_shade);
    }
    if (shade) {
        DeleteObject(shade);
    }
    if (shade_dc) {
        DeleteDC(shade_dc);
    }
    base_pen = CreatePen(PS_SOLID, 1, RGB(175, 173, 170));
    if (base_pen) {
        old_pen = SelectObject(hdc, base_pen);
        MoveToEx(hdc, graph.left, graph.bottom - 1, NULL);
        LineTo(hdc, graph.right, graph.bottom - 1);
        SelectObject(hdc, old_pen);
        DeleteObject(base_pen);
    }
    RestoreDC(hdc, saved);

    positions[0] = app->edit.spectrum_low;
    positions[1] = app->edit.spectrum_mid;
    positions[2] = app->edit.spectrum_high;
    fills[0] = ZI_INK;
    fills[1] = ZI_MUTED;
    fills[2] = RGB(255, 255, 255);
    half = zi_px(app, 6);
    drop = zi_px(app, 13);
    for (i = 0; i < 3; i++) {
        HBRUSH brush;
        int x = zi_spectrum_x(&graph, width, positions[i]);
        int tip = graph.bottom - 1;

        tri[0].x = x;
        tri[0].y = tip;
        tri[1].x = x - half;
        tri[1].y = tip + drop;
        tri[2].x = x + half;
        tri[2].y = tip + drop;
        brush = CreateSolidBrush(fills[i]);
        if (!brush) {
            continue;
        }
        old_brush = SelectObject(hdc, brush);
        old_pen = SelectObject(hdc, app->ink_pen);
        Polygon(hdc, tri, 3);
        SelectObject(hdc, old_pen);
        SelectObject(hdc, old_brush);
        DeleteObject(brush);
    }
}

int zi_spectrum_handle_at(const ZiApp *app, int x, int y) {
    const ZiCtrl *ctrl = NULL;
    RECT graph;
    double positions[3];
    int width;
    int axis;
    int pad;
    int best = -1;
    int i;
    long long best_dist = 0;
    POINT pt;

    if (!app || zi_showing_folder(app)) {
        return -1;
    }
    for (i = 0; i < app->ctrl_count; i++) {
        if (app->ctrls[i].id == ZI_ID_SPECTRUM && app->ctrls[i].enabled) {
            ctrl = &app->ctrls[i];
        }
    }
    pt.x = x;
    pt.y = y;
    if (!ctrl || !PtInRect(&ctrl->rc, pt)) {
        return -1;
    }
    width = zi_spectrum_graph(app, &ctrl->rc, &graph);
    axis = graph.bottom + zi_px(app, 5);
    pad = zi_px(app, 20);
    best_dist = (long long)pad * (long long)pad;
    positions[0] = app->edit.spectrum_low;
    positions[1] = app->edit.spectrum_mid;
    positions[2] = app->edit.spectrum_high;
    for (i = 0; i < 3; i++) {
        long long dx = (long long)x - zi_spectrum_x(&graph, width, positions[i]);
        long long dy = (long long)y - axis;
        long long dist = dx * dx + dy * dy;

        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

static void zi_paint_eyedropper(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    HBRUSH fill = ctrl->selected ? app->select_brush : app->button_brush;
    HPEN pen = ctrl->selected ? app->tool_pen : app->muted_pen;
    HGDIOBJ old_brush = SelectObject(hdc, fill);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    int radius = zi_px(app, 8);
    RECT text = ctrl->rc;

    RoundRect(hdc, ctrl->rc.left, ctrl->rc.top, ctrl->rc.right, ctrl->rc.bottom, radius, radius);
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
    if (ctrl->label) {
        InflateRect(&text, -zi_px(app, 4), 0);
        zi_text(hdc, app->font, ctrl->enabled ? ZI_INK : ZI_MUTED, text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, ctrl->label);
    }
}

static void zi_paint_swatch(ZiApp *app, HDC hdc, const ZiCtrl *ctrl) {
    HBRUSH fill = CreateSolidBrush(zi_skin_color(ctrl->extra));
    HPEN owned = NULL;
    HPEN pen;
    HGDIOBJ old_brush;
    HGDIOBJ old_pen;
    int radius = zi_px(app, 4);
    int width = zi_px(app, 2);

    if (!fill) {
        return;
    }
    if (width < 2) {
        width = 2;
    }
    if (ctrl->selected) {
        owned = CreatePen(PS_SOLID, width, ZI_INK);
        pen = owned ? owned : app->ink_pen;
    } else {
        pen = (HPEN)GetStockObject(NULL_PEN);
    }
    old_brush = SelectObject(hdc, fill);
    old_pen = SelectObject(hdc, pen);
    RoundRect(hdc, ctrl->rc.left, ctrl->rc.top, ctrl->rc.right, ctrl->rc.bottom, radius, radius);
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
    if (owned) {
        DeleteObject(owned);
    }
    DeleteObject(fill);
}

static void zi_paint_right(ZiApp *app, HDC hdc) {
    RECT column = app->regions.right;
    int i;

    FillRect(hdc, &column, app->panel);
    if (zi_showing_folder(app)) {
        if (app->source_server && app->library_section == 2 && !app->events_open && !app->reorg_open) {
            zi_library_paint_side(app, hdc);
            return;
        }
        zi_editor_paint_details(app, hdc);
        zi_library_paint_side(app, hdc);
        return;
    }

    IntersectClipRect(hdc, column.left, column.top, column.right, column.bottom);
    for (i = 0; i < app->ctrl_count; i++) {
        ZiCtrl *ctrl = &app->ctrls[i];
        RECT title;
        RECT hint;
        int body_h;

        if (ctrl->id != ZI_ID_SECTION) {
            continue;
        }
        title = ctrl->rc;
        title.left += zi_px(app, ZI_PAD);
        title.right -= zi_px(app, 28);
        if (ctrl->extra == 1) {
            title.right -= zi_px(app, 128);
        }
        zi_text(hdc, app->font_header, ZI_INK, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, ctrl->label);
        {
            int cx = ctrl->rc.right - zi_px(app, 18);
            int cy = (ctrl->rc.top + ctrl->rc.bottom) / 2;
            HGDIOBJ old = SelectObject(hdc, app->ink_pen);
            if (ctrl->selected) {
                MoveToEx(hdc, cx - zi_px(app, 5), cy - zi_px(app, 2), NULL);
                LineTo(hdc, cx, cy + zi_px(app, 3));
                LineTo(hdc, cx + zi_px(app, 5), cy - zi_px(app, 2));
            } else {
                MoveToEx(hdc, cx - zi_px(app, 5), cy + zi_px(app, 2), NULL);
                LineTo(hdc, cx, cy - zi_px(app, 3));
                LineTo(hdc, cx + zi_px(app, 5), cy + zi_px(app, 2));
            }
            SelectObject(hdc, old);
        }
        if (!ctrl->selected || ctrl->extra < 0 || ctrl->extra >= ZI_SECTIONS) {
            continue;
        }
        body_h = zi_px(app, zi_section_body_dip(app, ctrl->extra));
        if (ctrl->extra == 5 && app->hist_len < 1) {
            hint = ctrl->rc;
            hint.top = ctrl->rc.bottom + zi_px(app, 4);
            hint.bottom = ctrl->rc.bottom + body_h;
            hint.left += zi_px(app, ZI_PAD);
            hint.right -= zi_px(app, ZI_PAD);
            zi_text(hdc, app->font, ZI_MUTED, hint, DT_LEFT | DT_TOP | DT_WORDBREAK, L"No edits yet");
        }
        {
            int rule_y = ctrl->rc.bottom + body_h - 1;

            SelectObject(hdc, app->hair);
            MoveToEx(hdc, column.left + zi_px(app, ZI_PAD), rule_y, NULL);
            LineTo(hdc, column.right - zi_px(app, ZI_PAD), rule_y);
        }
    }
    for (i = 0; i < app->ctrl_count; i++) {
        ZiCtrl *row = &app->ctrls[i];

        if (row->id == ZI_ID_SLIDER) {
            zi_paint_slider(app, hdc, row);
        } else if (row->id == ZI_ID_SPECTRUM) {
            zi_paint_spectrum(app, hdc, row);
        } else if (row->id == ZI_ID_HISTORY) {
            zi_paint_history_row(app, hdc, row);
        } else if (row->id == ZI_ID_AUTO || row->id == ZI_ID_RESET ||
                   row->id == ZI_ID_COLOR_WHITE || row->id == ZI_ID_COLOR_SKIN ||
                   row->id == ZI_ID_SKIN_MENU) {
            zi_paint_button(app, hdc, row);
        } else if (row->id == ZI_ID_EYEDROPPER) {
            zi_paint_eyedropper(app, hdc, row);
        } else if (row->id == ZI_ID_SWATCH) {
            zi_paint_swatch(app, hdc, row);
        } else if (row->id == ZI_ID_COLOR_HINT && row->label) {
            zi_text(hdc, app->font, ZI_MUTED, row->rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, row->label);
        } else if (row->id == ZI_ID_GEO_HINT && row->label) {
            zi_text(hdc, app->font, ZI_MUTED, row->rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, row->label);
        } else if (row->id == ZI_ID_CORNER_TITLE && row->label) {
            zi_text(hdc, app->font, ZI_MUTED, row->rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, row->label);
        } else if (row->id == ZI_ID_TOUCH_HINT && row->label) {
            zi_text(hdc, app->font_hint ? app->font_hint : app->font, ZI_MUTED, row->rc,
                    DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EXTERNALLEADING, row->label);
        } else if (row->id == ZI_ID_TOUCH_SIZE || row->id == ZI_ID_DUST_AMOUNT ||
                   row->id == ZI_ID_SCRATCH_AMOUNT) {
            zi_paint_touch_slider(app, hdc, row);
        } else if (row->id == ZI_ID_DUST || row->id == ZI_ID_SCRATCHES ||
                   row->id == ZI_ID_SPOT || row->id == ZI_ID_SCRATCH) {
            zi_paint_button(app, hdc, row);
        } else if (row->id == ZI_ID_HORIZON || row->id == ZI_ID_LENS_MENU) {
            zi_paint_button(app, hdc, row);
        } else if (row->id == ZI_ID_CORNER_PIN) {
            zi_paint_checkbox(app, hdc, row);
        }
    }
}

static void zi_paint_controls(ZiApp *app, HDC hdc) {
    int i;

    for (i = 0; i < app->ctrl_count; i++) {
        ZiCtrl *ctrl = &app->ctrls[i];

        if (ctrl->id == ZI_ID_NONE && ctrl->extra == 2 && ctrl->label) {
            zi_text(hdc, app->font, ZI_INK, ctrl->rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, ctrl->label);
        } else if (ctrl->id == ZI_ID_NONE && ctrl->label && wcscmp(ctrl->label, L"Source") == 0) {
            zi_text(hdc, app->font, ZI_INK, ctrl->rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, ctrl->label);
        } else if (ctrl->id == ZI_ID_NONE && ctrl->extra == 1 && ctrl->label) {
            zi_text(hdc, app->font, ZI_MUTED, ctrl->rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE, ctrl->label);
        } else if ((ctrl->id == ZI_ID_NONE || ctrl->id == ZI_ID_ZOOM_TRACK) &&
                   ctrl->label == NULL && ctrl->rc.right > ctrl->rc.left) {
            zi_paint_zoom_track(app, hdc, ctrl);
        } else if (ctrl->id == ZI_ID_MENU) {
            zi_paint_menu_button(app, hdc, ctrl);
        } else if (ctrl->id == ZI_ID_SUBFOLDER) {
            zi_paint_checkbox(app, hdc, ctrl);
        } else if (ctrl->id == ZI_ID_SOURCE_DISK || ctrl->id == ZI_ID_SOURCE_SERVER ||
                   ctrl->id == ZI_ID_VIEW_FOLDER || ctrl->id == ZI_ID_VIEW_IMAGE ||
                   ctrl->id == ZI_ID_OPEN_FILE || ctrl->id == ZI_ID_OPEN_FOLDER ||
                   ctrl->id == ZI_ID_UNDO || ctrl->id == ZI_ID_REDO ||
                   ctrl->id == ZI_ID_COPY || ctrl->id == ZI_ID_PASTE ||
                   ctrl->id == ZI_ID_EVENTS || ctrl->id == ZI_ID_REORG ||
                   ctrl->id == ZI_ID_ROT_LEFT || ctrl->id == ZI_ID_ROT_RIGHT ||
                   ctrl->id == ZI_ID_FOLDER_PASTE || ctrl->id == ZI_ID_FOLDER_EXPORT ||
                   ctrl->id == ZI_ID_FOLDER_PRINT || ctrl->id == ZI_ID_SELECT ||
                   ctrl->id == ZI_ID_CROP || ctrl->id == ZI_ID_EXPORT ||
                   ctrl->id == ZI_ID_PRINT || ctrl->id == ZI_ID_ZOOM) {
            zi_paint_button(app, hdc, ctrl);
        }
    }
}

void zi_chrome_paint(ZiApp *app, HDC hdc, const RECT *client) {
    HDC buffer = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, client->right - client->left, client->bottom - client->top);
    HGDIOBJ old_bitmap = SelectObject(buffer, bitmap);
    RECT hair;
    int saved;

    zi_chrome_layout(app, buffer, client);
    SetBkMode(buffer, TRANSPARENT);
    FillRect(buffer, client, app->panel);

    FillRect(buffer, &app->regions.toolbar, app->panel);
    hair = app->regions.toolbar;
    hair.top = hair.bottom - 1;
    FillRect(buffer, &hair, app->panel);
    SelectObject(buffer, app->hair);
    MoveToEx(buffer, client->left, app->regions.toolbar.bottom - 1, NULL);
    LineTo(buffer, client->right, app->regions.toolbar.bottom - 1);

    saved = SaveDC(buffer);
    IntersectClipRect(buffer, app->regions.sidebar.left, app->regions.sidebar.top,
                      app->regions.sidebar.right, app->regions.sidebar.bottom);
    zi_paint_sidebar(app, buffer);
    RestoreDC(buffer, saved);

    SelectObject(buffer, app->hair);
    MoveToEx(buffer, app->regions.split.left + (app->regions.split.right - app->regions.split.left) / 2,
             app->regions.split.top, NULL);
    LineTo(buffer, app->regions.split.left + (app->regions.split.right - app->regions.split.left) / 2,
           app->regions.split.bottom);
    MoveToEx(buffer, app->regions.right.left, app->regions.right.top, NULL);
    LineTo(buffer, app->regions.right.left, app->regions.right.bottom);

    FillRect(buffer, &app->regions.viewbar, app->panel);
    if (app->regions.editorbar.bottom > app->regions.editorbar.top) {
        FillRect(buffer, &app->regions.editorbar, app->panel);
        SelectObject(buffer, app->hair);
        MoveToEx(buffer, app->regions.editorbar.left, app->regions.editorbar.bottom - 1, NULL);
        LineTo(buffer, app->regions.editorbar.right, app->regions.editorbar.bottom - 1);
    }

    if (app->events_open || app->reorg_open) {
        zi_library_paint_center(app, buffer);
    } else if (zi_showing_folder(app)) {
        FillRect(buffer, &app->regions.canvas, app->panel);
        if (zi_grid_active(app)) {
            zi_grid_paint(app, buffer);
        } else {
            zi_paint_message(app, buffer, app->regions.canvas, ZI_INK);
        }
    } else {
        FillRect(buffer, &app->regions.canvas, app->canvas_brush);
        if (!zi_editor_paint(app, buffer)) {
            zi_paint_message(app, buffer, app->regions.canvas, ZI_WELL_INK);
        }
    }

    saved = SaveDC(buffer);
    zi_paint_right(app, buffer);
    RestoreDC(buffer, saved);

    zi_paint_controls(app, buffer);
    BitBlt(hdc, 0, 0, client->right - client->left, client->bottom - client->top, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

int zi_chrome_hittest(const ZiApp *app, int x, int y, int *extra) {
    int i;

    if (extra) {
        *extra = 0;
    }
    for (i = app->ctrl_count - 1; i >= 0; i--) {
        const ZiCtrl *ctrl = &app->ctrls[i];

        if (ctrl->id == ZI_ID_NONE) {
            continue;
        }
        if (!ctrl->enabled && ctrl->id != ZI_ID_AUTO && ctrl->id != ZI_ID_RESET) {
            continue;
        }
        if (PtInRect(&ctrl->rc, (POINT){ x, y })) {
            if ((ctrl->id == ZI_ID_SECTION || ctrl->id == ZI_ID_SLIDER ||
                 ctrl->id == ZI_ID_SPECTRUM || ctrl->id == ZI_ID_HISTORY ||
                 ctrl->id == ZI_ID_AUTO || ctrl->id == ZI_ID_RESET ||
                 ctrl->id == ZI_ID_COLOR_WHITE || ctrl->id == ZI_ID_COLOR_SKIN ||
                 ctrl->id == ZI_ID_EYEDROPPER || ctrl->id == ZI_ID_SKIN_MENU ||
                 ctrl->id == ZI_ID_SWATCH || ctrl->id == ZI_ID_COLOR_HINT ||
                 ctrl->id == ZI_ID_HORIZON || ctrl->id == ZI_ID_CORNER_PIN ||
                 ctrl->id == ZI_ID_LENS_MENU || ctrl->id == ZI_ID_GEO_HINT ||
                 ctrl->id == ZI_ID_CORNER_TITLE ||
                 ctrl->id == ZI_ID_DUST || ctrl->id == ZI_ID_SCRATCHES ||
                 ctrl->id == ZI_ID_SPOT || ctrl->id == ZI_ID_SCRATCH ||
                 ctrl->id == ZI_ID_TOUCH_SIZE || ctrl->id == ZI_ID_DUST_AMOUNT ||
                 ctrl->id == ZI_ID_SCRATCH_AMOUNT || ctrl->id == ZI_ID_TOUCH_HINT) &&
                !PtInRect(&app->regions.right, (POINT){ x, y })) {
                continue;
            }
            if (extra) {
                *extra = ctrl->extra;
            }
            return ctrl->id;
        }
    }
    return ZI_ID_NONE;
}
