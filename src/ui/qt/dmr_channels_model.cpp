// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "dmr_channels_model.h"

#include <QMap>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>
#include <algorithm>
#include <array>
#include <dsd-neo/app_control/dmr_t3_view.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/utf16.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <memory>
#include <stddef.h>
#include <string.h>
#include <utility>
#include <vector>

namespace dsd_qt {

/* Every field a Channels row reads from the view: rows are built again only when one of these moved. */
struct DmrChannelsModel::ChannelInputs {
    struct Entry {
        uint16_t lpcn = 0;
        uint8_t hidden = 0;
        long base_hz = 0;

        bool
        operator==(const Entry& other) const {
            return lpcn == other.lpcn && hidden == other.hidden && base_hz == other.base_hz;
        }
    };

    std::array<uint8_t, DSD_UI_DMR_T3_ROWS> origin{};
    std::array<long, DSD_UI_DMR_T3_ROWS> hz{};
    std::vector<Entry> overlay;

    explicit ChannelInputs(const dsd_ui_dmr_t3_view& view) {
        std::copy(view.origin, view.origin + DSD_UI_DMR_T3_ROWS, origin.begin());
        std::copy(view.channel_hz, view.channel_hz + DSD_UI_DMR_T3_ROWS, hz.begin());
        const uint16_t count = view.overlay_count <= DSD_UI_DMR_T3_OVERLAY_MAX_ROWS ? view.overlay_count : 0U;
        overlay.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            Entry e;
            e.lpcn = view.overlay[i].lpcn;
            e.hidden = view.overlay[i].hidden;
            e.base_hz = view.overlay[i].base_hz;
            overlay.push_back(e);
        }
    }

    bool
    operator==(const ChannelInputs& other) const {
        return origin == other.origin && hz == other.hz && overlay == other.overlay;
    }
};

namespace {

/* A list's name as text: only whole, well-formed characters (a name can be cut mid-character). */
QString
name_text(const char* name) {
    const size_t n = strnlen(name, 64);
    return QString::fromUtf8(name, static_cast<qsizetype>(dsd_utf8_whole_prefix_len(name, n, n)));
}

QString
radio_name(const dsd_state* snapshot, uint32_t id) {
    char name[64] = {0};
    return id != 0U && dsd_source_label_lookup(snapshot, id, nullptr, 0U, name, sizeof name) == 1 ? name_text(name)
                                                                                                  : QString();
}

QString
group_name(const dsd_state* snapshot, uint32_t tg) {
    char name[64] = {0};
    return tg != 0U && dsd_tg_policy_lookup_group_label(snapshot, tg, nullptr, 0U, name, sizeof name) == 1
               ? name_text(name)
               : QString();
}

/* The overlay rows, by LPCN ascending, walked once beside a walk of the LPCNs. */
class OverlayCursor {
  public:
    explicit OverlayCursor(const dsd_ui_dmr_t3_view& view)
        : m_rows(view.overlay),
          m_count(view.overlay_count <= DSD_UI_DMR_T3_OVERLAY_MAX_ROWS ? view.overlay_count : 0U) {}

    /* The row of @p lpcn, or nullptr; @p lpcn never goes down between calls. */
    const dsd_ui_dmr_t3_overlay_row*
    at(uint16_t lpcn) {
        while (m_next < m_count && m_rows[m_next].lpcn < lpcn) {
            ++m_next;
        }
        return m_next < m_count && m_rows[m_next].lpcn == lpcn ? &m_rows[m_next] : nullptr;
    }

