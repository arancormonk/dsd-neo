// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_visualizers.c
 * RTL-SDR visualization panels: constellation, eye diagram, histogram, spectrum
 */

#include <curses.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/ui/ncurses_internal.h>
#include <dsd-neo/ui/ncurses_visualizers.h>
#include <dsd-neo/ui/ui_prims.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

static int
clamp_int_local(int value, int lo, int hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static float
clamp_float_local(float value, float lo, float hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static const char k_density_ascii_palette[] = " .:-=+*#%@";
static const char* const k_density_block_palette[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
static const char* const k_constellation_dot_palette[] = {" ", "·", "∙", "•", "●", "⬤"};
static const short k_density_color_seq[] = {COLOR_BLUE,    COLOR_CYAN, COLOR_GREEN, COLOR_YELLOW,
                                            COLOR_MAGENTA, COLOR_RED,  COLOR_WHITE};

static int
density_palette_index(double g, int len) {
    return clamp_int_local((int)lrint(g * (double)(len - 1)), 0, len - 1);
}

static double
density_gamma_value(unsigned short d, unsigned short dmax) {
    double f = (double)d / (double)dmax;
    f = clamp_float_local((float)f, 0.0f, 1.0f);
    return ui_gamma_map01(f);
}

static int
density_color_pair_id(short color_base, int color_len, double g) {
    int ci = density_palette_index(g, color_len);
    return (int)color_base + ci;
}

static void
print_density_color_bar(int use_unicode, short color_base, int color_len) {
    ui_print_lborder();
    printw(" Color:   ");
    for (int i = 0; i < color_len; i++) {
        attron(COLOR_PAIR((short)(color_base + i)));
        if (use_unicode) {
            addstr("██");
        } else {
            addstr("##");
        }
        attroff(COLOR_PAIR((short)(color_base + i)));
    }
    printw("  low -> high\n");

    ui_print_lborder();
    printw("          ");
    int barw = color_len * 2;
    for (int x = 0; x < barw; x++) {
        if (x == 0 || x == barw / 2 || x == barw - 1) {
            addch('|');
        } else {
            addch(' ');
        }
    }
    printw("\n");

    ui_print_lborder();
    printw("          0%%");
    int pad_mid = barw / 2 - 2;
    for (int i = 0; i < pad_mid; i++) {
        addch(' ');
    }
    printw("50%%");
    int pad_end = barw - (barw / 2 + 2) - 4;
    for (int i = 0; i < pad_end; i++) {
        addch(' ');
    }
    printw("100%%\n");
}

typedef struct {
    int use_unicode;
    int use_dots;
    int color_enabled;
    int ascii_len;
    int block_len;
    int dot_len;
    int color_len;
    short color_base;
    short guide_h_pair;
    short guide_v_pair;
    short guide_x_pair;
} constellation_style;

typedef struct {
    int W;
    int H;
    int cx;
    int cy;
    int x0;
    int x1;
    int y0;
    int y1;
    double y_aspect;
    double outer_margin;
} constellation_geom;

static void
constellation_grid_size(int* W, int* H) {
    int rows = 24;
    int cols = 80;
    getmaxyx(stdscr, rows, cols);
    *W = cols - 4;
    if (*W < 32) {
        *W = 32;
    }
    *H = rows / 2;
    if (*H < 12) {
        *H = 12;
    }
}

static constellation_style
constellation_make_style(const dsd_opts* opts) {
    constellation_style style;
    style.use_unicode = (opts && opts->eye_unicode && ui_unicode_supported());
    style.use_dots = 1;
    style.color_enabled = (opts && opts->eye_color && has_colors());
    style.ascii_len = (int)(sizeof(k_density_ascii_palette) - 1);
    style.block_len = (int)(sizeof(k_density_block_palette) / sizeof(k_density_block_palette[0]));
    style.dot_len = (int)(sizeof(k_constellation_dot_palette) / sizeof(k_constellation_dot_palette[0]));
    style.color_len = (int)(sizeof(k_density_color_seq) / sizeof(k_density_color_seq[0]));
    style.color_base = 41;
    style.guide_h_pair = (short)(style.color_base + 8);
    style.guide_v_pair = (short)(style.color_base + 9);
    style.guide_x_pair = (short)(style.color_base + 10);

    static int color_inited = 0;
    if (style.color_enabled && !color_inited) {
        for (int i = 0; i < style.color_len; i++) {
            init_pair((short)(style.color_base + i), k_density_color_seq[i], COLOR_BLACK);
        }
        init_pair(style.guide_h_pair, COLOR_YELLOW, COLOR_BLACK);
        init_pair(style.guide_v_pair, COLOR_CYAN, COLOR_BLACK);
        init_pair(style.guide_x_pair, COLOR_MAGENTA, COLOR_BLACK);
        color_inited = 1;
    }
    return style;
}

static int
constellation_prepare_density_buffer(int H, int W, unsigned short** den_out) {
    static unsigned short* s_den = NULL;
    static size_t s_den_cap = 0;
    size_t den_sz = (size_t)H * (size_t)W;

    if (s_den_cap < den_sz) {
        void* nb = realloc(s_den, den_sz * sizeof(unsigned short));
        if (!nb) {
            free(s_den);
            s_den = NULL;
            s_den_cap = 0;
            *den_out = NULL;
            return 0;
        }
        s_den = (unsigned short*)nb;
        s_den_cap = den_sz;
    }

    if (den_sz > 0 && s_den != NULL) {
        DSD_MEMSET(s_den, 0, den_sz * sizeof(unsigned short));
    }
    *den_out = s_den;
    return (s_den != NULL);
}

static int
constellation_compute_scale_radius(const float* buf, int n) {
    static int s_maxR = 256;
    static int magR[4096];
    const float kFloatScale = 16384.0f;

    for (int k = 0; k < n; k++) {
        float i = buf[(size_t)(k << 1) + 0] * kFloatScale;
        float q = buf[(size_t)(k << 1) + 1] * kFloatScale;
        double r2 = (double)i * (double)i + (double)q * (double)q;
        magR[k] = (int)lrint(sqrt(r2));
    }

    int idxP = (int)lrint(0.99 * (double)(n - 1));
    idxP = clamp_int_local(idxP, 0, n - 1);
    int pR = select_k_int_local(magR, n, idxP);
    if (pR < 64) {
        pR = 64;
    }
    s_maxR = (int)(0.8 * (double)s_maxR + 0.2 * (double)pR);
    if (s_maxR < 64) {
        s_maxR = 64;
    }
    return s_maxR;
}

static double
constellation_gate_squared(const dsd_opts* opts) {
    double gate = 0.10;
    if (opts) {
        gate = (opts->mod_qpsk == 1) ? (double)opts->const_gate_qpsk : (double)opts->const_gate_other;
        if (gate < 0.0) {
            gate = 0.0;
        }
        if (gate > 0.90) {
            gate = 0.90;
        }
    }
    return gate * gate;
}

static constellation_geom
constellation_compute_geometry(int W, int H) {
    constellation_geom geom;
    geom.W = W;
    geom.H = H;
    geom.cx = W / 2;
    geom.cy = H / 2;
    int halfX = (W / 2) - 1;
    int halfY = (H / 2) - 1;
    if (halfX < 1) {
        halfX = 1;
    }
    if (halfY < 1) {
        halfY = 1;
    }
    int scale_eq = (halfX < halfY) ? halfX : halfY;
    geom.y_aspect = 0.55;
    geom.outer_margin = 0.92;
    geom.x0 = geom.cx - scale_eq;
    geom.x1 = geom.cx + scale_eq;
    geom.y0 = geom.cy - scale_eq;
    geom.y1 = geom.cy + scale_eq;
    return geom;
}

static unsigned short
constellation_accumulate_density(const float* buf, int n, const dsd_opts* opts, const constellation_geom* geom,
                                 int s_maxR, double gate2, unsigned short* den) {
    const float kFloatScale = 16384.0f;
    unsigned short dmax = 0;

    for (int k = 0; k < n; k++) {
        float fi = buf[(size_t)(k << 1) + 0] * kFloatScale;
        float fq = buf[(size_t)(k << 1) + 1] * kFloatScale;
        double ii = (double)fi;
        double qq = (double)fq;
        double r = sqrt(ii * ii + qq * qq);
        double rn = r / (double)s_maxR;
        if ((rn * rn) < gate2) {
            continue;
        }

        double nx;
        double ny;
        if (opts && opts->const_norm_mode == 1) {
            if (r <= 1e-9) {
                continue;
            }
            nx = ii / r;
            ny = qq / r;
        } else {
            nx = ii / (double)s_maxR;
            ny = qq / (double)s_maxR;
        }

        int x = geom->cx + (int)lrint(nx * (double)(geom->x1 - geom->cx) * geom->outer_margin);
        int y = geom->cy - (int)lrint(ny * (double)(geom->y1 - geom->cy) * geom->outer_margin * geom->y_aspect);
        x = clamp_int_local(x, 0, geom->W - 1);
        y = clamp_int_local(y, 0, geom->H - 1);
        unsigned short* cell = &den[(size_t)y * (size_t)geom->W + (size_t)x];
        if (*cell != 0xFFFF) {
            (*cell)++;
            if (*cell > dmax) {
                dmax = *cell;
            }
        }
    }

    if (dmax == 0) {
        dmax = 1;
    }
    return dmax;
}

static void
constellation_active_y_bounds(const unsigned short* den, const constellation_geom* geom, int* y_start_out,
                              int* y_end_out) {
    int y_start = clamp_int_local(geom->y0, 0, geom->H - 1);
    int y_end = clamp_int_local(geom->y1, 0, geom->H - 1);
    int y_top = -1;
    int y_bot = -1;

    for (int y = y_start; y <= y_end; y++) {
        int has = 0;
        for (int x = geom->x0; x <= geom->x1; x++) {
            if (x >= 0 && x < geom->W && den[(size_t)y * (size_t)geom->W + (size_t)x] > 0) {
                has = 1;
                break;
            }
        }
        if (has) {
            y_top = y;
            break;
        }
    }

    for (int y = y_end; y >= y_start; y--) {
        int has = 0;
        for (int x = geom->x0; x <= geom->x1; x++) {
            if (x >= 0 && x < geom->W && den[(size_t)y * (size_t)geom->W + (size_t)x] > 0) {
                has = 1;
                break;
            }
        }
        if (has) {
            y_bot = y;
            break;
        }
    }

    if (y_top >= 0 && y_bot >= y_top) {
        y_start = y_top;
        y_end = y_bot;
    }
    *y_start_out = y_start;
    *y_end_out = y_end;
}

static void
constellation_apply_density_color(const constellation_style* style, double g, int* last_pair) {
    int pid = density_color_pair_id(style->color_base, style->color_len, g);
    if (pid != *last_pair) {
        if (*last_pair >= 0) {
            attroff(COLOR_PAIR(*last_pair));
        }
        attron(COLOR_PAIR(pid));
        *last_pair = pid;
    }
}

static void
constellation_density_glyph(const constellation_style* style, double g, char* ch_out) {
    if (style->use_unicode) {
        if (style->use_dots) {
            int idx = density_palette_index(g, style->dot_len);
            addstr(k_constellation_dot_palette[idx]);
        } else {
            int idx = density_palette_index(g, style->block_len);
            addstr(k_density_block_palette[idx]);
        }
        *ch_out = 0;
        return;
    }

    int idx = density_palette_index(g, style->ascii_len);
    *ch_out = k_density_ascii_palette[idx];
}

static char
constellation_axis_char(int is_haxis, int is_vaxis) {
    if (is_haxis && is_vaxis) {
        return '+';
    }
    if (is_haxis) {
        return '-';
    }
    return '|';
}

static int
constellation_apply_axis_guide(const constellation_style* style, int is_haxis, int is_vaxis, int* last_pair) {
    if (!style->color_enabled) {
        return 0;
    }
    short gp = (is_haxis && is_vaxis) ? style->guide_x_pair : (is_haxis ? style->guide_h_pair : style->guide_v_pair);
    if (*last_pair >= 0) {
        attroff(COLOR_PAIR(*last_pair));
        *last_pair = -1;
    }
    attron(COLOR_PAIR(gp));
    return gp;
}

static char
constellation_density_char(const constellation_style* style, unsigned short d, unsigned short dmax, int* last_pair) {
    double g = density_gamma_value(d, dmax);
    if (style->color_enabled) {
        constellation_apply_density_color(style, g, last_pair);
    }
    char ch = ' ';
    constellation_density_glyph(style, g, &ch);
    return ch;
}

static void
constellation_restore_after_guide(const constellation_style* style, int used_guide, unsigned short d,
                                  unsigned short dmax, int* last_pair) {
    if (!used_guide) {
        return;
    }
    attroff(COLOR_PAIR(used_guide));
    if (style->color_enabled && d > 0) {
        double g = density_gamma_value(d, dmax);
        int pid = density_color_pair_id(style->color_base, style->color_len, g);
        attron(COLOR_PAIR(pid));
        *last_pair = pid;
    }
}

static void
constellation_render_cell(const constellation_style* style, const constellation_geom* geom, const unsigned short* den,
                          int x, int y, unsigned short dmax, int* last_pair) {
    int inside_sq = (x >= geom->x0 && x <= geom->x1 && y >= geom->y0 && y <= geom->y1);
    int is_haxis = inside_sq && (y == geom->cy);
    int is_vaxis = inside_sq && (x == geom->cx);
    unsigned short d = den[(size_t)y * (size_t)geom->W + (size_t)x];
    char ch = ' ';
    int used_guide = 0;

    if (is_haxis || is_vaxis) {
        ch = constellation_axis_char(is_haxis, is_vaxis);
        used_guide = constellation_apply_axis_guide(style, is_haxis, is_vaxis, last_pair);
    } else if (inside_sq && d > 0) {
        ch = constellation_density_char(style, d, dmax, last_pair);
    }

    if (ch != 0) {
        addch(ch);
    }
    constellation_restore_after_guide(style, used_guide, d, dmax, last_pair);
}

static void
constellation_render_rows(const constellation_style* style, const constellation_geom* geom, const unsigned short* den,
                          int y_start, int y_end, unsigned short dmax) {
    for (int y = y_start; y <= y_end; y++) {
        ui_print_lborder();
        int last_pair = -1;
        for (int x = 0; x < geom->W; x++) {
            constellation_render_cell(style, geom, den, x, y, dmax, &last_pair);
        }
        if (style->color_enabled && last_pair >= 0) {
            attroff(COLOR_PAIR(last_pair));
        }
        printw("\n");
    }
}

static void
constellation_print_legend(const dsd_opts* opts, const constellation_style* style) {
    ui_print_lborder();
    printw(" Ref: axes '+', '-', '|'\n");
    if (style->use_unicode) {
        ui_print_lborder();
        if (style->use_dots) {
            printw(" Density: · • ● ⬤  (low -> high)%s\n", style->color_enabled ? "; colored" : "");
        } else {
            printw(" Density: ▁ ▂ ▃ ▄ ▅ ▆ ▇ █  (low -> high)%s\n", style->color_enabled ? "; colored" : "");
        }
    } else {
        ui_print_lborder();
        printw(" Density: . : - = + * # @  (low -> high)%s\n", style->color_enabled ? "; colored" : "");
    }
    ui_print_lborder();
    printw(" Norm: %s (toggle with 'n')\n", (opts && opts->const_norm_mode) ? "unit-circle" : "radial (p99)");
    if (style->color_enabled) {
        ui_print_lborder();
        addch('\n');
        print_density_color_bar(style->use_unicode, style->color_base, style->color_len);
    }
}

typedef struct {
    int use_unicode_ui;
    int unicode_requested;
    int color_enabled;
    int color_len;
    short color_base;
    short guide_h_pair;
    short guide_v_pair;
    short guide_x_pair;
} eye_style;

typedef struct {
    const unsigned short* den;
    int W;
    int H;
    unsigned short dmax;
    int yq1;
    int yq2;
    int yq3;
    int xb0;
    int xb1;
    int xb2;
} eye_plot;

static int
eye_effective_unicode_ui(const dsd_opts* opts) {
    static int s_unicode_ready = -1;
    static int s_unicode_warned = 0;
    if (s_unicode_ready < 0) {
        s_unicode_ready = ui_unicode_supported() ? 1 : 0;
    }
    int use_unicode_ui = (opts && opts->eye_unicode && s_unicode_ready);
    if (opts && opts->eye_unicode && !s_unicode_ready && !s_unicode_warned) {
        printw("| (Unicode block glyphs unsupported; falling back to ASCII)\n");
        s_unicode_warned = 1;
    }
    return use_unicode_ui;
}

static void
eye_grid_size(int* W, int* H) {
    int rows = 24;
    int cols = 80;
    getmaxyx(stdscr, rows, cols);
    *W = cols - 4;
    if (*W < 32) {
        *W = 32;
    }
    *H = rows / 3;
    if (*H < 12) {
        *H = 12;
    }
}

static int
eye_prepare_density_buffer(int H, int W, unsigned short** den_out) {
    static unsigned short* s_den_eye = NULL;
    static size_t s_den_eye_cap = 0;
    size_t den_sz = (size_t)H * (size_t)W;

    if (s_den_eye_cap < den_sz) {
        void* nb = realloc(s_den_eye, den_sz * sizeof(unsigned short));
        if (!nb) {
            free(s_den_eye);
            s_den_eye = NULL;
            s_den_eye_cap = 0;
            *den_out = NULL;
            return 0;
        }
        s_den_eye = (unsigned short*)nb;
        s_den_eye_cap = den_sz;
    }

    if (den_sz > 0 && s_den_eye != NULL) {
        DSD_MEMSET(s_den_eye, 0, den_sz * sizeof(unsigned short));
    }
    *den_out = s_den_eye;
    return (s_den_eye != NULL);
}

static int
eye_smooth_peak(const float* buf, int n, float scale) {
    static int s_peak = 256;
    int peak = 1;
    for (int i = 0; i < n; i++) {
        float v = buf[i] * scale;
        int a = (int)lrintf(fabsf(v));
        if (a > peak) {
            peak = a;
        }
    }
    if (peak < 64) {
        peak = 64;
    }
    s_peak = (int)(0.8 * (double)s_peak + 0.2 * (double)peak);
    if (s_peak < 64) {
        s_peak = 64;
    }
    return s_peak;
}

static void
eye_compute_quartiles(const float* buf, int n, float scale, int s_peak, int* q1, int* q2, int* q3) {
    int step_ds = (n > 8192) ? (n / 8192) : 1;
    int m = (n + step_ds - 1) / step_ds;
    if (m > 8192) {
        m = 8192;
    }
    static int qvals[8192];
    int vi = 0;
    for (int i = 0; i < n && vi < m; i += step_ds) {
        qvals[vi++] = (int)lrintf(buf[i] * scale);
    }
    m = vi;
    if (m < 8) {
        qvals[0] = -s_peak;
        qvals[1] = s_peak;
        m = 2;
    }
    int idx1 = (int)((size_t)m / 4);
    int idx2 = (int)((size_t)m / 2);
    int idx3 = (int)((size_t)(3 * (size_t)m) / 4);
    *q2 = select_k_int_local(qvals, m, idx2);
    *q1 = select_k_int_local(qvals, idx2, idx1);
    *q3 = select_k_int_local(qvals + idx2 + 1, m - (idx2 + 1), idx3 - (idx2 + 1));
}

static void
eye_accumulate_density(const float* buf, int n, float scale, int s_peak, int mid, int H, int W, int two_sps,
                       unsigned short* den) {
    for (int i = 0; i < n; i++) {
        double v = ((double)buf[i] * (double)scale) / (double)s_peak;
        v = clamp_float_local((float)v, -1.0f, 1.0f);
        int y = mid - (int)lrint(v * ((double)H / 2.0 - 1.0));
        y = clamp_int_local(y, 0, H - 1);
        int phase = i % two_sps;
        int x = (int)lrint(((double)phase / (double)(two_sps - 1)) * (double)(W - 1));
        x = clamp_int_local(x, 0, W - 1);
        size_t di = (size_t)y * (size_t)W + (size_t)x;
        if (den[di] < 65535) {
            den[di]++;
        }
    }
}

static unsigned short
eye_density_max(const unsigned short* den, int W, int H) {
    unsigned short dmax = 1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned short dv = den[(size_t)y * (size_t)W + (size_t)x];
            if (dv > dmax) {
                dmax = dv;
            }
        }
    }
    return dmax;
}

static int
eye_row_for_level(int q, int s_peak, int mid, int H) {
    int y = mid - (int)lrint(((double)q / (double)s_peak) * ((double)H / 2.0 - 1.0));
    return clamp_int_local(y, 0, H - 1);
}

static int
eye_boundary_col(int phase, int two_sps, int W) {
    int x = (int)lrint(((double)phase / (double)(two_sps - 1)) * (double)(W - 1));
    return clamp_int_local(x, 0, W - 1);
}

static eye_style
eye_make_style(const dsd_opts* opts, int use_unicode_ui) {
    eye_style style;
    style.use_unicode_ui = use_unicode_ui;
    style.unicode_requested = opts->eye_unicode;
    style.color_enabled = opts->eye_color && has_colors();
    style.color_len = (int)(sizeof(k_density_color_seq) / sizeof(k_density_color_seq[0]));
    style.color_base = 21;
    style.guide_h_pair = (short)(style.color_base + 8);
    style.guide_v_pair = (short)(style.color_base + 9);
    style.guide_x_pair = (short)(style.color_base + 10);

    static int color_inited = 0;
    if (style.color_enabled && !color_inited) {
        for (int i = 0; i < style.color_len; i++) {
            init_pair((short)(style.color_base + i), k_density_color_seq[i], COLOR_BLACK);
        }
        init_pair(style.guide_h_pair, COLOR_YELLOW, COLOR_BLACK);
        init_pair(style.guide_v_pair, COLOR_CYAN, COLOR_BLACK);
        init_pair(style.guide_x_pair, COLOR_MAGENTA, COLOR_BLACK);
        color_inited = 1;
    }
    return style;
}

static void
eye_apply_density_color(const eye_style* style, double g, int* last_pair) {
    int pid = density_color_pair_id(style->color_base, style->color_len, g);
    if (pid != *last_pair) {
        if (*last_pair >= 0) {
            attroff(COLOR_PAIR(*last_pair));
        }
        attron(COLOR_PAIR(pid));
        *last_pair = pid;
    }
}

static char
eye_ascii_density_char(double g) {
    int idx = density_palette_index(g, (int)(sizeof(k_density_ascii_palette) - 1));
    return k_density_ascii_palette[idx];
}

static char
eye_overlay_char(char ch, int is_hline, int is_vline) {
    if (is_hline && is_vline) {
        return '+';
    }
    if (is_hline) {
        return (ch == ' ' || ch == '.' || ch == ':') ? '-' : '=';
    }
    return (ch == ' ' || ch == '.' || ch == ':' || ch == '-') ? '|' : '+';
}

static int
eye_is_hline(const eye_plot* plot, int y) {
    return (y == plot->yq1 || y == plot->yq2 || y == plot->yq3);
}

static int
eye_is_vline(const eye_plot* plot, int x) {
    return (x == plot->xb0 || x == plot->xb1 || x == plot->xb2);
}

static int
eye_apply_guide_color(const eye_style* style, int is_hline, int is_vline, int* last_pair) {
    if (!style->color_enabled) {
        return 0;
    }
    short gp = is_hline && is_vline ? style->guide_x_pair : (is_hline ? style->guide_h_pair : style->guide_v_pair);
    if (*last_pair >= 0) {
        attroff(COLOR_PAIR(*last_pair));
        *last_pair = -1;
    }
    attron(COLOR_PAIR(gp));
    return gp;
}

static char
eye_density_char(const eye_style* style, unsigned short d, unsigned short dmax, int* last_pair) {
    double g = density_gamma_value(d, dmax);
    if (style->color_enabled) {
        eye_apply_density_color(style, g, last_pair);
    }
    if (style->unicode_requested) {
        return 0;
    }
    return eye_ascii_density_char(g);
}

static void
eye_draw_density_or_char(const eye_style* style, unsigned short d, unsigned short dmax, char ch, int used_guide) {
    if (style->use_unicode_ui && ch == 0 && !used_guide) {
        double g2 = density_gamma_value(d, dmax);
        int uidx =
            density_palette_index(g2, (int)(sizeof(k_density_block_palette) / sizeof(k_density_block_palette[0])));
        addstr(k_density_block_palette[uidx]);
    } else {
        addch(ch);
    }
}

static void
eye_restore_after_guide(const eye_style* style, int used_guide, unsigned short d, unsigned short dmax, int* last_pair) {
    if (!used_guide) {
        return;
    }
    attroff(COLOR_PAIR(used_guide));
    if (style->color_enabled) {
        double g = density_gamma_value(d, dmax);
        eye_apply_density_color(style, g, last_pair);
    }
}

static void
eye_render_cell(const eye_style* style, const eye_plot* plot, int x, int y, int* last_pair) {
    unsigned short d = plot->den[(size_t)y * (size_t)plot->W + (size_t)x];
    char ch = ' ';

    if (d > 0) {
        ch = eye_density_char(style, d, plot->dmax, last_pair);
    }

    int is_hline = eye_is_hline(plot, y);
    int is_vline = eye_is_vline(plot, x);
    int used_guide = 0;
    if (is_hline || is_vline) {
        ch = eye_overlay_char(ch, is_hline, is_vline);
        used_guide = eye_apply_guide_color(style, is_hline, is_vline, last_pair);
    }

    eye_draw_density_or_char(style, d, plot->dmax, ch, used_guide);
    eye_restore_after_guide(style, used_guide, d, plot->dmax, last_pair);
}

static void
eye_render_rows(const eye_style* style, const eye_plot* plot) {
    for (int y = 0; y < plot->H; y++) {
        ui_print_lborder();
        int last_pair = -1;
        for (int x = 0; x < plot->W; x++) {
            eye_render_cell(style, plot, x, y, &last_pair);
        }
        if (style->color_enabled && last_pair >= 0) {
            attroff(COLOR_PAIR(last_pair));
        }
        addch('\n');
    }
}

static void
eye_print_legend(const eye_style* style) {
    ui_print_lborder();
    printw(" Ref: '-' Q1/Q3, '=' median; '|' edges; '+' crossings\n");
    if (style->use_unicode_ui) {
        ui_print_lborder();
        printw(" Density: ▁ ▂ ▃ ▄ ▅ ▆ ▇ █  (low -> high)%s\n", style->color_enabled ? "; colored" : "");
    } else {
        ui_print_lborder();
        printw(" Density: . : - = + * # @  (low -> high)%s\n", style->color_enabled ? "; colored" : "");
    }
    if (style->color_enabled) {
        ui_print_lborder();
        addch('\n');
        print_density_color_bar(style->use_unicode_ui, style->color_base, style->color_len);
    }
}

static int
eye_snr_in_window(int phase, int c1, int c2, int win) {
    return (abs(phase - c1) <= win) || (abs(phase - c2) <= win);
}

static int
eye_snr_bucket(int v, int q1, int q2, int q3) {
    if (v <= q1) {
        return 0;
    }
    if (v <= q2) {
        return 1;
    }
    if (v <= q3) {
        return 2;
    }
    return 3;
}

static void
eye_snr_collect_bins(const float* buf, int n, int two_sps, int c1, int c2, int win, int q1, int q2, int q3,
                     long long cnt[4], double sum[4]) {
    for (int i = 0; i < n; i++) {
        int phase = i % two_sps;
        if (!eye_snr_in_window(phase, c1, c2, win)) {
            continue;
        }
        int v = (int)buf[i];
        int b = eye_snr_bucket(v, q1, q2, q3);
        cnt[b]++;
        sum[b] += (double)v;
    }
}

static long long
eye_snr_level_means(const long long cnt[4], const double sum[4], double mu[4]) {
    long long total = 0;
    for (int b = 0; b < 4; b++) {
        if (cnt[b] > 0) {
            mu[b] = sum[b] / (double)cnt[b];
        }
        total += cnt[b];
    }
    return total;
}

static int
eye_snr_has_levels(long long total, const long long cnt[4]) {
    if (total <= 50) {
        return 0;
    }
    if (!cnt[0] || !cnt[1] || !cnt[2] || !cnt[3]) {
        return 0;
    }
    return 1;
}

static double
eye_snr_noise_var(const float* buf, int n, int two_sps, int c1, int c2, int win, int q1, int q2, int q3,
                  const double mu[4], long long total) {
    double nsum = 0.0;
    for (int i = 0; i < n; i++) {
        int phase = i % two_sps;
        if (!eye_snr_in_window(phase, c1, c2, win)) {
            continue;
        }
        int v = (int)buf[i];
        int b = eye_snr_bucket(v, q1, q2, q3);
        double e = (double)v - mu[b];
        nsum += e * e;
    }
    return nsum / (double)total;
}

static double
eye_snr_signal_var(const double mu[4], const long long cnt[4], long long total) {
    double mu_all = 0.0;
    for (int b = 0; b < 4; b++) {
        mu_all += mu[b] * (double)cnt[b] / (double)total;
    }
    double ssum = 0.0;
    for (int b = 0; b < 4; b++) {
        double d = mu[b] - mu_all;
        ssum += (double)cnt[b] * d * d;
    }
    return ssum / (double)total;
}

static double
eye_estimate_snr_fallback(const float* buf, int n, int sps, int two_sps, int q1, int q2, int q3, double snr_db) {
    if (sps <= 0 || n <= 100) {
        return snr_db;
    }

    int c1 = sps / 2;
    int c2 = (3 * sps) / 2;
    int win = sps / 10;
    if (win < 1) {
        win = 1;
    }
    long long cnt[4] = {0, 0, 0, 0};
    double sum[4] = {0, 0, 0, 0};
    eye_snr_collect_bins(buf, n, two_sps, c1, c2, win, q1, q2, q3, cnt, sum);

    double mu[4] = {0, 0, 0, 0};
    long long total = eye_snr_level_means(cnt, sum, mu);
    if (!eye_snr_has_levels(total, cnt)) {
        return snr_db;
    }

    double noise_var = eye_snr_noise_var(buf, n, two_sps, c1, c2, win, q1, q2, q3, mu, total);
    double sig_var = eye_snr_signal_var(mu, cnt, total);
    if (noise_var > 1e-9 && sig_var > 1e-9) {
#ifdef USE_RTLSDR
        double bias = rtl_stream_get_snr_bias_c4fm();
#else
        double bias = 8.0;
#endif
        snr_db = 10.0 * log10(sig_var / noise_var) - bias;
    }
    return snr_db;
}

void
print_constellation_view(dsd_opts* opts, dsd_state* state) {
    UNUSED(opts);
    UNUSED(state);
#ifdef USE_RTLSDR
    enum { MAXP = 4096 };

    float buf[(size_t)MAXP * 2];
    int n = rtl_stream_constellation_get(buf, MAXP);
    ui_print_header("Constellation");
    if (n <= 0) {
        ui_print_lborder();
        printw(" (no samples yet)\n");
        attron(COLOR_PAIR(4));
        ui_print_hr();
        attroff(COLOR_PAIR(4));
        return;
    }

    int W = 0;
    int H = 0;
    constellation_grid_size(&W, &H);
    constellation_style style = constellation_make_style(opts);
    unsigned short* den = NULL;
    if (!constellation_prepare_density_buffer(H, W, &den)) {
        printw("| (constellation: out of memory)\n");
        ui_print_hr();
        return;
    }
    if (den == NULL) {
        return;
    }

    int s_maxR = constellation_compute_scale_radius(buf, n);
    double gate2 = constellation_gate_squared(opts);
    constellation_geom geom = constellation_compute_geometry(W, H);
    unsigned short dmax = constellation_accumulate_density(buf, n, opts, &geom, s_maxR, gate2, den);

    int y_start = 0;
    int y_end = 0;
    constellation_active_y_bounds(den, &geom, &y_start, &y_end);
    constellation_render_rows(&style, &geom, den, y_start, y_end, dmax);
    constellation_print_legend(opts, &style);

    attron(COLOR_PAIR(4));
    attron(COLOR_PAIR(4));
    ui_print_hr();
    attroff(COLOR_PAIR(4));
    attroff(COLOR_PAIR(4));
#else
    ui_print_header("Constellation");
    printw("| (RTL disabled in this build)\n");
    ui_print_hr();
#endif
}

void
print_eye_view(dsd_opts* opts, dsd_state* state) {
    UNUSED2(opts, state);
#ifdef USE_RTLSDR
    enum { MAXS = 16384 };

    static float buf[(size_t)MAXS];
    int sps = 0;
    int n = rtl_stream_eye_get(buf, MAXS, &sps);
    ui_print_header("Eye Diagram (C4FM/FSK)");
    int use_unicode_ui = eye_effective_unicode_ui(opts);
    if (n <= 0 || sps <= 0) {
        ui_print_lborder();
        printw(" (no samples or SPS)\n");
        attron(COLOR_PAIR(4));
        ui_print_hr();
        attroff(COLOR_PAIR(4));
        return;
    }
    int W = 0;
    int H = 0;
    eye_grid_size(&W, &H);
    unsigned short* den = NULL;
    if (!eye_prepare_density_buffer(H, W, &den)) {
        printw("| (eye: out of memory)\n");
        ui_print_hr();
        return;
    }
    if (den == NULL) {
        return;
    }

    int mid = H / 2;
    static const float kEyeFloatScale = 16384.0f;
    int s_peak = eye_smooth_peak(buf, n, kEyeFloatScale);
    int q1 = 0;
    int q2 = 0;
    int q3 = 0;
    eye_compute_quartiles(buf, n, kEyeFloatScale, s_peak, &q1, &q2, &q3);

    int two_sps = 2 * sps;
    if (two_sps < 8) {
        two_sps = 8;
    }
    eye_accumulate_density(buf, n, kEyeFloatScale, s_peak, mid, H, W, two_sps, den);
    unsigned short dmax = eye_density_max(den, W, H);
    eye_style style = eye_make_style(opts, use_unicode_ui);

    int yq1 = eye_row_for_level(q1, s_peak, mid, H);
    int yq2 = eye_row_for_level(q2, s_peak, mid, H);
    int yq3 = eye_row_for_level(q3, s_peak, mid, H);
    int xb0 = 0;
    int xb1 = eye_boundary_col(sps, two_sps, W);
    int xb2 = W - 1;

    eye_plot plot = {.den = den,
                     .W = W,
                     .H = H,
                     .dmax = dmax,
                     .yq1 = yq1,
                     .yq2 = yq2,
                     .yq3 = yq3,
                     .xb0 = xb0,
                     .xb1 = xb1,
                     .xb2 = xb2};
    eye_render_rows(&style, &plot);
    eye_print_legend(&style);

    double snr_db = -1.0;
    int is_c4fm = (opts->mod_c4fm == 1);
    if (state) {
        is_c4fm = is_c4fm && (state->rf_mod == 0);
    }
#ifdef USE_RTLSDR
    if (is_c4fm) {
        snr_db = rtl_stream_get_snr_c4fm();
    }
#endif
    if (is_c4fm && snr_db < -20.0) {
        snr_db = eye_estimate_snr_fallback(buf, n, sps, two_sps, q1, q2, q3, snr_db);
    }
    if (is_c4fm && snr_db > -50.0) {
        ui_print_lborder();
        printw(" Rows: Q1=%d  Median=%d  Q3=%d   SPS=%d  SNR=%.1f dB\n", yq1, yq2, yq3, sps, snr_db);
    } else {
        ui_print_lborder();
        printw(" Rows: Q1=%d  Median=%d  Q3=%d   SPS=%d  SNR=n/a\n", yq1, yq2, yq3, sps);
    }
    attron(COLOR_PAIR(4));
    ui_print_hr();
    attroff(COLOR_PAIR(4));
#else
    ui_print_header("Eye Diagram");
    printw("| (RTL disabled in this build)\n");
    ui_print_hr();
#endif
}

#ifdef USE_RTLSDR
static int
fsk_hist_bucket_for_value(int v, int q1, int q2, int q3) {
    if (v <= q1) {
        return 0;
    }
    if (v <= q2) {
        return 1;
    }
    if (v <= q3) {
        return 2;
    }
    return 3;
}

static double
fsk_hist_dc_offset_norm(const float* buf, int n, float scale, int* peak_out) {
    int peak = 1;
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        float v = buf[i] * scale;
        int a = (int)lrintf(fabsf(v));
        if (a > peak) {
            peak = a;
        }
        sum += v;
    }
    if (peak < 64) {
        peak = 64;
    }
    *peak_out = peak;
    return (double)sum / (double)n / (double)peak;
}

