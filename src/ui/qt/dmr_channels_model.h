// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The DMR Tier III Channels sheet's rows, from UiController's held snapshot (discussion #900 F3).
 *
 * Two lists: the channels granted without a frequency (the missing-channel ledger) and the channels with an entry in
 * the channel map or the user's overlay, each named by its decimal LPCN. The two are read apart, each from its own part
 * of the view: the missing rows are rebuilt when the ledger was copied anew (missing_seq) or a name list changed, the
 * mapped channels (up to 4094 rows) only when the channel rows were (content_seq), so a grant heard again on a channel
 * with no frequency costs one ledger walk. Of the missing channels (the ledger can hold 4094) only the page the sheet
 * shows is built as rows (missingPage: the first missingLimit, in the order missingInChannelOrder picks), so a grant
 * heard again builds at most one page. A list is published only when its rows differ, so a sheet
 * a screen reader reads sees no churn from repeated grants (#782). While the sheet is closed (active false) nothing is read. While the carrier names
 * another system (other_system_here), or the session does not decode DMR now (dmr_decoding 0), both lists are empty
 * and no channel is offered for an edit: no Tier III lookup reads them there.
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
     * @brief The first missingLimit channels granted without a frequency, the most recently heard first and then the
     * rest by channel, or (missingInChannelOrder) all by channel: {lpcn, slot (1 or 2, 0 unknown), group, data, caller,
     * recipient, callerName, recipientName, lastHeardMs (dsd_decode_now_mono_ms() stamp, 0 unknown), unverified (the
     * latest grant's, or for a channel past the details whether no checked grant named it), detailed (false: only the
     * channel is known)}.
     */
    Q_PROPERTY(QVariantList missingPage READ missingPage NOTIFY missingChanged)
    /** @brief How many missing channels missingPage holds at most (the sheet's pages shown); 1 or more. */
    Q_PROPERTY(int missingLimit READ missingLimit WRITE setMissingLimit NOTIFY missingChanged)
    /** @brief missingPage by channel number ascending: the fixed reading order under a screen reader. */
    Q_PROPERTY(
        bool missingInChannelOrder READ missingInChannelOrder WRITE setMissingInChannelOrder NOTIFY missingChanged)
    /** @brief Every channel in the ledger, the ones past missingPage included. */
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
    /** @brief The carrier names another system (dsd_ui_dmr_t3_view::other_system_here): its map is that system's, so
     *  no Tier III row is listed and no edit is offered until the receiver is back on the Tier III system. */
    Q_PROPERTY(bool otherSystemHere READ otherSystemHere NOTIFY stateChanged)
    /** @brief The session decodes DMR now (dsd_ui_dmr_t3_view::dmr_decoding; true while no view is known): false
     *  after a live mode change left DMR off, when no Tier III row is listed and no edit is offered, as for
     *  otherSystemHere. */
    Q_PROPERTY(bool dmrDecoding READ dmrDecoding NOTIFY stateChanged)
    /** @brief An edit can be sent: no trunk scan, the carrier names no other system, and the session decodes DMR. */
    Q_PROPERTY(bool editable READ editable NOTIFY stateChanged)
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
    ~DmrChannelsModel() override;

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
     * origin, baseHz, hidden, yours, missing, valid (a channel number 1..4094, the carrier names no other
     * system, and the session decodes DMR)}. Read from the rows last published.
     */
    Q_INVOKABLE QVariantMap channel(int lpcn) const;
    /** @brief The decode clock the missing rows' lastHeardMs is stamped on, in ms, for their age. */
    Q_INVOKABLE qint64 decodeNowMs() const;
    /** @brief "file", "user", "hidden", "site", "unchecked", "neighbour", "estimated", or "" for none. */
    static QString originKey(int originCode, bool hidden);

    QVariantList
    missingPage() const {
        return m_missingPage;
    }

    int
    missingLimit() const {
        return m_missingLimit;
    }

    void setMissingLimit(int limit);

    bool
    missingInChannelOrder() const {
        return m_missingInChannelOrder;
    }

    void setMissingInChannelOrder(bool byChannel);

    int
    missingCount() const {
        return m_missingCount;
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

    bool
    otherSystemHere() const {
        return m_state.otherSystemHere;
    }

    bool
    dmrDecoding() const {
        return m_state.dmrDecoding;
    }

    bool
    editable() const {
        return !m_state.trunkScan && !m_state.otherSystemHere && m_state.dmrDecoding;
    }

    /** @brief How many times the Channels list's rows were built (tests read it: a grant heard again builds none). */
    quint64
    channelRowBuilds() const {
        return m_channelRowBuilds;
    }

    /** @brief How many missing-channel rows were built in all (tests read it: a grant heard again builds one page). */
    quint64
    missingRowBuilds() const {
        return m_missingRowBuilds;
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
        bool otherSystemHere = false;
        bool dmrDecoding = true;
        qint64 ccHz = 0;
        quint32 generation = 0;
        quint32 revision = 0;
        QString session;

        bool operator==(const State& other) const;
    };

    /* A ledger detail (the latest grant that named the channel) with the names the lists gave its parties. */
    struct MissingDetail {
        dsd_ui_dmr_t3_missing_detail grant{};
        QString callerName;
        QString recipientName;
    };

    /* Each list whose part of @p view moved since the last build (all of them before the first). */
    void rebuildMoved(const dsd_ui_dmr_t3_view& view, const dsd_state* snapshot);
    void rebuildMissing(const dsd_ui_dmr_t3_view& view, const dsd_state* snapshot);
    void rebuildChannels(const dsd_ui_dmr_t3_view& view);
    void publishState(const State& next);
    /* Build missingPage from the ledger kept, and publish it with the count when either differs (true: published). */
    bool publishMissingPage();
    /* Channel @p lpcn's missing row: from @p detail when the ledger details it (nullptr: only the channel). */
    QVariantMap missingRow(uint16_t lpcn, const MissingDetail* detail);
    const MissingDetail* missingDetail(uint16_t lpcn) const;
    bool missingHas(uint16_t lpcn) const;
    void publishChannels(const QVariantList& channelRows);
    /* No Tier III rows: the ledger kept and both lists emptied. */
    void dropLists();

    bool m_active = false;
    bool m_built = false;
    uint64_t m_contentSeq = 0;
    uint64_t m_missingSeq = 0;
    uint64_t m_nameKey = 0;
    State m_state;
    QString m_sessionSystemUid;
    /* The decode clock decodeNowMs() reads (dsd_decode_now_mono_ms()). */
    uint64_t (*m_decodeNowMs)(void);
    /* The ledger as last read (its sets; details holds no names), the details still missing with their names, newest
       first, and how many channels it holds. */
    dsd_ui_dmr_t3_missing m_ledger{};
    QList<MissingDetail> m_details;
    int m_missingCount = 0;
    int m_missingLimit = 50;
    bool m_missingInChannelOrder = false;
    QVariantList m_missingPage;
    int m_publishedMissingCount = 0;
    QVariantList m_channels;
    quint64 m_channelRowBuilds = 0;
    quint64 m_missingRowBuilds = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DMR_CHANNELS_MODEL_H_ */
