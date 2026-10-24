// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/access_code_view.h>
#include <dsd-neo/app_control/site_identity_view.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/platform.h>

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* U+00B7 MIDDLE DOT between parts, as the frontends separate details. */
static const char k_dot[] = " \xC2\xB7 ";

static void site_token(dsd_app_site_identity* out, int compact, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 3, 4);

static void
site_token(dsd_app_site_identity* out, int compact, const char* fmt, ...) {
    if (out->token_count >= DSD_APP_SITE_IDENTITY_MAX_TOKENS) {
        return;
    }
    dsd_app_site_identity_token* token = &out->tokens[out->token_count];
    va_list ap;
    va_start(ap, fmt);
    const int n = DSD_VSNPRINTF(token->text, sizeof(token->text), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        token->text[0] = '\0';
        return;
    }
    token->compact = compact ? 1U : 0U;
    out->token_count++;
}

static int
has(const dsd_received_identity* id, uint32_t part) {
    return (id->valid & part) != 0U;
}

static void
site_access(dsd_app_site_identity* out, const dsd_received_identity* id) {
    char text[16];
    if (has(id, DSD_RXID_ACCESS)
        && dsd_app_access_code_format(id->access_kind, id->access, DSD_APP_ACCESS_CODE_SHORT, text, sizeof(text))) {
        site_token(out, 1, "%s", text);
    }
}

static int
site_starts_with(const char* text, size_t len, const char* prefix) {
    const size_t n = strlen(prefix);
    return len >= n && strncmp(text, prefix, n) == 0;
}

/* The DMR site text, one token per " · " part (dsd_received_identity::dmr_site_text): the network ("Net n") and site
   ("Site n", "Site n.m") identify it on a short line; the Tier III model, the Capacity Plus and Connect Plus tags and
   the system code are details. */
static void
site_dmr_text(dsd_app_site_identity* out, const char* text) {
    const size_t dot_len = sizeof(k_dot) - 1U;
    const char* part = text;
    while (part[0] != '\0') {
        const char* const end = strstr(part, k_dot);
        const size_t len = end != NULL ? (size_t)(end - part) : strlen(part);
        if (len > 0U) {
            const int compact = site_starts_with(part, len, "Net ") || site_starts_with(part, len, "Site ");
            site_token(out, compact, "%.*s", (int)len, part);
        }
        if (end == NULL) {
            break;
        }
        part = end + dot_len;
    }
}

static void
site_dmr(dsd_app_site_identity* out, const dsd_received_identity* id) {
    site_access(out, id);
    if (has(id, DSD_RXID_DMR_SITE_TEXT)) {
        site_dmr_text(out, id->dmr_site_text);
    }
    if (has(id, DSD_RXID_DMR_REST_LSN)) {
        site_token(out, 1, "Rest LSN %u", (unsigned)id->dmr_rest_lsn);
    }
    if (has(id, DSD_RXID_DMR_XPT_FREE_LCN)) {
        site_token(out, 1, "Free LCN %u", (unsigned)id->dmr_xpt_free_lcn);
    }
    /* The carrier uses Restricted Access to System (dmr_ras.c); dmr_ras_view.h says whether relaxed CRC decodes it. */
    if (has(id, DSD_RXID_DMR_RAS)) {
        site_token(out, 1, "%s", "RAS");
    }
    if (has(id, DSD_RXID_DMR_T3_SYSCODE) && !has(id, DSD_RXID_DMR_SITE_TEXT)) {
        site_token(out, 0, "SYS %04X", (unsigned)id->dmr_t3_syscode);
    }
    if (has(id, DSD_RXID_DMR_BRAND)) {
        site_token(out, 0, "%s", id->dmr_brand);
    }
}

static void
site_p25(dsd_app_site_identity* out, const dsd_received_identity* id) {
    site_access(out, id);
    if (has(id, DSD_RXID_P25_SYSID)) {
        site_token(out, 1, "SYS %03X", (unsigned)id->p25_sysid);
    }
    if (has(id, DSD_RXID_P25_SITE)) {
        site_token(out, 1, "RFSS %u", (unsigned)id->p25_rfss);
        site_token(out, 1, "SITE %u", (unsigned)id->p25_site);
    }
    if (has(id, DSD_RXID_P25_WACN)) {
        site_token(out, 1, "WACN %05X", (unsigned)id->p25_wacn);
    }
    if (has(id, DSD_RXID_P25_LRA)) {
        site_token(out, 0, "LRA %02X", (unsigned)id->p25_lra);
    }
}

static int
site_is_idas(const dsd_received_identity* id) {
    return has(id, DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_TYPED_SYSTEM);
}

static void
site_nxdn(dsd_app_site_identity* out, const dsd_received_identity* id) {
    static const char* const categories[] = {"Global", "Regional", "Local"};
    static const char* const site_types[] = {"", "Wide", "Middle", "Narrow"};
    site_access(out, id);
    if (has(id, DSD_RXID_NXDN_IDAS_AREA)) {
        site_token(out, 1, "Area %u", (unsigned)id->nxdn_idas_area);
    }
    if (has(id, DSD_RXID_NXDN_TYPED_SYSTEM)) {
        site_token(out, 1, "SYS %u", (unsigned)id->nxdn_typed_sys);
    }
    if (has(id, DSD_RXID_NXDN_IDAS_SITE)) {
        site_token(out, 1, "SITE %u", (unsigned)id->nxdn_idas_site_code);
        site_token(out, 0, "%s", site_types[id->nxdn_idas_site_type & 3U]);
    } else if (has(id, DSD_RXID_NXDN_TYPED_SYSTEM)) {
        site_token(out, 1, "SITE %u", (unsigned)id->nxdn_typed_site);
    }
    if (has(id, DSD_RXID_NXDN_LOCATION)) {
        site_token(out, 1, "SYS %u", (unsigned)id->nxdn_sys);
        site_token(out, 1, "SITE %u", (unsigned)id->nxdn_site);
        if (id->nxdn_category < 3U) {
            site_token(out, 0, "%s", categories[id->nxdn_category]);
        }
    }
}

