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
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
}
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/platform/platform.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <QStringList>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
extern "C" {
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/scan_mode.h>
}
#endif
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
    dsd_trunk_scan_hooks_set(nullptr);
    freeState(state);
    DSD_SECURE_ZERO(state, sizeof *state);
    delete state;
    delete opts;
}

static void
p25CandidateRotation(int configured) {
    QTemporaryDir dir;
    QVariantList systems, entries;
    for (const auto& flag : {"-mq -^", "-ft -^", "-ft"}) {
        const QString uid = QString::number(systems.size());
        systems << QVariantMap{
            {"uid", uid}, {"decodeFlag", flag}, {"trunking", true}, {"freqMhz", QString::number(850 + systems.size())}};
        entries << QVariantMap{{"uid", uid}, {"kind", "system"}, {"systemUid", uid}};
    }
    const auto generated = dsd_qt::scan_list_targets({{"sourceType", "usb"}, {"entries", entries}}, systems);
    check(generated.ok && generated.targetCount == 3);
    QFile file(dir.filePath("p25.csv"));
    check(file.open(QIODevice::WriteOnly));
    check(file.write(generated.csv) == generated.csv.size());
    file.close();
    auto* opts = new dsd_opts{};
    auto* state = new dsd_state{};
    initOpts(opts);
    initState(state);
    opts->p25_prefer_candidates = static_cast<uint8_t>(configured);
    opts->trunk_scan_enabled = 1;
    opts->use_rigctl = 1;
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s",
                 file.fileName().toUtf8().constData());
    dsd_trunk_tuning_hooks hooks{};
    hooks.tune_to_freq_request = tune;
    hooks.tune_to_cc_request = tune;
    dsd_trunk_tuning_hooks_set(hooks);
    char error[256]{};
    const bool initialized = dsd_engine_trunk_scan_init(opts, state, error, sizeof error) == 0;
    check(initialized);
    if (initialized) {
        for (int i = 0; i < 6; ++i) {
            check(opts->p25_prefer_candidates == (i % 3 == 2 ? configured : 1));
            dsdneoUserConfig config{};
            dsd_snapshot_opts_to_user_config(opts, state, &config);
            check(config.trunk_p25_prefer_candidates == configured);
            if (i % 3 == 0) {
                check(opts->mod_qpsk == 1 && state->rf_mod == 1);
            }
            check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        }
    }
    dsd_engine_trunk_scan_shutdown(opts, state);
    check(opts->p25_prefer_candidates == configured);
    dsd_trunk_tuning_hooks_set({});
    dsd_trunk_scan_hooks_set(nullptr);
    freeState(state);
    delete state;
    delete opts;
}

#if !DSD_PLATFORM_WIN_NATIVE
namespace {

// A rigctl peer on a loopback socket (issue #526), modelled on SDR++'s rigctl server: it knows the modes FM (narrow
// FM) and AM and refuses the NFM token, as SDR++ and GQRX do; it keeps one passband per mode, which an "M" with a
// passband of 0 leaves unchanged and a selected mode brings back, as SDR++ does (it also saves them across restarts);
// and it answers "m" with the mode it runs and that mode's passband. It records each command line, and refuses "M AM"
// while refuseAm is set, as an FM-only rig does. stop() ends the worker whether or not a client ever connected.
class FakeRigctlPeer {
  public:
    std::atomic<bool> refuseAm{false};

    bool
    start() {
        listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
        if (listener == DSD_INVALID_SOCKET) {
            return false;
        }
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof addr;
        if (dsd_socket_bind(listener, reinterpret_cast<const struct sockaddr*>(&addr), sizeof addr) != 0
            || dsd_socket_listen(listener, 1) != 0
            || getsockname(listener, reinterpret_cast<struct sockaddr*>(&addr), &length) != 0) {
            (void)dsd_socket_close(listener);
            listener = DSD_INVALID_SOCKET;
            return false;
        }
        port = ntohs(addr.sin_port);
        worker = std::thread([this] { serve(); });
        return true;
    }

    void
    stop() {
        stopping = true;
        if (worker.joinable()) {
            worker.join();
        }
    }

    QStringList
    commands() {
        const std::lock_guard<std::mutex> lock(mutex);
        return seen;
    }

