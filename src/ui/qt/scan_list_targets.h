// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SCAN_LIST_TARGETS_H
#define DSD_NEO_UI_QT_SCAN_LIST_TARGETS_H
#include <QByteArray>
#include <QHash>
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
/** Issue #903: the saved system's uid behind each target id of scan list @p list, as the list's generated target file
 * names it, read in one pass over the entries: an id's first entry names its owner when it is an enabled saved-system
 * entry of a list of manual entries, and empty when it is a frequency entry or a disabled one. An id no entry has is
 * absent, and a list that runs an imported target file gives none. */
QHash<QString, QString> scan_list_entry_owners(const QVariantMap& list);
struct ScanProtocol;
/** Issue #890: the catalog protocol (scan_protocol_catalog.h) a scan entry runs -- a frequency entry's protocol id, a
 * saved system's decode flags @p systemDecodeFlag (scan_protocol_for_flag()) -- or nullptr for one a scan list cannot
 * map, which it refuses. */
const ScanProtocol* scan_list_entry_protocol(const QVariantMap& entry, const QString& systemDecodeFlag);
/** Issue #890: whether a scan entry's own decryption choice (inherit, a profile, no keys) applies to it: the editor
 * offers it only for a protocol whose row takes keys (P25, DMR, NXDN, and dPMR's scrambler), and hides it for one with
 * none of its own (YSF, D-STAR, M17, TETRA DMO, NFM, AM), whose stored choice is kept but never resolved. True for an
 * entry no protocol maps, which the list refuses anyway. */
bool scan_list_entry_takes_keys(const QVariantMap& entry, const QString& systemDecodeFlag);
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
