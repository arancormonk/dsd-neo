// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QList>
#include <QMap>
#include <QVariantMap>
#include <dsd-neo/app_control/p25_metrics.h>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/key_material.h>
#include <stddef.h>
#include <stdint.h>
#include <utility>
#include "metrics_model.h"

#include <QStringList>
#include <algorithm>
#include <iterator>
#include <tuple>

#include <QChar>
#include <QDateTime>
#include <QtGlobal>
#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/scan_mode.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

namespace dsd_qt {

namespace {

/**
 * @brief How long a lock keeps reading as locked after the last synced frame.
 *
 * Long enough to ride out the gaps between frames a 250 ms poll lands in, short
 * enough that a decoder which has genuinely stopped finding anything says so
 * while the reader is still looking at it.
 *
 * Shared with the Android notification through app-control, so the two surfaces cannot
 * drift apart on when a session stops reading as locked.
 */
constexpr double kSyncHoldSeconds = DSD_APP_SYNC_HOLD_S;

/**
 * @brief "ALG 84 · KID 0001" when the crypto header decoded, else empty.
 *
 * What the terminal UI's slot line showed, so AES and RC4 traffic read differently at a
 * glance; the ENC tag alone covers "encrypted, alg unknown".
 */
QString
slot_enc_text(quint8 algid, quint16 kid) {
    if (algid == 0U) {
        return QString();
    }
    return QStringLiteral("ALG %1 · KID %2")
        .arg(algid, 2, 16, QLatin1Char('0'))
        .arg(kid, 4, 16, QLatin1Char('0'))
        .toUpper();
}

template <size_t N>
QString
siteText(const char (&text)[N]) {
    return QString::fromUtf8(text, static_cast<int>(std::find(text, text + N, '\0') - text));
}

bool
fecEquals(const dsd_app_fec_ratio& a, const dsd_app_fec_ratio& b) {
    return a.valid == b.valid && a.ok == b.ok && a.err == b.err && a.ok_pct == b.ok_pct;
}

bool
voiceEquals(const dsd_app_voice_errs& a, const dsd_app_voice_errs& b) {
    return a.valid == b.valid && a.samples == b.samples && a.errs_per_frame == b.errs_per_frame;
}

bool
frameEquals(const dsd_app_frame_errs& a, const dsd_app_frame_errs& b) {
    return a.valid == b.valid && a.errs == b.errs && a.errs2 == b.errs2;
}

} // namespace

/**
 * @brief Structured identity for one slot, display-ready for the hero panel.
 *
 * Thin adapter over the shared app-control view: it owns the ended-hold decay, the
 * identity-less-epoch suppression and the staged CSV group name lookup, so the
 * terminal, this panel and the Android notification cannot drift apart on any of them.
 */
MetricsModel::SlotCall
MetricsModel::slotCallView(const dsd_state* snapshot, quint8 slot, double now_m) {
    MetricsModel::SlotCall out;
    dsd_app_slot_call view;
    dsd_app_slot_call_view(snapshot, slot, now_m, &view);

    out.state = view.state;
    if (view.state != DSD_APP_CALL_LINE_ACTIVE && view.state != DSD_APP_CALL_LINE_ENDED) {
        return out;
    }

    out.tg_text = QString::fromUtf8(view.tg_text);
    out.tg_id = static_cast<qulonglong>(view.tg_id);
    out.src_text = QString::fromUtf8(view.src_text);
    out.name = QString::fromUtf8(view.name);
    out.channel = QString::fromUtf8(view.channel);
    out.enc = view.enc != 0U;
    out.emergency = view.emergency != 0U;
    out.priority = view.priority;
    if (out.enc) {
        out.enc_text = slot_enc_text(view.algid, view.kid);
    }
    out.seconds = static_cast<int>(view.elapsed_ms / 1000U);
    return out;
}

bool
MetricsModel::View::qualityEquals(const View& other) const {
    return quality.valid == other.quality.valid && fecEquals(quality.cc_fec, other.quality.cc_fec)
           && fecEquals(quality.voice_fec, other.quality.voice_fec) && fecEquals(quality.rs, other.quality.rs)
           && voiceEquals(quality.p1_voice, other.quality.p1_voice)
           && voiceEquals(quality.p2_voice[0], other.quality.p2_voice[0])
           && voiceEquals(quality.p2_voice[1], other.quality.p2_voice[1])
           && frameEquals(quality.last_frame[0], other.quality.last_frame[0])
           && frameEquals(quality.last_frame[1], other.quality.last_frame[1])
           && voiceEquals(voice_errs, other.voice_errs) && frameEquals(last_frame, other.last_frame);
}

bool
MetricsModel::SiteView::operator==(const SiteView& other) const {
    return std::tie(siteProtocol, p25NacValid, p25Nac, p25WacnValid, p25Wacn, p25SysIdValid, p25SysId, p25Rfss, p25Site,
                    p25LraValid, p25Lra, p25Phase2ParamsReady, dmrColorCode, dmrSiteText, dmrRestLsn, nxdnRan,
                    nxdnLocationCategory, nxdnSysCode, nxdnSiteCode, edacsSiteText, ccFreqHz, vcFreqHz, siteLine,
                    siteConfirmed)
           == std::tie(other.siteProtocol, other.p25NacValid, other.p25Nac, other.p25WacnValid, other.p25Wacn,
                       other.p25SysIdValid, other.p25SysId, other.p25Rfss, other.p25Site, other.p25LraValid,
                       other.p25Lra, other.p25Phase2ParamsReady, other.dmrColorCode, other.dmrSiteText,
                       other.dmrRestLsn, other.nxdnRan, other.nxdnLocationCategory, other.nxdnSysCode,
                       other.nxdnSiteCode, other.edacsSiteText, other.ccFreqHz, other.vcFreqHz, other.siteLine,
                       other.siteConfirmed);
}

static void
appendSiteHex(QStringList& parts, const QString& label, int value, int width) {
    parts << label + QLatin1Char(' ') + QStringLiteral("%1").arg(value, width, 16, QLatin1Char('0')).toUpper();
}

static void
appendSiteDecimal(QStringList& parts, const QString& label, int value) {
    parts << label + QLatin1Char(' ') + QString::number(value);
}

void
MetricsModel::fillP25Identity(SiteView& site, const dsd_state* snapshot) {
    site.siteProtocol = QStringLiteral("P25");
    site.p25NacValid = snapshot->p2_cc > 0 && snapshot->p2_cc < 0xFFF;
    site.p25WacnValid = snapshot->p2_wacn > 0 && snapshot->p2_wacn < 0xFFFFF;
    site.p25SysIdValid = snapshot->p2_sysid > 0 && snapshot->p2_sysid < 0xFFF;
    site.p25Nac = site.p25NacValid ? static_cast<int>(snapshot->p2_cc) : 0;
    site.p25Wacn = site.p25WacnValid ? static_cast<int>(snapshot->p2_wacn) : 0;
    site.p25SysId = site.p25SysIdValid ? static_cast<int>(snapshot->p2_sysid) : 0;
    site.p25Rfss = snapshot->p2_rfssid <= 255 ? static_cast<int>(snapshot->p2_rfssid) : 0;
    site.p25Site = snapshot->p2_siteid <= 255 ? static_cast<int>(snapshot->p2_siteid) : 0;
    site.p25LraValid = snapshot->p25_site_lra_valid != 0;
    site.p25Lra = site.p25LraValid ? snapshot->p25_site_lra : 0;
    // The terminal's zero/all-ones parameter gate is independent of P1 identity.
    site.p25Phase2ParamsReady = site.p25NacValid && site.p25WacnValid && site.p25SysIdValid;
}

QStringList
MetricsModel::p25SiteParts(const SiteView& site) {
    QStringList parts;
    if (site.p25WacnValid) {
        appendSiteHex(parts, QStringLiteral("WACN"), site.p25Wacn, 5);
    }
    if (site.p25SysIdValid) {
        appendSiteHex(parts, QStringLiteral("SYS"), site.p25SysId, 3);
    }
    if (site.p25NacValid) {
        appendSiteHex(parts, QStringLiteral("NAC"), site.p25Nac, 3);
    }
    if (site.p25Rfss) {
        appendSiteDecimal(parts, QStringLiteral("RFSS"), site.p25Rfss);
    }
    if (site.p25Site) {
        appendSiteDecimal(parts, QStringLiteral("SITE"), site.p25Site);
    }
    if (site.p25LraValid) {
        appendSiteHex(parts, QStringLiteral("LRA"), site.p25Lra, 2);
    }
    return parts;
}

QStringList
MetricsModel::fillDmrSite(SiteView& site, const dsd_state* snapshot) {
    QStringList parts;
    site.siteProtocol = QStringLiteral("DMR");
    site.dmrColorCode = snapshot->dmr_color_code <= 15 ? static_cast<int>(snapshot->dmr_color_code) : -1;
    site.dmrSiteText = siteText(snapshot->dmr_site_parms);
    site.dmrRestLsn = qMax(0, snapshot->dmr_rest_channel);
    if (site.dmrColorCode >= 0) {
        appendSiteDecimal(parts, QStringLiteral("CC"), site.dmrColorCode);
    }
    if (!site.dmrSiteText.isEmpty()) {
        parts << site.dmrSiteText;
    }
    if (site.dmrRestLsn > 0) {
        appendSiteDecimal(parts, QStringLiteral("Rest LSN"), site.dmrRestLsn);
    }
    return parts;
}

QStringList
MetricsModel::fillNxdnSite(SiteView& site, const dsd_state* snapshot) {
    QStringList parts;
    site.nxdnRan = snapshot->nxdn_last_ran <= 63 ? static_cast<int>(snapshot->nxdn_last_ran) : -1;
    site.nxdnLocationCategory = siteText(snapshot->nxdn_location_category);
    const bool idas = site.nxdnLocationCategory == QStringLiteral("Type-D");
    site.siteProtocol = idas ? QStringLiteral("IDAS") : QStringLiteral("NXDN");
    site.nxdnSiteCode = snapshot->nxdn_location_site_code;
    // As in the terminal, a decoded site code establishes location validity.
    site.nxdnSysCode = site.nxdnSiteCode ? static_cast<int>(snapshot->nxdn_location_sys_code) : 0;
    if (site.nxdnRan >= 0) {
        appendSiteDecimal(parts, idas ? QStringLiteral("Area") : QStringLiteral("RAN"), site.nxdnRan);
    }
    if (site.nxdnSiteCode) {
        if (!site.nxdnLocationCategory.isEmpty()) {
            parts << site.nxdnLocationCategory;
        }
        appendSiteDecimal(parts, QStringLiteral("SYS"), site.nxdnSysCode);
        appendSiteDecimal(parts, QStringLiteral("SITE"), site.nxdnSiteCode);
    }
    return parts;
}

QStringList
MetricsModel::fillEdacsSite(SiteView& site, const dsd_state* snapshot) {
    QStringList parts;
    site.siteProtocol = QStringLiteral("EDACS");
    if (snapshot->edacs_site_id) {
        site.edacsSiteText =
            QStringLiteral("SITE %1 [%2] · %3 · %4")
                .arg(snapshot->edacs_site_id, 3, 10, QLatin1Char('0'))
                .arg(QStringLiteral("%1").arg(snapshot->edacs_site_id, 2, 16, QLatin1Char('0')).toUpper())
                .arg(snapshot->ea_mode == 1 ? QStringLiteral("Extended Addressing")
                                            : QStringLiteral("Standard/Networked"))
                .arg(snapshot->esk_mask == 0xA0 ? QStringLiteral("ESK") : QStringLiteral("No ESK"));
        parts << site.edacsSiteText;
    }
    return parts;
}

void
MetricsModel::fillSiteView(View& next, const dsd_state* snapshot) const {
    auto& site = next.site;
    // No snapshot pointer survives the tick. Retain the last copied identity on
    // loss, even if the no-carrier path has already reset the decoder's fields.
    if (snapshot->synctype == DSD_SYNC_NONE) {
        site = m_view.site;
        site.siteConfirmed = false;
        return;
    }
    QStringList parts;
    if (DSD_SYNC_IS_P25(snapshot->synctype)) {
        fillP25Identity(site, snapshot);
        parts = p25SiteParts(site);
    } else if (DSD_SYNC_IS_DMR(snapshot->synctype)) {
        parts = fillDmrSite(site, snapshot);
    } else if (DSD_SYNC_IS_NXDN(snapshot->synctype)) {
        parts = fillNxdnSite(site, snapshot);
    } else if (DSD_SYNC_IS_EDACS(snapshot->synctype)) {
        parts = fillEdacsSite(site, snapshot);
    }
    if (!site.siteProtocol.isEmpty()) {
        site.ccFreqHz = qMax(0L, snapshot->trunk_cc_freq != 0 ? snapshot->trunk_cc_freq : snapshot->p25_cc_freq);
        site.vcFreqHz =
            qMax(0L, snapshot->trunk_vc_freq[0] != 0 ? snapshot->trunk_vc_freq[0] : snapshot->p25_vc_freq[0]);
    }
    if (!parts.isEmpty()) {
        parts.prepend(site.siteProtocol);
        site.siteLine = parts.join(QStringLiteral(" · "));
        site.siteConfirmed = true;
    }
}

MetricsModel::MetricsModel(QObject* parent) : QObject(parent) {
    m_messageTimer.setSingleShot(true);
    connect(&m_messageTimer, &QTimer::timeout, this, [this]() {
        View next = m_view;
        next.ui_message.clear();
        publish(next);
    });
}

MetricsModel::~MetricsModel() = default;

bool
MetricsModel::View::operator==(const View& other) const {
    return site == other.site && qualityEquals(other) && tunerEquals(other) && slot_call[0] == other.slot_call[0]
           && slot_call[1] == other.slot_call[1] && controlEquals(other) && ui_message == other.ui_message;
}

void
MetricsModel::publish(const View& next) {
    if (next == m_view) {
        return;
    }
    const bool siteMoved = !(next.site == m_view.site);
    const bool qualityMoved = !next.qualityEquals(m_view);
    const bool tunerMoved = !next.tunerEquals(m_view);
    const bool slot1Moved = !(next.slot_call[0] == m_view.slot_call[0]);
    const bool slot2Moved = !(next.slot_call[1] == m_view.slot_call[1]);
    const bool controlMoved = !next.controlEquals(m_view);
    const bool messageMoved = next.ui_message != m_view.ui_message;
    m_view = next;
    if (siteMoved) {
        Q_EMIT siteChanged();
    }
    if (qualityMoved) {
        Q_EMIT qualityChanged();
    }
    if (tunerMoved) {
        Q_EMIT tunerChanged();
    }
    if (slot1Moved) {
        Q_EMIT slot1Changed();
    }
    if (slot2Moved) {
        Q_EMIT slot2Changed();
    }
    if (slot1Moved || slot2Moved) {
        Q_EMIT leadSlotChanged();
    }
    if (controlMoved) {
        Q_EMIT controlChanged();
    }
    if (messageMoved) {
        Q_EMIT uiMessageChanged();
    }
}

void
MetricsModel::clear() {
    m_messageTimer.stop();
    /* The latch is per-session state, not part of the published frame: a stopped
     * session that starts again on the same frequency must not inherit the old
     * session's answer to "is there anything here". */
    m_sync_type_here = DSD_SYNC_NONE;
    m_sync_seen_m = 0.0;
    publish(View());
}

/**
 * @brief Fill in what the decoder is doing and what it is set to.
 *
 * The settings are read from the options snapshot rather than remembered from the
 * last command, because a command can be refused and, on Android, the service
 * that owns them outlives this process.
 */
/*
 * Scan hold and avoids read whichever rotation owns the tuner: the coordinator's
 * publication under --trunk-scan, the scan-list flags under -Y. Plain trunking is not
 * a rotation, so the controls have nothing to act on and the gate stays false.
 */
void
MetricsModel::fillScanControlView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot) {
    const bool trunk_scan = opts_snapshot->trunk_scan_enabled != 0;
    if (trunk_scan) {
        next.scan_target_id = QString::fromUtf8(snapshot->trunk_scan_active_id);
        next.scan_target_ordinal = snapshot->trunk_scan_active_ordinal;
        next.scan_target_count = snapshot->trunk_scan_target_count;
    }
    next.scan_rotation_active = opts_snapshot->scanner_mode != 0 || trunk_scan;
    next.scan_hold = trunk_scan ? (snapshot->trunk_scan_hold != 0) : (snapshot->lcn_scan_hold != 0);
    next.scan_avoid_count = trunk_scan ? snapshot->trunk_scan_avoided_count : snapshot->lcn_avoid_count;
    next.scan_target_avoided = trunk_scan && snapshot->trunk_scan_active_avoided != 0;
}

