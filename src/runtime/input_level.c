// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/input_level.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/log.h>
#include <math.h>

enum {
    DSD_INPUT_LEVEL_TOAST_TTL_SEC = 4,
};

static const double DSD_INPUT_LEVEL_CLIP_PCT = 0.1;
static const double DSD_INPUT_LEVEL_HOT_PEAK_DBFS = -1.0;
static const double DSD_INPUT_LEVEL_SILENCE_FLOOR_DBFS = -120.0;
static const double DSD_INPUT_LEVEL_SILENCE_EPSILON_DB = 0.1;

static double
input_level_pwr_to_db(double mean_power) {
    if (mean_power <= 0.0 || !isfinite(mean_power)) {
        return -120.0;
    }
    double db = 10.0 * log10(mean_power);
    if (db > 0.0) {
        db = 0.0;
    }
    if (db < -120.0) {
        db = -120.0;
    }
    return db;
}

static double
input_level_db_to_pwr(double db) {
    if (db >= 0.0) {
        return 1.0;
    }
    if (db < -200.0 || !isfinite(db)) {
        db = -200.0;
    }
    const double ln10_over_10 = 2.302585092994046 / 10.0;
    double pwr = exp(db * ln10_over_10);
    if (pwr < 0.0) {
        pwr = 0.0;
    }
    return pwr;
}

const char*
dsd_input_level_status_label(dsd_input_level_status status) {
    switch (status) {
        case DSD_INPUT_LEVEL_OK: return "OK";
        case DSD_INPUT_LEVEL_LOW: return "LOW";
        case DSD_INPUT_LEVEL_HOT: return "HOT";
        case DSD_INPUT_LEVEL_CLIPPING: return "CLIP";
        case DSD_INPUT_LEVEL_UNKNOWN:
        default: return "UNKNOWN";
    }
}

int
dsd_input_level_source_is_rf(dsd_input_level_source source) {
    switch (source) {
        case DSD_INPUT_LEVEL_SOURCE_RTL_CU8:
        case DSD_INPUT_LEVEL_SOURCE_RTL_TCP_CU8:
        case DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16:
        case DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32: return 1;
        case DSD_INPUT_LEVEL_SOURCE_PCM:
        case DSD_INPUT_LEVEL_SOURCE_UNKNOWN:
        default: return 0;
    }
}

const char*
dsd_input_level_display_label(dsd_input_level_source source) {
    return dsd_input_level_source_is_rf(source) ? "RF Level" : "Input Level";
}

static double
input_level_peak_to_dbfs(double peak) {
    if (peak <= 0.0 || !isfinite(peak)) {
        return -120.0;
    }
    double db = 20.0 * log10(peak);
    if (db > 0.0) {
        db = 0.0;
    }
    if (db < -120.0) {
        db = -120.0;
    }
    return db;
}

static double
input_level_safe_abs(double value) {
    if (!isfinite(value)) {
        return 1.0;
    }
    return fabs(value);
}

static void
input_level_fill(dsd_input_level_snapshot* out, dsd_input_level_source source, double mean_power, double peak,
                 uint64_t clipped, uint64_t count) {
    if (!out) {
        return;
    }
    out->status = DSD_INPUT_LEVEL_UNKNOWN;
    out->source = source;
    out->rms_dbfs = input_level_pwr_to_db(mean_power);
    out->peak_dbfs = input_level_peak_to_dbfs(peak);
    out->clip_pct = (count > 0U) ? ((double)clipped * 100.0 / (double)count) : 0.0;
    out->sample_count = count;
    out->updated = time(NULL);
}

static int
input_level_count_valid(size_t count, size_t step) {
    return count > 0U && step > 0U;
}

void
dsd_input_level_classify(dsd_input_level_snapshot* snapshot, double low_warn_db) {
    if (!snapshot || snapshot->sample_count == 0U || snapshot->source == DSD_INPUT_LEVEL_SOURCE_UNKNOWN) {
        if (snapshot) {
            snapshot->status = DSD_INPUT_LEVEL_UNKNOWN;
        }
        return;
    }
    if (snapshot->clip_pct >= DSD_INPUT_LEVEL_CLIP_PCT) {
        snapshot->status = DSD_INPUT_LEVEL_CLIPPING;
        return;
    }
    if (snapshot->peak_dbfs >= DSD_INPUT_LEVEL_HOT_PEAK_DBFS) {
        snapshot->status = DSD_INPUT_LEVEL_HOT;
        return;
    }
    if (low_warn_db < 0.0 && snapshot->rms_dbfs <= low_warn_db) {
        snapshot->status = DSD_INPUT_LEVEL_LOW;
        return;
    }
    snapshot->status = DSD_INPUT_LEVEL_OK;
}