    // The mode the peer runs, and the passband it keeps for @p mode.
    QString
    mode() {
        const std::lock_guard<std::mutex> lock(mutex);
        return current;
    }

    int
    passband(const QString& forMode) {
        const std::lock_guard<std::mutex> lock(mutex);
        return passbands.value(forMode);
    }

    int port = 0;

  private:
    // Wait for the one client, looking at stopping every 100 ms; false when stop() came first.
    bool
    waitForClient() {
        struct pollfd ready{};
        ready.fd = listener;
        ready.events = POLLIN;
        while (!stopping) {
            if (poll(&ready, 1, 100) > 0) {
                return true;
            }
        }
        return false;
    }

    QByteArray
    answer(const QString& line) {
        const QStringList parts = line.split(QLatin1Char(' '));
        const std::lock_guard<std::mutex> lock(mutex);
        seen << line;
        if (parts.value(0) == QStringLiteral("m")) {
            return QStringLiteral("%1\n%2\n").arg(current).arg(passbands.value(current)).toUtf8();
        }
        if (parts.value(0) != QStringLiteral("M")) {
            return "RPRT 0\n";
        }
        const QString requested = parts.value(1);
        if (parts.size() != 3 || !passbands.contains(requested)) {
            return "RPRT 1\n";
        }
        if (refuseAm && requested == QStringLiteral("AM")) {
            return "RPRT -11\n";
        }
        current = requested;
        const int hz = parts.value(2).toInt();
        if (hz > 0) {
            passbands[requested] = hz;
        }
        return "RPRT 0\n";
    }

    void
    serve() {
        const dsd_socket_t client =
            waitForClient() ? dsd_socket_accept(listener, nullptr, nullptr) : DSD_INVALID_SOCKET;
        (void)dsd_socket_close(listener);
        if (client == DSD_INVALID_SOCKET) {
            return;
        }
        (void)dsd_socket_set_recv_timeout(client, 100U);
        QByteArray pending;
        char chunk[256];
        while (!stopping) {
            const int n = dsd_socket_recv(client, chunk, sizeof chunk, 0);
            if (n == 0) {
                break;
            }
            if (n < 0) {
                const int error = dsd_socket_get_error();
                if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
                    continue; // the receive timeout: look at stopping again
                }
                break;
            }
            pending.append(chunk, n);
            qsizetype end = 0;
            while ((end = pending.indexOf('\n')) >= 0) {
                const QByteArray reply = answer(QString::fromUtf8(pending.left(end)));
                pending.remove(0, end + 1);
                (void)dsd_socket_send(client, reply.constData(), static_cast<size_t>(reply.size()), 0);
            }
        }
        (void)dsd_socket_close(client);
    }

    dsd_socket_t listener = DSD_INVALID_SOCKET;
    std::atomic<bool> stopping{false};
    std::thread worker;
    std::mutex mutex;
    QStringList seen;
    QString current = QStringLiteral("FM");
    QMap<QString, int> passbands{{QStringLiteral("FM"), 11000}, {QStringLiteral("AM"), 9000}};
};

} // namespace

