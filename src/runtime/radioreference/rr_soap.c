// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * SOAP request construction and rpc/encoded response decoding.
 *
 * Envelope construction is always compiled; the decoder needs expat and returns
 * DSD_RR_ERR_UNSUPPORTED without it.
 */

#include "rr_soap.h"

#include "rr_internal.h"

#include <dsd-neo/core/safe_api.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_EXPAT
#include <expat.h>
#endif

/* ------------------------------------------------------------------------- */
/* Request construction                                                       */
/* ------------------------------------------------------------------------- */

/** Growable text buffer for assembling the envelope. */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} rr_strbuf;

/**
 * @brief Grow the envelope buffer to hold at least `needed` bytes.
 *
 * @param sb     Buffer.
 * @param needed Required capacity including the terminator.
 * @return 0 on success, -1 on allocation failure.
 */
static int
rr_sb_reserve(rr_strbuf* sb, size_t needed) {
    if (needed <= sb->cap) {
        return 0;
    }
    size_t next = (sb->cap == 0U) ? 1024U : sb->cap;
    while (next < needed) {
        if (next > SIZE_MAX / 2U) {
            next = needed;
            break;
        }
        next *= 2U;
    }
    char* grown = (char*)realloc(sb->data, next);
    if (grown == NULL) {
        return -1;
    }
    sb->data = grown;
    sb->cap = next;
    return 0;
}

/**
 * @brief Append a NUL-terminated string to the envelope buffer.
 *
 * @param sb   Buffer.
 * @param text Text to append.
 * @return 0 on success, -1 on failure.
 */
static int
rr_sb_append(rr_strbuf* sb, const char* text) {
    if (sb == NULL || text == NULL) {
        return -1;
    }
    const size_t n = strlen(text);
    if (rr_sb_reserve(sb, sb->len + n + 1U) != 0) {
        return -1;
    }
    DSD_MEMCPY(sb->data + sb->len, text, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

int
rr_xml_escape(const char* in, char* out, size_t out_sz) {
    if (in == NULL || out == NULL || out_sz == 0U) {
        return -1;
    }

    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p != '\0'; p++) {
        const char* rep = NULL;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&apos;"; break;
            default: break;
        }

        if (rep != NULL) {
            const size_t rl = strlen(rep);
            if (o + rl >= out_sz) {
                out[0] = '\0';
                return -1;
            }
            DSD_MEMCPY(out + o, rep, rl);
            o += rl;
        } else {
            if (o + 1U >= out_sz) {
                out[0] = '\0';
                return -1;
            }
            out[o] = (char)*p;
            o++;
        }
    }

    out[o] = '\0';
    return 0;
}

/** Escaped credential and parameter values never exceed six bytes per input byte. */
#define RR_ESCAPED_MAX 1024

/**
 * @brief Emit one `<name xsi:type="...">value</name>` request part.
 *
 * @param sb    Envelope buffer.
 * @param param Part to emit.
 * @return 0 on success, -1 on failure.
 */
/**
 * @brief Emit one complete `<name xsi:type="type">value</name>` line.
 *
 * The appends accumulate into `rc` rather than short-circuiting: the buffer is
 * left consistent either way, and a chain of `||` would push this function past
 * the project's cyclomatic-complexity ceiling for no benefit.
 *
 * @param sb     Envelope buffer.
 * @param indent Leading whitespace.
 * @param name   Element name.
 * @param type   xsi:type value.
 * @param value  Already-escaped element text.
 * @return 0 on success, -1 on failure.
 */
static int
rr_emit_element(rr_strbuf* sb, const char* indent, const char* name, const char* type, const char* value) {
    int rc = 0;
    rc |= rr_sb_append(sb, indent);
    rc |= rr_sb_append(sb, "<");
    rc |= rr_sb_append(sb, name);
    rc |= rr_sb_append(sb, " xsi:type=\"");
    rc |= rr_sb_append(sb, type);
    rc |= rr_sb_append(sb, "\">");
    rc |= rr_sb_append(sb, value);
    rc |= rr_sb_append(sb, "</");
    rc |= rr_sb_append(sb, name);
    rc |= rr_sb_append(sb, ">\n");
    return rc;
}

/**
 * @brief Render a parameter's value and report its xsd type.
 *
 * @param param    Part to render.
 * @param value    Destination buffer.
 * @param value_sz Destination size, passed explicitly.
 * @return The xsd type name, or NULL when the value does not fit.
 */
static const char*
rr_render_param_value(const rr_soap_param* param, char* value, size_t value_sz) {
    if (param->kind == RR_PARAM_INT) {
        const int n = DSD_SNPRINTF(value, value_sz, "%ld", param->ivalue);
        if (n <= 0 || (size_t)n >= value_sz) {
            return NULL;
        }
        return "xsd:int";
    }
    if (rr_xml_escape(param->svalue != NULL ? param->svalue : "", value, value_sz) != 0) {
        return NULL;
    }
    return "xsd:string";
}

static int
rr_emit_param(rr_strbuf* sb, const rr_soap_param* param) {
    if (param == NULL || param->name == NULL) {
        return -1;
    }

    char value[RR_ESCAPED_MAX];
    const char* type = rr_render_param_value(param, value, sizeof(value));
    if (type == NULL) {
        return -1;
    }
    return rr_emit_element(sb, "      ", param->name, type, value);
}

/**
 * @brief Emit one escaped authInfo member.
 *
 * @param sb   Envelope buffer.
 * @param name Member name.
 * @param text Member value.
 * @return 0 on success, -1 on failure.
 */
