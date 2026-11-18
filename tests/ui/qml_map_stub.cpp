// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Canned positions and map tiles for the QML suite (see qml_map_stub.h).
 */

#include "qml_map_stub.h"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QIODevice>
#include <QImage>
#include <atomic>
#include <cstdlib>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <mutex>
#include <vector>

namespace dsd_neo_qml_stub {

namespace {

std::mutex g_mutex;
std::vector<dsd_position_fix> g_fixes;
uint64_t g_stream = 1;
qint64 g_wall = kMapWallSeconds;
std::atomic<int> g_tile_requests{0};

char*
dup_bytes(const QByteArray& bytes, size_t* len) {
    *len = static_cast<size_t>(bytes.size());
    char* out = static_cast<char*>(malloc(*len + 1U));
    if (out != nullptr) {
        DSD_MEMCPY(out, bytes.constData(), *len);
        out[*len] = '\0';
    }
    return out;
}

/* A plain white tile: toned, it is exactly the map's paper colour, so a case can find it on screen. */
QByteArray
canned_tile_png() {
    QImage image(256, 256, QImage::Format_RGB32);
    image.fill(Qt::white);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

/* Runs on the worker's thread: every request gets the canned tile, never stored (no-store), so each view asks again. */
int
canned_perform(void* /*ctx*/, const dsd_tile_fetch_request* /*req*/, dsd_tile_fetch_exchange* ex) {
    static const QByteArray body = canned_tile_png();
    g_tile_requests.fetch_add(1);
    const QByteArray headers = "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\n\r\n";
    ex->http_status = 200;
    ex->headers = dup_bytes(headers, &ex->headers_len);
    size_t len = 0;
    ex->body = reinterpret_cast<unsigned char*>(dup_bytes(body, &len));
    ex->body_len = len;
    return 0;
}

/* Static: the transport must outlive every worker thread, not only the worker. */
const dsd_tile_fetch_transport kCannedTransport{canned_perform, nullptr};

dsd_qt::MapTileWorker::Options
canned_options() {
    dsd_qt::MapTileWorker::Options options;
    options.transport = &kCannedTransport;
    return options;
}

} // namespace

void
map_publish_fix(const dsd_position_fix& fix) {
    std::lock_guard<std::mutex> lock(g_mutex);
    dsd_position_fix f = fix;
    f.stream_id = g_stream;
    f.seq = static_cast<uint64_t>(g_fixes.size()) + 1U;
    g_fixes.push_back(f);
}

void
map_reset_fixes() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fixes.clear();
    g_stream++;
}

int
map_position_source(uint64_t stream_id, uint64_t after_seq, dsd_position_fix* out, size_t cap,
                    dsd_app_position_cursor* next) {
    if (next == nullptr || (out == nullptr && cap > 0U)) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    DSD_MEMSET(next, 0, sizeof(*next));
    next->stream_id = g_stream;
    next->sample.stream_id = g_stream;
    next->sample.clock_source = static_cast<uint8_t>(DSD_DECODE_CLOCK_SYSTEM);
    next->sample.running = 1U;
    next->sample.decode_now_s = g_wall;
    uint64_t after = after_seq;
    if (stream_id != 0U && stream_id != g_stream) {
        next->reset = 1U;
        after = 0U;
    }
    const auto held = static_cast<uint64_t>(g_fixes.size());
    next->oldest_seq = held > 0U ? 1U : 0U;
    next->newest_seq = held;
    size_t copied = 0;
    for (uint64_t seq = after + 1U; seq <= held && copied < cap; seq++, copied++) {
        out[copied] = g_fixes[static_cast<size_t>(seq - 1U)];
    }
    next->last_seq = after + copied;
    next->more = (next->last_seq < held) ? 1U : 0U;
    return static_cast<int>(copied);
}

qint64
map_wall_seconds() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_wall;
}

void
set_map_wall_seconds(qint64 seconds) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_wall = seconds;
}

int
map_tile_requests() {
    return g_tile_requests.load();
}

MapTileSourceStub::MapTileSourceStub(QObject* parent) : dsd_qt::MapTileWorker(canned_options(), parent) {}

} // namespace dsd_neo_qml_stub