/**
 * @brief Why the rotation is staying on the row on air, and how long is left (#508).
 *
 * Every decision behind the row -- the phrase, whether a window is counting down,
 * which of the dwell/hold/hang budgets is worth printing -- is made once in
 * app-control, from a publication whose deadlines the decoder owns. A frontend only
 * copies and formats, so this panel, the terminal row and Android cannot drift apart
 * on what "suspended" means or on when a trunked row may show a conventional hold.
 *
 * The withheld budgets are zeroed rather than carried: QML gates each group on its
 * own reading being positive, so a value the view declined to show must not arrive
 * as a number the row could print.
 */
void
MetricsModel::fillScanTimingView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot,
                                 double now_m) const {
    dsd_app_scan_timing view;
    if (dsd_app_scan_timing_view(opts_snapshot, snapshot, now_m, &view) != 1) {
        return;
    }
    next.scan_timing_visible = view.active != 0U;
    next.scan_stay_reason = view.reason;
    /* A static English label out of a fixed set, translated at run time. lupdate
     * cannot extract through the pointer; the set is small enough that a catalogue
     * lists it beside the terminal's own wording rather than duplicating it here. */
    next.scan_stay_phrase = view.phrase != nullptr ? tr(view.phrase) : QString();
    next.scan_timer_live = view.timer_live != 0U;
    /* Tenths, truncated: see scanTimerRemainingDs(). */
    next.scan_timer_remaining_ds = static_cast<int>(view.remaining_ms / 100U);
    next.scan_timer_span_ms = static_cast<int>(view.span_ms);
    next.scan_dwell_ms = view.show_dwell != 0U ? static_cast<int>(view.dwell_ms) : 0;
    next.scan_dwell_state = view.dwell_state;
    next.scan_hold_ms = view.show_hold != 0U ? static_cast<int>(view.hold_ms) : 0;
    next.scan_hang_ms = view.show_hang != 0U ? static_cast<int>(view.hang_ms) : 0;
    /* The cap on the whole visit (#507), withheld the same way when none applies. */
    next.scan_visit_ms = view.show_visit != 0U ? static_cast<int>(view.visit_ms) : 0;
    next.scan_visit_live = view.visit_live != 0U;
    next.scan_visit_remaining_ds = static_cast<int>(view.visit_remaining_ms / 100U);
}