static int
fsk_hist_collect_values(const float* buf, int n, float scale, int* vals) {
    int step = (n > 4096) ? (n / 4096) : 1;
    int m = (n + step - 1) / step;
    if (m < 8) {
        m = n;
        step = 1;
    }
    if (m > 8192) {
        m = 8192;
    }
    int vi = 0;
    for (int i = 0; i < n && vi < m; i += step) {
        vals[vi++] = (int)lrintf(buf[i] * scale);
    }
    return vi;
}

static void
fsk_hist_compute_quartiles(int* vals, int m, int* q1, int* q2, int* q3) {
    int idx1 = (int)((size_t)m / 4);
    int idx2 = (int)((size_t)m / 2);
    int idx3 = (int)((size_t)(3 * (size_t)m) / 4);
    *q2 = select_k_int_local(vals, m, idx2);
    *q1 = select_k_int_local(vals, idx2, idx1);
    *q3 = select_k_int_local(vals + idx2 + 1, m - (idx2 + 1), idx3 - (idx2 + 1));
}

static void
fsk_hist_count_bins(const float* buf, int n, float scale, int q1, int q2, int q3, int64_t bin[4]) {
    for (int i = 0; i < n; i++) {
        int v = (int)lrintf(buf[i] * scale);
        int b = fsk_hist_bucket_for_value(v, q1, q2, q3);
        bin[b]++;
    }
}