static void
site_tetra(dsd_app_site_identity* out, const dsd_received_identity* id) {
    if (has(id, DSD_RXID_TETRA_NET)) {
        site_token(out, 1, "MCC %u", (unsigned)id->tetra_mcc);
        site_token(out, 1, "MNC %u", (unsigned)id->tetra_mnc);
    }
    site_access(out, id);
    if (has(id, DSD_RXID_TETRA_LA)) {
        site_token(out, 0, "LA %u", (unsigned)id->tetra_la);
    }
}

static void
site_edacs(dsd_app_site_identity* out, const dsd_received_identity* id, const dsd_state* state) {
    if (has(id, DSD_RXID_EDACS_SITE)) {
        site_token(out, 1, "SITE %03u [%02X]", (unsigned)id->edacs_site, (unsigned)id->edacs_site);
    }
    if (has(id, DSD_RXID_EDACS_SYSTEM)) {
        site_token(out, 1, "SYS %u", (unsigned)id->edacs_system);
    }
    if (has(id, DSD_RXID_EDACS_AREA)) {
        site_token(out, 0, "Area %u", (unsigned)id->edacs_area);
    }
    if (has(id, DSD_RXID_EDACS_CC_LCN)) {
        site_token(out, 0, "CC LCN %u", (unsigned)id->edacs_cc_lcn);
    }
    if (out->token_count > 0U) {
        site_token(out, 0, "%s", state->ea_mode == 1 ? "Extended Addressing" : "Standard/Networked");
        site_token(out, 0, "%s", state->esk_mask == 0xA0 ? "ESK" : "No ESK");
    }
}

static const char*
site_protocol(const dsd_received_identity* id) {
    switch (id->family) {
        case DSD_RXID_FAMILY_DMR: return "DMR";
        case DSD_RXID_FAMILY_P25: return "P25";
        case DSD_RXID_FAMILY_NXDN: return site_is_idas(id) ? "IDAS" : "NXDN";
        case DSD_RXID_FAMILY_DPMR: return "dPMR";
        case DSD_RXID_FAMILY_M17: return "M17";
        case DSD_RXID_FAMILY_TETRA: return "TETRA";
        case DSD_RXID_FAMILY_EDACS: return "EDACS";
        default: return "";
    }
}

void
dsd_app_site_identity_view(const dsd_state* state, dsd_app_site_identity* out) {
    if (out == NULL) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (state == NULL) {
        return;
    }
    const dsd_received_identity* id = &state->rx_id;
    out->epoch = id->epoch;
    switch (id->family) {
        case DSD_RXID_FAMILY_DMR: site_dmr(out, id); break;
        case DSD_RXID_FAMILY_P25: site_p25(out, id); break;
        case DSD_RXID_FAMILY_NXDN: site_nxdn(out, id); break;
        case DSD_RXID_FAMILY_DPMR:
        case DSD_RXID_FAMILY_M17: site_access(out, id); break;
        case DSD_RXID_FAMILY_TETRA: site_tetra(out, id); break;
        case DSD_RXID_FAMILY_EDACS: site_edacs(out, id, state); break;
        default: break;
    }
    if (has(id, DSD_RXID_CC_FREQ)) {
        out->cc_freq_hz = id->cc_freq_hz;
    }
    if (out->token_count == 0U && out->cc_freq_hz == 0U) {
        return;
    }
    DSD_SNPRINTF(out->protocol, sizeof(out->protocol), "%s", site_protocol(id));
    out->family = id->family;
    out->live = (id->fresh != 0U && dsd_rxid_family_for_sync(state->synctype) == (dsd_rxid_family)id->family) ? 1U : 0U;
}

/* Append @p lead and @p text at @p len; a text that does not fit is cut short. Returns the new length. */
static size_t
site_append(char* out, size_t out_size, size_t len, const char* lead, const char* text) {
    const int n = DSD_SNPRINTF(out + len, out_size - len, "%s%s", lead, text);
    len += (n > 0) ? (size_t)n : 0U;
    return len < out_size ? len : out_size - 1U;
}

size_t
dsd_app_site_identity_format(const dsd_app_site_identity* view, int form, int with_protocol, char* out,
                             size_t out_size) {
    if (out == NULL || out_size == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (view == NULL) {
        return 0U;
    }
    size_t len = 0U;
    int shown = 0;
    for (size_t i = 0U; i < view->token_count && i < DSD_APP_SITE_IDENTITY_MAX_TOKENS; i++) {
        const dsd_app_site_identity_token* token = &view->tokens[i];
        if (form == DSD_APP_SITE_IDENTITY_COMPACT && !token->compact) {
            continue;
        }
        if (shown == 0 && with_protocol && view->protocol[0] != '\0') {
            len = site_append(out, out_size, len, "", view->protocol);
            shown++;
        }
        len = site_append(out, out_size, len, shown > 0 ? k_dot : "", token->text);
        shown++;
    }
    return len;
}
