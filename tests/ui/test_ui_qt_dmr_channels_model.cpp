// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the DMR Tier III Channels sheet's model (discussion #900 F3), read from a synthetic snapshot: the rows it
 * lists and their order, what an edit expects of a channel, and no churn while the view's body has not moved. */

#include <QCoreApplication>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <dsd-neo/app_control/dmr_t3_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <initializer_list>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include "dmr_channels_model.h"

namespace {

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* The options, state and Tier III view every case reads, on the heap (main() owns them). */
struct Fixture {
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    std::unique_ptr<dsd_state> state = std::make_unique<dsd_state>();
    dsd_ui_dmr_t3_view view;
};

Fixture* g_fixture = nullptr;

void
set_bit(uint8_t* set, uint16_t lpcn) {
    set[lpcn >> 3] = static_cast<uint8_t>(set[lpcn >> 3] | (1U << (lpcn & 7U)));
}

void
add_detail(uint16_t lpcn, uint8_t slot, bool group, uint32_t caller, uint32_t recipient, uint64_t heard) {
    dsd_ui_dmr_t3_missing_detail& d = g_fixture->view.missing.details[g_fixture->view.missing.detail_count++];
    d.lpcn = lpcn;
    d.slot = slot;
    d.group = group ? 1U : 0U;
    d.caller = caller;
    d.recipient = recipient;
    d.last_heard_ms = heard;
    set_bit(g_fixture->view.missing.checked, lpcn);
}

/* Channel @p lpcn at @p hz in both the shared map row and the frequency DMR uses, as they read with no other writer. */
void
set_row(uint16_t lpcn, long hz) {
    g_fixture->state->trunk_chan_map[lpcn] = hz;
    g_fixture->view.channel_hz[lpcn] = hz;
}

void
add_overlay(uint16_t lpcn, bool hidden, long hz, long base) {
    dsd_ui_dmr_t3_overlay_row& row = g_fixture->view.overlay[g_fixture->view.overlay_count++];
    row.lpcn = lpcn;
    row.hidden = hidden ? 1U : 0U;
    row.hz = hz;
    row.base_hz = base;
    g_fixture->view.origin[lpcn] = DSD_UI_DMR_T3_ORIGIN_USER;
    set_row(lpcn, hidden ? 0 : hz);
}

/* A Tier III site: three channels granted without a frequency (12 heard last, 7 before it, 40 past the details and
 * never in a checked grant) and one channel of each origin. */
void
seed_view() {
    DSD_MEMSET(&g_fixture->view, 0, sizeof g_fixture->view);
    g_fixture->view.present = 1U;
    g_fixture->view.tier3 = 1U;
    g_fixture->view.cc_confirmed_here = 1U;
    g_fixture->view.cc_hz = 451000000L;
    g_fixture->view.generation = 3U;
    g_fixture->view.revision = 9U;
    g_fixture->view.content_seq = 1U;
    for (uint16_t lpcn : {12U, 7U, 40U}) {
        set_bit(g_fixture->view.missing.missing, lpcn);
    }
    add_detail(12U, 0U, true, 5001U, 1234U, 2000U);
    add_detail(7U, 1U, false, 5002U, 5003U, 1000U);
    g_fixture->view.origin[18] = DSD_UI_DMR_T3_ORIGIN_FILE;
    set_row(18U, 451012500L);
    add_overlay(20U, false, 451137500L, 451100000L);
    add_overlay(21U, true, 0, 451200000L);
    const int origins[] = {DSD_UI_DMR_T3_ORIGIN_SITE, DSD_UI_DMR_T3_ORIGIN_UNVERIFIED, DSD_UI_DMR_T3_ORIGIN_NEIGHBOUR,
                           DSD_UI_DMR_T3_ORIGIN_ESTIMATE};
    for (int i = 0; i < 4; ++i) {
        g_fixture->view.origin[22 + i] = static_cast<uint8_t>(origins[i]);
        set_row(static_cast<uint16_t>(22 + i), 451300000L + 12500L * i);
    }
    g_fixture->state->state_ext[DSD_STATE_EXT_APP_DMR_T3_VIEW] = &g_fixture->view;
}

QList<int>
lpcns(const QVariantList& rows) {
    QList<int> out;
    for (const QVariant& row : rows) {
        out.append(row.toMap().value(QStringLiteral("lpcn")).toInt());
    }
    return out;
}

void
test_rows() {
    seed_view();
    dsd_tg_policy_entry entry = {};
    expect("a talkgroup name",
           dsd_tg_policy_make_exact_entry(1234U, "A", "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0
               && dsd_tg_policy_append_exact(g_fixture->state.get(), &entry) == 0);
    dsd_qt::DmrChannelsModel model;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("closed, nothing is read", model.missing().isEmpty() && model.channels().isEmpty() && !model.present());
    model.setActive(true);
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("the most recently heard first, then the rest by channel",
           lpcns(model.missing()) == QList<int>({12, 7, 40}));
    expect("the reading order is by channel", lpcns(model.missingByChannel()) == QList<int>({7, 12, 40}));
    expect("the count holds every ledger channel", model.missingCount() == 3);
    const QVariantMap twelve = model.missing().at(0).toMap();
    expect("a group grant's row",
           twelve.value("detailed").toBool() && twelve.value("slot").toInt() == 1 && twelve.value("group").toBool()
               && !twelve.value("data").toBool() && twelve.value("recipient").toUInt() == 1234U
               && twelve.value("caller").toUInt() == 5001U && twelve.value("lastHeardMs").toLongLong() == 2000
               && !twelve.value("unverified").toBool());
    expect("names the talkgroup", twelve.value("recipientName").toString() == QStringLiteral("Dispatch"));
    const QVariantMap seven = model.missing().at(1).toMap();
    expect("a private grant's row: slot 2, caller and called radio",
           seven.value("slot").toInt() == 2 && !seven.value("group").toBool()
               && seven.value("recipient").toUInt() == 5003U && seven.value("caller").toUInt() == 5002U);
    const QVariantMap forty = model.missing().at(2).toMap();
    expect("past the details only the channel is known, unchecked without a checked grant",
           !forty.value("detailed").toBool() && forty.value("unverified").toBool() && forty.value("slot").toInt() == 0);

    expect("the channels by number", lpcns(model.channels()) == QList<int>({18, 20, 21, 22, 23, 24, 25}));
    QStringList origins;
    for (const QVariant& row : model.channels()) {
        origins << row.toMap().value("origin").toString();
    }
    expect("each origin's key",
           origins == QStringList({"file", "user", "hidden", "site", "unchecked", "neighbour", "estimated"}));
    const QVariantMap file = model.channels().at(0).toMap();
    expect("a file row", file.value("hz").toLongLong() == 451012500 && !file.value("yours").toBool()
                             && file.value("originCode").toInt() == DSD_UI_DMR_T3_ORIGIN_FILE);
    const QVariantMap hidden = model.channels().at(2).toMap();
    expect("a hidden row stays listed with what Restore returns",
           hidden.value("hidden").toBool() && hidden.value("hz").toLongLong() == 0
               && hidden.value("baseHz").toLongLong() == 451200000 && hidden.value("yours").toBool()
               && hidden.value("originCode").toInt() == DSD_UI_DMR_T3_ORIGIN_USER);

    expect("an edit's expectation of the user's row",
           model.channel(20).value("hz").toLongLong() == 451137500
               && model.channel(20).value("originCode").toInt() == DSD_UI_DMR_T3_ORIGIN_USER
               && model.channel(20).value("baseHz").toLongLong() == 451100000);
    expect("of a hidden row: no frequency, USER",
           model.channel(21).value("hz").toLongLong() == 0
               && model.channel(21).value("originCode").toInt() == DSD_UI_DMR_T3_ORIGIN_USER);
    expect("of a missing channel: none, NONE",
           model.channel(12).value("missing").toBool() && model.channel(12).value("hz").toLongLong() == 0
               && model.channel(12).value("originCode").toInt() == DSD_UI_DMR_T3_ORIGIN_NONE);
    expect("channel numbers past 1..4094 are not channels", !model.channel(0).value("valid").toBool()
                                                                && !model.channel(4095).value("valid").toBool()
                                                                && model.channel(4094).value("valid").toBool());

    expect("the identity and the control channel", model.present() && model.tier3() && !model.tier3Unchecked()
                                                       && model.ccConfirmedHere() && model.ccHz() == 451000000);
    expect("what an edit expects", model.generation() == 3U && model.revision() == 9U && model.session().isEmpty());
    model.refresh(g_fixture->opts.get(), g_fixture->state.get(), 42U);
    expect("and the command session it was read in", model.session() == QStringLiteral("42"));
    expect("no trunk scan", !model.trunkScanActive());
    dsd_state_ext_free_all(g_fixture->state.get());
    g_fixture->state->state_ext[DSD_STATE_EXT_APP_DMR_T3_VIEW] = &g_fixture->view;
}

/* Repeated grants publish nothing: the rows are rebuilt only when the body moved, and published only when they
 * differ. */
void
test_no_churn() {
    seed_view();
    dsd_qt::DmrChannelsModel model;
    model.setActive(true);
    int missingSignals = 0, channelSignals = 0, stateSignals = 0;
    QObject::connect(&model, &dsd_qt::DmrChannelsModel::missingChanged, [&]() { ++missingSignals; });
    QObject::connect(&model, &dsd_qt::DmrChannelsModel::channelsChanged, [&]() { ++channelSignals; });
    QObject::connect(&model, &dsd_qt::DmrChannelsModel::stateChanged, [&]() { ++stateSignals; });
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("the first read publishes each group once", missingSignals == 1 && channelSignals == 1 && stateSignals == 1);
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    g_fixture->view.content_seq++;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("the same body, or a body copied again unchanged, publishes nothing",
           missingSignals == 1 && channelSignals == 1 && stateSignals == 1);
    set_row(18U, 451025000L);
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("rows are read only when the body moved",
           channelSignals == 1 && model.channel(18).value("hz").toLongLong() == 451012500);
    g_fixture->view.content_seq++;
    g_fixture->view.revision++;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("a moved body is read again",
           channelSignals == 2 && model.channel(18).value("hz").toLongLong() == 451025000 && missingSignals == 1);
    expect("and the revision is published", stateSignals == 2 && model.revision() == 10U);

    set_bit(g_fixture->view.missing.missing, 41U);
    g_fixture->view.content_seq++;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("a channel newly missing moves only the missing rows",
           missingSignals == 2 && channelSignals == 2 && model.missingCount() == 4);

    g_fixture->opts->trunk_scan_enabled = 1;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("under a trunk scan the sheet is read only", model.trunkScanActive() && stateSignals == 3);
    g_fixture->opts->trunk_scan_enabled = 0;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());

    model.setSessionSystemUid(QStringLiteral("8d3b1e7a-0000-4000-8000-000000000001"));
    expect("the session's saved system", model.sessionHasSavedSystem() && stateSignals == 5);

    model.replaceForTarget(g_fixture->opts.get(), nullptr);
    expect("no live session: no rows", model.missing().isEmpty() && model.channels().isEmpty() && !model.present());
    model.replaceForTarget(g_fixture->opts.get(), g_fixture->state.get());
    expect("a new target's rows in one replacement", model.missingCount() == 4 && model.channels().size() == 7);
    model.setActive(false);
    model.replaceForTarget(g_fixture->opts.get(), g_fixture->state.get());
    expect("closed, a hop drops the rows", model.channels().isEmpty());
    model.setActive(true);
    g_fixture->view.present = 0U;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("a session with no DMR record lists nothing", model.channels().isEmpty() && !model.present());
    g_fixture->state->state_ext[DSD_STATE_EXT_APP_DMR_T3_VIEW] = nullptr;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("nor does a snapshot without the view", model.channels().isEmpty());
}

/* Another protocol's writer can empty or fill the shared map row under one of the user's entries: the sheet shows the
 * frequency DMR uses, and an edit expects it, since the decoder's STALE check compares against that value. */
void
test_rows_read_the_effective_frequency() {
    seed_view();
    g_fixture->state->trunk_chan_map[20] = 0;          // emptied under the user's SET entry at 451.1375 MHz
    g_fixture->state->trunk_chan_map[21] = 452000000L; // filled under the hidden channel
    g_fixture->state->trunk_chan_map[18] = 452500000L; // and a row of the file's moved in the shared map only
    dsd_qt::DmrChannelsModel model;
    model.setActive(true);
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("a set channel shows its entry's frequency",
           model.channel(20).value("hz").toLongLong() == 451137500 && !model.channel(20).value("hidden").toBool());
    expect("a hidden channel shows none and an edit expects none",
           model.channel(21).value("hz").toLongLong() == 0 && model.channel(21).value("hidden").toBool());
    expect("any other channel shows what DMR uses", model.channel(18).value("hz").toLongLong() == 451012500);
    QList<qint64> listed;
    for (const QVariant& row : model.channels()) {
        listed.append(row.toMap().value("hz").toLongLong());
    }
    expect("the list reads the same values",
           listed.size() == 7 && listed.at(0) == 451012500 && listed.at(1) == 451137500 && listed.at(2) == 0);
}

/* R7: a carrier that names another system (Connect Plus, P25 under Auto) has that system's map. An open sheet lists no
 * Tier III rows and offers no edits until the receiver is back on the Tier III system, and the flag moves the rows by
 * itself (the body need not be copied anew). A trunk scan offers no edits either. */
void
test_other_system_offers_no_edits() {
    seed_view();
    dsd_qt::DmrChannelsModel model;
    model.setActive(true);
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("a Tier III carrier offers edits",
           model.editable() && !model.otherSystemHere() && model.channel(20).value("valid").toBool());
    int stateSignals = 0;
    QObject::connect(&model, &dsd_qt::DmrChannelsModel::stateChanged, [&]() { ++stateSignals; });
    g_fixture->view.other_system_here = 1U;
    g_fixture->view.tier3 = 0U;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("another system: no edits", !model.editable() && model.otherSystemHere() && stateSignals == 1);
    expect("and no Tier III rows", model.missing().isEmpty() && model.missingByChannel().isEmpty()
                                       && model.missingCount() == 0 && model.channels().isEmpty());
    expect("and no channel to edit",
           !model.channel(20).value("valid").toBool() && model.channel(20).value("hz").toLongLong() == 0);
    g_fixture->view.other_system_here = 0U;
    g_fixture->view.tier3 = 1U;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("back on Tier III: the rows and the edits are back",
           model.editable() && !model.otherSystemHere() && model.missingCount() == 3 && model.channels().size() == 7
               && model.channel(20).value("hz").toLongLong() == 451137500);
    g_fixture->opts->trunk_scan_enabled = 1;
    model.refresh(g_fixture->opts.get(), g_fixture->state.get());
    expect("a trunk scan offers no edits", !model.editable() && model.trunkScanActive());
    g_fixture->opts->trunk_scan_enabled = 0;
}

void
test_origin_keys() {
    expect("no origin has no key", dsd_qt::DmrChannelsModel::originKey(DSD_UI_DMR_T3_ORIGIN_NONE, false).isEmpty());
    expect("hidden wins over USER", dsd_qt::DmrChannelsModel::originKey(DSD_UI_DMR_T3_ORIGIN_USER, true) == "hidden");
    expect("an unknown code has no key", dsd_qt::DmrChannelsModel::originKey(99, false).isEmpty());
    expect("the constants are the command's",
           dsd_qt::DmrChannelsModel::opSet() == 1 && dsd_qt::DmrChannelsModel::opHide() == 2
               && dsd_qt::DmrChannelsModel::opRestore() == 3 && dsd_qt::DmrChannelsModel::maxChannel() == 4094
               && dsd_qt::DmrChannelsModel::minHz() == 100000 && dsd_qt::DmrChannelsModel::maxHz() == 2147483647
               && dsd_qt::DmrChannelsModel::maxEntries() == 256);
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto fixture = std::make_unique<Fixture>();
    g_fixture = fixture.get();
    test_rows();
    test_no_churn();
    test_rows_read_the_effective_frequency();
    test_other_system_offers_no_edits();
    test_origin_keys();
    g_fixture->state->state_ext[DSD_STATE_EXT_APP_DMR_T3_VIEW] = nullptr;
    dsd_state_ext_free_all(g_fixture->state.get());
    g_fixture = nullptr;
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
