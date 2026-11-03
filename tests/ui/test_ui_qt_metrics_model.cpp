// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * Regression test: what MetricsModel makes of a decoder snapshot.
 *
 * The readings a screen gates on are derived here, and getting them wrong is
 * invisible in a screenshot: a lock light that stays lit after the decoder has
 * been told to look for something else reads as "it is working" while nothing is
 * being decoded at all. That one shipped — a one-shot latch reset raced whatever
 * the poll happened to read on the same frame — which is what these cases pin.
 */

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDebug>
#include <QLatin1Char>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <cmath>
#include <cstring>
#include <dsd-neo/core/receive_types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <utility>

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/rtl_gain_view.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/app_control/scan_state_view.h>
#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
/* The scan-list store helpers (issue #672) have no C++ linkage of their own. */
extern "C" {
#include <dsd-neo/core/state.h>
}
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <dsd-neo/runtime/squelch.h>
#include <functional>
#include <memory>
#include <vector>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include "../test_support/radio_policy_test.h"
#include "metrics_model.h"
#include "scan_avoid_list_model.h"
#include "scan_list_toggle_model.h"

namespace {

int g_failures = 0;

/* What the stubbed frontend reports for the channel width. Per-case, because
 * every other case wants the at-rest 0. */
static int g_stub_channel_bandwidth_hz = 0;
static int g_stub_channel_bandwidth_dsp_limited = 0;
/* Whether the stubbed front end says a stream runs, and its demod rate (issue #525). */
static int g_stub_stream_active = 0;
/* The demodulator kind of the published analog width (issue #524): FM unless a case publishes an AM width. */
static int g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_FM;
static int g_stub_demod_rate_hz = 0;

/* The scan-timing view a case feeds the model. Zeroed between cases: reason NONE
 * is the contract's "nothing to show", which is what every other case wants. */
static dsd_app_scan_timing g_stub_scan_timing;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

} // namespace

/*
 * The frontend boundary, stubbed. Linking the real one would drag in the engine
 * and the radio backends for a test about arithmetic over a snapshot.
 */
extern "C" int
dsd_app_frontend_get_metrics_for_snapshot(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* out,
                                          unsigned int snr_fallbacks) {
    (void)opts;
    (void)state;
    (void)snr_fallbacks;
    if (out == nullptr) {
        return -1;
    }
    *out = dsd_frontend_metrics{};
    out->channel_bandwidth_hz = g_stub_channel_bandwidth_hz;
    out->channel_bandwidth_dsp_limited = g_stub_channel_bandwidth_dsp_limited;
    out->stream_active = g_stub_stream_active;
    out->channel_analog_kind = g_stub_channel_analog_kind;
    out->demod_rate_hz = g_stub_demod_rate_hz;
    return 0;
}

/*
 * The scan-timing view, stubbed through the same link seam as the frontend metrics
 * above, so a case can hand the model a view it built by hand.
 *
 * Deliberately not the real one: the reason -> phrase / show-this-budget table is
 * app-control's and is pinned by APP_CONTROL_SCAN_TIMING_VIEW. What this suite owns
 * is the copy out of the view into the published frame, and running the real table
 * here would only pin it twice. The one rule reproduced is the contract's `active`
 * gate, because the model deliberately has no gate of its own -- "is anything
 * scanning" is decided once, in the view.
 */
extern "C" int
dsd_app_scan_timing_view(const dsd_opts* opts, const dsd_state* state, double now_m, dsd_app_scan_timing* out) {
    if (out == nullptr) {
        return -1;
    }
    *out = dsd_app_scan_timing{};
    if (opts == nullptr || state == nullptr) {
        return -1;
    }
    (void)now_m;
    const bool scanning = opts->trunk_scan_enabled == 1 || opts->scanner_mode == 1;
    if (!scanning || g_stub_scan_timing.reason == DSD_SCAN_STAY_NONE) {
        return 0;
    }
    *out = g_stub_scan_timing;
    out->active = 1U;
    return 1;
}

/*
 * Issue #697: the scan-list switch calls the toggle model's default backend makes. Never reached here: the cases
 * that switch a list hand the model a backend of their own.
 */
extern "C" uint32_t
dsd_app_scan_list_request_next(void) {
    return 0;
}

extern "C" int
dsd_app_command_scan_list_set(const dsd_app_scan_list_set_payload* payload) {
    (void)payload;
    return DSD_APP_COMMAND_SUBMIT_REJECTED;
}

extern "C" int
dsd_app_scan_list_results_since(uint64_t after_sequence, dsd_app_scan_list_result* out, int cap) {
    (void)after_sequence;
    (void)out;
    (void)cap;
    return 0;
}

extern "C" int
dsd_app_scan_list_result_get(dsd_app_scan_list_result* out) {
    if (out != nullptr) {
        *out = dsd_app_scan_list_result{};
    }
    return 0;
}

extern "C" dsd_frontend_snr_readout
dsd_app_frontend_snr_for_mod(const dsd_frontend_metrics* metrics, int rf_mod) {
    (void)metrics;
    (void)rf_mod;
    dsd_frontend_snr_readout out{};
    out.valid = 0;
    out.snr_db = 0.0;
    return out;
}

/* Issue #672: a -Y list of @p rows rows, 851.0125 MHz up in 12.5 kHz steps, as a channel map leaves it. */
static void
seed_lcn_rows(dsd_state* state, int rows) {
    expect("the LCN rows fit", dsd_state_trunk_lcn_reserve(state, static_cast<size_t>(rows)) == 0);
    for (int i = 0; i < rows; ++i) {
        *dsd_state_trunk_lcn_slot(state, i) = 851012500L + (12500L * i);
    }
    state->lcn_freq_count = rows;
}

/* The coordinator's published avoid list: @p count targets "t1".. of session @p session, with a new revision, as
   trunk_scan_publish_avoids() leaves it. */
static void
publish_trunk_avoids(dsd_state* state, uint32_t session, size_t count) {
    std::vector<dsd_trunk_scan_avoid_entry> entries(count);
    for (size_t i = 0; i < count; ++i) {
        entries[i] = dsd_trunk_scan_avoid_entry{};
        entries[i].ordinal = static_cast<uint32_t>(i + 1U);
        entries[i].frequency_hz = 851012500ULL + (12500ULL * i);
        DSD_SNPRINTF(entries[i].id, sizeof entries[i].id, "t%zu", i + 1U);
    }
    expect("the trunk-scan avoid list is published",
           dsd_state_trunk_scan_avoids_set(state, entries.empty() ? nullptr : entries.data(), count) == 0);
    state->trunk_scan_session = session;
    state->trunk_scan_avoid_rev = dsd_state_rev_next();
}

namespace {

/* What a list model told its views since the last take(): each structural change as "-first:last", "+first:last",
   "~first:last>dest", "=row" for a label, "reset". */
class ModelLog {
  public:
    explicit ModelLog(QAbstractItemModel* model) {
        QObject::connect(model, &QAbstractItemModel::rowsRemoved, [this](const QModelIndex&, int first, int last) {
            m_events.append(QStringLiteral("-%1:%2").arg(first).arg(last));
        });
        QObject::connect(model, &QAbstractItemModel::rowsInserted, [this](const QModelIndex&, int first, int last) {
            m_events.append(QStringLiteral("+%1:%2").arg(first).arg(last));
        });
        QObject::connect(model, &QAbstractItemModel::rowsMoved,
                         [this](const QModelIndex&, int first, int last, const QModelIndex&, int dest) {
                             m_events.append(QStringLiteral("~%1:%2>%3").arg(first).arg(last).arg(dest));
                         });
        QObject::connect(model, &QAbstractItemModel::dataChanged, [this](const QModelIndex& from, const QModelIndex&) {
            m_events.append(QStringLiteral("=%1").arg(from.row()));
        });
        QObject::connect(model, &QAbstractItemModel::modelReset, [this] { m_events.append(QStringLiteral("reset")); });
    }

    QString
    take() {
        const QString joined = m_events.join(QLatin1Char(' '));
        m_events.clear();
        return joined;
    }

  private:
    QStringList m_events;
};

} // namespace

static QStringList
model_keys(const QAbstractItemModel& model) {
    QStringList keys;
    for (int i = 0; i < model.rowCount(); ++i) {
        keys.append(model.data(model.index(i, 0), dsd_qt::ScanAvoidListModel::KeyRole).toString());
    }
    return keys;
}

static dsd_qt::ScanAvoidListModel::Entry
avoid_entry(const char* key, const char* label) {
    dsd_qt::ScanAvoidListModel::Entry entry{QString::fromUtf8(key), QString::fromUtf8(label), {}};
    entry.identity.kind = DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN;
    return entry;
}

/* ---- Issue #697: the Scan lists sheet's rows ---- */

/* The app-control side of the toggle model, scripted: requests are numbered from 101, every submit is kept, a case
   queues the results it wants answered, and a case can make the queue refuse. */
namespace {
struct ToggleRig {
    uint32_t nextRequest = 101;
    QList<dsd_app_scan_list_set_payload> sent;
    QList<dsd_app_scan_list_result> results;
    uint64_t latest = 0;
    bool queueFull = false;

    dsd_qt::ScanListToggleModel::Backend
    backend() {
        dsd_qt::ScanListToggleModel::Backend b;
        b.requestNext = [this] { return nextRequest++; };
        b.submit = [this](const dsd_app_scan_list_set_payload& payload) {
            if (queueFull) {
                return static_cast<int>(DSD_APP_COMMAND_SUBMIT_REJECTED);
            }
            sent << payload;
            return static_cast<int>(DSD_APP_COMMAND_SUBMIT_QUEUED);
        };
        /* As the real history does, only the last DSD_APP_RESULT_HISTORY results are kept. */
        b.resultsSince = [this](uint64_t after, dsd_app_scan_list_result* out, int cap) {
            const uint64_t oldest =
                latest > uint64_t{DSD_APP_RESULT_HISTORY} ? latest - DSD_APP_RESULT_HISTORY + 1U : 1U;
            int n = 0;
            for (const auto& result : results) {
                if (result.sequence > after && result.sequence >= oldest && n < cap) {
                    out[n++] = result;
                }
            }
            return n;
        };
        b.latestSequence = [this] { return latest; };
        return b;
    }

    /* Answer request @p request with @p code, the decoder's lists then at @p listsSeq. */
    void
    answer(uint32_t request, int32_t code, uint32_t listsSeq) {
        dsd_app_scan_list_result result{};
        result.sequence = ++latest;
        result.request = request;
        result.code = code;
        result.lists_seq = listsSeq;
        results << result;
    }
};
} // namespace

/* Three lists of session 7 at lists_seq @p seq: security (12 targets), hospital (1, on air), municipality (3). */
static dsd_app_scan_list_view
three_lists(uint32_t seq, bool securityOn = true, bool hospitalOn = true, bool paused = false) {
    dsd_app_scan_list_view view{};
    view.session = 7U;
    view.lists_seq = seq;
    view.list_count = 3U;
    view.running = paused ? 0U : 1U;
    view.paused = paused ? 1U : 0U;
    view.toggleable = paused ? 0U : 1U;
    const char* names[] = {"security", "hospital", "municipality"};
    const uint16_t targets[] = {12U, 1U, 3U};
    const bool on[] = {securityOn, hospitalOn, true};
    for (int i = 0; i < 3; ++i) {
        view.lists[i].number = static_cast<uint8_t>(i + 1);
        view.lists[i].enabled = on[i] ? 1U : 0U;
        view.lists[i].targets = targets[i];
        view.lists[i].has_active = i == 1 ? 1U : 0U;
        DSD_SNPRINTF(view.lists[i].name, sizeof view.lists[i].name, "%s", names[i]);
    }
    view.active_list = 2U;
    return view;
}

static QVariant
toggle_row(const dsd_qt::ScanListToggleModel& model, int row, int role) {
    return model.data(model.index(row, 0), role);
}

static bool
toggle_row_is(const dsd_qt::ScanListToggleModel& model, int row, bool on, bool pending, const QString& note) {
    using M = dsd_qt::ScanListToggleModel;
    return toggle_row(model, row, M::OnRole).toBool() == on
           && toggle_row(model, row, M::PendingRole).toBool() == pending
           && toggle_row(model, row, M::NoteRole).toString() == note;
}

/* The rows a view gives, a switch that waits for its answer and then holds what it asked for until the lists show
   it, and a later change from elsewhere taking over. */
static void
test_scan_list_toggle_switch_and_hold() {
    using M = dsd_qt::ScanListToggleModel;
    ToggleRig rig;
    rig.latest = 40; /* an earlier answer, before this session: never matched */
    rig.answer(101, DSD_APP_SCAN_LIST_SET_OK, 3);
    M model;
    model.setBackend(rig.backend());
    model.refresh(three_lists(10));
    expect("three rows", model.count() == 3 && model.rowCount() == 3);
    expect("a row's fields", toggle_row(model, 1, M::NumberRole).toInt() == 2
                                 && toggle_row(model, 1, M::NameRole).toString() == QStringLiteral("hospital")
                                 && toggle_row(model, 1, M::TargetsRole).toInt() == 1
                                 && toggle_row(model, 1, M::OnAirRole).toBool()
                                 && !toggle_row(model, 0, M::OnAirRole).toBool());
    expect("all on, none waiting", toggle_row_is(model, 1, true, false, QString()));
    expect("the sheet's counts",
           model.switchable() && !model.paused() && model.listsOn() == 3 && model.targetsOn() == 16);
    expect("the answer from before the session is not this row's", toggle_row_is(model, 0, true, false, QString()));

    expect("hospital off is sent", model.setEnabled(2, false));
    expect("one request, as the payload names it", rig.sent.size() == 1 && rig.sent.at(0).session == 7U
                                                       && rig.sent.at(0).request == 101U && rig.sent.at(0).list == 2
                                                       && rig.sent.at(0).enabled == 0U);
    expect("the row waits, showing what it asked for", toggle_row_is(model, 1, false, true, QString()));
    expect("a waiting row takes no second switch", !model.setEnabled(2, true) && rig.sent.size() == 1);
    expect("the counts follow the rows", model.listsOn() == 2 && model.targetsOn() == 15);
    model.refresh(three_lists(10));
    expect("no answer yet: still waiting", toggle_row_is(model, 1, false, true, QString()));

    /* The answer before the snapshot that shows it: the row keeps what it asked for. */
    rig.answer(101, DSD_APP_SCAN_LIST_SET_OK, 11);
    model.refresh(three_lists(10));
    expect("answered OK ahead of the snapshot: off, not waiting", toggle_row_is(model, 1, false, false, QString()));
    model.refresh(three_lists(11, true, false));
    expect("the snapshot shows it", toggle_row_is(model, 1, false, false, QString()));
    /* Switched back on from elsewhere: the snapshot is the truth from then on. */
    model.refresh(three_lists(12, true, true));
    expect("a later change from elsewhere shows", toggle_row_is(model, 1, true, false, QString()));
}

/* More answers than the history keeps went by between two refreshes: the row's own may be among those lost, so it
   stops waiting and shows the snapshot, with no reason, and takes a switch again. */
static void
test_scan_list_toggle_history_gap() {
    ToggleRig rig;
    dsd_qt::ScanListToggleModel model;
    model.setBackend(rig.backend());
    model.refresh(three_lists(10));
    expect("security off is sent", model.setEnabled(1, false));
    expect("the row waits", toggle_row_is(model, 0, false, true, QString()));
    for (int i = 0; i < DSD_APP_RESULT_HISTORY + 4; ++i) {
        rig.answer(9000U + static_cast<uint32_t>(i), DSD_APP_SCAN_LIST_SET_UNCHANGED, 10);
    }
    model.refresh(three_lists(10));
    expect("a gap ends the wait: the snapshot, no reason", toggle_row_is(model, 0, true, false, QString()));
    expect("and the row switches again", model.setEnabled(1, false) && rig.sent.size() == 2);
}

/* A refusal puts the switch back and says why beside it, until the lists move past the ones it described; a paused
   refusal also goes once a resume this model saw follows the pause. */
static void
test_scan_list_toggle_refusals() {
    ToggleRig rig;
    dsd_qt::ScanListToggleModel model;
    model.setBackend(rig.backend());
    model.refresh(three_lists(10));
    expect("security off is sent", model.setEnabled(1, false));
    rig.answer(101, DSD_APP_SCAN_LIST_SET_REFUSED_LAST, 10);
    model.refresh(three_lists(10));
    const QString nothing = QStringLiteral("Nothing else to scan. Switch on another list first.");
    expect("refused: back on, with the reason", toggle_row_is(model, 0, true, false, nothing));
    model.refresh(three_lists(10));
    expect("the reason stays while the lists stand", toggle_row_is(model, 0, true, false, nothing));
    model.refresh(three_lists(11));
    expect("the reason goes once the lists move", toggle_row_is(model, 0, true, false, QString()));

    /* Every other code's reason. */
    const struct {
        int32_t code;
        const char* note;
    } codes[] = {
        {DSD_APP_SCAN_LIST_SET_BUSY, "The scan was busy. Try again."},
        {DSD_APP_SCAN_LIST_SET_STALE, "The scan changed. Nothing was switched."},
        {DSD_APP_SCAN_LIST_SET_UNAVAILABLE, "The trunk scan is not running."},
        {DSD_APP_SCAN_LIST_SET_SEARCH, "Stop Signal search first."},
        {DSD_APP_SCAN_LIST_SET_DROPPED, "Not applied. Try again."},
        {DSD_APP_SCAN_LIST_SET_INVALID, "Not applied."},
        {DSD_APP_SCAN_LIST_SET_UNCHANGED, ""},
    };

    uint32_t seq = 11;
    for (const auto& c : codes) {
        const uint32_t request = rig.nextRequest;
        expect("municipality off is sent", model.setEnabled(3, false));
        rig.answer(request, c.code, seq);
        model.refresh(three_lists(seq));
        expect(c.note, toggle_row_is(model, 2, true, false, QString::fromUtf8(c.note)));
        model.refresh(three_lists(++seq));
    }

    /* Dropped before a drain reached it, carrying older lists than the ones shown at the switch: the reason stays
       until the view passes the lists the switch was made on. */
    const uint32_t dropped = rig.nextRequest;
    expect("security off is sent again", model.setEnabled(1, false));
    rig.answer(dropped, DSD_APP_SCAN_LIST_SET_DROPPED, seq - 3);
    model.refresh(three_lists(seq));
    expect("a dropped switch's reason stays at the lists it was made on",
           toggle_row_is(model, 0, true, false, QStringLiteral("Not applied. Try again.")));
    model.refresh(three_lists(++seq));
    expect("and goes once they move", toggle_row_is(model, 0, true, false, QString()));

    /* Paused: the answer can come in a frame that still shows the scan running. */
    const uint32_t pausedRequest = rig.nextRequest;
    expect("hospital off is sent", model.setEnabled(2, false));
    rig.answer(pausedRequest, DSD_APP_SCAN_LIST_SET_PAUSED, seq);
    model.refresh(three_lists(seq));
    const QString pausedNote = QStringLiteral("The scan is paused. Resume it first.");
    expect("a paused refusal before the pause shows", toggle_row_is(model, 1, true, false, pausedNote));
    model.refresh(three_lists(seq, true, true, true));
    expect("kept while paused", toggle_row_is(model, 1, true, false, pausedNote));
    expect("paused: nothing can be switched", !model.switchable() && model.paused() && !model.setEnabled(1, false));
    model.refresh(three_lists(seq));
    expect("gone on the resume", toggle_row_is(model, 1, true, false, QString()));

    /* The queue refused it: never pending, the row says so until the lists move. */
    rig.queueFull = true;
    const int sentBefore = static_cast<int>(rig.sent.size());
    expect("a full queue sends nothing", !model.setEnabled(3, false) && rig.sent.size() == sentBefore);
    expect("not sent, said beside the row",
           toggle_row_is(model, 2, true, false, QStringLiteral("Not sent. Try again.")));
    rig.queueFull = false;
    model.refresh(three_lists(++seq));
    expect("not sent goes once the lists move", toggle_row_is(model, 2, true, false, QString()));
}

/* A new session drops every waiting switch and reason, and an answer to the old request is never matched. */
static void
test_scan_list_toggle_session_change() {
    ToggleRig rig;
    dsd_qt::ScanListToggleModel model;
    model.setBackend(rig.backend());
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&]() { ++resets; });
    model.refresh(three_lists(10));
    expect("security off is sent", model.setEnabled(1, false));
    expect("waiting", toggle_row_is(model, 0, false, true, QString()));
    dsd_app_scan_list_view next = three_lists(1);
    next.session = 8U;
    resets = 0;
    model.refresh(next);
    expect("a new session is a new set of rows", resets == 1);
    expect("the old switch no longer waits", toggle_row_is(model, 0, true, false, QString()));
    rig.answer(101, DSD_APP_SCAN_LIST_SET_REFUSED_LAST, 1);
    model.refresh(next);
    expect("its answer is not this session's", toggle_row_is(model, 0, true, false, QString()));
    model.clear();
    expect("no session, no rows", model.count() == 0 && !model.switchable() && !model.setEnabled(1, false));
}

/* The header's readings and the sheet's rows from a snapshot: the lists, the list on air, and the rotation's place
   among the targets of the lists that are on. */
static void
test_scan_list_readings() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.trunk_scan_enabled = 1;
    state.trunk_scan_session = 7U;
    state.trunk_scan_lists_seq = 4U;
    state.trunk_scan_list_count = 2U;
    DSD_SNPRINTF(state.trunk_scan_lists[0].name, sizeof state.trunk_scan_lists[0].name, "%s", "security");
    DSD_SNPRINTF(state.trunk_scan_lists[1].name, sizeof state.trunk_scan_lists[1].name, "%s", "hospital");
    state.trunk_scan_lists[0].enabled = 1U;
    state.trunk_scan_lists[0].targets = 12U;
    state.trunk_scan_lists[1].enabled = 0U;
    state.trunk_scan_lists[1].targets = 2U;
    state.trunk_scan_active_list = 2U;
    state.trunk_scan_active_list_off = 1U;
    state.trunk_scan_rotation_ordinal = 0U;
    state.trunk_scan_rotation_count = 12U;
    state.trunk_scan_active_ordinal = 13U;
    state.trunk_scan_target_count = 14U;
    dsd_qt::MetricsModel model;
    model.refresh(&opts, &state);
    expect("the lists are read",
           model.scanListCount() == 2 && model.scanActiveList() == 2 && model.scanActiveListOff());
    expect("the rotation's place: none while its list is off",
           model.scanRotationActive() && model.scanRotationOrdinal() == 0 && model.scanRotationCount() == 12);
    state.trunk_scan_active_list = 1U;
    state.trunk_scan_active_list_off = 0U;
    state.trunk_scan_rotation_ordinal = 3U;
    model.refresh(&opts, &state);
    expect("the rotation's place among the lists that are on, not the union's",
           model.scanActiveList() == 1 && !model.scanActiveListOff() && model.scanRotationOrdinal() == 3
               && model.scanRotationCount() == 12 && model.scanTargetOrdinal() == 13);
    state.trunk_scan_paused = 1U;
    model.refresh(&opts, &state);
    expect("a paused scan has no place", model.scanRotationOrdinal() == 0 && model.scanRotationCount() == 0);
    state.trunk_scan_paused = 0U;
    expect("the sheet's rows", model.scanListModel()->count() == 2 && model.scanListModel()->listsOn() == 1
                                   && model.scanListModel()->targetsOn() == 12);
    model.clear();
    expect("cleared with the session", model.scanListCount() == 0 && model.scanListModel()->count() == 0);
    freeState(&state);
}