static void
fsk_hist_draw_ruler(int q1, int q2, int q3, int minv, int maxv) {
    enum { WR = 60 };

    char ruler[WR];
    for (int x = 0; x < WR; x++) {
        ruler[x] = '-';
    }
    int p1 = (int)lrint(((double)(q1 - minv) / (double)(maxv - minv)) * (double)(WR - 1));
    int p2 = (int)lrint(((double)(q2 - minv) / (double)(maxv - minv)) * (double)(WR - 1));
    int p3 = (int)lrint(((double)(q3 - minv) / (double)(maxv - minv)) * (double)(WR - 1));
    p1 = clamp_int_local(p1, 0, WR - 1);
    p2 = clamp_int_local(p2, 0, WR - 1);
    p3 = clamp_int_local(p3, 0, WR - 1);
    ruler[p1] = '|';
    ruler[p2] = '+';
    ruler[p3] = '|';
    ui_print_lborder();
    printw(" Ruler:  ");
    for (int x = 0; x < WR; x++) {
        addch(ruler[x]);
    }
    printw("  (Q1='|', Median='+', Q3='|')\n");
}

static void
fsk_hist_draw_bars(const int64_t bin[4], double dc_norm) {
    const int bar_width = 60;
    static const char* labels[4] = {"L3(-)", "L1(-)", "L1(+)", "L3(+)"};
    int64_t maxc = 1;
    for (int i = 0; i < 4; i++) {
        if (bin[i] > maxc) {
            maxc = bin[i];
        }
    }
    ui_print_lborder();
    printw(" DC Offset: %+0.2f%% of full-scale\n", dc_norm * 100.0);
    for (int i = 0; i < 4; i++) {
        int w = (int)((double)bin[i] / (double)maxc * (double)bar_width + 0.5);
        w = clamp_int_local(w, 0, bar_width);
        ui_print_lborder();
        printw(" %-6s ", labels[i]);
        for (int x = 0; x < w; x++) {
            addch('#');
        }
        for (int x = w; x < bar_width; x++) {
            addch(' ');
        }
        printw(" %lld\n", (long long)bin[i]);
    }
}

