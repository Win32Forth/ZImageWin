/* Headless checks for the shared 1.8.6 grade math. */
#include "features.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failed;

static void expect(int cond, const char *name) {
    if (!cond) {
        printf("FAIL %s\n", name);
        g_failed++;
    }
}

static int near(double a, double b) {
    double d = a - b;

    if (d < 0) {
        d = -d;
    }
    return d < 0.02;
}

int main(void) {
    ZiCurves curves;
    ZiCurve linear;
    ZiAutoLevels levels;
    ZiAutoGrade grade;
    ZiNormRect crop;
    ZiPageSlice page;
    wchar_t name[128];
    int turns = 0;
    double degrees = 0;
    int flip_h = 1;
    int flip_v = 0;
    int pages[3] = {2, 7, 9};
    double r = 0.2;
    double g = 0.2;
    double b = 0.2;

    zi_curve_linear(&linear);
    expect(near(zi_curve_eval(&linear, 0.25), 0.25), "linear quarter");
    expect(near(zi_curve_eval(&linear, 0.0), 0.0), "linear start");
    expect(near(zi_curve_eval(&linear, 1.0), 1.0), "linear end");
    zi_curve_preset(&curves, 4);
    expect(near(zi_curve_eval(&curves.master, 0.0), 0.14), "lofi start");
    expect(near(zi_curve_eval(&curves.master, 1.0), 0.84), "lofi end");
    expect(zi_curve_eval(&curves.master, 1.5) != zi_curve_eval(&curves.master, 1.0) ||
               zi_curve_eval(&curves.master, 1.5) > 0.8,
           "lofi past end follows a slope");
    expect(zi_curve_matches_preset(&curves, 4), "lofi preset match");
    expect(!zi_curve_is_linear(&curves.master), "lofi is not linear");
    zi_curve_preset(&curves, 0);
    expect(zi_curves_linear(&curves), "linear preset");

    expect(near(zi_vignette_coverage(50, 50, 100, 100, 0.5, 0, 0.5, 0, 0), 0), "vignette center");
    expect(near(zi_vignette_coverage(0, 0, 100, 100, 0.5, 0, 0.5, 0, 0), 1), "vignette corner");
    expect(zi_vignette_shade(0.5, -1, 1) == 0, "vignette dark floor");

    zi_horizon_measure(0, 0, 1, 0, 100, 80, &turns, &degrees);
    expect(turns == 0 && near(degrees, 0), "horizon flat");
    zi_horizon_measure(0, 0, 1, 0.5, 2, 1, &turns, &degrees);
    expect(turns == 0 && near(degrees, 14.036), "horizon scaled");

    flip_h = 1;
    flip_v = 0;
    zi_flip_swap_if_odd(&flip_h, &flip_v, 1);
    expect(flip_h == 0 && flip_v == 1, "odd turn swaps flips");
    zi_flip_swap_if_odd(&flip_h, &flip_v, 2);
    expect(flip_h == 0 && flip_v == 1, "even turn keeps flips");
    crop.x = 0;
    crop.y = 0;
    crop.w = 0.5;
    crop.h = 0.25;
    zi_crop_rotate(&crop, 1);
    expect(near(crop.x, 0.75) && near(crop.y, 0) && near(crop.w, 0.25) && near(crop.h, 0.5),
           "crop quarter turn");

    levels.black = 0.02;
    levels.shadow = 0.10;
    levels.mid = 0.30;
    levels.highlight = 0.70;
    levels.white = 0.90;
    zi_auto_from_levels(&levels, &grade);
    expect(grade.exposure >= -1.5 && grade.exposure <= 3.0, "exposure clamp");
    expect(grade.blacks == 0, "blacks stay when the middle is low");
    levels.black = 0;
    levels.shadow = 0.02;
    levels.mid = 0.1;
    levels.highlight = 0.2;
    levels.white = 0.4;
    zi_auto_from_levels(&levels, &grade);
    expect(grade.contrast == 0, "contrast will not darken the middle");

    expect(zi_name_match(L"Photo.JPG", L"photo") == 1, "name match");
    expect(zi_name_match(L"Photo.JPG", L"raw") == 0, "name miss");
    expect(zi_name_match(L"Photo.JPG", L"") == 1, "empty filter");

    zi_page_slice(0, 500, 0, 0, &page);
    expect(page.still_begin == 0 && page.still_count == 200 && page.include_videos == 1, "page 0");
    zi_page_slice(2, 500, 0, 0, &page);
    expect(page.still_begin == 400 && page.still_count == 100 && page.include_videos == 0, "page 2");
    zi_page_slice(1, 0, 1, 450, &page);
    expect(page.filtered_begin == 200 && page.filtered_count == 200 && page.include_videos == 0,
           "filtered page");

    expect(zi_merge_filename(name, 128, L"C:\\a.jpg", L"b.PNG", 0) && wcscmp(name, L"a-b.tiff") == 0,
           "merge base");
    expect(zi_merge_filename(name, 128, L"a.jpg", L"b.jpg", 1) && wcscmp(name, L"a-b-2.tiff") == 0,
           "merge suffix 2");
    expect(zi_merge_filename(name, 128, L"a.jpg", L"b.jpg", 998) && wcscmp(name, L"a-b-999.tiff") == 0,
           "merge suffix 999");
    expect(zi_merge_filename(name, 128, L"a.jpg", L"b.jpg", 999) == 0, "merge past 999");

    expect(zi_chosen_page(pages, 3, 1) == 7, "clicked page");
    expect(zi_chosen_page(pages, 3, 1) != 5, "page is not a constant 5");
    expect(zi_scratch_spacing(8) == 2, "scratch spacing 8");
    expect(zi_scratch_spacing(3) == 1, "scratch spacing 3");
    expect(!zi_is_pupil_red(0.6, 0.4, 0.35), "skin is not a pupil");
    expect(zi_is_pupil_red(0.5, 0.1, 0.1), "pupil red");
    expect(zi_redeye_reject(4, 10, 10), "redeye small count");
    expect(zi_redeye_reject(20, 40, 10), "redeye long box");
    expect(!zi_redeye_reject(20, 12, 10), "redeye box ok");

    zi_curve_map_linear(&curves, &r, &g, &b);
    expect(near(r, 0.2) && near(g, 0.2) && near(b, 0.2), "linear curve skips the lookup");

    if (g_failed) {
        printf("%d failed\n", g_failed);
        return 1;
    }
    printf("ok\n");
    return 0;
}
