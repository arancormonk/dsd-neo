// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA DMO (Direct Mode Operation) sync-burst and MAC helpers.
 * ETSI EN 300 396-2 V1.4.1 clauses 8.2.5 / 9.4.3, EN 300 396-3 V1.4.1 clauses
 * 8.2.4 / 9.1 / 9.2 / 9.3.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/tetra_etee.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_messages.h>

#include <stdio.h>
#include <string.h>
#include "tetra_state.h"

int
tetra_dmo_system_code_is_dmo(const uint8_t* sch_s_bits) {
    if (!sch_s_bits) {
        return 0;
    }
    /* 0xxx = TMO, 100y reserved, 1010..1111 = DMO (EN 300 396-3 clause 9.3.29). */
    return tetra_bits_to_uint(sch_s_bits, 0, 4) >= 0xAu;
}

int
tetra_dmo_parse_sync_s(const uint8_t* bits, tetra_dmo_sync_s_t* out) {
    if (!bits || !out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->system_code = (uint8_t)tetra_bits_to_uint(bits, 0, 4);
    out->sync_pdu_type = (uint8_t)tetra_bits_to_uint(bits, 4, 2);
    out->comm_type = (uint8_t)tetra_bits_to_uint(bits, 6, 2);
    /* Bit 8 is the master/slave link flag (comm 01/11) or reserved; bit 9 is
     * the gateway flag (comm 10/11) or reserved. */
    out->ms_link_flag = bits[8] & 1u;
    out->gw_flag = bits[9] & 1u;
    out->ab_usage = (uint8_t)tetra_bits_to_uint(bits, 10, 2);
    out->slot = (uint8_t)(tetra_bits_to_uint(bits, 12, 2) + 1u);
    out->frame = (uint8_t)tetra_bits_to_uint(bits, 14, 5);
    out->enc_state = (uint8_t)tetra_bits_to_uint(bits, 19, 2);
    return 1;
}

static int
take(const uint8_t* bits, int len, int* pos, int n) {
    int v;
    if (!bits || *pos + n > len) {
        return -1;
    }
    v = (int)tetra_bits_to_uint(bits, *pos, n);
    *pos += n;
    return v;
}

int
tetra_dmo_parse_presence(const uint8_t* sch_s, const uint8_t* sch_h, tetra_dmo_sync_s_t* s, tetra_dmo_pres_t* out) {
    int pos = 8; /* system code, SYNC PDU type and communication type already consumed */
    int v;
    if (!sch_s || !s || !out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (s->comm_type < 1 || s->comm_type > 3) {
        return 0;
    }
    out->comm_type = s->comm_type;

    if (s->comm_type != 1) {
        /* Gateway DPRES-SYNC (EN 300 396-5 table 5). Conditional elements
         * always occupy the same number of bits, so the cursor is fixed. */
        int dmrep;
        int two_present;
        int two = 0;
        if (take(sch_s, 60, &pos, 1) < 0) {
            return 0; /* M-DMO flag */
        }
        if ((v = take(sch_s, 60, &pos, 1)) < 0) {
            return 0;
        }
        out->swmi_up = (uint8_t)v;
        if ((v = take(sch_s, 60, &pos, 1)) < 0) {
            return 0;
        }
        dmrep = v;
        two_present = (s->comm_type == 3 || dmrep);
        if (two_present) {
            if ((v = take(sch_s, 60, &pos, 1)) < 0) {
                return 0;
            }
            two = v;
        } else if (take(sch_s, 60, &pos, 1) < 0) {
            return 0; /* reserved bit in place of the two-frequency flag */
        }
        if (take(sch_s, 60, &pos, 2) < 0) {
            return 0; /* operating modes, or reserved */
        }
        if (two_present && two) {
            out->two_freq = 1;
            if ((v = take(sch_s, 60, &pos, 5)) < 0) {
                return 0;
            }
            out->spacing = (uint8_t)v;
            if ((v = take(sch_s, 60, &pos, 1)) < 0) {
                return 0;
            }
            out->reverse = (uint8_t)v;
        } else if (take(sch_s, 60, &pos, 6) < 0) {
            return 0;
        }
        if ((v = take(sch_s, 60, &pos, 1)) < 0) {
            return 0;
        }
        out->ms_link = (uint8_t)v;
        s->ms_link_flag = out->ms_link;
        if (take(sch_s, 60, &pos, 2) < 0) {
            return 0; /* channel usage */
        }
        if ((v = take(sch_s, 60, &pos, 2)) < 0) {
            return 0;
        }
        out->channel_state = (uint8_t)v;
        if ((v = take(sch_s, 60, &pos, 2)) < 0) {
            return 0;
        }
        s->slot = (uint8_t)(v + 1);
        if ((v = take(sch_s, 60, &pos, 5)) < 0) {
            return 0;
        }
        s->frame = (uint8_t)v;
        if ((v = take(sch_s, 60, &pos, 3)) < 0) {
            return 0;
        }
        out->power_class = (uint8_t)v;
        if (take(sch_s, 60, &pos, 1) < 0) {
            return 0; /* power control */
        }
        if (take(sch_s, 60, &pos, 1) < 0) {
            return 0; /* registration-terminated, or reserved */
        }
        if (take(sch_s, 60, &pos, 2) < 0) {
            return 0; /* frame countdown */
        }
        if (out->channel_state == 0) {
            if (take(sch_s, 60, &pos, 2) < 0) {
                return 0;
            }
        } else {
            if ((v = take(sch_s, 60, &pos, 2)) < 0) {
                return 0;
            }
            out->priority = (uint8_t)v;
        }
        if (take(sch_s, 60, &pos, 2) < 0) {
            return 0; /* registrations permitted */
        }
        if (take(sch_s, 60, &pos, 4) < 0) {
            return 0; /* registration label */
        }
        if (take(sch_s, 60, &pos, 4) < 0) {
            return 0; /* phase time, DN232/DN233, or reserved */
        }
        if (take(sch_s, 60, &pos, 3) < 0) {
            return 0; /* access parameter or DT264 */
        }
        if (take(sch_s, 60, &pos, 1) < 0) {
            return 0; /* registrations forwarded */
        }
        if (take(sch_s, 60, &pos, 1) < 0) {
            return 0; /* gateway encryption on the SwMI */
        }
        if (take(sch_s, 60, &pos, 1) < 0) {
            return 0; /* system-wide services */
        }
        if (take(sch_s, 60, &pos, 3) < 0) {
            return 0; /* reserved */
        }
    } else if ((v = take(sch_s, 60, &pos, 1)) < 0) {
        return 0; /* M-DMO flag */
    } else {
        if (take(sch_s, 60, &pos, 2) < 0) {
            return 0; /* reserved */
        }
        if ((v = take(sch_s, 60, &pos, 1)) < 0) {
            return 0;
        }
        out->two_freq = (uint8_t)v;
        if (take(sch_s, 60, &pos, 2) < 0) {
            return 0; /* repeater operating modes */
        }
        if (out->two_freq) {
            if ((v = take(sch_s, 60, &pos, 5)) < 0) {
                return 0;
            }
            out->spacing = (uint8_t)v;
            if ((v = take(sch_s, 60, &pos, 1)) < 0) {
                return 0;
            }
            out->reverse = (uint8_t)v;
        } else if (take(sch_s, 60, &pos, 6) < 0) {
            return 0;
        }
        if ((v = take(sch_s, 60, &pos, 1)) < 0) {
            return 0;
        }
        out->ms_link = (uint8_t)v;
        s->ms_link_flag = out->ms_link;
        if (take(sch_s, 60, &pos, 2) < 0) {
            return 0; /* channel usage */
        }
        if ((v = take(sch_s, 60, &pos, 2)) < 0) {
            return 0;
        }
        out->channel_state = (uint8_t)v;
        if ((v = take(sch_s, 60, &pos, 2)) < 0) {
            return 0;
        }
        s->slot = (uint8_t)(v + 1);
        if ((v = take(sch_s, 60, &pos, 5)) < 0) {
            return 0;
        }
        s->frame = (uint8_t)v;
        if ((v = take(sch_s, 60, &pos, 3)) < 0) {
            return 0;
        }
        out->power_class = (uint8_t)v;
        if (take(sch_s, 60, &pos, 2) < 0) {
            return 0; /* power control, reserved */
        }
        if (take(sch_s, 60, &pos, 2) < 0) {
            return 0; /* frame countdown */
        }
        if (out->channel_state == 0) {
            if (take(sch_s, 60, &pos, 2) < 0) {
                return 0;
            }
        } else {
            if ((v = take(sch_s, 60, &pos, 2)) < 0) {
                return 0;
            }
            out->priority = (uint8_t)v;
        }
    }

    if (!sch_h) {
        out->parsed = 1;
        return 1;
    }
    pos = 0;
    if ((v = take(sch_h, 124, &pos, 10)) < 0) {
        return 0;
    }
    out->repeater = (uint16_t)v;
    if ((v = take(sch_h, 124, &pos, 24)) < 0) {
        return 0;
    }
    out->mni = (uint32_t)v;
    if ((v = take(sch_h, 124, &pos, 2)) < 0) {
        return 0; /* validity time unit */
    }
    if (take(sch_h, 124, &pos, 6) < 0) {
        return 0; /* units, or reserved when unrestricted */
    }
    if (take(sch_h, 124, &pos, 3) < 0) {
        return 0; /* maximum DM-MS power class */
    }
    if (take(sch_h, 124, &pos, 1) < 0) {
        return 0;
    }
    if ((v = take(sch_h, 124, &pos, 4)) < 0) {
        return 0;
    }
    out->urt = (uint8_t)v;
    out->parsed = 1;
    return 1;
}

int
tetra_dmo_describe_presence(const tetra_dmo_pres_t* p, char* buf, int buf_len) {
    static const char* const state_name[] = {"free", "occupied", "reserved", "gateway"};
    static const int spacing_khz[9] = {0, 1600, 4500, 8000, 10000, 18000, 30000, 39000, 45000};
    const char* link;
    const char* st;
    int n;
    if (!p || !buf || buf_len <= 0) {
        return 0;
    }
    link = p->ms_link ? "master" : "slave";
    if (p->comm_type >= 2 && p->channel_state == 3) {
        st = "registration";
    } else {
        st = p->channel_state < 4 ? state_name[p->channel_state] : "reserved-state";
    }
    if (p->two_freq && p->spacing >= 1 && p->spacing <= 8) {
        n = snprintf(buf, (size_t)buf_len, "%s=%u MNI=%u/%u %s %s pwr=%u urt=%u%s uplink=%s%d.%03d MHz",
                     p->comm_type >= 2 ? "gw" : "rep", p->repeater, (p->mni >> 14) & 0x3FFu, p->mni & 0x3FFFu, link, st,
                     p->power_class, p->urt, (p->comm_type >= 2) ? (p->swmi_up ? " swmi=up" : " swmi=down") : "",
                     p->reverse ? "-" : "+", spacing_khz[p->spacing] / 1000, spacing_khz[p->spacing] % 1000);
    } else {
        n = snprintf(buf, (size_t)buf_len, "%s=%u MNI=%u/%u %s %s pwr=%u urt=%u%s", p->comm_type >= 2 ? "gw" : "rep",
                     p->repeater, (p->mni >> 14) & 0x3FFu, p->mni & 0x3FFFu, link, st, p->power_class, p->urt,
                     (p->comm_type >= 2) ? (p->swmi_up ? " swmi=up" : " swmi=down") : "");
    }
    return n < 0 ? 0 : n;
}

/* Read up to 64 bits, most significant bit first. Returns 0, or -1 when the
 * block ends early. */
static int
take_u64(const uint8_t* bits, int len, int* pos, int n, uint64_t* out) {
    uint64_t v = 0;
    int i;
    if (n < 0 || n > 64 || *pos + n > len) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        v = (v << 1) | (uint64_t)(bits[(*pos)++] & 1u);
    }
    *out = v;
    return 0;
}

static char
tetra_dmo_digit(int d) {
    if (d >= 0 && d <= 9) {
        return (char)('0' + d);
    }
    if (d == 10) {
        return '*';
    }
    if (d == 11) {
        return '#';
    }
    if (d == 12) {
        return '+';
    }
    return '?';
}

/* Additional addressing (EN 300 396-3 9.7.3 and EN 300 396-5 table 25).
 * Order when several elements are present: calling TSI, area, external number.
 * Returns -1 when a variable element runs past `len`; the fields already read stay. */
static int
tetra_dmo_parse_extra(const uint8_t* bits, int len, int* pos, tetra_dmo_sync_h_t* out) {
    int kind;
    int n;
    int i;
    if ((kind = take(bits, len, pos, 4)) < 0) {
        return -1;
    }
    out->addr_type = (uint8_t)kind;
    if (kind == 1 || kind == 3 || kind == 5) {
        int mni;
        int ssi;
        if ((mni = take(bits, len, pos, 24)) < 0 || (ssi = take(bits, len, pos, 24)) < 0) {
            return -1;
        }
        out->calling_mni = (uint32_t)mni;
        out->calling_ssi = (uint32_t)ssi;
        out->has_calling_tsi = 1;
    }
    if (kind == 2 || kind == 3) {
        int area;
        if ((area = take(bits, len, pos, 4)) < 0) {
            return -1;
        }
        out->extra_area = (uint8_t)area;
        out->has_extra_area = 1;
    }
    if (kind == 4 || kind == 5) {
        if ((n = take(bits, len, pos, 5)) < 0) {
            return -1;
        }
        if (n > 24) {
            return -1;
        }
        for (i = 0; i < n; i++) {
            int d;
            if ((d = take(bits, len, pos, 4)) < 0) {
                out->external[i] = '\0';
                out->external_n = (uint8_t)i;
                return -1;
            }
            out->external[i] = tetra_dmo_digit(d);
        }
        out->external[n] = '\0';
        out->external_n = (uint8_t)n;
    }
    return 0;
}

/* SDTI payload (EN 300 396-3 9.7.16). Data 4 keeps its bit length and the first
 * 64 bits. Returns -1 when the block ends inside the payload. */
static int
tetra_dmo_parse_sdti(const uint8_t* bits, int len, int* pos, tetra_dmo_sync_h_t* out) {
    int sdti;
    int width;
    int keep;
    if ((sdti = take(bits, len, pos, 4)) < 0) {
        return -1;
    }
    out->sdti = (uint8_t)sdti;
    switch (sdti) {
        case 0: width = 16; break;
        case 1: width = 32; break;
        case 2: width = 64; break;
        case 4: width = 16; break;
        case 3: {
            int nbytes;
            if ((nbytes = take(bits, len, pos, 11)) < 0) {
                return -1;
            }
            out->sds_len = (uint16_t)nbytes;
            keep = nbytes > 64 ? 64 : nbytes;
            if (keep > 0 && take_u64(bits, len, pos, keep, &out->sds_value) < 0) {
                return -1;
            }
            if (nbytes > keep) {
                if (*pos + (nbytes - keep) > len) {
                    return -1;
                }
                *pos += nbytes - keep;
            }
            return 0;
        }
        default: return 0;
    }
    out->sds_len = (uint16_t)width;
    return take_u64(bits, len, pos, width, &out->sds_value);
}

static int
tetra_dmo_parse_fcs(const uint8_t* bits, int len, int* pos, tetra_dmo_sync_h_t* out) {
    uint64_t fcs;
    if (!out->fcs_flag) {
        return 0;
    }
    if (take_u64(bits, len, pos, 32, &fcs) < 0) {
        return -1;
    }
    out->sds_fcs = (uint32_t)fcs;
    out->has_fcs = 1;
    return 0;
}

/* Gateway-specific layer 3 PDU after the 5-bit message type (EN 300 396-5
 * clause 14.5). A variable external number that runs past the block is marked
 * truncated and does not consume a guessed tail. */
static void
tetra_dmo_parse_gateway(const uint8_t* bits, int pos, int len, tetra_dmo_sync_h_t* out) {
    int p = pos;
    int v;
    if ((v = take(bits, len, &p, 5)) < 0 || v > TETRA_DMO_GW_CANCEL_ACK) {
        return;
    }
    out->gw_subtype = (uint8_t)v;
    switch (v) {
        case TETRA_DMO_GW_SETUP:
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->circuit_mode = (uint8_t)v;
            if (take(bits, len, &p, 4) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 2)) < 0) {
                return;
            }
            out->priority = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->e2ee = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->group_call = (uint8_t)v;
            if (take(bits, len, &p, 2) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->request_label = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->called_tsi = (uint8_t)v;
            if ((v = take(bits, len, &p, 24)) < 0) {
                return;
            }
            out->called_ssi = (uint32_t)v;
            if (out->called_tsi) {
                if ((v = take(bits, len, &p, 24)) < 0) {
                    return;
                }
                out->called_mni = (uint32_t)v;
            }
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->area = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->extra_addr = (uint8_t)v;
            if (out->extra_addr && tetra_dmo_parse_extra(bits, len, &p, out) < 0) {
                out->sds_truncated = 1;
            }
            break;
        case TETRA_DMO_GW_CONNECT:
        case TETRA_DMO_GW_TX_ACCEPT:
        case TETRA_DMO_GW_PRE_ACCEPT:
            if ((v = take(bits, len, &p, 3)) < 0) {
                return;
            }
            out->power_class = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->power_control = (uint8_t)v;
            if (take(bits, len, &p, 1) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->gw_enc = (uint8_t)v;
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->circuit_mode = (uint8_t)v;
            if (take(bits, len, &p, 4) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->e2ee = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->group_call = (uint8_t)v;
            if (out->gw_subtype == TETRA_DMO_GW_CONNECT) {
                if ((v = take(bits, len, &p, 4)) < 0) {
                    return;
                }
                out->request_label = (uint8_t)v;
            }
            break;
        case TETRA_DMO_GW_ACK: {
            int reservation;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->timing_change = (uint8_t)v;
            if (out->timing_change) {
                if ((v = take(bits, len, &p, 12)) < 0) {
                    return;
                }
                out->timing_adjust = (uint16_t)v;
            }
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            reservation = v;
            out->has_reservation = (uint8_t)reservation;
            if (reservation) {
                int requests;
                if (take(bits, len, &p, 1) < 0) {
                    return;
                }
                if ((v = take(bits, len, &p, 6)) < 0) {
                    return;
                }
                out->reservation = (uint8_t)v;
                if ((v = take(bits, len, &p, 1)) < 0) {
                    return;
                }
                requests = v;
                if (take(bits, len, &p, 1) < 0) {
                    return;
                }
                if (requests && take(bits, len, &p, 8) < 0) {
                    return;
                }
                if ((v = take(bits, len, &p, 3)) < 0) {
                    return;
                }
                out->power_class = (uint8_t)v;
                if ((v = take(bits, len, &p, 1)) < 0) {
                    return;
                }
                out->power_control = (uint8_t)v;
                if (take(bits, len, &p, 1) < 0) {
                    return;
                }
                if ((v = take(bits, len, &p, 2)) < 0) {
                    return;
                }
                out->priority = (uint8_t)v;
            }
            if (take(bits, len, &p, 4) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 2)) < 0) {
                return;
            }
            out->gack_type = (uint8_t)v;
            if ((v = take(bits, len, &p, 3)) < 0) {
                return;
            }
            out->wait_code = (uint8_t)v;
            break;
        }
        case TETRA_DMO_GW_RELEASE:
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->release_cause = (uint8_t)v;
            break;
        case TETRA_DMO_GW_REJECT:
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->reject_cause = (uint8_t)v;
            break;
        case TETRA_DMO_GW_TX_REQUEST:
        case TETRA_DMO_GW_PREEMPT:
            if (v == TETRA_DMO_GW_TX_REQUEST) {
                if ((v = take(bits, len, &p, 1)) < 0) {
                    return;
                }
                out->after_preempt = (uint8_t)v;
                if (take(bits, len, &p, 3) < 0) {
                    return;
                }
            } else {
                if ((v = take(bits, len, &p, 2)) < 0) {
                    return;
                }
                out->channel_state = (uint8_t)v;
                if (take(bits, len, &p, 2) < 0) {
                    return;
                }
            }
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->circuit_mode = (uint8_t)v;
            if (take(bits, len, &p, 4) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 2)) < 0) {
                return;
            }
            out->priority = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->e2ee = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->group_call = (uint8_t)v;
            if (take(bits, len, &p, 2) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->has_calling_tsi = (uint8_t)v;
            if (out->has_calling_tsi) {
                if ((v = take(bits, len, &p, 24)) < 0) {
                    return;
                }
                out->calling_mni = (uint32_t)v;
                if ((v = take(bits, len, &p, 24)) < 0) {
                    return;
                }
                out->calling_ssi = (uint32_t)v;
            }
            break;
        case TETRA_DMO_GW_REG_REQUEST:
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->reg_unsolicited = (uint8_t)v;
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->reg_frames = (uint8_t)v;
            if (take(bits, len, &p, 2) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->has_calling_tsi = (uint8_t)v;
            if (out->has_calling_tsi) {
                if ((v = take(bits, len, &p, 24)) < 0) {
                    return;
                }
                out->calling_mni = (uint32_t)v;
                if ((v = take(bits, len, &p, 24)) < 0) {
                    return;
                }
                out->calling_ssi = (uint32_t)v;
            }
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->has_gssi = (uint8_t)v;
            if (out->has_gssi) {
                if ((v = take(bits, len, &p, 24)) < 0) {
                    return;
                }
                out->gssi = (uint32_t)v;
            }
            break;
        case TETRA_DMO_GW_REG_ACCEPT:
        case TETRA_DMO_GW_REG_REJECT:
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->reg_unsolicited = (uint8_t)v;
            if (take(bits, len, &p, 2) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->reg_label = (uint8_t)v;
            if (out->gw_subtype == TETRA_DMO_GW_REG_REJECT) {
                if ((v = take(bits, len, &p, 4)) < 0) {
                    return;
                }
                out->reject_cause = (uint8_t)v;
            }
            break;
        case TETRA_DMO_GW_REG_CANCEL:
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->reg_frames = (uint8_t)v;
            if (take(bits, len, &p, 2) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->reg_label = (uint8_t)v;
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->reject_cause = (uint8_t)v;
            break;
        case TETRA_DMO_GW_CANCEL_ACK:
            if (take(bits, len, &p, 2) < 0) {
                return;
            }
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->reg_label = (uint8_t)v;
            break;
        default: return;
    }
    out->elems = TETRA_DMO_ELEMS_GW;
}

/* Message-dependent elements and DM-SDU elements that follow the message type
 * (EN 300 396-3 tables 31 to 44). Fields that run past `len` stay 0. */
static void
tetra_dmo_parse_elems(const uint8_t* bits, int pos, int len, tetra_dmo_sync_h_t* out) {
    switch (out->msg_type) {
        case TETRA_DMO_MSG_SETUP:
        case TETRA_DMO_MSG_SETUP_PRES:
        case TETRA_DMO_MSG_CONNECT_ACK:
        case TETRA_DMO_MSG_OCCUPIED:
            if (pos + 26 > len) {
                return;
            }
            /* DM-SETUP PRES starts with 3 reserved bits. The following fields
             * sit at the same offsets as DM-SETUP (table 32 versus table 31). */
            if (out->msg_type != TETRA_DMO_MSG_SETUP_PRES) {
                out->timing_flag = bits[pos] & 1u;
                out->lch_frame3 = bits[pos + 1] & 1u;
                out->preempt_flag = bits[pos + 2] & 1u;
            }
            out->power_class = (uint8_t)tetra_bits_to_uint(bits, pos + 3, 3);
            out->power_control = bits[pos + 6] & 1u;
            /* 2 reserved bits */
            out->dual_watch = bits[pos + 9] & 1u;
            out->two_freq = bits[pos + 10] & 1u;
            out->circuit_mode = (uint8_t)tetra_bits_to_uint(bits, pos + 11, 4);
            /* 4 reserved bits */
            out->priority = (uint8_t)tetra_bits_to_uint(bits, pos + 19, 2);
            out->e2ee = bits[pos + 21] & 1u;
            out->group_call = bits[pos + 22] & 1u;
            out->ext_source = bits[pos + 23] & 1u;
            out->elems = TETRA_DMO_ELEMS_CALL;
            break;
        case TETRA_DMO_MSG_TX_CEASED: {
            int p = pos;
            if (p + 12 > len) {
                return;
            }
            out->reservation = (uint8_t)tetra_bits_to_uint(bits, p, 6);
            p += 6;
            p += 1; /* timing flag */
            const unsigned requests = bits[p++] & 1u;
            p += 1; /* changeover requests flag */
            if (requests) {
                p += 8; /* requests bitmap */
            }
            p += 1; /* recent user priority flag */
            const unsigned timing_change = bits[p++] & 1u;
            if (timing_change) {
                p += 12; /* timing adjustment */
            }
            if (p + 6 > len) {
                return;
            }
            out->priority = (uint8_t)tetra_bits_to_uint(bits, p, 2);
            out->cease_cause = (uint8_t)tetra_bits_to_uint(bits, p + 2, 4);
            out->elems = TETRA_DMO_ELEMS_CEASED;
            break;
        }
        case TETRA_DMO_MSG_RESERVED: {
            int p = pos + 1; /* channel reservation type */
            if (p + 9 > len) {
                return;
            }
            out->reservation = (uint8_t)tetra_bits_to_uint(bits, p, 6);
            p += 6;
            p += 1; /* timing flag */
            const unsigned requests = bits[p++] & 1u;
            p += 1; /* changeover requests flag */
            if (requests) {
                p += 8;
            }
            if (p + 6 > len) {
                return;
            }
            out->power_class = (uint8_t)tetra_bits_to_uint(bits, p, 3);
            out->priority = (uint8_t)tetra_bits_to_uint(bits, p + 4, 2);
            out->elems = TETRA_DMO_ELEMS_RESERVED;
            break;
        }
        case TETRA_DMO_MSG_SDS_OCCUPIED:
            if (pos + 7 > len) {
                return;
            }
            out->sds_remaining = (uint8_t)tetra_bits_to_uint(bits, pos, 4);
            out->priority = (uint8_t)tetra_bits_to_uint(bits, pos + 5, 2);
            out->elems = TETRA_DMO_ELEMS_SDS_OCC;
            break;
        case TETRA_DMO_MSG_RELEASE:
            if (pos + 4 > len) {
                return;
            }
            out->release_cause = (uint8_t)tetra_bits_to_uint(bits, pos, 4);
            if (out->release_cause == 15 && pos + 9 <= len) {
                out->release_ext = (uint8_t)tetra_bits_to_uint(bits, pos + 4, 5);
                out->has_release_ext = 1;
            }
            out->elems = TETRA_DMO_ELEMS_RELEASE;
            break;
        case TETRA_DMO_MSG_CONNECT:
            if (pos + 4 > len) {
                return;
            }
            out->circuit_mode = (uint8_t)tetra_bits_to_uint(bits, pos, 4);
            out->elems = TETRA_DMO_ELEMS_CONNECT;
            break;
        case TETRA_DMO_MSG_DISCONNECT:
            if (pos + 4 > len) {
                return;
            }
            out->disconnect_cause = (uint8_t)tetra_bits_to_uint(bits, pos, 4);
            out->elems = TETRA_DMO_ELEMS_DISCONNECT;
            break;
        case TETRA_DMO_MSG_TX_REQUEST:
        case TETRA_DMO_MSG_TX_ACCEPT:
        case TETRA_DMO_MSG_PREEMPT:
        case TETRA_DMO_MSG_PRE_ACCEPT: {
            int p = pos;
            int v;
            if (out->msg_type == TETRA_DMO_MSG_PREEMPT) {
                if ((v = take(bits, len, &p, 2)) < 0) {
                    return;
                }
                out->channel_state = (uint8_t)v;
            }
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->timing_change = (uint8_t)v;
            if (out->timing_change) {
                if ((v = take(bits, len, &p, 12)) < 0) {
                    return;
                }
                out->timing_adjust = (uint16_t)v;
            }
            if (out->msg_type == TETRA_DMO_MSG_PREEMPT || out->msg_type == TETRA_DMO_MSG_PRE_ACCEPT) {
                if ((v = take(bits, len, &p, 1)) < 0) {
                    return;
                }
                out->new_call = (uint8_t)v;
                if ((v = take(bits, len, &p, 4)) < 0) {
                    return;
                }
                out->preempt_type = (uint8_t)v;
            }
            if (out->msg_type == TETRA_DMO_MSG_TX_REQUEST || out->msg_type == TETRA_DMO_MSG_PREEMPT) {
                if ((v = take(bits, len, &p, 2)) < 0) {
                    return;
                }
                out->priority = (uint8_t)v;
            }
            if (out->msg_type == TETRA_DMO_MSG_TX_REQUEST) {
                out->elems = TETRA_DMO_ELEMS_TX_REQ;
            } else if (out->msg_type == TETRA_DMO_MSG_TX_ACCEPT) {
                out->elems = TETRA_DMO_ELEMS_TX_ACCEPT;
            } else if (out->msg_type == TETRA_DMO_MSG_PREEMPT) {
                out->elems = TETRA_DMO_ELEMS_PREEMPT;
            } else {
                out->elems = TETRA_DMO_ELEMS_PRE_ACCEPT;
            }
            break;
        }
        case TETRA_DMO_MSG_REJECT:
            if (pos + 4 > len) {
                return;
            }
            out->reject_cause = (uint8_t)tetra_bits_to_uint(bits, pos, 4);
            out->elems = TETRA_DMO_ELEMS_REJECT;
            break;
        case TETRA_DMO_MSG_TIMING_REQ: {
            int v;
            int p = pos;
            if ((v = take(bits, len, &p, 12)) < 0) {
                return;
            }
            out->timing_change = 1;
            out->timing_adjust = (uint16_t)v;
            out->elems = TETRA_DMO_ELEMS_TIMING;
            break;
        }
        case TETRA_DMO_MSG_TIMING_ACK: {
            int v;
            int p = pos;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->timing_accept = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->timing_change = (uint8_t)v;
            if (out->timing_change) {
                if ((v = take(bits, len, &p, 12)) < 0) {
                    return;
                }
                out->timing_adjust = (uint16_t)v;
            }
            out->elems = TETRA_DMO_ELEMS_TIMING;
            break;
        }
        case TETRA_DMO_MSG_SDS_UDATA:
        case TETRA_DMO_MSG_SDS_DATA: {
            int v;
            int p = pos;
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->sds_remaining = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->sds_tx_type = (uint8_t)v;
            if ((v = take(bits, len, &p, 2)) < 0) {
                return;
            }
            out->priority = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->fcs_flag = (uint8_t)v;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->extra_addr = (uint8_t)v;
            if (out->extra_addr && tetra_dmo_parse_extra(bits, len, &p, out) < 0) {
                out->sds_truncated = 1;
            } else if (tetra_dmo_parse_sdti(bits, len, &p, out) < 0 || tetra_dmo_parse_fcs(bits, len, &p, out) < 0) {
                out->sds_truncated = 1;
            }
            out->elems = TETRA_DMO_ELEMS_SDS;
            break;
        }
        case TETRA_DMO_MSG_SDS_ACK: {
            int v;
            int p = pos;
            if ((v = take(bits, len, &p, 1)) < 0) {
                return;
            }
            out->fcs_flag = (uint8_t)v;
            if ((v = take(bits, len, &p, 4)) < 0) {
                return;
            }
            out->ack_type = (uint8_t)v;
            if (out->ack_type == 1 && tetra_dmo_parse_sdti(bits, len, &p, out) < 0) {
                out->sds_truncated = 1;
            } else if (tetra_dmo_parse_fcs(bits, len, &p, out) < 0) {
                out->sds_truncated = 1;
            }
            out->elems = TETRA_DMO_ELEMS_SDS;
            break;
        }
        case TETRA_DMO_MSG_GATEWAY: tetra_dmo_parse_gateway(bits, pos, len, out); break;
        case TETRA_DMO_MSG_INFO: {
            int p = pos;
            int v;
            if ((v = take(bits, len, &p, 3)) < 0) {
                return;
            }
            out->info_type = (uint8_t)v;
            if (out->info_type == 0 && p + 48 <= len) {
                out->calling_mni = tetra_bits_to_uint(bits, p, 24);
                out->calling_ssi = tetra_bits_to_uint(bits, p + 24, 24);
            }
            out->elems = TETRA_DMO_ELEMS_INFO;
            break;
        }
        default: break;
    }
}

int
tetra_dmo_parse_sync_h(const uint8_t* bits, int comm_type, tetra_dmo_sync_h_t* out) {
    int pos = 10; /* repeater / gateway address / reserved: 10 bits in every variant */
    if (!bits || !out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->relay_addr = (uint16_t)tetra_bits_to_uint(bits, 0, 10);
    out->has_relay = comm_type != 0;
    out->fill_bit = bits[pos++] & 1u;
    out->frag_flag = bits[pos++] & 1u;
    if (out->frag_flag) {
        out->num_schf = (uint8_t)tetra_bits_to_uint(bits, pos, 4);
        pos += 4;
    }
    out->frame_countdown = (uint8_t)tetra_bits_to_uint(bits, pos, 2);
    pos += 2;
    out->dst_type = (uint8_t)tetra_bits_to_uint(bits, pos, 2);
    pos += 2;
    if (out->dst_type != 2u) {
        out->dst = tetra_bits_to_uint(bits, pos, 24);
        pos += 24;
    }
    out->src_type = (uint8_t)tetra_bits_to_uint(bits, pos, 2);
    pos += 2;
    if (out->src_type != 2u) {
        out->src = tetra_bits_to_uint(bits, pos, 24);
        pos += 24;
    }
    /* The MNI is always present for direct (00) and DM-REP (01). For a gateway
     * (10 or 11) it is present when either address is a true SSI (type 00),
     * EN 300 396-5 clause 14.1.1. The colour code uses the gateway address, so
     * a source address is enough even when the MNI is absent. */
    {
        int gateway = (comm_type == 2 || comm_type == 3);
        int mni_present = !gateway || out->dst_type == 0 || out->src_type == 0;
        if (mni_present && pos + 24 <= 124) {
            out->mni = tetra_bits_to_uint(bits, pos, 24);
            pos += 24;
            if (!gateway) {
                out->has_addrs = 1;
            }
        }
        if (gateway && out->src_type != 2u) {
            out->has_addrs = 1;
        }
    }
    out->msg_type = (uint8_t)tetra_bits_to_uint(bits, pos, 5);
    pos += 5;
    tetra_dmo_parse_elems(bits, pos, 124, out);
    return 1;
}

static const char*
tetra_dmo_circuit_name(unsigned mode) {
    if (mode == 0) {
        return "speech";
    }
    if (mode == 8) {
        return "prop-speech";
    }
    return "data";
}

static const char*
tetra_dmo_disconnect_name(unsigned cause) {
    static const char* const name[] = {
        "unknown",     "no E2EE support", "service unsupported", "rejected by called party",
        "SCK failure", "poor quality",    "E2EE required",       "security mismatch"};
    return cause < 8 ? name[cause] : "reserved";
}

static const char*
tetra_dmo_reject_name(unsigned cause) {
    switch (cause) {
        case 0: return "unknown";
        case 1: return "invalid";
        case 2: return "master will transmit";
        case 3: return "no short data";
        case 6: return "security mismatch";
        default: return "reserved";
    }
}

static const char*
tetra_dmo_preempt_kind(unsigned kind) {
    switch (kind) {
        case 0: return "circuit";
        case 1: return "short data";
        case 2: return "late gateway";
        case 3: return "TMO interrupt";
        case 4: return "TMO wait";
        case 5: return "TMO break";
        case 6: return "TMO timing";
        case 7: return "TMO timer";
        case 8: return "gateway pre-emption";
        default: return "reserved";
    }
}

static int
tetra_dmo_timing_text(const tetra_dmo_sync_h_t* h, char* buf, int buf_len) {
    if (!h->timing_change) {
        buf[0] = '\0';
        return 0;
    }
    if (h->timing_adjust >= 1 && h->timing_adjust <= 3671) {
        return snprintf(buf, (size_t)buf_len, " timing=+%u symbols", (unsigned)h->timing_adjust * 5u);
    }
    return snprintf(buf, (size_t)buf_len, " timing=reserved");
}

static const char*
tetra_dmo_gw_name(unsigned subtype) {
    switch (subtype) {
        case TETRA_DMO_GW_SETUP: return "DM-GSETUP";
        case TETRA_DMO_GW_CONNECT: return "DM-GCONNECT";
        case TETRA_DMO_GW_ACK: return "DM-GACK";
        case TETRA_DMO_GW_RELEASE: return "DM-GRELEASE";
        case TETRA_DMO_GW_TX_REQUEST: return "DM-GTX REQUEST";
        case TETRA_DMO_GW_TX_ACCEPT: return "DM-GTX ACCEPT";
        case TETRA_DMO_GW_PREEMPT: return "DM-GPREEMPT";
        case TETRA_DMO_GW_PRE_ACCEPT: return "DM-GPRE ACCEPT";
        case TETRA_DMO_GW_REJECT: return "DM-GREJECT";
        case TETRA_DMO_GW_REG_REQUEST: return "DM-GREGISTER REQUEST";
        case TETRA_DMO_GW_REG_ACCEPT: return "DM-GREGISTER ACCEPT";
        case TETRA_DMO_GW_REG_REJECT: return "DM-GREGISTER REJECT";
        case TETRA_DMO_GW_REG_CANCEL: return "DM-GREGISTER CANCEL";
        case TETRA_DMO_GW_CANCEL_ACK: return "DM-GCANCEL ACK";
        default: return "gateway";
    }
}

static const char*
tetra_dmo_gw_release_name(unsigned cause) {
    switch (cause) {
        case 0: return "unknown";
        case 1: return "TMO resource lost";
        case 2: return "TMO call timer expired";
        default: return "reserved";
    }
}

static const char*
tetra_dmo_gw_reject_name(unsigned cause) {
    switch (cause) {
        case 0: return "unknown";
        case 1: return "invalid";
        case 2: return "rejected by gateway";
        case 3: return "rejected by SwMI";
        case 4: return "TMO transmission failure";
        default: return "reserved";
    }
}

static const char*
tetra_dmo_reg_reject_name(unsigned cause) {
    switch (cause) {
        case 0: return "unknown";
        case 1: return "ITSI unknown";
        case 2: return "no SwMI response";
        case 3: return "attachment rejected";
        case 4: return "rejected by gateway";
        case 5: return "group unsupported";
        default: return "reserved";
    }
}

static int
tetra_dmo_describe_gateway(const tetra_dmo_sync_h_t* h, char* buf, int buf_len) {
    static const char* const prio[4] = {"normal", "high", "pre-emptive", "emergency"};
    static const int wait_s[8] = {0, 1, 2, 5, 10, 20, 30, 60};
    char timing[40];
    const char* name = tetra_dmo_gw_name(h->gw_subtype);
    tetra_dmo_timing_text(h, timing, (int)sizeof(timing));
    switch (h->gw_subtype) {
        case TETRA_DMO_GW_SETUP: {
            char area[8];
            char called[48];
            if (h->area == 0) {
                snprintf(area, sizeof(area), "unset");
            } else if (h->area == 15) {
                snprintf(area, sizeof(area), "all");
            } else {
                snprintf(area, sizeof(area), "%u", h->area);
            }
            if (h->called_tsi) {
                snprintf(called, sizeof(called), "called=%u MNI=%u/%u", h->called_ssi, (h->called_mni >> 14) & 0x3FFu,
                         h->called_mni & 0x3FFFu);
            } else {
                snprintf(called, sizeof(called), "called=%u", h->called_ssi);
            }
            return snprintf(buf, (size_t)buf_len, "%s %s %s prio=%s label=%u %s area=%s%s%s%s", name,
                            h->group_call ? "group" : "individual", tetra_dmo_circuit_name(h->circuit_mode),
                            prio[h->priority & 3u], h->request_label, called, area,
                            h->external_n ? " ext=" : (h->extra_addr ? " extra-addr" : ""),
                            h->external_n ? h->external : "", h->sds_truncated ? " truncated" : "");
        }
        case TETRA_DMO_GW_CONNECT:
            return snprintf(buf, (size_t)buf_len, "%s %s %s pwr=%u label=%u%s", name,
                            h->group_call ? "group" : "individual", tetra_dmo_circuit_name(h->circuit_mode),
                            h->power_class, h->request_label, h->gw_enc ? " SwMI-enc" : "");
        case TETRA_DMO_GW_TX_ACCEPT:
        case TETRA_DMO_GW_PRE_ACCEPT:
            return snprintf(buf, (size_t)buf_len, "%s %s %s pwr=%u%s", name, h->group_call ? "group" : "individual",
                            tetra_dmo_circuit_name(h->circuit_mode), h->power_class, h->gw_enc ? " SwMI-enc" : "");
        case TETRA_DMO_GW_ACK: {
            const char* wait;
            char wait_buf[24];
            if (h->wait_code == 0) {
                wait = "predefined";
            } else {
                snprintf(wait_buf, sizeof(wait_buf), "%ds", h->wait_code < 8 ? wait_s[h->wait_code] : 0);
                wait = wait_buf;
            }
            if (h->has_reservation) {
                return snprintf(buf, (size_t)buf_len, "%s %s wait=%s%s reserve=%u frames prio=%s", name,
                                h->gack_type == 1 ? "incomplete" : "full", wait, timing, h->reservation * 6u,
                                prio[h->priority & 3u]);
            }
            return snprintf(buf, (size_t)buf_len, "%s %s wait=%s%s", name, h->gack_type == 1 ? "incomplete" : "full",
                            wait, timing);
        }
        case TETRA_DMO_GW_RELEASE:
            return snprintf(buf, (size_t)buf_len, "%s %s", name, tetra_dmo_gw_release_name(h->release_cause));
        case TETRA_DMO_GW_REJECT:
            return snprintf(buf, (size_t)buf_len, "%s %s", name, tetra_dmo_gw_reject_name(h->reject_cause));
        case TETRA_DMO_GW_TX_REQUEST:
        case TETRA_DMO_GW_PREEMPT:
            if (h->has_calling_tsi) {
                return snprintf(buf, (size_t)buf_len, "%s%s %s %s prio=%s calling=%u/%u ssi=%u", name,
                                h->after_preempt ? " after-preempt" : "", h->group_call ? "group" : "individual",
                                tetra_dmo_circuit_name(h->circuit_mode), prio[h->priority & 3u],
                                (h->calling_mni >> 14) & 0x3FFu, h->calling_mni & 0x3FFFu, h->calling_ssi);
            }
            return snprintf(buf, (size_t)buf_len, "%s%s %s %s prio=%s", name, h->after_preempt ? " after-preempt" : "",
                            h->group_call ? "group" : "individual", tetra_dmo_circuit_name(h->circuit_mode),
                            prio[h->priority & 3u]);
        case TETRA_DMO_GW_REG_REQUEST:
            if (h->has_gssi) {
                return snprintf(buf, (size_t)buf_len, "%s %s time=%u frames gssi=%u", name,
                                h->reg_unsolicited ? "unsolicited" : "solicited", h->reg_frames, h->gssi);
            }
            if (h->has_calling_tsi) {
                return snprintf(buf, (size_t)buf_len, "%s %s time=%u frames calling=%u/%u ssi=%u", name,
                                h->reg_unsolicited ? "unsolicited" : "solicited", h->reg_frames,
                                (h->calling_mni >> 14) & 0x3FFu, h->calling_mni & 0x3FFFu, h->calling_ssi);
            }
            return snprintf(buf, (size_t)buf_len, "%s %s time=%u frames", name,
                            h->reg_unsolicited ? "unsolicited" : "solicited", h->reg_frames);
        case TETRA_DMO_GW_REG_ACCEPT: return snprintf(buf, (size_t)buf_len, "%s label=%u", name, h->reg_label);
        case TETRA_DMO_GW_REG_REJECT:
        case TETRA_DMO_GW_REG_CANCEL:
            return snprintf(buf, (size_t)buf_len, "%s label=%u %s", name, h->reg_label,
                            tetra_dmo_reg_reject_name(h->reject_cause));
        case TETRA_DMO_GW_CANCEL_ACK: return snprintf(buf, (size_t)buf_len, "%s label=%u", name, h->reg_label);
        default: return 0;
    }
}

static int
tetra_dmo_describe_sds(const tetra_dmo_sync_h_t* h, char* buf, int buf_len) {
    static const char* const prio[4] = {"normal", "high", "pre-emptive", "emergency"};
    static const char* const ack[] = {"full", "full+data", "fcs failed", "incomplete"};
    int n;
    if (h->msg_type == TETRA_DMO_MSG_SDS_ACK) {
        const char* name = h->ack_type < 4 ? ack[h->ack_type] : "reserved";
        n = snprintf(buf, (size_t)buf_len, "SDS ack %s", name);
    } else if (h->sdti == 4) {
        n = snprintf(buf, (size_t)buf_len, "SDS status=0x%04x prio=%s%s", (unsigned)(h->sds_value & 0xFFFFu),
                     prio[h->priority & 3u], h->sds_tx_type ? " in-call" : "");
    } else {
        n = snprintf(buf, (size_t)buf_len, "SDS data sdti=%u len=%u prio=%s%s", h->sdti, h->sds_len,
                     prio[h->priority & 3u], h->sds_tx_type ? " in-call" : "");
    }
    if (n < 0 || n >= buf_len) {
        return n < 0 ? 0 : n;
    }
    if (h->external_n) {
        n += snprintf(buf + n, (size_t)(buf_len - n), " ext=%s", h->external);
    }
    if (n >= 0 && n < buf_len && h->has_fcs) {
        n += snprintf(buf + n, (size_t)(buf_len - n), " fcs=0x%08x", h->sds_fcs);
    }
    if (n >= 0 && n < buf_len && h->sds_truncated) {
        n += snprintf(buf + n, (size_t)(buf_len - n), " truncated");
    }
    return n < 0 ? 0 : n;
}

int
tetra_dmo_describe_elems(const tetra_dmo_sync_h_t* h, char* buf, int buf_len) {
    static const char* const prio[4] = {"normal", "high", "pre-emptive", "emergency"};
    static const char* const seen[] = {"circuit occupation", "reservation", "short data occupation", "registration"};
    char timing[40];
    int n = 0;
    if (!h || !buf || buf_len <= 0) {
        return 0;
    }
    buf[0] = '\0';
    if (h->e2ee) {
        (void)tetra_etee_library_ready();
    }
    tetra_dmo_timing_text(h, timing, (int)sizeof(timing));
    if (h->elems & TETRA_DMO_ELEMS_CALL) {
        n = snprintf(buf, (size_t)buf_len, "%s %s prio=%s pwr=%u%s%s", h->group_call ? "group" : "individual",
                     tetra_dmo_circuit_name(h->circuit_mode), prio[h->priority & 3u], h->power_class,
                     h->e2ee ? " E2EE" : "", h->preempt_flag ? " preempt" : "");
    } else if (h->elems & TETRA_DMO_ELEMS_CEASED) {
        n = snprintf(buf, (size_t)buf_len, "cause=%u reserve=%u frames prio=%s", h->cease_cause, h->reservation * 6u,
                     prio[h->priority & 3u]);
    } else if (h->elems & TETRA_DMO_ELEMS_RESERVED) {
        n = snprintf(buf, (size_t)buf_len, "reserved %u frames prio=%s pwr=%u", h->reservation * 6u,
                     prio[h->priority & 3u], h->power_class);
    } else if (h->elems & TETRA_DMO_ELEMS_SDS_OCC) {
        n = snprintf(buf, (size_t)buf_len, "SDS time remaining=%u prio=%s", h->sds_remaining, prio[h->priority & 3u]);
    } else if (h->elems & TETRA_DMO_ELEMS_RELEASE) {
        if (h->has_release_ext && h->release_ext == 0) {
            n = snprintf(buf, (size_t)buf_len, "release occupied by other users");
        } else if (h->has_release_ext) {
            n = snprintf(buf, (size_t)buf_len, "release extension=%u", h->release_ext);
        } else {
            n = snprintf(buf, (size_t)buf_len, "release cause=%u", h->release_cause);
        }
    } else if (h->elems & TETRA_DMO_ELEMS_CONNECT) {
        n = snprintf(buf, (size_t)buf_len, "accept %s", tetra_dmo_circuit_name(h->circuit_mode));
    } else if (h->elems & TETRA_DMO_ELEMS_DISCONNECT) {
        n = snprintf(buf, (size_t)buf_len, "disconnect %s", tetra_dmo_disconnect_name(h->disconnect_cause));
    } else if (h->elems & TETRA_DMO_ELEMS_TX_REQ) {
        n = snprintf(buf, (size_t)buf_len, "changeover prio=%s%s", prio[h->priority & 3u], timing);
    } else if (h->elems & TETRA_DMO_ELEMS_TX_ACCEPT) {
        n = snprintf(buf, (size_t)buf_len, "changeover accepted%s", timing);
    } else if (h->elems & TETRA_DMO_ELEMS_PREEMPT) {
        const char* st = h->channel_state < 4 ? seen[h->channel_state] : "reserved-state";
        n = snprintf(buf, (size_t)buf_len, "preempt %s %s %s prio=%s%s", h->new_call ? "new-call" : "ongoing", st,
                     tetra_dmo_preempt_kind(h->preempt_type), prio[h->priority & 3u], timing);
    } else if (h->elems & TETRA_DMO_ELEMS_PRE_ACCEPT) {
        n = snprintf(buf, (size_t)buf_len, "preempt accepted %s %s%s", h->new_call ? "new-call" : "ongoing",
                     tetra_dmo_preempt_kind(h->preempt_type), timing);
    } else if (h->elems & TETRA_DMO_ELEMS_REJECT) {
        n = snprintf(buf, (size_t)buf_len, "reject %s", tetra_dmo_reject_name(h->reject_cause));
    } else if (h->elems & TETRA_DMO_ELEMS_INFO && h->info_type == 0) {
        n = snprintf(buf, (size_t)buf_len, "calling %u/%u ssi=%u", (h->calling_mni >> 14) & 0x3FFu,
                     h->calling_mni & 0x3FFFu, h->calling_ssi);
    } else if (h->elems & TETRA_DMO_ELEMS_GW) {
        n = tetra_dmo_describe_gateway(h, buf, buf_len);
    } else if (h->elems & TETRA_DMO_ELEMS_TIMING) {
        if (h->msg_type == TETRA_DMO_MSG_TIMING_ACK) {
            n = snprintf(buf, (size_t)buf_len, "timing %s%s", h->timing_accept ? "accepted" : "rejected", timing);
        } else {
            n = snprintf(buf, (size_t)buf_len, "timing request%s", timing);
        }
    } else if (h->elems & TETRA_DMO_ELEMS_SDS) {
        n = tetra_dmo_describe_sds(h, buf, buf_len);
    }
    return n < 0 ? 0 : n;
}

uint32_t
tetra_dmo_dcc_seed(uint32_t net_id, uint32_t src_addr) {
    /* e(1) is the most significant bit of the 30-bit colour code, exactly like
     * the TMO extended colour code, so the same "<<2 | 3" LFSR seed applies.
     * `net_id` is the MNI, or the gateway address; only its low 6 bits are used. */
    uint32_t e = ((net_id & 0x3Fu) << 24) | (src_addr & 0xFFFFFFu);
    return ((e << 2) | 3u) & 0xFFFFFFFFu;
}

static uint32_t
tetra_dmo_call_seed(const tetra_dmo_sync_s_t* s, const tetra_dmo_sync_h_t* h) {
    uint32_t net_id = (s->comm_type >= 2) ? (uint32_t)h->relay_addr : h->mni;
    return tetra_dmo_dcc_seed(net_id, h->src);
}

static int
tetra_dmo_ui_category(int msg_type) {
    switch (msg_type) {
        case TETRA_DMO_MSG_SDS_OCCUPIED:
        case TETRA_DMO_MSG_SDS_UDATA:
        case TETRA_DMO_MSG_SDS_DATA:
        case TETRA_DMO_MSG_SDS_ACK: return DSD_TETRA_MESSAGE_SDS;
        case TETRA_DMO_MSG_TIMING_REQ:
        case TETRA_DMO_MSG_TIMING_ACK: return DSD_TETRA_MESSAGE_CONTROL;
        default: return DSD_TETRA_MESSAGE_CALL;
    }
}

static void
tetra_dmo_ui_push(dsd_state* state, int category, const char* text) {
    if (!state || !text || text[0] == '\0') {
        return;
    }
    if (state->tetra_message_sequence > 0) {
        const dsd_tetra_message* last =
            &state->tetra_messages[(state->tetra_message_sequence - 1u) % DSD_TETRA_MESSAGE_CAPACITY];
        if (strcmp(last->text, text) == 0) {
            return;
        }
    }
    tetra_message_push(state, (dsd_tetra_message_category)category, text);
}

void
tetra_dmo_publish(dsd_state* state, int msg_type, uint32_t src, uint32_t dst, const char* desc) {
    char line[DSD_TETRA_MESSAGE_TEXT_SIZE];
    if (desc && desc[0] != '\0') {
        DSD_SNPRINTF(line, sizeof line, "DMO %s src=%u dst=%u | %s", tetra_dmo_msg_name(msg_type), src, dst, desc);
    } else {
        DSD_SNPRINTF(line, sizeof line, "DMO %s src=%u dst=%u", tetra_dmo_msg_name(msg_type), src, dst);
    }
    tetra_dmo_ui_push(state, tetra_dmo_ui_category(msg_type), line);
}

int
tetra_dmo_format_mode(const dsd_state* state, char* buf, int buf_len) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    const char* mode;
    const char* channel;
    int n;
    if (!ts || !buf || buf_len <= 0) {
        return 0;
    }
    if (ts->dmo) {
        switch (ts->dmo_comm_type) {
            case 1: mode = "DMO repeater"; break;
            case 2: mode = "DMO gateway"; break;
            case 3: mode = "DMO repeater+gateway"; break;
            default: mode = "DMO direct"; break;
        }
        channel = ts->dmo_ab_usage == 2 ? "ch B" : ts->dmo_ab_usage == 1 ? "ch A freq-eff" : "ch A";
        n = snprintf(buf, (size_t)buf_len, "%s  %s  %s  slot %u frame %u", mode, channel,
                     ts->dmo_enc_state == 0 ? "clear" : "encrypted", ts->dmo_slot, ts->dmo_frame);
    } else if (ts->uplink) {
        if (ts->net_known) {
            n = snprintf(buf, (size_t)buf_len, "TMO uplink%s  MCC=%u MNC=%u CC=0x%02x",
                         state->tetra_sync.cb ? " control" : "", ts->mcc, ts->mnc, ts->colour);
        } else {
            n = snprintf(buf, (size_t)buf_len, "TMO uplink%s", state->tetra_sync.cb ? " control" : "");
        }
    } else if (ts->net_known) {
        n = snprintf(buf, (size_t)buf_len, "TMO downlink  MCC=%u MNC=%u CC=0x%02x", ts->mcc, ts->mnc, ts->colour);
    } else {
        n = snprintf(buf, (size_t)buf_len, "TMO downlink");
    }
    if (n > 0 && n < buf_len && !ts->dmo && ts->net_known) {
        const char* extra = tetra_air_ksg_name(tetra_air_ksg_lookup(state, ts->mcc, ts->mnc));
        int extra_n;
        if (!extra && (ts->enc_mode == 1 || ts->enc_mode == 2)) {
            extra = "encrypted";
        }
        if (extra) {
            extra_n = snprintf(buf + n, (size_t)(buf_len - n), "  %s", extra);
            if (extra_n > 0) {
                n += extra_n;
            }
        }
    }
    return n < 0 ? 0 : n;
}

void
tetra_dmo_publish_presence(dsd_state* state, const char* desc) {
    char line[DSD_TETRA_MESSAGE_TEXT_SIZE];
    if (!desc || desc[0] == '\0') {
        return;
    }
    DSD_SNPRINTF(line, sizeof line, "DMO presence | %s", desc);
    tetra_dmo_ui_push(state, DSD_TETRA_MESSAGE_CONTROL, line);
}

const char*
tetra_dmo_msg_name(int msg_type) {
    switch (msg_type) {
        case 0: return "DM-RESERVED";
        case 1: return "DM-SDS OCCUPIED";
        case 2: return "DM-TIMING REQUEST";
        case 3: return "DM-TIMING ACK";
        case 8: return "DM-SETUP";
        case 9: return "DM-SETUP PRES";
        case 10: return "DM-CONNECT";
        case 11: return "DM-DISCONNECT";
        case 12: return "DM-CONNECT ACK";
        case 13: return "DM-OCCUPIED";
        case 14: return "DM-RELEASE";
        case 15: return "DM-TX CEASED";
        case 16: return "DM-TX REQUEST";
        case 17: return "DM-TX ACCEPT";
        case 18: return "DM-PREEMPT";
        case 19: return "DM-PRE ACCEPT";
        case 20: return "DM-REJECT";
        case 21: return "DM-INFO";
        case 22: return "DM-SDS UDATA";
        case 23: return "DM-SDS DATA";
        case 24: return "DM-SDS ACK";
        case 25: return "GATEWAY-SPECIFIC";
        default: return "reserved";
    }
}

void
tetra_dmo_apply_sync(struct dsd_state* state, const tetra_dmo_sync_s_t* s, const tetra_dmo_sync_h_t* h) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !s) {
        return;
    }
    ts->dmo = 1;
    ts->dmo_since_sync = 0;
    ts->dmo_slot = s->slot;
    ts->dmo_frame = s->frame;
    ts->dmo_comm_type = s->comm_type;
    ts->dmo_ab_usage = s->ab_usage;
    ts->dmo_enc_state = s->enc_state;
    ts->dmo_sync_count++;
    /* Keep the TDMA display fields meaningful for the UI. */
    ts->tn = s->slot;
    ts->fn = s->frame;
    ts->tdma_valid = 1;

    if (!h || !h->has_addrs) {
        return;
    }
    ts->dmo_msg_type = h->msg_type;
    switch (h->msg_type) {
        case TETRA_DMO_MSG_SETUP:
        case TETRA_DMO_MSG_CONNECT_ACK:
        case TETRA_DMO_MSG_OCCUPIED:
            if (s->enc_state == 0) {
                ts->dmo_mni = h->mni;
                ts->dmo_src = h->src;
                ts->dmo_dst = h->dst;
                ts->dmo_dcc_seed = tetra_dmo_call_seed(s, h);
                ts->dmo_dcc_valid = 1;
            } else {
                /* DM-2-C scrambles with an encrypted short identity (EDSI);
                 * that needs key material this decoder does not have. */
                ts->dmo_dcc_valid = 0;
            }
            break;
        case TETRA_DMO_MSG_SDS_UDATA:
        case TETRA_DMO_MSG_SDS_DATA:
        case TETRA_DMO_MSG_SDS_ACK:
            /* The SCH/F fragments of a short data message are scrambled with the
             * colour code of the DMAC-SYNC that started the fragmentation
             * (EN 300 396-3 clause 8.2.4, EN 300 396-5 clause 8.2.4). */
            if (h->frag_flag && s->enc_state == 0) {
                ts->dmo_mni = h->mni;
                ts->dmo_src = h->src;
                ts->dmo_dst = h->dst;
                ts->dmo_dcc_seed = tetra_dmo_call_seed(s, h);
                ts->dmo_dcc_valid = 1;
            }
            break;
        case TETRA_DMO_MSG_TX_CEASED:
        case TETRA_DMO_MSG_DISCONNECT:
        case TETRA_DMO_MSG_RELEASE:
            /* The next talker sends its own DM-SETUP / DM-OCCUPIED. Until then
             * the old colour code would turn traffic into noise. */
            ts->dmo_dcc_valid = 0;
            break;
        case TETRA_DMO_MSG_GATEWAY:
            if (h->gw_subtype == TETRA_DMO_GW_SETUP && h->frag_flag && s->enc_state == 0) {
                /* A fragmented DM-GSETUP scrambles its SCH/F with this burst's
                 * gateway address and source address (EN 300 396-5 clause 8.2.4). */
                ts->dmo_mni = h->mni;
                ts->dmo_src = h->src;
                ts->dmo_dst = h->dst;
                ts->dmo_dcc_seed = tetra_dmo_call_seed(s, h);
                ts->dmo_dcc_valid = 1;
            } else if (h->gw_subtype == TETRA_DMO_GW_RELEASE) {
                ts->dmo_dcc_valid = 0;
            }
            break;
        default: break;
    }
}

