// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The system identity received on the carrier being decoded (dsd_state::rx_id).
 *
 * Decoder state keeps identity values that do not belong to the carrier on air: the Phase 2 seed (`p2_wacn`,
 * `p2_sysid`, `p2_cc`), which `-X` or the menu may set by hand, the control-channel anchors a trunking state machine
 * returns to, the TETRA network a `--tetra-net` preset names, and values a previous carrier left. Decoding needs them;
 * a display must not show them as received. This record holds only what the carrier being decoded sent, so every
 * frontend (Spectrum and Monitor screens, the terminal, call history) shows the same identity and none of another
 * carrier's.
 *
 * Protocol code calls a `dsd_rxid_note_*()` function where a message passed its own check (CRC, FEC, a confirmed
 * transmission), with that message's own values: never decoder state, a voice link control word, a fill-in, a cache,
 * a hand-set seed or a preset. Content accepted only because `-F` or RAS relaxed a check is never noted. Each part
 * has its own valid bit, so a carrier that sent some parts shows those alone, and a fresh bit, which the no-carrier
 * pass clears, so a retained value never reads as live.
 *
 * A note naming another system than the record's (another P25 WACN or SYSID, Tier III system code, Connect Plus or
 * XPT site, NXDN location or IDAS site, EDACS site, TETRA network or colour) drops what belonged to the old one:
 * the parts that bind to it and the control-channel frequency. An IDAS carrier names its site twice, in the SCCH site
 * ID and in a Type-D SRV_INFO: when the two disagree the newer one stays and the other is dropped. NXDN Type-C (a
 * location ID) and IDAS Type-D (area, site ID, SRV_INFO system ID) are two kinds of system: a checked note of one drops
 * what the other sent and the control channel. A RAN is Type-C's (a Type-D RTCH2 carries SCCH, FACCH1, UDCH2 and FACCH3,
 * NXDN TS 1-E 4.2.3, none of which carries one): a Type-D note drops a Type-C location and a RAN, and a RAN drops the
 * IDAS parts (a Type-C location keeps the RAN its own channel's check noted just before). So are the DMR trunking systems (Tier III, Capacity Plus,
 * Connect Plus, Hytera XPT): a note that names another one than the record's parts belong to (its brand, site text
 * prefix, rest LSN, system code or free LCN; dsd_rxid_dmr_trunk) drops those parts and the control channel; the colour
 * code is the carrier's and stays.
 *
 * Another access code on the same carrier is another system sharing the channel (co-channel DMR or dPMR colour codes,
 * P25 NACs, NXDN RANs): a valid access code that changes drops every other part of the family with the
 * control-channel frequency, so the new system's code never shows with the old one's site. An accepted trunk tune
 * drops the access code first, so the code a followed channel carries is the first, not a change. An M17 CAN belongs
 * to each transmission and names no system, so its change drops nothing (M17 sends no other part). A note that carries
 * both the access code and system parts notes the access code first.
 *
 * A frame read across a carrier boundary, or cut by an input interruption, is no carrier's: while such a frame is
 * dispatched (dsd_state::frame_dispatch_active, dsd_state_frame_read_lost() against frame_dispatch_carrier_seq) every
 * note is refused, whichever decoder's own carrier check comes after it.
 *
 * Lifetime: dsd_rxid_reset() at every carrier boundary and trunk-scan target switch; dsd_rxid_note_sync() at frame
 * sync drops a record of another protocol family; every accepted trunking retune drops the carrier's access code and
 * keeps the system (a voice channel belongs to its control channel's system), requested by the engine's trunk tuning
 * (dsd_rxid_request_trunk_tune()) and applied by the decoder thread (dsd_rxid_apply_trunk_tune()): at once for a
 * retune the decoder thread makes, in a frame or between frames, so nothing it publishes or notes after the retune
 * reads the carrier left's code; later for one the P25 watchdog makes.
 * Commands that forget or reinterpret a site forget it here too: the DMR reset (dsd_rxid_forget_system()), and a
 * committed change of the decode mode or the EDACS addressing mode (dsd_rxid_note_decoder_settings()).
 *
 * Plain integers and characters only: the record rides the dsd_state snapshot copy range (ui_snapshot.c pins it).
 *
 * Threading: written on the decoder thread only, and wherever the P25 watchdog's tick could run beside it, under the
 * P25 SM tick guard: a frame's notes run in processFrame() under it, and the no-carrier pass, the trunk-scan switches
 * and the commands that reset or forget take it. The watchdog's tick reads the record under the guard (the calls its
 * release ends keep it, dsd_call_snapshot::ended_rx_id) and writes nothing of it: its accepted retunes request the
 * access code's drop, which the decoder thread applies (dsd_rxid_apply_trunk_tune()). The decoder thread's own
 * retunes apply it at once under the guard: inside a frame dispatch (dsd_state::frame_dispatch_active, set only under
 * processFrame()'s hold) the request does, and elsewhere the engine's trunk tuning does on the thread a run marks as
 * the decoder's (dsd_engine_set_decoder_thread()), in its own hold or one it takes. The decoder thread reads the record
 * anywhere (frontend publication included). The type stays out of state.h so app-control's public views can name it.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_RECEIVED_IDENTITY_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_RECEIVED_IDENTITY_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The protocol family a record belongs to (dsd_received_identity::family). The values are stable. */
typedef enum {
    DSD_RXID_FAMILY_NONE = 0, /**< Nothing received since the last reset. */
    DSD_RXID_FAMILY_DMR = 1,  /**< DMR, base station or mobile (Tier II, Tier III, Capacity Plus, Connect Plus, XPT). */
    DSD_RXID_FAMILY_P25 = 2,  /**< P25 Phase 1 or Phase 2. */
    DSD_RXID_FAMILY_NXDN = 3, /**< NXDN, Type-C or IDAS (Type-D). */
    DSD_RXID_FAMILY_DPMR = 4, /**< dPMR. */
    DSD_RXID_FAMILY_M17 = 5,  /**< M17. */
    DSD_RXID_FAMILY_TETRA = 6, /**< TETRA trunked mode (a direct mode carrier has no system identity). */
    DSD_RXID_FAMILY_EDACS = 7, /**< EDACS, with its ProVoice voice channels. */
    /** A protocol that sends no identity this record holds (YSF, D-STAR, X2-TDMA, analog): a sync of it drops the
     * record of another family. Never stored. */
    DSD_RXID_FAMILY_OTHER = 8,
} dsd_rxid_family;

/** The DMR trunking system a record's DMR parts belong to (dsd_received_identity::dmr_trunk). The values are stable. */
typedef enum {
    DSD_RXID_DMR_TRUNK_NONE = 0,          /**< None named. */
    DSD_RXID_DMR_TRUNK_TIER3 = 1,         /**< Tier III: a system code, a "TIII" site text. */
    DSD_RXID_DMR_TRUNK_CAPACITY_PLUS = 2, /**< Capacity Plus: a rest LSN, a "Cap+" site text, its brand. */
    DSD_RXID_DMR_TRUNK_CONNECT_PLUS = 3,  /**< Connect Plus: a "Con+" site text, its brand. */
    DSD_RXID_DMR_TRUNK_XPT = 4,           /**< Hytera XPT: a free LCN, its brand. */
} dsd_rxid_dmr_trunk;

/** dsd_received_identity::valid and ::fresh bits, one per part a message can send on its own. */
enum {
    DSD_RXID_ACCESS = 1U << 0,             /**< access_kind/access: DMR/dPMR/TETRA colour code, NAC, RAN, CAN. */
    DSD_RXID_P25_WACN = 1U << 1,           /**< p25_wacn, with p25_sysid (a network status broadcast). */
    DSD_RXID_P25_SYSID = 1U << 2,          /**< p25_sysid. */
    DSD_RXID_P25_SITE = 1U << 3,           /**< p25_rfss/p25_site, of the system p25_sysid names. */
    DSD_RXID_P25_LRA = 1U << 4,            /**< p25_lra, of the system p25_sysid names. */
    DSD_RXID_DMR_REST_LSN = 1U << 5,       /**< dmr_rest_lsn: the Capacity Plus rest channel's logical slot number. */
    DSD_RXID_DMR_SITE_TEXT = 1U << 6,      /**< dmr_site_text: Tier III, Connect Plus or XPT network and site. */
    DSD_RXID_DMR_BRAND = 1U << 7,          /**< dmr_brand: the trunking brand that message names. */
    DSD_RXID_DMR_T3_SYSCODE = 1U << 8,     /**< dmr_t3_syscode: the Tier III system code. */
    DSD_RXID_NXDN_LOCATION = 1U << 9,      /**< nxdn_category/nxdn_sys/nxdn_site: a Type-C location ID. */
    DSD_RXID_NXDN_IDAS_AREA = 1U << 10,    /**< nxdn_idas_area: an IDAS SCCH area bit. */
    DSD_RXID_NXDN_IDAS_SITE = 1U << 11,    /**< nxdn_idas_site_type/nxdn_idas_site_code: an IDAS SCCH site ID. */
    DSD_RXID_NXDN_TYPED_SYSTEM = 1U << 12, /**< nxdn_typed_sys/nxdn_typed_site: a Type-D SRV_INFO system ID. */
    DSD_RXID_EDACS_SITE = 1U << 13,        /**< edacs_site. */
    DSD_RXID_EDACS_AREA = 1U << 14,        /**< edacs_area. */
    DSD_RXID_EDACS_SYSTEM = 1U << 15,      /**< edacs_system. */
    DSD_RXID_EDACS_CC_LCN = 1U << 16,      /**< edacs_cc_lcn. */
    DSD_RXID_TETRA_NET = 1U << 17,         /**< tetra_mcc/tetra_mnc; the cell's colour code is the access code. */
    DSD_RXID_TETRA_LA = 1U << 18,          /**< tetra_la (a D-MLE-SYSINFO). */
    DSD_RXID_CC_FREQ = 1U << 19,           /**< cc_freq_hz: this system's control channel, where it was received. */
    DSD_RXID_DMR_XPT_FREE_LCN = 1U << 20,  /**< dmr_xpt_free_lcn: the Hytera XPT site's free channel (LCN). */
    DSD_RXID_DMR_VENDOR = 1U << 21, /**< dmr_vendor: the manufacturer's feature set ID a checked LC or CSBK carried. */
    /** The carrier's DMR bursts carry RAS (Restricted Access to System): two RAS signature bursts on this carrier
     * confirmed it (dmr_ras_note_signature()). A property of the codewords received, never a decoded field; carrier
     * scoped, so the DMR RAS reset (dmr_ras_reset()) drops it at every carrier change, a trunk tune included. */
    DSD_RXID_DMR_RAS = 1U << 22,
};

/** The system identity received on the carrier being decoded. Read a value only while its valid bit is set. */
typedef struct dsd_received_identity {
    /* The fields go by size (32-bit, 16-bit, 8-bit, then text), each size in protocol order, so the record carries one
       byte of padding. 32-bit parts: */
    /** Bumped by every dsd_rxid_reset() and family replacement, and whenever a note invalidates parts because it names
     * another system, site, cell or subtype (another access code on the carrier included): a reader can tell one
     * identity from the next, and a call's history row keeps the one it was heard under. A note that only adds or
     * repeats, the no-carrier pass and a trunk follow's dropped access code keep it. */
    uint32_t epoch;
    uint32_t valid;          /**< DSD_RXID_* parts that hold a received value. */
    uint32_t fresh;          /**< DSD_RXID_* parts noted since the last no-carrier pass; a subset of valid. */
    uint32_t p25_wacn;       /**< 20 bits. */
    uint32_t nxdn_sys;       /**< Location ID system code. */
    uint32_t nxdn_typed_sys; /**< Type-D SRV_INFO 19-bit system ID. */
    uint32_t cc_freq_hz;

    /* 16-bit parts: */
    uint16_t access;    /**< The access code, of access_kind. */
    uint16_t p25_sysid; /**< 12 bits, never 0. */
    uint16_t nxdn_site; /**< Location ID site code. */
    uint16_t dmr_rest_lsn;
    uint16_t dmr_t3_syscode;
    uint16_t edacs_site;
    uint16_t edacs_area;
    uint16_t edacs_system;
    uint16_t edacs_cc_lcn;
    uint16_t tetra_mcc; /**< 10 bits. */
    uint16_t tetra_mnc; /**< 14 bits. */
    uint16_t tetra_la;  /**< 14 bits. */

    /* 8-bit parts: */
    uint8_t family;      /**< dsd_rxid_family */
    uint8_t access_kind; /**< dsd_access_code_kind (<dsd-neo/core/access_code.h>) */
    uint8_t p25_rfss;
    uint8_t p25_site;
    uint8_t p25_lra;
    uint8_t nxdn_category;       /**< Location ID category: 0 Global, 1 Regional, 2 Local (3 is reserved). */
    uint8_t nxdn_idas_area;      /**< The SCCH area bit. */
    uint8_t nxdn_idas_site_type; /**< 1 Wide, 2 Middle, 3 Narrow (NXDN TS 1-E 6.6.4). */
    uint8_t nxdn_idas_site_code; /**< 1..250 (NXDN TS 1-E 6.6.5). */
    uint8_t nxdn_typed_site;     /**< Type-D SRV_INFO site code. */
    uint8_t dmr_xpt_free_lcn;    /**< 0..15, as the XPT message sends it. */
    /** dsd_rxid_dmr_trunk the DMR trunking parts (rest LSN, site text, brand, system code, free LCN) belong to;
     * meaningful while one of them is valid. */
    uint8_t dmr_trunk;
    /** The manufacturer's feature set ID (FID) a checked LC or CSBK carried, e.g. 0x68 Hytera, 0x58 Tait, 0x10 or 0x06
     * Motorola; no trunking type rule touches it. */
    uint8_t dmr_vendor;
    int8_t edacs_ea_mode; /**< dsd_state::ea_mode the EDACS parts were read under; meaningful while one is valid. */
    /** The colour code of the cell tetra_mcc/tetra_mnc came with (6 bits), valid with DSD_RXID_TETRA_NET. A trunk tune
     * drops the access code from display; the cell stays bound to this colour. */
    uint8_t tetra_colour;

    /* Text: */
    /** Display text, its parts joined by " · " (U+00B7), e.g. "TIII Small · Net 12 · Site 3.1 · SYS 0A3F",
     * "Cap+ · Site 2", "Con+ · Net 3 · Site 5"; NUL-terminated. The view shows each part as a token, the "Net n" and
     * "Site n" parts on the compact line. */
    char dmr_site_text[64];
    char dmr_brand[32]; /**< e.g. "Capacity Plus"; NUL-terminated. */
} dsd_received_identity;

/** The family a `DSD_SYNC_*` value belongs to: NONE for no sync, OTHER for a protocol without system identity. */
dsd_rxid_family dsd_rxid_family_for_sync(int synctype);

/** Forget everything (a carrier boundary, a trunk-scan target switch, a decoder reset). */
void dsd_rxid_reset(dsd_state* state);
/** Frame sync acquired @p synctype: a record of another family is dropped. No sync (DSD_SYNC_NONE) changes nothing. */
void dsd_rxid_note_sync(dsd_state* state, int synctype);
/** The no-carrier pass: every part goes stale; the values stay. */
void dsd_rxid_mark_stale(dsd_state* state);
/** An accepted trunking retune (to a voice channel, or back to the control channel): the access code belonged to
 * the carrier left; the system stays. A refused retune must not call this. Decoder thread only: the engine's trunk
 * tuning, which the P25 watchdog also runs, requests it instead (dsd_rxid_request_trunk_tune()). */
void dsd_rxid_trunk_tune(dsd_state* state);
/** An accepted trunking retune made under the P25 SM tick guard, on the decoder thread or the P25 watchdog's: records
 * the drop dsd_rxid_trunk_tune() makes (dsd_state::rx_id_trunk_tune_pending), which the decoder thread applies
 * (dsd_rxid_apply_trunk_tune()). Inside a frame dispatch (dsd_state::frame_dispatch_active), which only the decoder
 * thread runs, under processFrame()'s hold on the guard, it applies the drop at once: a handler that publishes or
 * decodes more bursts after the retune (DMR's burst loop) shows and notes only the reached carrier's code. Outside one
 * it writes nothing of the record; the engine's trunk tuning applies the drop itself when the decoder thread retuned.
 * A refused retune must not call this. Returns 1 when it applied the drop at once (inside a frame dispatch), 0 when
 * the drop waits: the engine resets the DMR RAS evidence (dmr_ras_reset()) wherever a drop is applied. */
int dsd_rxid_request_trunk_tune(dsd_state* state);
/** Decoder thread, under the P25 SM tick guard: applies a requested retune's drop (dsd_rxid_trunk_tune()) once. A
 * decoder-thread retune applies it where it is made; one the watchdog requested waits for the decode loop, which
 * applies it before a frame's first note and after the trunking ticks that follow the frame, for the no-carrier pass
 * before the record goes stale, and for a trunk-scan rollback before it keeps the record: it is applied before anything
 * the carrier it reached sends is noted. dsd_rxid_reset() forgets a request. Returns 1 when it applied a drop, 0 when
 * none was pending: the engine then resets the DMR RAS evidence (dmr_ras_reset()), which is the DMR decoder's. */
int dsd_rxid_apply_trunk_tune(dsd_state* state);

/** The access code of @p kind (dsd_access_code_kind) a checked message of @p family carried on this carrier. Another
 * value than a valid one drops the family's other parts (another system shares the carrier); M17 drops nothing. */
void dsd_rxid_note_access(dsd_state* state, dsd_rxid_family family, uint8_t kind, uint16_t value);

/** A checked P25 network status: WACN and SYSID (SYSID 0 is ignored). Another WACN or SYSID than the record's
 * replaces its system; the same SYSID completes a record a site or LRA started. */
void dsd_rxid_note_p25_network(dsd_state* state, uint32_t wacn, uint16_t sysid);
/** A checked P25 RFSS status: the SYSID it advertises, RFSS and site (SYSID 0 is ignored). A site of another system
 * than the record's is not this carrier's and is ignored; with no SYSID yet it gives the record its SYSID. */
void dsd_rxid_note_p25_site(dsd_state* state, uint16_t sysid, uint8_t rfss, uint8_t site);
/** A checked P25 location registration area, under @p sysid, bound as a site is. */
void dsd_rxid_note_p25_lra(dsd_state* state, uint16_t sysid, uint8_t lra);

void dsd_rxid_note_dmr_rest_lsn(dsd_state* state, uint16_t lsn);
/** @p text is display text for the site (copied, truncated to the record's capacity). */
void dsd_rxid_note_dmr_site_text(dsd_state* state, const char* text);
void dsd_rxid_note_dmr_brand(dsd_state* state, const char* brand);
/** The manufacturer's feature set ID @p mfid of a checked full LC or CSBK (0, the standard set, names none). */
void dsd_rxid_note_dmr_vendor(dsd_state* state, uint8_t mfid);
void dsd_rxid_note_dmr_t3_syscode(dsd_state* state, uint16_t syscode);
/** The DMR decoder confirmed RAS on this carrier (dmr_ras_note_signature()): sets DSD_RXID_DMR_RAS. It notes no decoded
 * field, so it neither moves the epoch nor names another system. */
void dsd_rxid_note_dmr_ras(dsd_state* state);
/** Drops DSD_RXID_DMR_RAS (dmr_ras_reset()), whatever frame is being dispatched; the epoch stays. */
void dsd_rxid_forget_dmr_ras(dsd_state* state);
/** A checked Hytera XPT site status, call alert or short LC: the logical channel number the site names free now (its
 * 4-bit field as sent). It changes with the site's traffic, so each note replaces the last. */
void dsd_rxid_note_dmr_xpt_free_lcn(dsd_state* state, uint8_t lcn);

/** A confirmed Type-C location ID of category 0..2 (3 is reserved and ignored). */
void dsd_rxid_note_nxdn_location(dsd_state* state, uint8_t category, uint32_t sys, uint16_t site);
/** A confirmed IDAS SCCH area bit. It tells adjacent co-channel sites apart (NXDN TS 1-E 6.6.3): another value than a
 * valid one drops the SCCH site ID, the Type-D system ID and the control-channel frequency. */
void dsd_rxid_note_nxdn_idas_area(dsd_state* state, uint8_t area);
/** A confirmed IDAS SCCH site ID: site type 1..3 and site code 1..250 (anything else is ignored). A Type-D system ID
 * of another site code is dropped with the control-channel frequency. */
void dsd_rxid_note_nxdn_idas_site(dsd_state* state, uint8_t site_type, uint8_t site_code);
/** A confirmed Type-D SRV_INFO: the 19-bit system ID and site code. Another system drops the SCCH area and site ID and
 * the control-channel frequency; another site code than the SCCH site ID's drops that site ID. */
void dsd_rxid_note_nxdn_typed_system(dsd_state* state, uint32_t sys, uint8_t site);

void dsd_rxid_note_edacs_site(dsd_state* state, uint16_t site);
void dsd_rxid_note_edacs_area(dsd_state* state, uint16_t area);
void dsd_rxid_note_edacs_system(dsd_state* state, uint16_t system);
void dsd_rxid_note_edacs_cc_lcn(dsd_state* state, uint16_t lcn);

/** A CRC-checked trunked mode BSCH, or a CRC-checked downlink block descrambled with the network's extended colour
 * code (every carrier of a cell scrambles with it): the network and the cell's colour code (the access code). Another
 * network or colour than the cell's (tetra_colour, kept across a trunk tune) drops the location area and the
 * control-channel frequency. */
void dsd_rxid_note_tetra_cell(dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t colour);
void dsd_rxid_note_tetra_la(dsd_state* state, uint16_t la);
/** A checked SYSINFO named another cell of the same network and colour code (another main carrier or location area):
 * the last cell's location area and control-channel frequency are dropped, before the SYSINFO's own are noted. */
void dsd_rxid_note_tetra_other_cell(dsd_state* state);
/** A checked direct mode burst: the carrier belongs to no trunked network, so a TETRA record is dropped. */
void dsd_rxid_note_tetra_direct_mode(dsd_state* state);

/** The operator forgot @p family's site (the DMR reset command): a record of that family drops every part but the
 * access code, and the access code too when @p with_access is set. */
void dsd_rxid_forget_system(dsd_state* state, dsd_rxid_family family, int with_access);
/** The decoder settings changed (a decode mode, the EDACS addressing mode): a record of a protocol @p opts no longer
 * decodes, or an EDACS record read under the other addressing mode, is dropped. Call it once the change commits, after
 * the calls it ends were ended (they keep the identity they ended with). */
void dsd_rxid_note_decoder_settings(const dsd_opts* opts, dsd_state* state);

/** A checked message of @p family confirmed this carrier, at @p freq_hz, as the control channel of the system the
 * record names. 0 Hz (an unknown tuner frequency) is ignored. */
void dsd_rxid_note_cc_freq(dsd_state* state, dsd_rxid_family family, uint32_t freq_hz);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_RECEIVED_IDENTITY_H_ */
