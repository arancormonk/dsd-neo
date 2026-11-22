// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The DMR Tier III Channels sheet's rows, from UiController's held snapshot (discussion #900 F3).
 *
 * Two lists: the channels granted without a frequency (the missing-channel ledger) and the channels with an entry in
 * the channel map or the user's overlay, each named by its decimal LPCN. The rows are rebuilt only when the view's
 * body was copied anew (its content_seq) or a name list changed, and a list is published only when its rows differ,
 * so a sheet a screen reader reads sees no churn from repeated grants (#782). While the sheet is closed (active false)
 * nothing is read.
 */

#ifndef DSD_NEO_SRC_UI_QT_DMR_CHANNELS_MODEL_H_
#define DSD_NEO_SRC_UI_QT_DMR_CHANNELS_MODEL_H_

// Export the container and element definitions used by the inline QVariantList getters.
#include <QList> // IWYU pragma: export
#include <QObject>
#include <QString>
#include <QVariant> // IWYU pragma: export
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <dsd-neo/app_control/dmr_t3_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

namespace dsd_qt {

class DmrChannelsModel : public QObject {
    Q_OBJECT
    /** @brief The sheet is open: only then is the snapshot read. Opening it reads the held snapshot at once
     *  (UiController, on activeChanged), so the sheet never shows the rows of an earlier opening. */
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    /**
     * @brief The channels granted without a frequency, the most recently heard first and then the rest by channel:
     * {lpcn, slot (1 or 2, 0 unknown), group, data, caller, recipient, callerName, recipientName, lastHeardMs
     * (dsd_decode_now_mono_ms() stamp, 0 unknown), unverified, detailed (false: only the channel is known)}.
     */
    Q_PROPERTY(QVariantList missing READ missing NOTIFY missingChanged)
    /** @brief The same rows by channel number ascending: the fixed reading order under a screen reader. */
    Q_PROPERTY(QVariantList missingByChannel READ missingByChannel NOTIFY missingChanged)
    Q_PROPERTY(int missingCount READ missingCount NOTIFY missingChanged)
    /**
     * @brief The channels with a frequency or an entry of the user's, by channel number ascending: {lpcn, hz (the
     * frequency DMR uses, dsd_ui_dmr_t3_view::channel_hz), origin ("file", "user", "hidden", "site", "unchecked",
     * "neighbour", "estimated"), originCode (dmr_chan_origin_e), baseHz (what Restore returns to, 0 none), hidden,
     * yours (the user's overlay holds an entry)}.
     */
    Q_PROPERTY(QVariantList channels READ channels NOTIFY channelsChanged)
    /** @brief The session recorded DMR channel state (dsd_ui_dmr_t3_view::present). */
    Q_PROPERTY(bool present READ present NOTIFY stateChanged)
    /** @brief Checked messages named the site Tier III: the identity, not display only. */
    Q_PROPERTY(bool tier3 READ tier3 NOTIFY stateChanged)
    /** @brief Only RAS-accepted messages named it so: display only. */
    Q_PROPERTY(bool tier3Unchecked READ tier3Unchecked NOTIFY stateChanged)
    /** @brief The current carrier's TSCC frequency when checked evidence names it, else 0. */
    Q_PROPERTY(qint64 ccHz READ ccHz NOTIFY stateChanged)
    Q_PROPERTY(bool ccConfirmedHere READ ccConfirmedHere NOTIFY stateChanged)
    /** @brief What an edit expects: the network generation, and the channel revision the rows were read at. */
    Q_PROPERTY(quint32 generation READ generation NOTIFY stateChanged)
    Q_PROPERTY(quint32 revision READ revision NOTIFY stateChanged)
    /** @brief The command session the rows were read in, in decimal (CommandBridge::setDmrChannel()'s @c session);
     *  empty when none is known. */
    Q_PROPERTY(QString session READ session NOTIFY stateChanged)
    /** @brief The session runs a trunk scan: every edit is refused (read only). */
    Q_PROPERTY(bool trunkScanActive READ trunkScanActive NOTIFY stateChanged)
    /** @brief The saved system the session runs, set by QML (empty: Explore, a scan, a search): where edits are
     *  kept. */
    Q_PROPERTY(QString sessionSystemUid READ sessionSystemUid WRITE setSessionSystemUid NOTIFY stateChanged)
    Q_PROPERTY(bool sessionHasSavedSystem READ sessionHasSavedSystem NOTIFY stateChanged)
    /* The command's operations, the channel numbers and frequencies it takes, and the most entries the user keeps. */
    Q_PROPERTY(int opSet READ opSet CONSTANT)
    Q_PROPERTY(int opHide READ opHide CONSTANT)
    Q_PROPERTY(int opRestore READ opRestore CONSTANT)
    Q_PROPERTY(int minChannel READ minChannel CONSTANT)
    Q_PROPERTY(int maxChannel READ maxChannel CONSTANT)
    Q_PROPERTY(qint64 minHz READ minHz CONSTANT)
    Q_PROPERTY(qint64 maxHz READ maxHz CONSTANT)
    Q_PROPERTY(int maxEntries READ maxEntries CONSTANT)

  public:
    explicit DmrChannelsModel(QObject* parent = nullptr);

    bool
    active() const {
        return m_active;
    }

    void setActive(bool enabled);
    /**
     * @brief Read @p snapshot's Tier III view (and @p opts_snapshot's trunk scan setting) while active.
     * @p commandSession is the command session it was read in (dsd_app_command_session_generation(); 0 for none known),
     * published as session().
     */
    void refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot, quint64 commandSession = 0);
    /** @brief A new scan target, or no live session (nullptr): publish @p snapshot's rows in one replacement, or
     *  none. */
    void replaceForTarget(const dsd_opts* opts_snapshot, const dsd_state* snapshot, quint64 commandSession = 0);
    /** @brief No session: no rows, nothing expected. */
    void clear();

    /**
     * @brief Channel @p lpcn as an edit expects it: {lpcn, hz (the frequency DMR uses, 0 none or hidden), originCode,
     * origin, baseHz, hidden, yours, missing, valid (a channel number 1..4094)}. Read from the rows last published.
     */
    Q_INVOKABLE QVariantMap channel(int lpcn) const;
    /** @brief The decode clock the missing rows' lastHeardMs is stamped on, in ms, for their age. */
    Q_INVOKABLE qint64 decodeNowMs() const;
    /** @brief "file", "user", "hidden", "site", "unchecked", "neighbour", "estimated", or "" for none. */
    static QString originKey(int originCode, bool hidden);

    QVariantList
    missing() const {
        return m_missing;
    }

    QVariantList
    missingByChannel() const {
        return m_missingByChannel;
    }

    int
    missingCount() const {
        return static_cast<int>(m_missingByChannel.size());
    }

    QVariantList
    channels() const {
        return m_channels;
    }

    bool
    present() const {
        return m_state.present;
    }

    bool
    tier3() const {
        return m_state.tier3;
    }

    bool
    tier3Unchecked() const {
        return m_state.tier3Unchecked;
    }

    qint64
    ccHz() const {
        return m_state.ccHz;
    }

    bool
    ccConfirmedHere() const {
        return m_state.ccConfirmedHere;
    }

    quint32
    generation() const {
        return m_state.generation;
    }

    quint32
    revision() const {
        return m_state.revision;
    }

    QString
    session() const {
        return m_state.session;
    }

    bool
    trunkScanActive() const {
        return m_state.trunkScan;
    }

    QString
    sessionSystemUid() const {
        return m_sessionSystemUid;
    }

    void setSessionSystemUid(const QString& uid);

    bool
    sessionHasSavedSystem() const {
        return !m_sessionSystemUid.isEmpty();
    }

    static int
    opSet() {
        return DSD_UI_DMR_T3_OP_SET;
    }

    static int
    opHide() {
        return DSD_UI_DMR_T3_OP_HIDE;
    }

    static int
    opRestore() {
        return DSD_UI_DMR_T3_OP_RESTORE;
    }

    static int
    minChannel() {
        return 1;
    }

    static int
    maxChannel() {
        return DSD_UI_DMR_T3_ROWS - 2;
    }

    static qint64
    minHz() {
        return DSD_UI_DMR_T3_MIN_HZ;
    }

    static qint64
    maxHz() {
        return DSD_UI_DMR_T3_MAX_HZ;
    }

    static int
    maxEntries() {
        return DSD_UI_DMR_T3_OVERLAY_MAX_ROWS;
    }

  Q_SIGNALS:
    void activeChanged();
    void missingChanged();
    void channelsChanged();
    void stateChanged();

  private:
    struct State {
        bool present = false;
        bool tier3 = false;
        bool tier3Unchecked = false;
        bool ccConfirmedHere = false;
        bool trunkScan = false;
        qint64 ccHz = 0;
        quint32 generation = 0;
        quint32 revision = 0;
        QString session;

        bool operator==(const State& other) const;
    };

    void rebuild(const dsd_ui_dmr_t3_view& view, const dsd_state* snapshot);
    void publishState(const State& next);
    void publishLists(const QVariantList& recentRows, const QVariantList& byChannelRows,
                      const QVariantList& channelRows);

    bool m_active = false;
    bool m_built = false;
    uint64_t m_contentSeq = 0;
    uint64_t m_nameKey = 0;
    State m_state;
    QString m_sessionSystemUid;
    /* The decode clock decodeNowMs() reads (dsd_decode_now_mono_ms()). */
    uint64_t (*m_decodeNowMs)(void);
    QVariantList m_missing;
    QVariantList m_missingByChannel;
    QVariantList m_channels;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DMR_CHANNELS_MODEL_H_ */