  private:
    const dsd_ui_dmr_t3_overlay_row* m_rows;
    uint16_t m_count;
    uint16_t m_next = 0;
};

/* Ledger channel @p lpcn as a row: with the latest grant that named it while the ledger details it. */
QVariantMap
missing_row(const dsd_ui_dmr_t3_view& view, uint16_t lpcn, const dsd_state* snapshot) {
    const uint8_t details = view.missing.detail_count <= DSD_UI_DMR_T3_MISSING_DETAILS ? view.missing.detail_count : 0U;
    for (uint8_t i = 0; i < details; ++i) {
        const dsd_ui_dmr_t3_missing_detail& d = view.missing.details[i];
        if (d.lpcn != lpcn) {
            continue;
        }
        return {{QStringLiteral("lpcn"), lpcn},
                {QStringLiteral("detailed"), true},
                {QStringLiteral("slot"), (d.slot & 1U) + 1},
                {QStringLiteral("group"), d.group != 0U},
                {QStringLiteral("data"), d.data != 0U},
                {QStringLiteral("unverified"), d.unverified != 0U},
                {QStringLiteral("caller"), QVariant::fromValue<quint32>(d.caller)},
                {QStringLiteral("recipient"), QVariant::fromValue<quint32>(d.recipient)},
                {QStringLiteral("callerName"), radio_name(snapshot, d.caller)},
                {QStringLiteral("recipientName"),
                 d.group != 0U ? group_name(snapshot, d.recipient) : radio_name(snapshot, d.recipient)},
                {QStringLiteral("lastHeardMs"), QVariant::fromValue<qint64>(static_cast<qint64>(d.last_heard_ms))}};
    }
    /* Past the ledger's details only the channel is known. */
    return {{QStringLiteral("lpcn"), lpcn},
            {QStringLiteral("detailed"), false},
            {QStringLiteral("slot"), 0},
            {QStringLiteral("group"), false},
            {QStringLiteral("data"), false},
            {QStringLiteral("unverified"), ((view.missing.checked[lpcn >> 3] >> (lpcn & 7U)) & 1U) == 0U},
            {QStringLiteral("caller"), QVariant::fromValue<quint32>(0U)},
            {QStringLiteral("recipient"), QVariant::fromValue<quint32>(0U)},
            {QStringLiteral("callerName"), QString()},
            {QStringLiteral("recipientName"), QString()},
            {QStringLiteral("lastHeardMs"), QVariant::fromValue<qint64>(0)}};
}

} // namespace

DmrChannelsModel::DmrChannelsModel(QObject* parent) : QObject(parent), m_decodeNowMs(&dsd_decode_now_mono_ms) {}

DmrChannelsModel::~DmrChannelsModel() = default;

bool
DmrChannelsModel::State::operator==(const State& other) const {
    return present == other.present && tier3 == other.tier3 && tier3Unchecked == other.tier3Unchecked
           && ccConfirmedHere == other.ccConfirmedHere && trunkScan == other.trunkScan
           && otherSystemHere == other.otherSystemHere && dmrDecoding == other.dmrDecoding && ccHz == other.ccHz
           && generation == other.generation && revision == other.revision && session == other.session;
}

QString
DmrChannelsModel::originKey(int originCode, bool hidden) {
    if (hidden) {
        return QStringLiteral("hidden");
    }
    switch (originCode) {
        case DSD_UI_DMR_T3_ORIGIN_FILE: return QStringLiteral("file");
        case DSD_UI_DMR_T3_ORIGIN_USER: return QStringLiteral("user");
        case DSD_UI_DMR_T3_ORIGIN_SITE: return QStringLiteral("site");
        case DSD_UI_DMR_T3_ORIGIN_UNVERIFIED: return QStringLiteral("unchecked");
        case DSD_UI_DMR_T3_ORIGIN_NEIGHBOUR: return QStringLiteral("neighbour");
        case DSD_UI_DMR_T3_ORIGIN_ESTIMATE: return QStringLiteral("estimated");
        default: return QString();
    }
}

void
DmrChannelsModel::setActive(bool enabled) {
    if (m_active == enabled) {
        return;
    }
    m_active = enabled;
    Q_EMIT activeChanged();
}

void
DmrChannelsModel::setSessionSystemUid(const QString& uid) {
    if (m_sessionSystemUid == uid) {
        return;
    }
    m_sessionSystemUid = uid;
    Q_EMIT stateChanged();
}

