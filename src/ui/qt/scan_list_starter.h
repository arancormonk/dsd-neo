// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SCAN_LIST_STARTER_H
#define DSD_NEO_UI_QT_SCAN_LIST_STARTER_H
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
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
#endif

class ScanListStarter : public QObject {
    Q_OBJECT
    /** @brief The most lists one session scans (DSD_TRUNK_SCAN_MAX_LISTS, issue #697). */
    Q_PROPERTY(int maxLists READ maxLists CONSTANT)
  public:
    explicit ScanListStarter(const AppPrefs* prefs, const SavedSystemsModel* systems, QObject* parent = nullptr);
    QVariantMap build(const QVariantMap& list) const;
    Q_INVOKABLE QVariantMap start(const QVariantMap& list, DecoderHost* host) const;
    Q_INVOKABLE QVariantMap validate(const QVariantMap& list) const;
    /**
     * @brief Start one session that scans every list of @p lists (issue #697): the first list's receiver and session
     * options, one --trunk-scan per list in that order (list 1 is the first), one call recording session, the first
     * list's. Nothing starts unless every list passes its own checks, joins the first (joinConflict()), builds, and the
     * lists together pass the engine's checks across them (no target id twice). One list is start().
     * @return start()'s map; "error" names the list it is about.
     */
    Q_INVOKABLE QVariantMap startLists(const QVariantList& lists, DecoderHost* host) const;
    /** @brief Why @p other cannot scan in one session with @p primary, or empty; see session_args_scan_join_conflict(). */
    Q_INVOKABLE QString joinConflict(const QVariantMap& primary, const QVariantMap& other) const;
    static int maxLists();

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
    QVariantMap prepare(const QVariantMap& list, bool materialize, const QString& recordingFolder = QString(),
                        bool recordingUnavailable = false, const QStringList& companionPaths = QStringList(),
                        int listNumber = 1) const;
    QVariantMap prepareCsv(const QVariantMap& list, bool materialize, const QString& recordingFolder,
                           bool recordingUnavailable, const QStringList& companionPaths, int listNumber) const;
    /* Issue #697: an imported target CSV copied to the list's own file (scan_list_csv_path()), its companions still
       resolving, so the decoder names the list after it; @p source itself when no copy can be made. */
    static QString listCopyOf(const QVariantMap& list, const QString& source, int listNumber);
    /* start()'s prepare: with the list's call recording folder when @p host can record it (@p recording set, the
       folder's session begun). */
    QVariantMap prepareForStart(const QVariantMap& list, const DecoderHost* host, bool* recording,
                                const QStringList& companionPaths) const;
    /* Start the first of @p lists with @p companionPaths, the others' CSVs, built already. */
    QVariantMap startPrepared(const QVariantList& lists, DecoderHost* host, const QStringList& companionPaths) const;
    /* Hold the decryption profiles @p list's entries use for the session. */
    void retainProfiles(const QVariantMap& list) const;
    /* startLists()' checks before anything is written: each list's own, then whether it joins the first. Their
       warnings, each named by its list, go to @p warnings. */
    QString checkLists(const QVariantList& lists, QStringList* warnings) const;
    /* Write each list's CSV (@p paths, in order) and check them together; @p total is their target count. */
    QString materializeLists(const QVariantList& lists, QStringList* paths, int* total) const;
    /* The recording session prepareForStart() began ends when the start did not happen and no session runs. */
    void endUnstartedRecording(bool recording, bool started, const DecoderHost* host) const;
    std::function<bool(const QString&)> m_targetFileLookup;
    const AppPrefs* m_prefs;
    const SavedSystemsModel* m_systems;
    DecryptionProfileProvider* m_profiles = nullptr;
    RecordingSessions* m_recordingSessions = nullptr;
};
} // namespace dsd_qt
#endif
