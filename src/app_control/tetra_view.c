// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/app_control/access_code_view.h>
#include <dsd-neo/app_control/tetra_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/core/utf16.h>
#include <stddef.h>
#include <stdint.h>

/* The ring holds DSD_TETRA_MESSAGE_TEXT_SIZE - 1 bytes of text; each can become a three-byte U+FFFD. */
_Static_assert((int)DSD_APP_TETRA_MESSAGE_SIZE >= 3 * ((int)DSD_TETRA_MESSAGE_TEXT_SIZE - 1) + 1,
               "a recent message fits dsd_app_tetra_message()'s buffer whatever it holds");

/* What goes between the parts of a row: a middle dot (U+00B7), as the Qt monitor joins its readings, or a hyphen on a
   terminal without UTF-8. */
static const char*
tetra_view_sep(unsigned flags) {
    return (flags & DSD_APP_TETRA_ASCII) != 0U ? " - " : " \xC2\xB7 ";
}

/* NULL for DSD_TETRA_MODE_NONE and anything this build does not know. */
static const char*
tetra_view_mode_name(uint8_t mode) {
    static const char* const names[] = {
        NULL,         "TMO downlink",     "TMO uplink",      "TMO control uplink",
        "DMO direct", "DMO via repeater", "DMO via gateway", "DMO via repeater/gateway",
    };
    _Static_assert(sizeof names / sizeof names[0] == (size_t)DSD_TETRA_MODE_DMO_REPEATER_GATEWAY + 1U,
                   "one name per working mode");
    return mode < sizeof names / sizeof names[0] ? names[mode] : NULL;
}

static int
tetra_view_mode_is_trunked(uint8_t mode) {
    return mode == DSD_TETRA_MODE_TMO_DOWNLINK || mode == DSD_TETRA_MODE_TMO_UPLINK
           || mode == DSD_TETRA_MODE_TMO_CONTROL_UPLINK;
}

/* "TMO downlink · encrypted (TEA2)": the mode, then whatever is known of the air-interface encryption. */
static void
tetra_view_fill_mode(dsd_app_tetra* out, const dsd_tetra_publication* pub, unsigned flags) {
    const char* const mode = tetra_view_mode_name(pub->mode);
    const char* const sep = tetra_view_sep(flags);
    char tea[8] = "";
    if (pub->tea >= 1U && pub->tea <= (uint8_t)DSD_TETRA_TEA_MAX) {
        DSD_SNPRINTF(tea, sizeof tea, "TEA%u", (unsigned)pub->tea);
    }
    out->encrypted = (uint8_t)(pub->air_encryption == DSD_TETRA_AIR_ENC_ENCRYPTED);
    if (out->encrypted) {
        DSD_SNPRINTF(out->mode_text, sizeof out->mode_text, "%s%sencrypted%s%s%s", mode, sep, tea[0] ? " (" : "", tea,
                     tea[0] ? ")" : "");
    } else if (pub->air_encryption == DSD_TETRA_AIR_ENC_CLEAR) {
        DSD_SNPRINTF(out->mode_text, sizeof out->mode_text, "%s%sclear", mode, sep);
    } else if (tea[0] != '\0') {
        DSD_SNPRINTF(out->mode_text, sizeof out->mode_text, "%s%s%s", mode, sep, tea);
    } else {
        DSD_SNPRINTF(out->mode_text, sizeof out->mode_text, "%s", mode);
    }
}

/* "MCC 262 · MNC 1001 · CC 25". */
static void
tetra_view_fill_network(dsd_app_tetra* out, const dsd_tetra_publication* pub, unsigned flags) {
    char cc[16];
    if (pub->net_known == 0U
        || dsd_app_access_code_format(DSD_ACCESS_CODE_COLOR_CODE, pub->colour, DSD_APP_ACCESS_CODE_SHORT, cc, sizeof cc)
               != 1) {
        return;
    }
    const char* const sep = tetra_view_sep(flags);
    DSD_SNPRINTF(out->network_text, sizeof out->network_text, "MCC %u%sMNC %u%s%s", (unsigned)pub->mcc, sep,
                 (unsigned)pub->mnc, sep, cc);
    out->network_known = 1U;
}

/* A downlink frequency in MHz, five decimals with the fifth dropped when it is a zero ("392.0125 MHz",
   "769.76875 MHz"): 6.25 kHz channel plans put real frequencies on the fifth decimal. Integer arithmetic, rounded to
   the nearest 10 Hz. */
