// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RadioReference.com database client: types, SOAP transport and parsing.
 *
 * UI-agnostic C API. Qt is the first consumer; the terminal UI can adopt the same
 * entry points later.
 *
 * Three constraints shape this header and must survive edits:
 *
 * 1. Every entry point is declared unconditionally. Availability is a runtime
 *    answer from dsd_rr_available(), never an #ifdef, because the public-header
 *    smoke test compiles this file standalone with no USE_* defines, and because
 *    the Android JNI sources link dsd-neo_ui_qt PRIVATE and therefore compile
 *    without USE_CURL/USE_EXPAT. A conditional member or declaration would be an ODR
 *    violation that links cleanly and crashes at runtime.
 * 2. It includes nothing but <stddef.h>, <stdint.h> and dsd-neo headers.
 * 3. It names no curl, expat or Qt type; dsd_rr_client is opaque.
 *
 * Credential policy: no username, password or application key may ever reach a
 * log, a status string, an error detail or test output. dsd_rr_error::detail is
 * sanitized server text only, and never echoes the request body - the SOAP
 * envelope carries the password in cleartext.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_H_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Status and errors                                                          */
/* ------------------------------------------------------------------------- */

typedef enum {
    DSD_RR_OK = 0,
    DSD_RR_ERR_UNSUPPORTED,  /**< Built without curl and/or expat. */
    DSD_RR_ERR_INVALID_ARG,  /**< Caller passed NULL or an out-of-range value. */
    DSD_RR_ERR_NOMEM,        /**< Allocation failed. */
    DSD_RR_ERR_NETWORK,      /**< Transport-level failure; the only retried class. */
    DSD_RR_ERR_HTTP,         /**< Non-2xx with a body that is neither a fault nor a result. */
    DSD_RR_ERR_PARSE,        /**< Malformed, hostile or oversized XML. */
    DSD_RR_ERR_SOAP_FAULT,   /**< SOAP fault with a code we do not classify further. */
    DSD_RR_ERR_AUTH,         /**< SOAP fault with faultcode AUTH. */
    DSD_RR_ERR_SUBSCRIPTION, /**< Premium subscription expired (from getUserData). */
    DSD_RR_ERR_CANCELLED     /**< Caller cancelled the request. */
} dsd_rr_status;

/**
 * @brief Failure detail for a request.
 *
 * `detail` holds a sanitized faultstring or curl error message for display. It
 * MUST NEVER contain credentials and MUST NEVER echo the request body.
 */
typedef struct {
    dsd_rr_status status;
    long http_status;
    char detail[256];
} dsd_rr_error;

/**
 * @brief Per-request credentials. Never copy by value; pass as const pointer.
 */
typedef struct {
    char username[128];
    char password[128];
    char app_key[64];
} dsd_rr_auth;

/* ------------------------------------------------------------------------- */
/* Geography                                                                  */
/* ------------------------------------------------------------------------- */

/** getZipcodeInfo -> ZipInfo: a single struct of IDs, not a county list. */
typedef struct {
    int zip_code;
    int stid;
    int ctid;
    char city[96];
} dsd_rr_zip_info;

typedef struct {
    int coid;
    char name[96];
    char code[8];
} dsd_rr_country;

typedef struct {
    dsd_rr_country* items;
    size_t count;
} dsd_rr_country_list;

typedef struct {
    int stid;
    char name[64];
    char code[8];
} dsd_rr_state;

typedef struct {
    dsd_rr_state* items;
    size_t count;
} dsd_rr_state_list;

/**
 * getStateInfo.countyList rows carry only {ctid, countyName}; stid/state_name are
 * copied down by the parser from the enclosing StateInfo. There is no state code.
 */
typedef struct {
    int ctid;
    char county_name[96];
    int stid;
    char state_name[64];
} dsd_rr_county;

typedef struct {
    dsd_rr_county* items;
    size_t count;
} dsd_rr_county_list;

/* ------------------------------------------------------------------------- */
/* Trunked systems                                                            */
/* ------------------------------------------------------------------------- */