static void
print_fsk_hist_view_rtlsdr(void) {
    enum { MAXS = 8192 };

    static const float kHistFloatScale = 16384.0f;

    static float buf[(size_t)MAXS];
    static int vals[8192];
    int sps = 0;
    int n = rtl_stream_eye_get(buf, MAXS, &sps);
    UNUSED(sps);

    ui_print_header("FSK 4-Level Histogram");
    if (n <= 0) {
        ui_print_lborder();
        printw(" (no samples)\n");
        attron(COLOR_PAIR(4));
        ui_print_hr();
        attroff(COLOR_PAIR(4));
        return;
    }

    int peak = 1;
    double dc_norm = fsk_hist_dc_offset_norm(buf, n, kHistFloatScale, &peak);
    UNUSED(peak);

    int m = fsk_hist_collect_values(buf, n, kHistFloatScale, vals);
    if (m <= 0) {
        ui_print_lborder();
        printw(" (insufficient samples)\n");
        attron(COLOR_PAIR(4));
        ui_print_hr();
        attroff(COLOR_PAIR(4));
        return;
    }

    int q1 = 0;
    int q2 = 0;
    int q3 = 0;
    fsk_hist_compute_quartiles(vals, m, &q1, &q2, &q3);

    int64_t bin[4] = {0, 0, 0, 0};
    fsk_hist_count_bins(buf, n, kHistFloatScale, q1, q2, q3, bin);

    int minv = vals[0];
    int maxv = vals[m - 1];
    if (maxv == minv) {
        maxv = minv + 1;
    }
    fsk_hist_draw_ruler(q1, q2, q3, minv, maxv);
    fsk_hist_draw_bars(bin, dc_norm);
    attron(COLOR_PAIR(4));
    ui_print_hr();
    attroff(COLOR_PAIR(4));
}
#endif

