// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief P25 control channel candidate list helpers.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Control-channel candidates and neighbor list helpers for P25.
 * Provides small utilities to track announced neighbors, load a historical
 * read-only per-system candidate cache, and expire stale entries.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CC_CANDIDATES_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CC_CANDIDATES_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of neighbor table entries. */
#define P25_NB_MAX                            32
/** Maximum number of resolved current-site secondary control channels retained. */
#define P25_SECONDARY_CC_MAX                  16
/** Maximum unresolved P25 channel announcements retained until IDEN arrives. */
#define P25_PENDING_ANNOUNCEMENT_MAX          32

#define P25_PENDING_ANNOUNCEMENT_NEIGHBOR     1U
#define P25_PENDING_ANNOUNCEMENT_SECONDARY_CC 2U

/**
 * @brief Per-neighbor entry with site metadata.
 *
 * Carries CFVA status and site identity alongside the frequency so downstream
 * filtering and site scoping operate on one structured record.
 */
typedef struct {
    long freq;          /**< Frequency in Hz (0 = empty slot). */
    uint32_t wacn;      /**< Optional WACN (20-bit value, valid when wacn_valid=1). */
    uint16_t sysid;     /**< System ID (12-bit value stored in 16-bit field). */
    uint8_t rfss;       /**< RFSS ID (8-bit). */
    uint8_t site;       /**< Site ID (8-bit). */
    uint8_t cfva;       /**< CFVA status nibble (4-bit in 8-bit field). */
    uint8_t lra;        /**< Adjacent-site Location Registration Area. */
    uint8_t wacn_valid; /**< 1 when @ref wacn was decoded with the neighbor. */
    uint8_t lra_valid;  /**< 1 when @ref lra was decoded with the neighbor. */
    uint8_t cfva_valid; /**< 1 when @ref cfva was decoded with the neighbor. */
    time_t last_seen;   /**< Timestamp of last update. */
} p25_nb_entry_t;

/**
 * @brief Resolved current-site secondary control channel broadcast.
 */
typedef struct {
    long freq;        /**< Resolved downlink frequency in Hz (0 = empty slot). */
    uint16_t channel; /**< Raw P25 channel ID (IDEN + channel number). */
    uint8_t rfss;     /**< RFSS ID associated with announcement. */
    uint8_t site;     /**< Site ID associated with announcement. */
    uint8_t ssc;      /**< Secondary control channel system service class. */
    uint8_t reserved; /**< Reserved for alignment/future flags. */
    time_t last_seen; /**< Timestamp of last update. */
} p25_secondary_cc_entry_t;

/**
 * @brief Unresolved P25 channel announcement waiting for IDEN frequency data.
 */
typedef struct {
    uint8_t populated;  /**< 1 when this slot is active. */
    uint8_t kind;       /**< P25_PENDING_ANNOUNCEMENT_* value. */
    uint8_t rfss;       /**< RFSS ID associated with announcement. */
    uint8_t site;       /**< Site ID associated with announcement. */
    uint8_t cfva;       /**< Adjacent-site CFVA flags for neighbor records. */
    uint8_t lra;        /**< Adjacent-site Location Registration Area. */
    uint8_t wacn_valid; /**< 1 when @ref wacn is valid. */
    uint8_t lra_valid;  /**< 1 when @ref lra is valid. */
    uint8_t cfva_valid; /**< 1 when @ref cfva is valid. */
    uint8_t ssc;        /**< Secondary control channel system service class. */
    uint16_t sysid;     /**< System ID associated with neighbor records. */
    uint16_t channel;   /**< Raw P25 channel ID (IDEN + channel number). */
    uint32_t wacn;      /**< Optional WACN associated with neighbor records. */
    time_t last_seen;   /**< Last update time for dedupe/LRU eviction. */
} p25_pending_announcement_t;

/**
 * @brief Decoded adjacent-site channel announcement with explicit metadata validity.
 */
