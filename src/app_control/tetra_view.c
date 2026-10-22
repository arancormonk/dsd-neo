// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/app_control/access_code_view.h>
#include <dsd-neo/app_control/tetra_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/tetra_publication.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

/* Append @p text to @p out (capacity @p cap, @p used bytes long) after @p sep when it is not the first part. Returns 0
 * when it does not fit, leaving @p out as it was. */
static int
tetra_view_append(char* out, size_t cap, size_t* used, const char* sep, const char* text) {
    const int n = DSD_SNPRINTF(out + *used, cap - *used, "%s%s", *used > 0U ? sep : "", text);
    if (n < 0 || (size_t)n >= cap - *used) {
        out[*used] = '\0';
        return 0;
    }
    *used += (size_t)n;
    return 1;
}

/* "TEA1/TEA2", "proprietary KSG 9", "TEA4/KSG 5": every KSG number of @p mask (EN 300 392-7 table 6.2), as many as fit
 * in @p out, "/..." for the rest. A name other than the last leaves room for that marker. */
static void
tetra_view_ksg_names(uint16_t mask, char* out, size_t cap) {
    static const char k_more[] = "/...";
    size_t used = 0U;
    out[0] = '\0';
    if (cap <= sizeof k_more) {
        return;
    }
    const size_t room = cap - (sizeof k_more - 1U);
    for (unsigned k = 0U; k < 16U; k++) {
        if (((mask >> k) & 1U) == 0U) {
            continue;
        }
        char name[24];
        if (k < 4U) {
            DSD_SNPRINTF(name, sizeof name, "TEA%u", k + 1U);
        } else {
            DSD_SNPRINTF(name, sizeof name, "%sKSG %u", k >= 8U ? "proprietary " : "", k);
        }
        const int last = (mask >> (k + 1U)) == 0U;
        if (!tetra_view_append(out, last ? cap : room, &used, "/", name)) {
            (void)tetra_view_append(out, cap, &used, "/", "...");
            return;
        }
    }
}

/* "TMO downlink · encrypted (TEA2)": the mode, then whatever is known of the air-interface encryption, and the
   algorithms signalling named whatever the last PDU's encryption ("TMO downlink · clear · TEA2"). */
static void
tetra_view_fill_mode(dsd_app_tetra* out, const dsd_tetra_publication* pub, unsigned flags) {
    const char* const mode = tetra_view_mode_name(pub->mode);
    const char* const sep = tetra_view_sep(flags);
    if (!mode) {
        return;
    }
    /* The algorithms take what the row leaves them after its longest form, "<mode><sep>encrypted (...)". */
    const size_t fixed = strlen(mode) + strlen(sep) + sizeof("encrypted ()") - 1U;
    char tea[DSD_APP_TETRA_TEXT_SIZE];
    tetra_view_ksg_names(pub->ksg_mask, tea, fixed < sizeof tea ? sizeof tea - fixed : 1U);
    out->encrypted = (uint8_t)(pub->air_encryption == DSD_TETRA_AIR_ENC_ENCRYPTED);
    if (out->encrypted) {
        DSD_SNPRINTF(out->mode_text, sizeof out->mode_text, "%s%sencrypted%s%s%s", mode, sep, tea[0] ? " (" : "", tea,
                     tea[0] ? ")" : "");
    } else if (pub->air_encryption == DSD_TETRA_AIR_ENC_CLEAR) {
        DSD_SNPRINTF(out->mode_text, sizeof out->mode_text, "%s%sclear%s%s", mode, sep, tea[0] ? sep : "", tea);
    } else if (tea[0] != '\0') {
        DSD_SNPRINTF(out->mode_text, sizeof out->mode_text, "%s%s%s", mode, sep, tea);
    } else {
        DSD_SNPRINTF(out->mode_text, sizeof out->mode_text, "%s", mode);
    }
}

/* The security class(es) a cell offers air interface encryption under (EN 300 392-7 table A.104: class 1 beside class
 * 2 or class 3). */
static const char*
tetra_view_cell_classes(const dsd_tetra_publication* pub) {
    if (pub->sec_class3) {
        return pub->sec_class1 ? "classes 1 and 3" : "class 3";
    }
    return pub->sec_class1 ? "classes 1 and 2" : "class 2";
}

/* Trunked mode: "class 2 · SCKN 3 · authentication · TEA1 reduced key". The class bits mean something only on a cell
   whose BS service details offer air interface encryption (table A.104 note 1). */
