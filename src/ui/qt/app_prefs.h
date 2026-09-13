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

#include <QObject>
#include <QSettings>
#include <QString>
#include <QTimer>
#include <QtGlobal>

namespace dsd_qt {

class AppPrefs : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool autoStartOnAttach READ autoStartOnAttach WRITE setAutoStartOnAttach NOTIFY autoStartOnAttachChanged)
    Q_PROPERTY(QString lastStartedKind READ lastStartedKind WRITE setLastStartedKind NOTIFY lastStartedKindChanged)
    Q_PROPERTY(QString lastStartedUid READ lastStartedUid WRITE setLastStartedUid NOTIFY lastStartedUidChanged)
    Q_PROPERTY(double lastLat READ lastLat NOTIFY locationChanged)
    Q_PROPERTY(double lastLon READ lastLon NOTIFY locationChanged)
    Q_PROPERTY(qint64 lastFixAt READ lastFixAt NOTIFY locationChanged)

    Q_PROPERTY(int appearance READ appearance WRITE setAppearance NOTIFY appearanceChanged)
    Q_PROPERTY(bool metricUnits READ metricUnits WRITE setMetricUnits NOTIFY metricUnitsChanged)
    Q_PROPERTY(bool onboardingDone READ onboardingDone WRITE setOnboardingDone NOTIFY onboardingDoneChanged)
    Q_PROPERTY(bool notificationExplained READ notificationExplained WRITE setNotificationExplained NOTIFY
                   notificationExplainedChanged)
    Q_PROPERTY(bool backgroundListening READ backgroundListening WRITE setBackgroundListening NOTIFY
                   backgroundListeningChanged)
    Q_PROPERTY(bool keepScreenAwake READ keepScreenAwake WRITE setKeepScreenAwake NOTIFY keepScreenAwakeChanged)
    Q_PROPERTY(bool skipEncrypted READ skipEncrypted WRITE setSkipEncrypted NOTIFY skipEncryptedChanged)
    Q_PROPERTY(bool persistTgLockouts READ persistTgLockouts WRITE setPersistTgLockouts NOTIFY persistTgLockoutsChanged)
    Q_PROPERTY(double hangtimeSec READ hangtimeSec WRITE setHangtimeSec NOTIFY hangtimeSecChanged)
    Q_PROPERTY(bool autoPpm READ autoPpm WRITE setAutoPpm NOTIFY autoPpmChanged)
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

  public:
    /** @brief Appearance follows the OS by default; see the settings screen. */
    enum Appearance { FollowSystem = 0, Light = 1, Dark = 2 };
    Q_ENUM(Appearance)

    explicit AppPrefs(QObject* parent = nullptr);
    ~AppPrefs() override;

    bool autoStartOnAttach() const;
    void setAutoStartOnAttach(bool value);
    QString lastStartedKind() const;
    void setLastStartedKind(const QString& value);
    QString lastStartedUid() const;
    void setLastStartedUid(const QString& value);
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

    bool keepScreenAwake() const;
    void setKeepScreenAwake(bool on);

    bool persistTgLockouts() const;
    void setPersistTgLockouts(bool on);

    bool skipEncrypted() const;
    void setSkipEncrypted(bool on);

    double hangtimeSec() const;
    void setHangtimeSec(double seconds);

    bool autoPpm() const;
    void setAutoPpm(bool on);

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

    /** @brief "usb" or "rtltcp"; empty until the user has chosen, which is what makes
     *         the first Explore tap open the setup sheet instead of starting blind. */
    QString exploreSourceType() const;
    void setExploreSourceType(const QString& type);

    QString exploreHost() const;
    void setExploreHost(const QString& host);

    int explorePort() const;
    void setExplorePort(int port);

    /** @brief Start frequency in MHz, as text; empty until an explore session has run. */
    QString exploreFreqMhz() const;
    void setExploreFreqMhz(const QString& mhz);

  Q_SIGNALS:
    void autoStartOnAttachChanged();
    void lastStartedKindChanged();
    void lastStartedUidChanged();
    void locationChanged();

    void appearanceChanged();
    void metricUnitsChanged();
    void onboardingDoneChanged();
    void notificationExplainedChanged();
    void backgroundListeningChanged();
    void keepScreenAwakeChanged();
    void skipEncryptedChanged();
    void persistTgLockoutsChanged();
    void hangtimeSecChanged();
    void autoPpmChanged();
    void gainDbChanged();
    void ppmChanged();
    void bandwidthKhzChanged();
    void biasTeeChanged();
    void extraArgsChanged();
    void rrUsernameChanged();
    void rrAppKeyChanged();
    void exploreChanged();

  private:
    void expireLocation() const;
    void armLocationExpiry();
    QTimer m_locationExpiry;
    mutable QSettings m_settings;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_APP_PREFS_H_ */
