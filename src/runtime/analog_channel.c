// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Analog channel width ranges, strict parsing, validation and formatting.
 *
 * Pure arithmetic mirroring the DSP channel-filter design; see <dsd-neo/runtime/analog_channel.h>.
 */

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* RTL DSP bandwidths a user can select (rtl_bw_khz), in kHz, ascending. */
static const int kRtlDspBandwidthsKhz[] = {4, 6, 8, 12, 16, 24, 48};

enum { kRtlDspBandwidthCount = (int)(sizeof kRtlDspBandwidthsKhz / sizeof kRtlDspBandwidthsKhz[0]) };

static void
analog_error_clear(char* err, size_t err_size) {
    if (err && err_size > 0U) {
        err[0] = '\0';
    }
}

int
dsd_analog_demod_is_valid(int kind) {
    return (kind == DSD_ANALOG_DEMOD_FM || kind == DSD_ANALOG_DEMOD_AM) ? 1 : 0;
}

const char*
dsd_analog_demod_label(int kind) {
    switch (kind) {
        case DSD_ANALOG_DEMOD_FM: return "NFM";
        case DSD_ANALOG_DEMOD_AM: return "AM";
        default: return "?";
    }
}

int
dsd_analog_width_min_hz(int kind) {
    switch (kind) {
        case DSD_ANALOG_DEMOD_FM: return DSD_ANALOG_NFM_WIDTH_MIN_HZ;
        case DSD_ANALOG_DEMOD_AM: return DSD_ANALOG_AM_WIDTH_MIN_HZ;
        default: return 0;
    }
}

int
dsd_analog_width_max_hz(int kind) {
    switch (kind) {
        case DSD_ANALOG_DEMOD_FM: return DSD_ANALOG_NFM_WIDTH_MAX_HZ;
        case DSD_ANALOG_DEMOD_AM: return DSD_ANALOG_AM_WIDTH_MAX_HZ;
        default: return 0;
    }
}

int
dsd_analog_width_default_hz(int kind) {
    switch (kind) {
        case DSD_ANALOG_DEMOD_FM: return DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ;
        case DSD_ANALOG_DEMOD_AM: return DSD_ANALOG_AM_WIDTH_DEFAULT_HZ;
        default: return 0;
    }
}

int
dsd_analog_width_effective_hz(int kind, int configured_hz) {
    return configured_hz > 0 ? configured_hz : dsd_analog_width_default_hz(kind);
}

int
dsd_analog_width_in_range(int kind, int width_hz) {
    if (!dsd_analog_demod_is_valid(kind) || width_hz <= 0) {
        return 0;
    }
    return (width_hz >= dsd_analog_width_min_hz(kind) && width_hz <= dsd_analog_width_max_hz(kind)) ? 1 : 0;
}