static void
tetra_view_mhz(uint32_t hz, char* out, size_t out_size) {
    const uint64_t tens = ((uint64_t)hz + 5U) / 10U;
    const unsigned mhz = (unsigned)(tens / 100000U);
    const unsigned frac = (unsigned)(tens % 100000U);
    if (frac % 10U == 0U) {
        DSD_SNPRINTF(out, out_size, "%u.%04u MHz", mhz, frac / 10U);
    } else {
        DSD_SNPRINTF(out, out_size, "%u.%05u MHz", mhz, frac);
    }
}

/* "1,3" from the Timeslot assigned bitmap: bit 3 is timeslot 1, bit 0 timeslot 4. */
static void
tetra_view_slots(uint8_t slots, char* out, size_t out_size) {
    size_t used = 0U;
    out[0] = '\0';
    for (unsigned tn = 1U; tn <= 4U; ++tn) {
        if ((slots & (1U << (4U - tn))) == 0U) {
            continue;
        }
        const int n = DSD_SNPRINTF(out + used, out_size - used, "%s%u", used > 0U ? "," : "", tn);
        if (n < 0 || (size_t)n >= out_size - used) {
            break;
        }
        used += (size_t)n;
    }
}

/* "carrier 321 · 392.0125 MHz", the frequency only once it is resolved. */
static void
tetra_view_carrier(char* out, size_t out_size, unsigned carrier, const char* slots, uint32_t hz, unsigned flags) {
    const char* const sep = tetra_view_sep(flags);
    char freq[24] = "";
    if (hz != 0U) {
        tetra_view_mhz(hz, freq, sizeof freq);
    }
    DSD_SNPRINTF(out, out_size, "carrier %u%s%s%s%s%s", carrier, slots[0] ? sep : "", slots[0] ? "TS " : "", slots,
                 freq[0] ? sep : "", freq);
}

static void
tetra_view_fill_channels(dsd_app_tetra* out, const dsd_opts* opts, const dsd_tetra_publication* pub, unsigned flags) {
    if (!out->trunked) {
        return;
    }
    if (opts->trunk_enable != 0 && pub->mode == DSD_TETRA_MODE_TMO_DOWNLINK) {
        static const char* const states[] = {"idle", "control channel", "traffic channel"};
        const uint8_t state = pub->trunk_state < sizeof states / sizeof states[0] ? pub->trunk_state : 0U;
        out->trunk_visible = 1U;
        out->trunk_state = state;
        out->on_traffic = (uint8_t)(state == DSD_TETRA_TRUNK_ON_TRAFFIC);
        DSD_SNPRINTF(out->trunk_text, sizeof out->trunk_text, "%s", states[state]);
    }
    if (pub->cc_known != 0U) {
        tetra_view_carrier(out->control_text, sizeof out->control_text, pub->cc_carrier, "", pub->cc_freq_hz, flags);
        out->control_known = 1U;
    }
    if (pub->traffic_valid != 0U) {
        char slots[8];
        tetra_view_slots(pub->traffic_slots, slots, sizeof slots);
        tetra_view_carrier(out->traffic_text, sizeof out->traffic_text, pub->traffic_carrier, slots,
                           pub->traffic_freq_hz, flags);
        out->traffic_known = 1U;
    }
}

/* The Status downlink values of D-MM STATUS (EN 300 392-2) the decoder names; every other value is reserved. */
static const char*
tetra_view_mm_status_name(uint8_t code) {
    switch (code) {
        case 1: return "change energy saving request";
        case 2: return "change energy saving response";
        case 3: return "dual watch response";
        case 4: return "terminate dual watch response";
        case 5: return "change dual watch request";
        case 7: return "MS frequency bands request";
        case 8: return "periodic distance reporting";
        default: return "reserved";
    }
}

static void
tetra_view_fill_mm_status(dsd_app_tetra* out, const dsd_tetra_publication* pub, unsigned flags) {
    if (pub->mm_status_valid == 0U) {
        return;
    }
    DSD_SNPRINTF(out->mm_status_text, sizeof out->mm_status_text, "%u%s%s", (unsigned)pub->mm_status_code,
                 tetra_view_sep(flags), tetra_view_mm_status_name(pub->mm_status_code));
    out->mm_status_known = 1U;
}

/* "ready · 18 frames · 2 errors"; the counts only once there are any. */
static void
tetra_view_fill_codec(dsd_app_tetra* out, const dsd_tetra_publication* pub, unsigned flags) {
    static const char* const words[] = {NULL, "ready", "missing", "failed to load", "decode errors"};
    if (pub->codec_status >= sizeof words / sizeof words[0] || words[pub->codec_status] == NULL) {
        return;
    }
    const char* const word = words[pub->codec_status];
    out->codec_known = 1U;
    out->codec_status = pub->codec_status;
    out->codec_fault = (uint8_t)(pub->codec_status != DSD_TETRA_CODEC_READY);
    if (pub->codec_frames == 0U && pub->codec_errors == 0U) {
        DSD_SNPRINTF(out->codec_text, sizeof out->codec_text, "%s", word);
        return;
    }
    const char* const sep = tetra_view_sep(flags);
    DSD_SNPRINTF(out->codec_text, sizeof out->codec_text, "%s%s%u frames%s%u errors", word, sep,
                 (unsigned)pub->codec_frames, sep, (unsigned)pub->codec_errors);
}