static int
rr_emit_auth_member(rr_strbuf* sb, const char* name, const char* text) {
    char value[RR_ESCAPED_MAX];
    if (rr_xml_escape(text != NULL ? text : "", value, sizeof(value)) != 0) {
        return -1;
    }
    return rr_emit_element(sb, "        ", name, "xsd:string", value);
}

/**
 * @brief Emit the authInfo struct.
 *
 * `version` and `style` are pinned here rather than by the endpoint URL: the wiki
 * says an unspecified version falls back to version 1, which predates the enc,
 * colorCode, ch_id and tdma_cc fields this client depends on.
 *
 * @param sb   Envelope buffer.
 * @param auth Credentials.
 * @return 0 on success, -1 on failure.
 */
static int
rr_emit_auth(rr_strbuf* sb, const dsd_rr_auth* auth) {
    int rc = 0;
    rc |= rr_sb_append(sb, "      <authInfo xsi:type=\"ns1:authInfo\">\n");
    rc |= rr_emit_auth_member(sb, "appKey", auth->app_key);
    rc |= rr_emit_auth_member(sb, "username", auth->username);
    rc |= rr_emit_auth_member(sb, "password", auth->password);
    rc |= rr_emit_auth_member(sb, "version", "18");
    rc |= rr_emit_auth_member(sb, "style", "rpc");
    rc |= rr_sb_append(sb, "      </authInfo>\n");
    return rc;
}

