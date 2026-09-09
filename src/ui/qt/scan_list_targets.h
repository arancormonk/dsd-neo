// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SCAN_LIST_TARGETS_H
#define DSD_NEO_UI_QT_SCAN_LIST_TARGETS_H
#include <QByteArray>
#include <QStringList>
#include <QVariantMap>

namespace dsd_qt {
// No I/O or host dependency. QString copies cannot promise erasure; never display
// CSV/options or key-bearing maps. Owned UTF-8 CSV storage is erased on disposal.
struct ScanListTargets {
    ScanListTargets() = default;
    ScanListTargets(const ScanListTargets&) = delete;
    ScanListTargets& operator=(const ScanListTargets&) = delete;
    ScanListTargets(ScanListTargets&&) = default;
    ScanListTargets& operator=(ScanListTargets&&) = delete;
    bool ok = false;
    QByteArray csv;
    QString error;
    QStringList warnings;
    QStringList paths;
    QString firstFreqMhz;
    int targetCount = 0;

    ~ScanListTargets() {
        volatile char* p = csv.data();
        for (qsizetype i = 0; i < csv.size(); ++i) {
            p[i] = 0;
        }
    }
};

ScanListTargets scan_list_targets(const QVariantMap& list, const QVariantList& systems);
} // namespace dsd_qt
#endif