/** TrsListDef: type/flavor/voice are numeric IDs, and there is no county member. */
typedef struct {
    int sid;
    char name[128];
    int type_id;
    int flavor_id;
    int voice_id;
    char city[96];
} dsd_rr_trs_summary;

typedef struct {
    dsd_rr_trs_summary* items;
    size_t count;
} dsd_rr_trs_list;

/** getTrsType/Flavor/Voice rows all fit one shape. For types, stype == id. */
typedef struct {
    int stype;
    int id;
    char descr[96];
} dsd_rr_support_entry;

typedef struct {
    dsd_rr_support_entry* items;
    size_t count;
} dsd_rr_support_list;

typedef struct {
    dsd_rr_support_list types;
    dsd_rr_support_list flavors;
    dsd_rr_support_list voices;
} dsd_rr_support_maps;

/** Trs.sysid is an ARRAY of string tuples, and members are individually optional. */
typedef struct {
    char sysid[16];
    char wacn[16];
    char ct[32];
    char model[32];
} dsd_rr_trs_sysid;

/** has_custom_bandplan is derived: (bandplan_count > 0). Trs carries no sid. */
typedef struct {
    char name[128];
    int type_id;
    int flavor_id;
    int voice_id;
    char city[96];
    dsd_rr_trs_sysid* sysids;
    size_t sysid_count;
    int bandplan_count;
} dsd_rr_trs_details;

typedef struct {
    long long freq_hz;
    int lcn;            /**< -1 when absent or nil. */
    char ch_id[16];     /**< NXDN Channel ID / DMR channel number text; "" when absent. */
    char color_code[8]; /**< DMR colour code, text; "" when absent. */
    int is_control;     /**< use == "d" */
    int is_alt_control; /**< use == "a" */
    char use[8];
} dsd_rr_site_freq;

typedef struct {
    int site_db_id;  /**< TrsSite.siteId - database row id, NOT the RF site. */
    int site_number; /**< TrsSite.siteNumber - the RF site. Display only: it repeats within a system. */
    char descr[128];
    int zone_number;
    char zone_descr[64];
    int rfss;
    char nac[16]; /**< xsd:string, hex text. */
    int ran;
    char modulation[32]; /**< Free text, e.g. "CQPSK Phase 1", "TDMA"; often absent. */
    int splinter;
    int rebanded;
    int tdma_cc;
    int bandplan_count;
    dsd_rr_site_freq* freqs;
    size_t freq_count;
} dsd_rr_site;

typedef struct {
    dsd_rr_site* items;
    size_t count;
} dsd_rr_site_list;

typedef struct {
    uint32_t tg_dec;
    int tg_cid;
    char alpha_tag[64];
    char description[160];
    char mode[8];
    int enc;           /**< 0 clear, 1 partial, 2 full. */
    char slot[8];      /**< tgSlot is xsd:string; parse where a number is needed. */
    char category[96]; /**< Resolved from getTrsTalkgroupCats via tg_cid; "" until then. */
} dsd_rr_talkgroup;

typedef struct {
    dsd_rr_talkgroup* items;
    size_t count;
} dsd_rr_talkgroup_list;

typedef struct {
    int tg_cid;
    char name[96];
} dsd_rr_talkgroup_cat;

typedef struct {
    dsd_rr_talkgroup_cat* items;
    size_t count;
} dsd_rr_talkgroup_cat_list;

typedef struct {
    char username[128];
    char sub_expire[64];
} dsd_rr_user_info;

/** Bytes in a warning, terminator included. Sized for the longest message a
 *  generator emits — the 241-byte P25 no-channel-identifiers explanation —
 *  since a bound that trims a real sentence mid-word reads as a rendering bug,
 *  not a bound. Generators that build a warning with a format string must stage
 *  it in a buffer of this size, or their own local would reimpose the older,
 *  smaller bound before the text ever reaches a slot. */
#define DSD_RR_WARNING_TEXT_MAX 256

/** Warnings surfaced in the import preview. One shape for every generator. */
typedef struct {
    char text[DSD_RR_WARNING_TEXT_MAX];
} dsd_rr_warning;

