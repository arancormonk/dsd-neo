// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Builds the CLI-shaped argv a saved system starts with.
 *
 * Reusing the CLI parser is what buys the whole option surface, but the specs it
 * consumes are ':'-delimited strings a typo can silently mistune — so their
 * assembly lives here, in host-testable C++, rather than in QML-side JavaScript.
 * The pure function takes plain values; the QObject wrapper reads the live
 * AppPrefs and is what QML calls.
 */

#ifndef DSD_NEO_SRC_UI_QT_SESSION_ARGS_H_
#define DSD_NEO_SRC_UI_QT_SESSION_ARGS_H_

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace dsd_qt {

class AppPrefs;

/** @brief The app-wide defaults a saved system's overrides fall back to. */
struct SessionArgPrefs {
    int gainDb = 30;
    int ppm = 0;
    int bandwidthKhz = 48;
    bool biasTee = false;
    bool skipEncrypted = true;
    bool autoPpm = false;
    QString extraArgs;
};

/** @brief Why session_args_build() refused; None means the argv is usable. */
enum class SessionArgsError { None, Frequency, Ppm };

/**
 * @brief Whether a saved system's frequency field parses as a positive MHz value.
 *
 * Strict parse: trailing junk ("851.375M") must fail, because the value is
 * spliced verbatim into the rtl input spec where dsd_parse_freq_hz would read
 * garbage as 0 Hz and the session would come up silently mistuned.
 */
bool session_args_freq_valid(const QString& freqMhz);

/**
 * @brief The CLI-shaped argv for @p system, or an empty list with @p error set.
 *
 * Per-system overrides fall back to the app-wide defaults (-1 / empty string
 * mean "no override"; biasTee is -1 follow / 0 off / 1 on). A malformed
 * frequency or PPM must fail here, not downstream as a silently mistuned or
 * uncorrected session.
 */
QStringList session_args_build(const QVariantMap& system, const SessionArgPrefs& prefs, SessionArgsError* error);

/** @brief QML-facing wrapper binding the pure builder to the live AppPrefs. */
class SessionArgsBuilder : public QObject {
    Q_OBJECT

  public:
    explicit SessionArgsBuilder(const AppPrefs* prefs, QObject* parent = nullptr);
    ~SessionArgsBuilder() override;

    /**
     * @brief Build the argv for one saved-system field map.
     * @return {"ok": bool, "args": QStringList, "error": ""|"frequency"|"ppm"}.
     */
    Q_INVOKABLE QVariantMap build(const QVariantMap& system) const;

    /** @brief Frequency validity for the wizard's step gating; see session_args_freq_valid(). */
    Q_INVOKABLE bool freqValid(const QString& freqMhz) const;

  private:
    const AppPrefs* m_prefs;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SESSION_ARGS_H_ */