/* Issue #672 (S8): the list model turns one list into the next by key, in runs, so a delegate keeps its item while its
   entry stays, and only the rows that changed are announced. */
static void
test_scan_avoid_list_model_diff() {
    dsd_qt::ScanAvoidListModel model;
    ModelLog log(&model);
    model.apply({avoid_entry("a", "A"), avoid_entry("b", "B"), avoid_entry("c", "C"), avoid_entry("d", "D")});
    expect("a first list is one insert", log.take() == QStringLiteral("+0:3"));
    model.apply({avoid_entry("a", "A"), avoid_entry("b", "B"), avoid_entry("c", "C"), avoid_entry("d", "D")});
    expect("the same list announces nothing", log.take().isEmpty());
    /* One out, one in, the same count: a remove and an insert, never a rebuild of the rows that stay. */
    model.apply({avoid_entry("a", "A"), avoid_entry("c", "C"), avoid_entry("d", "D"), avoid_entry("e", "E")});
    expect("an equal-count membership change removes and inserts", log.take() == QStringLiteral("-1:1 +3:3"));
    expect("the rows follow the new list", model_keys(model) == QStringList({"a", "c", "d", "e"}));
    /* Runs: two left at the front, two new in the middle. */
    model.apply({avoid_entry("d", "D"), avoid_entry("x", "X"), avoid_entry("y", "Y"), avoid_entry("e", "E")});
    expect("a run is one remove or one insert", log.take() == QStringLiteral("-0:1 +1:2"));
    /* A reordered target list moves the rows, by runs. */
    model.apply({avoid_entry("y", "Y"), avoid_entry("e", "E"), avoid_entry("d", "D"), avoid_entry("x", "X")});
    expect("a reorder moves the rows", log.take() == QStringLiteral("~2:3>0"));
    expect("the moved rows read in the new order", model_keys(model) == QStringList({"y", "e", "d", "x"}));
    /* A new label for a kept key is a data change of that row alone. */
    model.apply({avoid_entry("y", "Y"), avoid_entry("e", "E2"), avoid_entry("d", "D"), avoid_entry("x", "X")});
    expect("a relabel is a data change", log.take() == QStringLiteral("=1"));
    expect("the row reads its new label",
           model.data(model.index(1, 0), dsd_qt::ScanAvoidListModel::LabelRole).toString() == QStringLiteral("E2"));
    /* Two entries with one key break the contract; the model starts over rather than guess. */
    model.apply({avoid_entry("q", "Q"), avoid_entry("q", "Q")});
    expect("duplicate keys reset", log.take() == QStringLiteral("reset") && model.rowCount() == 2);
    model.clear();
    expect("clear removes every row", log.take() == QStringLiteral("-0:1") && model.rowCount() == 0);
    expect("no row, no entry", model.entry(QStringLiteral("q")).isEmpty());

    /* Keys: an LCN identity by every part, a trunk-scan one by session and id, an id that looks like an arg() marker
       kept as it is. The entry carries 64-bit values as decimal strings. */
    dsd_app_scan_avoid_remove_payload lcn{};
    lcn.kind = DSD_APP_SCAN_AVOID_KIND_LCN;
    lcn.instance = 18446744073709551615ULL;
    lcn.list_rev = 9007199254740993ULL;
    lcn.row = 3;
    lcn.frequency_hz = 851012500ULL;
    expect("an LCN key", dsd_qt::ScanAvoidListModel::keyOf(lcn)
                             == QStringLiteral("lcn:18446744073709551615:9007199254740993:3:851012500"));
    dsd_app_scan_avoid_remove_payload trunk{};
    trunk.kind = DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN;
    trunk.session = 4294967295U;
    DSD_SNPRINTF(trunk.target_id, sizeof trunk.target_id, "%s", "%1:x");
    expect("a trunk-scan key", dsd_qt::ScanAvoidListModel::keyOf(trunk) == QStringLiteral("trunk:4294967295:%1:x"));
    model.apply({{dsd_qt::ScanAvoidListModel::keyOf(lcn), QStringLiteral("Row 4  851.012500 MHz"), lcn},
                 {dsd_qt::ScanAvoidListModel::keyOf(trunk), QStringLiteral("#1  %1:x  frequency unknown"), trunk}});
    const QVariantMap row = model.entry(dsd_qt::ScanAvoidListModel::keyOf(lcn));
    expect("an LCN entry", row.value("kind").toInt() == DSD_APP_SCAN_AVOID_KIND_LCN
                               && row.value("instance").toString() == QStringLiteral("18446744073709551615")
                               && row.value("listRev").toString() == QStringLiteral("9007199254740993")
                               && row.value("frequencyHz").toString() == QStringLiteral("851012500")
                               && row.value("row").toInt() == 3
                               && row.value("label").toString() == QStringLiteral("Row 4  851.012500 MHz"));
    const QVariantMap target = model.entry(dsd_qt::ScanAvoidListModel::keyOf(trunk));
    expect("a trunk-scan entry", target.value("kind").toInt() == DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN
                                     && target.value("session").toULongLong() == 4294967295ULL
                                     && target.value("targetId").toString() == QStringLiteral("%1:x"));
}

/* Issue #672: the Monitor's avoid list and its readings, from the app-control view over the held snapshot. Built only
   when the view's key moves; a hop, a call or a refresh with nothing new announces nothing. */
static void
test_scan_avoids() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    dsd_qt::ScanAvoidListModel* list = model.scanAvoidModel();
    ModelLog log(list);
    int avoidSignals = 0;
    int controlSignals = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::scanAvoidsChanged, [&] { ++avoidSignals; });
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&] { ++controlSignals; });

    model.refresh(&opts, &state);
    expect("no store, nothing to manage", !model.scanAvoidsManageable() && model.scanAvoidCount() == 0
                                              && model.scanAvoidsAvailable() && list->rowCount() == 0);

    /* B1: -Y stopped (the -T hunts still skip its avoided rows): listed and manageable, with no rotation running. */
    seed_lcn_rows(&state, 4);
    expect("row 1 avoided", dsd_state_trunk_lcn_avoid_set(&state, 1U, 1) == 0);
    expect("row 3 avoided", dsd_state_trunk_lcn_avoid_set(&state, 3U, 1) == 0);
    avoidSignals = 0;
    model.refresh(&opts, &state);
    expect("a stopped -Y list's avoids are manageable",
           !model.scanRotationActive() && model.scanAvoidsManageable() && model.scanAvoidsAvailable()
               && model.scanAvoidsTotalKnown() && model.scanAvoidCount() == 2);
    expect("one announcement", avoidSignals == 1 && log.take() == QStringLiteral("+0:1"));
    expect("labels from the view", list->data(list->index(0, 0), dsd_qt::ScanAvoidListModel::LabelRole).toString()
                                       == QStringLiteral("Row 2  851.025000 MHz"));
    const QString row1 = list->data(list->index(0, 0), dsd_qt::ScanAvoidListModel::KeyRole).toString();
    expect("keys carry the identity", row1 == dsd_qt::ScanAvoidListModel::keyOf([&] {
                                          dsd_app_scan_avoid_remove_payload id{};
                                          id.kind = DSD_APP_SCAN_AVOID_KIND_LCN;
                                          id.instance = state.state_instance;
                                          id.list_rev = state.lcn_list_rev;
                                          id.row = 1;
                                          id.frequency_hz = 851025000ULL;
                                          return id;
                                      }()));

    /* Nothing new for the list: a running rotation, a hop, a call. The list is not rebuilt or announced. */
    opts.scanner_mode = 1;
    avoidSignals = 0;
    model.refresh(&opts, &state, true);
    state.lcn_scan_hold = 1;
    model.refresh(&opts, &state);
    expect("the controls moved", controlSignals > 0);
    expect("the list did not", avoidSignals == 0 && log.take().isEmpty());

    /* A put back removes its row alone. */
    expect("row 1 put back", dsd_state_trunk_lcn_avoid_set(&state, 1U, 0) == 0);
    model.refresh(&opts, &state);
    expect("one row leaves", log.take() == QStringLiteral("-0:0") && model.scanAvoidCount() == 1 && avoidSignals == 1);
    /* One in, one out at the same count. */
    expect("row 0 avoided", dsd_state_trunk_lcn_avoid_set(&state, 0U, 1) == 0);
    expect("row 3 put back", dsd_state_trunk_lcn_avoid_set(&state, 3U, 0) == 0);
    avoidSignals = 0;
    model.refresh(&opts, &state);
    expect("an equal-count change is a remove and an insert", log.take() == QStringLiteral("-0:0 +0:0"));
    expect("the count did not move, so its reading says nothing", avoidSignals == 0 && model.scanAvoidCount() == 1);

    /* An incomplete snapshot (B2): no rows to act on, the count unknown, Clear still offered. */
    state.scan_avoids_complete = 0U;
    model.refresh(&opts, &state);
    expect("an incomplete copy is not listed", !model.scanAvoidsAvailable() && !model.scanAvoidsTotalKnown()
                                                   && model.scanAvoidCount() == 0 && model.scanAvoidsManageable());
    expect("its rows are dropped", log.take() == QStringLiteral("-0:0"));
    state.scan_avoids_complete = 1U;
    model.refresh(&opts, &state);
    expect("a whole copy lists them again",
           model.scanAvoidsAvailable() && model.scanAvoidCount() == 1 && log.take() == QStringLiteral("+0:0"));

    /* T2: a build that fails reads unavailable with no rows, and the next refresh at the same key builds it. */
    expect("row 2 avoided", dsd_state_trunk_lcn_avoid_set(&state, 2U, 1) == 0);
    model.failScanAvoidBuildsForTest(1);
    model.refresh(&opts, &state);
    expect("a failed build is unavailable", !model.scanAvoidsAvailable() && !model.scanAvoidsTotalKnown()
                                                && model.scanAvoidCount() == 0 && model.scanAvoidsManageable()
                                                && list->rowCount() == 0);
    log.take();
    model.refresh(&opts, &state);
    expect("the same key builds again",
           model.scanAvoidsAvailable() && model.scanAvoidCount() == 2 && log.take() == QStringLiteral("+0:1"));

    /* A trunk scan's targets win while it is configured, paused too (D3), keyed by session and id. */
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 1;
    publish_trunk_avoids(&state, 7U, 2U);
    model.refresh(&opts, &state);
    expect("the trunk scan's targets",
           model.scanAvoidCount() == 2 && model_keys(*list) == QStringList({"trunk:7:t1", "trunk:7:t2"}));
    expect("a trunk-scan label", list->data(list->index(1, 0), dsd_qt::ScanAvoidListModel::LabelRole).toString()
                                     == QStringLiteral("#2  t2  851.025000 MHz"));
    log.take();
    /* The app writes each target's uid as its id: the list names a target by the channel name Main hands the model
       (issue #671), else by its label column, and keeps the id only for a target with neither. A rename is a data
       change of that row alone. */
    model.setChannelNames({{"t1", "Fire Dispatch"}});
    model.refresh(&opts, &state);
    expect("a target named by its channel",
           list->data(list->index(0, 0), dsd_qt::ScanAvoidListModel::LabelRole).toString()
               == QStringLiteral("#1  Fire Dispatch  851.012500 MHz"));
    expect("only that row is relabelled", log.take() == QStringLiteral("=0"));
    /* A channel name longer than a target id holds is cut on a character boundary, never inside one: 32 two-byte
       characters (64 bytes) keep 31, with no replacement character in the row or in Put back's spoken name. */
    model.setChannelNames({{"t1", QString(32, QChar(0x00E9))}});
    model.refresh(&opts, &state);
    {
        const QString label = list->data(list->index(0, 0), dsd_qt::ScanAvoidListModel::LabelRole).toString();
        expect("a long channel name is cut between characters",
               label == QStringLiteral("#1  ") + QString(31, QChar(0x00E9)) + QStringLiteral("  851.012500 MHz")
                   && !label.contains(QChar(0xFFFD)));
    }
    model.setChannelNames({{"t1", "Fire Dispatch"}});
    model.refresh(&opts, &state);
    log.take();
    {
        dsd_trunk_scan_avoid_entry labelled[2] = {};
        for (uint32_t i = 0; i < 2U; ++i) {
            labelled[i].ordinal = i + 1U;
            labelled[i].frequency_hz = 851012500ULL + (12500ULL * i);
            DSD_SNPRINTF(labelled[i].id, sizeof labelled[i].id, "t%u", i + 1U);
        }
        DSD_SNPRINTF(labelled[1].label, sizeof labelled[1].label, "%s", "County");
        expect("labelled targets published", dsd_state_trunk_scan_avoids_set(&state, labelled, 2U) == 0);
        state.trunk_scan_avoid_rev = dsd_state_rev_next();
    }
    model.refresh(&opts, &state);
    expect("a target with no channel name by its label column",
           list->data(list->index(1, 0), dsd_qt::ScanAvoidListModel::LabelRole).toString()
               == QStringLiteral("#2  County  851.025000 MHz"));
    model.setChannelNames({});
    model.refresh(&opts, &state);
    expect("an unnamed target keeps its id",
           list->data(list->index(0, 0), dsd_qt::ScanAvoidListModel::LabelRole).toString()
               == QStringLiteral("#1  t1  851.012500 MHz"));
    log.take();
    state.trunk_scan_paused = 1U;
    avoidSignals = 0;
    model.refresh(&opts, &state);
    expect("paused, the list stays and is manageable",
           model.scanAvoidsManageable() && model.scanAvoidCount() == 2 && log.take().isEmpty() && avoidSignals == 0);
    state.trunk_scan_paused = 0U;
    /* A hop to another target publishes nothing new for the list. */
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof state.trunk_scan_active_id, "%s", "t3");
    model.refresh(&opts, &state, true);
    expect("a hop rebuilds nothing", log.take().isEmpty() && avoidSignals == 0);
    /* The coordinator could not allocate its list: unavailable, nothing listed, Clear still offered. */
    state.trunk_scan_avoids_ok = 0U;
    model.refresh(&opts, &state);
    expect("an unallocated trunk-scan list is not listed",
           !model.scanAvoidsAvailable() && model.scanAvoidCount() == 0 && model.scanAvoidsManageable());
    state.trunk_scan_avoids_ok = 1U;

    /* A stop drops the list with the session; the next refresh builds it again. */
    model.clear();
    expect("a cleared model lists nothing", list->rowCount() == 0 && model.scanAvoidCount() == 0
                                                && !model.scanAvoidsManageable() && model.scanAvoidsAvailable());
    model.refresh(&opts, &state);
    expect("the next session builds its own", model.scanAvoidCount() == 2);
    opts.trunk_scan_enabled = 0;
    publish_trunk_avoids(&state, 0U, 0U);
    DSD_MEMSET(state.trunk_scan_active_id, 0, sizeof state.trunk_scan_active_id);
    freeState(&state);
}

/* Issue #672 (S8): a long list costs only what changed. 10,000 avoided rows, the first put back: one remove, and the
   rest of the rows keep their items. */
static void
test_scan_avoids_large_list() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    seed_lcn_rows(&state, 10001);
    for (size_t row = 0; row < 10000U; ++row) {
        (void)dsd_state_trunk_lcn_avoid_set(&state, row, 1);
    }
    dsd_qt::MetricsModel model;
    ModelLog log(model.scanAvoidModel());
    model.refresh(&opts, &state);
    expect("10,000 avoided rows", model.scanAvoidCount() == 10000 && log.take() == QStringLiteral("+0:9999"));
    expect("the first put back", dsd_state_trunk_lcn_avoid_set(&state, 0U, 0) == 0);
    model.refresh(&opts, &state);
    expect("one remove, nothing else", log.take() == QStringLiteral("-0:0") && model.scanAvoidCount() == 9999);
    freeState(&state);
}

static void
test_quality_without_identity(int protocol, uint8_t slot) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_PULSE;
    state.synctype = protocol;
    state.errs = 3;
    state.errs2 = 9;
    state.errsR = 4;
    state.errs2R = 11;
    dsd_qt::MetricsModel model;

    // The vocoder can establish media before a late-entry identity header arrives.
    dsd_call_observation call = {};
    call.protocol = protocol;
    call.slot = slot;
    call.kind = DSD_CALL_KIND_VOICE;
    expect("provisional voice call opens", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    expect("provisional voice has media", dsd_call_state_update_media(&state, slot, 1, 0) == 1);
    model.refresh(&opts, &state);
    expect("provisional voice stays out of the identity headline", model.leadSlot() == 0);
    expect("late-entry media exposes non-P25 quality without identity",
           model.qualityValid() && model.lastFrameErrsValid() && model.lastFrameErrs() == (slot == 0 ? 3 : 4)
               && model.lastFrameErrs2() == (slot == 0 ? 9 : 11));

    if (slot == 1) {
        call.slot = 0;
        call.kind = DSD_CALL_KIND_GROUP_VOICE;
        call.ota_target_id = 101;
        expect("identified companion opens", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
        model.refresh(&opts, &state);
        expect("an identity lead retains priority before media arrives",
               model.leadSlot() == 1 && !model.lastFrameErrsValid());
        dsd_call_state_update_media(&state, 0, 1, 0);
        model.refresh(&opts, &state);
        expect("an identity lead keeps its own error readings", model.leadSlot() == 1 && model.lastFrameErrsValid()
                                                                    && model.lastFrameErrs() == 3
                                                                    && model.lastFrameErrs2() == 9);
        dsd_call_state_end(&state, 0, 0);
    }
    dsd_call_state_end(&state, slot, 0);
    model.refresh(&opts, &state);
    expect("ended provisional media cannot supply fallback errors", !model.lastFrameErrsValid());
    dsd_state_ext_free_all(&state);
}

static void
test_quality() {
    static dsd_opts opts;
    static dsd_state state;
    opts.audio_in_type = AUDIO_IN_PULSE;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_p1_voice_err_hist_len = 50;
    dsd_qt::MetricsModel model;
    int quality_signals = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::qualityChanged, [&]() { ++quality_signals; });
    model.refresh(&opts, &state);
    expect("P25 sync alone has no quality reading", !model.qualityValid() && !model.voiceErrsValid());
    state.p25_p1_fec_ok = 9;
    state.p25_p1_fec_err = 1;
    model.refresh(&opts, &state);
    expect("CC quality is available on PCM", model.qualityValid() && !model.radioInput() && model.ccFecValid());
    expect("CC counters and ok percent", model.ccFecOk() == 9 && model.ccFecErr() == 1 && model.ccFecOkPct() == 90.0);
    expect("one quality notification", quality_signals == 1);
    model.refresh(&opts, &state);
    expect("identical quality is silent", quality_signals == 1);
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("sync gap retains CC FEC", model.ccFecValid() && quality_signals == 1);
    state.p25_p1_fec_ok = state.p25_p1_fec_err = 0;
    model.refresh(&opts, &state);
    expect("no-carrier counter reset clears CC FEC", !model.ccFecValid() && !model.qualityValid());

    dsd_call_observation call = {};
    call.protocol = DSD_SYNC_P25P1_POS;
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.ota_target_id = 101;
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    state.p25_p1_voice_err_hist_count = 2;
    state.p25_p1_voice_err_hist_sum = 9;
    state.p25_p1_voice_fec_ok = 3;
    state.p25_p1_voice_fec_err = 1;
    model.refresh(&opts, &state);
    expect("P1 uses populated count",
           model.voiceErrsValid() && model.voiceErrsSamples() == 2 && model.voiceErrsPerFrame() == 4.5);
    expect("voice FEC ok percent", model.voiceFecValid() && model.voiceFecOkPct() == 75.0);
    const int before_samples = quality_signals;
    state.p25_p1_voice_err_hist_count = 4;
    state.p25_p1_voice_err_hist_sum = 18;
    model.refresh(&opts, &state);
    expect("sample count alone notifies", quality_signals == before_samples + 1 && model.voiceErrsSamples() == 4);
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    model.refresh(&opts, &state);
    expect("new call waits for fresh samples", !model.voiceErrsValid() && model.voiceErrsSamples() == 0);

    call.protocol = DSD_SYNC_P25P2_POS;
    state.synctype = call.protocol;
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    call.slot = 1;
    call.ota_target_id = 102;
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    state.p25_p2_voice_err_hist_len = 50;
    state.p25_p2_voice_err_hist_count[1] = 3;
    state.p25_p2_voice_err_hist_sum[1] = 6;
    state.p25_p2_rs_facch_ok = 2;
    state.p25_p2_rs_sacch_ok = 1;
    state.p25_p2_rs_ess_err = 1;
    model.refresh(&opts, &state);
    expect("unsampled lead slot does not borrow other slot", model.leadSlot() == 1 && !model.voiceErrsValid());
    expect("P2 slot readings are independent", !model.slot1VoiceErrsValid() && model.slot2VoiceErrsValid()
                                                   && model.slot2VoiceErrsSamples() == 3
                                                   && model.slot2VoiceErrsPerFrame() == 2.0);
    expect("P2 RS sums FACCH SACCH ESS", model.rsValid() && model.rsOkPct() == 75.0);
    dsd_call_state_end(&state, 0, 0);
    model.refresh(&opts, &state);
    expect("P2 summary follows lead slot", model.leadSlot() == 2 && model.voiceErrsValid()
                                               && model.voiceErrsPerFrame() == 2.0 && model.voiceErrsSamples() == 3);
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    model.refresh(&opts, &state);
    expect("P2 new call invalidates its samples", !model.slot2VoiceErrsValid() && !model.voiceErrsValid());

    call.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    state.synctype = call.protocol;
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    state.errsR = 2;
    state.errs2R = 7;
    dsd_call_state_update_media(&state, 1, 1, 0);
    model.refresh(&opts, &state);
    expect("non-P25 lead slot last-frame fallback", model.lastFrameErrsValid() && model.lastFrameErrs() == 2
                                                        && model.lastFrameErrs2() == 7 && !model.voiceErrsValid());
    model.clear();
    expect("clear removes all quality", !model.qualityValid() && !model.ccFecValid() && !model.voiceFecValid()
                                            && !model.rsValid() && !model.voiceErrsValid()
                                            && !model.slot2VoiceErrsValid() && !model.lastFrameErrsValid()
                                            && model.voiceErrsSamples() == 0);
    const int cleared = quality_signals;
    model.clear();
    expect("repeated clear is silent", quality_signals == cleared);
    model.refresh(&opts, &state);
    model.refresh(nullptr, &state);
    expect("missing snapshot clears quality", !model.qualityValid());
    dsd_state_ext_free_all(&state);
}