int
rr_soap_build_request(const char* method, const rr_soap_param* params, size_t n, const dsd_rr_auth* auth, char** out,
                      size_t* out_len) {
    if (method == NULL || out == NULL || out_len == NULL || (params == NULL && n > 0U)) {
        return -1;
    }

    rr_strbuf sb = {NULL, 0, 0};
    int rc = 0;
    rc |= rr_sb_append(&sb, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<SOAP-ENV:Envelope\n"
                            "    xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\"\n"
                            "    xmlns:ns1=\"http://api.radioreference.com/soap2\"\n"
                            "    xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"\n"
                            "    xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
                            "    SOAP-ENV:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\n"
                            "  <SOAP-ENV:Body>\n"
                            "    <ns1:");
    rc |= rr_sb_append(&sb, method);
    rc |= rr_sb_append(&sb, ">\n");

    for (size_t i = 0; rc == 0 && i < n; i++) {
        rc |= rr_emit_param(&sb, &params[i]);
    }
    if (rc == 0 && auth != NULL) {
        rc |= rr_emit_auth(&sb, auth);
    }

    rc |= rr_sb_append(&sb, "    </ns1:");
    rc |= rr_sb_append(&sb, method);
    rc |= rr_sb_append(&sb, ">\n"
                            "  </SOAP-ENV:Body>\n"
                            "</SOAP-ENV:Envelope>\n");

    if (rc != 0) {
        free(sb.data);
        return -1;
    }

    *out = sb.data;
    *out_len = sb.len;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Response decoding                                                          */
/* ------------------------------------------------------------------------- */

#ifdef USE_EXPAT

#define RR_MAX_DEPTH  32
#define RR_MAX_NAME   64
#define RR_LEAF_MAX   4096
#define RR_DETAIL_MAX 256

typedef enum { RR_F_INT, RR_F_U32, RR_F_STR, RR_F_MHZ } rr_field_kind;

/** One leaf element mapped onto a record member. */
typedef struct {
    const char* name;
    rr_field_kind kind;
    size_t offset;
    size_t size;
} rr_field_def;

#define RR_STR(type, member, elem) {elem, RR_F_STR, offsetof(type, member), sizeof(((type*)0)->member)}
#define RR_INT(type, member, elem) {elem, RR_F_INT, offsetof(type, member), 0}
#define RR_U32(type, member, elem) {elem, RR_F_U32, offsetof(type, member), 0}
#define RR_MHZ(type, member, elem) {elem, RR_F_MHZ, offsetof(type, member), 0}

static const rr_field_def k_country_fields[] = {
    RR_INT(dsd_rr_country, coid, "coid"),
    RR_STR(dsd_rr_country, name, "countryName"),
    RR_STR(dsd_rr_country, code, "countryCode"),
};

static const rr_field_def k_state_fields[] = {
    RR_INT(dsd_rr_state, stid, "stid"),
    RR_STR(dsd_rr_state, name, "stateName"),
    RR_STR(dsd_rr_state, code, "stateCode"),
};

static const rr_field_def k_county_fields[] = {
    RR_INT(dsd_rr_county, ctid, "ctid"),
    RR_STR(dsd_rr_county, county_name, "countyName"),
};

static const rr_field_def k_trs_summary_fields[] = {
    RR_INT(dsd_rr_trs_summary, sid, "sid"),         RR_STR(dsd_rr_trs_summary, name, "sName"),
    RR_INT(dsd_rr_trs_summary, type_id, "sType"),   RR_INT(dsd_rr_trs_summary, flavor_id, "sFlavor"),
    RR_INT(dsd_rr_trs_summary, voice_id, "sVoice"), RR_STR(dsd_rr_trs_summary, city, "sCity"),
};

static const rr_field_def k_trs_details_fields[] = {
    RR_STR(dsd_rr_trs_details, name, "sName"),        RR_INT(dsd_rr_trs_details, type_id, "sType"),
    RR_INT(dsd_rr_trs_details, flavor_id, "sFlavor"), RR_INT(dsd_rr_trs_details, voice_id, "sVoice"),
    RR_STR(dsd_rr_trs_details, city, "sCity"),
};

static const rr_field_def k_sysid_fields[] = {
    RR_STR(dsd_rr_trs_sysid, sysid, "sysid"),
    RR_STR(dsd_rr_trs_sysid, ct, "ct"),
    RR_STR(dsd_rr_trs_sysid, wacn, "wacn"),
    RR_STR(dsd_rr_trs_sysid, model, "model"),
};

static const rr_field_def k_site_fields[] = {
    RR_INT(dsd_rr_site, site_db_id, "siteId"),
    RR_INT(dsd_rr_site, site_number, "siteNumber"),
    RR_STR(dsd_rr_site, descr, "siteDescr"),
    RR_INT(dsd_rr_site, zone_number, "zoneNumber"),
    RR_STR(dsd_rr_site, zone_descr, "zoneDescr"),
    RR_INT(dsd_rr_site, rfss, "rfss"),
    RR_STR(dsd_rr_site, nac, "nac"),
    RR_INT(dsd_rr_site, ran, "ran"),
    RR_STR(dsd_rr_site, modulation, "siteModulation"),
    RR_INT(dsd_rr_site, splinter, "splinter"),
    RR_INT(dsd_rr_site, rebanded, "rebanded"),
    RR_INT(dsd_rr_site, tdma_cc, "tdma_cc"),
};

static const rr_field_def k_site_freq_fields[] = {
    RR_INT(dsd_rr_site_freq, lcn, "lcn"),     RR_MHZ(dsd_rr_site_freq, freq_hz, "freq"),
    RR_STR(dsd_rr_site_freq, use, "use"),     RR_STR(dsd_rr_site_freq, color_code, "colorCode"),
    RR_STR(dsd_rr_site_freq, ch_id, "ch_id"),
};

static const rr_field_def k_talkgroup_fields[] = {
    RR_U32(dsd_rr_talkgroup, tg_dec, "tgDec"),      RR_INT(dsd_rr_talkgroup, tg_cid, "tgCid"),
    RR_STR(dsd_rr_talkgroup, alpha_tag, "tgAlpha"), RR_STR(dsd_rr_talkgroup, description, "tgDescr"),
    RR_STR(dsd_rr_talkgroup, mode, "tgMode"),       RR_INT(dsd_rr_talkgroup, enc, "enc"),
    RR_STR(dsd_rr_talkgroup, slot, "tgSlot"),
};

static const rr_field_def k_talkgroup_cat_fields[] = {
    RR_INT(dsd_rr_talkgroup_cat, tg_cid, "tgCid"),
    RR_STR(dsd_rr_talkgroup_cat, name, "tgCname"),
};

static const rr_field_def k_user_info_fields[] = {
    RR_STR(dsd_rr_user_info, username, "username"),
    RR_STR(dsd_rr_user_info, sub_expire, "subExpireDate"),
};

static const rr_field_def k_zip_info_fields[] = {
    RR_INT(dsd_rr_zip_info, zip_code, "zipCode"),
    RR_STR(dsd_rr_zip_info, city, "city"),
    RR_INT(dsd_rr_zip_info, stid, "stid"),
    RR_INT(dsd_rr_zip_info, ctid, "ctid"),
};

/** Scratch records plus the element stack for one parse. */
typedef struct {
    XML_Parser parser;

    char names[RR_MAX_DEPTH][RR_MAX_NAME];
    int depth;

    char text[RR_LEAF_MAX];
    size_t text_len;
    int leaf_nil;

    rr_shape shape;
    void* sink;
    size_t list_cap;

    int body_depth;
    int return_depth;
    int record_depth;
    int sub_depth;

    int in_fault;
    int fault_depth;
    char fault_code[64];
    char fault_string[RR_DETAIL_MAX];

    int failed;
    dsd_rr_status fail_status;
    char fail_detail[RR_DETAIL_MAX];

    /* Scratch for the record currently being assembled. */
    dsd_rr_country country;
    dsd_rr_state state;
    dsd_rr_county county;
    dsd_rr_trs_summary trs_summary;
    dsd_rr_trs_sysid sysid;
    dsd_rr_site site;
    size_t site_freq_cap;
    dsd_rr_site_freq freq;
    dsd_rr_talkgroup talkgroup;
    dsd_rr_talkgroup_cat talkgroup_cat;
    dsd_rr_support_entry support;
    size_t sysid_cap;

    /* getStateInfo carries stid/stateName once, above the county rows. */
    int enclosing_stid;
    char enclosing_state_name[64];
} rr_parse_ctx;

/**
 * @brief Strip an XML qualified name down to its local part.
 *
 * @param qname Possibly prefixed name.
 * @return Pointer past the last ':', or qname when unprefixed.
 */
static const char*
rr_local_name(const char* qname) {
    const char* colon = strrchr(qname, ':');
    return (colon != NULL) ? (colon + 1) : qname;
}

/**
 * @brief Record the first failure; later ones do not overwrite it.
 *
 * @param ctx    Parse context.
 * @param status Failure class.
 * @param detail Human-readable detail; never credentials.
 */
static void
rr_fail(rr_parse_ctx* ctx, dsd_rr_status status, const char* detail) {
    if (ctx->failed) {
        return;
    }
    ctx->failed = 1;
    ctx->fail_status = status;
    rr_copy_field(ctx->fail_detail, sizeof(ctx->fail_detail), detail);
}

/**
 * @brief Store one leaf value into a record member.
 *
 * @param record Record base address.
 * @param table  Field table.
 * @param n      Field count.
 * @param name   Local element name.
 * @param text   Accumulated character data.
 */
static void
rr_apply_field(void* record, const rr_field_def* table, size_t n, const char* name, const char* text) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) != 0) {
            continue;
        }
        char* base = (char*)record + table[i].offset;
        switch (table[i].kind) {
            case RR_F_STR: rr_copy_field(base, table[i].size, text); break;
            case RR_F_INT: {
                long value = 0;
                if (rr_parse_long_strict(text, &value) == 0) {
                    const int narrowed = (int)value;
                    DSD_MEMCPY(base, &narrowed, sizeof(narrowed));
                }
                break;
            }
            case RR_F_U32: {
                long value = 0;
                if (rr_parse_long_strict(text, &value) == 0 && value >= 0) {
                    const uint32_t narrowed = (uint32_t)value;
                    DSD_MEMCPY(base, &narrowed, sizeof(narrowed));
                }
                break;
            }
            case RR_F_MHZ: {
                long long hz = 0;
                if (dsd_rr_mhz_to_hz(text, &hz) == 0) {
                    DSD_MEMCPY(base, &hz, sizeof(hz));
                }
                break;
            }
            default: break;
        }
        return;
    }
}