void
DmrChannelsModel::publishState(const State& next) {
    if (next == m_state) {
        return;
    }
    m_state = next;
    Q_EMIT stateChanged();
}

void
DmrChannelsModel::publishMissing(const QVariantList& recentRows, const QVariantList& byChannelRows) {
    if (recentRows != m_missing || byChannelRows != m_missingByChannel) {
        m_missing = recentRows;
        m_missingByChannel = byChannelRows;
        Q_EMIT missingChanged();
    }
}

void
DmrChannelsModel::publishChannels(const QVariantList& channelRows) {
    if (channelRows != m_channels) {
        m_channels = channelRows;
        Q_EMIT channelsChanged();
    }
}

void
DmrChannelsModel::publishLists(const QVariantList& recentRows, const QVariantList& byChannelRows,
                               const QVariantList& channelRows) {
    publishMissing(recentRows, byChannelRows);
    publishChannels(channelRows);
}

void
DmrChannelsModel::clear() {
    m_built = false;
    m_contentSeq = 0;
    m_nameKey = 0;
    m_channelInputs.reset();
    publishLists({}, {}, {});
    publishState(State());
}

void
DmrChannelsModel::replaceForTarget(const dsd_opts* opts_snapshot, const dsd_state* snapshot, quint64 commandSession) {
    if (m_active && snapshot != nullptr) {
        m_built = false; // one replacement: the new target's rows straight over the old ones
        refresh(opts_snapshot, snapshot, commandSession);
    } else {
        clear();
    }
}

void
DmrChannelsModel::refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot, quint64 commandSession) {
    if (!m_active) {
        return;
    }
    const dsd_ui_dmr_t3_view* view = dsd_app_dmr_t3_view(snapshot);
    if (view == nullptr || view->present == 0U) {
        m_built = false;
        publishLists({}, {}, {});
        State none;
        none.trunkScan = opts_snapshot != nullptr && opts_snapshot->trunk_scan_enabled == 1;
        none.dmrDecoding = view == nullptr || view->dmr_decoding != 0U;
        publishState(none);
        return;
    }
    State next;
    next.present = true;
    next.tier3 = view->tier3 != 0U;
    next.tier3Unchecked = view->tier3_unchecked != 0U;
    next.ccConfirmedHere = view->cc_confirmed_here != 0U;
    next.ccHz = next.ccConfirmedHere ? static_cast<qint64>(view->cc_hz) : 0;
    next.generation = view->generation;
    next.revision = view->revision;
    next.session = commandSession != 0U ? QString::number(commandSession) : QString();
    next.trunkScan = opts_snapshot != nullptr && opts_snapshot->trunk_scan_enabled == 1;
    next.otherSystemHere = view->other_system_here != 0U;
    next.dmrDecoding = view->dmr_decoding != 0U;
    const uint64_t nameKey = dsd_alias_name_key(snapshot);
    if (next.otherSystemHere || !next.dmrDecoding) {
        /* Another system's map, or a session that no longer decodes DMR: no Tier III rows, and nothing to edit. The
           flags can move without a new body, so the rows are read again once they clear. */
        m_built = false;
        publishLists({}, {}, {});
    } else {
        const bool bodyMoved = !m_built || view->content_seq != m_contentSeq;
        if (bodyMoved || nameKey != m_nameKey) {
            rebuildMissing(*view, snapshot); // only these rows carry names
        }
        if (bodyMoved) {
            rebuildChannels(*view, !m_built);
        }
        m_built = true;
        m_contentSeq = view->content_seq;
        m_nameKey = nameKey;
    }
    publishState(next);
}