static void
test_lead_slot_keeps_earlier_call_and_quality() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    state.synctype = DSD_SYNC_P25P2_POS;
    dsd_qt::MetricsModel model;

    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 1U, 4242U, 1202U);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.observed_m = dsd_decode_now_mono_s() - 2.0;
    expect("slot 2 opens first", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    state.p25_p2_voice_err_hist_len = 50;
    state.p25_p2_voice_err_hist_count[1] = 3;
    state.p25_p2_voice_err_hist_sum[1] = 6;
    model.refresh(&opts, &state);
    expect("slot 2 alone headlines with its quality", model.leadSlot() == 2 && model.voiceErrsValid()
                                                          && model.voiceErrsSamples() == 3
                                                          && model.voiceErrsPerFrame() == 2.0);

    call.slot = 0U;
    call.ota_target_id = 1201U;
    call.observed_m += 1.0;
    expect("slot 1 opens later", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    state.p25_p2_voice_err_hist_count[0] = 4;
    state.p25_p2_voice_err_hist_sum[0] = 20;
    model.refresh(&opts, &state);
    expect("earlier slot 2 call keeps the headline",
           model.slot1CallState() == 2 && model.slot2CallState() == 2 && model.leadSlot() == 2);
    expect("quality stays with the earlier slot 2 call", model.voiceErrsValid() && model.voiceErrsSamples() == 3
                                                             && model.voiceErrsPerFrame() == 2.0
                                                             && model.slot1VoiceErrsPerFrame() == 5.0);

    expect("earlier slot 2 call ends", dsd_call_state_end(&state, 1U, dsd_decode_now_mono_s()) == 1);
    model.refresh(&opts, &state);
    expect("remaining slot 1 call takes the headline",
           model.slot1CallState() == 2 && model.slot2CallState() == 3 && model.leadSlot() == 1);
    expect("quality moves to the remaining slot 1 call",
           model.voiceErrsValid() && model.voiceErrsSamples() == 4 && model.voiceErrsPerFrame() == 5.0);
    freeState(&state);
}

static void
test_lead_slot_change_alone_notifies() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    state.synctype = DSD_SYNC_P25P2_POS;
    dsd_qt::MetricsModel model;

    /* Stamps ahead of the poll keep both displayed durations at zero, even if the
     * test is descheduled. Restarting an epoch can then change only the lead. */
    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 4242U, 1201U);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.observed_m = dsd_decode_now_mono_s() + 60.0;
    expect("initial lead opens", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_observation other = call;
    other.slot = 1U;
    other.ota_target_id = 1202U;
    other.observed_m += 0.0004;
    expect("companion opens", dsd_call_state_observe(&state, &other, DSD_CALL_BOUNDARY_BEGIN) == 1);
    model.refresh(&opts, &state);
    expect("initial lead is slot 1", model.leadSlot() == 1);

    int slot1_changes = 0;
    int slot2_changes = 0;
    int quality_changes = 0;
    int lead_changes = 0;
    int notified_lead = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::slot1Changed, [&]() { ++slot1_changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::slot2Changed, [&]() { ++slot2_changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::qualityChanged, [&]() { ++quality_changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::leadSlotChanged, [&]() {
        ++lead_changes;
        notified_lead = model.leadSlot();
    });

    call.observed_m += 0.0008;
    expect("initial lead restarts with the same identity",
           dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    model.refresh(&opts, &state);
    expect("slot fields and quality did not change", slot1_changes == 0 && slot2_changes == 0 && quality_changes == 0);
    expect("lead-only change publishes and notifies", model.leadSlot() == 2 && lead_changes == 1 && notified_lead == 2);
    model.refresh(&opts, &state);
    expect("unchanged lead is silent", lead_changes == 1);
    model.clear();
    expect("clear notifies the no-lead sentinel", model.leadSlot() == 0 && lead_changes == 2 && notified_lead == 0);
    freeState(&state);
}

/* Issue #773 (D10): the hero and the second-slot strip word a private call and a call no party named apart from a
   talkgroup call, so each slot exposes the call kind, the target's and the source's own labels and the protocol's
   name, and a change to any of them alone notifies. */
static void
test_slot_kind_and_labels() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    state.synctype = DSD_SYNC_NXDN_POS;
    dsd_qt::MetricsModel model;

    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_NXDN_POS, 0U, 4242U, 2001U);
    call.kind = DSD_CALL_KIND_PRIVATE_VOICE;
    call.observed_m = dsd_decode_now_mono_s() + 60.0;
    expect("a private call opens", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    expect("the state carries an event ring", state.event_history_s != nullptr);
    if (state.event_history_s == nullptr) {
        freeState(&state);
        return;
    }
    Event_History* staged = &state.event_history_s[0].Event_History_Items[0];
    staged->target_id = 2001U;
    DSD_SNPRINTF(staged->t_name, sizeof(staged->t_name), "%s", "Dispatcher 7");
    staged->source_id = 4242U;
    DSD_SNPRINTF(staged->s_name, sizeof(staged->s_name), "%s", "Engine 21");
    model.refresh(&opts, &state);
    expect("the slot exposes the call kind", model.slot1CallKind() == DSD_CALL_KIND_PRIVATE_VOICE);
    expect("the slot exposes the target's label", model.slot1TgLabel() == QStringLiteral("Dispatcher 7"));
    expect("the slot exposes the source's label", model.slot1SrcLabel() == QStringLiteral("Engine 21"));
    expect("the slot exposes the protocol's name", model.slot1ProtocolLabel() == QStringLiteral("NXDN"));
    expect("the other slot has none", model.slot2CallKind() == 0 && model.slot2TgLabel().isEmpty()
                                          && model.slot2SrcLabel().isEmpty() && model.slot2ProtocolLabel().isEmpty());

    int slot1_changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::slot1Changed, [&]() { ++slot1_changes; });
    DSD_SNPRINTF(staged->t_name, sizeof(staged->t_name), "%s", "Dispatcher 8");
    model.refresh(&opts, &state);
    expect("a target label change alone notifies", slot1_changes == 1);
    DSD_SNPRINTF(staged->s_name, sizeof(staged->s_name), "%s", "Engine 22");
    model.refresh(&opts, &state);
    expect("a source label change notifies",
           slot1_changes == 2 && model.slot1SrcLabel() == QStringLiteral("Engine 22"));
    model.refresh(&opts, &state);
    expect("an unchanged slot is silent", slot1_changes == 2);

    /* The same call restarted as a group call, every other field equal: the kind alone notifies. */
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    expect("the group call opens", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    model.refresh(&opts, &state);
    expect("a kind change alone notifies", model.slot1CallKind() == DSD_CALL_KIND_GROUP_VOICE && slot1_changes == 3
                                               && model.slot1TgLabel() == QStringLiteral("Dispatcher 8"));

    /* Speech no D-SETUP named: no party's text, and the protocol to headline it with. */
    dsd_call_observation anonymous = dsd_call_observation_data(DSD_SYNC_TETRA_NDB_POS, 1U, 0U, 0U);
    anonymous.kind = DSD_CALL_KIND_VOICE;
    anonymous.observed_m = dsd_decode_now_mono_s() + 61.0;
    expect("an identity-less TETRA call opens",
           dsd_call_state_observe(&state, &anonymous, DSD_CALL_BOUNDARY_BEGIN) == 1);
    model.refresh(&opts, &state);
    expect("an identity-less TETRA call is presented", model.slot2CallState() == 2);
    expect("it names no party", model.slot2TgText().isEmpty() && model.slot2SrcText().isEmpty());
    expect("it names its protocol", model.slot2ProtocolLabel() == QStringLiteral("TETRA"));
    freeState(&state);
}

/* twoSlotCarrier is the decoder's own verdict, copied from each snapshot: no latch of its own. Snapshots that still
   carry a TDMA sync type after an acquisition reset cleared the verdict read false however many of them come, a
   snapshot whose verdict is set reads true at once, and a clear reads false at once. */
static void
test_two_slot_carrier_mirrors_the_decoder() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::twoSlotCarrierChanged, [&]() { ++changes; });

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.tdma_carrier = 0U;
    for (int i = 0; i < 5; ++i) {
        model.refresh(&opts, &state);
    }
    expect("stale TDMA sync without the verdict reads no two-slot carrier", !model.twoSlotCarrier() && changes == 0);

    state.tdma_carrier = 1U;
    model.refresh(&opts, &state);
    expect("the verdict reads at once", model.twoSlotCarrier() && changes == 1);
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("a sync loss leaves the verdict to the decoder", model.twoSlotCarrier() && changes == 1);

    state.tdma_carrier = 0U;
    model.refresh(&opts, &state);
    expect("a cleared verdict reads at once, with no hold", !model.twoSlotCarrier() && changes == 2);

    state.tdma_carrier = 1U;
    model.refresh(&opts, &state);
    model.clear();
    expect("clear drops the verdict", !model.twoSlotCarrier() && changes == 4);
    freeState(&state);
}

/* The Monitor site identity and the Spectrum carrier identity read the received identity record through the shared
   view: only what this carrier sent, never the Phase 2 seed or decoder state another carrier left, with no latch of
   their own. The record keeps the last heard values (dimmed: siteConfirmed and carrierIdentityLive false) until a
   carrier boundary resets it. */
static void
test_site() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int signals = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::siteChanged, [&]() { ++signals; });

    /* A hand-set seed and another carrier's control channel, nothing received: no identity, and the seed reads as
       configuration. */
    state.synctype = DSD_SYNC_P25P2_POS;
    state.p2_wacn = 0xBEE00;
    state.p2_sysid = 0x348;
    state.p2_cc = 0x293;
    state.p2_hardset = 1U;
    state.p2_rfssid = 3;
    state.p2_siteid = 21;
    state.p25_site_lra_valid = 1;
    state.trunk_cc_freq = 851000000;
    state.p25_cc_freq = 851000000;
    model.refresh(&opts, &state);
    expect("a seed is no received identity",
           model.siteLine().isEmpty() && model.siteProtocol().isEmpty() && !model.p25NacValid() && !model.p25WacnValid()
               && !model.p25SysIdValid() && model.p25Rfss() == 0 && !model.p25LraValid() && model.ccFreqHz() == 0
               && model.carrierIdentityTokens().isEmpty() && model.carrierIdentityProtocol().isEmpty()
               && model.carrierIdentityText().isEmpty() && !model.carrierIdentityLive());
    expect("Phase 2 readiness reads the seed", model.p25Phase2ParamsReady()
                                                   && model.p25Phase2Seed() == QStringLiteral("BEE00.348.293")
                                                   && model.p25Phase2SeedSet());
    state.p2_hardset = 0U;
    state.p2_cc = 0;
    model.refresh(&opts, &state);
    expect("an incomplete learned seed", !model.p25Phase2ParamsReady()
                                             && model.p25Phase2Seed() == QStringLiteral("BEE00.348.000")
                                             && !model.p25Phase2SeedSet());

    /* The NAC this carrier sent, not the seed's. */
    state.synctype = DSD_SYNC_P25P1_POS;
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x2A1U);
    model.refresh(&opts, &state);
    expect("NAC-only P1 identity", model.siteProtocol() == QStringLiteral("P25") && model.p25NacValid()
                                       && model.p25Nac() == 0x2A1 && !model.p25WacnValid() && !model.p25SysIdValid()
                                       && model.siteLine() == QStringLiteral("P25 · NAC 2A1") && model.siteConfirmed());
    expect("carrier identity", model.carrierIdentityTokens() == QStringList{QStringLiteral("NAC 2A1")}
                                   && model.carrierIdentityProtocol() == QStringLiteral("P25")
                                   && model.carrierIdentityText() == QStringLiteral("P25 · NAC 2A1")
                                   && model.carrierIdentityLive());
    const int first = signals;
    model.refresh(&opts, &state);
    expect("identical site does not notify", signals == first);

    dsd_rxid_note_p25_network(&state, 0x45564U, 0x006U);
    dsd_rxid_note_p25_site(&state, 0x006U, 10U, 11U);
    dsd_rxid_note_p25_lra(&state, 0x006U, 0x1AU);
    dsd_rxid_note_cc_freq(&state, DSD_RXID_FAMILY_P25, 769031250U);
    state.trunk_vc_freq[0] = 852000000;
    model.refresh(&opts, &state);
    expect("full P25", model.p25WacnValid() && model.p25Wacn() == 0x45564 && model.p25SysIdValid()
                           && model.p25SysId() == 0x006 && model.p25Rfss() == 10 && model.p25Site() == 11
                           && model.p25LraValid() && model.p25Lra() == 0x1A && model.ccFreqHz() == 769031250
                           && model.vcFreqHz() == 852000000);
    expect("full P25 line",
           model.siteLine() == QStringLiteral("P25 · NAC 2A1 · SYS 006 · RFSS 10 · SITE 11 · WACN 45564 · LRA 1A"));
    expect("compact tokens",
           model.carrierIdentityTokens()
               == QStringList({QStringLiteral("NAC 2A1"), QStringLiteral("SYS 006"), QStringLiteral("RFSS 10"),
                               QStringLiteral("SITE 11"), QStringLiteral("WACN 45564")}));

    /* The sync hold has expired and the no-carrier pass marked the record stale: the values stay, dimmed. A change of
       liveness alone notifies. */
    const QString retained = model.siteLine();
    const QStringList retainedTokens = model.carrierIdentityTokens();
    int beforeLoss = signals;
    state.synctype = DSD_SYNC_NONE;
    dsd_rxid_mark_stale(&state);
    model.refresh(&opts, &state);
    expect("retained after the hold", model.siteLine() == retained && !model.siteConfirmed()
                                          && model.carrierIdentityTokens() == retainedTokens
                                          && !model.carrierIdentityLive() && model.p25Nac() == 0x2A1);
    expect("liveness alone notifies", signals == beforeLoss + 1);
    state.synctype = DSD_SYNC_P25P1_POS;
    model.refresh(&opts, &state);
    expect("sync without a reception stays retained", !model.carrierIdentityLive() && signals == beforeLoss + 1);
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x2A1U);
    model.refresh(&opts, &state);
    expect("a reception is live again", model.carrierIdentityLive() && model.siteConfirmed()
                                            && model.siteLine() == retained && signals == beforeLoss + 2);

    /* A carrier boundary resets the record; conventional carrier B shows only its own NAC, no control channel. */
    dsd_rxid_reset(&state);
    model.refresh(&opts, &state);
    expect("a reset record shows nothing", model.siteLine().isEmpty() && model.siteProtocol().isEmpty()
                                               && model.ccFreqHz() == 0 && model.carrierIdentityTokens().isEmpty());
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x3B2U);
    model.refresh(&opts, &state);
    expect("carrier B", model.siteLine() == QStringLiteral("P25 · NAC 3B2") && model.ccFreqHz() == 0
                            && !model.p25WacnValid() && model.p25Rfss() == 0);

    /* Another protocol replaces the identity. */
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 7U);
    dsd_rxid_note_dmr_site(&state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 3U, 0U, "Cap+ · Site 3");
    dsd_rxid_note_dmr_rest_lsn(&state, 4U);
    dsd_rxid_note_dmr_brand(&state, "Capacity Plus");
    DSD_SNPRINTF(state.dmr_site_parms, sizeof(state.dmr_site_parms), "%s", "stale; ");
    state.dmr_rest_channel = 9;
    model.refresh(&opts, &state);
    expect("DMR site", model.siteProtocol() == "DMR" && model.dmrColorCode() == 7
                           && model.dmrSiteText() == QStringLiteral("Cap+ · Site 3") && model.dmrRestLsn() == 4
                           && !model.p25NacValid() && model.carrierIdentityProtocol() == QStringLiteral("DMR"));
    /* The site text is one token per part, the site compact and the Cap+ tag full form only, so a narrow line keeps the
       parts that fit. */
    expect("DMR line", model.siteLine() == QStringLiteral("DMR · CC 7 · Cap+ · Site 3 · Rest LSN 4 · Capacity Plus")
                           && model.carrierIdentityTokens()
                                  == QStringList({
                                      QStringLiteral("CC 7"),
                                      QStringLiteral("Site 3"),
                                      QStringLiteral("Rest LSN 4"),
                                  }));
    expect("no XPT free channel", model.dmrXptFreeLcn() == -1);
    /* A Hytera XPT site's free channel: its own SiteSheet field and a compact token. */
    dsd_rxid_reset(&state);
    dsd_rxid_note_dmr_brand(&state, "Hytera XPT");
    dsd_rxid_note_dmr_xpt_free_lcn(&state, 0U);
    model.refresh(&opts, &state);
    expect("XPT free channel 0", model.dmrXptFreeLcn() == 0);
    dsd_rxid_note_dmr_xpt_free_lcn(&state, 3U);
    model.refresh(&opts, &state);
    expect("XPT free channel",
           model.dmrXptFreeLcn() == 3 && model.carrierIdentityTokens() == QStringList({QStringLiteral("Free LCN 3")}));

    /* NXDN Type-C and IDAS. */
    dsd_rxid_reset(&state);
    state.synctype = DSD_SYNC_NXDN_POS;
    state.nxdn_last_ran = 5;
    state.nxdn_location_sys_code = 99;
    state.nxdn_location_site_code = 98;
    model.refresh(&opts, &state);
    expect("NXDN decoder state is no identity", model.siteLine().isEmpty() && model.nxdnRan() == -1);
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_NXDN, (uint8_t)DSD_ACCESS_CODE_RAN, 7U);
    dsd_rxid_note_nxdn_location(&state, 0U, 12U, 3U);
    model.refresh(&opts, &state);
    expect("NXDN location", model.siteProtocol() == "NXDN" && model.nxdnRan() == 7
                                && model.nxdnLocationCategory() == QStringLiteral("Global") && model.nxdnSysCode() == 12
                                && model.nxdnSiteCode() == 3 && model.siteLine().contains("RAN 7"));
    dsd_rxid_reset(&state);
    dsd_rxid_note_nxdn_idas_area(&state, 0U);
    dsd_rxid_note_nxdn_idas_site(&state, 1U, 7U);
    model.refresh(&opts, &state);
    expect("IDAS area and site code", model.siteProtocol() == "IDAS" && model.nxdnRan() == 0
                                          && model.nxdnLocationCategory() == "Type-D" && model.nxdnSysCode() == 0
                                          && model.nxdnSiteCode() == 7 && model.siteLine().contains("Area 0")
                                          && model.siteLine().contains("SITE 7") && !model.siteLine().contains("SYS"));
    dsd_rxid_note_nxdn_typed_system(&state, 0x5A5A5U, 7U);
    model.refresh(&opts, &state);
    expect("IDAS system and site", model.nxdnSysCode() == 0x5A5A5 && model.nxdnSiteCode() == 7
                                       && model.siteLine().contains(QStringLiteral("SYS %1").arg(0x5A5A5)));

    /* EDACS. */
    dsd_rxid_reset(&state);
    state.synctype = DSD_SYNC_EDACS_POS;
    state.edacs_site_id = 9;
    model.refresh(&opts, &state);
    expect("EDACS decoder state is no identity", model.edacsSiteText().isEmpty() && model.siteLine().isEmpty());
    dsd_rxid_note_edacs_site(&state, 7U);
    model.refresh(&opts, &state);
    expect("EDACS site", model.siteProtocol() == "EDACS" && model.edacsSiteText().contains("007")
                             && model.carrierIdentityTokens() == QStringList{QStringLiteral("SITE 007 [07]")});
    /* Extended addressing names the system apart from the site: each part the record holds has a SiteSheet field. */
    dsd_rxid_reset(&state);
    expect("EDACS parts unset", model.edacsSystem() == -1 && model.edacsArea() == -1 && model.edacsCcLcn() == -1);
    dsd_rxid_note_edacs_system(&state, 3U);
    dsd_rxid_note_edacs_area(&state, 0U);
    dsd_rxid_note_edacs_cc_lcn(&state, 12U);
    model.refresh(&opts, &state);
    expect("EDACS system, area and CC LCN", model.edacsSiteText().isEmpty() && model.edacsSystem() == 3
                                                && model.edacsArea() == 0 && model.edacsCcLcn() == 12);

    /* dPMR and M17 send an access code alone; TETRA its network, the cell's colour code and the location area. */
    dsd_rxid_reset(&state);
    state.synctype = DSD_SYNC_DPMR_FS1_POS;
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_DPMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 0U);
    model.refresh(&opts, &state);
    expect("dPMR colour code", model.siteProtocol() == "dPMR" && model.siteAccessCode() == 0);
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_M17, (uint8_t)DSD_ACCESS_CODE_CAN, 9U);
    model.refresh(&opts, &state);
    expect("M17 CAN", model.siteProtocol() == "M17" && model.siteAccessCode() == 9);
    dsd_rxid_note_tetra_cell(&state, 262U, 1001U, 25U);
    model.refresh(&opts, &state);
    expect("TETRA network", model.siteProtocol() == "TETRA" && model.tetraSiteMcc() == 262
                                && model.tetraSiteMnc() == 1001 && model.siteAccessCode() == 25
                                && model.tetraSiteLa() == -1);
    dsd_rxid_note_tetra_la(&state, 4242U);
    dsd_rxid_trunk_tune(&state);
    model.refresh(&opts, &state);
    expect("TETRA area, colour dropped by a tune", model.tetraSiteLa() == 4242 && model.siteAccessCode() == -1);

    /* A DMR carrier that named only its brand, or only its Tier III system code. */
    dsd_rxid_reset(&state);
    dsd_rxid_note_dmr_brand(&state, "Hytera XPT");
    model.refresh(&opts, &state);
    expect("DMR brand", model.siteProtocol() == "DMR" && model.dmrBrand() == QStringLiteral("Hytera XPT")
                            && model.dmrT3Syscode() == -1);
    dsd_rxid_note_dmr_t3_syscode(&state, 0x1001U);
    model.refresh(&opts, &state);
    expect("DMR system code", model.dmrT3Syscode() == 0x1001);
    dsd_rxid_note_dmr_site(&state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 0U, 1U, 1U,
                           "TIII Small · Net 1 · Site 2 · SYS 1001");
    model.refresh(&opts, &state);
    expect("DMR system code in the site text", model.dmrT3Syscode() == -1);

    model.clear();
    expect("stop clears every site group",
           model.siteLine().isEmpty() && model.siteProtocol().isEmpty() && !model.siteConfirmed()
               && model.ccFreqHz() == 0 && model.vcFreqHz() == 0 && model.dmrSiteText().isEmpty()
               && model.edacsSiteText().isEmpty() && model.nxdnRan() == -1 && model.carrierIdentityTokens().isEmpty()
               && model.siteAccessCode() == -1 && model.tetraSiteMcc() == -1 && model.dmrBrand().isEmpty()
               && model.carrierIdentityProtocol().isEmpty() && model.carrierIdentityText().isEmpty()
               && !model.carrierIdentityLive() && model.p25Phase2Seed().isEmpty());
    const int cleared = signals;
    model.clear();
    expect("repeated site clear silent", signals == cleared);
    dsd_state_ext_free_all(&state);
}

