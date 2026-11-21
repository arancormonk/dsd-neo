// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SCAN_LIST_TARGETS_H
#define DSD_NEO_UI_QT_SCAN_LIST_TARGETS_H
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

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
QString scan_list_settings_error(const QVariantMap& list);
/** Issue #526: the analog demodulator (dsd_analog_demod) a scan entry runs -- a frequency entry's nfm or am protocol,
 * a saved system whose decode flags are exactly -fA or -fM, the only analog flags a scan list maps -- or -1 for a
 * digital entry. @p systemDecodeFlag is the saved system's decodeFlag for a system entry, ignored otherwise. An analog
 * entry carries no keys, talkgroups, decryption profile or modulation. */
int scan_list_entry_analog_kind(const QVariantMap& entry, const QString& systemDecodeFlag);
/** Issue #903: the saved system's uid behind target @p id of scan list @p list, as the list's generated target file
 * names it: an enabled saved-system entry of a list of manual entries whose uid is @p id. Empty for a frequency entry,
 * a disabled or missing one, and every target of a list that runs an imported target file. */
QString scan_list_entry_owner(const QVariantMap& list, const QString& id);
} // namespace dsd_qt
#endif
