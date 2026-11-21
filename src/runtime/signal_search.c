// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/airspy_config.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <string.h>

/* The last status revision handed out; 0 is never one. */
static dsd_atomic_u64 status_revision = {0U};

uint64_t
dsd_signal_search_status_revision_next(void) {
    return dsd_atomic_u64_fetch_add_relaxed(&status_revision, 1U) + 1U;
}

void
dsd_signal_search_range_default(dsd_signal_search_range* range) {
    if (!range) {
        return;
    }
    DSD_MEMSET(range, 0, sizeof(*range));
    dsd_airspy_config_defaults(&range->airspy);
    DSD_SNPRINTF(range->name, sizeof(range->name), "%s", "UHF");
    range->enabled = 1;
    range->start_hz = 450000000;
    range->end_hz = 470000000;
    range->step_num = 12500;
    range->step_den = 1;
    range->bandwidth_hz = 16000;
    range->digital = 1;
    range->sensitivity_db = 15;
    range->minimum_dbfs = -60;
    range->squelch_mode = DSD_SQUELCH_MODE_NOISE;
    range->squelch_margin_db = 10;
    range->squelch_dbfs = -60;
    /* Three seconds: an Auto probe reaches P25 Phase 2 (6000/4) 1.1 s in and needs up to about a second more to
       recover a scrambling code and confirm, and NXDN48 joined mid-call confirms up to 2.3 s in (issue #804,
       tests/engine/search_schedule_replay.c). Two seconds stays the shortest a range may ask for. */
    range->probe_ms = 3000;
    range->max_visit_ms = 30000;
}

uint32_t
dsd_signal_search_channel_count(const dsd_signal_search_range* range) {
    if (!range || !range->step_num || !range->step_den || range->end_hz < range->start_hz) {
        return 0;
    }
    uint64_t n = ((uint64_t)(range->end_hz - range->start_hz) * range->step_den) / range->step_num + 1;
    return n <= DSD_SEARCH_MAX_CHANNELS ? (uint32_t)n : 0;
}

uint32_t
dsd_signal_search_frequency(const dsd_signal_search_range* range, uint32_t index) {
    if (!range || index >= dsd_signal_search_channel_count(range)) {
        return 0;
    }
    return range->start_hz + (uint32_t)(((uint64_t)index * range->step_num + range->step_den / 2) / range->step_den);
}

static const char*
validate_grid(const dsd_signal_search_range* range) {
    if (!range || !memchr(range->name, '\0', sizeof(range->name))) {
        return "Invalid range name";
    }
    if (!range->start_hz || range->end_hz < range->start_hz) {
        return "End frequency must follow start frequency";
    }
    if (!range->step_den || range->step_den > 1000 || !range->step_num || range->step_num > 100000000
        || range->step_num < range->step_den || !dsd_signal_search_channel_count(range)) {
        return "Range must contain 1 to 100000 channels with a positive rational step";
    }
    if (!dsd_analog_width_in_range(range->am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM, (int)range->bandwidth_hz)) {
        return "Channel width must be 8000 to 25000 Hz for NFM, or 5000 to 20000 Hz for AM";
    }
    return NULL;
}

static const char*
validate_receiver(const dsd_signal_search_range* range) {
    if (range->sensitivity_db < 3 || range->sensitivity_db > 30 || range->minimum_dbfs < -160
        || range->minimum_dbfs > 3) {
        return "RF sensitivity must be 3 to 30 dB and minimum level -160 to 3 dBFS";
    }
    if (range->gain_tenth_db < -100 || range->gain_tenth_db > 1000) {
        return "Invalid tuner gain";
    }
    if (range->airspy_gain_set && !dsd_airspy_config_valid(&range->airspy)) {
        return "Invalid Airspy gain controls";
    }
    if (range->digital < 0 || range->digital > DSD_SEARCH_DIGITAL_TETRA || (range->am && range->digital)) {
        return "AM requires analog-only monitoring";
    }
    return NULL;
}

