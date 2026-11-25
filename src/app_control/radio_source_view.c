// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/radio_source_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_net_spec.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <stddef.h>
#include <string.h>

int
dsd_app_iq_stream_describe(const char* spec, char* endpoint, size_t endpoint_size, char* detail, size_t detail_size) {
    if (!endpoint || endpoint_size == 0U || !detail || detail_size == 0U) {
        return -1;
    }
    endpoint[0] = '\0';
    detail[0] = '\0';
    dsd_iq_net_spec parsed;
    char rate[DSD_ANALOG_WIDTH_TEXT_MAX];
    if (!spec || dsd_iq_net_spec_parse(spec, &parsed, NULL, 0U) != 0
        || dsd_analog_width_format((int)parsed.rate_hz, rate, sizeof rate) != 0) {
        return -1;
    }
    if (parsed.kind == DSD_IQ_NET_KIND_TCP) {
        DSD_SNPRINTF(endpoint, endpoint_size, "%s:%d", parsed.host, parsed.port);
    } else if (strcmp(parsed.host, "0.0.0.0") == 0) {
        DSD_SNPRINTF(endpoint, endpoint_size, "UDP :%d", parsed.port);
    } else {
        DSD_SNPRINTF(endpoint, endpoint_size, "UDP %s:%d", parsed.host, parsed.port);
    }
    DSD_SNPRINTF(detail, detail_size, "%s, %s", dsd_iq_net_format_name(parsed.format), rate);
    return 0;
}

/* "I/Q stream ENDPOINT (DETAIL)", the endpoint cut to leave room for the detail. */
static void
radio_source_iq_stream_name(const char* dev, char* out, size_t out_size) {
    char endpoint[DSD_IQ_NET_SPEC_TEXT_MAX];
    char detail[64];
    if (dsd_app_iq_stream_describe(dev, endpoint, sizeof endpoint, detail, sizeof detail) != 0) {
        DSD_SNPRINTF(out, out_size, "I/Q stream");
        return;
    }
    const size_t fixed = strlen("I/Q stream  ()") + strlen(detail);
    const size_t room = out_size > fixed + 1U ? out_size - fixed - 1U : 0U;
    if (strlen(endpoint) <= room) {
        DSD_SNPRINTF(out, out_size, "I/Q stream %s (%s)", endpoint, detail);
    } else {
        /* Cut, and marked so, ASCII as every label is. */
        DSD_SNPRINTF(out, out_size, "I/Q stream %.*s... (%s)", room > 3U ? (int)(room - 3U) : 0, endpoint, detail);
    }
}

/* "I/Q replay FILE": the capture's file name, without its folders. */
static void
radio_source_replay_name(const char* dev, char* out, size_t out_size) {
    const char* path = strchr(dev, ':') ? strchr(dev, ':') + 1 : "";
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
    const char* file = slash ? slash + 1 : path;
    if (file[0] == '\0') {
        DSD_SNPRINTF(out, out_size, "I/Q replay");
        return;
    }
    DSD_SNPRINTF(out, out_size, "I/Q replay %s", file);
}

/* "rtl_tcp HOST:PORT": the running server's, else the spec's own first two fields (rtltcp:HOST:PORT:...). */
static void
radio_source_rtltcp_name(const dsd_opts* opts, int running, const char* dev, char* out, size_t out_size) {
    if (running && opts->rtltcp_hostname[0] != '\0') {
        DSD_SNPRINTF(out, out_size, "rtl_tcp %s:%d", opts->rtltcp_hostname, opts->rtltcp_portno);
        return;
    }
    const char* host = strchr(dev, ':');
    if (!host || host[1] == '\0' || host[1] == ':') {
        DSD_SNPRINTF(out, out_size, "rtl_tcp");
        return;
    }
    host++;
    const char* host_end = strchr(host, ':');
    const int host_len = host_end ? (int)(host_end - host) : (int)strlen(host);
    const char* port = host_end ? host_end + 1 : "";
    const char* port_end = strchr(port, ':');
    const int port_len = port_end ? (int)(port_end - port) : (int)strlen(port);
    if (port_len > 0) {
        DSD_SNPRINTF(out, out_size, "rtl_tcp %.*s:%.*s", host_len, host, port_len, port);
    } else {
        DSD_SNPRINTF(out, out_size, "rtl_tcp %.*s", host_len, host);
    }
}

/* A local RTL-SDR: the one running by its USB serial when the input names it by one (as an Airspy is named), else by
   its index; one not running is "RTL-SDR". */
static void
radio_source_rtl_name(const dsd_opts* opts, int running, char* out, size_t out_size) {
    if (running && opts->rtl_dev_serial[0] != '\0') {
        DSD_SNPRINTF(out, out_size, "RTL-SDR SN %s", opts->rtl_dev_serial);
    } else if (running) {
        DSD_SNPRINTF(out, out_size, "RTL-SDR dev %d", opts->rtl_dev_index);
    } else {
        DSD_SNPRINTF(out, out_size, "RTL-SDR");
    }
}

void
dsd_app_radio_source_name(const dsd_opts* opts, const char* dev, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return;
    }
    if (!dev) {
        dev = "";
    }
    const int running = opts && strcmp(dev, opts->audio_in_dev) == 0;
    if (dsd_opts_audio_in_dev_is_iq_stream_spec(dev)) {
        radio_source_iq_stream_name(dev, out, out_size);
    } else if (dsd_opts_audio_in_dev_is_iqreplay_spec(dev)) {
        radio_source_replay_name(dev, out, out_size);
    } else if (dsd_opts_audio_in_dev_is_rtltcp_spec(dev)) {
        radio_source_rtltcp_name(opts, running, dev, out, out_size);
    } else if (dsd_opts_audio_in_dev_is_airspy_spec(dev)) {
        if (running && opts->airspy_info.serial[0] != '\0') {
            DSD_SNPRINTF(out, out_size, "Airspy %s", opts->airspy_info.serial);
        } else {
            DSD_SNPRINTF(out, out_size, "Airspy");
        }
    } else if (dsd_opts_audio_in_dev_is_soapy_spec(dev)) {
        if (strncmp(dev, "soapy:", 6) == 0 && dev[6] != '\0') {
            DSD_SNPRINTF(out, out_size, "SoapySDR [%s]", dev + 6);
        } else {
            DSD_SNPRINTF(out, out_size, "SoapySDR");
        }
    } else {
        radio_source_rtl_name(opts, running, out, out_size);
    }
}

void
dsd_app_radio_row_destination(const dsd_opts* opts, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return;
    }
    const char* dev = "rtl";
    if (opts && dsd_opts_audio_in_dev_is_radio_spec(opts->audio_in_dev)
        && !dsd_opts_audio_in_dev_is_airspy_spec(opts->audio_in_dev)) {
        dev = opts->audio_in_dev;
    } else if (opts && dsd_opts_audio_in_dev_is_radio_spec(opts->radio_in_dev)
               && !dsd_opts_audio_in_dev_is_airspy_spec(opts->radio_in_dev)) {
        dev = opts->radio_in_dev;
    }
    DSD_SNPRINTF(out, out_size, "%s", dev);
}