// Issue #526: a generated list mixing a keyed DMR system, a saved AM system (-fM) that still carries keys and a
// talkgroup file, and nfm and am frequency entries, plus a target with its own NFM passband and a DMR conventional
// target, through the real parser and coordinator with a rigctl peer that demodulates audio input. The analog targets
// install no keys and no talkgroup policy (the baseline stays in force on them), run their own demodulator, and ask
// the peer for it: "M AM 6000" for an am target, the target's own --nfm-bandwidth-hz for the nfm one that sets it
// (read through the scan scope's options, dsd_engine_scan_tuning_row_options()). The peer, like SDR++, keeps each
// passband it is sent and takes a passband of 0 as "unchanged", so the client reads its own passbands first ("m") and
// undoes a row's with them: the nfm target without a width and the DMR target after the one with its own run the peer's
// own 11 kHz again. A peer that refuses AM fails that target's tune, and the advance moves on past it. Shutdown puts
// the baseline back and returns the peer to FM at its own passband, with its own AM passband put back too.
static void
mixedAnalogRotation() {
    QTemporaryDir dir;
    const QString groups = dir.path() + "/groups.csv";
    QFile groupFile(groups);
    check(groupFile.open(QIODevice::WriteOnly));
    groupFile.write("id,mode,name\n123,B,Keyed\n");
    groupFile.close();
    const QVariantList systems{QVariantMap{{"uid", "keyed"},
                                           {"decodeFlag", "-fs"},
                                           {"trunking", true},
                                           {"freqMhz", "461"},
                                           {"groupCsvPath", groups},
                                           {"encKeyType", "basic"},
                                           {"encKeyValue", "7"},
                                           {"encForceKey", 1}},
                               QVariantMap{{"uid", "tower"},
                                           {"decodeFlag", "-fM"},
                                           {"trunking", false},
                                           {"freqMhz", "118.3"},
                                           {"groupCsvPath", groups},
                                           {"encKeyType", "basic"},
                                           {"encKeyValue", "9"},
                                           {"encForceKey", 1}}};
    const QVariantList entries{
        QVariantMap{{"uid", "keyed"}, {"kind", "system"}, {"systemUid", "keyed"}},
        QVariantMap{{"uid", "tower"}, {"kind", "system"}, {"systemUid", "tower"}},
        QVariantMap{{"uid", "ops"}, {"kind", "freq"}, {"protocol", "nfm"}, {"freqMhz", "154.43"}},
        QVariantMap{{"uid", "guard"}, {"kind", "freq"}, {"protocol", "am"}, {"freqMhz", "121.5"}}};
    auto generated = dsd_qt::scan_list_targets({{"sourceType", "usb"}, {"entries", entries}}, systems);
    check(generated.ok && generated.targetCount == 4);
    check(generated.csv.contains("tower,am-conventional,118300000,")
          && generated.csv.contains("ops,nfm-conventional,154430000,")
          && generated.csv.contains("guard,am-conventional,121500000,"));
    QFile file(dir.path() + "/mixed.csv");
    check(file.open(QIODevice::WriteOnly));
    check(file.write(generated.csv) == generated.csv.size());
    file.write("fire,nfm-conventional,155475000,,,,,,,,,,--nfm-bandwidth-hz 12500\n");
    file.write("plant,dmr-conventional,461112500,,,,,,,,,,\n");
    file.close();

    FakeRigctlPeer peer;
    check(dsd_socket_init() == 0 && peer.start());
    char host[] = "127.0.0.1";
    auto* opts = static_cast<dsd_opts*>(std::calloc(1, sizeof(dsd_opts)));
    auto* state = static_cast<dsd_state*>(std::calloc(1, sizeof(dsd_state)));
    const dsd_socket_t rigctl = opts && state ? Connect(host, peer.port) : DSD_INVALID_SOCKET;
    if (!opts || !state || rigctl == DSD_INVALID_SOCKET) {
        ++failures;
        std::fprintf(stderr, "mixed analog rotation: no session or rigctl connection\n");
        if (rigctl != DSD_INVALID_SOCKET) {
            (void)dsd_socket_close(rigctl);
        }
        std::free(opts);
        std::free(state);
        peer.stop();
        return;
    }
    opts->trunk_scan_enabled = 1;
    opts->trunk_scan_idle_dwell_ms = 250;
    opts->trunk_scan_activity_hold_ms = 250;
    opts->audio_in_type = AUDIO_IN_UDP;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = rigctl;
    state->K = 5;
    dsd_tg_policy_entry baseline{};
    check(dsd_tg_policy_make_exact_entry(123, "A", "Global", DSD_TG_POLICY_SOURCE_IMPORTED, &baseline) == 0);
    check(dsd_tg_policy_append_exact(state, &baseline) == 0);
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s",
                 file.fileName().toUtf8().constData());
    dsd_trunk_tuning_hooks hooks{};
    hooks.tune_to_freq_request = tune;
    hooks.tune_to_cc_request = tune;
    dsd_trunk_tuning_hooks_set(hooks);
    char error[256]{};
    const bool initialized = dsd_engine_trunk_scan_init(opts, state, error, sizeof error) == 0;
    check(initialized);
    if (!initialized) {
        std::fprintf(stderr, "mixed analog initialization: %s\n", error);
    }
    if (initialized) {
        check(dsd_engine_trunk_scan_target_count(state) == 6);
        policy(state, "Keyed", "B");
        check(state->K == 7);
        check(peer.commands().isEmpty());

        // keyed -> tower (am) -> ops (nfm) -> guard (am) -> fire (nfm with its own passband) -> plant (DMR). The first
        // am target reads the peer's FM passband, switches it to AM at its own to read that, then asks for 6 kHz.
        const struct {
            dsd_scan_mode mode;
            int demod;
            QStringList sent;
            const char* peerMode;
            int peerPassband;
        } rows[] = {
            {DSD_SCAN_MODE_AM, DSD_ANALOG_DEMOD_AM, {"m", "M AM 0", "m", "M AM 6000", "F 118300000"}, "AM", 6000},
            {DSD_SCAN_MODE_NFM, DSD_ANALOG_DEMOD_FM, {"M NFM 11000", "M FM 11000", "F 154430000"}, "FM", 11000},
            {DSD_SCAN_MODE_AM, DSD_ANALOG_DEMOD_AM, {"M AM 6000", "F 121500000"}, "AM", 6000},
            {DSD_SCAN_MODE_NFM, DSD_ANALOG_DEMOD_FM, {"M NFM 12500", "M FM 12500", "F 155475000"}, "FM", 12500},
        };

        for (const auto& expected : rows) {
            const qsizetype before = peer.commands().size();
            check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
            check(peer.commands().mid(before) == expected.sent);
            check(peer.mode() == QString::fromUtf8(expected.peerMode)
                  && peer.passband(peer.mode()) == expected.peerPassband);
            check(dsd_scan_mode_active(state) == expected.mode && opts->analog_only == 1
                  && opts->analog_demod == expected.demod);
            policy(state, "Global", "A");
            check(state->K == 5 && opts->frame_dmr == 0);
        }
        // The DMR target after the row with its own passband: the peer's own 11 kHz, which a passband of 0 would not
        // have put back.
        qsizetype before = peer.commands().size();
        check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        check(peer.commands().mid(before) == QStringList({"M NFM 11000", "M FM 11000", "F 461112500"}));
        check(peer.mode() == "FM" && peer.passband("FM") == 11000 && opts->analog_only == 0);
        check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        policy(state, "Keyed", "B");
        check(state->K == 7 && opts->analog_only == 0);

        // A peer that cannot demodulate AM: the am target's tune fails before the frequency moves, and the advance
        // lands on the nfm target after it, whose passband the peer already runs.
        peer.refuseAm = true;
        before = peer.commands().size();
        check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        check(peer.commands().mid(before) == QStringList({"M AM 6000", "F 154430000"}));
        check(dsd_scan_mode_active(state) == DSD_SCAN_MODE_NFM && opts->analog_demod == DSD_ANALOG_DEMOD_FM);
        peer.refuseAm = false;
        check(dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        check(dsd_scan_mode_active(state) == DSD_SCAN_MODE_AM && peer.commands().contains("F 121500000"));
        check(peer.mode() == "AM" && peer.passband("AM") == 6000);
    }
    // Shutdown on the am target: the baseline comes back, and the peer's own AM passband goes back before it returns to
    // FM at its own passband.
    const qsizetype beforeShutdown = peer.commands().size();
    dsd_engine_trunk_scan_shutdown(opts, state);
    policy(state, "Global", "A");
    check(state->K == 5 && opts->analog_only == 0);
    check(peer.commands().mid(beforeShutdown) == QStringList({"M AM 9000", "M NFM 11000", "M FM 11000"}));
    check(peer.mode() == "FM" && peer.passband("FM") == 11000 && peer.passband("AM") == 9000);
    dsd_trunk_tuning_hooks_set({});
    dsd_trunk_scan_hooks_set(nullptr);
    (void)dsd_socket_close(opts->rigctl_sockfd);
    peer.stop();
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    std::free(state);
    std::free(opts);
}
#endif

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
    dsd_trunk_scan_hooks_set(nullptr);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    DSD_SECURE_ZERO(state, sizeof *state);
    std::free(state);
    std::free(opts);
    p25CandidateRotation(0);
    p25CandidateRotation(1);
    keyMuteRotation(false);
    keyMuteRotation(true);
#if !DSD_PLATFORM_WIN_NATIVE
    mixedAnalogRotation();
#endif
    return failures ? 1 : 0;
}
