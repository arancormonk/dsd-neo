// SPDX-License-Identifier: GPL-3.0-or-later
// Real generator -> parser -> coordinator -> policy/key installation. Only the
// tuning side effect is replaced; no policy, profile or key stubs are linked.
#include <QByteArray>
#include <QChar>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <initializer_list>
#include <stdint.h>
#include <utility>
#include "scan_list_targets.h"
#include "session_args.h"
extern "C" {
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
}
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static int failures;
static int tunes;

static void
expect(bool ok, int line) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "generated scan-list integration assertion failed at line %d\n", line);
    }
}

#define check(ok) expect((ok), __LINE__)

static dsd_trunk_tune_result
tune(dsd_opts*, dsd_state*, long int, int, uint64_t) {
    ++tunes;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
policy(const dsd_state* state, const char* name, const char* mode) {
    dsd_tg_policy_lookup found{};
    check(dsd_tg_policy_lookup_id(state, 123, &found) == 0 && !std::strcmp(found.entry.name, name)
          && !std::strcmp(found.entry.mode, mode));
}

// Compare actual standalone argv parsing with generated scoped options, including
// initialized mute defaults and restoration across two complete rotations.
static void
keyMuteRotation(bool unmuteP25) {
    QTemporaryDir dir;
    QVariantList systems, entries;
    for (const auto& type : {"basic", "hex", "rc4", "scrambler"}) {
        for (int nonzero = 0; nonzero < 2; ++nonzero) {
            const QString uid = QString::number(systems.size());
            const QString value = QString(type) == "hex" ? QString(10, nonzero ? '1' : '0') : QString::number(nonzero);
            systems << QVariantMap{{"uid", uid},
                                   {"trunking", true},
                                   {"decodeFlag", QString(type) == "scrambler" ? "-fi" : "-fs"},
                                   {"freqMhz", QString::number(461 + systems.size())},
                                   {"encKeyType", type},
                                   {"encKeyValue", value}};
            entries << QVariantMap{{"uid", uid}, {"kind", "system"}, {"systemUid", uid}};
        }
    }
    systems << QVariantMap{{"uid", "plain"}, {"trunking", true}, {"decodeFlag", "-fs"}, {"freqMhz", "480"}};
    entries << QVariantMap{{"uid", "plain"}, {"kind", "system"}, {"systemUid", "plain"}};
    auto generated = dsd_qt::scan_list_targets({{"sourceType", "usb"}, {"entries", entries}}, systems);
    check(generated.ok && generated.targetCount == systems.size());
    const QString path = dir.path() + "/mute-targets.csv";
    QFile file(path);
    check(file.open(QIODevice::WriteOnly));
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    check(file.write(generated.csv) == generated.csv.size());
    file.close();
    auto* opts = new dsd_opts{};
    auto* state = new dsd_state{};
    initOpts(opts);
    initState(state);
    opts->unmute_encrypted_p25 = unmuteP25;
    opts->trunk_scan_enabled = 1;
    opts->use_rigctl = 1;
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", path.toUtf8().constData());
    dsd_trunk_tuning_hooks hooks{};
    hooks.tune_to_freq_request = tune;
    hooks.tune_to_cc_request = tune;
    dsd_trunk_tuning_hooks_set(hooks);
    char error[256]{};
    const bool initialized = dsd_engine_trunk_scan_init(opts, state, error, sizeof error) == 0;
    check(initialized);
    if (initialized) {
        for (int i = 0; i < systems.size() * 2; ++i) {
            auto* standaloneOpts = new dsd_opts{};
            auto* standaloneState = new dsd_state{};
            initOpts(standaloneOpts);
            initState(standaloneState);
            standaloneOpts->unmute_encrypted_p25 = unmuteP25;
            auto system = systems[i % systems.size()].toMap();
            system["sourceType"] = "usb";
            auto args = dsd_qt::session_args_build(system, {}, nullptr);
            QList<QByteArray> storage{QByteArray("scan-mute-test")};
            for (const auto& arg : args) {
                storage << arg.toUtf8();
            }
            QList<char*> argv;
            for (auto& arg : storage) {
                argv << arg.data();
            }
            int effective = 0, exitCode = 0;
            check(dsd_parse_args(static_cast<int>(argv.size()), argv.data(), standaloneOpts, standaloneState,
                                 &effective, &exitCode)
                  == 0);
            check(opts->dmr_mute_encL == standaloneOpts->dmr_mute_encL);
            check(opts->dmr_mute_encR == standaloneOpts->dmr_mute_encR);
            check(opts->unmute_encrypted_p25 == standaloneOpts->unmute_encrypted_p25);
            for (auto& arg : storage) {
                DSD_SECURE_ZERO(arg.data(), static_cast<size_t>(arg.size()));
            }
            freeState(standaloneState);
            DSD_SECURE_ZERO(standaloneState, sizeof *standaloneState);
            delete standaloneState;
            delete standaloneOpts;
            check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        }
    }
    dsd_engine_trunk_scan_shutdown(opts, state);
    dsd_trunk_tuning_hooks_set({});
    dsd_trunk_scan_hooks_set({});
    freeState(state);
    DSD_SECURE_ZERO(state, sizeof *state);
    delete state;
    delete opts;
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    QVariantList systems, entries;
    for (int i = 0; i < 3; ++i) {
        QString uid = QString::number(i);
        QVariantMap system{{"uid", uid},
                           {"name", "Target " + uid},
                           {"decodeFlag", "-fs"},
                           {"trunking", true},
                           {"freqMhz", QString::number(461 + i)}};
        if (i < 2) {
            QString path = dir.path() + "/group " + uid + ".csv";
            QFile file(path);
            check(file.open(QIODevice::WriteOnly));
            file.write(i == 0 ? "id,mode,name\n123,B,First\n" : "id,mode,name\n123,A,Second\n");
            file.close();
            system["groupCsvPath"] = path;
            system["encKeyType"] = i == 0 ? "rc4" : "basic";
            system["encKeyValue"] = i == 0 ? "0123456789" : "7";
            system["encForceKey"] = i == 0 ? 2 : 1;
        }
        systems << system;
        entries << QVariantMap{{"uid", uid}, {"kind", "system"}, {"systemUid", uid}, {"enabled", true}};
    }
    auto generated = dsd_qt::scan_list_targets({{"sourceType", "usb"}, {"entries", entries}}, systems);
    check(generated.ok && generated.targetCount == 3);
    QString path = dir.path() + "/targets.csv";
    QFile file(path);
    check(file.open(QIODevice::WriteOnly));
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    check(file.write(generated.csv) == generated.csv.size());
    file.close();
    auto* opts = static_cast<dsd_opts*>(std::calloc(1, sizeof(dsd_opts)));
    auto* state = static_cast<dsd_state*>(std::calloc(1, sizeof(dsd_state)));
    if (!opts || !state) {
        std::free(opts);
        std::free(state);
        return 1;
    }
    opts->trunk_scan_enabled = 1;
    opts->trunk_scan_idle_dwell_ms = 250;
    opts->trunk_scan_activity_hold_ms = 250;
    opts->use_rigctl = 1;
    opts->rtl_dsp_bw_khz = 48;
    state->R = 99;
    state->M = 1;
    dsd_tg_policy_entry baseline{};
    check(dsd_tg_policy_make_exact_entry(123, "A", "Global", DSD_TG_POLICY_SOURCE_IMPORTED, &baseline) == 0);
    check(dsd_tg_policy_append_exact(state, &baseline) == 0);
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", path.toUtf8().constData());
    dsd_trunk_tuning_hooks hooks{};
    hooks.tune_to_freq_request = tune;
    hooks.tune_to_cc_request = tune;
    dsd_trunk_tuning_hooks_set(hooks);
    char error[256]{};
    const bool initialized = dsd_engine_trunk_scan_init(opts, state, error, sizeof error) == 0;
    check(initialized);
    if (!initialized) {
        std::fprintf(stderr, "Initialization: %s\n", error);
    }
    if (initialized) {
        check(dsd_engine_trunk_scan_target_count(state) == 3 && tunes == 1);
        policy(state, "First", "B");
        check(state->R == 0x123456789ULL && state->RR == state->R && state->M == 0x21);
        check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        policy(state, "Second", "A");
        check(state->R == 0 && state->K == 7 && state->M == 1);
        check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        policy(state, "Global", "A");
        check(state->R == 99 && state->K == 0 && state->M == 1);
        check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        policy(state, "First", "B");
        check(state->R == 0x123456789ULL && state->K == 0 && tunes == 4);
    }
    dsd_engine_trunk_scan_shutdown(opts, state);
    policy(state, "Global", "A");
    check(state->R == 99 && state->M == 1);
    dsd_trunk_tuning_hooks_set({});
    dsd_trunk_scan_hooks_set({});
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    DSD_SECURE_ZERO(state, sizeof *state);
    std::free(state);
    std::free(opts);
    keyMuteRotation(false);
    keyMuteRotation(true);
    return failures ? 1 : 0;
}