int
dsd_input_level_metrics_from_pcm_i16(const int16_t* samples, size_t count, size_t step, dsd_input_level_source source,
                                     dsd_input_level_snapshot* out) {
    if (!out || !samples || !input_level_count_valid(count, step)) {
        return -1;
    }
    double p = 0.0;
    double t = 0.0;
    double peak = 0.0;
    uint64_t clipped = 0U;
    uint64_t used = 0U;
    const double scale = 1.0 / 32768.0;
    for (size_t i = 0U; i < count; i += step) {
        const int16_t sample = samples[i];
        const double s = (double)sample * scale;
        const double a = input_level_safe_abs(s);
        if (a > peak) {
            peak = a;
        }
        if (sample <= INT16_MIN || sample >= INT16_MAX) {
            clipped++;
        }
        t += s;
        p += s * s;
        used++;
    }
    double mean_power = 0.0;
    if (used > 0U) {
        mean_power = p - ((t * t) / (double)used);
        if (mean_power < 0.0) {
            mean_power = 0.0;
        }
        mean_power /= (double)used;
    }
    input_level_fill(out, source, mean_power, peak, clipped, used);
    return 0;
}

int
dsd_input_level_metrics_from_pcm_f32_i16_scale(const float* samples, size_t count, size_t step,
                                               dsd_input_level_source source, dsd_input_level_snapshot* out) {
    if (!out || !samples || !input_level_count_valid(count, step)) {
        return -1;
    }
    double p = 0.0;
    double t = 0.0;
    double peak = 0.0;
    uint64_t clipped = 0U;
    uint64_t used = 0U;
    const double scale = 1.0 / 32768.0;
    for (size_t i = 0U; i < count; i += step) {
        double raw = (double)samples[i];
        if (!isfinite(raw)) {
            raw = (raw < 0.0) ? -32768.0 : 32767.0;
        }
        const double s = raw * scale;
        const double a = input_level_safe_abs(s);
        if (a > peak) {
            peak = a;
        }
        if (raw <= -32768.0 || raw >= 32767.0) {
            clipped++;
        }
        t += s;
        p += s * s;
        used++;
    }
    double mean_power = 0.0;
    if (used > 0U) {
        mean_power = p - ((t * t) / (double)used);
        if (mean_power < 0.0) {
            mean_power = 0.0;
        }
        mean_power /= (double)used;
    }
    input_level_fill(out, source, mean_power, peak, clipped, used);
    return 0;
}

int
dsd_input_level_metrics_from_cu8(const uint8_t* samples, size_t count, dsd_input_level_source source,
                                 dsd_input_level_snapshot* out) {
    if (!out || !samples || count == 0U) {
        return -1;
    }
    double p = 0.0;
    double t = 0.0;
    double peak = 0.0;
    uint64_t clipped = 0U;
    const double scale = 1.0 / 127.5;
    for (size_t i = 0U; i < count; i++) {
        const uint8_t sample = samples[i];
        const double s = ((double)sample - 127.5) * scale;
        const double a = fabs(s);
        if (a > peak) {
            peak = a;
        }
        if (sample <= 1U || sample >= 254U) {
            clipped++;
        }
        t += s;
        p += s * s;
    }
    double mean_power = p - ((t * t) / (double)count);
    if (mean_power < 0.0) {
        mean_power = 0.0;
    }
    mean_power /= (double)count;
    input_level_fill(out, source, mean_power, peak, clipped, (uint64_t)count);
    return 0;
}