/**
 * @brief Reset the scratch record that a fresh `<item>` opens.
 *
 * @param ctx Parse context.
 */
static void
rr_reset_record(rr_parse_ctx* ctx) {
    DSD_MEMSET(&ctx->country, 0, sizeof(ctx->country));
    DSD_MEMSET(&ctx->state, 0, sizeof(ctx->state));
    DSD_MEMSET(&ctx->county, 0, sizeof(ctx->county));
    DSD_MEMSET(&ctx->trs_summary, 0, sizeof(ctx->trs_summary));
    DSD_MEMSET(&ctx->talkgroup, 0, sizeof(ctx->talkgroup));
    DSD_MEMSET(&ctx->talkgroup_cat, 0, sizeof(ctx->talkgroup_cat));
    DSD_MEMSET(&ctx->support, 0, sizeof(ctx->support));
    DSD_MEMSET(&ctx->site, 0, sizeof(ctx->site));
    ctx->site_freq_cap = 0;
}

/**
 * @brief Reset the scratch sub-record (`siteFreqs` / `sysid` member).
 *
 * `lcn` starts at -1 so a nil or absent value stays distinguishable from zero.
 *
 * @param ctx Parse context.
 */
static void
rr_reset_sub(rr_parse_ctx* ctx) {
    DSD_MEMSET(&ctx->freq, 0, sizeof(ctx->freq));
    ctx->freq.lcn = -1;
    DSD_MEMSET(&ctx->sysid, 0, sizeof(ctx->sysid));
}

/**
 * @brief Append one element to a sink list, growing it as needed.
 *
 * @param ctx       Parse context (owns the capacity).
 * @param items     Address of the list's item pointer.
 * @param count     Address of the list's element count.
 * @param elem      Element to copy in.
 * @param elem_size Element size.
 * @return 0 on success, -1 on allocation failure.
 */
static int
rr_list_append(rr_parse_ctx* ctx, void** items, size_t* count, const void* elem, size_t elem_size) {
    if (rr_array_reserve(items, &ctx->list_cap, *count + 1U, elem_size) != 0) {
        return -1;
    }
    DSD_MEMCPY((char*)*items + (*count * elem_size), elem, elem_size);
    (*count)++;
    return 0;
}

/**
 * @brief Element name whose `<item>` children open a top-level record.
 *
 * @param shape Response shape.
 * @return Parent element name, or NULL when the shape has no repeated record.
 */
static const char*
rr_record_parent(rr_shape shape) {
    switch (shape) {
        case RR_SHAPE_STATE_LIST: return "stateList";
        case RR_SHAPE_COUNTY_LIST: return "countyList";
        case RR_SHAPE_TRS_LIST: return "trsList";
        case RR_SHAPE_COUNTRY_LIST:
        case RR_SHAPE_SITE_LIST:
        case RR_SHAPE_TALKGROUP_LIST:
        case RR_SHAPE_TALKGROUP_CAT_LIST:
        case RR_SHAPE_SUPPORT_TYPE:
        case RR_SHAPE_SUPPORT_FLAVOR:
        case RR_SHAPE_SUPPORT_VOICE: return "return";
        default: return NULL;
    }
}

/**
 * @brief Element name whose `<item>` children open a sub-record.
 *
 * @param shape Response shape.
 * @return Parent element name, or NULL when the shape has no sub-record.
 */
static const char*
rr_sub_parent(rr_shape shape) {
    if (shape == RR_SHAPE_SITE_LIST) {
        return "siteFreqs";
    }
    if (shape == RR_SHAPE_TRS_DETAILS) {
        return "sysid";
    }
    return NULL;
}

/**
 * @brief Scratch record and field table for the shape's repeated element.
 *
 * @param ctx    Parse context.
 * @param table  Receives the field table, or NULL when handled specially.
 * @param n      Receives the field count.
 * @return Scratch record address, or NULL when the shape has no record.
 */
