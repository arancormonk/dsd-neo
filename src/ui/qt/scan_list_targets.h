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
/** The width option of analog demodulator @p kind (dsd_analog_demod): --am-bandwidth-hz for AM, else
 * --nfm-bandwidth-hz, as the CLI and a target's options column spell it. */
QString scan_list_width_option_name(int kind);

/** One occurrence of a width option among Extra arguments (scan_list_width_options()): @p tokens it spans, 2 for
 * `--name N` and 1 for `--name=N` (or a `--name` with nothing after it), and its value, @p hz, when that is whole Hz
 * (digits only), else 0. */
struct ScanListWidthOption {
    int hz = 0;
    qsizetype tokens = 0;
};

/** Issue #526/#890: every occurrence, in order, of @p kind's width option (scan_list_width_option_name()) in
 * @p extraArgs, split at blanks as a session splits its Extra arguments; @p tokenCount (when not NULL) receives the
 * number of tokens. Which occurrence counts is the caller's: the configured width is the last one, as on the command
 * line; a saved system's own width is its only token (scan_list_saved_system_width_hz()). */
QList<ScanListWidthOption> scan_list_width_options(const QString& extraArgs, int kind, qsizetype* tokenCount);
/** Issue #890: the channel width in Hz a saved analog system of demodulator @p kind (dsd_analog_demod) carries into a
 * manual scan list, from its Extra arguments @p extraArgs, which must be exactly that kind's one width option
 * (--nfm-bandwidth-hz N for NFM, --am-bandwidth-hz N for AM, or the --name=N form) in whole Hz within the range a
 * target's options column takes (dsd_scan_options_parse()). 0 for anything else, which a manual list refuses. A Signal
 * search save keeps its measured width this way. */
int scan_list_saved_system_width_hz(const QString& extraArgs, int kind);
} // namespace dsd_qt
#endif