namespace {
struct DecryptionView {
    const dsd_call_snapshot& call;
    const dsd_call_key_selection& selected;
    const dsd_opts* opts;
    const dsd_state* state;
    bool current, encrypted, dmr;

    DecryptionView(const dsd_call_snapshot& c, const dsd_opts* o, const dsd_state* s)
        : call(c), selected(c.key_selection), opts(o), state(s),
          current(selected.valid && selected.key_epoch == s->enc_lockout_key_epoch && selected.signaled_id == c.kid
                  && selected.algorithm == c.algid),
          encrypted(c.crypto >= DSD_CALL_CRYPTO_ENCRYPTED_PENDING), dmr(DSD_SYNC_IS_DMR(c.protocol)) {}

    QString
    source() const {
        const QStringList sources{QStringLiteral("Source not reported"), QStringLiteral("Direct key override"),
                                  QStringLiteral("Received key ID"),     QStringLiteral("Talkgroup override"),
                                  QStringLiteral("Destination lookup"),  QStringLiteral("Default / existing material")};
        const QString source = current && selected.source < sources.size() ? sources[selected.source]
                               : state->keyloader ? QStringLiteral("Automatic key collection")
                                                  : QStringLiteral("Direct / existing material");
        return source;
    }

    QString
    availability() const {
        if (call.crypto == DSD_CALL_CRYPTO_CLEAR) {
            return QStringLiteral("No decryption needed");
        }
        if (!current && selected.valid) {
            return QStringLiteral("Waiting for key reevaluation");
        }
        if (current && selected.available >= 0) {
            return selected.available == 1 ? QStringLiteral("Key material available")
                                           : QStringLiteral("No usable key material");
        }
        return call.crypto == DSD_CALL_CRYPTO_DECRYPTABLE ? QStringLiteral("Key material available")
                                                          : QStringLiteral("Key availability not yet known");
    }

