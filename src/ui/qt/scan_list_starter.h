// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SCAN_LIST_STARTER_H
#define DSD_NEO_UI_QT_SCAN_LIST_STARTER_H
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant> // IWYU pragma: keep -- QVariantList's element; IWYU's answer differs by TU.
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <functional>
#include <utility>
Q_MOC_INCLUDE("decoder_host.h")

namespace dsd_qt {
class AppPrefs;
class SavedSystemsModel;
class DecryptionProfileProvider;
class DecoderHost;
class RecordingSessions;

/**
 * @brief The file name, without ".csv", a manual scan list's generated targets are written under (issue #697): the
 * list's display name @p name with path separators, control characters and the characters Android or Windows refuse
 * in a file name each made "_", blanks around it and trailing dots dropped, a Windows device name given a "_", and
 * cut to the 31 UTF-8 bytes the engine names a list by, whole characters only. "list N" (@p listNumber) when nothing is
 * left. The engine names the list after it, so the decoder's toasts, logs and API say the list's name.
 */
QString scan_list_csv_stem(const QString& name, int listNumber);
/**
 * @brief Where list @p uid's generated targets go: scan_lists/<uid>/<scan_list_csv_stem()>.csv in the app's data
 * folder. The folder is the list's identity; nothing keys on the file's path.
 */
QString scan_list_csv_path(const QString& uid, const QString& name, int listNumber);

#ifdef DSD_NEO_TEST_HOOKS
/** @brief Call @p hook with an imported list's copy once it is written, checked and its folder cleaned, before the
 *  starter makes sure it is there: a test removes it, as a copy written under another name is removed. */
void scan_list_set_copy_written_hook_for_test(std::function<void(const QString&)> hook);
/** @brief How many times ScanListStarter::hasTetraTargets() has read an imported target CSV since the process began. */
int scan_list_tetra_csv_reads_for_test();
#endif

class ScanListStarter : public QObject {
    Q_OBJECT
    /** @brief The most lists one session scans (DSD_TRUNK_SCAN_MAX_LISTS, issue #697). */
    Q_PROPERTY(int maxLists READ maxLists CONSTANT)
    /** @brief Why a draft list does not start; a start refuses a draft with it, and so does a pinned draft before any
     *  start is tried (issue #801). */
    Q_PROPERTY(QString draftRefusal READ draftRefusal CONSTANT)
  public:
    explicit ScanListStarter(const AppPrefs* prefs, const SavedSystemsModel* systems, QObject* parent = nullptr);
    QVariantMap build(const QVariantMap& list) const;
    Q_INVOKABLE QVariantMap start(const QVariantMap& list, DecoderHost* host) const;
    Q_INVOKABLE QVariantMap validate(const QVariantMap& list) const;
    /**
     * @brief Start one session that scans every list of @p lists (issue #697): the first list's receiver and session
     * options, one --trunk-scan per list in that order (list 1 is the first), one call recording session, the first
     * list's. Nothing starts unless every list passes its own checks, joins the first (joinConflict()), builds, and the
     * lists together pass the engine's checks across them (a target id in two lists has the same settings in both,
     * and is one target). One list is start().
     * @p holdTargetId, when not empty, starts the scan held on that target of the first list (issue #644,
     * session_args_with_scan_hold()), for this start only: nothing here keeps it.
     * @return start()'s map; "error" names the list it is about.
     */
    Q_INVOKABLE QVariantMap startLists(const QVariantList& lists, DecoderHost* host,
                                       const QString& holdTargetId = QString()) const;
    /**
     * @brief The target CSV the last start of @p list ran (issue #644): the file its start wrote in the list's own
     * folder (scan_list_csv_path()'s, which holds only that one), else the imported file it ran from when no copy was
     * made; empty for neither. A saved edit changes neither: they are what the running session scans.
     */
    Q_INVOKABLE QString sessionCsvPath(const QVariantMap& list) const;
    /**
     * @brief Why @p other cannot scan in one session with @p primary and the lists already switched on, @p chosen
     * (the Scan with other lists sheet's), or empty; see session_args_scan_join_conflict(). They are judged as the one
     * session the start would run (issue #890): App default is 72 kHz when any of them has a TETRA DMO target, so
     * @p other is named for a bandwidth that differs from @p primary's, its own or one a chosen list's would set, and
     * "TETRA DMO needs 72 kHz" when the session's bandwidth cannot decode the TETRA DMO targets
     * (session_args_scan_list_rate_error()). The chosen lists counted are those that stay without @p other: one that
     * cannot join the primary in the session the other chosen lists make (a list fixed at 72 kHz once the TETRA DMO
     * list is switched off) leaves the choice, as the sheet drops it, and names no reason for @p other.
     */
    Q_INVOKABLE QString joinConflict(const QVariantMap& primary, const QVariantMap& other,
                                     const QVariantList& chosen = QVariantList()) const;
    /**
     * @brief Why @p list cannot start for its TETRA DMO targets (session_args_scan_list_rate_error(), the start's own
     * refusal), or empty: the editor's note under the Bandwidth choice.
     */
    Q_INVOKABLE QString rateError(const QVariantMap& list) const;
    /**
     * @brief Whether @p list has a target the engine can visit that is TETRA DMO (issue #890, D3): an enabled entry
     * (a disabled one is left out of the CSV it builds) whose protocol is TETRA DMO, a frequency entry's or a saved
     * TETRA system's, or a tetra-dmo-conventional row of its imported target CSV.
     */
    Q_INVOKABLE bool hasTetraTargets(const QVariantMap& list) const;
    /**
     * @brief The DSP bandwidth, in kHz, @p list scans at on its own (session_args_scan_list_bandwidth_khz()): its own,
     * else 72 kHz with a TETRA DMO target (hasTetraTargets()), else the app's. The editor's App default note.
     */
    Q_INVOKABLE int bandwidthKhz(const QVariantMap& list) const;
    /**
     * @brief Forget what hasTetraTargets() learnt of each imported target CSV (issue #890), so the next ask reads the
     * file again: the imported files changed (one replaced or removed).
     */
    void forgetTargetFiles();
    static int maxLists();
    static QString draftRefusal();

