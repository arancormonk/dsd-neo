// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The standard 50-tone EIA/TIA CTCSS table in tenths of a hertz, ascending. 150.0 Hz is the
 * tone some radios offer as a 51st; it is left out on purpose, because it sits 1.4 Hz from
 * 151.4 Hz and a detector that snapped it to the table would name the wrong tone.
 */
static const uint16_t k_ctcss_tones_tenths[] = {
    670,  693,  719,  744,  770,  797,  825,  854,  885,  915,  948,  974,  1000, 1035, 1072, 1109, 1148,
    1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799,
    1835, 1862, 1899, 1928, 1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};

_Static_assert(sizeof(k_ctcss_tones_tenths) / sizeof(k_ctcss_tones_tenths[0]) == DSD_CTCSS_TONE_COUNT,
               "the CTCSS table holds exactly the standard 50 tones");

int
dsd_ctcss_tone_count(void) {
    return DSD_CTCSS_TONE_COUNT;
}

int
dsd_ctcss_tone_tenths(int index) {
    if (index < 0 || index >= DSD_CTCSS_TONE_COUNT) {
        return -1;
    }
    return (int)k_ctcss_tones_tenths[index];
}

int
dsd_ctcss_tone_index(int tenths_hz) {
    int lo = 0;
    int hi = DSD_CTCSS_TONE_COUNT - 1;
    while (lo <= hi) {
        const int mid = lo + ((hi - lo) / 2);
        const int value = (int)k_ctcss_tones_tenths[mid];
        if (value == tenths_hz) {
            return mid;
        }
        if (value < tenths_hz) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

static int
ctcss_format_with(const char* prefix, const char* suffix, int tenths_hz, char* buf, size_t buf_size) {
    if (buf == NULL || buf_size == 0U) {
        return -1;
    }
    buf[0] = '\0';
    if (tenths_hz < 1 || tenths_hz > 9999) {
        return -1;
    }
    const int written = DSD_SNPRINTF(buf, buf_size, "%s%d.%d%s", prefix, tenths_hz / 10, tenths_hz % 10, suffix);
    if (written < 0 || (size_t)written >= buf_size) {
        buf[0] = '\0';
        return -1;
    }
    return written;
}

int
dsd_ctcss_format(int tenths_hz, char* buf, size_t buf_size) {
    return ctcss_format_with("", "", tenths_hz, buf, buf_size);
}

int
dsd_ctcss_format_label(int tenths_hz, char* buf, size_t buf_size) {
    return ctcss_format_with("CTCSS ", " Hz", tenths_hz, buf, buf_size);
}

/* ------------------------------------------------------------------------------------------
 * DCS (issue #523)
 * ---------------------------------------------------------------------------------------- */

/* The standard 104-code DCS set, ascending, each code as its value (023 octal = 19). */
static const uint16_t k_dcs_codes[] = {
    0023, 0025, 0026, 0031, 0032, 0036, 0043, 0047, 0051, 0053, 0054, 0065, 0071, 0072, 0073, 0074, 0114, 0115,
    0116, 0122, 0125, 0131, 0132, 0134, 0143, 0145, 0152, 0155, 0156, 0162, 0165, 0172, 0174, 0205, 0212, 0223,
    0225, 0226, 0243, 0244, 0245, 0246, 0251, 0252, 0255, 0261, 0263, 0265, 0266, 0271, 0274, 0306, 0311, 0315,
    0325, 0331, 0332, 0343, 0346, 0351, 0356, 0364, 0365, 0371, 0411, 0412, 0413, 0423, 0431, 0432, 0445, 0446,
    0452, 0454, 0455, 0462, 0464, 0465, 0466, 0503, 0506, 0516, 0523, 0526, 0532, 0546, 0565, 0606, 0612, 0624,
    0627, 0631, 0632, 0654, 0662, 0664, 0703, 0712, 0723, 0731, 0732, 0734, 0743, 0754,
};

_Static_assert(sizeof(k_dcs_codes) / sizeof(k_dcs_codes[0]) == DSD_DCS_CODE_COUNT,
               "the DCS table holds exactly the standard 104 codes");

/* Golay (23,12) generator x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1, bit i the coefficient of x^i. */
#define DCS_GOLAY_GENERATOR 0xC75U
#define DCS_WORD_MASK       ((1U << DSD_DCS_WORD_BITS) - 1U)
/* The fixed data bits 9-11, "100" as printed most significant first: only bit 11 set. */
#define DCS_MARKER          0x800U
#define DCS_MARKER_FIELD    0xE00U

int
dsd_dcs_code_count(void) {
    return DSD_DCS_CODE_COUNT;
}

int
dsd_dcs_code(int index) {
    if (index < 0 || index >= DSD_DCS_CODE_COUNT) {
        return -1;
    }
    return (int)k_dcs_codes[index];
}

int
dsd_dcs_code_index(int code) {
    int lo = 0;
    int hi = DSD_DCS_CODE_COUNT - 1;
    while (lo <= hi) {
        const int mid = lo + ((hi - lo) / 2);
        const int value = (int)k_dcs_codes[mid];
        if (value == code) {
            return mid;
        }
        if (value < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/* Remainder of a(x) modulo the generator, for a(x) of degree below 23. */
static uint32_t
dcs_golay_remainder(uint32_t a) {
    for (int bit = DSD_DCS_WORD_BITS - 1; bit >= 11; bit--) {
        if ((a >> bit) & 1U) {
            a ^= DCS_GOLAY_GENERATOR << (bit - 11);
        }
    }
    return a;
}

/*
 * The word is d(x) + x^12 p(x) for the 12 data bits d and the 11 check bits p, and must be a
 * multiple of g(x). g(x) divides x^23 + 1, so x^23 = 1 modulo g, and multiplying
 * x^12 p(x) = d(x) (mod g) by x^11 gives p(x) = x^11 d(x) mod g.
 */
uint32_t
dsd_dcs_word(int code, int inverted) {
    if (code < 0 || code > DSD_DCS_CODE_MAX) {
        return 0U;
    }
    const uint32_t data = (uint32_t)code | DCS_MARKER;
    const uint32_t word = data | (dcs_golay_remainder(data << 11) << 12);
    return inverted ? (~word & DCS_WORD_MASK) : word;
}

static uint32_t
dcs_rotate_right(uint32_t word, int k) {
    if (k == 0) {
        return word;
    }
    return ((word >> k) | (word << (DSD_DCS_WORD_BITS - k))) & DCS_WORD_MASK;
}

/* The supported code whose word @p word is, or -1. */
static int
dcs_supported_code_of(uint32_t word) {
    if ((word & DCS_MARKER_FIELD) != DCS_MARKER) {
        return -1;
    }
    const int code = (int)(word & 0x1FFU);
    if (dsd_dcs_code_index(code) < 0 || dsd_dcs_word(code, 0) != word) {
        return -1;
    }
    return code;
}

int
dsd_dcs_match(uint32_t window, int* code, int* inverted) {
    window &= DCS_WORD_MASK;
    int best_code = -1;
    int best_inverted = 1;
    for (int k = 0; k < DSD_DCS_WORD_BITS; k++) {
        const uint32_t rotated = dcs_rotate_right(window, k);
        for (int inv = 0; inv < 2; inv++) {
            const int found = dcs_supported_code_of(inv ? (~rotated & DCS_WORD_MASK) : rotated);
            if (found < 0) {
                continue;
            }
            /* Normal polarity first, then the lowest code. */
            if (best_code < 0 || inv < best_inverted || (inv == best_inverted && found < best_code)) {
                best_code = found;
                best_inverted = inv;
            }
        }
    }
    if (best_code < 0) {
        return 0;
    }
    if (code) {
        *code = best_code;
    }
    if (inverted) {
        *inverted = best_inverted;
    }
    return 1;
}

int
dsd_dcs_canonical(int code, int inverted, int* canon_code, int* canon_inverted) {
    const uint32_t word = dsd_dcs_word(code, inverted);
    if (word == 0U || !dsd_dcs_match(word, canon_code, canon_inverted)) {
        return -1;
    }
    return 0;
}

static int
dcs_format_with(const char* prefix, int code, int inverted, char* buf, size_t buf_size) {
    if (buf == NULL || buf_size == 0U) {
        return -1;
    }
    buf[0] = '\0';
    if (code < 0 || code > DSD_DCS_CODE_MAX) {
        return -1;
    }
    const int written = DSD_SNPRINTF(buf, buf_size, "%sD%03o%c", prefix, (unsigned int)code, inverted ? 'I' : 'N');
    if (written < 0 || (size_t)written >= buf_size) {
        buf[0] = '\0';
        return -1;
    }
    return written;
}

int
dsd_dcs_format(int code, int inverted, char* buf, size_t buf_size) {
    return dcs_format_with("", code, inverted, buf, buf_size);
}

int
dsd_dcs_format_label(int code, int inverted, char* buf, size_t buf_size) {
    return dcs_format_with("DCS ", code, inverted, buf, buf_size);
}

/* RTL stream output kind that carries monitor audio: RTL_STREAM_OUTPUT_AUDIO_MONITOR in the IO
   header runtime may not include. */
enum { ANALOG_TONES_RTL_OUTPUT_AUDIO_MONITOR = 0 };

static int
analog_tone_input_carries_audio(const dsd_opts* opts) {
    switch (opts->audio_in_type) {
        case AUDIO_IN_PULSE:
        case AUDIO_IN_STDIN:
        case AUDIO_IN_WAV:
        case AUDIO_IN_UDP:
        case AUDIO_IN_TCP: return 1;
        case AUDIO_IN_RTL: return dsd_rtl_stream_metrics_hook_output_kind() == ANALOG_TONES_RTL_OUTPUT_AUDIO_MONITOR;
        default: return 0;
    }
}

int
dsd_analog_tone_detection_active(const dsd_opts* opts) {
    if (opts == NULL || opts->analog_only != 1 || opts->monitor_input_audio != 1) {
        return 0;
    }
    return analog_tone_input_carries_audio(opts);
}