static const char*
validate_listening(const dsd_signal_search_range* range) {
    if (range->probe_ms < 2000 || range->probe_ms > 10000 || range->max_visit_ms < 0 || range->max_visit_ms > 600000) {
        return "Probe must be 2 to 10 seconds; maximum visit 0 to 600 seconds";
    }
    if (range->squelch_mode < DSD_SQUELCH_MODE_LEVEL || range->squelch_mode > DSD_SQUELCH_MODE_NOISE
        || (range->am && range->squelch_mode == DSD_SQUELCH_MODE_NOISE) || range->squelch_margin_db < 0
        || range->squelch_margin_db > DSD_SQUELCH_MARGIN_MAX_DB || range->squelch_dbfs < -160
        || range->squelch_dbfs > 0) {
        return "Invalid listening squelch for this modulation";
    }
    /* The receiver clamps a dynamic squelch's margin into this range; a saved margin outside it would not be the one
       applied. Level mode has no margin. */
    if (dsd_squelch_mode_is_dynamic(range->squelch_mode) && range->squelch_margin_db < DSD_SQUELCH_MARGIN_MIN_DB) {
        return "Squelch margin must be 3 to 30 dB";
    }
    return NULL;
}

int
dsd_signal_search_validate(const dsd_signal_search_range* range, char* error, size_t capacity) {
    const char* why = validate_grid(range);
    if (!why) {
        why = validate_receiver(range);
    }
    if (!why) {
        why = validate_listening(range);
    }
    if (error && capacity) {
        DSD_SNPRINTF(error, capacity, "%s", why ? why : "");
    }
    return why ? -1 : 0;
}

int
dsd_signal_search_range_is_tetra(const dsd_signal_search_range* range) {
    return range && !range->am && range->digital == DSD_SEARCH_DIGITAL_TETRA;
}

int
dsd_signal_search_config_needs_tetra(const dsd_signal_search_config* config) {
    if (!config) {
        return 0;
    }
    for (uint32_t i = 0; i < config->range_count && i < DSD_SEARCH_MAX_RANGES; ++i) {
        if (config->ranges[i].enabled && dsd_signal_search_range_is_tetra(&config->ranges[i])) {
            return 1;
        }
    }
    return 0;
}

static int
search_config_error(char* error, size_t capacity, const char* why) {
    if (error && capacity) {
        DSD_SNPRINTF(error, capacity, "%s", why);
    }
    return -1;
}

int
dsd_signal_search_validate_config(const dsd_signal_search_config* config, char* error, size_t capacity) {
    if (!config || !config->range_count || config->range_count > DSD_SEARCH_MAX_RANGES) {
        return search_config_error(error, capacity, "Invalid signal search configuration");
    }
    int enabled = 0;
    uint64_t total_channels = 0;
    for (uint32_t i = 0; i < config->range_count; ++i) {
        if (dsd_signal_search_validate(&config->ranges[i], error, capacity)) {
            return -1;
        }
        if (config->ranges[i].enabled) {
            ++enabled;
            total_channels += dsd_signal_search_channel_count(&config->ranges[i]);
        }
    }
    if (!enabled) {
        return search_config_error(error, capacity, "Enable at least one range");
    }
    if (total_channels > DSD_SEARCH_MAX_CHANNELS) {
        return search_config_error(error, capacity, "A search supports up to 100,000 enabled channels");
    }
    if (error && capacity) {
        error[0] = '\0';
    }
    return 0;
}

int
dsd_signal_fft_size_for_width(uint32_t rate, double width_hz) {
    if (!rate || !(width_hz > 0)) {
        return 0;
    }
    int n = 32;
    while ((double)rate * 8 > width_hz * n && n < DSD_SEARCH_MAX_FFT) {
        n *= 2;
    }
    return (double)rate * 8 <= width_hz * n ? n : 0;
}

int
dsd_signal_search_fft_size(uint32_t rate, const dsd_signal_search_range* range) {
    if (!rate || dsd_signal_search_validate(range, NULL, 0)) {
        return 0;
    }
    double width = (double)range->step_num / range->step_den;
    if (width > range->bandwidth_hz) {
        width = range->bandwidth_hz;
    }
    return dsd_signal_fft_size_for_width(rate, width);
}