    void
    setTargetFileLookup(std::function<bool(const QString&)> lookup) {
        m_targetFileLookup = std::move(lookup);
    }

    void
    setDecryptionProfiles(DecryptionProfileProvider* profiles) {
        m_profiles = profiles;
    }

    /** @brief Where start() asks for the list's call recording folder (issue #708); nullptr: nowhere. */
    void
    setRecordingSessions(RecordingSessions* sessions) {
        m_recordingSessions = sessions;
    }

  private:
    QVariantList resolveSystems(const QVariantMap& list, QString* error) const;
    bool resolveEntryProfiles(QVariantMap& prepared, QString* error) const;
    /* @p recordingFolder / @p recordingUnavailable: SessionArgPrefs::recordingFolder / recordingUnavailable. */
    /* @p companionPaths: the target CSVs of the other lists a session started from @p list scans (issue #697), each
       one more --trunk-scan after the list's own. The result's "csvPath" is the list's own CSV. */
    /* @p listNumber: the list's number in the session, which names its CSV when its own name gives nothing. */
    /* @p tetraTargets: whether the session it starts has a TETRA DMO target in any list (sessionHasTetra()), so App
       default runs at 72 kHz and another explicit bandwidth is refused (issue #890, D3). */
    QVariantMap prepare(const QVariantMap& list, bool materialize, bool tetraTargets,
                        const QString& recordingFolder = QString(), bool recordingUnavailable = false,
                        const QStringList& companionPaths = QStringList(), int listNumber = 1) const;
    QVariantMap prepareCsv(const QVariantMap& list, bool materialize, bool tetraTargets, const QString& recordingFolder,
                           bool recordingUnavailable, const QStringList& companionPaths, int listNumber) const;
    /* Whether any list of @p lists has a TETRA DMO target (hasTetraTargets()): the whole session's rate follows it. */
    bool sessionHasTetra(const QVariantList& lists) const;
    /* Issue #697: an imported target CSV copied to the list's own file (scan_list_csv_path()), its companions still
       resolving, so the decoder names the list after it; @p source itself when no copy can be made. */
    static QString listCopyOf(const QVariantMap& list, const QString& source, int listNumber);
    /* start()'s prepare: with the list's call recording folder when @p host can record it (@p recording set, the
       folder's session begun). */
    QVariantMap prepareForStart(const QVariantMap& list, const DecoderHost* host, bool* recording,
                                const QStringList& companionPaths, bool tetraTargets) const;
    /* Start the first of @p lists with @p companionPaths, the others' CSVs, built already; held on @p holdTargetId
       when it is not empty. */
    QVariantMap startPrepared(const QVariantList& lists, DecoderHost* host, const QStringList& companionPaths,
                              const QString& holdTargetId = QString()) const;
    /* Hold the decryption profiles @p list's entries use for the session. */
    void retainProfiles(const QVariantMap& list) const;
    /* startLists()' checks before anything is written: each list's own, then whether it joins the first. Their
       warnings, each named by its list, go to @p warnings. */
    QString checkLists(const QVariantList& lists, bool tetraTargets, QStringList* warnings) const;
    /* Write each list's CSV (@p paths, in order) and check them together; @p total is their target count. */
    QString materializeLists(const QVariantList& lists, bool tetraTargets, QStringList* paths, int* total) const;
    /* The recording session prepareForStart() began ends when the start did not happen and no session runs. */
    void endUnstartedRecording(bool recording, bool started, const DecoderHost* host) const;
    /* Whether the imported target CSV at @p path has a tetra-dmo-conventional row (hasTetraTargets()), read once per
       version of the file. */
    bool csvHasTetraTargets(const QString& path) const;

    /* An imported target CSV hasTetraTargets() read: the file as it was then (its size and times) and its answer. The
       Scan with other lists sheet asks for every list at each change, and a read parses the whole file; a file whose
       size or times differ, or forgetTargetFiles(), reads it again. UI thread only, as every caller is. */
    struct CsvTetraVerdict {
        qint64 size = -1;
        qint64 modifiedMs = 0;
        qint64 changedMs = 0;
        qint64 bornMs = 0;
        bool tetra = false;
    };

    mutable QHash<QString, CsvTetraVerdict> m_csvTetra;
    std::function<bool(const QString&)> m_targetFileLookup;
    const AppPrefs* m_prefs;
    const SavedSystemsModel* m_systems;
    DecryptionProfileProvider* m_profiles = nullptr;
    RecordingSessions* m_recordingSessions = nullptr;
};
} // namespace dsd_qt
#endif