void
print_fsk_hist_view(void) {
#ifdef USE_RTLSDR
    print_fsk_hist_view_rtlsdr();
#else
    ui_print_header("FSK 4-Level Histogram");
    printw("| (RTL disabled in this build)\n");
    ui_print_hr();
#endif
}

#ifdef USE_RTLSDR
static int
spectrum_nfft_clamped(void) {
    int nfft = rtl_stream_spectrum_get_size();
    if (nfft < 64) {
        nfft = 64;
    }
    if (nfft > 1024) {
        nfft = 1024;
    }
    return nfft;
}

static int
spectrum_grid_width(void) {
    int rows = 24;
    int cols = 80;
    getmaxyx(stdscr, rows, cols);
    UNUSED(rows);
    int w = cols - 4;
    if (w < 32) {
        w = 32;
    }
    if (w > 2048) {
        w = 2048;
    }
    return w;
}

static int
spectrum_grid_height(void) {
    int rows = 24;
    int cols = 80;
    getmaxyx(stdscr, rows, cols);
    UNUSED(cols);
    int h = rows / 3;
    if (h < 10) {
        h = 10;
    }
    return h;
}

static void
spectrum_resample_columns(const float* bins, int n, float* col, int w) {
    if (n >= w) {
        for (int x = 0; x < w; x++) {
            int i0 = (int)((long long)x * n / w);
            int i1 = (int)((long long)(x + 1) * n / w);
            if (i1 <= i0) {
                i1 = i0 + 1;
            }
            if (i1 > n) {
                i1 = n;
            }
            float s = -1e9f;
            for (int i = i0; i < i1; i++) {
                if (bins[i] > s) {
                    s = bins[i];
                }
            }
            col[x] = s;
        }
        return;
    }

    for (int x = 0; x < w; x++) {
        int src = (int)((long long)x * n / w);
        src = clamp_int_local(src, 0, n - 1);
        col[x] = bins[src];
    }
}

