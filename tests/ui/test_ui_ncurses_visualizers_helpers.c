// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Pure-helper checks for RTL ncurses visualizer math and bucketing.
 */

#define USE_RTLSDR    1
#define PRETTY_COLORS 1

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

static char g_render_out[32768];
static size_t g_render_len;
static int g_attron_count;
static int g_attroff_count;
static int g_has_colors;
static int g_test_rows = 24;
static int g_test_cols = 80;

static void
render_reset(void) {
    g_render_len = 0;
    g_render_out[0] = '\0';
    g_attron_count = 0;
    g_attroff_count = 0;
}

static void
render_append(const char* text) {
    size_t n = strlen(text);
    if (n >= sizeof(g_render_out) - g_render_len) {
        n = sizeof(g_render_out) - g_render_len - 1;
    }
    DSD_MEMCPY(g_render_out + g_render_len, text, n);
    g_render_len += n;
    g_render_out[g_render_len] = '\0';
}

static int test_printw(const char* fmt, ...) DSD_ATTR_FORMAT(printf, 1, 2);
static int test_addch(const chtype ch);
static int test_addstr(const char* text);
static int test_attron(int attrs);
static int test_attroff(int attrs);
static int test_has_colors(void);
static int test_init_pair(short pair, short f, short b);

#undef getmaxyx
#undef addch
#undef addstr
#undef attron
#undef attroff
#define getmaxyx(win, y, x)                                                                                            \
    do {                                                                                                               \
        (void)(win);                                                                                                   \
        (y) = g_test_rows;                                                                                             \
        (x) = g_test_cols;                                                                                             \
    } while (0)
#define printw     test_printw
#define addch      test_addch
#define addstr     test_addstr
#define attron     test_attron
#define attroff    test_attroff
#define has_colors test_has_colors
#define init_pair  test_init_pair

#include "../../src/ui/terminal/ncurses_visualizers.c"
#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/io/rtl_stream_c.h"
#include "dsd-neo/ui/ncurses_internal.h"
#include "dsd-neo/ui/ui_prims.h"

#undef init_pair
#undef has_colors
#undef attroff
#undef attron
#undef addstr
#undef addch
#undef printw
#undef getmaxyx

static int
test_printw(const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = DSD_VSNPRINTF(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        render_append(tmp);
    }
    return n;
}

static int
test_addch(const chtype ch) {
    char tmp[2] = {(char)(ch & 0xFF), '\0'};
    render_append(tmp);
    return 0;
}

static int
test_addstr(const char* text) {
    render_append(text ? text : "");
    return 0;
}

static int
test_attron(int attrs) {
    (void)attrs;
    g_attron_count++;
    return 0;
}

static int
test_attroff(int attrs) {
    (void)attrs;
    g_attroff_count++;
    return 0;
}

static int
test_has_colors(void) {
    return g_has_colors;
}

static int
test_init_pair(short pair, short f, short b) {
    (void)pair;
    (void)f;
    (void)b;
    return 0;
}

int
select_k_int_local(int* a, int n, int k) { // NOLINT(misc-use-internal-linkage)
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (a[j] < a[i]) {
                int tmp = a[i];
                a[i] = a[j];
                a[j] = tmp;
            }
        }
    }
    return a[k];
}

int
ui_block_glyphs_supported(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

double
ui_gamma_map01(double f) { // NOLINT(misc-use-internal-linkage)
    if (f < 0.0) {
        return 0.0;
    }
    if (f > 1.0) {
        return 1.0;
    }
    return sqrt(f);
}

void
ui_print_hr(void) { // NOLINT(misc-use-internal-linkage)
}

void
ui_print_header(const char* title) { // NOLINT(misc-use-internal-linkage)
    (void)title;
}

void
ui_print_lborder(void) { // NOLINT(misc-use-internal-linkage)
}

int
rtl_stream_constellation_get(float* out, int max_points) { // NOLINT(misc-use-internal-linkage)
    (void)out;
    (void)max_points;
    return 0;
}

int
rtl_stream_eye_get(float* out, int max_samples, int* out_sps) { // NOLINT(misc-use-internal-linkage)
    (void)out;
    (void)max_samples;
    if (out_sps) {
        *out_sps = 0;
    }
    return 0;
}

double
rtl_stream_get_snr_c4fm(void) { // NOLINT(misc-use-internal-linkage)
    return -100.0;
}

double
rtl_stream_get_snr_bias_c4fm(void) { // NOLINT(misc-use-internal-linkage)
    return 0.0;
}

int
rtl_stream_spectrum_get_size(void) { // NOLINT(misc-use-internal-linkage)
    return 128;
}

int
rtl_stream_spectrum_get(float* out, int max_bins, int* out_rate) { // NOLINT(misc-use-internal-linkage)
    (void)out;
    (void)max_bins;
    if (out_rate) {
        *out_rate = 0;
    }
    return 0;
}

static int
sum_density(const unsigned short* den, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += den[i];
    }
    return sum;
}

