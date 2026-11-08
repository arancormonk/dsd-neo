// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Persistent app preferences exposed to QML.
 *
 * Everything the settings screen edits, plus the onboarding-completed flag. Values
 * persist through QSettings so they survive process death on every platform. The
 * advanced tuner values here are the app-wide defaults; a saved system may carry
 * its own overrides (see saved_systems_model.h).
 */

#ifndef DSD_NEO_SRC_UI_QT_APP_PREFS_H_
#define DSD_NEO_SRC_UI_QT_APP_PREFS_H_

#include <QList>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

namespace dsd_qt {

class AppPrefs : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool autoStartOnAttach READ autoStartOnAttach WRITE setAutoStartOnAttach NOTIFY autoStartOnAttachChanged)
    Q_PROPERTY(QString lastStartedKind READ lastStartedKind WRITE setLastStartedKind NOTIFY lastStartedKindChanged)
    Q_PROPERTY(QString lastStartedUid READ lastStartedUid WRITE setLastStartedUid NOTIFY lastStartedUidChanged)
    /* The session the host last accepted a start for ("scan", "explore" or "saved") and its UID (issue #671). Written
     * when the start is accepted, unlike lastStarted*, which wait for the post-initialization edge and drive Home and
     * auto-start: a recreated Activity takes the identity of the session it finds running from these. Device-local. */
    Q_PROPERTY(QString acceptedStartKind READ acceptedStartKind WRITE setAcceptedStartKind NOTIFY acceptedStartChanged)
    Q_PROPERTY(QString acceptedStartUid READ acceptedStartUid WRITE setAcceptedStartUid NOTIFY acceptedStartChanged)
    /* Issue #697: the other scan lists a scan session scans with its list (the UID above), in list order: the rest of
     * the session's identity, written with acceptedStart* and lastStarted* (acceptedStartCompanions,
     * lastStartedCompanions), empty for every other start. Device-local: a backup carries no list identity a restore
     * could leave dangling, and a restore clears them (forgetScanCompanions()). */
    Q_PROPERTY(QStringList acceptedStartCompanions READ acceptedStartCompanions WRITE setAcceptedStartCompanions NOTIFY
                   acceptedStartChanged)
    Q_PROPERTY(QStringList lastStartedCompanions READ lastStartedCompanions WRITE setLastStartedCompanions NOTIFY
                   lastStartedCompanionsChanged)
    /* Issue #644: the targets the accepted scan session runs (the list of maps KnownFrequencyIndex::sessionTargets()
     * gives), written when its start is accepted, empty for every other start: a recreated Activity takes them up from
     * here, since a list deleted while its session runs takes its generated target files with it. Device-local. */
    Q_PROPERTY(QVariantList acceptedStartTargets READ acceptedStartTargets WRITE setAcceptedStartTargets NOTIFY
                   acceptedStartChanged)
    /* Issue #801: the shortcuts pinned to Listen, in order, each {kind, uid}: "system" or "site" (a saved row's UID),
     * "scan" (a scan list's UID), "search" or "explore" (uid empty). Read sanitized (unknown kinds, malformed UIDs and
     * repeats dropped, at most kPinnedCap); ListenPins resolves and edits them. Portable. */
    Q_PROPERTY(QVariantList pinned READ pinned WRITE setPinned NOTIFY pinnedChanged)
    Q_PROPERTY(double lastLat READ lastLat NOTIFY locationChanged)
    Q_PROPERTY(double lastLon READ lastLon NOTIFY locationChanged)
    Q_PROPERTY(qint64 lastFixAt READ lastFixAt NOTIFY locationChanged)
    Q_PROPERTY(double lastAccuracyM READ lastAccuracyM NOTIFY locationChanged)

    Q_PROPERTY(int appearance READ appearance WRITE setAppearance NOTIFY appearanceChanged)
    Q_PROPERTY(bool metricUnits READ metricUnits WRITE setMetricUnits NOTIFY metricUnitsChanged)
    Q_PROPERTY(bool onboardingDone READ onboardingDone WRITE setOnboardingDone NOTIFY onboardingDoneChanged)
    Q_PROPERTY(bool notificationExplained READ notificationExplained WRITE setNotificationExplained NOTIFY
                   notificationExplainedChanged)
    Q_PROPERTY(bool backgroundListening READ backgroundListening WRITE setBackgroundListening NOTIFY
                   backgroundListeningChanged)
    /* How the screen behaves while the app is open (#574). One signal for both:
     * the host takes them as a single policy, so a change to either re-applies the
     * pair. The delay matters only to the two between-calls modes. */
    Q_PROPERTY(int screenMode READ screenMode WRITE setScreenMode NOTIFY screenPolicyChanged)
    Q_PROPERTY(int screenDelaySec READ screenDelaySec WRITE setScreenDelaySec NOTIFY screenPolicyChanged)
    Q_PROPERTY(QVariantList screenDelayChoices READ screenDelayChoices CONSTANT)
    Q_PROPERTY(bool skipEncrypted READ skipEncrypted WRITE setSkipEncrypted NOTIFY skipEncryptedChanged)
    Q_PROPERTY(bool persistTgLockouts READ persistTgLockouts WRITE setPersistTgLockouts NOTIFY persistTgLockoutsChanged)
    Q_PROPERTY(double hangtimeSec READ hangtimeSec WRITE setHangtimeSec NOTIFY hangtimeSecChanged)
    Q_PROPERTY(bool autoPpm READ autoPpm WRITE setAutoPpm NOTIFY autoPpmChanged)
    Q_PROPERTY(bool relaxedCrc READ relaxedCrc WRITE setRelaxedCrc NOTIFY relaxedCrcChanged)
    Q_PROPERTY(int gainDb READ gainDb WRITE setGainDb NOTIFY gainDbChanged)
    Q_PROPERTY(int ppm READ ppm WRITE setPpm NOTIFY ppmChanged)
    Q_PROPERTY(int bandwidthKhz READ bandwidthKhz WRITE setBandwidthKhz NOTIFY bandwidthKhzChanged)
    Q_PROPERTY(bool biasTee READ biasTee WRITE setBiasTee NOTIFY biasTeeChanged)
    Q_PROPERTY(QString extraArgs READ extraArgs WRITE setExtraArgs NOTIFY extraArgsChanged)
    /* RadioReference account. The password is deliberately absent: it is held in
     * memory for the session only and re-prompted next launch, so it can never
     * reach a settings file, a backup or a log. */
    Q_PROPERTY(QString rrUsername READ rrUsername WRITE setRrUsername NOTIFY rrUsernameChanged)
    Q_PROPERTY(QString rrAppKey READ rrAppKey WRITE setRrAppKey NOTIFY rrAppKeyChanged)
    /* Where exploring was left off. One signal for all four: they are written
     * together as a single "how to start exploring" answer, and nothing binds to
     * one of them without the rest. */
    Q_PROPERTY(QString exploreSourceType READ exploreSourceType WRITE setExploreSourceType NOTIFY exploreChanged)
    Q_PROPERTY(QString exploreHost READ exploreHost WRITE setExploreHost NOTIFY exploreChanged)
    Q_PROPERTY(int explorePort READ explorePort WRITE setExplorePort NOTIFY exploreChanged)
    Q_PROPERTY(QString exploreFreqMhz READ exploreFreqMhz WRITE setExploreFreqMhz NOTIFY exploreChanged)
    /* The radio the last signal search started on, written together when Start
     * search is pressed. Separate from the explore keys: a search must not move
     * where Explore resumes, and Explore's start frequency has no meaning here. */
    Q_PROPERTY(QString searchSourceType READ searchSourceType WRITE setSearchSourceType NOTIFY searchChanged)
    Q_PROPERTY(QString searchHost READ searchHost WRITE setSearchHost NOTIFY searchChanged)
    Q_PROPERTY(int searchPort READ searchPort WRITE setSearchPort NOTIFY searchChanged)
    /* Whether the Monitor's "Signal & site" readings were left open (#718). How this
     * phone's screen was arranged, so it stays on the device and out of backups. */
    Q_PROPERTY(bool monitorDetailsExpanded READ monitorDetailsExpanded WRITE setMonitorDetailsExpanded NOTIFY
                   monitorDetailsExpandedChanged)
    /* Issue #646: the Listen tab last chosen ("systems", "scanLists" or "explore"), so Listen opens on it. How this
     * phone's screen was arranged, like the one above: device-local. */
    Q_PROPERTY(QString listenSection READ listenSection WRITE setListenSection NOTIFY listenSectionChanged)
    /* Call recording (issue #708). Every session the app starts records per call while recordCalls is on; Settings
     * also applies both switches to a running session. The limit is the recording store's, in decimal MB. */
    Q_PROPERTY(bool recordCalls READ recordCalls WRITE setRecordCalls NOTIFY recordCallsChanged)
    Q_PROPERTY(bool recordUnknownOnly READ recordUnknownOnly WRITE setRecordUnknownOnly NOTIFY recordUnknownOnlyChanged)
    Q_PROPERTY(int recordingLimitMb READ recordingLimitMb WRITE setRecordingLimitMb NOTIFY recordingLimitMbChanged)
    Q_PROPERTY(QVariantList recordingLimitChoices READ recordingLimitChoices CONSTANT)
    /* Issue #756: the longest pause one analog call recording keeps going through, in ms; sent with -P and live by
     * Settings like the switches above. */
    Q_PROPERTY(int analogMaxGapMs READ analogMaxGapMs WRITE setAnalogMaxGapMs NOTIFY analogMaxGapMsChanged)
    Q_PROPERTY(QVariantList analogMaxGapChoices READ analogMaxGapChoices CONSTANT)
    /* Issue #668: how much of what was heard just before an NFM call each recording starts with, in ms; sent with -P
     * and live by Settings like the pause hold. */
    Q_PROPERTY(int analogPrerollMs READ analogPrerollMs WRITE setAnalogPrerollMs NOTIFY analogPrerollMsChanged)
    Q_PROPERTY(QVariantList analogPrerollChoices READ analogPrerollChoices CONSTANT)

  public:
    /** @brief Appearance follows the OS by default; see the settings screen. */
    enum Appearance { FollowSystem = 0, Light = 1, Dark = 2 };
    Q_ENUM(Appearance)

    /**
     * @brief The screen modes the settings screen offers (#574).
     *
     * The codes are a contract with the Android policy's `ScreenPolicy.Mode`
     * (android/package/src/io/github/arancormonk/dsdneo/ScreenPolicy.kt), which
     * receives them unchanged through DecoderHost::setScreenPolicy(). They are
     * also persisted. Never renumber them.
     */
    enum ScreenMode { ScreenSystem = 0, ScreenAlwaysOn = 1, ScreenDimBetweenCalls = 2, ScreenOffBetweenCalls = 3 };
    Q_ENUM(ScreenMode)

    /** @brief How many shortcuts Listen holds pinned (issue #801). */
    static constexpr int kPinnedCap = 6;

    /** @brief One persisted key and whether it travels with a backup; device-local state does not. */
    struct KeySpec {
        const char* name;
        bool portable;
    };

    /** @brief Every key AppPrefs persists. */
    static QList<KeySpec> keyTable();
    /** @brief The names of the keys with portable set. */
    static QStringList portableKeys();
    /** @brief Portable keys present in the persisted settings, read through a fresh QSettings on the same file. */
    static QVariantMap readPortable();
    /** @brief Set every portable key present in @p values, remove every portable key absent from it; ignore other
     *         keys; sync() and return status() == QSettings::NoError. Device-local keys are never touched. */
    static bool replacePortable(const QVariantMap& values);
    /** @brief Remove both scan-list companion keys (issue #697), as a restore or a repaired list UID must: the UIDs they
     *         name may now be other lists or none. sync() and return status() == QSettings::NoError. */
    static bool forgetScanCompanions();

    explicit AppPrefs(QObject* parent = nullptr);
    ~AppPrefs() override;

    bool autoStartOnAttach() const;
    void setAutoStartOnAttach(bool value);
    QString lastStartedKind() const;
    void setLastStartedKind(const QString& value);
    QString lastStartedUid() const;
    void setLastStartedUid(const QString& value);
    QString acceptedStartKind() const;
    void setAcceptedStartKind(const QString& value);
    QString acceptedStartUid() const;
    void setAcceptedStartUid(const QString& value);
    QStringList acceptedStartCompanions() const;
    void setAcceptedStartCompanions(const QStringList& value);
    QStringList lastStartedCompanions() const;
    void setLastStartedCompanions(const QStringList& value);
    QVariantList acceptedStartTargets() const;
    void setAcceptedStartTargets(const QVariantList& value);
    /** @brief The pinned shortcuts as stored, sanitized: text that is no JSON array (a hand-edited unquoted value
     *         included) reads as none. */
    QVariantList pinned() const;
    /** @brief Store @p value sanitized as compact JSON, or remove the key when that leaves none. */
    void setPinned(const QVariantList& value);
    double lastLat() const;
    void setLastLat(double value);
    double lastLon() const;
    void setLastLon(double value);
    /** Publish coordinates and timestamp together. Separate property writes can
     * notify a reader before the timestamp makes the new coordinates valid. */
    Q_INVOKABLE void setLocationFix(double lat, double lon, qint64 fixAtMs, double accuracyM = 0);
    double lastAccuracyM() const;
    qint64 lastFixAt() const;
    void setLastFixAt(qint64 value);

    int appearance() const;
    void setAppearance(int mode);

    /** @brief Display metric units when enabled; imperial is the default. */
    bool metricUnits() const;
    void setMetricUnits(bool on);

    bool onboardingDone() const;
    void setOnboardingDone(bool done);

    bool notificationExplained() const;
    void setNotificationExplained(bool value);
    bool backgroundListening() const;
    void setBackgroundListening(bool on);

    /**
     * @brief The stored ScreenMode, or System default if the stored value is not one.
     *
     * An install that never chose a mode reads the switch this replaced: keep-awake
     * on reads as Always on. Reading never writes.
     */
    int screenMode() const;
    void setScreenMode(int mode);

    /** @brief Seconds without audio before a between-calls mode acts; one of screenDelayChoices(), else 30. */
    int screenDelaySec() const;
    void setScreenDelaySec(int seconds);

    /** @brief The delays offered, in seconds; the same list as ScreenPolicy's DELAY_CHOICES_SECONDS. */
    QVariantList screenDelayChoices() const;

    bool persistTgLockouts() const;
    void setPersistTgLockouts(bool on);

    bool skipEncrypted() const;
    void setSkipEncrypted(bool on);

    double hangtimeSec() const;
    void setHangtimeSec(double seconds);

    bool autoPpm() const;
    void setAutoPpm(bool on);

    /** @brief Relaxed CRC (-F) for every session the app starts; off by default. Settings also applies it live. */
    bool relaxedCrc() const;
    void setRelaxedCrc(bool on);

    int gainDb() const;
    void setGainDb(int db);

    int ppm() const;
    void setPpm(int value);

    int bandwidthKhz() const;
    void setBandwidthKhz(int khz);

    bool biasTee() const;
    void setBiasTee(bool on);

    QString extraArgs() const;
    void setExtraArgs(const QString& args);

    /** @brief RadioReference.com username; empty until the user signs in. */
    QString rrUsername() const;
    void setRrUsername(const QString& username);

    /** @brief User-supplied RadioReference application key; empty means use the
     *         key baked in at build time, if this build carries one. */
    QString rrAppKey() const;
    void setRrAppKey(const QString& key);

    /** @brief "usb", "airspy" or "rtltcp"; empty until the user has chosen, which is what
     *         makes the first Explore tap open the setup sheet instead of starting blind. */
    QString exploreSourceType() const;
    void setExploreSourceType(const QString& type);

    QString exploreHost() const;
    void setExploreHost(const QString& host);

    int explorePort() const;
    void setExplorePort(int port);

    /** @brief Start frequency in MHz, as text; empty until an explore session has run. */
    QString exploreFreqMhz() const;
    void setExploreFreqMhz(const QString& mhz);

    /** @brief "usb", "airspy" or "rtltcp" as the last search started; empty until one has. */
    QString searchSourceType() const;
    void setSearchSourceType(const QString& type);

    QString searchHost() const;
    void setSearchHost(const QString& host);

    int searchPort() const;
    void setSearchPort(int port);

    /** @brief The Monitor's "Signal & site" disclosure is open; closed by default. Device-local. */
    bool monitorDetailsExpanded() const;
    void setMonitorDetailsExpanded(bool on);

    /** @brief The Listen tab to open on: "systems", "scanLists" or "explore"; anything else stored reads as "systems".
     *         Device-local. */
    QString listenSection() const;
    /** @brief Store @p section if it is one of the three ids; any other text is refused and writes nothing. */
    void setListenSection(const QString& section);

    /** @brief Record each call of every session the app starts (-P); off by default. */
    bool recordCalls() const;
    void setRecordCalls(bool on);

    /** @brief Record only calls no talkgroup row knows (--record-unknown-only); off by default. */
    bool recordUnknownOnly() const;
    void setRecordUnknownOnly(bool on);

    /** @brief The call recordings' storage limit in decimal MB, 0 for none; one of recordingLimitChoices(), else
     *         1000 (1 GB). */
    int recordingLimitMb() const;
    void setRecordingLimitMb(int mb);

    /** @brief The limits offered, in MB: 250, 500, 1000, 2000, 5000 and 0 (no limit). */
    QVariantList recordingLimitChoices() const;

    /** @brief The longest pause in ms one analog call recording keeps going through (--analog-record-max-gap-ms):
     *         any value the decoder takes, 0..10000, 200 or less meaning off; else 200 (off, the decoder's default).
     *         A value in range that Settings does not offer is kept as it is. */
    int analogMaxGapMs() const;
    void setAnalogMaxGapMs(int ms);

    /** @brief The pause holds offered, in ms: 200 (off), 500, 1000, 2000, 3000, 5000 and 10000. */
    QVariantList analogMaxGapChoices() const;

    /** @brief How much in ms of what was heard just before an NFM call each recording starts with
     *         (--analog-record-preroll-ms): any value the decoder takes, 0..1000, 0 meaning off; else the decoder's
     *         default. A value in range that Settings does not offer is kept as it is. */
    int analogPrerollMs() const;
    void setAnalogPrerollMs(int ms);

    /** @brief The pre-rolls offered, in ms: 0 (off), 100, 200, 300, 500 and 1000. */
    static QVariantList analogPrerollChoices();

  Q_SIGNALS:
    void autoStartOnAttachChanged();
    void lastStartedKindChanged();
    void lastStartedUidChanged();
    void acceptedStartChanged();
    void lastStartedCompanionsChanged();
    void pinnedChanged();
    void locationChanged();

    void appearanceChanged();
    void metricUnitsChanged();
    void onboardingDoneChanged();
    void notificationExplainedChanged();
    void backgroundListeningChanged();
    void screenPolicyChanged();
    void skipEncryptedChanged();
    void persistTgLockoutsChanged();
    void hangtimeSecChanged();
    void autoPpmChanged();
    void relaxedCrcChanged();
    void gainDbChanged();
    void ppmChanged();
    void bandwidthKhzChanged();
    void biasTeeChanged();
    void extraArgsChanged();
    void rrUsernameChanged();
    void rrAppKeyChanged();
    void exploreChanged();
    void searchChanged();
    void monitorDetailsExpandedChanged();
    void listenSectionChanged();
    void recordCallsChanged();
    void recordUnknownOnlyChanged();
    void recordingLimitMbChanged();
    void analogMaxGapMsChanged();
    void analogPrerollMsChanged();

  private:
    void expireLocation() const;
    void armLocationExpiry();

    /** @brief The app's settings file; the single place its scope and names are spelled. */
    class Settings : public QSettings {
      public:
        Settings();
    };

    QTimer m_locationExpiry;
    mutable Settings m_settings;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_APP_PREFS_H_ */