static void*
rr_record_table(rr_parse_ctx* ctx, const rr_field_def** table, size_t* n) {
    *table = NULL;
    *n = 0;
    switch (ctx->shape) {
        case RR_SHAPE_COUNTRY_LIST:
            *table = k_country_fields;
            *n = sizeof(k_country_fields) / sizeof(k_country_fields[0]);
            return &ctx->country;
        case RR_SHAPE_STATE_LIST:
            *table = k_state_fields;
            *n = sizeof(k_state_fields) / sizeof(k_state_fields[0]);
            return &ctx->state;
        case RR_SHAPE_COUNTY_LIST:
            *table = k_county_fields;
            *n = sizeof(k_county_fields) / sizeof(k_county_fields[0]);
            return &ctx->county;
        case RR_SHAPE_TRS_LIST:
            *table = k_trs_summary_fields;
            *n = sizeof(k_trs_summary_fields) / sizeof(k_trs_summary_fields[0]);
            return &ctx->trs_summary;
        case RR_SHAPE_SITE_LIST:
            *table = k_site_fields;
            *n = sizeof(k_site_fields) / sizeof(k_site_fields[0]);
            return &ctx->site;
        case RR_SHAPE_TALKGROUP_LIST:
            *table = k_talkgroup_fields;
            *n = sizeof(k_talkgroup_fields) / sizeof(k_talkgroup_fields[0]);
            return &ctx->talkgroup;
        case RR_SHAPE_TALKGROUP_CAT_LIST:
            *table = k_talkgroup_cat_fields;
            *n = sizeof(k_talkgroup_cat_fields) / sizeof(k_talkgroup_cat_fields[0]);
            return &ctx->talkgroup_cat;
        case RR_SHAPE_SUPPORT_TYPE:
        case RR_SHAPE_SUPPORT_FLAVOR:
        case RR_SHAPE_SUPPORT_VOICE: return &ctx->support;
        default: return NULL;
    }
}

/**
 * @brief Apply a leaf inside a support-list `<item>`.
 *
 * The ID member is named differently per call (sType/sFlavor/sVoice) and the type
 * list uses sType for both key halves, so a plain field table cannot express it.
 *
 * @param ctx  Parse context.
 * @param name Local element name.
 */
static void
rr_apply_support_leaf(rr_parse_ctx* ctx, const char* name) {
    long value = 0;
    if (strcmp(name, "sType") == 0) {
        if (rr_parse_long_strict(ctx->text, &value) == 0) {
            ctx->support.stype = (int)value;
            if (ctx->shape == RR_SHAPE_SUPPORT_TYPE) {
                ctx->support.id = (int)value;
            }
        }
        return;
    }
    if ((ctx->shape == RR_SHAPE_SUPPORT_FLAVOR && strcmp(name, "sFlavor") == 0)
        || (ctx->shape == RR_SHAPE_SUPPORT_VOICE && strcmp(name, "sVoice") == 0)) {
        if (rr_parse_long_strict(ctx->text, &value) == 0) {
            ctx->support.id = (int)value;
        }
        return;
    }
    if (strcmp(name, "sTypeDescr") == 0 || strcmp(name, "sFlavorDescr") == 0 || strcmp(name, "sVoiceDescr") == 0) {
        rr_copy_field(ctx->support.descr, sizeof(ctx->support.descr), ctx->text);
    }
}

/**
 * @brief Apply a leaf that sits directly under `<return>`.
 *
 * @param ctx  Parse context.
 * @param name Local element name.
 */
static void
rr_apply_root_leaf(rr_parse_ctx* ctx, const char* name) {
    switch (ctx->shape) {
        case RR_SHAPE_USER_INFO:
            rr_apply_field(ctx->sink, k_user_info_fields, sizeof(k_user_info_fields) / sizeof(k_user_info_fields[0]),
                           name, ctx->text);
            break;
        case RR_SHAPE_ZIP_INFO:
            rr_apply_field(ctx->sink, k_zip_info_fields, sizeof(k_zip_info_fields) / sizeof(k_zip_info_fields[0]), name,
                           ctx->text);
            break;
        case RR_SHAPE_TRS_DETAILS:
            rr_apply_field(ctx->sink, k_trs_details_fields,
                           sizeof(k_trs_details_fields) / sizeof(k_trs_details_fields[0]), name, ctx->text);
            break;
        case RR_SHAPE_COUNTY_LIST: {
            /* StateInfo carries these once, above the rows they belong to. */
            long value = 0;
            if (strcmp(name, "stid") == 0 && rr_parse_long_strict(ctx->text, &value) == 0) {
                ctx->enclosing_stid = (int)value;
            } else if (strcmp(name, "stateName") == 0) {
                rr_copy_field(ctx->enclosing_state_name, sizeof(ctx->enclosing_state_name), ctx->text);
            }
            break;
        }
        default: break;
    }
}

/**
 * @brief Route one leaf to the record, sub-record or root it belongs to.
 *
 * @param ctx   Parse context.
 * @param name  Local element name.
 * @param depth 1-based depth of the leaf element.
 */
static void
rr_apply_leaf(rr_parse_ctx* ctx, const char* name, int depth) {
    const int parent_depth = depth - 1;

    if (ctx->sub_depth >= 0 && parent_depth == ctx->sub_depth) {
        if (ctx->shape == RR_SHAPE_SITE_LIST) {
            rr_apply_field(&ctx->freq, k_site_freq_fields, sizeof(k_site_freq_fields) / sizeof(k_site_freq_fields[0]),
                           name, ctx->text);
        } else if (ctx->shape == RR_SHAPE_TRS_DETAILS) {
            rr_apply_field(&ctx->sysid, k_sysid_fields, sizeof(k_sysid_fields) / sizeof(k_sysid_fields[0]), name,
                           ctx->text);
        }
        return;
    }

    if (ctx->record_depth >= 0 && parent_depth == ctx->record_depth) {
        if (ctx->shape == RR_SHAPE_SUPPORT_TYPE || ctx->shape == RR_SHAPE_SUPPORT_FLAVOR
            || ctx->shape == RR_SHAPE_SUPPORT_VOICE) {
            rr_apply_support_leaf(ctx, name);
            return;
        }
        const rr_field_def* table = NULL;
        size_t n = 0;
        void* record = rr_record_table(ctx, &table, &n);
        if (record != NULL && table != NULL) {
            rr_apply_field(record, table, n, name, ctx->text);
        }
        return;
    }

    if (ctx->return_depth >= 0 && parent_depth == ctx->return_depth) {
        rr_apply_root_leaf(ctx, name);
    }
}