    QString
    fallback() const {
        QString fallback;
        if (current && selected.fallback == DSD_CALL_KEY_FALLBACK_MAPPED_MISSING) {
            fallback = QStringLiteral("Mapped key is absent; using received key ID.");
        }
        if (current && selected.fallback == DSD_CALL_KEY_FALLBACK_MAPPED_INCOMPATIBLE) {
            fallback = QStringLiteral("Mapped material is incompatible; using received key ID.");
        }
        if (current && selected.fallback == DSD_CALL_KEY_FALLBACK_UNKNOWN_ALGORITHM) {
            fallback = QStringLiteral("This algorithm does not use the standard keyring mapping.");
        }
        return fallback;
    }

    QString
    block() const {
        QString block;
        if ((call.kind == DSD_CALL_KIND_GROUP_VOICE || call.kind == DSD_CALL_KIND_PRIVATE_VOICE)
            && call.policy_target_id > 0 && call.policy_target_id <= UINT32_MAX && call.ota_source_id <= UINT32_MAX) {
            dsd_tg_policy_decision policy = {};
            const int needs_key = encrypted && call.crypto != DSD_CALL_CRYPTO_DECRYPTABLE;
            if (call.kind == DSD_CALL_KIND_PRIVATE_VOICE) {
                dsd_tg_policy_evaluate_private_call(opts, state, static_cast<uint32_t>(call.ota_source_id),
                                                    static_cast<uint32_t>(call.policy_target_id), needs_key, 0,
                                                    &policy);
            } else {
                dsd_tg_policy_evaluate_group_call(opts, state, static_cast<uint32_t>(call.policy_target_id),
                                                  static_cast<uint32_t>(call.ota_source_id), needs_key, 0, &policy);
            }
            if (policy.block_reasons) {
                block = QString::fromUtf8(dsd_tg_policy_block_reason_label(policy.block_reasons));
            }
        }
        return block;
    }

