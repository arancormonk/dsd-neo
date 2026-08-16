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

namespace dsd_qt {

class AppPrefs : public QObject {
    Q_OBJECT
    Q_PROPERTY(int appearance READ appearance WRITE setAppearance NOTIFY appearanceChanged)
    Q_PROPERTY(bool onboardingDone READ onboardingDone WRITE setOnboardingDone NOTIFY onboardingDoneChanged)
    Q_PROPERTY(bool backgroundListening READ backgroundListening WRITE setBackgroundListening NOTIFY
                   backgroundListeningChanged)
    Q_PROPERTY(bool keepScreenAwake READ keepScreenAwake WRITE setKeepScreenAwake NOTIFY keepScreenAwakeChanged)
    Q_PROPERTY(bool skipEncrypted READ skipEncrypted WRITE setSkipEncrypted NOTIFY skipEncryptedChanged)
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

    int appearance() const;
    void setAppearance(int mode);

    bool onboardingDone() const;
    void setOnboardingDone(bool done);

    bool backgroundListening() const;
    void setBackgroundListening(bool on);

    bool keepScreenAwake() const;
    void setKeepScreenAwake(bool on);

    bool skipEncrypted() const;
    void setSkipEncrypted(bool on);

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
    void appearanceChanged();
    void onboardingDoneChanged();
    void backgroundListeningChanged();
    void keepScreenAwakeChanged();
    void skipEncryptedChanged();
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
    mutable QSettings m_settings;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_APP_PREFS_H_ */