int
dsd_signal_search_plan(const dsd_signal_search_range* range, uint32_t first, uint32_t rate, int64_t capture_offset,
                       dsd_signal_search_span* span) {
    if (!span || !dsd_signal_search_fft_size(rate, range)) {
        return -1;
    }
    DSD_MEMSET(span, 0, sizeof(*span));
    uint32_t total = dsd_signal_search_channel_count(range);
    if (first >= total) {
        return 0;
    }
    /* Four channel widths leave room for the noise sidebands; an additional 1% protects DC.
       Always place the first unvisited channel in this lobe, including DC/edge holes of earlier spans. */
    int64_t margin = (int64_t)range->bandwidth_hz * 4;
    int64_t width = (int64_t)rate * 4 / 10 - 2 * margin - rate / 100;
    if (width < 0) {
        return -1;
    }
    int64_t low = dsd_signal_search_frequency(range, first);
    int64_t capture = low + (int64_t)rate * 4 / 10 - margin;
    int64_t tune = capture - capture_offset;
    if (tune <= 0 || tune > UINT32_MAX || capture > UINT32_MAX) {
        return -1;
    }
    span->tune_hz = (uint32_t)tune;
    span->first = first;
    while (first < total && span->count < DSD_SEARCH_MAX_CANDIDATES) {
        uint32_t f = dsd_signal_search_frequency(range, first);
        if ((int64_t)f > low + width) {
            break;
        }
        span->frequencies[span->count++] = f;
        ++first;
    }
    span->next = first;
    return 1;
}

void
dsd_signal_search_site_clear(dsd_signal_search_site* record) {
    if (record) {
        DSD_MEMSET(record, 0, sizeof(*record));
    }
}

static const char*
site_note_protocol(int kind) {
    switch (kind) {
        case DSD_SEARCH_SITE_NOTE_P25_NETWORK:
        case DSD_SEARCH_SITE_NOTE_P25_SITE: return "P25";
        case DSD_SEARCH_SITE_NOTE_DMR_TIII: return "DMR";
        case DSD_SEARCH_SITE_NOTE_NXDN: return "NXDN";
        case DSD_SEARCH_SITE_NOTE_TETRA:
        case DSD_SEARCH_SITE_NOTE_TETRA_WITHDRAW: return "TETRA";
        default: return NULL;
    }
}

/* The part of a TETRA note its mode shows, each value only when the note marks it known, so two notes that show the
   same compare equal whatever else they carry. */
static dsd_signal_search_tetra_note
site_tetra_shown(const dsd_signal_search_tetra_note* note) {
    dsd_signal_search_tetra_note shown;
    DSD_MEMSET(&shown, 0, sizeof(shown));
    shown.mode = note->mode;
    if (note->mode == DSD_SEARCH_TETRA_TMO) {
        shown.mcc = note->mcc;
        shown.mnc = note->mnc;
        if (note->la_valid) {
            shown.la_valid = 1;
            shown.la = note->la;
        }
        if (note->main_state == DSD_SEARCH_TETRA_MAIN_HZ || note->main_state == DSD_SEARCH_TETRA_MAIN_NO_CA) {
            shown.main_state = note->main_state;
        }
        if (note->main_state == DSD_SEARCH_TETRA_MAIN_HZ) {
            shown.main_hz = note->main_hz;
        }
    } else {
        shown.presence_only = note->presence_only ? 1 : 0;
        shown.route = shown.presence_only || note->route > DSD_SEARCH_TETRA_ROUTE_REPEATER_GATEWAY ? 0 : note->route;
    }
    return shown;
}

/* The record @p note would make on its own. */
static void
site_from_note(dsd_signal_search_site* record, const dsd_signal_search_site_note* note, const char* protocol) {
    DSD_MEMSET(record, 0, sizeof(*record));
    DSD_SNPRINTF(record->protocol, sizeof(record->protocol), "%s", protocol);
    switch (note->kind) {
        case DSD_SEARCH_SITE_NOTE_P25_NETWORK:
            record->valid = DSD_SEARCH_SITE_WACN | DSD_SEARCH_SITE_SYSID;
            record->wacn = note->wacn;
            record->sysid = note->sysid;
            break;
        case DSD_SEARCH_SITE_NOTE_P25_SITE:
            record->valid = DSD_SEARCH_SITE_SYSID | DSD_SEARCH_SITE_RFSS_SITE;
            record->sysid = note->sysid;
            record->rfss = note->rfss;
            record->site = note->site;
            break;
        case DSD_SEARCH_SITE_NOTE_DMR_TIII:
            record->valid = DSD_SEARCH_SITE_TEXT;
            DSD_SNPRINTF(record->text, sizeof(record->text), "%.*s", (int)sizeof(note->text) - 1, note->text);
            break;
        case DSD_SEARCH_SITE_NOTE_TETRA:
            record->valid = DSD_SEARCH_SITE_TETRA;
            record->tetra = site_tetra_shown(&note->tetra);
            break;
        case DSD_SEARCH_SITE_NOTE_TETRA_WITHDRAW: record->withdrawn = 1; break;
        default:
            record->valid = DSD_SEARCH_SITE_NXDN;
            record->nxdn_sys = note->nxdn_sys;
            record->nxdn_site = note->nxdn_site;
            break;
    }
}