static void
test_density_and_constellation_helpers(void) {
    assert(clamp_int_local(-2, 0, 4) == 0);
    assert(clamp_int_local(6, 0, 4) == 4);
    assert(clamp_float_local(-0.5f, 0.0f, 1.0f) == 0.0f);
    assert(clamp_float_local(1.5f, 0.0f, 1.0f) == 1.0f);
    assert(density_palette_index(-1.0, 10) == 0);
    assert(density_palette_index(2.0, 10) == 9);
    assert(density_color_pair_id(40, 7, 1.0) == 46);
    assert(fabs(density_gamma_value(25, 100) - 0.5) < 0.0001);

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    assert(fabs(constellation_gate_squared(&opts) - 0.0) < 0.0001);
    opts.mod_qpsk = 1;
    opts.const_gate_qpsk = 2.0f;
    assert(fabs(constellation_gate_squared(&opts) - 0.81) < 0.0001);
    opts.mod_qpsk = 0;
    opts.const_gate_other = -1.0f;
    assert(fabs(constellation_gate_squared(&opts) - 0.0) < 0.0001);

    constellation_geom geom = constellation_compute_geometry(9, 7);
    assert(geom.W == 9);
    assert(geom.H == 7);
    assert(geom.cx == 4);
    assert(geom.cy == 3);
    assert(geom.x0 >= 0);
    assert(geom.x1 < geom.W);

    constellation_geom tiny_geom = constellation_compute_geometry(2, 1);
    assert(tiny_geom.x1 >= tiny_geom.x0);
    assert(tiny_geom.y1 >= tiny_geom.y0);

    unsigned short den[9 * 7];
    DSD_MEMSET(den, 0, sizeof den);
    const float iq[] = {1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f};
    unsigned short dmax = constellation_accumulate_density(iq, 3, NULL, &geom, 16384, 0.0, den);
    assert(dmax == 1);
    assert(sum_density(den, 9 * 7) == 3);

    const float gated_iq[] = {0.001f, 0.0f, -0.001f, 0.0f};
    DSD_MEMSET(den, 0, sizeof den);
    dmax = constellation_accumulate_density(gated_iq, 2, NULL, &geom, 16384, 0.01, den);
    assert(dmax == 1);
    assert(sum_density(den, 9 * 7) == 0);

    const float zero_iq[] = {0.0f, 0.0f, 0.5f, 0.0f};
    opts.const_norm_mode = 1;
    DSD_MEMSET(den, 0, sizeof den);
    dmax = constellation_accumulate_density(zero_iq, 2, &opts, &geom, 16384, 0.0, den);
    assert(dmax == 1);
    assert(sum_density(den, 9 * 7) == 1);
    opts.const_norm_mode = 0;

    unsigned short* dynamic_den = NULL;
    assert(constellation_prepare_density_buffer(3, 4, &dynamic_den));
    assert(dynamic_den != NULL);
    dynamic_den[0] = 9;
    assert(constellation_prepare_density_buffer(3, 4, &dynamic_den));
    assert(dynamic_den[0] == 0);

    int y_start = -1;
    int y_end = -1;
    constellation_active_y_bounds(den, &geom, &y_start, &y_end);
    assert(y_start >= geom.y0);
    assert(y_end <= geom.y1);
    assert(y_end >= y_start);

    assert(constellation_axis_char(1, 1) == '+');
    assert(constellation_axis_char(1, 0) == '-');
    assert(constellation_axis_char(0, 1) == '|');

    constellation_style style;
    DSD_MEMSET(&style, 0, sizeof style);
    style.ascii_len = (int)(sizeof(k_density_ascii_palette) - 1);
    style.block_len = (int)(sizeof(k_density_block_palette) / sizeof(k_density_block_palette[0]));
    style.dot_len = (int)(sizeof(k_constellation_dot_palette) / sizeof(k_constellation_dot_palette[0]));
    style.color_len = (int)(sizeof(k_density_color_seq) / sizeof(k_density_color_seq[0]));
    style.color_base = 41;
    int last_pair = -1;
    assert(constellation_density_char(&style, 1, 4, &last_pair) == '=');
    assert(last_pair == -1);

    constellation_style made_style = constellation_make_style(&opts);
    assert(made_style.use_unicode == 0);
    assert(made_style.use_dots == 1);
    assert(made_style.color_enabled == 0);
    assert(made_style.color_base == 41);

    char ch = '?';
    constellation_density_glyph(&style, 0.0, &ch);
    assert(ch == ' ');
    constellation_density_glyph(&style, 1.0, &ch);
    assert(ch == '@');

    const float scale_small[] = {0.01f, 0.0f, 0.02f, 0.0f, 0.03f, 0.0f, 0.04f, 0.0f};
    const float scale_large[] = {0.50f, 0.0f, 0.75f, 0.0f, 1.00f, 0.0f, 1.25f, 0.0f};
    int r_small = constellation_compute_scale_radius(scale_small, 4);
    int r_large = constellation_compute_scale_radius(scale_large, 4);
    assert(r_small >= 64);
    assert(r_large > r_small);

    g_test_rows = 10;
    g_test_cols = 20;
    int grid_w = 0;
    int grid_h = 0;
    constellation_grid_size(&grid_w, &grid_h);
    assert(grid_w == 32);
    assert(grid_h == 12);
    g_test_rows = 50;
    g_test_cols = 90;
    constellation_grid_size(&grid_w, &grid_h);
    assert(grid_w == 86);
    assert(grid_h == 25);
    g_test_rows = 24;
    g_test_cols = 80;

    constellation_geom render_geom = constellation_compute_geometry(9, 7);
    unsigned short render_den[9 * 7];
    DSD_MEMSET(render_den, 0, sizeof render_den);
    render_den[(size_t)render_geom.cy * (size_t)render_geom.W + (size_t)render_geom.cx] = 4;
    render_den[(size_t)(render_geom.cy - 1) * (size_t)render_geom.W + (size_t)(render_geom.cx + 1)] = 2;
    render_reset();
    constellation_render_rows(&style, &render_geom, render_den, render_geom.cy - 1, render_geom.cy, 4);
    assert(strchr(g_render_out, '+') != NULL);
    assert(strchr(g_render_out, '-') != NULL);
    assert(strchr(g_render_out, '*') != NULL);

    style.color_enabled = 1;
    style.guide_h_pair = 49;
    style.guide_v_pair = 50;
    style.guide_x_pair = 51;
    int color_pair = -1;
    render_reset();
    constellation_render_cell(&style, &render_geom, render_den, render_geom.cx + 1, render_geom.cy - 1, 4, &color_pair);
    assert(g_attron_count > 0);
    assert(color_pair >= style.color_base);
    render_reset();
    constellation_render_cell(&style, &render_geom, render_den, render_geom.cx, render_geom.cy, 4, &color_pair);
    assert(g_attron_count > 0);
    assert(g_attroff_count > 0);
    style.color_enabled = 0;

    render_reset();
    constellation_print_legend(&opts, &style);
    assert(strstr(g_render_out, "Ref: axes") != NULL);
    assert(strstr(g_render_out, "Density: . : - = + * # @") != NULL);
    assert(strstr(g_render_out, "Norm: radial") != NULL);

    render_reset();
    print_density_color_bar(0, 41, style.color_len);
    assert(strstr(g_render_out, "Color:") != NULL);
    assert(strstr(g_render_out, "low -> high") != NULL);
    assert(strstr(g_render_out, "0%") != NULL);
    assert(strstr(g_render_out, "100%") != NULL);
}