/**
 * @brief Append the finished scratch record to its sink list.
 *
 * @param ctx Parse context.
 * @return 0 on success, -1 on allocation failure.
 */
static int
rr_commit_record(rr_parse_ctx* ctx) {
    switch (ctx->shape) {
        case RR_SHAPE_COUNTRY_LIST: {
            dsd_rr_country_list* list = (dsd_rr_country_list*)ctx->sink;
            return rr_list_append(ctx, (void**)&list->items, &list->count, &ctx->country, sizeof(ctx->country));
        }
        case RR_SHAPE_STATE_LIST: {
            dsd_rr_state_list* list = (dsd_rr_state_list*)ctx->sink;
            return rr_list_append(ctx, (void**)&list->items, &list->count, &ctx->state, sizeof(ctx->state));
        }
        case RR_SHAPE_COUNTY_LIST: {
            dsd_rr_county_list* list = (dsd_rr_county_list*)ctx->sink;
            return rr_list_append(ctx, (void**)&list->items, &list->count, &ctx->county, sizeof(ctx->county));
        }
        case RR_SHAPE_TRS_LIST: {
            dsd_rr_trs_list* list = (dsd_rr_trs_list*)ctx->sink;
            return rr_list_append(ctx, (void**)&list->items, &list->count, &ctx->trs_summary, sizeof(ctx->trs_summary));
        }
        case RR_SHAPE_SITE_LIST: {
            dsd_rr_site_list* list = (dsd_rr_site_list*)ctx->sink;
            return rr_list_append(ctx, (void**)&list->items, &list->count, &ctx->site, sizeof(ctx->site));
        }
        case RR_SHAPE_TALKGROUP_LIST: {
            dsd_rr_talkgroup_list* list = (dsd_rr_talkgroup_list*)ctx->sink;
            return rr_list_append(ctx, (void**)&list->items, &list->count, &ctx->talkgroup, sizeof(ctx->talkgroup));
        }
        case RR_SHAPE_TALKGROUP_CAT_LIST: {
            dsd_rr_talkgroup_cat_list* list = (dsd_rr_talkgroup_cat_list*)ctx->sink;
            return rr_list_append(ctx, (void**)&list->items, &list->count, &ctx->talkgroup_cat,
                                  sizeof(ctx->talkgroup_cat));
        }
        case RR_SHAPE_SUPPORT_TYPE:
        case RR_SHAPE_SUPPORT_FLAVOR:
        case RR_SHAPE_SUPPORT_VOICE: {
            dsd_rr_support_list* list = (dsd_rr_support_list*)ctx->sink;
            return rr_list_append(ctx, (void**)&list->items, &list->count, &ctx->support, sizeof(ctx->support));
        }
        default: return 0;
    }
}

/**
 * @brief Append the finished sub-record to the record that contains it.
 *
 * @param ctx Parse context.
 * @return 0 on success, -1 on allocation failure.
 */
static int
rr_commit_sub(rr_parse_ctx* ctx) {
    if (ctx->shape == RR_SHAPE_SITE_LIST) {
        /* 'd' is the primary control channel, 'a' an alternate; anything else is voice. */
        ctx->freq.is_control = (strcmp(ctx->freq.use, "d") == 0) ? 1 : 0;
        ctx->freq.is_alt_control = (strcmp(ctx->freq.use, "a") == 0) ? 1 : 0;

        if (rr_array_reserve((void**)&ctx->site.freqs, &ctx->site_freq_cap, ctx->site.freq_count + 1U,
                             sizeof(dsd_rr_site_freq))
            != 0) {
            return -1;
        }
        ctx->site.freqs[ctx->site.freq_count] = ctx->freq;
        ctx->site.freq_count++;
        return 0;
    }

    if (ctx->shape == RR_SHAPE_TRS_DETAILS) {
        dsd_rr_trs_details* details = (dsd_rr_trs_details*)ctx->sink;
        if (rr_array_reserve((void**)&details->sysids, &ctx->sysid_cap, details->sysid_count + 1U,
                             sizeof(dsd_rr_trs_sysid))
            != 0) {
            return -1;
        }
        details->sysids[details->sysid_count] = ctx->sysid;
        details->sysid_count++;
        return 0;
    }

    return 0;
}

/**
 * @brief Count a bandplan entry against the record it belongs to.
 *
 * @param ctx Parse context.
 */
static void
rr_count_bandplan(rr_parse_ctx* ctx) {
    if (ctx->shape == RR_SHAPE_SITE_LIST) {
        ctx->site.bandplan_count++;
    } else if (ctx->shape == RR_SHAPE_TRS_DETAILS) {
        ((dsd_rr_trs_details*)ctx->sink)->bandplan_count++;
    }
}

/**
 * @brief Reject the multiRef/href encoding NuSOAP never emits.
 *
 * Scoped to an href attribute on an element, never a substring scan of the body:
 * a talkgroup description containing the literal text "href=" must not abort a
 * perfectly valid response.
 *
 * @param ctx  Parse context.
 * @param atts Expat's NULL-terminated name/value attribute array.
 * @return 1 when an href attribute was seen, 0 otherwise.
 */
