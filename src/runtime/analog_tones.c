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
