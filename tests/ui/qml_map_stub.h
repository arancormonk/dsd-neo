// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Canned positions and map tiles for the QML suite (issue #800).
 *
 * The QML suite links no decoder and must never reach a network, so the two
 * sources the map reads are stubbed (qml_map_stub.cpp): a published position
 * stream the production PositionModel reads through its source seam, with a
 * wall clock the case sets, and a tile transport that answers every request
 * with one canned tile and counts the requests. MapTileSourceStub is what the
 * suite registers as MapTileSource: the production worker on that transport,
 * so tiles stay off until a case turns them on, exactly as in the app.
 */

#ifndef DSD_NEO_TESTS_UI_QML_MAP_STUB_H_
#define DSD_NEO_TESTS_UI_QML_MAP_STUB_H_

#include <QObject>
#include <QtGlobal>
#include <cstddef>
#include <cstdint>
#include <dsd-neo/app_control/position_view.h>
#include <dsd-neo/core/position.h>

#include "map_tile_worker.h"

namespace dsd_neo_qml_stub {

/** @brief The wall clock the canned positions age against, Unix seconds. */
constexpr qint64 kMapWallSeconds = 1700000000;

/** @brief Publish @p fix on the canned stream (stream and seq are assigned). */
void map_publish_fix(const dsd_position_fix& fix);
/** @brief Start a new canned stream with nothing published (a new run). */
void map_reset_fixes();
/** @brief The canned stream, as dsd_app_position_copy_since() reads the published one. */
int map_position_source(uint64_t stream_id, uint64_t after_seq, dsd_position_fix* out, size_t cap,
                        dsd_app_position_cursor* next);
/** @brief The canned wall clock. */
qint64 map_wall_seconds();
void set_map_wall_seconds(qint64 seconds);

/** @brief Requests the canned tile transport has answered so far. */
int map_tile_requests();
/** @brief Whether the canned transport answers 503 (a server in trouble) instead of the canned tile. */
void set_map_tiles_failing(bool failing);
/** @brief Whether the canned transport answers 200 with a body that is not a PNG (a JPEG tile server). */
void set_map_tiles_not_png(bool notPng);

/** @brief The production tile worker on the canned transport: registered as MapTileSource in the QML suite. */
class MapTileSourceStub : public dsd_qt::MapTileWorker {
    Q_OBJECT

  public:
    explicit MapTileSourceStub(QObject* parent = nullptr);
};

} // namespace dsd_neo_qml_stub

#endif /* DSD_NEO_TESTS_UI_QML_MAP_STUB_H_ */