static void
spectrum_range(const float* col, int w, float* vmin, float* vmax, float* span) {
    float vmax_local = -1e9f;
    for (int x = 0; x < w; x++) {
        if (col[x] > vmax_local) {
            vmax_local = col[x];
        }
    }
    float vmin_local = vmax_local - 60.0f;
    float span_local = vmax_local - vmin_local;
    if (span_local < 1.0f) {
        span_local = 1.0f;
    }
    *vmin = vmin_local;
    *vmax = vmax_local;
    *span = span_local;
}

#ifdef PRETTY_COLORS
static short
spectrum_color_pair_for_level(float t) {
    if (t < 0.33f) {
        return 13;
    }
    if (t < 0.66f) {
        return 12;
    }
    return 11;
}
#endif

static void
spectrum_draw_cell(const dsd_opts* opts, int use_unicode, float v, float vmin, float vmax, float span, int y, int h) {
    float vc = clamp_float_local(v, vmin, vmax);
    float t = (vc - vmin) / span;
    int col_h = (int)lrintf(t * (float)(h - 1));
    int filled = (h - 1 - y) <= col_h;
#ifdef PRETTY_COLORS
    short cp = spectrum_color_pair_for_level(t);
    if (opts && opts->eye_color && has_colors()) {
        attron(COLOR_PAIR(cp));
    }
#endif
    if (filled) {
        if (use_unicode) {
            addstr("█");
        } else {
            addch('#');
        }
    } else {
        addch(' ');
    }
#ifdef PRETTY_COLORS
    if (opts && opts->eye_color && has_colors()) {
        attroff(COLOR_PAIR(cp));
    }
#endif
}