void
DmrChannelsModel::rebuildMissing(const dsd_ui_dmr_t3_view& view, const dsd_state* snapshot) {
    QVariantList recent, byChannel;
    /* The details first, newest first, then every other ledger channel by LPCN. */
    const uint8_t details = view.missing.detail_count <= DSD_UI_DMR_T3_MISSING_DETAILS ? view.missing.detail_count : 0U;
    for (uint8_t i = 0; i < details; ++i) {
        const uint16_t lpcn = view.missing.details[i].lpcn;
        if (dsd_app_dmr_t3_view_missing_has(&view, lpcn)) {
            recent.append(missing_row(view, lpcn, snapshot));
        }
    }
    for (uint16_t lpcn = 1U; lpcn <= static_cast<uint16_t>(maxChannel()); ++lpcn) {
        if (dsd_app_dmr_t3_view_missing_has(&view, lpcn)) {
            const QVariantMap row = missing_row(view, lpcn, snapshot);
            byChannel.append(row);
            if (!row.value(QStringLiteral("detailed")).toBool()) {
                recent.append(row);
            }
        }
    }
    publishMissing(recent, byChannel);
}

void
DmrChannelsModel::rebuildChannels(const dsd_ui_dmr_t3_view& view, bool force) {
    auto inputs = std::make_unique<ChannelInputs>(view);
    if (!force && m_channelInputs && *m_channelInputs == *inputs) {
        return; // a grant heard again, a name learned: nothing a Channels row shows moved
    }
    m_channelInputs = std::move(inputs);
    ++m_channelRowBuilds;
    QVariantList rows;
    OverlayCursor overlay(view);
    for (uint16_t lpcn = 1U; lpcn <= static_cast<uint16_t>(maxChannel()); ++lpcn) {
        const int origin = view.origin[lpcn];
        if (origin == DSD_UI_DMR_T3_ORIGIN_NONE) {
            continue;
        }
        const dsd_ui_dmr_t3_overlay_row* entry = overlay.at(lpcn);
        const bool hidden = entry != nullptr && entry->hidden != 0U;
        /* The frequency DMR uses (the user's entry over the shared map row), which an edit's STALE check compares
           against: another protocol can empty or fill the shared row under an entry. */
        rows.append(
            QVariantMap{{QStringLiteral("lpcn"), lpcn},
                        {QStringLiteral("hz"), QVariant::fromValue<qint64>(static_cast<qint64>(view.channel_hz[lpcn]))},
                        {QStringLiteral("origin"), originKey(origin, hidden)},
                        {QStringLiteral("originCode"), origin},
                        {QStringLiteral("baseHz"), QVariant::fromValue<qint64>(entry != nullptr ? entry->base_hz : 0)},
                        {QStringLiteral("hidden"), hidden},
                        {QStringLiteral("yours"), entry != nullptr}});
    }
    publishChannels(rows);
}

QVariantMap
DmrChannelsModel::channel(int lpcn) const {
    QVariantMap out{{QStringLiteral("lpcn"), lpcn},
                    {QStringLiteral("valid"),
                     lpcn >= minChannel() && lpcn <= maxChannel() && !m_state.otherSystemHere && m_state.dmrDecoding},
                    {QStringLiteral("hz"), QVariant::fromValue<qint64>(0)},
                    {QStringLiteral("originCode"), static_cast<int>(DSD_UI_DMR_T3_ORIGIN_NONE)},
                    {QStringLiteral("origin"), QString()},
                    {QStringLiteral("baseHz"), QVariant::fromValue<qint64>(0)},
                    {QStringLiteral("hidden"), false},
                    {QStringLiteral("yours"), false},
                    {QStringLiteral("missing"), false}};
    for (const QVariant& value : m_channels) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("lpcn")).toInt() == lpcn) {
            for (auto it = row.cbegin(); it != row.cend(); ++it) {
                out.insert(it.key(), it.value());
            }
            break;
        }
    }
    out.insert(QStringLiteral("missing"),
               std::any_of(m_missingByChannel.cbegin(), m_missingByChannel.cend(), [lpcn](const QVariant& value) {
                   return value.toMap().value(QStringLiteral("lpcn")).toInt() == lpcn;
               }));
    return out;
}

qint64
DmrChannelsModel::decodeNowMs() const {
    return static_cast<qint64>(m_decodeNowMs());
}

} // namespace dsd_qt