    QString
    materialKind() const {
        const auto need = dsd_dmr_alg_key_need(call.algid);
        QString materialKind = need == DSD_KEY_NEED_AES_2                                   ? QStringLiteral("aes128")
                               : need == DSD_KEY_NEED_AES_3                                 ? QStringLiteral("tdea")
                               : need == DSD_KEY_NEED_AES_4 || need == DSD_KEY_NEED_QUARTET ? QStringLiteral("aes256")
                                                                                            : QStringLiteral("scalar");
        if (DSD_SYNC_IS_NXDN(call.protocol)) {
            materialKind = call.algid == 1   ? QStringLiteral("scrambler")
                           : call.algid == 3 ? QStringLiteral("aes256")
                                             : QStringLiteral("scalar");
        }
        return materialKind;
    }

    QString
    kid() const {
        const bool keyedProtocol =
            DSD_SYNC_IS_P25(call.protocol) || DSD_SYNC_IS_DMR(call.protocol) || DSD_SYNC_IS_NXDN(call.protocol);
        const QString kid = keyedProtocol && encrypted && (!dmr || call.kid <= 255)
                                    && (!current || selected.source != DSD_CALL_KEY_DESTINATION)
                                ? QString::number(call.kid, 16).toUpper()
                                : QString();
        return kid;
    }

    QString
    effective() const {
        const QString effective =
            current && selected.effective_id >= 0
                    && (selected.source == DSD_CALL_KEY_DESTINATION || !dmr || selected.effective_id <= 255)
                ? QString::number(selected.effective_id, selected.source == DSD_CALL_KEY_DESTINATION ? 10 : 16)
                      .toUpper()
                : QString();
        return effective;
    }