typedef struct {
    uint16_t channel;   /**< Raw P25 channel ID (IDEN + channel number). */
    uint32_t wacn;      /**< Optional WACN (20-bit value, valid when wacn_valid=1). */
    uint16_t sysid;     /**< System ID (12-bit, stored in 16-bit field). */
    uint8_t rfss;       /**< RFSS ID. */
    uint8_t site;       /**< Site ID. */
    uint8_t lra;        /**< Location Registration Area. */
    uint8_t cfva;       /**< Adjacent-status CFVA flags. */
    uint8_t wacn_valid; /**< 1 when @ref wacn was decoded with this announcement. */
    uint8_t lra_valid;  /**< 1 when @ref lra was decoded with this announcement. */
    uint8_t cfva_valid; /**< 1 when @ref cfva was decoded with this announcement. */
} p25_neighbor_channel_announcement_t;

/** A resolved neighbor frequency plus any validity-bearing announcement metadata. */
typedef struct {
    long freq;
    p25_neighbor_channel_announcement_t announcement;
} p25_neighbor_record_update_t;

/**
 * @brief Add a validated current-site control channel candidate (Hz).
 *
 * @param state Decoder state containing candidate list.
 * @param freq_hz Candidate control channel frequency in Hz.
 * @param bump_added Whether to increment the added counter/stat.
 * @return 1 if added; 0 if skipped.
 */
int p25_cc_add_candidate(dsd_state* state, long freq_hz, int bump_added);

/** Record resolved neighbor frequencies without promoting them as current-site candidates. */
void p25_cc_record_neighbor_frequencies(const dsd_opts* opts, dsd_state* state, const long* freqs, int count);

/** Return the next eligible current-site control-channel candidate. */
int p25_cc_next_candidate(dsd_state* state, long* out_freq);

/**
 * Apply a resolved neighbor update. An update with only @ref p25_neighbor_record_update_t.freq set refreshes the
 * matching record without clobbering learned metadata.
 */
void p25_nb_record_update(dsd_state* state, const p25_neighbor_record_update_t* update);
/**
 * @brief Age/expire neighbor control channel candidates (call periodically).
 *
 * @param state Decoder state containing neighbor list.
 */
void p25_nb_tick(dsd_state* state);

/** Store System Service Broadcast masks and request priority. */
void p25_store_system_service_broadcast(dsd_state* state, uint32_t available, uint32_t supported,
                                        uint8_t request_priority);
/** Store current-site Location Registration Area. */
void p25_store_site_lra(dsd_state* state, uint8_t lra);
/** Store current-site network-active/failsoft status. */
void p25_store_site_network_active(dsd_state* state, uint8_t network_active);
/** Store protected control-channel broadcast metadata without changing call protection state. */
void p25_store_protected_control_channel(dsd_state* state, uint8_t algid);

/** Return a service name for System Service Broadcast bit b1..b24. */
const char* p25_system_service_name_for_bit(unsigned int service_bit);

/** Format names for all known services present in a 24-bit System Service Broadcast mask. */
size_t p25_format_system_service_names(uint32_t service_mask, char* out, size_t out_len);

/** Return a name for a single adjacent-status CFVA mask bit (0x8, 0x4, 0x2, or 0x1). */
const char* p25_adjacent_cfva_flag_name(uint8_t flag_mask);

/** Format adjacent-status CFVA tags, including the current/last-known validity state. */
size_t p25_format_adjacent_cfva(uint8_t cfva, char* out, size_t out_len);

/**
 * @brief Resolve or defer a P25 adjacent-site channel announcement with explicit metadata validity.
 */
int p25_announce_neighbor_channel(const dsd_opts* opts, dsd_state* state,
                                  const p25_neighbor_channel_announcement_t* announcement);

/**
 * @brief Resolve or defer a P25 secondary control-channel announcement.
 *
 * Returns 1 when the channel resolved and was promoted as a current-site CC
 * candidate, 0 when it was ignored or retained pending a later IDEN update.
 */
int p25_announce_secondary_cc_channel(const dsd_opts* opts, dsd_state* state, uint16_t channel, uint8_t rfss,
                                      uint8_t site, uint8_t ssc);

/** Retry pending neighbor and secondary CC announcements after IDEN updates. */
void p25_resolve_pending_announcements(const dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CC_CANDIDATES_H_H */