static void
test_eye_helpers(void) {
    const float samples[] = {-4.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f};
    int q1 = 0;
    int q2 = 0;
    int q3 = 0;
    eye_compute_quartiles(samples, 8, 1.0f, 64, &q1, &q2, &q3);
    assert(q1 <= q2);
    assert(q2 <= q3);

    unsigned short den[12 * 8];
    DSD_MEMSET(den, 0, sizeof den);
    den[(4 * 12) + 0] = 65535;
    eye_accumulate_density(samples, 8, 1.0f, 64, 4, 8, 12, 8, den);
    assert(den[(4 * 12) + 0] == 65535);
    assert(eye_density_max(den, 12, 8) == 65535);

    unsigned short* dynamic_den = NULL;
    assert(eye_prepare_density_buffer(4, 5, &dynamic_den));
    assert(dynamic_den != NULL);
    dynamic_den[0] = 7;
    assert(eye_prepare_density_buffer(4, 5, &dynamic_den));
    assert(dynamic_den[0] == 0);

    assert(eye_row_for_level(64, 64, 4, 8) == 1);
    assert(eye_row_for_level(-64, 64, 4, 8) == 7);
    assert(eye_boundary_col(0, 8, 12) == 0);
    assert(eye_boundary_col(7, 8, 12) == 11);
    assert(eye_overlay_char(' ', 1, 1) == '+');
    assert(eye_overlay_char('.', 1, 0) == '-');
    assert(eye_overlay_char('#', 1, 0) == '=');
    assert(eye_overlay_char('#', 0, 1) == '+');

    eye_style style;
    DSD_MEMSET(&style, 0, sizeof style);
    style.color_len = (int)(sizeof(k_density_color_seq) / sizeof(k_density_color_seq[0]));
    style.color_base = 21;
    int last_pair = -1;
    assert(eye_ascii_density_char(0.0) == ' ');
    assert(eye_ascii_density_char(1.0) == '@');
    assert(eye_density_char(&style, 1, 4, &last_pair) == '=');
    assert(last_pair == -1);
    style.unicode_requested = 1;
    assert(eye_density_char(&style, 1, 4, &last_pair) == 0);

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    eye_style made_style = eye_make_style(&opts, 0);
    assert(made_style.use_unicode_ui == 0);
    assert(made_style.unicode_requested == 0);
    assert(made_style.color_enabled == 0);
    assert(eye_effective_unicode_ui(&opts) == 0);

    eye_plot plot;
    DSD_MEMSET(&plot, 0, sizeof plot);
    plot.yq1 = 1;
    plot.yq2 = 3;
    plot.yq3 = 5;
    plot.xb0 = 0;
    plot.xb1 = 6;
    plot.xb2 = 11;
    assert(eye_is_hline(&plot, 3));
    assert(!eye_is_hline(&plot, 4));
    assert(eye_is_vline(&plot, 6));
    assert(!eye_is_vline(&plot, 7));

    assert(eye_snr_in_window(3, 4, 12, 1));
    assert(!eye_snr_in_window(8, 4, 12, 1));
    assert(eye_snr_bucket(-5, -1, 1, 5) == 0);
    assert(eye_snr_bucket(0, -1, 1, 5) == 1);
    assert(eye_snr_bucket(3, -1, 1, 5) == 2);
    assert(eye_snr_bucket(9, -1, 1, 5) == 3);

    long long cnt[4] = {20, 20, 20, 20};
    double sum[4] = {-200.0, -60.0, 60.0, 200.0};
    double mu[4] = {0.0, 0.0, 0.0, 0.0};
    assert(eye_snr_level_means(cnt, sum, mu) == 80);
    assert(eye_snr_has_levels(80, cnt));
    assert(eye_snr_signal_var(mu, cnt, 80) > 0.0);
    assert(!eye_snr_has_levels(50, cnt));

    float snr_samples[800];
    for (int i = 0; i < 800; i++) {
        int symbol = (i / 8) & 3;
        int noise = (i & 1) ? 3 : -3;
        int base = (symbol == 0) ? -90 : (symbol == 1) ? -30 : (symbol == 2) ? 30 : 90;
        snr_samples[i] = (float)(base + noise);
    }
    long long cnt2[4] = {0, 0, 0, 0};
    double sum2[4] = {0.0, 0.0, 0.0, 0.0};
    eye_snr_collect_bins(snr_samples, 800, 8, 2, 6, 1, -60, 0, 60, cnt2, sum2);
    assert(cnt2[0] > 0);
    assert(cnt2[1] > 0);
    assert(cnt2[2] > 0);
    assert(cnt2[3] > 0);
    double mu2[4] = {0.0, 0.0, 0.0, 0.0};
    long long total2 = eye_snr_level_means(cnt2, sum2, mu2);
    assert(eye_snr_noise_var(snr_samples, 800, 8, 2, 6, 1, -60, 0, 60, mu2, total2) > 0.0);
    assert(eye_estimate_snr_fallback(snr_samples, 800, 4, 8, -60, 0, 60, -100.0) > -20.0);
    assert(eye_estimate_snr_fallback(snr_samples, 80, 4, 8, -60, 0, 60, -17.5) == -17.5);
    assert(eye_smooth_peak(samples, 8, 64.0f) >= 64);

    g_test_rows = 9;
    g_test_cols = 18;
    int eye_w = 0;
    int eye_h = 0;
    eye_grid_size(&eye_w, &eye_h);
    assert(eye_w == 32);
    assert(eye_h == 12);
    g_test_rows = 60;
    g_test_cols = 90;
    eye_grid_size(&eye_w, &eye_h);
    assert(eye_w == 86);
    assert(eye_h == 20);
    g_test_rows = 24;
    g_test_cols = 80;

    unsigned short eye_den[8 * 6];
    DSD_MEMSET(eye_den, 0, sizeof eye_den);
    eye_den[(2 * 8) + 3] = 4;
    eye_plot render_plot;
    DSD_MEMSET(&render_plot, 0, sizeof render_plot);
    render_plot.den = eye_den;
    render_plot.W = 8;
    render_plot.H = 6;
    render_plot.dmax = 4;
    render_plot.yq1 = 1;
    render_plot.yq2 = 2;
    render_plot.yq3 = 4;
    render_plot.xb0 = 0;
    render_plot.xb1 = 3;
    render_plot.xb2 = 7;
    render_reset();
    eye_render_rows(&style, &render_plot);
    assert(strchr(g_render_out, '+') != NULL);
    assert(strchr(g_render_out, '-') != NULL);
    assert(strchr(g_render_out, '|') != NULL);

    style.color_enabled = 1;
    style.guide_h_pair = 29;
    style.guide_v_pair = 30;
    style.guide_x_pair = 31;
    int color_pair = -1;
    render_reset();
    eye_render_cell(&style, &render_plot, 3, 2, &color_pair);
    assert(g_attron_count > 0);
    assert(g_attroff_count > 0);
    style.color_enabled = 0;

    render_reset();
    eye_print_legend(&style);
    assert(strstr(g_render_out, "Ref: '-' Q1/Q3") != NULL);
    assert(strstr(g_render_out, "Density: . : - = + * # @") != NULL);
}

