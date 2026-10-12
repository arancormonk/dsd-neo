// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <cassert>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stdint.h>
#include <stdio.h>
#include <utility>
#include <vector>
#include "../test_support/qt_test_paths.h"
#include "known_frequency_index.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "signal_search_model.h"

static void
publish(uint64_t id, bool confirmed = false) {
    dsd_signal_search_status s{};
    s.session_id = 17;
    s.revision = id + (confirmed ? 2000 : 0);
    s.phase = DSD_SEARCH_LISTENING;
    auto& h = s.hit;
    h.session_id = 17;
    h.hit_id = id;
    h.frequency_hz = 463552100;
    h.bandwidth_hz = 12500;
    h.power_dbfs = -32.123f;
    h.noise_dbfs = -67.456f;
    h.started_ms = 123456789;
    h.confirmed = confirmed;
    h.unresolved = !confirmed;
    h.gain_tenth_db = 123;
    DSD_SNPRINTF(h.input_spec, sizeof(h.input_spec), "%s", "rtltcp:127.0.0.2:1235:463.55M");
    if (confirmed) {
        DSD_SNPRINTF(h.protocol, sizeof(h.protocol), "%s", "DMR");
        h.source = 123;
    }
    dsd_app_signal_search_publish(&s, &h, nullptr);
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("dsd-search-tests"));
    dsd_test_qt_isolate_paths();
    QTemporaryDir temp;
    assert(temp.isValid());
    const QString journal = temp.filePath(QStringLiteral("history"));
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    publish(1);
    dsd_app_signal_search_store_flush();
    const auto before = QFileInfo(journal).size();
    for (int i = 0; i < 50; ++i) {
        publish(1);
    }
    dsd_app_signal_search_store_flush();
    assert(QFileInfo(journal).size() == before);
    publish(1, true);
    std::vector<dsd_signal_search_hit> hits(1000);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1);
    assert(hits[0].confirmed && hits[0].source == 123);
    // Reattaching to an already open store must not end the current detection.
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1 && !hits[0].ended);
    for (int i = 2; i <= 1005; ++i) {
        publish(i);
    }
    dsd_app_signal_search_permanent_avoid(463550000);
    dsd_app_signal_search_permanent_avoid(463550000);
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_store_close();
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1000);
    assert(hits.front().hit_id == 1005 && hits.back().hit_id == 6 && hits.front().ended);
    uint32_t avoids[4];
    assert(dsd_app_signal_search_avoids_get(avoids, 4) == 1 && avoids[0] == 463550000);
    assert(dsd_app_signal_search_avoid_remove(463550000) == 0);
    dsd_app_signal_search_store_close();
    FILE* f = dsd_fopen_private(journal.toUtf8().constData(), "ab");
    assert(f);
    assert(fwrite("DSS", 1, 3, f) == 3);
    fclose(f);
    assert(dsd_app_signal_search_store_open(journal.toUtf8().constData()) == 0);
    char error[256];
    dsd_app_signal_search_status_get(nullptr, error, sizeof(error));
    assert(error[0]);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1000);
    dsd_app_signal_search_store_close();
    // Failed storage retains the hit, surfaces a failure and retries on an explicit flush.
    const QString missing = temp.filePath(QStringLiteral("missing/history"));
    assert(dsd_app_signal_search_store_open(missing.toUtf8().constData()) == 0);
    publish(1010);
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_status_get(nullptr, error, sizeof(error));
    assert(error[0]);
    assert(dsd_app_signal_search_history_get(hits.data(), hits.size()) == 1);
    assert(QDir().mkpath(QFileInfo(missing).absolutePath()));
    dsd_app_signal_search_store_flush();
    dsd_app_signal_search_status_get(nullptr, error, sizeof(error));
    assert(!error[0]);
    assert(QFileInfo(missing).size() > 0);
    dsd_app_signal_search_store_close();

    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    assert(systems.add(
        {{QStringLiteral("name"), QStringLiteral("Shop")}, {QStringLiteral("freqMhz"), QStringLiteral("463.5500")}}));
    dsd_qt::KnownFrequencyIndex index(&systems, &lists);
    auto matches = index.lookup(463552100, 12500);
    assert(matches.size() == 1 && matches[0].toMap().value(QStringLiteral("name")) == QStringLiteral("Shop"));
    assert(index.lookup(463570000, 12500).isEmpty());
    assert(index.lookup(463555000, 25000.0 / 3).isEmpty());
    assert(systems.add({{QStringLiteral("name"), QStringLiteral("Other shop")},
                        {QStringLiteral("freqMhz"), QStringLiteral("463.5500")}}));
    assert(index.lookup(463552100, 12500).size() == 2);
    assert(systems.add(
        {{QStringLiteral("name"), QStringLiteral("Exact")}, {QStringLiteral("freqMhz"), QStringLiteral("463.5521")}}));
    assert(index.lookup(463552100, 12500).size() == 1);
    const QString channels = temp.filePath(QStringLiteral("channels.csv"));
    f = dsd_fopen_private(channels.toUtf8().constData(), "wb");
    assert(f);
    const char csv[] = "Channel,Frequency\n1,464000000\n2,464025000\n";
    assert(fwrite(csv, 1, sizeof(csv) - 1, f) == sizeof(csv) - 1);
    fclose(f);
    assert(
        systems.add({{QStringLiteral("name"), QStringLiteral("Mapped")}, {QStringLiteral("chanCsvPath"), channels}}));
    assert(index.lookup(464000500, 12500).size() == 1);
    const QString targets = temp.filePath(QStringLiteral("targets.csv"));
    f = dsd_fopen_private(targets.toUtf8().constData(), "wb");
    assert(f);
    const char targetCsv[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                             "Tower,am-conventional,118300000,,,,,--am-bandwidth-hz 6000\n"
                             "Trunk,dmr-trunk,464100000,channels.csv,,,,\n";
    assert(fwrite(targetCsv, 1, sizeof(targetCsv) - 1, f) == sizeof(targetCsv) - 1);
    fclose(f);
    assert(lists.add({{QStringLiteral("name"), QStringLiteral("Imported list")},
                      {QStringLiteral("targetSource"), QStringLiteral("csv")},
                      {QStringLiteral("targetsCsvPath"), targets}}));
    assert(index.lookup(118302100, 12500).size() == 1);
    assert(index.lookup(118304000, 12500).isEmpty());
    assert(index.lookup(464100000, 12500).size() == 1);
    assert(index.lookup(464000000, 12500).size() == 2); // saved map and nested target map
    // Cached lookup keeps working without any rendering-time file reads.
    QFile::remove(channels);
    assert(index.lookup(464000500, 12500).size() == 2);

    dsd_qt::SignalSearchModel model(nullptr, &index);
    auto range = model.defaultRange();
    range[QStringLiteral("stepNum")] = 25000;
    range[QStringLiteral("stepDen")] = 3;
    range[QStringLiteral("startMHz")] = QStringLiteral("118.000000");
    range[QStringLiteral("endMHz")] = QStringLiteral("118.100000");
    range[QStringLiteral("am")] = true;
    range[QStringLiteral("digital")] = 0;
    range[QStringLiteral("bandwidth")] = 6000;
    range[QStringLiteral("squelchMode")] = 1;
    assert(model.saveRange(0, range).isEmpty());
    assert(model.ranges().front().toMap().value(QStringLiteral("stepDen")).toInt() == 3);
    auto bad = range;
    bad[QStringLiteral("stepDen")] = 0;
    assert(!model.saveRange(0, bad).isEmpty());
    bad = range;
    bad[QStringLiteral("endMHz")] = QStringLiteral("nan");
    assert(!model.saveRange(0, bad).isEmpty());
    publish(77);
    model.refresh();
    const auto frozen = model.history().front().toMap();
    publish(78, true);
    model.refresh();
    const auto saved = model.systemForHit(frozen, {{QStringLiteral("sourceType"), QStringLiteral("usb")}});
    assert(saved.value(QStringLiteral("freqMhz")) == QStringLiteral("463.552100"));
    assert(saved.value(QStringLiteral("sourceType")) == QStringLiteral("rtltcp"));
    assert(saved.value(QStringLiteral("host")) == QStringLiteral("127.0.0.2"));
    assert(saved.value(QStringLiteral("port")).toInt() == 1235);
    assert(saved.value(QStringLiteral("decodeFlag")) == QStringLiteral("-fA"));
    assert(saved.value(QStringLiteral("searchUnresolved")).toBool());
    const auto confirmed = model.systemForHit(model.history().front().toMap(), {});
    assert(confirmed.value(QStringLiteral("decodeFlag")) == QStringLiteral("-fs"));
    auto nxdn = model.history().front().toMap();
    nxdn[QStringLiteral("protocol")] = QStringLiteral("NXDN48");
    assert(model.systemForHit(nxdn, {}).value(QStringLiteral("decodeFlag")) == QStringLiteral("-fi"));
    nxdn[QStringLiteral("protocol")] = QStringLiteral("NXDN96");
    assert(model.systemForHit(nxdn, {}).value(QStringLiteral("decodeFlag")) == QStringLiteral("-fn"));
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