static void
test_direct_key_presence() {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    dsd_qt::MetricsModel model;
    model.refresh(&opts, &state);
    expect("empty snapshot has no direct keys", !model.directKeys());
    // Legacy writers can provide material without presence metadata.
    for (auto* key : {&state.K, &state.R, &state.RR, &state.K1}) {
        *key = 0x1234;
        model.refresh(&opts, &state);
        expect("legacy nonzero key remains configured", model.directKeys());
        *key = 0;
        model.refresh(&opts, &state);
        expect("cleared legacy key is absent", !model.directKeys());
    }
    // Presence must not depend on a nonzero key value or on an active call.
    state.basic_key_present = 1;
    model.refresh(&opts, &state);
    expect("zero basic key remains configured", model.directKeys());
    state.basic_key_present = 0;
    for (auto& present : state.scalar_key_present) {
        present = 1;
        model.refresh(&opts, &state);
        expect("zero scalar key remains configured", model.directKeys());
        present = 0;
    }
    state.hytera_key_segments = 1;
    model.refresh(&opts, &state);
    expect("Hytera presence is published", model.directKeys());
    state.hytera_key_segments = 0;
    state.aes_key_loaded[1] = 1;
    model.refresh(&opts, &state);
    expect("direct AES presence is published", model.directKeys());
    state.keyloader = 1;
    model.refresh(&opts, &state);
    expect("automatic slot activation is not a direct override", !model.directKeys());
    model.clear();
    expect("stop clears direct-key status", !model.directKeys());
    dsd_state_ext_free_all(&state);
}

static void
test_decryption_metadata() {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    state.enc_lockout_key_epoch = 10;
    DSD_SNPRINTF(state.key_profile_ref, sizeof(state.key_profile_ref), "%s", "opaque-profile");
    dsd_qt::MetricsModel model;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        dsd_call_observation observation = {};
        observation.protocol = DSD_SYNC_P25P2_POS;
        observation.slot = slot;
        observation.kind = DSD_CALL_KIND_GROUP_VOICE;
        observation.ota_target_id = 123;
        observation.observed_m = 4.0;
        dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
        dsd_call_crypto_update crypto = {};
        crypto.classification = DSD_CALL_CRYPTO_DECRYPTABLE;
        crypto.algid = 0x84;
        crypto.kid = 2 + slot;
        crypto.observed_m = 4.0;
        dsd_call_state_update_crypto(&state, slot, &crypto);
        dsd_call_snapshot before = {}, after = {};
        dsd_call_state_get(&state, slot, &before);
        expect("resolver note accepted",
               dsd_call_state_note_key_selection(&state, slot, before.epoch, DSD_CALL_KEY_SIGNALED, crypto.kid,
                                                 crypto.kid, 1, 0)
                   == 1);
        dsd_call_state_get(&state, slot, &after);
        expect("selection does not extend activity", before.updated_m == after.updated_m);
        expect("wrong call epoch rejected",
               dsd_call_state_note_key_selection(&state, slot, before.epoch + 100, DSD_CALL_KEY_SIGNALED, 99, 99, 1, 0)
                   == 0);
    }
    model.refresh(&opts, &state);
    const auto slots = model.decryptionSlots();
    expect("two independent key selections",
           slots.size() == 2 && slots[0].toMap().value("keyId") == "2" && slots[1].toMap().value("keyId") == "3");
    expect("only opaque profile association published",
           slots[0].toMap().value("profileRef") == "opaque-profile" && !slots[0].toMap().contains("material"));
    state.enc_lockout_key_epoch++;
    model.refresh(&opts, &state);
    expect("old key result becomes pending",
           model.decryptionSlots()[0].toMap().value("availability") == "Waiting for key reevaluation");
    model.clear();
    expect("stopped session has no live key metadata", model.decryptionSlots().isEmpty());
    dsd_state_ext_free_all(&state);
}

static void
test_temporary_lockout_metrics() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    model.refresh(&opts, &state);
    expect("saved lockout default published", model.persistTgLockouts());
    opts.persist_tg_lockouts = 0;
    expect("seed metric avoid", dsd_tg_policy_session_avoid_add(&state, 100) == 0);
    int changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++changes; });
    model.refresh(&opts, &state);
    expect("lockout state notifies", changes == 1 && !model.persistTgLockouts() && model.temporaryTgAvoidCount() == 1);
    uint64_t context = 0;
    dsd_tg_policy_table_version(&state, &context, nullptr);
    expect("current list context published losslessly", model.tgPolicyContext() == QString::number(context));
    model.refresh(&opts, &state);
    expect("stable lockouts do not notify twice", changes == 1);
    dsd_tg_policy_session_avoid_clear(&state);
    model.refresh(&opts, &state);
    expect("clear count notifies", changes == 2 && model.temporaryTgAvoidCount() == 0);
    model.clear();
    expect("stopped avoids unavailable", model.tgPolicyContext().isEmpty() && model.temporaryTgAvoidCount() == 0);
    freeState(&state);
}

static void
test_call_skip_metrics() {
    static dsd_opts opts;
    static dsd_state state;
    static dsd_state snapshot;
    initOpts(&opts);
    initState(&state);
    initState(&snapshot);
    dsd_qt::MetricsModel model;
    model.refresh(&opts, &snapshot);
    int changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++changes; });
    const double now = dsd_decode_now_mono_s();
    expect("seed metric skip", dsd_tg_policy_call_skip_arm(&state, 100, 1, 0, now) == 0);
    expect("publish skip snapshot", dsd_tg_policy_copy_snapshot(&snapshot, &state) == 0);
    model.refresh(&opts, &snapshot);
    expect("skip count notifies", changes == 1 && model.callSkipCount() == 1 && model.temporaryTgAvoidCount() == 0);
    model.refresh(&opts, &snapshot);
    expect("stable skip count does not notify twice", changes == 1);
    dsd_tg_policy_call_skip_clear(&state);
    model.refresh(&opts, &snapshot);
    expect("held snapshot keeps skip", changes == 1 && model.callSkipCount() == 1);
    expect("publish skip clear through reuse", dsd_tg_policy_copy_snapshot(&snapshot, &state) == 0);
    model.refresh(&opts, &snapshot);
    expect("skip clear notifies", changes == 2 && model.callSkipCount() == 0);
    expect("seed expired metric skip",
           dsd_tg_policy_call_skip_arm(&state, 100, 1, 0, now - DSD_TG_CALL_SKIP_QUIET_S - 1.0) == 0);
    expect("publish expired skip", dsd_tg_policy_copy_snapshot(&snapshot, &state) == 0);
    model.refresh(&opts, &snapshot);
    expect("expired skips not counted", changes == 2 && model.callSkipCount() == 0);
    model.clear();
    expect("stopped skips unavailable", model.callSkipCount() == 0);
    freeState(&snapshot);
    freeState(&state);
}

/*
 * The call lines, the call-skip count and decodeNowMs read the decode clock the decoder stamps
 * their inputs on, so under the TEST source they follow it with the real clock untouched: a call
 * begun at T reads 7 s old at T + 7.5 s, and a skip armed at T lapses once its quiet window has
 * passed in decode time.
 */
static void
test_decode_clock_readings() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    const uint64_t t0_ns = 1000000000000000000ULL; // 1e9 s: far from any platform clock reading.
    dsd_decode_clock_use_test(t0_ns);
    dsd_qt::MetricsModel model;
    int now_changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::decodeNowMsChanged, [&]() { ++now_changes; });

    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 123U, 456U);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    // No observed_m: the call store stamps the start from the decode clock.
    expect("decode-clock call begins", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    expect("decode-clock skip armed", dsd_tg_policy_call_skip_arm(&state, 100, 1, 0, dsd_decode_now_mono_s()) == 0);

    dsd_decode_clock_test_set_ns(t0_ns + 7500000000ULL);
    model.refresh(&opts, &state);
    expect("call line ages on the decode clock", model.slot1CallSeconds() == 7);
    expect("skip counted inside its decode-time quiet window", model.callSkipCount() == 1);
    expect("decodeNowMs is the decode clock's wall time", model.decodeNowMs() == 1000000007500LL);
    expect("decodeNowMs announces a new decode second", now_changes == 1);
    model.refresh(&opts, &state);
    expect("the same decode second stays quiet", now_changes == 1);

    dsd_decode_clock_test_set_ns(t0_ns + static_cast<uint64_t>((DSD_TG_CALL_SKIP_QUIET_S + 1.0) * 1e9));
    model.refresh(&opts, &state);
    expect("skip lapses on the decode clock", model.callSkipCount() == 0);
    expect("decodeNowMs announces the next decode second", now_changes == 2);

    dsd_decode_clock_use_system();
    freeState(&state);
}

/* Issue #782: a new scan target drops the old target's sync hold inside the same refresh, so the frame that names the
   new target never carries the old lock and never passes through the cleared view first. The same refresh without a
   new target keeps the hold. */
static void
test_new_target_drops_sync_hold_in_one_publication() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.trunk_scan_enabled = 1;
    dsd_qt::MetricsModel model;

    state.synctype = DSD_SYNC_P25P1_POS;
    state.trunk_scan_active_ordinal = 1U;
    model.refresh(&opts, &state, false);
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state, false);
    expect("an old target's sync is held across an unsynced poll", model.syncedHere());

    int tuner = 0;
    int control = 0;
    bool lockedAtPublication = false;
    bool defaultAtPublication = false;
    QObject::connect(&model, &dsd_qt::MetricsModel::tunerChanged, [&]() {
        ++tuner;
        lockedAtPublication = lockedAtPublication || model.syncedHere();
        defaultAtPublication = defaultAtPublication || !model.radioInput();
    });
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() {
        ++control;
        defaultAtPublication = defaultAtPublication || !model.optionsKnown();
    });
    state.trunk_scan_active_ordinal = 2U;
    model.refresh(&opts, &state, true);
    expect("a new target drops the held sync", !model.syncedHere() && model.syncLabel().isEmpty());
    expect("a new target publishes the tuner rows once, unlocked", tuner == 1 && !lockedAtPublication);
    expect("a new target publishes the controls once", control == 1);
    expect("a new target never publishes the cleared view", !defaultAtPublication);
    expect("a new target keeps the readings", model.radioInput() && model.optionsKnown());
    freeState(&state);
}

static void
test_options_readiness() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++changes; });
    expect("fresh metrics do not establish single-system options",
           !model.optionsKnown() && !model.scanRotationActive());
    model.refresh(nullptr, &state);
    expect("state without options is not ready", !model.optionsKnown());
    model.refresh(&opts, nullptr);
    expect("options without a state snapshot are not ready", !model.optionsKnown());
    model.refresh(&opts, &state);
    expect("valid quiet snapshot establishes options", model.optionsKnown() && !model.scanRotationActive());
    expect("readiness notifies controls", changes > 0);
    const int unchanged = changes;
    model.refresh(&opts, &state);
    expect("unchanged options do not notify twice", changes == unchanged);
    for (int mode = 0; mode < 2; ++mode) {
        model.clear();
        expect("clear forgets the previous session's options", !model.optionsKnown());
        opts.scanner_mode = mode == 0;
        opts.trunk_scan_enabled = mode == 1;
        model.refresh(&opts, &state);
        expect("both effective scan modes arrive with readiness", model.optionsKnown() && model.scanRotationActive());
        model.refresh(nullptr, &state);
        expect("losing a snapshot resets readiness", !model.optionsKnown());
    }
    freeState(&state);
}

/*
 * Issue #575: whether the input in force is an I/Q replay, which the spectrum reads to offer no tuning. It is the core
 * predicate on the same options snapshot as radioInput, and it notifies the control group with tunerControlled, the
 * other reason a tune is refused.
 */
static void
test_replay_input() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int controls = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++controls; });

    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl");
    model.refresh(&opts, &state);
    expect("a live tuner is not a replay", model.radioInput() && !model.replayInput());

    int before = controls;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "iqreplay:capture.iq.json");
    model.refresh(&opts, &state);
    expect("an iqreplay spec on the RTL input is a replay", model.radioInput() && model.replayInput());
    expect("the predicate agrees", model.replayInput() == (dsd_opts_input_is_iq_replay(&opts) != 0));
    expect("a replay notifies the control group", controls > before);

    /* The replay flag the RTL stream keeps in its own copy is not the predicate: only the input in force counts. */
    opts.iq_replay_active = 1;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl");
    before = controls;
    model.refresh(&opts, &state);
    expect("leaving the replay clears it", !model.replayInput());
    expect("and notifies the control group", controls > before);
    opts.iq_replay_active = 0;

    /* A spec string under a non-radio input type is no replay: nothing is being tuned at all. */
    opts.audio_in_type = AUDIO_IN_WAV;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "iqreplay:capture.iq.json");
    model.refresh(&opts, &state);
    expect("audio input is not a replay", !model.radioInput() && !model.replayInput());

    opts.audio_in_type = AUDIO_IN_RTL;
    model.refresh(&opts, &state);
    expect("replaying again", model.replayInput());
    model.clear();
    expect("a cleared model reports no replay", !model.replayInput());
    freeState(&state);
}

/*
 * The receiver the session runs on, in the Qt start path's source names, from the options snapshot: what a Signal
 * search screen reattaching to a running search after Activity recreation restores its receiver from. It notifies
 * the control group, and a cleared model names none.
 */
static void
test_radio_source() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int controls = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&] { ++controls; });
    /* A local receiver is named by its source type alone. */
    const auto named_only = [&](const char* type) {
        const QVariantMap source = model.radioSource();
        return source.size() == 1 && source.value(QStringLiteral("sourceType")) == QString::fromLatin1(type);
    };

    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:450M:0:0:48");
    model.refresh(&opts, &state);
    expect("an RTL-SDR spec is the usb source", named_only("usb"));

    const int before = controls;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy:450M:8:0:0");
    model.refresh(&opts, &state);
    expect("an Airspy spec is the airspy source", named_only("airspy"));
    expect("a new receiver notifies the control group", controls > before);

    /* The server is the one the engine parsed from the spec, not a re-reading of the text. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:10.0.0.5:1235:450M");
    DSD_SNPRINTF(opts.rtltcp_hostname, sizeof opts.rtltcp_hostname, "%s", "10.0.0.5");
    opts.rtltcp_portno = 1235;
    model.refresh(&opts, &state);
    const QVariantMap rtltcp = model.radioSource();
    expect("an rtl_tcp spec is the rtltcp source with its server",
           rtltcp.value(QStringLiteral("sourceType")) == QStringLiteral("rtltcp")
               && rtltcp.value(QStringLiteral("host")) == QStringLiteral("10.0.0.5")
               && rtltcp.value(QStringLiteral("port")).toInt() == 1235);

    /* No Qt start offers these receivers, and audio input has none. */
    for (const char* dev : {"soapy:driver=rtlsdr", "iqreplay:capture.iq.json"}) {
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", dev);
        model.refresh(&opts, &state);
        expect("a receiver no Qt start offers names no source", model.radioSource().isEmpty());
    }
    opts.audio_in_type = AUDIO_IN_WAV;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:450M");
    model.refresh(&opts, &state);
    expect("audio input names no source", model.radioSource().isEmpty());

    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    model.refresh(&opts, &state);
    expect("a bare airspy spec is the airspy source", named_only("airspy"));
    model.clear();
    expect("a cleared model names no source", model.radioSource().isEmpty());
    freeState(&state);
}

/*
 * Why the rotation is staying on the row on air, and how long is left (#508).
 *
 * The countdown is published in tenths of a second rather than milliseconds on
 * purpose: only changes to the rendered tenth notify the timing row, and they
 * never notify the unrelated control group.
 */
static void
test_scan_timing() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int changes = 0;
    int control_changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::scanTimingChanged, [&]() { ++changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++control_changes; });

    /* A trunked target following a call: no countdown, the dwell disarmed while the
     * call holds the row, and the -t hangtime that will release it. */
    opts.trunk_scan_enabled = 1;
    g_stub_scan_timing = dsd_app_scan_timing{};
    g_stub_scan_timing.reason = DSD_SCAN_STAY_CALL_FOLLOW;
    g_stub_scan_timing.phrase = "Following call";
    g_stub_scan_timing.show_dwell = 1U;
    g_stub_scan_timing.dwell_ms = 3000U;
    g_stub_scan_timing.dwell_state = DSD_APP_SCAN_DWELL_SUSPENDED;
    /* Carried by the publication but withheld by the view: a trunked row has no
     * conventional activity hold, and printing one would invent a budget. */
    g_stub_scan_timing.hold_ms = 2000U;
    g_stub_scan_timing.show_hang = 1U;
    g_stub_scan_timing.hang_ms = 2000U;
    model.refresh(&opts, &state);
    expect("a followed call names why the scanner stays",
           model.scanTimingVisible() && model.scanStayReason() == DSD_SCAN_STAY_CALL_FOLLOW
               && model.scanStayPhrase() == QStringLiteral("Following call"));
    expect("a followed call runs no countdown",
           !model.scanTimerLive() && model.scanTimerRemainingDs() == 0 && model.scanTimerSpanMs() == 0);
    expect("the suspended dwell still reads out",
           model.scanDwellMs() == 3000 && model.scanDwellState() == DSD_APP_SCAN_DWELL_SUSPENDED);
    expect("a trunked row never shows a conventional hold", model.scanHoldMs() == 0);
    expect("a followed call shows the hangtime that will release it", model.scanHangMs() == 2000);
    expect("scan timing has its own notification", changes > 0);
    const int settled = changes;
    model.refresh(&opts, &state);
    expect("an unchanged scan timing does not notify twice", changes == settled);

    /* A conventional row counting its idle dwell down, with the activity hold that
     * would extend the stay if something showed up. */
    opts.trunk_scan_enabled = 0;
    opts.scanner_mode = 1;
    g_stub_scan_timing = dsd_app_scan_timing{};
    g_stub_scan_timing.reason = DSD_SCAN_STAY_IDLE_DWELL;
    g_stub_scan_timing.phrase = "Idle dwell";
    g_stub_scan_timing.timer_live = 1U;
    g_stub_scan_timing.remaining_ms = 1855U;
    g_stub_scan_timing.span_ms = 3000U;
    g_stub_scan_timing.show_hold = 1U;
    g_stub_scan_timing.hold_ms = 2000U;
    model.refresh(&opts, &state);
    expect("an idle dwell counts down", model.scanTimerLive() && model.scanTimerSpanMs() == 3000);
    expect("the countdown reaches the row in tenths", model.scanTimerRemainingDs() == 18);
    expect("a conventional row shows its activity hold", model.scanHoldMs() == 2000);
    expect("the dwell the countdown already shows is not printed twice", model.scanDwellMs() == 0);

    const int quiet = changes;
    const int controls_settled = control_changes;
    g_stub_scan_timing.remaining_ms = 1801U;
    model.refresh(&opts, &state);
    expect("a countdown moving inside one tenth notifies nothing",
           changes == quiet && model.scanTimerRemainingDs() == 18);
    g_stub_scan_timing.remaining_ms = 1799U;
    model.refresh(&opts, &state);
    expect("the countdown moves on the tenth", model.scanTimerRemainingDs() == 17 && changes > quiet);
    expect("countdowns do not notify unrelated controls", control_changes == controls_settled);

    // The -Y publisher saturates large accepted -t values at UINT32_MAX milliseconds.
    // This must remain positive at the Qt property boundary, including through QVariant.
    g_stub_scan_timing.span_ms = UINT32_MAX;
    g_stub_scan_timing.show_hang = 1U;
    g_stub_scan_timing.hang_ms = UINT32_MAX;
    model.refresh(&opts, &state);
    expect("a large timer total does not become negative",
           model.property("scanTimerSpanMs").toULongLong() == static_cast<qulonglong>(UINT32_MAX));
    expect("a large effective hangtime does not become negative",
           model.property("scanHangMs").toULongLong() == static_cast<qulonglong>(UINT32_MAX));

    /* The per-visit cap (#507) rides the same row. It is not driven by the stay reason,
     * so it reads out beside whatever else holds the receiver, and its countdown is in
     * tenths for the same reason the window's is. */
    g_stub_scan_timing.show_visit = 1U;
    g_stub_scan_timing.visit_ms = 20000U;
    g_stub_scan_timing.visit_live = 1U;
    g_stub_scan_timing.visit_remaining_ms = 12349U;
    model.refresh(&opts, &state);
    expect("the visit cap reads out with its countdown",
           model.scanVisitMs() == 20000 && model.scanVisitLive() && model.scanVisitRemainingDs() == 123);
    const int capped = changes;
    g_stub_scan_timing.visit_remaining_ms = 12301U;
    model.refresh(&opts, &state);
    expect("a cap moving inside one tenth notifies nothing", changes == capped && model.scanVisitRemainingDs() == 123);

    /* Suspended by a hold: the cap still applies to the row, so its width is stated, but
     * there is no countdown -- zero tenths would read as a visit that just ran out. */
    g_stub_scan_timing.visit_live = 0U;
    g_stub_scan_timing.visit_remaining_ms = 0U;
    model.refresh(&opts, &state);
    expect("a suspended cap keeps its width and drops the countdown",
           model.scanVisitMs() == 20000 && !model.scanVisitLive() && model.scanVisitRemainingDs() == 0);
    expect("suspending the cap notifies the scan timing group", changes > capped);

    /* Withheld by the view: no cap is in force, so no number the row could print arrives. */
    g_stub_scan_timing.show_visit = 0U;
    model.refresh(&opts, &state);
    expect("no cap on air leaves nothing to print", model.scanVisitMs() == 0);

    model.clear();
    expect("a stopped session leaves no scan timing behind",
           !model.scanTimingVisible() && model.scanStayReason() == DSD_SCAN_STAY_NONE
               && model.scanStayPhrase().isEmpty() && !model.scanTimerLive() && model.scanTimerRemainingDs() == 0
               && model.scanTimerSpanMs() == 0 && model.scanDwellMs() == 0
               && model.scanDwellState() == DSD_APP_SCAN_DWELL_NONE && model.scanHoldMs() == 0
               && model.scanHangMs() == 0 && model.scanVisitMs() == 0 && !model.scanVisitLive()
               && model.scanVisitRemainingDs() == 0);

    /* Plain trunking and a plain single system are not rotations: there is no row to
     * stay on, so the view says nothing and the panel shows nothing. */
    opts.scanner_mode = 0;
    g_stub_scan_timing.reason = DSD_SCAN_STAY_IDLE_DWELL;
    opts.trunk_enable = 1;
    model.refresh(&opts, &state);
    expect("nothing is rotating: the row stays down",
           !model.scanTimingVisible() && model.scanStayPhrase().isEmpty() && model.scanDwellMs() == 0);
    opts.trunk_enable = 0;

    g_stub_scan_timing = dsd_app_scan_timing{};
    freeState(&state);
}