static void
test_histogram_and_spectrum_helpers(void) {
    assert(fsk_hist_bucket_for_value(-5, -1, 1, 5) == 0);
    assert(fsk_hist_bucket_for_value(0, -1, 1, 5) == 1);
    assert(fsk_hist_bucket_for_value(3, -1, 1, 5) == 2);
    assert(fsk_hist_bucket_for_value(9, -1, 1, 5) == 3);

    const float hist_samples[] = {-1.0f, -0.5f, 0.5f, 1.0f};
    int peak = 0;
    double dc_norm = fsk_hist_dc_offset_norm(hist_samples, 4, 100.0f, &peak);
    assert(peak == 100);
    assert(fabs(dc_norm) < 0.0001);

    int vals[8];
    int m = fsk_hist_collect_values(hist_samples, 4, 100.0f, vals);
    assert(m == 4);
    assert(vals[0] == -100);
    assert(vals[3] == 100);

    int64_t bins[4] = {0, 0, 0, 0};
    fsk_hist_count_bins(hist_samples, 4, 100.0f, -50, 0, 50, bins);
    assert(bins[0] == 2);
    assert(bins[1] == 0);
    assert(bins[2] == 1);
    assert(bins[3] == 1);

    int qvals[] = {-120, -40, -20, -10, 10, 20, 40, 120};
    int q1 = 0;
    int q2 = 0;
    int q3 = 0;
    fsk_hist_compute_quartiles(qvals, 8, &q1, &q2, &q3);
    assert(q1 <= q2);
    assert(q2 <= q3);

    const float fft_bins[] = {1.0f, 3.0f, 2.0f, 7.0f, 4.0f, 0.0f};
    float col[4];
    spectrum_resample_columns(fft_bins, 6, col, 3);
    assert(col[0] == 3.0f);
    assert(col[1] == 7.0f);
    assert(col[2] == 4.0f);
    spectrum_resample_columns(fft_bins, 3, col, 4);
    assert(col[0] == 1.0f);
    assert(col[3] == 2.0f);

    float vmin = 0.0f;
    float vmax = 0.0f;
    float span = 0.0f;
    spectrum_range(col, 4, &vmin, &vmax, &span);
    assert(vmax == 3.0f);
    assert(vmin == -57.0f);
    assert(span == 60.0f);

    assert(spectrum_color_pair_for_level(0.10f) == 13);
    assert(spectrum_color_pair_for_level(0.50f) == 12);
    assert(spectrum_color_pair_for_level(0.90f) == 11);

    render_reset();
    fsk_hist_draw_ruler(-50, 0, 50, -100, 100);
    assert(strstr(g_render_out, "Ruler:") != NULL);
    assert(strstr(g_render_out, "Median='+'") != NULL);
    assert(strchr(g_render_out, '+') != NULL);

    const int64_t bar_bins[4] = {1, 2, 4, 8};
    render_reset();
    fsk_hist_draw_bars(bar_bins, 0.125);
    assert(strstr(g_render_out, "DC Offset: +12.50%") != NULL);
    assert(strstr(g_render_out, "L3(-)") != NULL);
    assert(strstr(g_render_out, "L3(+)") != NULL);
    assert(strstr(g_render_out, " 8\n") != NULL);

    g_test_rows = 12;
    g_test_cols = 20;
    assert(spectrum_grid_width() == 32);
    assert(spectrum_grid_height() == 10);
    g_test_rows = 90;
    g_test_cols = 3000;
    assert(spectrum_grid_width() == 2048);
    assert(spectrum_grid_height() == 30);
    g_test_rows = 24;
    g_test_cols = 80;

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    g_has_colors = 1;
    opts.eye_color = 1;
    render_reset();
    spectrum_draw_cell(&opts, 0, vmax, vmin, vmax, span, 0, 4);
    assert(strcmp(g_render_out, "#") == 0);
    assert(g_attron_count == 1);
    assert(g_attroff_count == 1);
    render_reset();
    spectrum_draw_cell(&opts, 0, vmin, vmin, vmax, span, 0, 4);
    assert(strcmp(g_render_out, " ") == 0);

    const float plot_cols[] = {vmin, vmax};
    render_reset();
    spectrum_draw_plot(&opts, plot_cols, 2, 3, vmin, vmax, span, 0);
    assert(strstr(g_render_out, " #\n") != NULL);
    assert(strstr(g_render_out, "##\n") != NULL);

    render_reset();
    spectrum_print_legend(&opts, 48000, 48, 0, vmax, vmin);
    assert(strstr(g_render_out, "Span: 48.0 kHz") != NULL);
    assert(strstr(g_render_out, "FFT: 128") != NULL);
    assert(strstr(g_render_out, "Glyphs: ASCII; colored") != NULL);
    assert(strstr(g_render_out, "Color:") != NULL);
    g_has_colors = 0;

    render_reset();
    print_fsk_hist_view();
    assert(strstr(g_render_out, "(no samples)") != NULL);

    render_reset();
    print_spectrum_view(&opts);
    assert(strstr(g_render_out, "(no spectrum yet)") != NULL);
}

int
main(void) {
    test_density_and_constellation_helpers();
    test_eye_helpers();
    test_histogram_and_spectrum_helpers();
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