int
dsd_app_tetra_view(const dsd_opts* opts, const dsd_state* state, unsigned flags, dsd_app_tetra* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!opts || !state) {
        return -1;
    }
    const dsd_tetra_publication* const pub = &state->tetra_pub;
    out->generation = pub->generation;
    if (opts->frame_tetra == 0 || tetra_view_mode_name(pub->mode) == NULL) {
        return 0;
    }
    out->visible = 1U;
    out->mode = pub->mode;
    out->trunked = (uint8_t)tetra_view_mode_is_trunked(pub->mode);
    tetra_view_fill_mode(out, pub, flags);
    tetra_view_fill_network(out, pub, flags);
    tetra_view_fill_channels(out, opts, pub, flags);
    tetra_view_fill_mm_status(out, pub, flags);
    tetra_view_fill_codec(out, pub, flags);
    return 1;
}

/* One UTF-8 sequence at @p s, of which @p avail bytes may be read: its scalar value and length, or U+FFFD and 1 for a
   byte that does not start a valid sequence (a stray continuation byte, an overlong form, a surrogate, a value past
   U+10FFFF, or a sequence cut short by the end of the text). */
static size_t
tetra_view_utf8_next(const unsigned char* s, size_t avail, uint32_t* scalar) {
    static const uint32_t min_value[5] = {0U, 0U, 0x80U, 0x800U, 0x10000U};
    const unsigned char lead = s[0];
    size_t len = 4U;
    if (lead < 0x80U) {
        *scalar = lead;
        return 1U;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        len = 2U;
    } else if ((lead & 0xF0U) == 0xE0U) {
        len = 3U;
    } else if ((lead & 0xF8U) != 0xF0U) {
        len = 0U;
    }
    *scalar = DSD_UNICODE_REPLACEMENT;
    if (len == 0U || len > avail) {
        return 1U;
    }
    uint32_t value = lead & (0x7FU >> len);
    for (size_t i = 1U; i < len; ++i) {
        if ((s[i] & 0xC0U) != 0x80U) {
            return 1U;
        }
        value = (value << 6) | (uint32_t)(s[i] & 0x3FU);
    }
    if (value < min_value[len] || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
        return 1U;
    }
    *scalar = value;
    return len;
}

/* Copy @p in (at most @p in_size bytes, up to its terminator) to @p out as printable text: controls become a space,
   invalid UTF-8 U+FFFD, and with DSD_APP_TETRA_ASCII everything outside ASCII '?'. Cut at a character boundary. */
static void
tetra_view_printable(const char* in, size_t in_size, unsigned flags, size_t max_columns, char* out, size_t out_size) {
    size_t used = 0U;
    size_t columns = 0U;
    size_t i = 0U;
    while (i < in_size && in[i] != '\0' && (max_columns == 0U || columns < max_columns)) {
        uint32_t scalar = 0U;
        i += tetra_view_utf8_next((const unsigned char*)in + i, in_size - i, &scalar);
        if (dsd_unicode_scalar_is_control(scalar)) {
            scalar = ' ';
        } else if ((flags & DSD_APP_TETRA_ASCII) != 0U && scalar >= 0x80U) {
            scalar = '?';
        }
        char utf8[DSD_UTF8_MAX_BYTES + 1];
        const size_t n = dsd_utf8_encode_scalar(scalar, utf8, sizeof utf8);
        if (n == 0U || used + n >= out_size) {
            break;
        }
        DSD_MEMCPY(out + used, utf8, n);
        used += n;
        columns++;
    }
    out[used] = '\0';
}

int
dsd_app_tetra_message(const dsd_state* state, unsigned back, unsigned flags, size_t max_columns, char* out,
                      size_t out_size) {
    if (!state || !out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    const uint32_t sequence = state->tetra_message_sequence;
    const uint32_t held = sequence < DSD_TETRA_MESSAGE_CAPACITY ? sequence : DSD_TETRA_MESSAGE_CAPACITY;
    if (back >= held) {
        return 0;
    }
    const dsd_tetra_message* const item = &state->tetra_messages[(sequence - 1U - back) % DSD_TETRA_MESSAGE_CAPACITY];
    tetra_view_printable(item->text, sizeof item->text, flags, max_columns, out, out_size);
    return 1;
}