/*
 * The received sub-audible tone or code (#522, #523), read through the real app-control view:
 * what the monitor row shows for each state, the configured policy kept apart from it, and the
 * row cleared when the session stops -- which is MetricsModel::clear(), the path UiController
 * takes on stop, not something the QML fixture can prove.
 */
static void
test_receive_filter_status() {
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    initOpts(opts.get());
    initState(state.get());
    dsd_qt::MetricsModel model;
    expect("receive filter parses",
           dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_DCC, 1, "7", "", "", nullptr, 0) == 0);
    state->synctype = state->lastsynctype = DSD_SYNC_NONE;
    model.refresh(opts.get(), state.get());
    expect("unidentified digital traffic checks", model.receiveStatus().contains(QStringLiteral("Checking")));
    opts->analog_only = 1;
    model.refresh(opts.get(), state.get());
    expect("NFM never waits for a digital code", model.receiveStatus() == QStringLiteral("Filters off"));
    opts->analog_only = 0;
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->dmr_color_code = 8;
    model.refresh(opts.get(), state.get());
    expect("confirmed blocked code rejects", model.receiveStatus().contains(QStringLiteral("Rejected")));
    state->dmr_color_code = 7;
    model.refresh(opts.get(), state.get());
    expect("confirmed allowed code admits", model.receiveStatus().contains(QStringLiteral("Allowed")));
    freeState(state.get());
}

static void
test_rx_tone() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int changes = 0;
    int scan_changes = 0;
    int configured_changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::rxToneChanged, [&]() { ++changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::scanTimingChanged, [&]() { ++scan_changes; });
    /* The configured policy has its own signal, so nothing received ever announces it. */
    QObject::connect(&model, &dsd_qt::MetricsModel::rxToneConfiguredTextChanged, [&]() { ++configured_changes; });
    expect("the configured policy reads off before the first frame",
           model.rxToneConfiguredText() == QStringLiteral("off"));

    /* Digital decoding: no detection runs, so a stale publication must not reach the row. */
    state.analog_rx.carrier_open = 1;
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state.analog_rx.ctcss_tenths_hz = 1000;
    model.refresh(&opts, &state);
    expect("no FM monitor, no received-tone row", !model.rxToneVisible() && model.rxToneText().isEmpty());
    expect("the configured policy reads off", model.rxToneConfiguredText() == QStringLiteral("off"));

    /* The analog FM monitor locked on 100.0 Hz. */
    opts.analog_only = 1;
    opts.monitor_input_audio = 1;
    const int before = changes;
    model.refresh(&opts, &state);
    expect("a locked tone shows its value", model.rxToneVisible() && model.rxToneStatus() == DSD_APP_RX_TONE_LOCKED
                                                && model.rxToneText() == QStringLiteral("CTCSS 100.0 Hz")
                                                && model.rxToneKind() == DSD_ANALOG_TONE_KIND_CTCSS
                                                && model.rxToneTenthsHz() == 1000 && model.rxToneCarrier());
    expect("the received tone has its own notification",
           changes > before && scan_changes == 0 && configured_changes == 0);
    expect("received and configured stay apart", model.rxToneConfiguredText() == QStringLiteral("off"));
    const int settled = changes;
    model.refresh(&opts, &state);
    expect("an unchanged tone does not notify twice", changes == settled);

    /* A live stream input that went quiet: past the deadline the tap published, the decoder is
       waiting for samples and its last word no longer describes the channel. The frame's own
       clock ages it to no carrier; a deadline still ahead keeps the tone. */
    state.analog_rx.stale_after_ms = 1U;
    model.refresh(&opts, &state);
    expect("a paused stream's tone reads no carrier", model.rxToneVisible()
                                                          && model.rxToneStatus() == DSD_APP_RX_TONE_NO_CARRIER
                                                          && model.rxToneText() == QStringLiteral("\u2014")
                                                          && model.rxToneTenthsHz() == 0 && !model.rxToneCarrier());
    state.analog_rx.stale_after_ms = UINT64_MAX;
    model.refresh(&opts, &state);
    expect("a stream inside its deadline keeps the tone",
           model.rxToneStatus() == DSD_APP_RX_TONE_LOCKED && model.rxToneText() == QStringLiteral("CTCSS 100.0 Hz"));
    state.analog_rx.stale_after_ms = 0U;

    /* The policy verdict field is reserved: no value of it moves either text. */
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    model.refresh(&opts, &state);
    expect("a gate value changes neither the received nor the configured text",
           model.rxToneText() == QStringLiteral("CTCSS 100.0 Hz")
               && model.rxToneConfiguredText() == QStringLiteral("off"));
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_OFF;

    /* A retune resets the publication: the row stays, with nothing heard yet. */
    DSD_MEMSET(&state.analog_rx, 0, sizeof(state.analog_rx));
    state.analog_rx.generation = 2U;
    model.refresh(&opts, &state);
    expect("after a retune the tone is gone",
           model.rxToneVisible() && model.rxToneStatus() == DSD_APP_RX_TONE_NO_CARRIER
               && model.rxToneText() == QStringLiteral("\u2014") && model.rxToneTenthsHz() == 0);

    state.analog_rx.carrier_open = 1;
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    model.refresh(&opts, &state);
    expect("a carrier under evaluation reads detecting",
           model.rxToneStatus() == DSD_APP_RX_TONE_DETECTING && model.rxToneText() == QStringLiteral("detecting"));
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    model.refresh(&opts, &state);
    expect("a carrier with no tone reads none",
           model.rxToneStatus() == DSD_APP_RX_TONE_NONE && model.rxToneText() == QStringLiteral("none"));

    /* A received DCS code (#523): both standard spellings of its signal, canonical first, with
       leading zeros, apart from the configured policy, which neither it nor a gate value
       moves. */
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    state.analog_rx.dcs_code = 023;
    state.analog_rx.dcs_inverted = 0;
    const int before_code = changes;
    model.refresh(&opts, &state);
    expect("a locked code shows both of its spellings",
           model.rxToneStatus() == DSD_APP_RX_TONE_LOCKED && model.rxToneText() == QStringLiteral("DCS D023N / D047I")
               && model.rxToneKind() == DSD_ANALOG_TONE_KIND_DCS && model.rxToneDcsCode() == 023
               && !model.rxToneDcsInverted() && model.rxToneDcsAliasCode() == 047 && model.rxToneDcsAliasInverted()
               && model.rxToneTenthsHz() == 0);
    expect("the received code notifies its own group", changes > before_code && configured_changes == 0);
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    model.refresh(&opts, &state);
    expect("a gate value changes neither text for a code",
           model.rxToneText() == QStringLiteral("DCS D023N / D047I")
               && model.rxToneConfiguredText() == QStringLiteral("off"));
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_OFF;
    state.analog_rx.dcs_code = 0754;
    model.refresh(&opts, &state);
    expect("another code replaces it", model.rxToneText() == QStringLiteral("DCS D754N / D116I")
                                           && model.rxToneDcsCode() == 0754 && model.rxToneDcsAliasCode() == 0116);

    /* A retune clears the code as it clears a tone. */
    DSD_MEMSET(&state.analog_rx, 0, sizeof(state.analog_rx));
    state.analog_rx.generation = 3U;
    model.refresh(&opts, &state);
    expect("after a retune the code is gone",
           model.rxToneStatus() == DSD_APP_RX_TONE_NO_CARRIER && model.rxToneKind() == 0 && model.rxToneDcsCode() == 0
               && !model.rxToneDcsInverted() && model.rxToneDcsAliasCode() == 0 && !model.rxToneDcsAliasInverted());
    state.analog_rx.carrier_open = 1;

    /* Stop with a code locked clears it too. */
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    state.analog_rx.dcs_code = 047;
    model.refresh(&opts, &state);
    expect("locked on a code before a stop", model.rxToneDcsCode() == 047 && model.rxToneDcsAliasCode() == 023
                                                 && model.rxToneText() == QStringLiteral("DCS D047N / D023I"));
    model.clear();
    expect("stop clears the received code", !model.rxToneVisible() && model.rxToneText().isEmpty()
                                                && model.rxToneKind() == 0 && model.rxToneDcsCode() == 0
                                                && !model.rxToneDcsInverted() && model.rxToneDcsAliasCode() == 0
                                                && !model.rxToneDcsAliasInverted());
    expect("stop keeps the configured policy with a code too", model.rxToneConfiguredText() == QStringLiteral("off"));
    DSD_MEMSET(&state.analog_rx, 0, sizeof(state.analog_rx));
    state.analog_rx.carrier_open = 1;

    /* Stop: clear() returns every rxTone reading to unknown and says so. */
    model.refresh(&opts, &state);
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state.analog_rx.ctcss_tenths_hz = 1318;
    model.refresh(&opts, &state);
    expect("locked again before the stop", model.rxToneTenthsHz() == 1318);
    const int before_stop = changes;
    model.clear();
    expect("stop clears the received tone", !model.rxToneVisible() && model.rxToneStatus() == DSD_APP_RX_TONE_HIDDEN
                                                && model.rxToneText().isEmpty() && model.rxToneKind() == 0
                                                && model.rxToneTenthsHz() == 0 && !model.rxToneCarrier());
    /* The configured policy is configuration, not session state: stop leaves it as it was. */
    expect("stop keeps the configured policy", model.rxToneConfiguredText() == QStringLiteral("off"));
    expect("stop notifies the received-tone group", changes > before_stop);
    expect("nothing received ever announced the configured policy", configured_changes == 0);

    freeState(&state);
}

/*
 * The Tone filter row (#527) through the real app-control view: shown while a policy is in force where detection runs,
 * the policy text on its configuration signal and the verdict on its own, neither moving the received tone, and the
 * verdict gone on stop while the policy stays.
 */
static void
test_tone_filter() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int received = 0;
    int configured = 0;
    int verdicts = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::rxToneChanged, [&]() { ++received; });
    QObject::connect(&model, &dsd_qt::MetricsModel::rxToneConfiguredTextChanged, [&]() { ++configured; });
    QObject::connect(&model, &dsd_qt::MetricsModel::toneFilterChanged, [&]() { ++verdicts; });
    opts.analog_only = 1;
    opts.monitor_input_audio = 1;
    opts.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    expect("the list parses", dsd_tone_set_parse("100.0/D023N", &opts.analog_tone_set, nullptr, 0) == 0);
    state.analog_rx.carrier_open = 1;
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_PENDING;
    model.refresh(&opts, &state);
    expect("the policy row shows the policy and its check",
           model.toneFilterVisible() && model.rxToneConfiguredText() == QStringLiteral("allow 100.0 Hz/D023N")
               && model.toneFilterGate() == DSD_ANALOG_TONE_GATE_PENDING
               && model.toneFilterStatusText() == QStringLiteral("muted: checking tone"));
    expect("the policy and its verdict each notify", configured == 1 && verdicts == 1);

    const int received_before = received;
    const int configured_before = configured;
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state.analog_rx.ctcss_tenths_hz = 670;
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    model.refresh(&opts, &state);
    expect("a rejected tone", model.toneFilterStatusText() == QStringLiteral("muted: not allowed")
                                  && model.rxToneText() == QStringLiteral("CTCSS 67.0 Hz"));
    expect("the verdict never announces the policy", configured == configured_before && received > received_before);
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    state.analog_rx.tone_kind = 0;
    state.analog_rx.ctcss_tenths_hz = 0;
    state.analog_rx.gate_no_tone = 1;
    model.refresh(&opts, &state);
    expect("rejected for want of a tone", model.toneFilterStatusText() == QStringLiteral("muted: no tone"));
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_ALLOWED;
    state.analog_rx.gate_no_tone = 0;
    model.refresh(&opts, &state);
    expect("passing", model.toneFilterStatusText() == QStringLiteral("passing"));

    /* Stop: the verdict goes, the policy is configuration and stays. */
    model.clear();
    expect("stop clears the verdict",
           !model.toneFilterVisible() && model.toneFilterStatusText().isEmpty() && model.toneFilterGate() == 0);
    expect("stop keeps the policy", model.rxToneConfiguredText() == QStringLiteral("allow 100.0 Hz/D023N"));
    /* A digital mode: no detection, no row, whatever is configured. */
    opts.analog_only = 0;
    model.refresh(&opts, &state);
    expect("no detection, no policy row", !model.toneFilterVisible() && model.toneFilterStatusText().isEmpty());
    freeState(&state);
}

/*
 * What the live tone-filter editor opens on, through the real app-control view: the row is reachable wherever detection
 * runs, a policy or none (toneFilterEditable); the configured mode and list, spelled as written, on their own signal
 * and kept across a stop as configuration; and whether a scan row's own policy shadows an edit. Never the row's policy.
 */
static void
test_tone_filter_editor_setting() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int settings = 0;
    int verdicts = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::toneFilterSettingChanged, [&]() { ++settings; });
    QObject::connect(&model, &dsd_qt::MetricsModel::toneFilterChanged, [&]() { ++verdicts; });
    expect("the editor opens on off before the first frame",
           model.toneFilterConfiguredMode() == DSD_TONE_FILTER_OFF && model.toneFilterConfiguredList().isEmpty()
               && !model.toneFilterEditable() && !model.toneFilterRowOverride());
    opts.analog_only = 1;
    opts.monitor_input_audio = 1;
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    model.refresh(&opts, &state);
    expect("no policy, but the editor is reachable where detection runs",
           model.toneFilterEditable() && !model.toneFilterVisible()
               && model.toneFilterConfiguredMode() == DSD_TONE_FILTER_OFF);
    expect("reachability is session state", verdicts == 1 && settings == 0);

    opts.analog_tone_filter = DSD_TONE_FILTER_BLOCK;
    expect("the list parses", dsd_tone_set_parse("D023I/100", &opts.analog_tone_set, nullptr, 0) == 0);
    model.refresh(&opts, &state);
    expect("the configured policy as written",
           model.toneFilterConfiguredMode() == DSD_TONE_FILTER_BLOCK
               && model.toneFilterConfiguredList() == QStringLiteral("100.0/D023I"));
    expect("the setting announces itself once", settings == 1);
    model.refresh(&opts, &state);
    expect("an unchanged setting stays quiet", settings == 1);

    /* An nfm row's own policy on air: the editor still opens on the configured one, and knows the row shadows it. */
    expect("scope",
           dsd_scan_mode_begin(&opts, &state) == 0 && dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) == 0);
    dsd_scan_option_values row = {};
    row.present = DSD_SCAN_OPT_TONE;
    row.tone_filter = DSD_TONE_FILTER_ALLOW;
    expect("row list", dsd_tone_set_parse("67.0", &row.tone_set, nullptr, 0) == 0);
    expect("row installed", dsd_scan_mode_options(&opts, &state, &row) == 0);
    model.refresh(&opts, &state);
    expect("under a row: the configured policy, shadowed",
           model.toneFilterConfiguredMode() == DSD_TONE_FILTER_BLOCK
               && model.toneFilterConfiguredList() == QStringLiteral("100.0/D023I") && model.toneFilterRowOverride());
    dsd_scan_mode_leave(&opts, &state);
    model.refresh(&opts, &state);
    expect("the row gone, nothing shadows", !model.toneFilterRowOverride());

    /* Stop: the configured policy stays for the next session's editor; reachability goes. */
    const int settings_before = settings;
    model.clear();
    expect("stop keeps the configured policy", model.toneFilterConfiguredMode() == DSD_TONE_FILTER_BLOCK
                                                   && model.toneFilterConfiguredList() == QStringLiteral("100.0/D023I")
                                                   && settings == settings_before);
    expect("stop takes the editor away", !model.toneFilterEditable() && !model.toneFilterRowOverride());
    /* A digital mode: nothing to edit on screen, the setting still read. */
    opts.analog_only = 0;
    model.refresh(&opts, &state);
    expect("no detection, no editor",
           !model.toneFilterEditable() && model.toneFilterConfiguredList() == QStringLiteral("100.0/D023I"));
    freeState(&state);
}

/* Issue #518: the scan row on air for the "this channel" editors, from the app-control scan row view: the name and the
 * fields it takes, sets in its list and runs an edit of, while the scanner that published it runs. */
static void
test_scan_row() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int controls = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++controls; });
    model.refresh(&opts, &state);
    expect("no scan row, nothing to edit", !model.scanRowActive() && model.scanRowEditable() == 0);
    opts.trunk_scan_enabled = 1;
    state.scan_row_scanner = (uint8_t)DSD_SCAN_ROW_SCANNER_TRUNK_SCAN;
    state.scan_row_session = 5U;
    state.scan_row_index = 0;
    state.scan_row_editable = (uint8_t)(DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_GAIN);
    state.scan_row_listed = (uint8_t)DSD_SCAN_ROW_FIELD_SQUELCH;
    state.scan_row_edited = (uint8_t)DSD_SCAN_ROW_FIELD_GAIN;
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof state.trunk_scan_active_id, "%s", "county-p25");
    const int before = controls;
    model.refresh(&opts, &state);
    expect("a trunk target on air",
           model.scanRowActive() && model.scanRowLabel() == QStringLiteral("county-p25")
               && model.scanRowEditable() == (DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_GAIN)
               && model.scanRowListed() == DSD_SCAN_ROW_FIELD_SQUELCH
               && model.scanRowEdited() == DSD_SCAN_ROW_FIELD_GAIN);
    expect("the row announces itself", controls == before + 1);
    expect("the readings are the row's", model.scanRowSynced());
    /* The width setting a "this channel" step starts from is the options snapshot's, the row's own while on air. */
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 12500;
    model.refresh(&opts, &state);
    expect("the width setting in force", model.analogBandwidthSettingHz() == 12500 && controls == before + 2);
    opts.analog_nfm_bandwidth_hz = 0;
    model.refresh(&opts, &state);
    expect("the default's setting", model.analogBandwidthSettingHz() == 0 && controls == before + 3);
    /* Options from another row's scope (read between the decoder's two publishes): the row stays on air, and the
       readings beside it are not its own until the next pair. */
    state.scan_row_scope_seq = opts.scan_row_scope_seq + 1U;
    model.refresh(&opts, &state);
    expect("snapshots of two scopes", model.scanRowActive() && !model.scanRowSynced() && controls == before + 4);
    state.scan_row_scope_seq = opts.scan_row_scope_seq;
    model.refresh(&opts, &state);
    expect("one scope again", model.scanRowSynced() && controls == before + 5);
    state.scan_row_edited = 0U;
    model.refresh(&opts, &state);
    expect("an edit dropped is announced", model.scanRowEdited() == 0 && controls == before + 6);
    opts.trunk_scan_enabled = 0;
    model.refresh(&opts, &state);
    expect("the scanner gone, no row", !model.scanRowActive() && model.scanRowLabel().isEmpty());
    freeState(&state);
}

/* Issue #671: a scan the operator paused owns nothing. The tuner gate, the rotation the scan controls act on and the
 * target on air all read the app-control scan-state view, so a paused --trunk-scan (which keeps trunk_scan_enabled, its
 * configured setting) offers the tuning a running one holds back, and the scan controls give way to a resume. */
