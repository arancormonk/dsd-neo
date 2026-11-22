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

/**
 * @brief The target CSV for @p list's entries over saved @p systems.
 *
 * Issue #899 (B13): each row's squelch is one its row type takes, so a saved setting never refuses the list. A saved
 * system's own squelch goes to its row as the row runs it: noise on an AM row as auto with the same margin (as the
 * engine runs it there), a dynamic one on a digital row as off (there it ran as off on its own, and a session level
 * from the extra options must not gate the row either), a level in whole dB, -120..-1, rounded with a warning when it
 * is not a whole dB. A row with no squelch of its own carries none: it inherits the session's, and on an analog row the
 * decoder runs the app's default there (--squelch-analog-default) while nothing sets a squelch.
 */
ScanListTargets scan_list_targets(const QVariantMap& list, const QVariantList& systems);
QString scan_list_settings_error(const QVariantMap& list);
/** Issue #526: the analog demodulator (dsd_analog_demod) a scan entry runs -- a frequency entry's nfm or am protocol,
 * a saved system whose decode flags are exactly -fA or -fM, the only analog flags a scan list maps -- or -1 for a
 * digital entry. @p systemDecodeFlag is the saved system's decodeFlag for a system entry, ignored otherwise. An analog
 * entry carries no keys, talkgroups, decryption profile or modulation. */
int scan_list_entry_analog_kind(const QVariantMap& entry, const QString& systemDecodeFlag);
} // namespace dsd_qt
#endif