int
tetra_dmo_log_dnb_pdu(const uint8_t* bits, int len, const char* kind, struct dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    int pos;
    int type;
    uint32_t dst = 0;
    uint32_t src = 0;
    uint32_t mni = 0;
    uint32_t msg;
    int gateway = state && ts->dmo_comm_type >= 2;
    if (!bits || len < 60) {
        return -1;
    }
    type = (int)tetra_bits_to_uint(bits, 0, 2);
    if (type == 3) {
        fprintf(stderr, "[TETRA DMO %s] DMAC-U-SIGNAL stolen2=%u\n", kind, bits[2] & 1u);
    } else if (type == 1) {
        fprintf(stderr, "[TETRA DMO %s] DMAC-%s fill=%u\n", kind, (bits[2] & 1u) ? "END/FRAG(subtype=END)" : "FRAG",
                bits[3] & 1u);
    } else if (type == 0) {
        unsigned fill = bits[2] & 1u;
        unsigned stolen2 = bits[3] & 1u;
        unsigned frag = bits[4] & 1u;
        unsigned null_pdu = bits[5] & 1u;
        if (null_pdu) {
            fprintf(stderr, "[TETRA DMO %s] DMAC-DATA null PDU\n", kind);
            return type;
        }
        unsigned countdown = (unsigned)tetra_bits_to_uint(bits, 6, 2);
        unsigned enc = (unsigned)tetra_bits_to_uint(bits, 8, 2);
        unsigned dst_type = (unsigned)tetra_bits_to_uint(bits, 10, 2);
        pos = 12;
        if (dst_type != 2u) {
            dst = tetra_bits_to_uint(bits, pos, 24);
            pos += 24;
        }
        unsigned src_type = (unsigned)tetra_bits_to_uint(bits, pos, 2);
        pos += 2;
        if (src_type != 2u) {
            src = tetra_bits_to_uint(bits, pos, 24);
            pos += 24;
        }
        /* Same MNI rule as DMAC-SYNC: always for direct and DM-REP, and for a
         * gateway only when a true SSI is present. */
        if ((!gateway || dst_type == 0 || src_type == 0) && pos + 24 <= len) {
            mni = tetra_bits_to_uint(bits, pos, 24);
            pos += 24;
        }
        msg = tetra_bits_to_uint(bits, pos, 5);
        tetra_dmo_sync_h_t elems;
        char desc[160];
        memset(&elems, 0, sizeof(elems));
        elems.msg_type = (uint8_t)msg;
        tetra_dmo_parse_elems(bits, pos + 5, len, &elems);
        desc[0] = '\0';
        tetra_dmo_describe_elems(&elems, desc, (int)sizeof(desc));
        tetra_dmo_publish(state, (int)msg, src, dst, desc);
        fprintf(stderr,
                "[TETRA DMO %s] DMAC-DATA fill=%u stolen2=%u frag=%u cd=%u enc=%u %s src=%u dst=%u MNI=%u/%u%s%s\n",
                kind, fill, stolen2, frag, countdown, enc, tetra_dmo_msg_name((int)msg), src, dst, (mni >> 14) & 0x3FFu,
                mni & 0x3FFFu, desc[0] ? " | " : "", desc);
    } else {
        fprintf(stderr, "[TETRA DMO %s] reserved MAC PDU type\n", kind);
    }
    return type;
}