static void
test_scan_state() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_gain_value = 30;
    dsd_qt::MetricsModel model;
    int controls = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&] { ++controls; });

    /* A running trunk scan on a target: it owns the tuner. */
    opts.trunk_scan_enabled = 1;
    state.trunk_scan_target_count = 3U;
    state.trunk_scan_active_ordinal = 2U;
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof state.trunk_scan_active_id, "%s", "dispatch");
    state.trunk_scan_configured_gain = 20;
    state.trunk_scan_gain_override = 1U;
    model.refresh(&opts, &state);
    expect("a running trunk scan owns the tuner", model.tunerControlled() && model.scanRotationActive());
    expect("its target is on air", model.scanTargetId() == QStringLiteral("dispatch") && model.scanTargetOrdinal() == 2
                                       && model.scanTargetCount() == 3);
    expect("a target's own gain is flagged while it runs", model.tunerGainRowOverride());
    expect("a running trunk scan is not paused", model.scanKind() == DSD_APP_SCAN_KIND_TRUNK && !model.scanPaused()
                                                     && !model.scanResumable() && model.scanEntryCount() == 3
                                                     && !model.operatorTuned());

    /* Paused (the coordinator clears the target on air and keeps the list): nothing owns the tuner. */
    state.trunk_scan_paused = 1U;
    state.trunk_scan_active_id[0] = '\0';
    state.trunk_scan_active_ordinal = 0U;
    state.operator_tuned = 1U;
    const int before = controls;
    model.refresh(&opts, &state);
    expect("a paused trunk scan holds no tuner", !model.tunerControlled());
    expect("a paused trunk scan is no rotation for hold, avoid and next", !model.scanRotationActive());
    expect("the pause is announced", controls == before + 1);
    expect("no target on air while paused", model.scanTargetId().isEmpty() && model.scanTargetOrdinal() == 0);
    expect("the list keeps its size", model.scanTargetCount() == 3);
    expect("the gain buttons still edit the scan's configured gain", model.configuredTunerGainDb() == 20);
    expect("no target overrides the gain while paused", !model.tunerGainRowOverride());
    expect("the trunk scan reads paused and resumable", model.scanKind() == DSD_APP_SCAN_KIND_TRUNK
                                                            && model.scanPaused() && model.scanResumable()
                                                            && model.scanEntryCount() == 3);
    expect("the operator holds the tuner", model.operatorTuned());
    /* N6: the gain controls still edit the retained scan's configured gain and autogain while it is paused, and the
     * target's override (still published from the target it left) names nothing: the label, the edit notice and the
     * autogain label the terminal shows come from the same view the model reads. */
    {
        opts.rtl_gain_value = 20; // the pause restored the configured gain
        state.trunk_scan_configured_autogain = 1U;
        dsd_app_rtl_gain_view gain{};
        expect("paused gain view", dsd_app_rtl_gain_view_get(&opts, &state, &gain) == 0);
        char text[96];
        (void)dsd_app_rtl_gain_view_label(&gain, text, sizeof text);
        expect("paused gain label names no target", std::strcmp(text, "Gain... [20]") == 0);
        (void)dsd_app_rtl_gain_view_edit_notice(&gain, text, sizeof text);
        expect("paused gain notice names no target", std::strcmp(text, "Applied: RTL gain -> 20") == 0);
        (void)dsd_app_rtl_autogain_view_label(&gain, 0, text, sizeof text);
        expect("paused autogain label is the scan's configured one",
               std::strcmp(text, "Tuner autogain [On] (suspended: gain 20)") == 0);
        (void)dsd_app_rtl_autogain_view_edit_notice(&gain, text, sizeof text);
        expect("paused autogain toggle notice",
               std::strcmp(text, "Default tuner autogain -> On; manual gain 20 suspends it") == 0);
        state.trunk_scan_configured_autogain = 0U;
        opts.rtl_gain_value = 30;
    }

    /* A paused -Y list: scanner_mode 0 and lcn_scan_paused set. Its size is the list's rows. */
    opts.trunk_scan_enabled = 0;
    state.trunk_scan_paused = 0U;
    state.trunk_scan_target_count = 0U;
    state.lcn_scan_paused = 1U;
    state.lcn_freq_count = 5;
    model.refresh(&opts, &state);
    expect("a paused -Y list", model.scanKind() == DSD_APP_SCAN_KIND_CHANNEL && model.scanPaused()
                                   && model.scanResumable() && model.scanEntryCount() == 5);
    expect("a paused -Y list holds no tuner and runs no rotation",
           !model.tunerControlled() && !model.scanRotationActive() && model.scanTargetCount() == 0);
    /* A resume waiting on its row's tune: still paused, and nothing more to ask for. */
    state.lcn_scan_resume_pending = 1U;
    model.refresh(&opts, &state);
    expect("a pending resume is paused but not resumable", model.scanPaused() && !model.scanResumable());
    /* The resume lands. */
    state.lcn_scan_resume_pending = 0U;
    state.lcn_scan_paused = 0U;
    state.operator_tuned = 0U;
    opts.scanner_mode = 1;
    model.refresh(&opts, &state);
    expect("a running -Y list", model.scanKind() == DSD_APP_SCAN_KIND_CHANNEL && !model.scanPaused()
                                    && model.tunerControlled() && model.scanRotationActive() && !model.operatorTuned());
    /* A stop leaves the tuner with the operator: no scan, operator_tuned kept (Explore under -T reads the same). */
    opts.scanner_mode = 0;
    state.operator_tuned = 1U;
    model.refresh(&opts, &state);
    expect("no scan, the operator holds the tuner", model.scanKind() == DSD_APP_SCAN_KIND_NONE && !model.scanPaused()
                                                        && model.operatorTuned() && model.scanEntryCount() == 0);
    model.clear();
    expect("a cleared model has no scan",
           model.scanKind() == DSD_APP_SCAN_KIND_NONE && !model.scanPaused() && !model.operatorTuned());

    freeState(&state);
}

/* DMR RAS and the Relaxed CRC policy, from the app-control views every frontend reads (dmr_ras_view.h,
 * crc_policy_view.h): the site sheet's RAS field and hint, and the Settings switch with its "this channel" line. Read
 * by property name, as QML reads them. */
static void
test_dmr_ras_and_crc_policy() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int sites = 0;
    int controls = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::siteChanged, [&]() { ++sites; });
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++controls; });
    const auto read = [&model](const char* name) { return model.property(name); };

    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 1U);
    model.refresh(&opts, &state);
    expect("ras: the properties exist", read("dmrRasDetected").isValid() && read("dmrRasNeedsRelaxed").isValid()
                                            && read("relaxedCrcConfigured").isValid()
                                            && read("relaxedCrcEffective").isValid()
                                            && read("relaxedCrcRowOverride").isValid());
    expect("ras: none on a plain DMR carrier",
           !read("dmrRasDetected").toBool() && !read("dmrRasNeedsRelaxed").toBool());
    expect("ras: strict by default", !read("relaxedCrcConfigured").toBool() && !read("relaxedCrcEffective").toBool()
                                         && !read("relaxedCrcRowOverride").toBool());

    int site_before = sites;
    dsd_rxid_note_dmr_ras(&state);
    model.refresh(&opts, &state);
    expect("ras: detected, relaxed CRC needed",
           read("dmrRasDetected").toBool() && read("dmrRasNeedsRelaxed").toBool() && sites == site_before + 1);

    int control_before = controls;
    site_before = sites;
    dsd_opts_set_relaxed_crc(&opts, 1);
    model.refresh(&opts, &state);
    expect("ras: relaxed CRC on decodes it", read("dmrRasDetected").toBool() && !read("dmrRasNeedsRelaxed").toBool());
    expect("ras: the policy follows -F", read("relaxedCrcConfigured").toBool() && read("relaxedCrcEffective").toBool()
                                             && !read("relaxedCrcRowOverride").toBool());
    expect("ras: the policy and the hint each announce themselves",
           controls == control_before + 1 && sites == site_before + 1);

    /* A scan row with its own --strict-crc decides for this channel while the configured value stays on. */
    expect("ras: enter a DMR row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    dsd_scan_option_values strict_row{};
    strict_row.present = DSD_SCAN_OPT_CRC;
    strict_row.strict_crc = 1;
    expect("ras: strict row installed", dsd_scan_mode_options(&opts, &state, &strict_row) == 0);
    model.refresh(&opts, &state);
    expect("ras: a strict row overrides", read("relaxedCrcConfigured").toBool() && !read("relaxedCrcEffective").toBool()
                                              && read("relaxedCrcRowOverride").toBool());
    expect("ras: the strict row needs relaxed CRC again", read("dmrRasNeedsRelaxed").toBool());
    dsd_scan_mode_leave(&opts, &state);
    model.refresh(&opts, &state);
    expect("ras: leaving the row restores the configured policy", read("relaxedCrcEffective").toBool()
                                                                      && !read("relaxedCrcRowOverride").toBool()
                                                                      && !read("dmrRasNeedsRelaxed").toBool());

    /* Another carrier: no RAS until it proves its own. */
    dsd_rxid_reset(&state);
    model.refresh(&opts, &state);
    expect("ras: a carrier boundary clears it",
           !read("dmrRasDetected").toBool() && !read("dmrRasNeedsRelaxed").toBool());
    model.clear();
    expect("ras: a stop clears the policy",
           !read("relaxedCrcConfigured").toBool() && !read("relaxedCrcEffective").toBool());
    freeState(&state);
}

/* Issue #621: on audio input with a rigctl peer (-U) that demodulates it, the analog width is the passband the peer is
 * asked for. The monitor's Passband row offers it, no DSP rate bounds it, and an unset NFM width stands for -B (or the
 * peer's own passband without one), which the sheet shows and steps from. Without the peer PCM input filters nothing,
 * and a radio input has a channel filter instead. */