/* "12.5" for 12500 Hz: kHz with trailing zeros dropped, no unit. */
static int
analog_format_khz_number(int hz, char* out, size_t out_size) {
    if (!out || out_size == 0U || hz < 0) {
        return -1;
    }
    const int whole = hz / 1000;
    int frac = hz % 1000;
    int digits = 3;
    while (digits > 0 && frac % 10 == 0) {
        frac /= 10;
        digits--;
    }
    const int n = (digits == 0) ? DSD_SNPRINTF(out, out_size, "%d", whole)
                                : DSD_SNPRINTF(out, out_size, "%d.%0*d", whole, digits, frac);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

int
dsd_analog_width_format(int width_hz, char* out, size_t out_size) {
    char number[DSD_ANALOG_WIDTH_TEXT_MAX];
    if (!out || out_size == 0U || analog_format_khz_number(width_hz, number, sizeof number) != 0) {
        return -1;
    }
    const int n = DSD_SNPRINTF(out, out_size, "%s kHz", number);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static int
analog_text_is_digits(const char* text) {
    if (!text || text[0] == '\0') {
        return 0;
    }
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    return 1;
}

int
dsd_analog_width_parse(int kind, const char* text, int* out_width_hz, char* err, size_t err_size) {
    analog_error_clear(err, err_size);
    const int want_text = (err && err_size > 0U) ? 1 : 0;
    if (!out_width_hz || !dsd_analog_demod_is_valid(kind)) {
        if (want_text) {
            DSD_SNPRINTF(err, err_size, "unknown analog demodulator");
        }
        return -1;
    }
    const char* label = dsd_analog_demod_label(kind);
    const int min_hz = dsd_analog_width_min_hz(kind);
    const int max_hz = dsd_analog_width_max_hz(kind);
    int value = 0;
    if (!analog_text_is_digits(text) || dsd_parse_int_strict(text, 10, 0, INT_MAX, &value) != 0) {
        if (want_text) {
            /* Echo a bounded prefix of the input so the message always fits DSD_ANALOG_ERROR_TEXT_MAX. */
            const char* shown = text ? text : "";
            const char* more = strlen(shown) > (size_t)DSD_ANALOG_PARSE_ECHO_MAX ? "..." : "";
            DSD_SNPRINTF(err, err_size, "%s bandwidth must be a whole number of Hz from %d to %d (got \"%.*s%s\")",
                         label, min_hz, max_hz, DSD_ANALOG_PARSE_ECHO_MAX, shown, more);
        }
        return -1;
    }
    if (!dsd_analog_width_in_range(kind, value)) {
        if (want_text) {
            DSD_SNPRINTF(err, err_size, "%s bandwidth %d Hz is outside the supported range of %d to %d Hz", label,
                         value, min_hz, max_hz);
        }
        return -1;
    }
    *out_width_hz = value;
    return 0;
}

int
dsd_analog_channel_taps_for_rate(int rate_hz) {
    if (rate_hz <= 0) {
        return 0;
    }
    /* dsd_firdes_compute_ntaps(): (int)(attenuation * Fs / (22 * transition)), bumped to odd. The quotient's
       denominator divides 26400, so integer division reproduces the double truncation exactly. */
    const int64_t numerator = (int64_t)DSD_ANALOG_CHANNEL_WINDOW_ATTENUATION_DB * (int64_t)rate_hz;
    const int64_t denominator = (int64_t)22 * (int64_t)DSD_ANALOG_CHANNEL_TRANSITION_HZ;
    int64_t taps = numerator / denominator;
    if ((taps & 1) == 0) {
        taps++;
    }
    return taps > (int64_t)INT_MAX ? INT_MAX : (int)taps;
}

int
dsd_analog_width_realizable(int width_hz, int rate_hz) {
    if (width_hz <= 0 || rate_hz <= 0) {
        return 0;
    }
    /* width/2 + guard <= (9/20) * rate  <=>  20 * (width + 2 * guard) <= 2 * 9 * rate, exact in integers. */
    const int64_t lhs =
        (int64_t)DSD_ANALOG_CHANNEL_CUTOFF_RATE_DEN * ((int64_t)width_hz + 2 * (int64_t)DSD_ANALOG_CHANNEL_GUARD_HZ);
    const int64_t rhs = 2 * (int64_t)DSD_ANALOG_CHANNEL_CUTOFF_RATE_NUM * (int64_t)rate_hz;
    if (lhs > rhs) {
        return 0;
    }
    return dsd_analog_channel_taps_for_rate(rate_hz) <= DSD_ANALOG_CHANNEL_MAX_TAPS ? 1 : 0;
}

int
dsd_analog_width_max_for_rate(int rate_hz) {
    if (rate_hz <= 0 || dsd_analog_channel_taps_for_rate(rate_hz) > DSD_ANALOG_CHANNEL_MAX_TAPS) {
        return 0;
    }
    const int64_t max_hz =
        ((int64_t)2 * DSD_ANALOG_CHANNEL_CUTOFF_RATE_NUM * (int64_t)rate_hz) / DSD_ANALOG_CHANNEL_CUTOFF_RATE_DEN
        - 2 * (int64_t)DSD_ANALOG_CHANNEL_GUARD_HZ;
    if (max_hz <= 0) {
        return 0;
    }
    return max_hz > (int64_t)INT_MAX ? INT_MAX : (int)max_hz;
}

/* "24 or 48" / "16, 24 or 48": every selectable RTL DSP bandwidth that fits @p width_hz, in kHz. */
static void
analog_format_fitting_bandwidths(int width_hz, char* out, size_t out_size) {
    int fitting[kRtlDspBandwidthCount];
    int count = 0;
    for (int i = 0; i < kRtlDspBandwidthCount; i++) {
        if (dsd_analog_width_realizable(width_hz, kRtlDspBandwidthsKhz[i] * 1000)) {
            fitting[count++] = kRtlDspBandwidthsKhz[i];
        }
    }
    out[0] = '\0';
    size_t used = 0U;
    for (int i = 0; i < count && used < out_size; i++) {
        const char* sep = (i == 0) ? "" : ((i + 1 == count) ? " or " : ", ");
        const int n = DSD_SNPRINTF(out + used, out_size - used, "%s%d", sep, fitting[i]);
        if (n < 0) {
            return;
        }
        used += (size_t)n;
    }
}

static void
analog_format_rate_error(int kind, int width_hz, int rate_hz, char* err, size_t err_size) {
    char width_text[DSD_ANALOG_WIDTH_TEXT_MAX];
    char rate_text[DSD_ANALOG_WIDTH_TEXT_MAX];
    char fits_text[64];
    (void)dsd_analog_width_format(width_hz, width_text, sizeof width_text);
    (void)dsd_analog_width_format(rate_hz, rate_text, sizeof rate_text);
    analog_format_fitting_bandwidths(width_hz, fits_text, sizeof fits_text);
    const int max_hz = dsd_analog_width_max_for_rate(rate_hz);
    if (max_hz <= 0 && dsd_analog_channel_taps_for_rate(rate_hz) > DSD_ANALOG_CHANNEL_MAX_TAPS) {
        DSD_SNPRINTF(err, err_size,
                     "%s bandwidth %s cannot be filtered at the %s DSP rate: the channel filter would need more than "
                     "%d taps; set the RTL DSP bandwidth to %s kHz",
                     dsd_analog_demod_label(kind), width_text, rate_text, DSD_ANALOG_CHANNEL_MAX_TAPS, fits_text);
        return;
    }
    char max_text[DSD_ANALOG_WIDTH_TEXT_MAX];
    (void)dsd_analog_width_format(max_hz, max_text, sizeof max_text);
    DSD_SNPRINTF(err, err_size,
                 "%s bandwidth %s does not fit the %s DSP rate (the largest width it fits is %s); set the RTL DSP "
                 "bandwidth to %s kHz",
                 dsd_analog_demod_label(kind), width_text, rate_text, max_text, fits_text);
}

static void
analog_format_range_error(int kind, int width_hz, char* err, size_t err_size) {
    char width_text[DSD_ANALOG_WIDTH_TEXT_MAX];
    char min_text[DSD_ANALOG_WIDTH_TEXT_MAX];
    char max_text[DSD_ANALOG_WIDTH_TEXT_MAX];
    (void)dsd_analog_width_format(width_hz > 0 ? width_hz : 0, width_text, sizeof width_text);
    (void)analog_format_khz_number(dsd_analog_width_min_hz(kind), min_text, sizeof min_text);
    (void)analog_format_khz_number(dsd_analog_width_max_hz(kind), max_text, sizeof max_text);
    DSD_SNPRINTF(err, err_size, "%s bandwidth %s is outside the supported range of %s to %s kHz",
                 dsd_analog_demod_label(kind), width_text, min_text, max_text);
}

int
dsd_analog_width_check(int kind, int width_hz, int rate_hz, char* err, size_t err_size) {
    analog_error_clear(err, err_size);
    const int want_text = (err && err_size > 0U) ? 1 : 0;
    if (!dsd_analog_demod_is_valid(kind)) {
        if (want_text) {
            DSD_SNPRINTF(err, err_size, "unknown analog demodulator");
        }
        return -1;
    }
    if (!dsd_analog_width_in_range(kind, width_hz)) {
        if (want_text) {
            analog_format_range_error(kind, width_hz, err, err_size);
        }
        return -1;
    }
    if (rate_hz <= 0) {
        if (want_text) {
            DSD_SNPRINTF(err, err_size, "%s bandwidth cannot be checked without a DSP rate",
                         dsd_analog_demod_label(kind));
        }
        return -1;
    }
    if (!dsd_analog_width_realizable(width_hz, rate_hz)) {
        if (want_text) {
            analog_format_rate_error(kind, width_hz, rate_hz, err, err_size);
        }
        return -1;
    }
    return 0;
}