    QString
    protocol() const {
        if (DSD_SYNC_IS_P25(call.protocol)) {
            return "p25";
        }
        if (dmr) {
            return "dmr";
        }
        if (DSD_SYNC_IS_NXDN(call.protocol)) {
            return "nxdn";
        }
        if (DSD_SYNC_IS_M17(call.protocol)) {
            return "m17";
        }
        if (DSD_SYNC_IS_DPMR(call.protocol)) {
            return "dpmr";
        }
        if (DSD_SYNC_IS_DSTAR(call.protocol)) {
            return "dstar";
        }
        if (DSD_SYNC_IS_YSF(call.protocol)) {
            return "ysf";
        }
        return "unknown";
    }

    QString
    status() const {
        if (call.crypto == DSD_CALL_CRYPTO_CLEAR) {
            return QStringLiteral("Unencrypted");
        }
        if (call.crypto == DSD_CALL_CRYPTO_UNKNOWN) {
            return QStringLiteral("Unknown / waiting for signaling");
        }
        return QStringLiteral("Encrypted");
    }
};
} // namespace

static QVariantList
decryption_slot_views(const dsd_opts* opts, const dsd_state* state) {
    QVariantList result;
    for (uint8_t slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; ++slot) {
        dsd_call_snapshot call = {};
        if (dsd_call_state_get(state, slot, &call) <= 0 || call.phase == DSD_CALL_PHASE_IDLE) {
            continue;
        }
        const DecryptionView view(call, opts, state);
        result.append(QVariantMap{
            {"slot", slot + 1},
            {"status", view.status()},
            {"protocol", view.protocol()},
            {"lastObserved", call.phase == DSD_CALL_PHASE_ENDED},
            {"algorithm", view.encrypted && call.algid ? QString::number(call.algid, 16).toUpper() : QString()},
            {"keyId", view.kid()},
            {"effectiveId", view.effective()},
            {"source", view.source()},
            {"availability", view.availability()},
            {"fallback", view.fallback()},
            {"blockReason", view.block()},
            {"materialKind", view.materialKind()},
            {"targetId", QString::number(call.ota_target_id)},
            {"group", call.kind == DSD_CALL_KIND_GROUP_VOICE},
            {"privateCall", call.kind == DSD_CALL_KIND_PRIVATE_VOICE},
            {"dmr", view.dmr},
            {"profileRef", view.current ? QString::fromUtf8(call.key_selection.profile_ref)
                                        : QString::fromUtf8(state->key_profile_ref)}});
    }
    return result;
}

void
MetricsModel::fillDecoderView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot, double now_m) {
    const auto* configured = dsd_scan_mode_configured_view(snapshot);
    next.configured_force = configured ? configured->force_key : snapshot->M;
    next.effective_force = snapshot->M;
    next.key_profile_ref = QString::fromUtf8(snapshot->key_profile_ref);
    next.key_epoch = snapshot->enc_lockout_key_epoch;
    next.automatic_keys = snapshot->keyloader == 1;
    next.decryption_slots = decryption_slot_views(opts_snapshot, snapshot);
    next.scan_mode = QString::fromLatin1(dsd_scan_mode_name(dsd_scan_mode_active(snapshot)));
    next.decode_mode = static_cast<int>(dsd_scan_mode_configured_preset(opts_snapshot, snapshot));

    /* Held for a moment after the last live sync, rather than latched until
     * something explicitly clears it.
     *
     * A hold is needed at all because sync comes and goes between the 250 ms
     * polls, so an instantaneous reading flickers. But it has to expire on its
     * own: the first version cleared the latch on a retune or a decode-mode
     * change, and any such one-shot reset is a race against whatever the poll
     * happens to read on that same frame -- observed as the strip still naming
     * P25 minutes after being told to decode DMR, with no P25 sync in the log at
     * all. Decay answers the question the reader is actually asking ("is it
     * locked now") and cannot get stuck on an answer to a question they stopped
     * asking. lastsynctype is deliberately not consulted: it holds the previous
     * lock until the engine's no-carrier path gets round to clearing it. */
    if (snapshot->synctype != DSD_SYNC_NONE) {
        m_sync_type_here = snapshot->synctype;
        m_sync_seen_m = now_m;
    } else if (m_sync_type_here != DSD_SYNC_NONE && (now_m - m_sync_seen_m) > kSyncHoldSeconds) {
        m_sync_type_here = DSD_SYNC_NONE;
    }
    next.synced_here = m_sync_type_here != DSD_SYNC_NONE;
    if (next.synced_here) {
        next.sync_label = QString::fromUtf8(dsd_synctype_to_string(m_sync_type_here));
    }
    /* Rides the same decayed reading as synced_here rather than the raw snapshot:
     * a control offered on one frame of sync and withdrawn on the next would
     * flicker under the finger. */
    next.trunkable_sync = next.synced_here && DSD_SYNC_IS_TRUNKABLE(m_sync_type_here);
    /* Three states, not two: GFSK is what the DMR and EDACS/ProVoice presets
     * select, and folding it into C4FM made a control bound to this reading show
     * C4FM as already-selected on a session that was never on it. Through the
     * shared helper so this and ui_handle_mod_set()'s skip test cannot drift. */
    next.modulation = configured
                          ? dsd_modulation_from_flags(configured->mod_c4fm, configured->mod_qpsk, configured->mod_gfsk)
                          : dsd_opts_modulation(opts_snapshot);
    /* Gated on radio_input like center_freq_hz above, and for the same reason:
     * on a WAV, UDP, TCP or symbol-file session these are options the front end
     * never applied, and publishing them would put three plausible tuner
     * readings on screen for a session that has no tuner. */
    next.tuner_gain_db = next.radio_input ? opts_snapshot->rtl_gain_value : 0;
    /* rtl_squelch_level is a mean-power threshold, not decibels — the same
     * conversion the engine's own status line uses. Publishing the raw value
     * would put "0" on screen for a squelch of -120 dB. A level that gates
     * nothing is published separately, because pwr_to_dB() renders it as -120
     * too and the panel would otherwise show a threshold that is not in force. */
    next.squelch_db = next.radio_input ? pwr_to_dB(opts_snapshot->rtl_squelch_level) : 0.0;
    next.squelch_off = next.radio_input && dsd_squelch_is_off(opts_snapshot->rtl_squelch_level);
    next.ppm = next.radio_input ? opts_snapshot->rtlsdr_ppm_error : 0;
}