typedef struct {
    dsd_rr_warning* items;
    size_t count;
} dsd_rr_warning_list;

/* ------------------------------------------------------------------------- */
/* Ownership                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Every *_list is freed by its own free function, which is idempotent, safe on a
 * zeroed struct, and leaves the struct zeroed. The single-value structs
 * (dsd_rr_zip_info, dsd_rr_user_info) own nothing.
 */
void dsd_rr_country_list_free(dsd_rr_country_list* list);
void dsd_rr_state_list_free(dsd_rr_state_list* list);
void dsd_rr_county_list_free(dsd_rr_county_list* list);
void dsd_rr_trs_list_free(dsd_rr_trs_list* list);
void dsd_rr_support_list_free(dsd_rr_support_list* list);
void dsd_rr_support_maps_free(dsd_rr_support_maps* maps);
void dsd_rr_site_list_free(dsd_rr_site_list* list);
void dsd_rr_talkgroup_list_free(dsd_rr_talkgroup_list* list);
void dsd_rr_talkgroup_cat_list_free(dsd_rr_talkgroup_cat_list* list);
void dsd_rr_warning_list_free(dsd_rr_warning_list* list);
void dsd_rr_trs_details_free(dsd_rr_trs_details* details);

/**
 * @brief Append a formatted warning to a warning list.
 *
 * @param list Destination list, grown as needed.
 * @param text Warning text; truncated to fit dsd_rr_warning::text.
 * @return 0 on success, -1 on allocation failure or invalid argument.
 */
int dsd_rr_warning_list_add(dsd_rr_warning_list* list, const char* text);

/**
 * @brief Look up a support-list description.
 *
 * Flavor and voice IDs are namespaced by system type, so the lookup is keyed on
 * the (stype, id) pair. For the type list, stype == id.
 *
 * @param list  Support list to search.
 * @param stype System type ID.
 * @param id    Type, flavor or voice ID.
 * @return The description, or "" when absent. Never NULL.
 */
const char* dsd_rr_support_lookup(const dsd_rr_support_list* list, int stype, int id);

/**
 * @brief Convert MHz decimal text to exact integer Hz, without floating point.
 *
 * Accepts an optional fractional part of 1..6 digits, and trailing zeros beyond
 * six only when they are all '0'. Rejects a leading sign, an empty or non-digit
 * input, and 7+ significant fractional digits rather than rounding silently.
 *
 * @param mhz_text Decimal MHz text, e.g. "851.0125".
 * @param out_hz   Receives the exact frequency in Hz.
 * @return 0 on success, -1 on invalid input.
 */
int dsd_rr_mhz_to_hz(const char* mhz_text, long long* out_hz);

/* ------------------------------------------------------------------------- */
/* Client (implemented in Stage 4)                                            */
/* ------------------------------------------------------------------------- */

typedef struct dsd_rr_client dsd_rr_client;

typedef struct {
    char endpoint_url[256]; /**< Default "https://api.radioreference.com/soap2/". */
    int connect_timeout_ms; /**< Default 10000. */
    int total_timeout_ms;   /**< Default 30000. */
    int transient_retries;  /**< Default 1: retry NETWORK-class errors once after 500 ms. */
} dsd_rr_client_config;

/**
 * @brief Whether the BUILT-IN transport and parser are both available.
 *
 * This reports USE_CURL && USE_EXPAT. It is explicitly NOT a precondition for
 * dsd_rr_client_create() or for any getter: with an injected transport the client
 * is fully functional while this returns 0, as long as expat is present (without
 * expat the parser returns DSD_RR_ERR_UNSUPPORTED regardless of transport).
 * Treat it as a UI gate on the shipped transport, not a functional gate.
 *
 * @return 1 when the built-in path is usable, 0 otherwise.
 */
int dsd_rr_available(void);