/* Field by field: the note struct has padding. */
static int
site_tetra_equal(const dsd_signal_search_tetra_note* a, const dsd_signal_search_tetra_note* b) {
    return a->mode == b->mode && a->mcc == b->mcc && a->mnc == b->mnc && a->la == b->la && a->la_valid == b->la_valid
           && a->main_state == b->main_state && a->main_hz == b->main_hz && a->route == b->route
           && a->presence_only == b->presence_only;
}

static int
site_equal(const dsd_signal_search_site* a, const dsd_signal_search_site* b) {
    return strcmp(a->protocol, b->protocol) == 0 && a->valid == b->valid && a->wacn == b->wacn && a->sysid == b->sysid
           && a->rfss == b->rfss && a->site == b->site && strcmp(a->text, b->text) == 0 && a->nxdn_sys == b->nxdn_sys
           && a->nxdn_site == b->nxdn_site && site_tetra_equal(&a->tetra, &b->tetra) && a->withdrawn == b->withdrawn;
}

/* Whether @p note is no TETRA note this record may take: a mode the hooks do not define, or any TETRA note once the
   record's TETRA identity was withdrawn (a second withdrawal included, which changes nothing). */
static int
site_tetra_refused(const dsd_signal_search_site* record, const dsd_signal_search_site_note* note) {
    if (note->kind == DSD_SEARCH_SITE_NOTE_TETRA && note->tetra.mode != DSD_SEARCH_TETRA_TMO
        && note->tetra.mode != DSD_SEARCH_TETRA_DMO) {
        return 1;
    }
    return record->withdrawn && strcmp(record->protocol, "TETRA") == 0;
}

/* @p next (a copy of @p record) after @p note: a P25 site or network note completes the same system's record, any other
   note replaces it. 0 when the note is not this record's. */
static int
site_merge_note(dsd_signal_search_site* next, const dsd_signal_search_site* record,
                const dsd_signal_search_site_note* note, const char* protocol) {
    const int same_protocol = strcmp(record->protocol, protocol) == 0;
    const int has_sysid = same_protocol && (record->valid & DSD_SEARCH_SITE_SYSID);
    if (note->kind == DSD_SEARCH_SITE_NOTE_P25_SITE && has_sysid) {
        /* A site of another system is not this record's: the network it advertised under stays. */
        if (record->sysid != note->sysid) {
            return 0;
        }
        next->valid |= DSD_SEARCH_SITE_RFSS_SITE;
        next->rfss = note->rfss;
        next->site = note->site;
    } else if (note->kind == DSD_SEARCH_SITE_NOTE_P25_NETWORK && has_sysid && record->sysid == note->sysid
               && (!(record->valid & DSD_SEARCH_SITE_WACN) || record->wacn == note->wacn)) {
        /* The network a site note named by its SYSID alone: the site stays. */
        next->valid |= DSD_SEARCH_SITE_WACN;
        next->wacn = note->wacn;
    } else {
        site_from_note(next, note, protocol);
    }
    return 1;
}

int
dsd_signal_search_site_apply(dsd_signal_search_site* record, const dsd_signal_search_site_note* note) {
    const char* protocol = note ? site_note_protocol(note->kind) : NULL;
    if (!record || !protocol || (note->kind == DSD_SEARCH_SITE_NOTE_DMR_TIII && !note->text[0])) {
        return 0;
    }
    if (strcmp(protocol, "TETRA") == 0 && site_tetra_refused(record, note)) {
        return 0;
    }
    dsd_signal_search_site next = *record;
    if (!site_merge_note(&next, record, note, protocol) || site_equal(&next, record)) {
        return 0;
    }
    *record = next;
    return 1;
}