int
dsd_input_level_metrics_from_cs16(const int16_t* samples, size_t count, dsd_input_level_source source,
                                  dsd_input_level_snapshot* out) {
    if (!out || !samples || count == 0U) {
        return -1;
    }
    double p = 0.0;
    double t = 0.0;
    double peak = 0.0;
    uint64_t clipped = 0U;
    const double scale = 1.0 / 32768.0;
    for (size_t i = 0U; i < count; i++) {
        const int16_t sample = samples[i];
        const double s = (double)sample * scale;
        const double a = input_level_safe_abs(s);
        if (a > peak) {
            peak = a;
        }
        if (sample <= -32760 || sample >= 32760) {
            clipped++;
        }
        t += s;
        p += s * s;
    }
    double mean_power = p - ((t * t) / (double)count);
    if (mean_power < 0.0) {
        mean_power = 0.0;
    }
    mean_power /= (double)count;
    input_level_fill(out, source, mean_power, peak, clipped, (uint64_t)count);
    return 0;
}

int
dsd_input_level_metrics_from_cf32(const float* samples, size_t count, dsd_input_level_source source,
                                  dsd_input_level_snapshot* out) {
    if (!out || !samples || count == 0U) {
        return -1;
    }
    double p = 0.0;
    double t = 0.0;
    double peak = 0.0;
    uint64_t clipped = 0U;
    for (size_t i = 0U; i < count; i++) {
        double s = (double)samples[i];
        if (!isfinite(s)) {
            s = (s < 0.0) ? -1.0 : 1.0;
        }
        const double a = fabs(s);
        if (a > peak) {
            peak = a;
        }
        if (a >= 0.98) {
            clipped++;
        }
        t += s;
        p += s * s;
    }
    double mean_power = p - ((t * t) / (double)count);
    if (mean_power < 0.0) {
        mean_power = 0.0;
    }
    mean_power /= (double)count;
    input_level_fill(out, source, mean_power, peak, clipped, (uint64_t)count);
    return 0;
}

static unsigned int
input_level_notify_bit(dsd_input_level_status status) {
    switch (status) {
        case DSD_INPUT_LEVEL_LOW: return DSD_INPUT_LEVEL_NOTIFY_LOW;
        case DSD_INPUT_LEVEL_HOT: return DSD_INPUT_LEVEL_NOTIFY_HOT;
        case DSD_INPUT_LEVEL_CLIPPING: return DSD_INPUT_LEVEL_NOTIFY_CLIPPING;
        case DSD_INPUT_LEVEL_UNKNOWN:
        case DSD_INPUT_LEVEL_OK:
        default: return 0U;
    }
}

static int
input_level_status_notifies(dsd_input_level_status status) {
    return input_level_notify_bit(status) != 0U;
}

static int
input_level_status_severity(dsd_input_level_status status) {
    switch (status) {
        case DSD_INPUT_LEVEL_LOW: return 1;
        case DSD_INPUT_LEVEL_HOT: return 2;
        case DSD_INPUT_LEVEL_CLIPPING: return 3;
        case DSD_INPUT_LEVEL_UNKNOWN:
        case DSD_INPUT_LEVEL_OK:
        default: return 0;
    }
}

static int
input_level_m17_encoder_active(const dsd_opts* opts) {
    return opts && (opts->m17encoder == 1 || opts->m17encoderbrt == 1 || opts->m17encoderpkt == 1);
}

static int
input_level_is_digital_silence(const dsd_input_level_snapshot* snapshot) {
    return snapshot && snapshot->sample_count > 0U
           && snapshot->rms_dbfs <= DSD_INPUT_LEVEL_SILENCE_FLOOR_DBFS + DSD_INPUT_LEVEL_SILENCE_EPSILON_DB
           && snapshot->peak_dbfs <= DSD_INPUT_LEVEL_SILENCE_FLOOR_DBFS + DSD_INPUT_LEVEL_SILENCE_EPSILON_DB;
}

static int
input_level_should_suppress_tcp_pcm_toast(const dsd_opts* opts, const dsd_state* state,
                                          const dsd_input_level_snapshot* snapshot) {
    if (!opts || !state || !snapshot || opts->audio_in_type != AUDIO_IN_TCP
        || snapshot->source != DSD_INPUT_LEVEL_SOURCE_PCM || input_level_m17_encoder_active(opts)
        || !input_level_status_notifies(snapshot->status)) {
        return 0;
    }

    /* state->carrier can lag immediately after sync loss, so digital silence is also an idle signal. */
    return state->carrier == 0 || (snapshot->status == DSD_INPUT_LEVEL_LOW && input_level_is_digital_silence(snapshot));
}