static void
tetra_view_fill_cell_security(dsd_app_tetra* out, const dsd_tetra_publication* pub, const char* sep) {
    char* const row = out->security_text;
    const size_t cap = sizeof out->security_text;
    size_t used = 0U;
    char part[24];
    if (pub->aie == DSD_TETRA_AIE_UNAVAILABLE) {
        (void)tetra_view_append(row, cap, &used, sep, "no air encryption");
    } else if (pub->aie == DSD_TETRA_AIE_AVAILABLE) {
        (void)tetra_view_append(row, cap, &used, sep,
                                pub->sec_known ? tetra_view_cell_classes(pub) : "air encryption available");
        if (pub->sec_known && !pub->sec_class3 && pub->sec_sckn != 0U) {
            DSD_SNPRINTF(part, sizeof part, "SCKN %u", (unsigned)pub->sec_sckn);
            (void)tetra_view_append(row, cap, &used, sep, part);
        }
    }
    if (pub->sec_known && pub->sec_auth) {
        (void)tetra_view_append(row, cap, &used, sep, "authentication");
    }
    if (pub->key_kind == DSD_TETRA_KEY_TEA1_REDUCED) {
        (void)tetra_view_append(row, cap, &used, sep, "TEA1 reduced key");
    } else if (pub->key_kind == DSD_TETRA_KEY_CK && pub->key_tea >= 1U && pub->key_tea <= 3U) {
        DSD_SNPRINTF(part, sizeof part, "TEA%u key", (unsigned)pub->key_tea);
        (void)tetra_view_append(row, cap, &used, sep, part);
    }
}

/* Direct mode: the transmission's security class (EN 300 396-3 clause 9.3.2) and SCKN, "DM-2-A · SCKN 5". */
static void
tetra_view_fill_dmo_security(dsd_app_tetra* out, const dsd_tetra_publication* pub, const char* sep) {
    static const char* const k_classes[] = {NULL, "DM-1", "DM-2-A", "DM-2-B", "DM-2-C"};
    if (pub->dmo_class >= sizeof k_classes / sizeof k_classes[0] || k_classes[pub->dmo_class] == NULL) {
        return;
    }
    if (pub->dmo_class != DSD_TETRA_DMO_CLASS_DM1 && pub->dmo_sckn != 0U) {
        DSD_SNPRINTF(out->security_text, sizeof out->security_text, "%s%sSCKN %u", k_classes[pub->dmo_class], sep,
                     (unsigned)pub->dmo_sckn);
    } else {
        DSD_SNPRINTF(out->security_text, sizeof out->security_text, "%s", k_classes[pub->dmo_class]);
    }
}

static void
tetra_view_fill_security(dsd_app_tetra* out, const dsd_tetra_publication* pub, unsigned flags) {
    const char* const sep = tetra_view_sep(flags);
    if (out->trunked) {
        tetra_view_fill_cell_security(out, pub, sep);
    } else {
        tetra_view_fill_dmo_security(out, pub, sep);
    }
    out->security_known = (uint8_t)(out->security_text[0] != '\0');
}

/* "MCC 262 · MNC 1001 · LA 4097 · CC 25 · carrier 654": the network a CRC-checked BSCH, or a CRC-checked downlink block
   descrambled with it, proved on this carrier (the received identity record, never a --tetra-net preset or a network
   the descrambler kept on their own), its location area and colour code once received, and the carrier once known. */
static void
tetra_view_fill_network(dsd_app_tetra* out, const dsd_state* state, unsigned flags) {
    const dsd_received_identity* const id = &state->rx_id;
    if (id->family != (uint8_t)DSD_RXID_FAMILY_TETRA || (id->valid & DSD_RXID_TETRA_NET) == 0U) {
        return;
    }
    const dsd_tetra_publication* const pub = &state->tetra_pub;
    const char* const sep = tetra_view_sep(flags);
    char cc[24] = "";
    char code[16];
    if ((id->valid & DSD_RXID_ACCESS) != 0U
        && dsd_app_access_code_format(id->access_kind, id->access, DSD_APP_ACCESS_CODE_SHORT, code, sizeof code) == 1) {
        DSD_SNPRINTF(cc, sizeof cc, "%s%s", sep, code);
    }
    char la[24] = "";
    char carrier[24] = "";
    if ((id->valid & DSD_RXID_TETRA_LA) != 0U) {
        DSD_SNPRINTF(la, sizeof la, "%sLA %u", sep, (unsigned)id->tetra_la);
    }
    if (pub->rx_carrier_known != 0U) {
        DSD_SNPRINTF(carrier, sizeof carrier, "%scarrier %u", sep, (unsigned)pub->rx_carrier);
    }
    DSD_SNPRINTF(out->network_text, sizeof out->network_text, "MCC %u%sMNC %u%s%s%s", (unsigned)id->tetra_mcc, sep,
                 (unsigned)id->tetra_mnc, la, cc, carrier);
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
    static const char* const words[] = {NULL, "ready", "decode errors"};
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
    tetra_view_fill_security(out, pub, flags);
    tetra_view_fill_network(out, state, flags);
    tetra_view_fill_channels(out, opts, pub, flags);
    tetra_view_fill_mm_status(out, pub, flags);
    tetra_view_fill_codec(out, pub, flags);
    return 1;
}
