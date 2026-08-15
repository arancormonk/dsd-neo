// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * SOAP envelope construction and response decoding for the RadioReference API.
 * Module-private: include as "rr_soap.h" from siblings in this directory.
 */

#ifndef DSD_NEO_SRC_RUNTIME_RADIOREFERENCE_RR_SOAP_H
#define DSD_NEO_SRC_RUNTIME_RADIOREFERENCE_RR_SOAP_H

#include <dsd-neo/runtime/radioreference.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RR_PARAM_INT, RR_PARAM_STRING } rr_param_kind;

/** One `<name xsi:type="...">value</name>` part of a request message. */
typedef struct {
    const char* name;
    rr_param_kind kind;
    long ivalue;
    const char* svalue;
} rr_soap_param;

/**
 * Which response body to decode. The parser watches for a SOAP Fault in every
 * shape, so a fault is classified no matter which call produced it.
 */
typedef enum {
    RR_SHAPE_USER_INFO,          /**< sink: dsd_rr_user_info */
    RR_SHAPE_ZIP_INFO,           /**< sink: dsd_rr_zip_info */
    RR_SHAPE_COUNTRY_LIST,       /**< sink: dsd_rr_country_list */
    RR_SHAPE_STATE_LIST,         /**< getCountryInfo.stateList; sink: dsd_rr_state_list */
    RR_SHAPE_COUNTY_LIST,        /**< getStateInfo.countyList; sink: dsd_rr_county_list */
    RR_SHAPE_TRS_LIST,           /**< getStateInfo/getCountyInfo trsList; sink: dsd_rr_trs_list */
    RR_SHAPE_TRS_DETAILS,        /**< sink: dsd_rr_trs_details */
    RR_SHAPE_SITE_LIST,          /**< sink: dsd_rr_site_list */
    RR_SHAPE_TALKGROUP_LIST,     /**< sink: dsd_rr_talkgroup_list */
    RR_SHAPE_TALKGROUP_CAT_LIST, /**< sink: dsd_rr_talkgroup_cat_list */
    RR_SHAPE_SUPPORT_TYPE,       /**< sink: dsd_rr_support_list */
    RR_SHAPE_SUPPORT_FLAVOR,     /**< sink: dsd_rr_support_list */
    RR_SHAPE_SUPPORT_VOICE       /**< sink: dsd_rr_support_list */
} rr_shape;

/**
 * @brief XML-escape `& < > " '` into a caller-sized buffer.
 *
 * @param in     Source text.
 * @param out    Destination buffer.
 * @param out_sz Destination size in bytes, passed explicitly.
 * @return 0 on success, -1 on invalid argument or overflow (out is emptied).
 */
int rr_xml_escape(const char* in, char* out, size_t out_sz);

/**
 * @brief Build a complete rpc/encoded SOAP request envelope.
 *
 * Emits every part it is given, in order, followed by authInfo when `auth` is
 * non-NULL. Never drop a declared part: this endpoint answers a missing part with
 * an empty-bodied HTTP 500, which is why the whole-system talkgroup query sends
 * explicit zero filters rather than omitting them. getCountryList is the one
 * message with no parts at all, including no authInfo.
 *
 * @param method  Method name, e.g. "getTrsSites".
 * @param params  Parts to emit, or NULL when there are none.
 * @param n       Number of parts.
 * @param auth    Credentials, or NULL to omit authInfo entirely.
 * @param out     Receives the heap envelope; caller frees.
 * @param out_len Receives the envelope length in bytes.
 * @return 0 on success, -1 on invalid argument or allocation failure.
 */
int rr_soap_build_request(const char* method, const rr_soap_param* params, size_t n, const dsd_rr_auth* auth,
                          char** out, size_t* out_len);

/**
 * Why a parse ended the way it did.
 *
 * The caller needs RR_PARSE_NO_RESULT distinguished from RR_PARSE_MALFORMED so a
 * proxy's HTML error page on a 502 is reported as an HTTP failure rather than as
 * a parser bug. Deriving that from err->detail text would be fragile.
 */
typedef enum {
    RR_PARSE_OK,        /**< A result element was decoded. */
    RR_PARSE_FAULT,     /**< A SOAP fault was decoded and classified. */
    RR_PARSE_NO_RESULT, /**< Well-formed, but carries neither a fault nor a result. */
    RR_PARSE_MALFORMED  /**< Not usable XML, or rejected by the hardening rules. */
} rr_parse_outcome;

/**
 * @brief Decode a SOAP response body into `sink`.
 *
 * Matches local element names only, treats xsi:nil as absent rather than empty,
 * ignores unknown elements for forward compatibility, and rejects DOCTYPE,
 * entity declarations and href attributes outright.
 *
 * @param body    Response bytes.
 * @param len     Response length.
 * @param shape   Which sink type `sink` points at.
 * @param sink    Destination struct, zeroed by the caller.
 * @param err     Receives failure detail, including a classified SOAP fault.
 * @param outcome Optional; receives why the parse ended as it did.
 * @return 0 on success, -1 on failure with err->status set.
 */
int rr_soap_parse(const char* body, size_t len, rr_shape shape, void* sink, dsd_rr_error* err,
                  rr_parse_outcome* outcome);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_RUNTIME_RADIOREFERENCE_RR_SOAP_H */