/* U+00B7 MIDDLE DOT between parts, as the frontends separate details. */
static const char site_dot[] = " \xC2\xB7 ";

/* A direct mode record: the route of the latest checked DMAC-SYNC, or what a presence-only confirmation shows. */
static const char*
site_tetra_dmo_text(const dsd_signal_search_tetra_note* t) {
    if (t->presence_only) {
        return "presence signal";
    }
    switch (t->route) {
        case DSD_SEARCH_TETRA_ROUTE_DIRECT: return "Direct";
        case DSD_SEARCH_TETRA_ROUTE_REPEATER: return "Via repeater";
        case DSD_SEARCH_TETRA_ROUTE_GATEWAY: return "Via gateway";
        case DSD_SEARCH_TETRA_ROUTE_REPEATER_GATEWAY: return "Via repeater/gateway";
        default: return "";
    }
}

/* "Main carrier 426.70625 MHz · MCC 228 · MNC 8889 · LA 12": the main carrier first, so a detail cut short keeps it
   (issue #748), in MHz to five decimals (10 Hz), or "No main carrier", or nothing when no SYSINFO is shown; the LA only
   when known and the cell has a main carrier; a direct mode record's route text. */
static int
site_tetra_format(const dsd_signal_search_site* record, char* out, size_t capacity) {
    const dsd_signal_search_tetra_note* t = &record->tetra;
    if (record->withdrawn || !(record->valid & DSD_SEARCH_SITE_TETRA)) {
        return 0;
    }
    if (t->mode == DSD_SEARCH_TETRA_DMO) {
        return DSD_SNPRINTF(out, capacity, "%s", site_tetra_dmo_text(t));
    }
    char main_carrier[48] = "";
    char la[24] = "";
    if (t->main_state == DSD_SEARCH_TETRA_MAIN_NO_CA) {
        DSD_SNPRINTF(main_carrier, sizeof(main_carrier), "No main carrier%s", site_dot);
    } else {
        if (t->main_state == DSD_SEARCH_TETRA_MAIN_HZ) {
            const uint64_t tens = ((uint64_t)t->main_hz + 5U) / 10U;
            DSD_SNPRINTF(main_carrier, sizeof(main_carrier), "Main carrier %u.%05u MHz%s", (unsigned)(tens / 100000U),
                         (unsigned)(tens % 100000U), site_dot);
        }
        if (t->la_valid) {
            DSD_SNPRINTF(la, sizeof(la), "%sLA %u", site_dot, (unsigned)t->la);
        }
    }
    return DSD_SNPRINTF(out, capacity, "%sMCC %u%sMNC %u%s", main_carrier, (unsigned)t->mcc, site_dot, (unsigned)t->mnc,
                        la);
}

size_t
dsd_signal_search_site_format(const dsd_signal_search_site* record, char* out, size_t capacity) {
    if (!out || !capacity) {
        return 0;
    }
    out[0] = '\0';
    if (!record || !record->protocol[0]) {
        return 0;
    }
    const char* dot = site_dot;
    int n = 0;
    if (strcmp(record->protocol, "TETRA") == 0) {
        n = site_tetra_format(record, out, capacity);
    } else if (record->valid & DSD_SEARCH_SITE_TEXT) {
        n = DSD_SNPRINTF(out, capacity, "%s", record->text);
    } else if (record->valid & DSD_SEARCH_SITE_NXDN) {
        n = DSD_SNPRINTF(out, capacity, "Site %u%sSys %u", (unsigned)record->nxdn_site, dot,
                         (unsigned)record->nxdn_sys);
    } else {
        char wacn[16] = "";
        char site[48] = "";
        if (record->valid & DSD_SEARCH_SITE_WACN) {
            DSD_SNPRINTF(wacn, sizeof(wacn), "WACN %05X%s", (unsigned)record->wacn, dot);
        }
        if (record->valid & DSD_SEARCH_SITE_RFSS_SITE) {
            DSD_SNPRINTF(site, sizeof(site), "%sRFSS %u%sSite %u", dot, (unsigned)record->rfss, dot,
                         (unsigned)record->site);
        }
        n = DSD_SNPRINTF(out, capacity, "%sSYS %03X%s", wacn, (unsigned)record->sysid, site);
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < capacity ? (size_t)n : capacity - 1;
}