/**
 * @brief Application key baked in at build time.
 *
 * Defined by a configure_file-generated translation unit; returns "" when no key
 * was supplied at configure time, in which case the UI must prompt for one. The
 * baked key is extractable from any shipped binary with `strings`; it is not a
 * secret, it merely keeps the value out of the repository.
 *
 * @return The key, or "" when none was baked in. Never NULL.
 */
const char* dsd_rr_builtin_app_key(void);

/**
 * @brief Create a client. Spawns one worker thread for the async API.
 *
 * @param config Configuration, or NULL for defaults.
 * @return New client, or NULL on failure.
 */
dsd_rr_client* dsd_rr_client_create(const dsd_rr_client_config* config);

/**
 * @brief Cancel everything, join the worker, and destroy the client.
 *
 * An in-flight transfer unwinds on the next libcurl progress tick, which fires at
 * least once a second, so this can block for roughly a second.
 *
 * @param client Client to destroy; NULL is a no-op.
 */
void dsd_rr_client_destroy(dsd_rr_client* client);

/* Blocking getters. Each returns 0 on success, -1 on failure with *err filled. */
int dsd_rr_get_user_data(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_user_info* out, dsd_rr_error* err);
int dsd_rr_get_zipcode_info(dsd_rr_client* client, const dsd_rr_auth* auth, const char* zip, dsd_rr_zip_info* out,
                            dsd_rr_error* err);
int dsd_rr_get_countries(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_country_list* out, dsd_rr_error* err);
int dsd_rr_get_country_states(dsd_rr_client* client, const dsd_rr_auth* auth, int coid, dsd_rr_state_list* out,
                              dsd_rr_error* err);
int dsd_rr_get_state_counties(dsd_rr_client* client, const dsd_rr_auth* auth, int stid, dsd_rr_county_list* out,
                              dsd_rr_error* err);
int dsd_rr_get_state_trs(dsd_rr_client* client, const dsd_rr_auth* auth, int stid, dsd_rr_trs_list* out,
                         dsd_rr_error* err);
int dsd_rr_get_county_trs(dsd_rr_client* client, const dsd_rr_auth* auth, int ctid, dsd_rr_trs_list* out,
                          dsd_rr_error* err);
int dsd_rr_get_trs_details(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_trs_details* out,
                           dsd_rr_error* err);
int dsd_rr_get_trs_sites(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_site_list* out,
                         dsd_rr_error* err);
int dsd_rr_get_trs_talkgroups(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_talkgroup_list* out,
                              dsd_rr_error* err);
int dsd_rr_get_trs_talkgroup_cats(dsd_rr_client* client, const dsd_rr_auth* auth, int sid,
                                  dsd_rr_talkgroup_cat_list* out, dsd_rr_error* err);

/**
 * @brief Fetch and cache the type/flavor/voice description tables.
 *
 * Calls getTrsType/getTrsFlavor/getTrsVoice with id=0, which is what returns every
 * row - omitting the part is an empty-bodied HTTP 500. Cached per client instance
 * after the first success: RR adds rows over time, so never bake a static table.
 *
 * @param client Client.
 * @param auth   Credentials.
 * @param out    Receives a BORROWED view of the client's cache. Do not free it and
 *               do not use it after dsd_rr_client_destroy().
 * @param err    Receives failure detail.
 * @return 0 on success, -1 on failure.
 */
int dsd_rr_get_support_maps(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_support_maps* out,
                            dsd_rr_error* err);

/**
 * @brief Async completion callback.
 *
 * FIRES ON THE CLIENT'S WORKER THREAD. A GUI consumer must marshal to its own
 * thread before touching UI state. `result` is the matching heap list or struct
 * and becomes the callee's to free.
 */
typedef void (*dsd_rr_done_cb)(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result);

uint64_t dsd_rr_fetch_user_data(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_done_cb cb, void* user);
uint64_t dsd_rr_fetch_zipcode_info(dsd_rr_client* client, const dsd_rr_auth* auth, const char* zip, dsd_rr_done_cb cb,
                                   void* user);