static int
rr_atts_have_href(rr_parse_ctx* ctx, const XML_Char** atts) {
    if (atts == NULL || ctx->body_depth < 0) {
        return 0;
    }
    for (int i = 0; atts[i] != NULL && atts[i + 1] != NULL; i += 2) {
        if (strcmp(rr_local_name(atts[i]), "href") == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Whether the element carries xsi:nil="true".
 *
 * The prefix bound to the XMLSchema-instance namespace is server-chosen, so the
 * attribute name is compared by local part like every element name.
 *
 * @param atts Expat's NULL-terminated name/value attribute array.
 * @return 1 when nil, 0 otherwise.
 */
static int
rr_atts_are_nil(const XML_Char** atts) {
    if (atts == NULL) {
        return 0;
    }
    for (int i = 0; atts[i] != NULL && atts[i + 1] != NULL; i += 2) {
        if (strcmp(rr_local_name(atts[i]), "nil") != 0) {
            continue;
        }
        if (strcmp(atts[i + 1], "true") == 0 || strcmp(atts[i + 1], "1") == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Open a container when an `<item>` element starts.
 *
 * @param ctx    Parse context.
 * @param parent Local name of the enclosing element.
 */
static void
rr_open_item(rr_parse_ctx* ctx, const char* parent) {
    if (parent == NULL) {
        return;
    }

    if (strcmp(parent, "bandplan") == 0) {
        rr_count_bandplan(ctx);
        return;
    }

    const char* sub_parent = rr_sub_parent(ctx->shape);
    if (sub_parent != NULL && strcmp(parent, sub_parent) == 0) {
        ctx->sub_depth = ctx->depth;
        rr_reset_sub(ctx);
        return;
    }

    const char* record_parent = rr_record_parent(ctx->shape);
    if (record_parent != NULL && ctx->record_depth < 0 && strcmp(parent, record_parent) == 0) {
        ctx->record_depth = ctx->depth;
        rr_reset_record(ctx);
    }
}

static void XMLCALL
rr_start_element(void* user, const XML_Char* name, const XML_Char** atts) {
    rr_parse_ctx* ctx = (rr_parse_ctx*)user;
    if (ctx->failed) {
        return;
    }
    if (ctx->depth >= RR_MAX_DEPTH) {
        rr_fail(ctx, DSD_RR_ERR_PARSE, "element nesting too deep");
        XML_StopParser(ctx->parser, XML_FALSE);
        return;
    }

    const char* local = rr_local_name(name);
    if (rr_atts_have_href(ctx, atts)) {
        rr_fail(ctx, DSD_RR_ERR_PARSE, "unsupported multiRef/href response encoding");
        XML_StopParser(ctx->parser, XML_FALSE);
        return;
    }

    ctx->leaf_nil = rr_atts_are_nil(atts);
    rr_copy_field(ctx->names[ctx->depth], RR_MAX_NAME, local);
    ctx->depth++;
    ctx->text_len = 0;
    ctx->text[0] = '\0';

    if (strcmp(local, "Body") == 0 && ctx->body_depth < 0) {
        ctx->body_depth = ctx->depth;
        return;
    }
    if (strcmp(local, "Fault") == 0) {
        ctx->in_fault = 1;
        ctx->fault_depth = ctx->depth;
        return;
    }
    if (ctx->in_fault) {
        return;
    }
    if (strcmp(local, "return") == 0 && ctx->return_depth < 0) {
        ctx->return_depth = ctx->depth;
        return;
    }
    if (strcmp(local, "item") == 0) {
        rr_open_item(ctx, (ctx->depth >= 2) ? ctx->names[ctx->depth - 2] : NULL);
    }
}

static void XMLCALL
rr_end_element(void* user, const XML_Char* name) {
    rr_parse_ctx* ctx = (rr_parse_ctx*)user;
    const int depth = ctx->depth;
    ctx->depth--;
    if (ctx->failed) {
        return;
    }

    const char* local = rr_local_name(name);

    if (ctx->in_fault) {
        if (depth - 1 == ctx->fault_depth && !ctx->leaf_nil) {
            if (strcmp(local, "faultcode") == 0) {
                rr_copy_field(ctx->fault_code, sizeof(ctx->fault_code), ctx->text);
            } else if (strcmp(local, "faultstring") == 0) {
                rr_copy_field(ctx->fault_string, sizeof(ctx->fault_string), ctx->text);
            }
        }
        ctx->text_len = 0;
        ctx->text[0] = '\0';
        return;
    }

    if (ctx->sub_depth >= 0 && depth == ctx->sub_depth) {
        if (rr_commit_sub(ctx) != 0) {
            rr_fail(ctx, DSD_RR_ERR_NOMEM, "out of memory decoding response");
            XML_StopParser(ctx->parser, XML_FALSE);
        }
        ctx->sub_depth = -1;
    } else if (ctx->record_depth >= 0 && depth == ctx->record_depth) {
        if (rr_commit_record(ctx) != 0) {
            rr_fail(ctx, DSD_RR_ERR_NOMEM, "out of memory decoding response");
            XML_StopParser(ctx->parser, XML_FALSE);
        }
        ctx->record_depth = -1;
    } else if (!ctx->leaf_nil) {
        /* xsi:nil means absent: leave the initialized value alone. */
        rr_apply_leaf(ctx, local, depth);
    }

    ctx->text_len = 0;
    ctx->text[0] = '\0';
}

static void XMLCALL
rr_char_data(void* user, const XML_Char* s, int len) {
    rr_parse_ctx* ctx = (rr_parse_ctx*)user;
    if (ctx->failed || s == NULL || len <= 0) {
        return;
    }
    if (ctx->text_len + 1U >= RR_LEAF_MAX) {
        return;
    }

    /* Character data arrives in several callbacks; always append, never assign. */
    size_t n = (size_t)len;
    const size_t room = RR_LEAF_MAX - 1U - ctx->text_len;
    if (n > room) {
        n = room;
    }
    DSD_MEMCPY(ctx->text + ctx->text_len, s, n);
    ctx->text_len += n;
    ctx->text[ctx->text_len] = '\0';
}

static void XMLCALL
rr_doctype_handler(void* user, const XML_Char* doctype, const XML_Char* sysid, const XML_Char* pubid, int subset) {
    (void)doctype;
    (void)sysid;
    (void)pubid;
    (void)subset;
    rr_parse_ctx* ctx = (rr_parse_ctx*)user;
    rr_fail(ctx, DSD_RR_ERR_PARSE, "DOCTYPE declaration rejected");
    XML_StopParser(ctx->parser, XML_FALSE);
}

static void XMLCALL
rr_entity_decl_handler(void* user, const XML_Char* name, int is_param, const XML_Char* value, int value_len,
                       const XML_Char* base, const XML_Char* system_id, const XML_Char* public_id,
                       const XML_Char* notation) {
    (void)name;
    (void)is_param;
    (void)value;
    (void)value_len;
    (void)base;
    (void)system_id;
    (void)public_id;
    (void)notation;
    rr_parse_ctx* ctx = (rr_parse_ctx*)user;
    rr_fail(ctx, DSD_RR_ERR_PARSE, "entity declaration rejected");
    XML_StopParser(ctx->parser, XML_FALSE);
}

/**
 * @brief Copy the enclosing StateInfo identity down onto every county row.
 *
 * @param ctx Parse context.
 */
static void
rr_finish_county_list(rr_parse_ctx* ctx) {
    dsd_rr_county_list* list = (dsd_rr_county_list*)ctx->sink;
    for (size_t i = 0; i < list->count; i++) {
        list->items[i].stid = ctx->enclosing_stid;
        rr_copy_field(list->items[i].state_name, sizeof(list->items[i].state_name), ctx->enclosing_state_name);
    }
}

/**
 * @brief Configure an expat parser with the handlers and the hardening.
 *
 * @param parser Parser to configure.
 * @param ctx    User data.
 */
static void
rr_install_handlers(XML_Parser parser, rr_parse_ctx* ctx) {
    XML_SetUserData(parser, ctx);
    XML_SetElementHandler(parser, rr_start_element, rr_end_element);
    XML_SetCharacterDataHandler(parser, rr_char_data);
    XML_SetStartDoctypeDeclHandler(parser, rr_doctype_handler);
    XML_SetEntityDeclHandler(parser, rr_entity_decl_handler);
    XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_NEVER);
}

int
rr_soap_parse(const char* body, size_t len, rr_shape shape, void* sink, dsd_rr_error* err) {
    if (body == NULL || sink == NULL || err == NULL) {
        return -1;
    }
    if (len == 0U || len > (size_t)INT_MAX) {
        err->status = DSD_RR_ERR_PARSE;
        rr_copy_field(err->detail, sizeof(err->detail), "empty or oversized response body");
        return -1;
    }

    rr_parse_ctx* ctx = (rr_parse_ctx*)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        err->status = DSD_RR_ERR_NOMEM;
        rr_copy_field(err->detail, sizeof(err->detail), "out of memory");
        return -1;
    }
    ctx->shape = shape;
    ctx->sink = sink;
    ctx->return_depth = -1;
    ctx->record_depth = -1;
    ctx->sub_depth = -1;
    ctx->fault_depth = -1;
    ctx->body_depth = -1;

    /*
     * NULL, never an explicit encoding: RR declares utf-8 on successful responses
     * and ISO-8859-1 on faults, while the HTTP header always claims utf-8. Passing
     * an encoding here would override the prolog and mangle non-ASCII text.
     */
    XML_Parser parser = XML_ParserCreate(NULL);
    if (parser == NULL) {
        free(ctx);
        err->status = DSD_RR_ERR_NOMEM;
        rr_copy_field(err->detail, sizeof(err->detail), "out of memory");
        return -1;
    }
    ctx->parser = parser;
    rr_install_handlers(parser, ctx);

    const int ok = XML_Parse(parser, body, (int)len, XML_TRUE);
    int rc = 0;

    if (ctx->failed) {
        err->status = ctx->fail_status;
        rr_copy_field(err->detail, sizeof(err->detail), ctx->fail_detail);
        rc = -1;
    } else if (ctx->in_fault) {
        /* Classify on faultcode; faultstring is English prose RR may reword. */
        err->status = (strcmp(ctx->fault_code, "AUTH") == 0) ? DSD_RR_ERR_AUTH : DSD_RR_ERR_SOAP_FAULT;
        rr_copy_field(err->detail, sizeof(err->detail), ctx->fault_string);
        rc = -1;
    } else if (ok == XML_STATUS_ERROR) {
        err->status = DSD_RR_ERR_PARSE;
        rr_copy_field(err->detail, sizeof(err->detail), XML_ErrorString(XML_GetErrorCode(parser)));
        rc = -1;
    } else if (ctx->return_depth < 0) {
        err->status = DSD_RR_ERR_PARSE;
        rr_copy_field(err->detail, sizeof(err->detail), "response contains no result element");
        rc = -1;
    } else if (shape == RR_SHAPE_COUNTY_LIST) {
        rr_finish_county_list(ctx);
    }

    XML_ParserFree(parser);
    free(ctx);
    return rc;
}

#else /* !USE_EXPAT */

int
rr_soap_parse(const char* body, size_t len, rr_shape shape, void* sink, dsd_rr_error* err) {
    (void)body;
    (void)len;
    (void)shape;
    (void)sink;
    if (err == NULL) {
        return -1;
    }
    err->status = DSD_RR_ERR_UNSUPPORTED;
    rr_copy_field(err->detail, sizeof(err->detail), "built without expat: XML parsing unavailable");
    return -1;
}

#endif /* USE_EXPAT */