static void
spectrum_draw_plot(const dsd_opts* opts, const float* col, int w, int h, float vmin, float vmax, float span,
                   int use_unicode) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            spectrum_draw_cell(opts, use_unicode, col[x], vmin, vmax, span, y, h);
        }
        addch('\n');
    }
}

#ifdef PRETTY_COLORS
static void
spectrum_print_color_legend(int use_unicode) {
    printw("| Color:   ");
    attron(COLOR_PAIR(13));
    addstr(use_unicode ? "██" : "##");
    attroff(COLOR_PAIR(13));
    printw(" low  ");
    attron(COLOR_PAIR(12));
    addstr(use_unicode ? "██" : "##");
    attroff(COLOR_PAIR(12));
    printw(" mid  ");
    attron(COLOR_PAIR(11));
    addstr(use_unicode ? "██" : "##");
    attroff(COLOR_PAIR(11));
    printw(" high\n");
}
#endif

static void
spectrum_print_legend(const dsd_opts* opts, int rate, int w, int use_unicode, float vmax, float vmin) {
    float span_hz = (float)rate;
    int nfft2 = rtl_stream_spectrum_get_size();
    const char* df = use_unicode ? "\xCE\x94"
                                   "f"
                                 : "df";
    ui_print_lborder();
    printw(" Span: %.1f kHz  %s(FFT): %.1f Hz  %s(col): %.1f Hz  FFT: %d  Glyphs: %s%s\n", span_hz / 1000.0f, df,
           (rate > 0 && nfft2 > 0) ? (span_hz / (float)nfft2) : 0.0f, df,
           (rate > 0 && w > 0) ? (span_hz / (float)w) : 0.0f, nfft2, use_unicode ? "Unicode" : "ASCII",
           (opts && opts->eye_color && has_colors()) ? "; colored" : "");
    ui_print_lborder();
    printw(" Freq: -%.1fk   0   +%.1fk\n", (span_hz * 0.5f) / 1000.0f, (span_hz * 0.5f) / 1000.0f);
    ui_print_lborder();
    printw(" Scale: top=%.1f dB  floor=%.1f dB (relative)\n", vmax, vmin);
#ifdef PRETTY_COLORS
    if (opts && opts->eye_color && has_colors()) {
        spectrum_print_color_legend(use_unicode);
    }
#endif
    ui_print_hr();
}