int
dsd_input_level_format_advisory(const dsd_input_level_snapshot* snapshot, char* out, size_t out_size) {
    if (!snapshot || !out || out_size == 0U) {
        return -1;
    }
    const char* label = dsd_input_level_display_label(snapshot->source);
    const char* status = dsd_input_level_status_label(snapshot->status);
    const char* advice = dsd_input_level_source_is_rf(snapshot->source) ? "lower RF gain or add filtering/attenuation"
                                                                        : "lower source/input volume";
    if (snapshot->status == DSD_INPUT_LEVEL_LOW) {
        advice = dsd_input_level_source_is_rf(snapshot->source) ? "raise RF gain if signal is present"
                                                                : "raise source/input volume if signal is present";
        DSD_SNPRINTF(out, out_size, "%s %s %.1f dBFS: %s", label, status, snapshot->rms_dbfs, advice);
        return 0;
    }
    if (snapshot->status == DSD_INPUT_LEVEL_HOT) {
        DSD_SNPRINTF(out, out_size, "%s %s peak %.1f dBFS: %s", label, status, snapshot->peak_dbfs, advice);
        return 0;
    }
    if (snapshot->status == DSD_INPUT_LEVEL_CLIPPING) {
        DSD_SNPRINTF(out, out_size, "%s %s %.1f%%: %s", label, status, snapshot->clip_pct, advice);
        return 0;
    }
    DSD_SNPRINTF(out, out_size, "%s %s", label, status);
    return 0;
}

static int
input_level_should_notify(const dsd_opts* opts, const dsd_state* state, const dsd_input_level_snapshot* snapshot,
                          unsigned int notify_mask, time_t now) {
    if (!state || !snapshot) {
        return 0;
    }
    unsigned int bit = input_level_notify_bit(snapshot->status);
    if (bit == 0U || (notify_mask & bit) == 0U) {
        return 0;
    }
    int cooldown = opts ? opts->input_warn_cooldown_sec : 10;
    if (cooldown <= 0) {
        cooldown = 10;
    }
    if (state->input_level_last_toast_time == 0) {
        return 1;
    }
    const dsd_input_level_status last_status = state->input_level_last_toast_status;
    const dsd_input_level_source last_source = state->input_level_last_toast_source;
    if (!input_level_status_notifies(last_status) || last_source == DSD_INPUT_LEVEL_SOURCE_UNKNOWN) {
        return 1;
    }
    if ((last_status != snapshot->status || last_source != snapshot->source)
        && input_level_status_severity(snapshot->status) > input_level_status_severity(last_status)) {
        return 1;
    }
    return difftime(now, state->input_level_last_toast_time) >= (double)cooldown;
}

void
dsd_input_level_publish(dsd_opts* opts, dsd_state* state, const dsd_input_level_snapshot* snapshot,
                        unsigned int notify_mask) {
    if (!snapshot) {
        return;
    }

    dsd_input_level_snapshot next = *snapshot;
    double low_warn_db = opts ? opts->input_warn_db : -40.0;
    dsd_input_level_classify(&next, low_warn_db);

    if (opts && next.source == DSD_INPUT_LEVEL_SOURCE_PCM && next.sample_count > 0U && next.rms_dbfs > -200.0) {
        opts->rtl_pwr = input_level_db_to_pwr(next.rms_dbfs);
    }
    if (!state) {
        return;
    }

    time_t now = next.updated != 0 ? next.updated : time(NULL);
    int notify = input_level_should_suppress_tcp_pcm_toast(opts, state, &next)
                     ? 0
                     : input_level_should_notify(opts, state, &next, notify_mask, now);
    state->input_level = next;
    if (!notify) {
        return;
    }

    char msg[sizeof(state->ui_msg)];
    if (dsd_input_level_format_advisory(&next, msg, sizeof(msg)) != 0) {
        return;
    }
    LOG_WARN("WARNING: %s\n", msg);
    DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", msg);
    state->ui_msg_expire = now + DSD_INPUT_LEVEL_TOAST_TTL_SEC;
    state->input_level_last_toast_time = now;
    state->input_level_last_toast_status = next.status;
    state->input_level_last_toast_source = next.source;
    if (opts) {
        opts->last_input_warn_time = now;
    }
}