void
MetricsModel::fillQualityView(View& next, const dsd_state* snapshot) {
    dsd_app_p25_quality_from_state(snapshot, &next.quality);
    const int line_states[] = {next.slot_call[0].state, next.slot_call[1].state};
    const int lead = dsd_app_lead_slot(line_states, DSD_CALL_STATE_SLOT_COUNT);
    next.voice_errs = next.quality.p1_voice;
    if (lead >= 0) {
        if (!next.voice_errs.valid) {
            next.voice_errs = next.quality.p2_voice[lead];
        }
        next.last_frame = next.quality.last_frame[lead];
    } else {
        // Late-entry media may precede any decoded identity. The facade already
        // limits these readings to active non-P25 media; keep the first valid
        // slot until the call view can supply its usual identity-based lead.
        const auto& frames = next.quality.last_frame;
        const auto* frame = std::find_if(std::begin(frames), std::end(frames),
                                         [](const dsd_app_frame_errs& item) { return item.valid != 0; });
        if (frame != std::end(frames)) {
            next.last_frame = *frame;
        }
    }
}

QVariantMap
MetricsModel::airspyView(const dsd_opts* opts_snapshot) {
    QVariantMap native;
    if (opts_snapshot && opts_snapshot->audio_in_type == AUDIO_IN_RTL
        && dsd_opts_audio_in_dev_is_airspy_spec(opts_snapshot->audio_in_dev)) {
        const auto& c = opts_snapshot->airspy;
        const char* modes[] = {"sensitivity", "linearity", "manual"};
        native.insert(QStringLiteral("gain_mode"), QString::fromLatin1(modes[std::max(0, std::min(2, c.gain_mode))]));
        native.insert(QStringLiteral("serial"), QString::fromLatin1(c.serial));
        native.insert(QStringLiteral("sample_rate"), c.sample_rate);
        native.insert(QStringLiteral("sensitivity_gain"), c.sensitivity_gain);
        native.insert(QStringLiteral("linearity_gain"), c.linearity_gain);
        native.insert(QStringLiteral("lna_gain"), c.lna_gain);
        native.insert(QStringLiteral("mixer_gain"), c.mixer_gain);
        native.insert(QStringLiteral("vga_gain"), c.vga_gain);
        native.insert(QStringLiteral("lna_agc"), c.lna_agc);
        native.insert(QStringLiteral("mixer_agc"), c.mixer_agc);
        native.insert(QStringLiteral("bias_tee"), c.bias_tee);
        QVariantList rates;
        for (uint32_t i = 0; i < opts_snapshot->airspy_info.rate_count && i < DSD_AIRSPY_MAX_RATES; ++i) {
            rates.append(opts_snapshot->airspy_info.rates[i]);
        }
        native.insert(QStringLiteral("rates"), rates);
        native.insert(QStringLiteral("actual_rate"), opts_snapshot->airspy_info.sample_rate);
        native.insert(QStringLiteral("actual_serial"), QString::fromLatin1(opts_snapshot->airspy_info.serial));
    }
    return native;
}