static void
test_rigctl_audio_passband() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int tuner = 0;
    int controls = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::tunerChanged, [&]() { ++tuner; });
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++controls; });

    opts.audio_in_type = AUDIO_IN_TCP;
    opts.analog_only = 0;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 0;
    opts.setmod_bw = 12500;
    opts.use_rigctl = 0;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;

    /* A digital session with no width set: the peer changes no reading and offers nothing, so the flag alone moves the
     * tuner group, and the spelling of an unset width (-B 16 kHz, the same width as the default) the control group. */
    opts.setmod_bw = 16000;
    model.refresh(&opts, &state);
    int tuner_before = tuner;
    int controls_before = controls;
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = (dsd_socket_t)5;
    model.refresh(&opts, &state);
    expect("digital peer: the passband flag", model.peerPassband() && model.analogBandwidthReading().isEmpty()
                                                  && !model.nfmBandwidthOffered() && !model.amBandwidthOffered());
    expect("digital peer: the flag alone moves the tuner group", tuner == tuner_before + 1);
    expect("digital peer: no analog monitor runs, so no passband is in force", !model.passbandInForce());
    expect("digital peer: the spelling alone moves the control group",
           controls == controls_before + 1 && model.nfmBandwidthUnsetText() == QStringLiteral("-B 16 kHz")
               && model.nfmBandwidthUnsetHz() == 16000);
    /* An explicit NFM default on the digital session offers the NFM width, and the reading is what the peer is asked
     * for now, FM at -B, under a typed digital row on air as well: never the configured width, which nothing asks
     * for (the Passband row's summary and the terminal's field read it). */
    opts.analog_nfm_bandwidth_hz = 20000;
    opts.setmod_bw = 12500;
    expect("digital peer: typed digital row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    expect("digital peer: typed digital row options", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
    model.refresh(&opts, &state);
    expect("digital peer, explicit width: NFM offered", model.nfmBandwidthOffered() && !model.passbandInForce());
    expect("digital peer, explicit width: the reading is -B",
           model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)") && !model.analogBandwidthAm());
    dsd_scan_mode_leave(&opts, &state);
    model.refresh(&opts, &state);
    expect("digital peer, explicit width, no row: the reading is -B",
           model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)"));
    opts.analog_nfm_bandwidth_hz = 0;
    opts.setmod_bw = 16000;
    model.refresh(&opts, &state);
    expect("digital peer, no width: no reading", model.analogBandwidthReading().isEmpty());
    opts.use_rigctl = 0;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    opts.setmod_bw = 12500;

    /* The analog FM monitor (-fA) without the peer. */
    opts.analog_only = 1;
    model.refresh(&opts, &state);
    expect("no peer: no passband", !model.peerPassband());
    expect("no peer: no width offered", !model.nfmBandwidthOffered() && !model.amBandwidthOffered());
    expect("no peer: PCM input filters nothing",
           model.analogBandwidthReading() == QStringLiteral("not used on PCM input"));
    expect("no peer: an unset width reads as the default",
           model.nfmBandwidthUnsetText() == QStringLiteral("default")
               && model.amBandwidthUnsetText() == QStringLiteral("default"));
    expect("no peer: an unset width stands for the kind's default",
           model.nfmBandwidthUnsetHz() == 16000 && model.amBandwidthUnsetHz() == 6000);

    /* The peer connects: the FM monitor asks it for -B, standing in for the unset NFM width. */
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = (dsd_socket_t)5;
    tuner_before = tuner;
    controls_before = controls;
    model.refresh(&opts, &state);
    expect("peer: the passband", model.peerPassband());
    expect("peer: the FM monitor runs, so its passband is in force", model.passbandInForce());
    expect("peer: the NFM width offered, AM's not", model.nfmBandwidthOffered() && !model.amBandwidthOffered());
    expect("peer: no DSP rate bounds the passband", model.analogBandwidthMaxHz() == 0);
    expect("peer: -B stands in for the unset width", model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)"));
    expect("peer: an unset NFM width reads as -B, AM's as the default",
           model.nfmBandwidthUnsetText() == QStringLiteral("-B 12.5 kHz")
               && model.amBandwidthUnsetText() == QStringLiteral("default"));
    expect("peer: an unset NFM width steps from -B, AM's from its default",
           model.nfmBandwidthUnsetHz() == 12500 && model.amBandwidthUnsetHz() == 6000);
    expect("peer: the flag moves the tuner group", tuner == tuner_before + 1);
    expect("peer: the stand-ins move the control group", controls == controls_before + 1);

    /* With the NFM width unset a typed digital row on air asks for the same -B, so the reading holds, and the flag
     * alone moves the tuner group. */
    expect("peer: typed digital row over -B", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    expect("peer: typed digital row over -B options", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
    tuner_before = tuner;
    model.refresh(&opts, &state);
    expect("peer: under a typed digital row -B is no setting's passband",
           model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)") && !model.passbandInForce());
    expect("peer: the in-force flag alone moves the tuner group", tuner == tuner_before + 1);
    dsd_scan_mode_leave(&opts, &state);
    model.refresh(&opts, &state);
    expect("peer: the row's leave puts -B back in force", model.passbandInForce());

    /* An explicit NFM width is the passband asked for; -B still names what an unset one would be. */
    opts.analog_nfm_bandwidth_hz = 20000;
    model.refresh(&opts, &state);
    expect("peer: an explicit width is the passband", model.analogBandwidthReading() == QStringLiteral("20 kHz"));
    expect("peer: the explicit width is configured", model.nfmBandwidthConfiguredHz() == 20000);
    expect("peer: -B still stands in for an unset width",
           model.nfmBandwidthUnsetText() == QStringLiteral("-B 12.5 kHz"));

    /* A typed digital row on air keeps the peer following at -B: the reading is not the configured width's, and the
     * flag alone says so, the configured width staying what a step moves. */
    expect("peer: typed digital row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    expect("peer: typed digital row options", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
    model.refresh(&opts, &state);
    expect("peer: under a typed digital row the peer follows at -B",
           model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)") && !model.passbandInForce());
    expect("peer: under a typed digital row the configured width stays", model.nfmBandwidthConfiguredHz() == 20000);
    dsd_scan_mode_leave(&opts, &state);
    model.refresh(&opts, &state);
    expect("peer: the row's leave brings the configured passband back",
           model.analogBandwidthReading() == QStringLiteral("20 kHz") && model.passbandInForce());

    /* -B moves under the explicit width: the reading holds, and only what an unset width stands for changes. */
    opts.setmod_bw = 16000;
    tuner_before = tuner;
    controls_before = controls;
    model.refresh(&opts, &state);
    expect("peer: -B under an explicit width leaves the reading",
           model.analogBandwidthReading() == QStringLiteral("20 kHz") && tuner == tuner_before);
    expect("peer: -B under an explicit width moves the stand-in",
           model.nfmBandwidthUnsetText() == QStringLiteral("-B 16 kHz") && model.nfmBandwidthUnsetHz() == 16000
               && controls == controls_before + 1);

    /* Neither a width nor -B: the peer keeps its own passband. */
    opts.analog_nfm_bandwidth_hz = 0;
    opts.setmod_bw = 0;
    model.refresh(&opts, &state);
    expect("peer: without -B the peer's own passband", model.analogBandwidthReading() == QStringLiteral("peer's own"));
    expect("peer: an unset width stands for the peer's own",
           model.nfmBandwidthUnsetText() == QStringLiteral("peer's own") && model.nfmBandwidthUnsetHz() == 0);

    /* A radio input has a channel filter, whatever rigctl follows. */
    opts.setmod_bw = 12500;
    opts.audio_in_type = AUDIO_IN_RTL;
    model.refresh(&opts, &state);
    expect("radio input: no peer passband", !model.peerPassband() && model.radioInput());
    expect("radio input: an unset width is the default",
           model.nfmBandwidthUnsetText() == QStringLiteral("default") && model.nfmBandwidthUnsetHz() == 16000);

    opts.use_rigctl = 0;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    freeState(&state);
}

/* The TETRA rows reach QML from the snapshot handed to refresh(), through the app-control view the terminal shares:
   hidden outside the TETRA preset and until the publication holds a working mode, every value spelled by the view, and
   announced on their own signal only when they change. */
static void
test_tetra_rows() {
    dsd_opts* opts = static_cast<dsd_opts*>(calloc(1U, sizeof(dsd_opts)));
    dsd_state* snapshot = static_cast<dsd_state*>(calloc(1U, sizeof(dsd_state)));
    if (opts == nullptr || snapshot == nullptr) {
        expect("TETRA test allocates", false);
        free(opts);
        free(snapshot);
        return;
    }
    opts->audio_in_type = AUDIO_IN_PULSE;
    dsd_tetra_publication* const pub = &snapshot->tetra_pub;
    pub->mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    pub->air_encryption = DSD_TETRA_AIR_ENC_ENCRYPTED;
    pub->ksg_mask = 1U << 1;
    pub->net_known = 1U;
    pub->mcc = 262U;
    pub->mnc = 1001U;
    pub->colour = 25U;
    dsd_rxid_note_tetra_cell(snapshot, 262U, 1001U, 25U); /* the cell's checked BSCH, received on this carrier */
    pub->trunk_state = DSD_TETRA_TRUNK_ON_TRAFFIC;
    pub->cc_known = 1U;
    pub->cc_carrier = 321U;
    pub->cc_freq_hz = 392012500U;
    pub->traffic_valid = 1U;
    pub->traffic_carrier = 654U;
    pub->traffic_slots = 0x0AU; /* timeslots 1 and 3: bit 3 is timeslot 1 */
    pub->traffic_freq_hz = 392125000U;
    pub->mm_status_valid = 1U;
    pub->mm_status_code = 7U;
    pub->codec_status = DSD_TETRA_CODEC_DECODE_ERRORS;
    pub->codec_frames = 18U;
    pub->codec_errors = 2U;

    dsd_qt::MetricsModel model;
    int changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::tetraChanged, [&changes]() { ++changes; });
    model.refresh(opts, snapshot);
    expect("TETRA rows stay hidden outside the TETRA preset",
           !model.tetraVisible() && model.tetraModeText().isEmpty() && changes == 0);

    opts->frame_tetra = 1;
    opts->trunk_enable = 1;
    model.refresh(opts, snapshot);
    expect("TETRA rows show under the TETRA preset", model.tetraVisible() && changes == 1);
    expect("TETRA mode line", model.tetraModeText() == QString::fromUtf8("TMO downlink \xC2\xB7 encrypted (TEA2)"));
    expect("TETRA encryption flag", model.tetraEncrypted());
    expect("TETRA network names its colour code in decimal",
           model.tetraNetworkText() == QString::fromUtf8("MCC 262 \xC2\xB7 MNC 1001 \xC2\xB7 CC 25"));
    expect("TETRA trunk state", model.tetraTrunkText() == QStringLiteral("traffic channel") && model.tetraOnTraffic());
    expect("TETRA control channel", model.tetraControlText() == QString::fromUtf8("carrier 321 \xC2\xB7 392.0125 MHz"));
    expect("TETRA traffic channel lists timeslots TN1 first",
           model.tetraTrafficText() == QString::fromUtf8("carrier 654 \xC2\xB7 TS 1,3 \xC2\xB7 392.1250 MHz"));
    expect("TETRA MM status", model.tetraMmStatusText() == QString::fromUtf8("7 \xC2\xB7 MS frequency bands request"));
    expect("TETRA codec health",
           model.tetraCodecText() == QString::fromUtf8("decode errors \xC2\xB7 18 frames \xC2\xB7 2 errors")
               && model.tetraCodecFault());

    model.refresh(opts, snapshot);
    expect("an unchanged TETRA publication announces nothing", changes == 1);
    expect("TETRA security row hidden while nothing is known", model.tetraSecurityText().isEmpty());

    /* A change of the cell's security alone is announced, and clearing it hides the row again. */
    pub->aie = DSD_TETRA_AIE_AVAILABLE;
    pub->sec_known = 1U;
    pub->sec_sckn = 3U;
    pub->sec_auth = 1U;
    model.refresh(opts, snapshot);
    expect("a security-only change is announced", changes == 2);
    expect("TETRA security row",
           model.tetraSecurityText() == QString::fromUtf8("class 2 \xC2\xB7 SCKN 3 \xC2\xB7 authentication"));
    pub->sec_auth = 0U;
    model.refresh(opts, snapshot);
    expect("and so is a change of one security element",
           changes == 3 && model.tetraSecurityText() == QString::fromUtf8("class 2 \xC2\xB7 SCKN 3"));
    pub->aie = DSD_TETRA_AIE_UNKNOWN;
    pub->sec_known = 0U;
    pub->sec_sckn = 0U;
    model.refresh(opts, snapshot);
    expect("clearing the security hides its row", changes == 4 && model.tetraSecurityText().isEmpty());

    /* Trunking off: no trunk row, the channels stay. */
    opts->trunk_enable = 0;
    model.refresh(opts, snapshot);
    expect("TETRA trunk row needs trunking", model.tetraTrunkText().isEmpty() && !model.tetraOnTraffic()
                                                 && !model.tetraControlText().isEmpty() && changes == 5);

    /* A reset (a mode change, or the TETRA state dropped) hides the rows again. */
    dsd_tetra_publication_reset(pub);
    model.refresh(opts, snapshot);
    expect("a reset TETRA publication hides the rows",
           !model.tetraVisible() && model.tetraModeText().isEmpty() && model.tetraNetworkText().isEmpty()
               && model.tetraCodecText().isEmpty() && !model.tetraCodecFault() && changes == 6);

    dsd_state_ext_free_all(snapshot);
    free(snapshot);
    free(opts);
}

/* The block label names a radio the radio list blocks, for a call whose target is known and for one whose target is
   not (only its source is). */
static void
test_block_label_radio_blocked() {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.trunk_tune_group_calls = 1;
    expect("radio list installed", dsd_test_install_radio_policy(&state, 777U, "B", 0, 0, 0) == 0);
    dsd_qt::MetricsModel model;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        dsd_call_observation observation = {};
        observation.protocol = DSD_SYNC_P25P2_POS;
        observation.slot = slot;
        observation.kind = DSD_CALL_KIND_GROUP_VOICE;
        observation.ota_target_id = slot == 0 ? 123U : 0U;
        observation.policy_target_id = observation.ota_target_id;
        observation.ota_source_id = 777U;
        observation.observed_m = 4.0;
        dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    }
    model.refresh(&opts, &state);
    const auto slots = model.decryptionSlots();
    expect("two slots listed", slots.size() == 2);
    expect("a known target's blocked radio is labelled",
           slots.size() == 2 && slots[0].toMap().value("blockReason") == QStringLiteral("radio blocked"));
    expect("a source-only call's blocked radio is labelled",
           slots.size() == 2 && slots[1].toMap().value("blockReason") == QStringLiteral("radio blocked"));
    expect("radio list cleared", dsd_source_alias_clear(&state) == 0);
    model.refresh(&opts, &state);
    expect("no label without the radio list",
           model.decryptionSlots().size() == 2
               && model.decryptionSlots()[1].toMap().value("blockReason").toString().isEmpty());
    dsd_state_ext_free_all(&state);
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_new_target_drops_sync_hold_in_one_publication();
    test_tetra_rows();
    test_slot_kind_and_labels();
    test_scan_row();
    test_scan_state();
    test_scan_avoid_list_model_diff();
    test_scan_list_toggle_switch_and_hold();
    test_scan_list_toggle_refusals();
    test_scan_list_toggle_history_gap();
    test_scan_list_toggle_session_change();
    test_scan_list_readings();
    test_scan_avoids();
    test_scan_avoids_large_list();
    test_dmr_ras_and_crc_policy();
    test_rigctl_audio_passband();
    test_options_readiness();
    test_replay_input();
    test_radio_source();
    test_decode_clock_readings();
    test_temporary_lockout_metrics();
    test_call_skip_metrics();
    test_scan_timing();
    test_rx_tone();
    test_receive_filter_status();
    test_tone_filter();
    test_tone_filter_editor_setting();
    test_direct_key_presence();
    test_decryption_metadata();
    test_block_label_radio_blocked();
    test_site();
    test_quality();
    test_lead_slot_keeps_earlier_call_and_quality();
    test_lead_slot_change_alone_notifies();
    test_two_slot_carrier_mirrors_the_decoder();
    test_quality_without_identity(DSD_SYNC_DMR_BS_VOICE_POS, 0);
    test_quality_without_identity(DSD_SYNC_DMR_BS_VOICE_POS, 1);
    test_quality_without_identity(DSD_SYNC_NXDN_POS, 0);

    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 769768750;
    opts.rtl_gain_value = 30;
    opts.rtl_squelch_level = dB_to_pwr(-120.0);
    opts.mod_c4fm = 1;
    opts.mod_qpsk = 0;

    dsd_qt::MetricsModel model;

    dsd_call_observation emergency = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 123U, 456U);
    emergency.kind = DSD_CALL_KIND_GROUP_VOICE;
    emergency.has_service_metadata = 1;
    emergency.emergency = 1;
    emergency.priority = 3;
    emergency.observed_m = dsd_decode_now_mono_s();
    dsd_call_state_observe(&state, &emergency, DSD_CALL_BOUNDARY_BEGIN);
    model.refresh(&opts, &state);
    expect("emergency and priority reach metrics", model.slot1CallEmergency() && model.slot1CallPriority() == 3);
    model.clear();
    expect("clear resets emergency and priority", !model.slot1CallEmergency() && model.slot1CallPriority() == 0);
    dsd_state_ext_free_all(&state);

    /* Nothing has synced: the strip must say so rather than default to a lock. */
    state.synctype = DSD_SYNC_NONE;
    state.lastsynctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("an unsynced decoder does not read as locked", !model.syncedHere());
    expect("an unsynced decoder names nothing", model.syncLabel().isEmpty());

    /* A live sync latches, and names what it locked to. */
    state.synctype = DSD_SYNC_P25P1_POS;
    model.refresh(&opts, &state);
    expect("a live sync reads as locked", model.syncedHere());
    expect("a live sync names the protocol", model.syncLabel().contains(QStringLiteral("P25")));

    /* Sync comes and goes between polls, so a single unsynced frame must not
     * drop the lock — that is what the hold is for. */
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("one unsynced poll does not drop the lock", model.syncedHere());

    /* But the hold expires. Nothing clears it explicitly: a decoder told to look
     * for another protocol simply stops finding this one, and the reading has to
     * follow that on its own. This is the case the shipped bug failed. */
    model.expireSyncForTest();
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("a decoder that stopped finding anything stops reading as locked", !model.syncedHere());
    expect("an expired lock names nothing", model.syncLabel().isEmpty());

    /* Whether trunking has anything to follow here. A control offering to hand
     * the tuner over needs the protocol, not merely a lock: on M17 there is no
     * trunking to hand it to, and on nothing at all there is nothing to follow. */
    expect("no lock is not something to follow", !model.trunkableSync());

    state.synctype = DSD_SYNC_P25P1_POS;
    model.refresh(&opts, &state);
    expect("P25p1 is something trunking can follow", model.trunkableSync());

    model.expireSyncForTest();
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    model.refresh(&opts, &state);
    expect("DMR is something trunking can follow", model.trunkableSync());

    model.expireSyncForTest();
    state.synctype = DSD_SYNC_M17_STR_POS;
    model.refresh(&opts, &state);
    expect("M17 locks without giving trunking anything to follow", !model.trunkableSync());
    expect("M17 still reads as locked", model.syncedHere());

    model.expireSyncForTest();
    state.synctype = DSD_SYNC_DSTAR_VOICE_POS;
    model.refresh(&opts, &state);
    expect("D-STAR locks without giving trunking anything to follow", !model.trunkableSync());

    /* Rides the decayed lock, not the raw frame: a control that vanished on the
     * one unsynced poll between two synced ones would flicker under the finger. */
    model.expireSyncForTest();
    state.synctype = DSD_SYNC_P25P2_POS;
    model.refresh(&opts, &state);
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("one unsynced poll does not withdraw the offer", model.trunkableSync());

    model.expireSyncForTest();
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("an expired lock withdraws the offer", !model.trunkableSync());

    /* The panel's own readings come from the options, not from what anything
     * asked for. Squelch is stored as mean power and must reach QML as decibels,
     * or a -120 dB threshold renders as 0. */
    expect("gain is reported", model.tunerGainDb() == 30);
    expect("without a trunk scan the buttons edit the gain in force", model.configuredTunerGainDb() == 30);
    expect("without a trunk scan no target overrides the gain", !model.tunerGainRowOverride());
    expect("squelch is reported in dB", std::fabs(model.squelchDb() - (-120.0)) < 0.5);

    /* Issue #518 follow-up: under --trunk-scan the buttons edit the configured gain the coordinator publishes, and a
     * parked target with its own rtl_gain is flagged, from the same app_control view the terminal uses. */
    opts.trunk_scan_enabled = 1;
    state.trunk_scan_target_count = 2U;
    state.trunk_scan_configured_gain = 20;
    state.trunk_scan_gain_override = 1U;
    model.refresh(&opts, &state);
    expect("the target's own gain is the reading", model.tunerGainDb() == 30);
    expect("the configured gain is what the buttons edit", model.configuredTunerGainDb() == 20);
    expect("the target's override is flagged", model.tunerGainRowOverride());
    opts.trunk_scan_enabled = 0;
    state.trunk_scan_target_count = 0U;
    state.trunk_scan_configured_gain = 0;
    state.trunk_scan_gain_override = 0U;
    model.refresh(&opts, &state);

    /* A threshold at the display floor is still a threshold. Only a level that
     * gates nothing is off, and the panel has to be able to tell them apart --
     * both rendered as "-120 dB" there was no way to see which one was in force. */
    expect("a -120 dB threshold is not off", !model.squelchOff());

    opts.rtl_squelch_level = 0.0;
    model.refresh(&opts, &state);
    expect("a squelch that gates nothing reads as off", model.squelchOff());

    opts.rtl_squelch_level = dB_to_pwr(-120.0);
    model.refresh(&opts, &state);
    expect("a restored threshold stops reading as off", !model.squelchOff());
    expect("c4fm reads as modulation 0", model.modulation() == 0);

    opts.mod_qpsk = 1;
    opts.mod_c4fm = 0;
    model.refresh(&opts, &state);
    expect("qpsk reads as modulation 1", model.modulation() == 1);

    /* GFSK is a third state, not an absence of QPSK. Reported as C4FM, it made a
     * control bound to this reading show C4FM as already-selected on a DMR or
     * EDACS/ProVoice session that had never been on it. */
    opts.mod_qpsk = 0;
    opts.mod_c4fm = 0;
    opts.mod_gfsk = 1;
    model.refresh(&opts, &state);
    expect("gfsk reads as modulation 2", model.modulation() == 2);

    opts.mod_gfsk = 0;
    opts.mod_c4fm = 1;
    model.refresh(&opts, &state);
    expect("c4fm reads as modulation 0 again", model.modulation() == 0);
    expect("enter DMR row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    expect("row selects GFSK", opts.mod_gfsk == 1);
    model.refresh(&opts, &state);
    expect("modulation control keeps configured C4FM", model.modulation() == 0);
    dsd_scan_mode_leave(&opts, &state);

    /* Issue #521: a row overriding the squelch. The panel pairs what is in force with the
     * configured default its buttons edit, and says a row is responsible. 0 dB means off. */
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    model.refresh(&opts, &state);
    expect("no scope: effective and configured agree", std::fabs(model.effectiveSquelchDb() - (-80.0)) < 1e-6
                                                           && std::fabs(model.configuredSquelchDb() - (-80.0)) < 1e-6);
    expect("no scope: no row badge", !model.squelchRowOverride());
    expect("no scope: the readout is the terminal's", model.squelchReadout() == QStringLiteral("-80.0 dB"));
    expect("row squelch scope", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    dsd_scan_option_values squelch_row{};
    squelch_row.present = DSD_SCAN_OPT_SQUELCH;
    squelch_row.squelch_db = -60;
    expect("row squelch installed", dsd_scan_mode_options(&opts, &state, &squelch_row) == 0);
    model.refresh(&opts, &state);
    expect("row squelch is in force", std::fabs(model.effectiveSquelchDb() - (-60.0)) < 1e-6);
    expect("the default stays configured", std::fabs(model.configuredSquelchDb() - (-80.0)) < 1e-6);
    expect("a row override is flagged", model.squelchRowOverride());
    expect("the effective squelch still drives squelchDb", std::fabs(model.squelchDb() - (-60.0)) < 1e-6);
    expect("the override readout is the terminal's",
           model.squelchReadout() == QStringLiteral("-60.0 dB (row; default -80.0 dB)"));
    expect("neither level is off", !model.effectiveSquelchOff() && !model.configuredSquelchOff());
    squelch_row.squelch_db = 0;
    expect("row squelch off installed", dsd_scan_mode_options(&opts, &state, &squelch_row) == 0);
    model.refresh(&opts, &state);
    expect("a row that switches the squelch off reads 0", std::fabs(model.effectiveSquelchDb()) < 1e-9);
    expect("an off row is still an override", model.squelchRowOverride() && model.squelchOff());
    expect("the view decides the row is off", model.effectiveSquelchOff() && !model.configuredSquelchOff());
    expect("the off readout is the terminal's",
           model.squelchReadout() == QStringLiteral("off (row; default -80.0 dB)"));
    /* A legacy linear default at full scale reads 0 dB like off, but gates everything: the
     * panel takes the view's decision, not the sign of the number. */
    dsd_scan_mode_leave(&opts, &state);
    opts.rtl_squelch_level = 1.0;
    expect("full-scale default scope", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    squelch_row.squelch_db = -60;
    expect("row over a full-scale default", dsd_scan_mode_options(&opts, &state, &squelch_row) == 0);
    model.refresh(&opts, &state);
    expect("a full-scale default reads 0 dB", std::fabs(model.configuredSquelchDb()) < 1e-9);
    expect("a full-scale default is not off", !model.configuredSquelchOff());
    expect("a full-scale default readout", model.squelchReadout() == QStringLiteral("-60.0 dB (row; default 0.0 dB)"));
    /* Audio input takes a squelch too (issue #628): the same readout, flagged for the monitor's Squelch row. */
    expect("a radio input is not audio input", !model.squelchAudioInput());
    opts.audio_in_type = AUDIO_IN_WAV;
    model.refresh(&opts, &state);
    expect("audio input is flagged", model.squelchAudioInput());
    expect("audio input publishes the squelch override", model.squelchRowOverride());
    expect("audio input publishes the readout",
           model.squelchReadout() == QStringLiteral("-60.0 dB (row; default 0.0 dB)"));
    /* A symbol file has no squelch to feed. */
    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    model.refresh(&opts, &state);
    expect("a symbol file is not audio input", !model.squelchAudioInput());
    expect("a symbol file publishes no squelch override", !model.squelchRowOverride());
    expect("a symbol file publishes no squelch readout", model.squelchReadout().isEmpty());
    opts.audio_in_type = AUDIO_IN_RTL;
    dsd_scan_mode_leave(&opts, &state);
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    model.refresh(&opts, &state);
    expect("leaving the row clears the badge", !model.squelchRowOverride());
    expect("leaving the row restores the default", std::fabs(model.effectiveSquelchDb() - (-80.0)) < 1e-6);
    opts.rtl_squelch_margin_db = 7;
    model.refresh(&opts, &state);
    expect("a level is not auto",
           !model.configuredSquelchAuto() && !model.effectiveSquelchAuto() && model.squelchAutoStatus().isEmpty());
    expect("a level keeps the margin Auto starts from again", model.configuredSquelchMarginDb() == 7);
    expect("the level beneath is a threshold", !model.configuredSquelchLevelOff());

    /* The auto squelch (issue #518 follow-up): whether each setting is AUTO, its margin and what the one in force
     * shows come from the same view, and the level beneath it stays what dB goes back to. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts.rtl_squelch_margin_db = 12;
    const int saved_analog_only = opts.analog_only;
    opts.analog_only = 0;
    model.refresh(&opts, &state);
    expect("an auto default is auto on both sides", model.configuredSquelchAuto() && model.effectiveSquelchAuto());
    expect("its margin on both sides",
           model.configuredSquelchMarginDb() == 12 && model.effectiveSquelchMarginDb() == 12);
    expect("the level beneath it stays readable", std::fabs(model.configuredSquelchDb() - (-80.0)) < 1e-6);
    expect("the level beneath it is not off", !model.configuredSquelchLevelOff());
    opts.rtl_squelch_level = 0.0;
    model.refresh(&opts, &state);
    expect("an off level beneath auto reads off there, not in the setting",
           model.configuredSquelchLevelOff() && !model.configuredSquelchOff());
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    expect("auto is off on a digital session", model.squelchAutoStatus() == QStringLiteral("off on digital"));
    expect("the readout is the terminal's", model.squelchReadout() == QStringLiteral("auto +12 dB (off on digital)"));
    opts.analog_only = 1;
    state.squelch_auto_active = 1;
    state.squelch_auto_plan_valid = 1;
    state.squelch_auto_state = 1;
    state.squelch_auto_floor_cdb = -7830;
    model.refresh(&opts, &state);
    expect("an analog session shows the floor", model.squelchAutoStatus() == QStringLiteral("floor -78.3 dB"));
    state.squelch_auto_floor_cdb = -7900;
    model.refresh(&opts, &state);
    expect("a moving floor reaches the panel", model.squelchAutoStatus() == QStringLiteral("floor -79.0 dB"));
    /* Issue #782: the gate opening and closing, which a screen reader hears at once. */
    expect("a closed auto gate reads closed", !model.squelchGateOpen());
    state.squelch_auto_gate_open = 1;
    model.refresh(&opts, &state);
    expect("an open auto gate reaches the panel", model.squelchGateOpen());
    state.squelch_auto_gate_open = 0;
    model.refresh(&opts, &state);
    expect("and closes again", !model.squelchGateOpen());
    /* The noise squelch: its flags, its quieting while it runs, and no Noise offered on the AM monitor on its own. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    state.squelch_noise_active = 1;
    state.squelch_noise_measured = 1;
    state.squelch_noise_quieting_cdb = 2310;
    model.refresh(&opts, &state);
    expect("a noise default is noise on both sides", model.configuredSquelchNoise() && model.effectiveSquelchNoise());
    expect("and not auto", !model.configuredSquelchAuto() && !model.effectiveSquelchAuto());
    expect("the noise squelch shows its quieting", model.squelchAutoStatus() == QStringLiteral("quieting 23 dB"));
    expect("the readout is the terminal's", model.squelchReadout() == QStringLiteral("noise +12 dB (quieting 23 dB)"));
    expect("the FM monitor offers Noise", model.squelchNoiseOffered());
    opts.analog_demod = DSD_ANALOG_DEMOD_AM;
    state.squelch_noise_active = 0;
    state.squelch_noise_measured = 0;
    model.refresh(&opts, &state);
    expect("the AM monitor offers no Noise", !model.squelchNoiseOffered());
    expect("a noise setting on AM runs as auto",
           model.squelchAutoStatus() == QStringLiteral("as auto: floor -79.0 dB"));
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    state.squelch_noise_quieting_cdb = 0;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts.audio_in_type = AUDIO_IN_WAV;
    state.squelch_auto_active = 0;
    model.refresh(&opts, &state);
    expect("auto on audio input reads off there",
           model.configuredSquelchAuto() && model.squelchAutoStatus() == QStringLiteral("off: no radio input"));
    /* The PCM noise squelch on audio input (issue #628): learning, then its quieting, or off with no band. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    const int saved_monitor = opts.monitor_input_audio;
    opts.monitor_input_audio = 1;
    state.squelch_noise_active = 1;
    state.squelch_noise_state = DSD_SQUELCH_NOISE_STATE_LEARNING;
    model.refresh(&opts, &state);
    expect("the PCM noise squelch learns", model.squelchAutoStatus() == QStringLiteral("learning"));
    expect("the readout is the terminal's", model.squelchReadout() == QStringLiteral("noise +12 dB (learning)"));
    expect("the PCM FM monitor offers Noise", model.squelchNoiseOffered());
    state.squelch_noise_state = DSD_SQUELCH_NOISE_STATE_KNOWN;
    state.squelch_noise_measured = 1;
    state.squelch_noise_quieting_cdb = 2310;
    model.refresh(&opts, &state);
    expect("then shows its quieting", model.squelchAutoStatus() == QStringLiteral("quieting 23 dB"));
    state.squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NO_BAND;
    model.refresh(&opts, &state);
    expect("or why it is off", model.squelchAutoStatus() == QStringLiteral("off: no band above voice"));
    state.squelch_noise_active = 0;
    state.squelch_noise_measured = 0;
    state.squelch_noise_quieting_cdb = 0;
    state.squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NONE;
    opts.monitor_input_audio = saved_monitor;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.squelch_auto_active = 0;
    state.squelch_auto_plan_valid = 0;
    state.squelch_auto_state = 0;
    state.squelch_auto_floor_cdb = 0;
    opts.analog_only = saved_analog_only;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts.rtl_squelch_margin_db = DSD_SQUELCH_MARGIN_DEFAULT_DB;
    model.refresh(&opts, &state);

    /* Every flag combination must mean the same thing inside a scope. */
    for (int flags = 0; flags < 8; flags++) {
        opts.mod_c4fm = (flags & 1) != 0;
        opts.mod_qpsk = (flags & 2) != 0;
        opts.mod_gfsk = (flags & 4) != 0;
        const int expected = dsd_opts_modulation(&opts);
        expect("enter modulation combination", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
        model.refresh(&opts, &state);
        expect("scoped modulation uses one-flag rule", model.modulation() == expected);
        dsd_scan_mode_leave(&opts, &state);
    }

    /* Tuner ownership: the gate is the OR, and the two named owners exist only to
     * word a message. All three have to agree. */
    opts.trunk_enable = 1;
    model.refresh(&opts, &state);
    expect("trunking owns the tuner", model.tunerControlled() && model.trunkingEnabled());
    expect("trunking is not the scanner", !model.scannerMode());
    opts.trunk_enable = 0;
    opts.scanner_mode = 1;
    model.refresh(&opts, &state);
    expect("the scanner owns the tuner too", model.tunerControlled() && model.scannerMode());
    expect("the scanner is not trunking", !model.trunkingEnabled());
    opts.scanner_mode = 0;

    /* Issue #575: the centre follows an I/Q replay's recorded RETUNEs, which leave rtlsdr_center_freq on the capture's
     * opening centre: the replay centre the decoder last read wins while there is one. */
    model.refresh(&opts, &state);
    expect("a radio reads its tuned centre", std::fabs(model.centerFreqHz() - 769768750.0) < 0.5);
    char live_dev[sizeof opts.audio_in_dev];
    DSD_SNPRINTF(live_dev, sizeof live_dev, "%s", opts.audio_in_dev);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "iqreplay:capture.iq.json");
    opts.iq_replay_center_freq = 769893750U;
    model.refresh(&opts, &state);
    expect("a replay reads the centre its samples were captured on",
           std::fabs(model.centerFreqHz() - 769893750.0) < 0.5);
    opts.iq_replay_center_freq = 0U;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", live_dev);
    model.refresh(&opts, &state);
    expect("with no replay centre the tuned centre reads again", std::fabs(model.centerFreqHz() - 769768750.0) < 0.5);

    /* Scan hold and avoids (#380) read whichever rotation is running. Plain trunking
     * follows one system and is not a rotation, so the controls have nothing to act on. */
    model.refresh(&opts, &state);
    expect("no rotation at rest", !model.scanRotationActive());
    expect("no hold at rest", !model.scanHold());
    expect("no avoids at rest", model.scanAvoidCount() == 0);
    opts.trunk_enable = 1;
    model.refresh(&opts, &state);
    expect("plain trunking is not a rotation", !model.scanRotationActive());
    opts.trunk_enable = 0;
    state.lcn_scan_hold = 1;
    /* Issue #672: the count is the avoid list's entries, from the store the view reads (the -Y rows' flags, the
       coordinator's published targets), never the saturating counters beside them. */
    seed_lcn_rows(&state, 4);
    for (size_t row = 0; row < 3U; ++row) {
        expect("an avoid flag is set", dsd_state_trunk_lcn_avoid_set(&state, row, 1) == 0);
    }
    state.lcn_avoid_count = 9;
    state.trunk_scan_hold = 0;
    publish_trunk_avoids(&state, 5U, 7U);
    state.trunk_scan_avoided_count = 9;
    state.trunk_scan_active_avoided = 1;
    opts.scanner_mode = 1;
    model.refresh(&opts, &state);
    expect("-Y is a rotation", model.scanRotationActive());
    expect("-Y hold reads the scan-list flag", model.scanHold());
    expect("-Y avoids are the avoided rows", model.scanAvoidCount() == 3);
    expect("-Y never parks on an avoided row", !model.scanTargetAvoided());
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 1;
    model.refresh(&opts, &state);
    expect("trunk scan is a rotation", model.scanRotationActive());
    expect("trunk scan hold reads the coordinator's flag", !model.scanHold());
    expect("trunk scan avoids are the coordinator's published targets", model.scanAvoidCount() == 7);
    expect("trunk scan reports the avoided fallback", model.scanTargetAvoided());
    // WP-S1: the held snapshot supplies target identity and lifecycle clearing.
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof state.trunk_scan_active_id, "%s", "dispatch");
    state.trunk_scan_active_ordinal = 2;
    state.trunk_scan_target_count = 3;
    state.trunk_scan_hold = 1;
    model.refresh(&opts, &state);
    expect("trunk scan hold on", model.scanHold());
    expect("scan target identity", model.scanTargetId() == QStringLiteral("dispatch") && model.scanTargetOrdinal() == 2
                                       && model.scanTargetCount() == 3);
    expect("an unlabelled target has no label", model.scanTargetLabel().isEmpty());
    // Issue #782: the target's label, the name its calls are given, beside its id.
    DSD_SNPRINTF(state.trunk_scan_active_label, sizeof state.trunk_scan_active_label, "%s", "County Dispatch");
    model.refresh(&opts, &state);
    expect("scan target label", model.scanTargetLabel() == QStringLiteral("County Dispatch")
                                    && model.scanTargetId() == QStringLiteral("dispatch"));
    /* Both flags set: --trunk-scan owns the tuner and the routing prefers it, so the
     * view reads the coordinator's fields, not the scan list's. */
    opts.scanner_mode = 1;
    state.trunk_scan_hold = 0;
    model.refresh(&opts, &state);
    expect("co-active rotations still count as one", model.scanRotationActive());
    expect("co-active hold reads the coordinator's flag", !model.scanHold());
    expect("co-active avoids are the coordinator's published targets", model.scanAvoidCount() == 7);
    expect("co-active avoided fallback comes from trunk scan", model.scanTargetAvoided());
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 0;
    state.lcn_scan_hold = 0;
    (void)dsd_state_trunk_lcn_avoid_clear(&state);
    state.lcn_avoid_count = 0;
    state.lcn_freq_count = 0;
    state.trunk_scan_hold = 0;
    publish_trunk_avoids(&state, 0U, 0U);
    state.trunk_scan_avoided_count = 0;
    state.trunk_scan_active_avoided = 0;

    /* An epoch the decoder opened on a frame that synced and went no further has
     * nothing in it. Rendered, it becomes a call from talkgroup 0 by nobody —
     * and, with a stale crypto header on the slot, an encrypted one. A session
     * that has not locked onto anything must not appear to be hearing traffic. */
    {
        dsd_call_observation empty = {};
        empty.protocol = DSD_SYNC_P25P1_POS;
        empty.slot = 0U;
        empty.kind = DSD_CALL_KIND_GROUP_VOICE;
        empty.observed_m = 1.0;
        expect("an identity-less epoch opens", dsd_call_state_observe(&state, &empty, DSD_CALL_BOUNDARY_BEGIN) == 1);
        state.payload_algid = 0x84; /* the stale header that made it read as ENC */
        model.refresh(&opts, &state);
        expect("an identity-less call is not presented as active", model.slot1CallState() != 2);
        expect("an identity-less call names no talkgroup", model.slot1TgText().isEmpty());
        expect("an identity-less call is not reported encrypted", !model.slot1CallEnc());

        /* The same epoch with a talkgroup on it is a real transmission. */
        dsd_call_observation named = empty;
        named.ota_target_id = 1201U;
        named.ota_source_id = 4242U;
        named.observed_m = 2.0;
        (void)dsd_call_state_observe(&state, &named, DSD_CALL_BOUNDARY_BEGIN);
        model.refresh(&opts, &state);
        expect("a call with a talkgroup is presented", model.slot1CallState() == 2);
        expect("a call with a talkgroup names it", model.slot1TgText() == QStringLiteral("1201"));

        /* leadSlot is what MonitorScreen.qml binds heroSlot to, and it is one-based so 0
         * falls out as "neither" — the rebasing of dsd_app_lead_slot()'s -1 happens here
         * and nowhere else. An off-by-one hides the hero panel or headlines the wrong
         * slot, which no C test of dsd_app_lead_slot() and no QML case can see. */
        expect("the only live call headlines", model.leadSlot() == 1);

        /* Slot 2 live as well: the earlier open epoch keeps the headline. */
        dsd_call_observation other = named;
        other.slot = 1U;
        other.ota_target_id = 1202U;
        other.observed_m = 2.5;
        (void)dsd_call_state_observe(&state, &other, DSD_CALL_BOUNDARY_BEGIN);
        model.refresh(&opts, &state);
        expect("slot 2 is live too", model.slot2CallState() == 2);
        expect("between two live calls the earlier slot 1 call headlines", model.leadSlot() == 1);

        /* With slot 1 ended and slot 2 still open, the open epoch wins outright — the
         * rule that made two surfaces name different units before it was shared. */
        (void)dsd_call_state_end(&state, 0U, 3.0);
        model.refresh(&opts, &state);
        expect("an open call outranks an ended one on a lower slot", model.leadSlot() == 2);

        (void)dsd_call_state_end(&state, 1U, 3.5);
    }

    /* Nothing on the air: one-based leadSlot answers 0 rather than naming slot 1. */
    model.clear();
    expect("a cleared model headlines no slot", model.leadSlot() == 0);

    /* A stopped session must not leave its answers behind for the next one. */
    state.synctype = DSD_SYNC_P25P1_POS;
    model.refresh(&opts, &state);
    expect("locked again before the stop", model.syncedHere());
    model.clear();
    expect("a cleared model reports no lock", !model.syncedHere());
    expect("a cleared model reports no tuner", !model.radioInput());
    expect("clear removes scan identity", model.scanTargetId().isEmpty() && model.scanTargetLabel().isEmpty()
                                              && model.scanTargetOrdinal() == 0 && model.scanTargetCount() == 0);

    /* The channel width rides the tuner group: it moves when the decoder changes
     * profile, not when the user changes a setting. It is also gated on a radio
     * input, like every other tuner reading. */
    {
        g_stub_channel_bandwidth_hz = 12500;
        opts.audio_in_type = AUDIO_IN_RTL;
        model.refresh(&opts, &state);
        expect("channel bandwidth reaches the model", model.channelBandwidthHz() == 12500);

        opts.audio_in_type = AUDIO_IN_PULSE;
        model.refresh(&opts, &state);
        expect("channel bandwidth is zero off a radio", model.channelBandwidthHz() == 0);

        g_stub_channel_bandwidth_hz = 0;
        opts.audio_in_type = AUDIO_IN_RTL;
    }

    /* The width must move on its own. Every other tuner reading is holding still
     * here, so if channel_bandwidth_hz were missing from tunerEquals() the frame
     * would compare equal, publish() would skip the whole View, and this second
     * reading would still be the first one. */
    {
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_channel_bandwidth_hz = 12500;
        model.refresh(&opts, &state);
        expect("width reads the first frame", model.channelBandwidthHz() == 12500);

        g_stub_channel_bandwidth_hz = 6250;
        model.refresh(&opts, &state);
        expect("width alone moves the tuner group", model.channelBandwidthHz() == 6250);

        g_stub_channel_bandwidth_hz = 0;
    }

    /* Issue #525: the analog channel width the Radio sheet shows and steps from. While a running stream runs the
     * analog monitor it is the width the front end reports (with its DSP-limited flag); otherwise the configured
     * width, the default when none is set. The configured value (0 = default) is published on its own for the stepper,
     * and with a stream running the widest width its DSP rate filters. The front end's width is kept apart from the
     * configured one throughout, so each case shows which of the two the model took. */
    {
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_stream_active = 1;
        g_stub_demod_rate_hz = 24000;
        model.refresh(&opts, &state);
        /* Issue #524: the rate still bounds the widths the controls offer outside the preset: a width set for the next
         * switch is held to it there. */
        expect("digital: no analog width", model.analogBandwidthHz() == 0 && !model.analogBandwidthDspLimited()
                                               && model.analogBandwidthMaxHz() == 20400);
        expect("digital: neither width offered by default",
               !model.nfmBandwidthOffered() && !model.amBandwidthOffered());
        opts.analog_only = 1;
        opts.analog_demod = DSD_ANALOG_DEMOD_FM;
        model.refresh(&opts, &state);
        expect("analog before a published width reads the default", model.analogBandwidthHz() == 16000);
        expect("the default is configured as 0", model.analogBandwidthConfiguredHz() == 0);
        expect("the widest width the 24 kHz rate filters", model.analogBandwidthMaxHz() == 20400);
        expect("analog: the NFM width offered, the AM default fits the rate",
               model.nfmBandwidthOffered() && !model.amBandwidthOffered());
        expect("the reading says it is the default",
               model.analogBandwidthReading() == QStringLiteral("16 kHz (default)"));

        opts.analog_nfm_bandwidth_hz = 12500;
        g_stub_channel_bandwidth_hz = 10800;
        model.refresh(&opts, &state);
        expect("on the monitor, the front end's width", model.analogBandwidthHz() == 10800);
        expect("not DSP-limited", !model.analogBandwidthDspLimited());
        expect("the configured width", model.analogBandwidthConfiguredHz() == 12500);
        expect("the reading is the front end's width", model.analogBandwidthReading() == QStringLiteral("10.8 kHz"));

        /* The flag must move on its own: everything else holds still here. */
        g_stub_channel_bandwidth_dsp_limited = 1;
        model.refresh(&opts, &state);
        expect("DSP-limited alone moves the tuner group", model.analogBandwidthDspLimited());
        expect("the reading says DSP-limited",
               model.analogBandwidthReading() == QStringLiteral("10.8 kHz (DSP-limited)"));
        g_stub_channel_bandwidth_dsp_limited = 0;

        /* The front end's mirror outlives its stream: once the stream stops, what it still says is the last
         * session's width, not this one's. */
        g_stub_stream_active = 0;
        g_stub_channel_bandwidth_dsp_limited = 1;
        model.refresh(&opts, &state);
        expect("no stream: the configured width", model.analogBandwidthHz() == 12500);
        expect("no stream: not DSP-limited", !model.analogBandwidthDspLimited());
        /* The input is an RTL one ("pulse" on an RTL input opens as an RTL-SDR): the next start runs at its 48 kHz DSP
         * bandwidth, which bounds the steps. */
        expect("no stream: the DSP bandwidth bounds the steps", model.analogBandwidthMaxHz() == 42000);
        /* At a 12 kHz DSP bandwidth the default runs no channel filter: the sheet reads what the next start publishes
         * and offers only the widths that rate filters. */
        opts.analog_nfm_bandwidth_hz = 0;
        opts.rtl_dsp_bw_khz = 12;
        model.refresh(&opts, &state);
        expect("no stream at 12 kHz: the default reads as the rate",
               model.analogBandwidthHz() == 12000 && model.analogBandwidthDspLimited());
        expect("no stream at 12 kHz: the steps it filters", model.analogBandwidthMaxHz() == 9600);
        expect("no stream at 12 kHz: the reading says DSP-limited",
               model.analogBandwidthReading() == QStringLiteral("12 kHz (DSP-limited)"));
        opts.rtl_dsp_bw_khz = 48;
        opts.analog_nfm_bandwidth_hz = 12500;
        g_stub_stream_active = 1;
        g_stub_channel_bandwidth_dsp_limited = 0;

        /* A typed digital row on the analog session filters with its own profile: the front end's width is the row's
         * channel, so the sheet shows the configured analog width the row's leave returns to. */
        expect("typed row on analog", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
        expect("typed row options", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
        model.refresh(&opts, &state);
        expect("under a typed row the configured width", model.analogBandwidthHz() == 12500);
        expect("under a typed row the configured preset still shows", model.analogBandwidthConfiguredHz() == 12500);
        dsd_scan_mode_leave(&opts, &state);

        /* Issue #526: an nfm row with its own width runs over the configured one. The reading is the row's width with
         * the configured width it returns to, and the stepper still edits the configured width, not the row's. */
        dsd_scan_option_values width_row{};
        width_row.present = DSD_SCAN_OPT_BANDWIDTH;
        width_row.channel_bw_hz = 16000;
        expect("nfm width row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) == 0);
        expect("nfm width row options", dsd_scan_mode_options(&opts, &state, &width_row) == 0);
        expect("nfm width row in force", opts.analog_nfm_bandwidth_hz == 16000);
        g_stub_channel_bandwidth_hz = 16000;
        model.refresh(&opts, &state);
        expect("under a width row the row's width", model.analogBandwidthHz() == 16000);
        expect("under a width row the stepper edits the configured width",
               model.analogBandwidthConfiguredHz() == 12500);
        expect("under a width row the reading names both",
               model.analogBandwidthReading() == QStringLiteral("16 kHz (row; default 12.5 kHz)"));
        expect("under a width row the row is on air", model.analogBandwidthRowActive());
        expect("under a width row the override is flagged", model.analogBandwidthRowOverride());
        dsd_scan_mode_leave(&opts, &state);
        expect("the width row's leave restores the configured width", opts.analog_nfm_bandwidth_hz == 12500);

        /* ...and on a digital session, where the nfm row is the only analog receiver: the width it runs is in force and
         * read as the terminal reads it, flagged for the sheet as a row on air, and badged while the row sets its own
         * width; nothing once it leaves. */
        opts.analog_only = 0;
        opts.frame_dmr = 1;
        model.refresh(&opts, &state);
        expect("digital session: no row, no width", !model.analogBandwidthRowActive() && model.analogBandwidthHz() == 0
                                                        && model.analogBandwidthReading().isEmpty());
        expect("digital nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) == 0);
        expect("digital nfm row without a width", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
        g_stub_channel_bandwidth_hz = 12500;
        model.refresh(&opts, &state);
        expect("digital nfm row: on air, no override",
               model.analogBandwidthRowActive() && !model.analogBandwidthRowOverride());
        expect("digital nfm row: the configured width in force", model.analogBandwidthHz() == 12500);
        expect("digital nfm row: read as the terminal reads it",
               model.analogBandwidthReading() == QStringLiteral("12.5 kHz"));
        expect("digital nfm row: the stepper edits the configured width", model.analogBandwidthConfiguredHz() == 12500);
        expect("digital nfm row with a width", dsd_scan_mode_options(&opts, &state, &width_row) == 0);
        g_stub_channel_bandwidth_hz = 16000;
        model.refresh(&opts, &state);
        expect("digital width row: the override is flagged", model.analogBandwidthRowOverride());
        expect("digital width row: the row's width", model.analogBandwidthHz() == 16000);
        expect("digital width row: the reading names both",
               model.analogBandwidthReading() == QStringLiteral("16 kHz (row; default 12.5 kHz)"));
        expect("digital width row: the stepper edits the configured width",
               model.analogBandwidthConfiguredHz() == 12500);
        dsd_scan_mode_leave(&opts, &state);
        model.refresh(&opts, &state);
        expect("digital session after the row: no row, no width",
               !model.analogBandwidthRowActive() && !model.analogBandwidthRowOverride()
                   && model.analogBandwidthHz() == 0 && model.analogBandwidthReading().isEmpty());
        opts.frame_dmr = 0;
        opts.analog_only = 1;
        g_stub_channel_bandwidth_hz = 10800;

        /* PCM input: no channel filter runs, so no width is in force (the terminal shows no Analog field there
         * either); the configured width is still published for the control. */
        opts.audio_in_type = AUDIO_IN_PULSE;
        opts.analog_nfm_bandwidth_hz = 20000;
        model.refresh(&opts, &state);
        expect("PCM input has no width in force", model.analogBandwidthHz() == 0 && !model.analogBandwidthDspLimited()
                                                      && model.analogBandwidthMaxHz() == 0);
        expect("PCM input says the width does not apply",
               model.analogBandwidthReading() == QStringLiteral("not used on PCM input"));
        expect("the configured width moves the control group", model.analogBandwidthConfiguredHz() == 20000);

        opts.analog_only = 0;
        opts.analog_nfm_bandwidth_hz = 0;
        g_stub_channel_bandwidth_hz = 0;
        g_stub_stream_active = 0;
        g_stub_demod_rate_hz = 0;
        opts.audio_in_type = AUDIO_IN_RTL;
    }

    /* Issue #524: under the AM preset the view reads the AM width: the configured one, 6 kHz by default and never
     * DSP-limited (a rate that cannot filter it is refused), and the running monitor's own width once the front end
     * publishes an AM one. An FM width still published (a switch to AM not landed yet) is another channel's. The
     * configured AM width is what the stepper edits, and stays published off a radio. */
    {
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_stream_active = 1;
        g_stub_demod_rate_hz = 16000;
        opts.analog_only = 1;
        opts.monitor_input_audio = 1;
        opts.analog_demod = DSD_ANALOG_DEMOD_AM;
        model.refresh(&opts, &state);
        expect("am: the default width", model.analogBandwidthHz() == 6000 && !model.analogBandwidthDspLimited());
        expect("am: the default is configured as 0", model.analogBandwidthConfiguredHz() == 0);
        expect("am: the 16 kHz rate bounds the steps at 13.2 kHz", model.analogBandwidthMaxHz() == 13200);
        expect("am: the reading says it is the default",
               model.analogBandwidthReading() == QStringLiteral("6 kHz (default)"));

        opts.analog_am_bandwidth_hz = 10000;
        g_stub_channel_bandwidth_hz = 16000;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_FM;
        model.refresh(&opts, &state);
        expect("am: an FM width is not the AM channel", model.analogBandwidthHz() == 10000);
        expect("am: the configured AM width", model.analogBandwidthConfiguredHz() == 10000);
        g_stub_channel_bandwidth_hz = 9000;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_AM;
        model.refresh(&opts, &state);
        expect("am: the AM monitor's width", model.analogBandwidthHz() == 9000);
        expect("am: the reading is the AM monitor's width", model.analogBandwidthReading() == QStringLiteral("9 kHz"));
        expect("am: the width is AM's", model.analogBandwidthAm());

        /* Issue #526: an nfm row with its own width over the AM session runs FM. The readings describe the row's NFM
         * width, the configured widths are the configured view's rather than the row's in dsd_opts, and the AM
         * preset's own width stays offered; the leave brings AM back. */
        opts.analog_nfm_bandwidth_hz = 11250;
        dsd_scan_option_values am_session_row{};
        am_session_row.present = DSD_SCAN_OPT_BANDWIDTH;
        am_session_row.channel_bw_hz = 12500;
        expect("am: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) == 0);
        expect("am: nfm row options", dsd_scan_mode_options(&opts, &state, &am_session_row) == 0);
        g_stub_channel_bandwidth_hz = 12500;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_FM;
        model.refresh(&opts, &state);
        expect("am nfm row: the row's NFM width", !model.analogBandwidthAm() && model.analogBandwidthHz() == 12500);
        expect("am nfm row: the configured NFM width",
               model.nfmBandwidthConfiguredHz() == 11250 && model.analogBandwidthConfiguredHz() == 11250);
        expect("am nfm row: the configured AM width", model.amBandwidthConfiguredHz() == 10000);
        expect("am nfm row: both widths offered", model.amBandwidthOffered() && model.nfmBandwidthOffered());
        dsd_scan_mode_leave(&opts, &state);
        opts.analog_nfm_bandwidth_hz = 0;
        g_stub_channel_bandwidth_hz = 9000;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_AM;
        model.refresh(&opts, &state);
        expect("am after the nfm row: AM again", model.analogBandwidthAm() && model.analogBandwidthHz() == 9000);

        opts.audio_in_type = AUDIO_IN_PULSE;
        model.refresh(&opts, &state);
        expect("am: no width in force off a radio",
               model.analogBandwidthHz() == 0 && model.analogBandwidthMaxHz() == 0);
        expect("am: the configured AM width stays off a radio", model.analogBandwidthConfiguredHz() == 10000);

        /* Each kind's configured width is published whichever preset runs, for the control of the kind the preset
         * does not run: an explicit NFM width under AM, an explicit AM width under a digital mode. */
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.analog_nfm_bandwidth_hz = 25000;
        model.refresh(&opts, &state);
        expect("am: the configured NFM width under AM", model.nfmBandwidthConfiguredHz() == 25000);
        expect("am: the configured AM width under AM", model.amBandwidthConfiguredHz() == 10000);
        opts.analog_only = 0;
        opts.monitor_input_audio = 0;
        opts.analog_demod = DSD_ANALOG_DEMOD_FM;
        model.refresh(&opts, &state);
        expect("digital: the configured AM width", model.amBandwidthConfiguredHz() == 10000);
        expect("digital: the configured NFM width", model.nfmBandwidthConfiguredHz() == 25000);
        expect("digital: the section's width is the NFM one", model.analogBandwidthConfiguredHz() == 25000);
        expect("digital: both explicit widths offered", model.nfmBandwidthOffered() && model.amBandwidthOffered());
        opts.analog_nfm_bandwidth_hz = 0;

        /* The unset AM default is offered where the running stream's rate cannot filter it but filters a narrower AM
         * width (a replay or device at 7.5 kHz filters up to 5.55 kHz): a switch to AM is refused there with word to
         * narrow the width, which has to be possible before it. */
        opts.analog_am_bandwidth_hz = 0;
        g_stub_demod_rate_hz = 7500;
        model.refresh(&opts, &state);
        expect("digital at 7.5 kHz: the AM default is offered", model.amBandwidthOffered());
        expect("digital at 7.5 kHz: the rate bounds the steps", model.analogBandwidthMaxHz() == 5550);
        expect("digital at 7.5 kHz: the NFM default is not", !model.nfmBandwidthOffered());
        opts.audio_in_type = AUDIO_IN_PULSE;
        model.refresh(&opts, &state);
        expect("pcm: nothing offered", !model.amBandwidthOffered() && model.analogBandwidthMaxHz() == 0);
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_demod_rate_hz = 16000;

        opts.analog_only = 0;
        opts.monitor_input_audio = 0;
        opts.analog_demod = DSD_ANALOG_DEMOD_FM;
        opts.analog_am_bandwidth_hz = 0;
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_channel_bandwidth_hz = 0;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_FM;
        g_stub_stream_active = 0;
        g_stub_demod_rate_hz = 0;
        model.refresh(&opts, &state);
    }

    /* A call with no name of its own on a named scan channel: the hero must show
     * the channel, the way the call history already does, and expose it on its own
     * so the panel can also show it beside a call that has a name. */
    {
        dsd_call_observation unnamed = {};
        unnamed.protocol = DSD_SYNC_NXDN_POS;
        unnamed.slot = 0U;
        unnamed.kind = DSD_CALL_KIND_GROUP_VOICE;
        unnamed.ota_target_id = 0U;
        unnamed.ota_source_id = 1U;
        unnamed.observed_m = 50.0;
        expect("a tg-0 call opens", dsd_call_state_observe(&state, &unnamed, DSD_CALL_BOUNDARY_BEGIN) == 1);
        expect("the state carries an event ring", state.event_history_s != nullptr);
        if (state.event_history_s != nullptr) {
            Event_History* staged = &state.event_history_s[0].Event_History_Items[0];
            DSD_SNPRINTF(staged->channel_label, sizeof(staged->channel_label), "%s", "Fire Dispatch");
            model.refresh(&opts, &state);
            expect("the slot exposes its scan channel", model.slot1Channel() == QStringLiteral("Fire Dispatch"));
            expect("a nameless call is named by its channel", model.slot1CallName() == QStringLiteral("Fire Dispatch"));
            /* No talkgroup decoded: no talkgroup text, never "0" (issue #773). */
            expect("the talkgroup text stays empty", model.slot1TgText().isEmpty());

            /* Issue #671: a trunk-scan target's id names the channel; the hero reads the entry's name instead. */
            DSD_SNPRINTF(staged->channel_label, sizeof(staged->channel_label), "%s", "96e4230d-7220-4a76");
            model.setChannelNames(QVariantMap{{QStringLiteral("96e4230d-7220-4a76"), QStringLiteral("Benton County")}});
            model.refresh(&opts, &state);
            expect("a target id reads as its name in the hero",
                   model.slot1CallName() == QStringLiteral("Benton County")
                       && model.slot1Channel() == QStringLiteral("Benton County"));
            model.setChannelNames(QVariantMap());
            model.refresh(&opts, &state);
            expect("without names the id shows as it is",
                   model.slot1CallName() == QStringLiteral("96e4230d-7220-4a76"));
        }
    }

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