static void
print_spectrum_view_rtlsdr(const dsd_opts* opts) {
    static float bins_static[1024];
    static float col[2048];

    int rate = 0;
    int nfft = spectrum_nfft_clamped();
    int n = rtl_stream_spectrum_get(bins_static, nfft, &rate);
    ui_print_header("Spectrum Analyzer");
    if (n <= 0) {
        printw("| (no spectrum yet)\n");
        ui_print_hr();
        return;
    }

    int w = spectrum_grid_width();
    int h = spectrum_grid_height();
    spectrum_resample_columns(bins_static, n, col, w);

    float vmin = 0.0f;
    float vmax = 0.0f;
    float span = 1.0f;
    spectrum_range(col, w, &vmin, &vmax, &span);

    int use_unicode = (opts && opts->eye_unicode && ui_unicode_supported());
    spectrum_draw_plot(opts, col, w, h, vmin, vmax, span, use_unicode);
    spectrum_print_legend(opts, rate, w, use_unicode, vmax, vmin);
}
#endif

void
print_spectrum_view(const dsd_opts* opts) {
#ifdef USE_RTLSDR
    print_spectrum_view_rtlsdr(opts);
#else
    UNUSED(opts);
    ui_print_header("Spectrum Analyzer");
    printw("| (RTL disabled in this build)\n");
    ui_print_hr();
#endif
}