void
MetricsModel::refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot) {
    dsd_frontend_metrics metrics;
    /* A missing snapshot is the real "nothing to show" case, and it has to be tested
     * for here: the fetch fills defaults for a NULL snapshot rather than failing, so
     * its result alone never distinguishes no data from a healthy decoder reporting
     * zeros. Keeping the last readings would render either as a live one — a locked
     * carrier and a plausible SNR for something that is not reporting. 0 is success
     * below, negative is failure; it is not a count. */
    if (opts_snapshot == nullptr || snapshot == nullptr
        || dsd_app_frontend_get_metrics_for_snapshot(opts_snapshot, snapshot, &metrics, DSD_FRONTEND_SNR_FALLBACK_ALL)
               != 0) {
        clear();
        return;
    }

    /* Built whole, then published in one step, so a frame that reads identically to
     * the last one costs no binding re-evaluation. See MetricsModel::View. */
    View next;
    next.airspy = airspyView(opts_snapshot);

    // Published atomically with the scan flags. Until this first valid snapshot,
    // a cleared model cannot prove that a new session is not scanning.
    next.options_known = true;

    /* Drives whether the tuner-facing rows are shown at all. Taken from the options
     * the running session was configured with, which is the same authority the
     * metrics fetch above uses to decide whether any of them mean anything. */
    next.radio_input = dsd_opts_input_is_radio(opts_snapshot) != 0;

    next.carrier_lock = metrics.carrier_lock != 0;
    next.cfo_hz = metrics.cfo_hz;
    next.stream_active = metrics.stream_active != 0;
    /* Where the front end is pointed, and whether something other than the user
     * is pointing it. Both come from the same options snapshot as radio_input,
     * so a frame never mixes a center from one generation with a gate from
     * another. Scanner mode counts alongside trunking: it owns the tuner too,
     * stepping the channel map once the hangtime expires. */
    next.center_freq_hz = next.radio_input ? static_cast<double>(opts_snapshot->rtlsdr_center_freq) : 0.0;
    next.channel_bandwidth_hz = next.radio_input ? metrics.channel_bandwidth_hz : 0;
    next.trunking_enabled = opts_snapshot->trunk_enable != 0;
    next.scanner_mode = opts_snapshot->scanner_mode != 0;
    /* Trunk scan counts as a third owner even though it has no reading of its own:
     * it steps targets from the engine loop and a release cannot clear it, so an
     * affordance gated only on the other two offers a tune the scan then undoes. */
    next.tuner_controlled = next.trunking_enabled || next.scanner_mode || (opts_snapshot->trunk_scan_enabled != 0);

    /* Sync is held for a moment after the last synced frame rather than sampled;
     * the hold decays on its own rather than being cleared on a retune. See
     * fillDecoderView(), which documents why a one-shot reset was a race. */
    fillDecoderView(next, opts_snapshot, snapshot, dsd_time_now_monotonic_s());

    /* Selected by modulation, not by cqpsk_enable: the C4FM estimator reads nothing on
     * a GFSK stream. Nothing reads at all on an input with no demodulator behind it
     * (UDP, TCP, a file) -- rtl_tcp does have one, since it is an RTL input like any
     * other. Publishing the estimator's no-reading sentinel would put "-100.0 dB" on
     * screen as though it were a measurement. */
    const dsd_frontend_snr_readout snr = dsd_app_frontend_snr_for_mod(&metrics, snapshot->rf_mod);
    next.snr_valid = snr.valid != 0;
    next.snr_db = next.snr_valid ? snr.snr_db : 0.0;

    if (metrics.tuner_gain_is_auto != 0) {
        next.tuner_gain_text = QStringLiteral("auto");
    } else if (metrics.tuner_gain_valid != 0) {
        next.tuner_gain_text = QStringLiteral("%1 dB").arg(metrics.tuner_gain_tenth_db / 10.0, 0, 'f', 1);
    } else {
        next.tuner_gain_text = QStringLiteral("—");
    }

    const double now_m = dsd_time_now_monotonic_s();
    next.slot_call[0] = slotCallView(snapshot, 0, now_m);
    next.slot_call[1] = slotCallView(snapshot, 1, now_m);

    fillQualityView(next, snapshot);
    fillSiteView(next, snapshot);

    /* Engine truth for the monitor's toggle buttons. The engine owns both states
     * — commands only enqueue a request — and on Android the service outlives the
     * Activity, so a relaunched UI must read where they actually stand rather than
     * assume a fresh session's defaults. */
    next.audio_muted = opts_snapshot->audio_out == 0;
    next.held_tg = static_cast<qulonglong>(snapshot->tg_hold);
    next.enc_lockout_count = dsd_enc_lockout_active_count(snapshot);
    fillScanControlView(next, opts_snapshot, snapshot);
    /* The same monotonic reading the call lines were aged against, not a second
     * clock read: one frame has to describe one instant, or the countdown and the
     * call durations beside it would come from moments either side of the poll. */
    fillScanTimingView(next, opts_snapshot, snapshot, now_m);

    /* The engine's command acknowledgement, shown until its own expiry stamp. The
     * timer takes an expired message down without waiting for another publish —
     * an idle engine may not raise the redraw flag again for minutes. */
    const qint64 message_remaining_s =
        static_cast<qint64>(snapshot->ui_msg_expire) - QDateTime::currentSecsSinceEpoch();
    if (snapshot->ui_msg[0] != '\0' && message_remaining_s > 0) {
        next.ui_message = QString::fromUtf8(snapshot->ui_msg);
        m_messageTimer.start(static_cast<int>(qMin<qint64>(message_remaining_s, 30) * 1000) + 100);
    }

    publish(next);
}

} // namespace dsd_qt
