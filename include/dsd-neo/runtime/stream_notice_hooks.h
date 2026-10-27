// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_RUNTIME_STREAM_NOTICE_HOOKS_H
#define DSD_NEO_RUNTIME_STREAM_NOTICE_HOOKS_H
#ifdef __cplusplus
extern "C" {
#endif
/** Something the input stream got over by itself that the user should hear about, with no failure behind it: the
 * session keeps running and nothing is latched in dsd_input_failure. */
typedef enum {
    DSD_STREAM_NOTICE_NONE = 0,
    /** A stalled RTL-SDR was reset and reopened under the running stream. */
    DSD_STREAM_NOTICE_USB_RECOVERED = 1
} dsd_stream_notice_kind;

typedef void (*dsd_stream_notice_fn)(int kind);

/** Install the notice sink (app-control at session start; NULL at its end). Thread-safe. Once it returned, no post
 * delivers to the sink it replaced: a post in progress finishes first. */
void dsd_runtime_stream_notice_set(dsd_stream_notice_fn fn);
/** Post a notice from any thread (the stream's controller). Without a sink installed it is dropped. The sink runs under
 * the module's lock: it must return promptly and must not call dsd_runtime_stream_notice_set() or _post(). */
void dsd_runtime_stream_notice_post(int kind);
#ifdef __cplusplus
}
#endif
#endif