uint64_t dsd_rr_fetch_countries(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_done_cb cb, void* user);
uint64_t dsd_rr_fetch_country_states(dsd_rr_client* client, const dsd_rr_auth* auth, int coid, dsd_rr_done_cb cb,
                                     void* user);
uint64_t dsd_rr_fetch_state_counties(dsd_rr_client* client, const dsd_rr_auth* auth, int stid, dsd_rr_done_cb cb,
                                     void* user);
uint64_t dsd_rr_fetch_state_trs(dsd_rr_client* client, const dsd_rr_auth* auth, int stid, dsd_rr_done_cb cb,
                                void* user);
uint64_t dsd_rr_fetch_county_trs(dsd_rr_client* client, const dsd_rr_auth* auth, int ctid, dsd_rr_done_cb cb,
                                 void* user);
uint64_t dsd_rr_fetch_trs_details(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_done_cb cb,
                                  void* user);
uint64_t dsd_rr_fetch_trs_sites(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_done_cb cb, void* user);
uint64_t dsd_rr_fetch_trs_talkgroups(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_done_cb cb,
                                     void* user);
uint64_t dsd_rr_fetch_trs_talkgroup_cats(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_done_cb cb,
                                         void* user);
/**
 * @brief Queue getTrsType.
 *
 * Named for the one table it fetches, not for dsd_rr_support_maps: the async
 * machinery runs one method per job, so `result` is a heap dsd_rr_support_list.
 * A caller that read the name as the async twin of dsd_rr_get_support_maps()
 * and cast it to dsd_rr_support_maps* would read past the allocation and then
 * free two wild pointers. The flavor and voice tables have no async form; the
 * blocking getter fetches all three.
 */
uint64_t dsd_rr_fetch_support_types(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_done_cb cb, void* user);

/**
 * @brief Best-effort cancellation of a queued or in-flight request.
 *
 * @param client     Client.
 * @param request_id ID returned by a dsd_rr_fetch_* call.
 * @return 0 when the request was found and flagged, -1 otherwise.
 */
int dsd_rr_cancel(dsd_rr_client* client, uint64_t request_id);

/**
 * @brief Opaque cancel token.
 *
 * Opaque so the atomics stay out of this header, which must compile standalone
 * and under C++. Named _token rather than dsd_rr_cancel because a typedef and a
 * function cannot share an identifier in C.
 */
typedef struct dsd_rr_cancel_token dsd_rr_cancel_token;

/**
 * @brief Whether cancellation has been requested. Safe from any thread.
 *
 * @param cancel Token, or NULL (which reads as "not cancelled").
 * @return 1 when cancelled, 0 otherwise.
 */
int dsd_rr_cancel_requested(const dsd_rr_cancel_token* cancel);

/** One HTTP request, as the transport sees it. */
typedef struct {
    const dsd_rr_client_config* config;
    const char* body;
    size_t body_len;
    const dsd_rr_cancel_token* cancel;
} dsd_rr_request;

/**
 * One HTTP response. `body` is owned by the transport's caller after perform().
 *
 * `status` carries the failure class when perform() returns -1. It is not
 * cosmetic: only DSD_RR_ERR_NETWORK is retried, DSD_RR_ERR_CANCELLED must never
 * be, and an oversized body must not trigger a second multi-megabyte download.
 */
typedef struct {
    char* body;
    size_t body_len;
    long http_status;
    dsd_rr_status status;
    char error[256];
} dsd_rr_response;

/**
 * @brief Pluggable HTTP transport, so the client is testable without a network.
 *
 * perform() returns 0 when the exchange completed (any HTTP status), -1 when it
 * did not, filling dsd_rr_response::error with a transport message.
 */
typedef struct {
    int (*perform)(void* ctx, const dsd_rr_request* req, dsd_rr_response* resp);
    void* ctx;
} dsd_rr_transport;

/**
 * @brief Replace the transport. Pass NULL to restore the built-in curl transport.
 *
 * @param client    Client.
 * @param transport Transport to install, or NULL.
 */
void dsd_rr_client_set_transport(dsd_rr_client* client, const dsd_rr_transport* transport);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RADIOREFERENCE_H_H */
