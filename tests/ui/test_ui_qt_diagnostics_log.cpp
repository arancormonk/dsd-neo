// SPDX-License-Identifier: GPL-3.0-or-later
#include <QAbstractListModel>
#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtEnvironmentVariables>
#include <QtGlobal>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <functional>
#include <mutex>
#include <string.h>
#include <thread>
#include <utility>
#include <vector>
#include "command_bridge.h"
#include "diagnostics_log.h"
extern "C" int dsd_test_diagnostics_drain(dsd_opts*, dsd_state*);
#include "../../src/app_control/commands_internal.h"
#include "session_args.h"
#include "teardown_trace.h"
extern "C" int dsd_test_diagnostics_direct_key_applied(dsd_opts*, dsd_state*, int, const char*);
#include <memory>
#include "../test_support/test_support.h"
using namespace dsd_qt;
static int failures = 0;

static void
check(bool ok, const char* reason = "diagnostics assertion failed") {
    if (!ok) {
        ++failures;
        DSD_FPRINTF(stderr, "%s\n", reason); // Never print values, captured output or key-bearing expressions.
    }
}

static QList<QByteArray>
hexRepresentations(const QByteArray& key) {
    QList<QByteArray> representations{key};
    QByteArray spaced;
    for (qsizetype i = 0; i < key.size(); i += 16) {
        const auto segment = key.mid(i, 16);
        if (!spaced.isEmpty()) {
            spaced += ' ';
        }
        spaced += segment;
        representations.append(segment); // Also catch a partial segment leak.
        representations.append(QByteArray::number(segment.toULongLong(nullptr, 16)));
    }
    representations.append(spaced);
    return representations;
}

/* The log's own process line ("[process] [info] pid <pid>, <os>") carries the real pid and kernel version, never a
 * key; left in, a pid such as 1845197 reads as a leak of the basic key 197. */
static QByteArray
withoutProcessFacts(const QByteArray& text) {
    QByteArray kept;
    for (const auto& line : text.split('\n')) {
        if (!line.startsWith("[process] [info] pid ")) {
            kept += line;
            kept += '\n';
        }
    }
    return kept;
}

static void
checkAbsent(const QByteArray& text, const QList<QByteArray>& representations, const char* surface) {
    const auto folded = withoutProcessFacts(text).toLower();
    for (const auto& representation : representations) {
        check(!representation.isEmpty() && !folded.contains(representation.toLower()), surface);
    }
}

static QByteArray
parseKey(dsd_opts* opts, dsd_state* state, const char* flag, QByteArray value, bool accepted,
         const char* errorMarker = nullptr) {
    QByteArray option(flag);
    char name[] = "diagnostics-test";
    char* argv[] = {name, option.data(), value.data(), nullptr};
    dsd_test_capture_stderr captured;
    if (dsd_test_capture_stderr_begin(&captured, "diagnostics_cli") != 0) {
        check(false, "CLI capture setup failed");
        DSD_SECURE_ZERO(value.data(), static_cast<size_t>(value.size()));
        return {};
    }
    int effective = 0, exitCode = 0;
    const int result = dsd_parse_args(3, argv, opts, state, &effective, &exitCode);
    check(result == (accepted ? DSD_PARSE_CONTINUE : DSD_PARSE_ERROR), "CLI key parse result mismatch");
    check(exitCode == (accepted ? 0 : 1), "CLI key exit result mismatch");
    check(effective == 3 && opts->show_keys == 0, "CLI argument count or key-display policy mismatch");
    check(dsd_test_capture_stderr_end(&captured) == 0, "CLI capture restore failed");
    QFile file(QString::fromUtf8(captured.path));
    check(file.open(QIODevice::ReadOnly), "CLI capture read failed");
    const auto text = file.readAll();
    // The shared direct-key helper emits no success diagnostic. State assertions
    // establish successful loading; rejected input has a format-only diagnostic.
    if (accepted) {
        check(text.isEmpty(), "successful CLI key load must be silent");
    } else {
        check(errorMarker && text.contains(errorMarker), "CLI diagnostic outcome marker missing");
    }
    file.close();
    check(file.remove(), "CLI capture cleanup failed");
    DSD_SECURE_ZERO(value.data(), static_cast<size_t>(value.size()));
    return text;
}

static void
checkHexState(const dsd_state* state, const QByteArray& key) {
    const auto first = key.left(16).toULongLong(nullptr, 16);
    const auto second = key.mid(16, 16).toULongLong(nullptr, 16);
    const auto third = key.mid(32, 16).toULongLong(nullptr, 16);
    const auto fourth = key.mid(48, 16).toULongLong(nullptr, 16);
    const int segments = key.size() == 10 ? 1 : key.size() / 16;
    check(state->H == first && state->K1 == first && state->K2 == second && state->K3 == third && state->K4 == fourth,
          "hex key scalars were not loaded");
    check(state->keyloader == 0 && state->hytera_key_segments == segments, "hex key-loader state mismatch");
    for (int slot = 0; slot < 2; ++slot) {
        check(state->A1[slot] == (segments == 1 ? 0 : first) && state->A2[slot] == second && state->A3[slot] == third
                  && state->A4[slot] == fourth,
              "AES slot scalars were not loaded");
        check(state->aes_key_loaded[slot] == (segments > 1)
                  && state->aes_key_segments[slot] == (segments == 1 ? 0 : segments),
              "AES slot loading flags mismatch");
    }
    const auto bytes = segments == 1 ? QByteArray() : QByteArray::fromHex(key);
    check(memcmp(state->aes_key, bytes.constData(), static_cast<size_t>(bytes.size())) == 0,
          "AES bytes were not loaded");
    for (size_t i = static_cast<size_t>(bytes.size()); i < sizeof(state->aes_key); ++i) {
        check(state->aes_key[i] == 0, "unused AES bytes were not cleared");
    }
}

static void
checkMutePolicy(const dsd_opts* opts, int muteLeft, int muteRight) {
    check(opts->dmr_mute_encL == muteLeft && opts->dmr_mute_encR == muteRight,
          "key loading changed the user's encrypted audio mute settings");
    check(opts->unmute_encrypted_p25 == 0, "key loading did not mute undecodable P25 audio");
}

static const char kHostHeading[] = "--- host and session records ---";
static const char kAllHeading[] = "--- all records ---";

// What Copy and Share export: the host records under their heading, when there are any, then the ring.
static QByteArray
exportOf(const DiagnosticsLog& log) {
    QStringList rows = log.hostSnapshot();
    if (!rows.isEmpty()) {
        rows.prepend(QLatin1String(kHostHeading));
        rows.append(QLatin1String(kAllHeading));
    }
    rows.append(log.snapshot());
    return rows.join(QLatin1Char('\n')).toUtf8();
}

static void
checkStoredDiagnostics(DiagnosticsLog& log, DiagnosticsLogModel& model, const QString& directory,
                       const QList<QByteArray>& representations, const QByteArray& marker) {
    model.refresh();
    log.flush();
    const auto ring = log.snapshot().join(QLatin1Char('\n')).toUtf8();
    check(ring.contains(marker), "ring positive capture marker missing");
    checkAbsent(ring, representations, "ring contains a key representation");
    QStringList displayed;
    for (int row = 0; row < model.rowCount(); ++row) {
        displayed.append(model.data(model.index(row), Qt::DisplayRole).toString());
    }
    check(displayed.join(QLatin1Char('\n')).toUtf8() == ring, "model rows do not show the captured ring");
    checkAbsent(displayed.join(QLatin1Char('\n')).toUtf8(), representations, "model rows contain a key representation");
    const auto text = model.allText().toUtf8();
    check(text == exportOf(log) && text.endsWith(ring), "allText does not contain the captured ring");
    checkAbsent(text, representations, "allText contains a key representation");
    QFile exported(directory + "/export.txt");
    check(exported.open(QIODevice::WriteOnly), "export open failed");
    check(exported.write(text) == text.size(), "export write failed");
    exported.close();
    check(exported.open(QIODevice::ReadOnly), "export read failed");
    const auto exportText = exported.readAll();
    check(exportText == text && exportText.contains(marker), "export positive capture marker missing");
    checkAbsent(exportText, representations, "export contains a key representation");
    QFile tail(directory + "/tail.log");
    check(tail.open(QIODevice::ReadOnly), "tail read failed");
    const auto tailText = tail.readAll();
    check(tailText.contains(marker), "tail positive capture marker missing");
    checkAbsent(tailText, representations, "tail contains a key representation");
}

static void
realKeySources() {
    DiagnosticsLog::installTap();
    auto& log = DiagnosticsLog::instance();
    DiagnosticsLogModel model(&log);
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    initOpts(opts.get());
    initState(state.get());
    dsd_app_frontend_runtime_start(opts.get(), state.get());

    struct KeyCase {
        int type = 0;
        const char* flag = nullptr;
        QByteArray value;
        const char* error = nullptr;
    };

    const KeyCase cases[] = {
        {DSD_KEY_TYPE_HEX, "-H", QByteArray(16, 'C') + QByteArray(16, 'D') + QByteArray(16, 'E') + QByteArray(16, 'F'),
         "-H expects 10, 32, or 64 hex digits"},
        {DSD_KEY_TYPE_HEX, "-H", QByteArray(10, 'C'), "-H expects 10, 32, or 64 hex digits"},
        {DSD_KEY_TYPE_HEX, "-H", QByteArray(16, 'A') + QByteArray(16, 'B'), "-H expects 10, 32, or 64 hex digits"},
        {DSD_KEY_TYPE_BASIC, "-b", "197", "-b expects decimal 0..255"},
        {DSD_KEY_TYPE_RC4, "-1", "DADADADADA", "-1 expects 1..16 hex digits"},
        {DSD_KEY_TYPE_SCRAMBLER, "-R", "23456", "-R expects decimal 0..32767"},
    };
    QList<QByteArray> representations;
    for (const auto& entry : cases) {
        if (entry.type == DSD_KEY_TYPE_BASIC || entry.type == DSD_KEY_TYPE_SCRAMBLER) {
            representations.append(entry.value);
            representations.append(QByteArray::number(entry.value.toULongLong(), 16));
        } else {
            representations.append(hexRepresentations(entry.value));
        }
        representations.append(entry.value + "invalid");
    }
    const QByteArray malformed = cases[1].value + "invalid";
    // Exercise wide-to-narrow HEX replacement and preservation across other
    // key types on both paths, preserving each combination of the user's
    // independent slot mute settings. The keyring survives every direct overlay.
    for (int path = 0; path < 2; ++path) {
        for (int muteSettings = 0; muteSettings < 4; ++muteSettings) {
            const int muteLeft = muteSettings & 1;
            const int muteRight = (muteSettings >> 1) & 1;
            QByteArray activeHex;
            state->K = 17;
            state->R = 19;
            state->RR = 23;
            unsigned long long basic = state->K, left = state->R, right = state->RR;
            state->rkey_array[7] = 29;
            state->rkey_array_loaded[7] = 1;
            for (const auto& entry : cases) {
                state->keyloader = 1;
                state->payload_keyid = state->payload_keyidR = 7;
                opts->dmr_mute_encL = muteLeft;
                opts->dmr_mute_encR = muteRight;
                opts->unmute_encrypted_p25 = 1;
                if (path == 0) {
                    const auto captured = parseKey(opts.get(), state.get(), entry.flag, entry.value, true);
                    checkAbsent(captured, representations, "CLI diagnostic contains a key representation");
                    log.submit("stderr", "info", QString::fromUtf8(captured));
                } else {
                    check(dsd_test_diagnostics_direct_key_applied(opts.get(), state.get(), entry.type,
                                                                  entry.value.constData()),
                          "KEY_DIRECT_SET did not apply key and activation flags");
                    checkAbsent(QByteArray(state->ui_msg), representations,
                                "direct-command message contains a key representation");
                    log.submit("host", "info", QString::fromUtf8(state->ui_msg));
                }
                if (entry.type == DSD_KEY_TYPE_HEX) {
                    activeHex = entry.value;
                } else if (entry.type == DSD_KEY_TYPE_BASIC) {
                    basic = entry.value.toULongLong();
                } else if (entry.type == DSD_KEY_TYPE_RC4) {
                    left = right = entry.value.toULongLong(nullptr, 16);
                } else {
                    left = entry.value.toULongLong();
                }
                checkHexState(state.get(), activeHex);
                checkMutePolicy(opts.get(), muteLeft, muteRight);
                check(state->K == basic && state->R == left && state->RR == right, "direct overlay scalar mismatch");
                check(state->rkey_array[7] == 29 && state->rkey_array_loaded[7] == 1, "direct overlay altered keyring");
                check(state->payload_keyid == 0 && state->payload_keyidR == 0, "direct activation retained key IDs");
                if (path == 0) {
                    dsd_key_set before{}, after{};
                    check(dsd_key_set_capture(&before, state.get()) == 0, "key state capture failed");
                    const auto captured =
                        parseKey(opts.get(), state.get(), entry.flag, entry.value + "invalid", false, entry.error);
                    check(dsd_key_set_capture(&after, state.get()) == 0 && dsd_key_set_equal(&before, &after),
                          "refused CLI key altered loaded state");
                    checkMutePolicy(opts.get(), muteLeft, muteRight);
                    dsd_key_set_free(&before);
                    dsd_key_set_free(&after);
                    checkAbsent(captured, representations, "CLI error diagnostic contains a key representation");
                    log.submit("stderr", "info", QString::fromUtf8(captured));
                }
            }
        }
    }
    log.submit("host", "error", "malformed key " + QString::fromUtf8(malformed));
    dsd_neo_log_write(LOG_LEVEL_INFO, "decoder restart marker\n");
    log.submit("host", "info", "host restart marker");
    log.submit("stderr", "info", "stderr restart marker");
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics";
    model.markSessionStarting();
    checkStoredDiagnostics(log, model, directory, representations, "--- session starting ---");
    check(model.allText().contains("decoder restart marker") && model.allText().contains("host restart marker")
              && model.allText().contains("stderr restart marker")
              && log.snapshot().join('\n').count("--- session starting ---") == 1
              && log.hostSnapshot().join('\n').count("--- session starting ---") == 1,
          "session divider must retain preceding diagnostics");
    {
        DiagnosticsLog restarted(directory);
        DiagnosticsLogModel previous(&restarted);
        previous.refresh();
        check(previous.allText().contains("previous run") && previous.allText().contains("--- session starting ---")
              && previous.allText().contains("decoder restart marker")
              && previous.allText().contains("host restart marker")
              && previous.allText().contains("stderr restart marker"));
        checkAbsent(previous.allText().toUtf8(), representations, "previous-run text contains a key representation");
    }
    dsd_app_frontend_runtime_stop();
    freeState(state.get());
    DSD_SECURE_ZERO(state.get(), sizeof(*state));
}

static void
clearWithConcurrentSubmit() {
    QTemporaryDir dir;
    DiagnosticsLog log(dir.path());
    DiagnosticsLogModel model(&log);
    log.submit("host", "info", "before clear");
    model.refresh();
    check(model.rowCount() == 1);
    // Deterministically submit after the ring clear and before the model reset
    // finishes. Sampling the generation a second time loses this record.
    const auto connection = QObject::connect(&model, &QAbstractItemModel::modelAboutToBeReset, &model, [&log] {
        std::thread producer([&log] { log.submit("host", "info", "during clear"); });
        producer.join();
    });
    model.clear();
    QObject::disconnect(connection);
    check(model.rowCount() == 0);
    model.refresh();
    check(model.rowCount() == 1);
    check(model.data(model.index(0), Qt::DisplayRole).toString().contains("during clear"));
}

/* Issue #718: the diagnostics model is a global context object, ticked every 250 ms whether or not the
 * screen is open. A full reset of up to 2000 rows per tick with new lines was ~70% of the UI tick's work
 * while the list was hidden. Hidden, it takes no rows; opened, it resets once. Set through the property
 * system so the test reads the same on a model without the property. */
static void
inactiveModelDefersResets() {
    QTemporaryDir dir;
    DiagnosticsLog log(dir.path());
    DiagnosticsLogModel model(&log);
    log.submit("host", "info", "first");
    model.refresh();
    check(model.rowCount() == 1, "an active model takes new lines at once");
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&resets] { ++resets; });

    model.setProperty("active", false);
    for (int i = 0; i < 20; ++i) {
        log.submit("host", "info", QStringLiteral("hidden %1").arg(i));
        model.refresh();
    }
    check(resets == 0, "an inactive model performs no resets across ticks with new lines");
    check(model.pendingCount() == 0, "an inactive, unpaused model counts nothing pending");
    model.setProperty("active", true);
    check(resets == 1, "becoming active resets once");
    check(model.rowCount() == 21 && model.data(model.index(20), Qt::DisplayRole).toString().contains("hidden 19"),
          "becoming active shows every line");
    model.setProperty("active", false);
    model.setProperty("active", true);
    check(resets == 1, "becoming active with nothing new does not reset");

    // Paused while hidden: the view shows the lines as they were at the pause, and counts the rest.
    model.setProperty("active", false);
    log.submit("host", "info", "before pause");
    model.refresh();
    model.setPaused(true);
    log.submit("host", "info", "paused one");
    log.submit("host", "info", "paused two");
    model.refresh();
    check(resets == 1 && model.pendingCount() == 2, "a paused, inactive model only counts new lines");
    model.setProperty("active", true);
    check(resets == 2 && model.rowCount() == 22 && model.pendingCount() == 2,
          "becoming active while paused shows the lines as they were at the pause");
    model.setPaused(false);
    check(model.rowCount() == 24 && model.pendingCount() == 0, "resuming shows the held lines");

    // A clear while hidden: only what came after it shows.
    model.setProperty("active", false);
    log.submit("host", "info", "cleared");
    model.refresh();
    model.clear();
    log.submit("host", "info", "after clear");
    model.refresh();
    model.setProperty("active", true);
    check(model.rowCount() == 1 && model.data(model.index(0), Qt::DisplayRole).toString().contains("after clear"),
          "a clear while inactive keeps only later lines");
}

static void
clearAndReopen() {
    for (const bool flushBeforeClear : {false, true}) {
        for (const bool submitAfterClear : {false, true}) {
            QTemporaryDir dir;
            {
                DiagnosticsLog log(dir.path());
                for (int i = 0; i < 2000; ++i) {
                    log.submit("host", "info", "discarded record");
                }
                if (flushBeforeClear) {
                    log.flush();
                }
                log.clear();
                check(log.snapshot().isEmpty(), "clear empties visible diagnostics");
                if (submitAfterClear) {
                    // Exceed queue capacity too: backpressure must not lose the clear.
                    for (int i = 0; i < 2100; ++i) {
                        log.submit("host", "info", "retained record");
                    }
                }
                log.flush();
                QFile tail(dir.filePath("tail.log"));
                check(tail.open(QIODevice::ReadOnly), "cleared tail is readable");
                const auto text = tail.readAll();
                check(!text.contains("discarded record"), "clear removes persisted and queued diagnostics");
                check(submitAfterClear ? text.contains("retained record") : text.isEmpty(),
                      "clear preserves only subsequent diagnostics");
            }
            DiagnosticsLog reopened(dir.path());
            DiagnosticsLogModel model(&reopened);
            model.refresh();
            check(!model.allText().contains("discarded record"), "cleared diagnostics stay absent after reopen");
            check(submitAfterClear ? model.allText().contains("retained record") : model.rowCount() == 0,
                  "reopened diagnostics preserve only subsequent records");
        }
    }
}

static void
earlyHostCaptureAndBridge() {
    {
        auto& log = DiagnosticsLog::instance();
        DiagnosticsLogModel model(&log);
        model.refresh();
        check(log.snapshot().join('\n').count("usb: permission pending before init") == 1
                  && log.hostSnapshot().join('\n').count("usb: permission pending before init") == 1,
              "pre-init USB status captured once");
        log.flush();
        QFile tail(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics/tail.log");
        check(tail.open(QIODevice::ReadOnly), "pre-init USB tail uses initialized app location");
        check(tail.readAll().count("usb: permission pending before init") == 1, "pre-init USB status persisted once");
    }
    {
        static dsd_opts opts;
        static dsd_state state;
        initOpts(&opts);
        initState(&state);
        dsd_app_frontend_runtime_start(&opts, &state);
        CommandBridge bridge;
        QString formatted;
        for (int i = 0; i < 32; ++i) {
            if (i) {
                formatted += ' ';
            }
            formatted += "11";
        }
        check(bridge.applyEncryptionKey("hex", formatted), "bridge accepts formatted AES-256 input");
        check(dsd_test_diagnostics_drain(&opts, &state) == 1 && state.K1 == 0x1111111111111111ULL
                  && state.K4 == state.K1,
              "formatted bridge AES installs full width");
        check(bridge.applyEncryptionKey("rc4", "0x 11 22 33 44"), "bridge accepts formatted RC4 input");
        check(dsd_test_diagnostics_drain(&opts, &state) == 1 && state.R == 0x11223344ULL,
              "formatted bridge RC4 installs normalized value");
        dsd_app_frontend_runtime_stop();
        DSD_SECURE_ZERO(formatted.data(), static_cast<size_t>(formatted.size()) * sizeof(QChar));
        freeState(&state);
        DSD_SECURE_ZERO(&state, sizeof state);
        DSD_SECURE_ZERO(&opts, sizeof opts);
    }
}

// Use the production bridge and queue: the QML CommandRecorder cannot exercise
// the UTF-8 payload bound. Report only assertions, never submitted key material.
static void
test_live_formatted_keys(void) {
    const auto expect = [](const char* what, bool ok) { check(ok, what); };
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    initOpts(opts.get());
    initState(state.get());
    dsd_app_frontend_runtime_start(opts.get(), state.get());
    const dsd_qt::CommandBridge bridge;

    QStringList pairs;
    for (int i = 0; i < 32; ++i) {
        pairs.append(QString::number(128 + i, 16));
    }
    const QString spaced = pairs.join(QLatin1Char(' '));
    expect("AES-256 byte-pair entry exceeds the wire capacity before normalization", spaced.size() == 95);
    const QString canonical = pairs.join(QString()).toUpper();
    for (const auto& formatted : {spaced, QStringLiteral(" \t0x") + pairs.join(QStringLiteral(" \r\n"))}) {
        QVariantMap sys{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                        {QStringLiteral("freqMhz"), QStringLiteral("851.375")}};
        sys[QStringLiteral("encKeyType")] = QStringLiteral("hex");
        sys[QStringLiteral("encKeyValue")] = formatted;
        const auto args = session_args_build(sys, SessionArgPrefs(), nullptr);
        const auto at = args.indexOf(QStringLiteral("-H"));
        expect("formatted AES-256 starts with a normalized discrete key",
               at >= 0 && at + 1 < args.size() && args[at + 1] == canonical);
        expect("formatted AES-256 accepted through the production bridge", bridge.applyEncryptionKey("hex", formatted));
        expect("formatted AES-256 reaches the real queue", dsd_app_drain_cmds(opts.get(), state.get()) == 1);
        for (int slot = 0; slot < 2; ++slot) {
            expect("formatted AES-256 arms both slots",
                   state->aes_key_loaded[slot] && state->aes_key_segments[slot] == 4);
        }
        bool correct = true;
        for (int i = 0; i < 32; ++i) {
            correct = correct && state->aes_key[i] == 128 + i;
        }
        expect("formatted AES-256 retains every decoded key byte", correct);
    }

    const QString rc4 = QStringLiteral(" \t0x") + pairs.mid(0, 8).join(QString(10, QLatin1Char(' ')));
    expect("formatted RC4 is valid before live submission", dsd_qt::session_args_key_valid("rc4", rc4));
    expect("formatted RC4 accepted through the production bridge", bridge.applyEncryptionKey("rc4", rc4));
    expect("formatted RC4 reaches the real queue", dsd_app_drain_cmds(opts.get(), state.get()) == 1);
    expect("formatted RC4 loads the complete key", state->R == canonical.left(16).toULongLong(nullptr, 16));

    expect("oversized normalized key remains refused",
           !bridge.applyEncryptionKey("hex", QString(80, QLatin1Char('a'))));
    expect("embedded NUL remains refused", !bridge.applyEncryptionKey("hex", spaced + QChar(0) + QStringLiteral(" ")));
    expect("rejected bridge payloads never enter the queue", dsd_app_drain_cmds(opts.get(), state.get()) == 0);
    dsd_app_frontend_runtime_stop();
    freeState(state.get());
    DSD_SECURE_ZERO(state.get(), sizeof(*state));
}

// --- Persistent tail: append-only segments (issue #745) ---

static std::atomic<quint64> g_clock{0};

static DiagnosticsLog::TestOptions
manualOptions() {
    DiagnosticsLog::TestOptions options;
    options.clockMs = [] { return g_clock.load(); };
    options.manualDrain = true;
    return options;
}

static QByteArray
readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static void
writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(),
          "fixture write failed");
}

static QStringList
withoutDivider(QStringList rows) {
    if (!rows.isEmpty() && rows.first() == QLatin1String("--- previous run ---")) {
        rows.removeFirst();
    }
    return rows;
}

// Every flush used to replace tail.log with a rewritten copy (one rename per batch). An append-only
// tail keeps the file a reader already holds open. Uses only the long-standing API, so it runs on
// either writer.
static void
appendKeepsTheFile() {
#ifndef _WIN32
    QTemporaryDir dir;
    DiagnosticsLog log(dir.path());
    log.submit("host", "info", "first record");
    log.flush();
    const QString path = dir.filePath("tail.log");
    QFile held(path);
    check(held.open(QIODevice::ReadOnly), "tail exists after the first flush");
    for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < 10; ++i) {
            log.submit("host", "info", QStringLiteral("record %1.%2").arg(round).arg(i));
        }
        log.flush();
        dsd_stat_t heldStat{};
        dsd_stat_t pathStat{};
        check(dsd_fstat(held.handle(), &heldStat) == 0
                  && dsd_stat_path(QFile::encodeName(path).constData(), &pathStat) == 0,
              "stat of the tail failed");
        check(heldStat.st_nlink == 1 && heldStat.st_ino == pathStat.st_ino,
              "the tail was replaced instead of appended to");
    }
#endif
}

static void
writeVolumeIsTheLogItself() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    quint64 payload = 0;
    for (int i = 0; i < 2000; ++i) {
        const QString text = QStringLiteral("Sync: +P25p1 NAC [0x006] frame %1").arg(i);
        payload += static_cast<quint64>(QStringLiteral("[stderr] [info] %1").arg(text).toUtf8().size()) + 1U;
        log.submit("stderr", "info", text);
        if (i % 50 == 49) {
            g_clock += DiagnosticsLog::kWriteIntervalMs;
            log.pump();
        }
    }
    check(log.flush(), "flush after a write burst");
    const auto stats = log.ioStats();
    check(stats.bytes <= payload + payload / 20, "write volume exceeds the log's own bytes");
    check(stats.appends <= 41, "appends were not coalesced");
    check(stats.syncs <= 2 + static_cast<quint64>(g_clock.load() / DiagnosticsLog::kSyncIntervalMs),
          "syncs exceed one per interval");
}

static void
rotationKeepsTheBound() {
    QTemporaryDir dir;
    g_clock = 0;
    int written = 0;
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        // Two queue-sized bursts of maximum-length records, then mixed sizes.
        for (int burst = 0; burst < 2; ++burst) {
            for (int i = 0; i < 2000; ++i) {
                log.submit("host", "info", QStringLiteral("seq %1 ").arg(written++) + QString(600, QLatin1Char('m')));
            }
            check(log.flush(), "burst flush");
            check(QFileInfo(dir.filePath("tail.log")).size() <= DiagnosticsLog::kSegmentLimit, "tail over the cap");
            check(QFileInfo(dir.filePath("tail.prev.log")).size() <= DiagnosticsLog::kSegmentLimit,
                  "previous segment over the cap");
        }
        for (int i = 0; i < 4000; ++i) {
            log.submit("host", "info", QStringLiteral("seq %1 ").arg(written++) + QString(i % 300, QLatin1Char('v')));
            if (i % 100 == 99) {
                g_clock += DiagnosticsLog::kWriteIntervalMs;
                log.pump();
            }
        }
        check(log.flush(), "mixed flush");
        const auto stats = log.ioStats();
        check(stats.rotations > 0 && stats.syncs >= stats.rotations, "a segment rotated before its data was synced");
    }
    check(QFileInfo(dir.filePath("tail.log")).size() <= DiagnosticsLog::kSegmentLimit, "tail over the cap");
    check(QFileInfo(dir.filePath("tail.prev.log")).size() <= DiagnosticsLog::kSegmentLimit, "previous over the cap");
    DiagnosticsLog reopened(dir.path(), manualOptions());
    const auto rows = withoutDivider(reopened.snapshot());
    check(!rows.isEmpty() && rows.last().contains(QStringLiteral("seq %1 ").arg(written - 1)),
          "reload lost the newest record");
    int last = -1;
    static const QRegularExpression seq(QStringLiteral("seq (\\d+) "));
    for (const auto& row : rows) {
        const auto match = seq.match(row);
        if (match.hasMatch()) {
            check(match.captured(1).toInt() > last, "reload out of order");
            last = match.captured(1).toInt();
        }
    }
}

static void
legacyTailIsSplit() {
    QTemporaryDir dir;
    QByteArray legacy;
    QStringList lines;
    for (int i = 0; legacy.size() < 250LL * 1024; ++i) {
        const QString line = QStringLiteral("[host] [info] legacy %1 ").arg(i) + QString(200, QChar(0x03bb));
        lines.append(line);
        legacy += line.toUtf8() + '\n';
    }
    writeFile(dir.filePath("tail.log"), legacy);
    g_clock = 0;
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        check(QFileInfo(dir.filePath("tail.log")).size() <= DiagnosticsLog::kSegmentLimit, "legacy tail not split");
        check(QFileInfo(dir.filePath("tail.prev.log")).size() <= DiagnosticsLog::kSegmentLimit, "legacy prev too big");
        const auto rows = withoutDivider(log.snapshot());
        check(!rows.isEmpty() && rows.size() <= lines.size() && lines.mid(lines.size() - rows.size()) == rows,
              "legacy text changed on load");
        check(QString::fromUtf8(readFile(dir.filePath("tail.prev.log")) + readFile(dir.filePath("tail.log")))
                      .split('\n', Qt::SkipEmptyParts)
                  == rows,
              "segments are not the loaded lines");
        for (int i = 0; i < 300; ++i) {
            log.submit("host", "info", QString(400, QLatin1Char('n')));
        }
        check(log.flush(), "flush after legacy load");
    }
    check(QFileInfo(dir.filePath("tail.log")).size() + QFileInfo(dir.filePath("tail.prev.log")).size() <= 256LL * 1024,
          "legacy tail grew past the bound");
}

static void
crashFragmentIsDropped() {
    QTemporaryDir dir;
    writeFile(dir.filePath("tail.log"), QByteArray("[host] [info] complete\n") + QByteArray(500, 'z'));
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    check(!log.snapshot().join('\n').contains(QStringLiteral("zzz")), "fragment loaded as a record");
    log.submit("host", "info", "after the crash");
    check(log.flush(), "flush after the crash");
    const auto text = readFile(dir.filePath("tail.log"));
    check(!text.contains("zzz"), "fragment kept in the tail");
    for (const auto& line : text.split('\n')) {
        check(line.size() <= 512, "a record grew past the bound");
    }
    check(text.contains("\n[host] [info] after the crash\n"), "new record not on its own line");
}

// The shape a kernel panic left on an emulator: the tail's size reached the journal before its last unsynced
// second of data, which reads back as NUL bytes.
static void
crashZeroRunIsDropped() {
    QTemporaryDir dir;
    writeFile(dir.filePath("tail.log"), QByteArray("[host] [info] before the crash\n[host] [info] zeroed")
                                            + QByteArray(3000, '\0') + QByteArray("\n[host] [info] after the run\n"));
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto rows = log.snapshot().join('\n');
    check(rows.contains(QStringLiteral("before the crash")) && rows.contains(QStringLiteral("after the run")),
          "intact lines around a zeroed run lost");
    check(!rows.contains(QChar(0)) && !rows.contains(QStringLiteral("zeroed")), "a zeroed run loaded as a record");
}

// Re-review N2: the platform hears its lines are durable only from a completed flush, so it never marks lines logged
// that a process death could still lose.
static QString g_durablePath;
static int g_durableCalls = 0;
static bool g_durableOnDisk = false;

static void
durableLogged() {
    ++g_durableCalls;
    g_durableOnDisk = readFile(g_durablePath).contains("exit: durable platform line");
}

static void
platformLinesReportedOnlyWhenDurable() {
    g_durableCalls = 0;
    {
        QTemporaryDir dir;
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        g_durablePath = dir.filePath(QStringLiteral("host.log"));
        log.noteProcess(QStringLiteral("facts"), {QStringLiteral("exit: durable platform line")});
        check(log.reportWhenDurable(0, durableLogged) && g_durableCalls == 1, "durable lines were not reported");
        check(g_durableOnDisk, "lines were reported durable before they were written");
    }
    {
        QTemporaryDir dir;
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        g_durablePath = dir.filePath(QStringLiteral("host.log"));
        log.setIoFaultForTest(DiagnosticsLog::IoFault::Sync);
        log.noteProcess(QStringLiteral("facts"), {QStringLiteral("exit: durable platform line")});
        check(!log.reportWhenDurable(0, durableLogged) && g_durableCalls == 1, "unsynced lines were reported durable");
    }
    // #782 gate r2 H4: another flush (a session attempt's) takes the platform lines between the publish and the
    // report. When its write or sync fails, the lines never became durable, whatever a later flush with nothing left
    // to write says.
    for (const auto fault : {DiagnosticsLog::IoFault::Write, DiagnosticsLog::IoFault::Sync}) {
        QTemporaryDir dir;
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        g_durablePath = dir.filePath(QStringLiteral("host.log"));
        log.noteProcess(QStringLiteral("facts"), {QStringLiteral("exit: durable platform line")});
        log.setIoFaultForTest(fault);
        log.noteSessionAttempt(QStringLiteral("rtl"));
        log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
        check(!log.reportWhenDurable(0, durableLogged) && g_durableCalls == 1,
              "lines another flush failed to write or sync were reported durable");
        // Nor does a later report: the batch that held them is gone.
        log.submit("host", "info", "a later record");
        check(!log.reportWhenDurable(0, durableLogged) && g_durableCalls == 1, "lost lines were reported later");
    }
    {
        // The other flush wrote and synced them: they are durable, and a later failure elsewhere does not undo that.
        QTemporaryDir dir;
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        g_durablePath = dir.filePath(QStringLiteral("host.log"));
        log.noteProcess(QStringLiteral("facts"), {QStringLiteral("exit: durable platform line")});
        log.noteSessionAttempt(QStringLiteral("rtl"));
        log.setIoFaultForTest(DiagnosticsLog::IoFault::Write);
        log.submit("host", "info", "lost after the lines");
        check(!log.flush(), "the later write failed");
        log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
        check(log.reportWhenDurable(0, durableLogged) && g_durableCalls == 2 && g_durableOnDisk,
              "lines another flush made durable were not reported");
    }
}

static void
flushReportsFailures() {
    QTemporaryDir dir;
    {
        DiagnosticsLog log(dir.path());
        check(log.flush(), "an empty flush completes");
        log.submit("host", "info", "durable record");
        check(log.flush(), "a flush is durable");
        DiagnosticsLog reader(dir.path(), manualOptions());
        check(reader.snapshot().join('\n').contains(QStringLiteral("durable record")), "flushed record not on disk");
        log.setIoFaultForTest(DiagnosticsLog::IoFault::Write);
        log.submit("host", "info", "lost record");
        check(log.flushOutcome() == DiagnosticsLog::FlushOutcome::Failed, "a failed write reported durable or late");
        log.setIoFaultForTest(DiagnosticsLog::IoFault::Sync);
        log.submit("host", "info", "unsynced record");
        check(!log.flush(), "a failed sync reported durable");
        log.submit("host", "info", "unsynced again");
        check(log.flushOutcome(10000) == DiagnosticsLog::FlushOutcome::Failed,
              "a failed sync within the bound reported as a timeout");
        log.noteSessionAttempt(QStringLiteral("rtl"));
        log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
        log.submit("host", "info", "recovered record");
        check(log.flush(), "writing resumes after a fault");
    }
    {
        // A record lost in an ordinary drain makes the next flush non-durable too.
        QTemporaryDir drained;
        g_clock = 0;
        DiagnosticsLog log(drained.path(), manualOptions());
        log.setIoFaultForTest(DiagnosticsLog::IoFault::Write);
        log.submit("host", "info", "lost in a drain");
        g_clock = DiagnosticsLog::kWriteIntervalMs;
        log.pump();
        log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
        check(!log.flush(), "a flush after a failed drain reported durable");
        check(log.flush(), "the failure is reported once");
    }
    QTemporaryDir blocked;
    check(QDir(blocked.path()).mkpath(QStringLiteral("tail.log/inner")), "fixture directory");
    DiagnosticsLog log(blocked.path());
    log.submit("host", "info", "nowhere to go");
    check(!log.flush(), "an unopenable tail reported durable");
}

static void
syncCadenceAndStartupWindow() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.submit("host", "info", "quiet record");
    g_clock = DiagnosticsLog::kWriteIntervalMs;
    log.pump();
    check(log.ioStats().appends == 1 && log.ioStats().syncs == 0, "an append was synced before its interval");
    g_clock = DiagnosticsLog::kSyncIntervalMs;
    log.pump();
    check(log.ioStats().syncs == 1, "dirty data not synced at its deadline");
    log.pump();
    check(log.ioStats().syncs == 1, "a clean tail was synced again");

    g_clock = 5000;
    log.noteSessionAttempt(QStringLiteral("rtl, boot 3"));
    check(readFile(dir.filePath("tail.log")).contains("--- session starting: rtl, boot 3 ---"),
          "the attempt marker is not on disk when the note returns");
    auto syncs = log.ioStats().syncs;
    for (int i = 0; i < 5; ++i) {
        g_clock += 100;
        log.submit("stderr", "info", QStringLiteral("stage %1").arg(i));
        log.pump();
        check(log.ioStats().syncs == ++syncs, "a startup-window record waited for the drain or the sync interval");
    }
    g_clock = 5000 + DiagnosticsLog::kStartupSyncWindowMs + 1;
    syncs = log.ioStats().syncs;
    for (int i = 0; i < 40; ++i) {
        g_clock += 50;
        log.submit("stderr", "info", QStringLiteral("steady %1").arg(i));
        log.pump();
    }
    check(log.ioStats().syncs - syncs <= 3, "steady-state syncs exceed one per second");
}

// One marker per attempt: a USB access request records its own line and only the start records the marker; a start
// note absorbs the plain marker of the Starting transition after it. Every new attempt, the same source again included,
// gets its own.
static void
oneMarkerPerAttempt() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto count = [&log](const QString& marker) { return log.snapshot().join('\n').count(marker); };
    const QString usb = QStringLiteral("--- session starting: usb, boot 3 ---");
    const QString access = QStringLiteral("usb access requested: usb, boot 3");
    const QString plain = QStringLiteral("--- session starting ---");

    log.noteSessionAttempt(QStringLiteral("usb, boot 3"), DiagnosticsLog::AttemptStage::UsbAccess);
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"));
    log.noteSessionAttempt(QString());
    check(count(access) == 1 && count(usb) == 1 && count(plain) == 0,
          "one attempt through USB access logged other than one marker");

    // Stop, then Play the same source again within the dedupe window: a new attempt.
    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"), DiagnosticsLog::AttemptStage::UsbAccess);
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"));
    log.noteSessionAttempt(QString());
    check(count(usb) == 2 && count(plain) == 0, "a second Play of the same source lost its marker");

    // A USB access request whose start never came (denied, or abandoned for a backup flow), then a Play of the device
    // it opened: that start is its own attempt and keeps its marker.
    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"), DiagnosticsLog::AttemptStage::UsbAccess);
    log.submit("host", "info", QStringLiteral("between the attempts"));
    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"));
    log.noteSessionAttempt(QString());
    const auto rows = log.snapshot();
    check(count(usb) == 3 && count(plain) == 0 && rows.value(rows.size() - 1).contains(usb)
              && rows.value(rows.size() - 2).contains(QStringLiteral("between the attempts")),
          "an abandoned USB access request swallowed a later start's marker");

    // The first Play of a dongle: its label joins the start's context once the access request has opened it.
    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"), DiagnosticsLog::AttemptStage::UsbAccess);
    log.noteSessionAttempt(QStringLiteral("usb, boot 3, RTL2838UHIDIR 0bda:2838"));
    check(count(QStringLiteral("--- session starting: usb, boot 3, RTL2838UHIDIR 0bda:2838 ---")) == 1,
          "a dongle's first start lost its labelled marker");

    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("rtltcp, boot 3"));
    log.noteSessionAttempt(QStringLiteral("rtltcp, boot 3"));
    check(count(QStringLiteral("--- session starting: rtltcp, boot 3 ---")) == 2, "two starts shared one marker");

    // Starting transitions with no start note before them each get the plain marker.
    log.noteSessionAttempt(QString());
    log.noteSessionAttempt(QString());
    log.noteSessionAttempt(QString());
    check(count(plain) == 2, "plain markers miscounted");
}

// A sync that keeps failing is retried after an interval, never in a loop (the writer thread runs the same
// due check pump() does).
static void
failingSyncBacksOff() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.setIoFaultForTest(DiagnosticsLog::IoFault::Sync);
    log.submit("host", "info", "record");
    g_clock = DiagnosticsLog::kSyncIntervalMs;
    check(log.pump() && log.ioStats().syncFailures == 1, "the due sync was not attempted");
    for (int i = 0; i < 100; ++i) {
        check(!log.pump(), "the writer would step again at once after a failed sync");
    }
    check(log.ioStats().syncFailures == 1, "a failed sync was retried at once");
    g_clock += DiagnosticsLog::kSyncIntervalMs;
    log.pump();
    check(log.ioStats().syncFailures == 2, "a failed sync was not retried after its interval");
    log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
    g_clock += DiagnosticsLog::kSyncIntervalMs;
    log.pump();
    check(log.ioStats().syncs == 1, "the sync did not recover");

    // A failure, then a forced sync that succeeds (a Play checkpoint) within the backoff: the startup window's
    // next append is synced at once rather than after the old failure's retry time.
    log.setIoFaultForTest(DiagnosticsLog::IoFault::Sync);
    log.submit("host", "info", "before the failure");
    g_clock += DiagnosticsLog::kSyncIntervalMs;
    log.pump();
    log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
    g_clock += 100;
    log.noteSessionAttempt(QStringLiteral("rtl"));
    const auto syncs = log.ioStats().syncs;
    g_clock += 100;
    log.submit("stderr", "info", "startup stage");
    log.pump();
    check(log.ioStats().syncs == syncs + 1, "a startup append waited out a recovered failure's backoff");
}

static void
segmentsStartWithTheirContext() {
    QTemporaryDir dir;
    g_clock = 0;
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        log.noteProcess(QStringLiteral("1, test os, boot 7"));
        log.noteSessionAttempt(QStringLiteral("rtltcp"));
        for (int i = 0; i < 1200; ++i) {
            log.submit("stderr", "info", QString(300, QLatin1Char('s')));
        }
        check(log.flush(), "header flush");
        check(log.ioStats().rotations >= 2, "the fixture did not rotate");
    }
    for (const char* name : {"tail.log", "tail.prev.log"}) {
        const auto lines = readFile(dir.filePath(QLatin1String(name))).split('\n');
        check(lines.size() > 2 && lines.at(0).startsWith("[process] [info] pid 1, test os, boot 7")
                  && lines.at(1).contains("--- session starting: rtltcp ---"),
              "a segment does not start with its process and session");
    }
}

// A full segment rotates on the run's first record, which is its own process line: written once, not again
// in the header ahead of it.
static void
contextLineWrittenOnce() {
    QTemporaryDir dir;
    QByteArray full;
    while (full.size() < DiagnosticsLog::kSegmentLimit - 40) {
        full += "[stderr] [info] earlier run line\n";
    }
    writeFile(dir.filePath("tail.log"), full);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("1, test os, boot 2"), {QStringLiteral("exit: one")});
    log.noteSessionAttempt(QStringLiteral("usb"));
    check(log.flush(), "context flush");
    const auto tail = readFile(dir.filePath("tail.log"));
    check(log.ioStats().rotations == 1, "the fixture did not rotate on the first record");
    check(tail.count("[process] [info] pid 1, test os, boot 2") == 1 && tail.startsWith("[process] [info] pid 1"),
          "the process line was written twice or not first");
    check(tail.count("--- session starting: usb ---") == 1, "the session line was written twice");
}

// After Clear the saved tail starts again with this process's line, and the cleared session's marker never comes
// back as a later segment's header. A clear that does not reach the disk is reported by the next flush.
static void
clearForgetsTheCleared() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("1, test os, boot 4"));
    log.noteSessionAttempt(QStringLiteral("cleared source"));
    check(log.flush(), "pre-clear flush");
    log.clear();
    // About one and a half segments: exactly one rotation, so the previous segment is the first one after Clear.
    for (int i = 0; i < 900; ++i) {
        log.submit("stderr", "info", QString(200, QLatin1Char('q')));
    }
    check(log.flush(), "post-clear flush");
    check(log.ioStats().rotations == 1, "post-clear writes did not rotate exactly once");
    const auto saved = readFile(dir.filePath("tail.prev.log")) + readFile(dir.filePath("tail.log"));
    check(!saved.contains("cleared source"), "a cleared session marker came back in a header");
    check(readFile(dir.filePath("tail.prev.log")).startsWith("[process] [info] pid 1, test os, boot 4"),
          "the first segment after Clear does not start with the process line");
    log.setIoFaultForTest(DiagnosticsLog::IoFault::Sync);
    log.clear();
    check(!log.flush(), "a clear that did not reach the disk reported durable");
}

// The previous run's identity and this run's process and session lines stay at the front of what Copy and Share
// export, however many records arrive after them.
static void
pinnedContextOutlivesTheRing() {
    QTemporaryDir dir;
    QByteArray previous("[process] [info] pid 9, android, boot 4\n[session] [info] --- session starting: rtl ---\n");
    for (int i = 0; i < 2500; ++i) {
        previous += QStringLiteral("[stderr] [info] short %1\n").arg(i).toUtf8();
    }
    writeFile(dir.filePath("tail.log"), previous);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("10, android, boot 4"), {QStringLiteral("exit: one"), QStringLiteral("exit: two")});
    log.noteSessionAttempt(QStringLiteral("rtltcp, boot 4"));
    for (int i = 0; i < 2100; ++i) {
        log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
    }
    const auto rows = log.snapshot();
    check(rows.size() <= 2000, "ring over its limit");
    const auto head = rows.mid(0, 6).join('\n');
    check(rows.value(0) == QLatin1String("--- previous run ---") && head.contains(QStringLiteral("pid 9, android"))
              && head.contains(QStringLiteral("session starting: rtl ---"))
              && head.contains(QStringLiteral("pid 10, android"))
              && head.contains(QStringLiteral("session starting: rtltcp, boot 4")),
          "context lines were evicted from the ring");
    check(rows.last().contains(QStringLiteral("live 2099")), "newest record missing");
}

// A split of an older build's tail that a crash interrupted after writing the previous segment is redone from the
// unsplit tail, without duplicating its records.
static void
interruptedLegacySplit() {
    QTemporaryDir dir;
    QByteArray legacy;
    for (int i = 0; i < 500; ++i) {
        legacy += QStringLiteral("[host] [info] legacy %1 ").arg(i).toUtf8() + QByteArray(400, 'l') + '\n';
    }
    writeFile(dir.filePath("tail.log"), legacy);
    writeFile(dir.filePath("tail.prev.log"), legacy.left(legacy.indexOf('\n', 100LL * 1024) + 1));
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto text = log.snapshot().join('\n');
    // Record 200 lies inside the interrupted split's prefix, so a reload that joined both would carry it twice.
    check(text.count(QStringLiteral("legacy 499 ")) == 1 && text.count(QStringLiteral("legacy 200 ")) == 1,
          "an interrupted split duplicated records");
    const auto saved = readFile(dir.filePath("tail.prev.log")) + readFile(dir.filePath("tail.log"));
    check(saved.count("legacy 200 ") == 1, "the re-split segments duplicate records");
}

// A split of an older build's tail whose previous segment cannot be written leaves the unsplit tail as it was, so the
// prefix is not lost; the next load splits it again.
static void
failedLegacySplitKeepsTheTail() {
    QTemporaryDir dir;
    QByteArray legacy;
    for (int i = 0; i < 500; ++i) {
        legacy += QStringLiteral("[host] [info] legacy %1 ").arg(i).toUtf8() + QByteArray(400, 'l') + '\n';
    }
    writeFile(dir.filePath("tail.log"), legacy);
    // A directory where the previous segment goes: replacing it fails on every platform.
    check(QDir(dir.path()).mkdir(QStringLiteral("tail.prev.log")), "blocker setup");
    g_clock = 0;
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        check(readFile(dir.filePath("tail.log")).startsWith("[host] [info] legacy 0 "),
              "a failed split truncated the unsplit tail");
    }
    check(QDir(dir.filePath(QStringLiteral("tail.prev.log"))).removeRecursively(), "blocker cleanup");
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto text = log.snapshot().join('\n');
    check(text.count(QStringLiteral("legacy 0 ")) == 1 && text.count(QStringLiteral("legacy 499 ")) == 1,
          "records were lost or duplicated after a failed split");
}

// A rotation whose sync fails and whose new segment cannot be opened leaves no dirty count behind with no file to
// sync it, so the writer does not stay due forever.
static void
lostSegmentDoesNotSpin() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    for (int i = 0; i < 590; ++i) {
        log.submit("stderr", "info", QString(200, QLatin1Char('k')));
    }
    check(log.flush(), "fill flush");
    log.setIoFaultForTest(DiagnosticsLog::IoFault::SyncAndOpen);
    for (int i = 0; i < 20; ++i) {
        log.submit("stderr", "info", QString(200, QLatin1Char('k')));
    }
    check(!log.flush(), "a lost segment reported durable");
    for (int i = 0; i < 5; ++i) {
        g_clock += 10ULL * DiagnosticsLog::kSyncIntervalMs;
        check(!log.pump(), "the writer stayed due with no segment to sync");
    }
}

// Session lines a newer one replaced give their pinned slot back: after many sessions the front still holds the
// previous run's identity, this process and the current session.
static void
pinsFollowTheCurrentSession() {
    QTemporaryDir dir;
    writeFile(dir.filePath("tail.log"),
              "[process] [info] pid 9, android, boot 4\n[session] [info] --- session starting: old ---\n");
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("10, android, boot 4"));
    for (int session = 0; session < 10; ++session) {
        g_clock += 70000;
        log.noteSessionAttempt(QStringLiteral("source %1").arg(session));
        for (int i = 0; i < 2100; ++i) {
            log.submit("stderr", "info", QStringLiteral("live %1.%2").arg(session).arg(i));
        }
    }
    const auto rows = log.snapshot();
    const auto head = rows.mid(0, 5).join('\n');
    check(rows.size() <= 2000, "ring over its limit");
    check(head.contains(QStringLiteral("pid 9, android")) && head.contains(QStringLiteral("session starting: old"))
              && head.contains(QStringLiteral("pid 10, android")) && head.contains(QStringLiteral("source 9")),
          "the current session's line was evicted while older ones stayed pinned");
    check(!rows.join('\n').contains(QStringLiteral("source 3 ")), "a replaced session line stayed pinned");
}

// Repeats of one session line (the same source started again and again) take a single pinned slot, so the current
// process and session lines still fit at the front.
static void
identicalSessionLinesPinOnce() {
    QTemporaryDir dir;
    QByteArray saved("[process] [info] pid 9, android, boot 4\n");
    for (int i = 0; i < 10; ++i) {
        saved += "[session] [info] --- session starting: usb, boot 4 ---\n[stderr] [info] between\n";
    }
    writeFile(dir.filePath("tail.log"), saved);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("10, android, boot 4"));
    log.noteSessionAttempt(QStringLiteral("rtltcp, boot 4"));
    for (int i = 0; i < 2100; ++i) {
        log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
    }
    const auto rows = log.snapshot();
    const auto head = rows.mid(0, 5).join('\n');
    check(head.contains(QStringLiteral("pid 10, android")) && head.contains(QStringLiteral("session starting: rtltcp")),
          "repeated session lines crowded the current context out of the front");
    check(rows.join('\n').count(QStringLiteral("session starting: usb, boot 4")) == 1,
          "a repeated line was pinned twice");
}

// A previous run that never started a session pins no session line: an older run's session line stays with that
// run's process line, so neither is pinned beside the newer process.
static void
pinnedSessionBelongsToThePinnedProcess() {
    QTemporaryDir dir;
    QByteArray saved("[process] [info] pid 7, android, boot 4\n[session] [info] --- session starting: older ---\n"
                     "[process] [info] pid 8, android, boot 4\n");
    for (int i = 0; i < 2500; ++i) {
        saved += QStringLiteral("[stderr] [info] short %1\n").arg(i).toUtf8();
    }
    writeFile(dir.filePath("tail.log"), saved);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("9, android, boot 4"));
    for (int i = 0; i < 2100; ++i) {
        log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
    }
    const auto text = log.snapshot().join('\n');
    check(text.contains(QStringLiteral("pid 8, android")) && text.contains(QStringLiteral("pid 9, android")),
          "a process line was evicted from the front");
    check(!text.contains(QStringLiteral("session starting: older")),
          "an older run's session line was pinned beside a newer process line");
}

// Runs on one boot that start the same source write identical session lines; each run's own line stays pinned after
// its own process line.
static void
identicalSessionLinesAcrossRuns() {
    const QString session = QStringLiteral("--- session starting: usb, boot 4 ---");
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("tail.log"), "[process] [info] pid 9, android, boot 4\n"
                                            "[session] [info] --- session starting: usb, boot 4 ---\n");
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        log.noteProcess(QStringLiteral("10, android, boot 4"));
        log.noteSessionAttempt(QStringLiteral("usb, boot 4"));
        for (int i = 0; i < 2100; ++i) {
            log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
        }
        const auto rows = log.snapshot();
        const auto text = rows.join('\n');
        const auto at = [&rows](const QString& needle, int from) {
            for (int i = from; i < rows.size(); ++i) {
                if (rows.at(i).contains(needle)) {
                    return i;
                }
            }
            return -1;
        };
        const int current = at(QStringLiteral("pid 10, android"), 0);
        check(text.count(session) == 2 && current >= 0 && at(session, current) > current,
              "the current run's session line was evicted as a repeat of the previous run's");
    }
    {
        QTemporaryDir dir;
        QByteArray saved(
            "[process] [info] pid 7, android, boot 4\n[session] [info] --- session starting: usb, boot 4 ---\n"
            "[process] [info] pid 8, android, boot 4\n[session] [info] --- session starting: usb, boot 4 ---\n");
        // Few enough to stay in the reloaded ring: the live records evict them later, past the pinning.
        for (int i = 0; i < 1000; ++i) {
            saved += QStringLiteral("[stderr] [info] short %1\n").arg(i).toUtf8();
        }
        writeFile(dir.filePath("tail.log"), saved);
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        log.noteProcess(QStringLiteral("9, android, boot 4"));
        for (int i = 0; i < 2100; ++i) {
            log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
        }
        const auto rows = log.snapshot();
        check(rows.value(1).contains(QStringLiteral("pid 8, android")) && rows.value(2).contains(session)
                  && rows.join('\n').count(session) == 1,
              "the pinned previous session line is not the one after the pinned process line");
    }
}

// A reload that reserves a slot for the process line moves its cutoff past the session line: that line is kept too.
static void
reloadCutoffKeepsBothContextLines() {
    QTemporaryDir dir;
    QByteArray saved("[process] [info] pid 9, android, boot 4\n[process] [info] exit: one\n"
                     "[session] [info] --- session starting: rtl ---\n");
    for (int i = 0; i < 1998; ++i) {
        saved += QStringLiteral("[stderr] [info] decoder %1\n").arg(i).toUtf8();
    }
    writeFile(dir.filePath("tail.log"), saved);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto text = log.snapshot().join('\n');
    check(text.contains(QStringLiteral("pid 9, android")) && text.contains(QStringLiteral("session starting: rtl")),
          "a context line fell between the reload cutoffs");
}

static void
previousRunBootFact() {
    const auto run = [](const QByteArray& previous, const QString& facts) {
        QTemporaryDir dir;
        writeFile(dir.filePath("tail.log"), previous);
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        log.noteProcess(facts);
        return log.snapshot().join('\n');
    };
    const QByteArray before("[process] [info] pid 1, android, boot 5\n[stderr] [info] streaming\n");
    check(run(before, QStringLiteral("2, android, boot 6"))
              .contains(QStringLiteral("previous run: Android has restarted since (boot 5 -> 6)")),
          "boot change not reported");
    check(!run(before, QStringLiteral("2, android, boot 5")).contains(QStringLiteral("previous run:")),
          "same boot reported as a restart");
    check(!run(before, QStringLiteral("2, desktop")).contains(QStringLiteral("previous run:")),
          "unknown boot reported as a restart");
}

static void
contextSurvivesTheRingLimit() {
    QTemporaryDir dir;
    QByteArray previous("[process] [info] pid 9, android, boot 4\n[session] [info] --- session starting: rtl ---\n");
    for (int i = 0; i < 2500; ++i) {
        previous += QStringLiteral("[stderr] [info] short %1\n").arg(i).toUtf8();
    }
    writeFile(dir.filePath("tail.log"), previous);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    DiagnosticsLogModel model(&log);
    model.refresh();
    const auto rows = model.allText().split('\n');
    check(rows.size() <= 2000, "ring over its limit");
    check(rows.size() > 3 && rows.at(1).startsWith(QStringLiteral("[process] [info] pid 9"))
              && rows.at(2).contains(QStringLiteral("session starting: rtl")),
          "context dropped by the ring limit");
    check(rows.last().contains(QStringLiteral("short 2499")), "newest previous-run line missing");
    log.noteProcess(QStringLiteral("10, android, boot 5"));
    check(log.snapshot().join('\n').contains(QStringLiteral("boot 4 -> 5")), "boot fact lost to the ring limit");
}

static void
unicodeDirectory() {
    QTemporaryDir root;
    const QString path = root.filePath(QStringLiteral("диагностика-診断"));
    check(QDir().mkpath(path), "unicode directory");
    QByteArray legacy;
    while (legacy.size() < 200LL * 1024) {
        legacy += "[host] [info] legacy line with some length to it\n";
    }
    writeFile(path + QStringLiteral("/tail.log"), legacy);
    g_clock = 0;
    DiagnosticsLog log(path, manualOptions());
    check(QFile::exists(path + QStringLiteral("/tail.prev.log")), "legacy split failed in a unicode directory");
    for (int i = 0; i < 2000; ++i) {
        log.submit("host", "info", QString(200, QLatin1Char('u')));
    }
    check(log.flush() && log.ioStats().rotations > 0, "rotation failed in a unicode directory");
    check(QFileInfo(path + QStringLiteral("/tail.log")).size() <= DiagnosticsLog::kSegmentLimit,
          "unicode tail over cap");
    check(!log.snapshot().join('\n').contains(QStringLiteral("could not be rotated")), "unicode rotation fell back");
}

static void
orphansAndExpiry() {
    QTemporaryDir dir;
    writeFile(dir.filePath("tail.log.AbC123"), "orphan");
    writeFile(dir.filePath("tail.prev.log.Xy9z00"), "orphan");
    writeFile(dir.filePath("tail.log.toolong7"), "kept");
    writeFile(dir.filePath("notes.txt"), "kept");
    writeFile(dir.filePath("tail.prev.log"), "[host] [info] stale\n");
    QFile stale(dir.filePath("tail.prev.log"));
    check(stale.open(QIODevice::ReadWrite)
          && stale.setFileTime(QDateTime::currentDateTimeUtc().addDays(-8), QFileDevice::FileModificationTime));
    stale.close();
    DiagnosticsLog log(dir.path(), manualOptions());
    check(!QFile::exists(dir.filePath("tail.log.AbC123")) && !QFile::exists(dir.filePath("tail.prev.log.Xy9z00")),
          "orphaned temporaries kept");
    check(QFile::exists(dir.filePath("tail.log.toolong7")) && QFile::exists(dir.filePath("notes.txt")),
          "unrelated files removed");
    check(!QFile::exists(dir.filePath("tail.prev.log")), "an expired previous segment kept");
}

static void
rotationFailureKeepsTheBound() {
    QTemporaryDir dir;
    check(QDir(dir.path()).mkpath(QStringLiteral("tail.prev.log/inner")), "fixture directory");
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    for (int i = 0; i < 3000; ++i) {
        log.submit("host", "info", QStringLiteral("seq %1 ").arg(i) + QString(100, QLatin1Char('r')));
    }
    log.flush();
    check(QFileInfo(dir.filePath("tail.log")).size() <= DiagnosticsLog::kSegmentLimit, "tail grew past the cap");
    check(log.snapshot().join('\n').count(QStringLiteral("could not be rotated")) == 1,
          "rotation failure not noted once");
    check(readFile(dir.filePath("tail.log")).contains("seq 2999 "), "newest record lost after a failed rotation");
}

static const char kDropped[] = "[redacted sensitive diagnostic]";

static void
checkLabelled(bool ok, const QByteArray& reason) {
    // Labels name the template or the option and form, never a value.
    check(ok, reason.constData());
}

static QStringList
streamRecords(const QByteArray& bytes, const QString& directory) {
    DiagnosticsLog log(directory, manualOptions());
    DiagnosticsLog::StreamCapture capture(log, QStringLiteral("stderr"));
    capture.feed(bytes.constData(), static_cast<size_t>(bytes.size()));
    capture.finish();
    return log.snapshot();
}

/* The records that say why a USB receiver failed, as each producer writes them: librtlsdr through the stderr pump,
 * dsd-neo's own rtl/usb lines through the runtime log, and libusb's log lines (forwarded to the runtime log with
 * their own "libusb: " header). Each keeps its negative code verbatim, live and again when a saved tail is reloaded
 * or shared. */
static void
producerErrorLinesSurvive() {
    const QStringList stderrLines = {
        QStringLiteral("usb_claim_interface error -1"),
        QStringLiteral("usb_open error -3"),
        QStringLiteral("libusb_set_option error -12"),
        QStringLiteral("libusb_wrap_sys_device error -1"),
        QStringLiteral("rtlsdr_read_array failed with -1"),
        QStringLiteral("rtlsdr_write_array failed with -9"),
        QStringLiteral("rtlsdr_read_reg failed with -7"),
        QStringLiteral("rtlsdr_write_reg failed with -4"),
        QStringLiteral("rtlsdr_demod_read_reg failed with -1"),
        QStringLiteral("rtlsdr_demod_write_reg failed with -99"),
        QStringLiteral("WARNING: Failed to read the tuner's gain table (rc=-5); gain unchanged."),
    };
    QTemporaryDir dir;
    {
        QByteArray bytes;
        for (const auto& line : stderrLines) {
            bytes += line.toUtf8() + '\n';
        }
        const auto records = streamRecords(bytes, dir.path());
        check(records.size() == stderrLines.size(), "stderr template records lost or split");
        for (qsizetype i = 0; i < std::min(records.size(), stderrLines.size()); ++i) {
            const QString expected = QStringLiteral("[stderr] [info] ") + stderrLines.at(i);
            checkLabelled(records.at(i) == expected, "stderr template not verbatim: " + stderrLines.at(i).toUtf8());
            checkLabelled(DiagnosticsLog::redact(records.at(i)) == expected,
                          "stored stderr template not verbatim: " + stderrLines.at(i).toUtf8());
        }
    }
    {
        DiagnosticsLog reloaded(dir.path());
        const auto previous = reloaded.snapshot();
        for (const auto& line : stderrLines) {
            checkLabelled(previous.contains(QStringLiteral("[stderr] [info] ") + line),
                          "reloaded stderr template not verbatim: " + line.toUtf8());
        }
    }

    struct DecoderLine {
        dsd_neo_log_level_t level;
        const char* levelName;
        QByteArray text;
    };

    const DecoderLine decoderLines[] = {
        {LOG_LEVEL_WARN, "warn",
         "WARNING: Failed to adjust fs/4 capture center for actual device rate: center=465975000, capture=1536000 Hz "
         "(rc=-1)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: PPM correction request -12 failed (rc=-1); keeping applied value -2."},
        {LOG_LEVEL_ERROR, "error", "Failed to apply RTL-SDR center frequency 465975000 Hz (rc=-1)."},
        {LOG_LEVEL_ERROR, "error", "Failed to apply RTL-SDR sample rate 1536000 Hz (rc=-9)."},
        {LOG_LEVEL_ERROR, "error", "Failed to return the RTL-SDR to center frequency 465975000 Hz (rc=-1)."},
        {LOG_LEVEL_ERROR, "error", "Failed to return the RTL-SDR to sample rate 1536000 Hz (rc=-1)."},
        {LOG_LEVEL_ERROR, "error",
         "The RTL-SDR could not be put back on 465975000 Hz after a retune refused for the analog channel width "
         "(rc=-1); the stream stops."},
        {LOG_LEVEL_ERROR, "error", "Failed to apply initial RTL-SDR center frequency 465975000 Hz (rc=-1)."},
        {LOG_LEVEL_ERROR, "error", "Failed to apply initial RTL-SDR sample rate 1536000 Hz (rc=-1)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: Retune applied with warning: 465975000 Hz (rc=-1)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: Retune failed: 465975000 Hz (rc=-1)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: PPM correction applied but reconfigure to 465975000 Hz failed (rc=-1)."},
        {LOG_LEVEL_WARN, "warn", "WARNING: Retune tuner gain apply failed (rc=-1); continuing retune"},
        {LOG_LEVEL_ERROR, "error", "RTL-SDR: could not allocate USB transfer buffers (rc=-1)"},
        {LOG_LEVEL_ERROR, "error",
         "RTL-SDR: could not start the sample-progress monitor (rc=-1); the stream does not start"},
        {LOG_LEVEL_ERROR, "error", "libusb: error [op_claim_interface] claim interface failed, errno=16"},
        {LOG_LEVEL_ERROR, "error", "libusb: error [handle_events] backend handle_events failed with error -1"},
        {LOG_LEVEL_WARN, "warn", "libusb: warning [do_close] cancel transfer failed error -5"},
        {LOG_LEVEL_ERROR, "error",
         "libusb: error [op_open] libusb couldn't open USB device /dev/bus/usb/001/002, errno=13"},
    };
    auto& log = DiagnosticsLog::instance();
    for (const auto& line : decoderLines) {
        dsd_neo_log_write(line.level, "%s\n", line.text.constData());
        const auto ring = log.snapshot();
        // The runtime log ends each record with its newline, which every record keeps as a space.
        const QString expected =
            QStringLiteral("[decoder] [%1] ").arg(QLatin1String(line.levelName)) + QString::fromUtf8(line.text) + ' ';
        checkLabelled(!ring.isEmpty() && ring.last() == expected, "decoder template not verbatim: " + line.text);
        checkLabelled(DiagnosticsLog::redact(expected) == expected,
                      "stored decoder template not verbatim: " + line.text);
    }
    // The Android service's results, which name a failed run's code (#782: "engine run returned -1").
    for (const auto& line :
         {QStringLiteral("engine run returned -1"), QStringLiteral("service: engine configuration returned -2")}) {
        DiagnosticsLog::submitHostDiagnostic(line);
        const auto ring = log.snapshot();
        const QString expected = QStringLiteral("[host] [info] ") + line;
        checkLabelled(!ring.isEmpty() && ring.last() == expected, "host template not verbatim: " + line.toUtf8());
        checkLabelled(DiagnosticsLog::redact(expected) == expected,
                      "stored host template not verbatim: " + line.toUtf8());
    }
    // Every listed producer line has only %d and %u conversions, so each one counts.
    const auto counts = DiagnosticsLog::producerLineCountsForTest();
    check(counts.first == 32 && counts.second == counts.first, "a producer line was left out or the list changed");
}

/* A decimal is no option cluster (getopt stops at the '.'), so measurements keep their negative values; a digit cluster
 * that getopt would read as options still drops. */
static void
negativeDecimalsSurvive() {
    for (const auto& record :
         {QStringLiteral("[stderr] [info] SNR -3.2 dB"), QStringLiteral("[decoder] [info] Input level -48.9 dBFS "),
          QStringLiteral("[stderr] [info] offset -0.5 ppm")}) {
        checkLabelled(DiagnosticsLog::redact(record) == record, "negative decimal dropped: " + record.toUtf8());
    }
    for (const auto& record : {QStringLiteral("[stderr] [info] level -35"), QStringLiteral("[stderr] [info] -ZT1F")}) {
        checkLabelled(DiagnosticsLog::redact(record) == kDropped, "option cluster kept: " + record.toUtf8());
    }
}

/* #782 gate r1 G5: lines that report a signed measurement or setting keep it, as each producer writes it, though its
 * leading digit is a key option's letter (-1, -2, -5, -9, or one bundled behind -0, -3, -4, -8): a number with a
 * fraction, one followed by a unit that is no hex value, or by punctuation that no key value starts with, and the whole
 * lines that end on an integer or follow it with a word. Live through the stream capture and the log tap, and again
 * from a saved record. */
static void
negativeMeasurementsSurvive() {
    const QStringList stderrLines = {
        // rtl_device.cpp: the applied ppm and tuner gains (E4000 and FC0012 tables go below 0 dB).
        QStringLiteral("Tuner error set to -5 ppm."),
        QStringLiteral("Tuner error set to -12 ppm."),
        QStringLiteral("Tuner gain set to -9.90 dB."),
        QStringLiteral("Tuner gain set to -1.00 dB."),
        QStringLiteral("Tuner manual gain (nearest): -9.9 dB."),
        QStringLiteral("SoapySDR: adjusted gain LNA from -12.00 dB to -10.00 dB."),
        // Protocol lines: P25 channel identifier offsets, a P25p2 local time offset, M17's power, DMR's offset.
        QStringLiteral(" Channel Identifier Update; Iden: 1; BW: 5; TX Offset: -25; Spacing: 64; Base: 170200000;"),
        QStringLiteral("  IDEN [1] Type [1] Base Freq [34040000] (170200000 Hz) TX Offset [-25] Spacing [100]"),
        QStringLiteral("  Local Time Offset: -5.0 Hours;"),
        QStringLiteral(" PWR: -51.2 dB SQL HIT: 0;"),
        QStringLiteral(" Offset: -5;"),
    };
    QTemporaryDir dir;
    {
        QByteArray bytes;
        QStringList expected;
        for (const auto& line : stderrLines) {
            bytes += line.toUtf8() + '\n';
            expected << QStringLiteral("[stderr] [info] ") + line;
        }
        const auto records = streamRecords(bytes, dir.path());
        check(records == expected, "a measurement line was dropped by the stream capture");
        for (const auto& record : expected) {
            checkLabelled(DiagnosticsLog::redact(record) == record, "stored measurement dropped: " + record.toUtf8());
        }
    }

    struct DecoderLine {
        dsd_neo_log_level_t level;
        const char* levelName;
        QByteArray text;
    };

    const DecoderLine decoderLines[] = {
        // args.c: the low input warning threshold, from the CLI or the environment.
        {LOG_LEVEL_INFO, "info", "NOTICE: Low input warning threshold: -50.0 dBFS"},
        {LOG_LEVEL_INFO, "info", "NOTICE: Low input warning threshold (env): -35.5 dBFS"},
        // rtl_sdr_fm.cpp and rtl_device.cpp: ppm requests, corrections and the replay resampler's ratio error.
        {LOG_LEVEL_INFO, "info", "AUTO-PPM: src=1 pwr=-25.3 dB snr=12.0 dB df=-120.5 Hz ppm -5->-3"},
        {LOG_LEVEL_INFO, "info",
         "IQ replay: resampling the 2048000 Hz capture to 1536000 Hz (L/M=3/4, -5.00 ppm, 32 taps per phase, group "
         "delay 0.50 ms)."},
        {LOG_LEVEL_INFO, "info", "PPM correction applied: -15."},
        {LOG_LEVEL_INFO, "info", "Requested RTL PPM Error Set to -5"},
        {LOG_LEVEL_INFO, "info",
         "Device does not support frequency (PPM) correction; configured ppm -25 will have no effect."},
        // channel_scan.c and trunk_scan.c: a row's squelch, -100..0 dB.
        {LOG_LEVEL_WARN, "warn",
         "WARNING: Scan channel 3 (851.012500 MHz): --squelch-db -50 cannot gate digital acquisition without a radio "
         "input; here it gates only the analog input monitor (-8) and the carrier activity it stamps."},
        {LOG_LEVEL_WARN, "warn",
         "WARNING: Trunk scan target 'site_1': --squelch-db -50 cannot gate digital acquisition without a radio input; "
         "here it gates only the analog input monitor (-8) and the carrier activity it stamps."},
    };
    auto& log = DiagnosticsLog::instance();
    for (const auto& line : decoderLines) {
        dsd_neo_log_write(line.level, "%s\n", line.text.constData());
        const auto ring = log.snapshot();
        const QString expected =
            QStringLiteral("[decoder] [%1] ").arg(QLatin1String(line.levelName)) + QString::fromUtf8(line.text) + ' ';
        checkLabelled(!ring.isEmpty() && ring.last() == expected, "decoder measurement dropped: " + line.text);
        checkLabelled(DiagnosticsLog::redact(expected) == expected, "stored decoder measurement dropped: " + line.text);
    }
}

/* G5: beside those measurements, and in the forms a key option could take, a key value still drops the record: an
 * integer with more digits than a measurement keeps, one followed by a unit that is itself hex ("dB"), by a value or by
 * nothing, a bracket that does not close on it, and a template line with anything added or a field that is not one. */
static void
negativeMeasurementsDoNotCarryKeys() {
    const QStringList records = {
        QStringLiteral("[stderr] [info] Tuner error set to -5 ppm. -2 1A2B"),
        QStringLiteral("[stderr] [info] Tuner error set to -5 1A2B ppm."),
        QStringLiteral("[stderr] [info] Tuner error set to -51A2B ppm."),
        QStringLiteral("[stderr] [info] Tuner error set to -5123 ppm."),
        QStringLiteral("[stderr] [info] Tuner error set to -912 ppm."),
        QStringLiteral("[stderr] [info] gain -2 dB"),
        QStringLiteral("[stderr] [info] gain -2dB"),
        QStringLiteral("[stderr] [info] threshold -5"),
        QStringLiteral("[stderr] [info] threshold -5 1A2B"),
        QStringLiteral("[stderr] [info] level -50.0 -9 12345"),
        QStringLiteral("[stderr] [info] args: -5 ppm -9 123"),
        QStringLiteral("[stderr] [info] TX Offset [-5, 1A2B]"),
        QStringLiteral("[stderr] [info] Offset: -5,1A2B"),
        QStringLiteral("[stderr] [info] Offset: -5-2 1A2B"),
        QStringLiteral("[stderr] [info] [-5] -2 1A2B"),
        QStringLiteral("[stderr] [info] Low input warning threshold: -50.0 dBFS key 1A2B"),
        QStringLiteral("[stderr] [info] Requested RTL PPM Error Set to -5"),
        QStringLiteral("[decoder] [info] Requested RTL PPM Error Set to -5 -2 1A2B\n"),
        QStringLiteral("[decoder] [info] Requested RTL PPM Error Set to -5123\n"),
        QStringLiteral(
            "[decoder] [info] Device does not support frequency (PPM) correction; configured ppm -21A2B will "
            "have no effect.\n"),
        QStringLiteral("[decoder] [warn] WARNING: Trunk scan target '-21A2B': --squelch-db -50 cannot gate digital "
                       "acquisition without a radio input; here it gates only the analog input monitor (-8) and the "
                       "carrier activity it stamps.\n"),
        QStringLiteral("[decoder] [warn] WARNING: Trunk scan target 'key1A2B': --squelch-db -50 cannot gate digital "
                       "acquisition without a radio input; here it gates only the analog input monitor (-8) and the "
                       "carrier activity it stamps.\n"),
        QStringLiteral("[decoder] [warn] WARNING: Scan channel 3 (851.012500 MHz): --squelch-db -5123 cannot gate "
                       "digital acquisition without a radio input; here it gates only the analog input monitor (-8) "
                       "and the carrier activity it stamps.\n"),
    };
    for (const auto& record : records) {
        checkLabelled(DiagnosticsLog::redact(record) == kDropped,
                      "key form beside a measurement kept: " + record.toUtf8());
    }
}

/* #782 gate r3 J2, J3 sweep: one realistic line per producer format that prints a signed measurement, each with the
 * negative values and every precision that producer prints. The producers were found by a format-string sweep of src/
 * and the Android native code (the gate r3 fix report lists them); each line here is rendered from the producer's own
 * format (file and function named beside it), stderr ones through the stream capture and runtime-log ones through the
 * log tap, and each must reach the log unchanged, live and from a saved record. */
static void
measurementCorpusSurvives() {
    const QString deg = QString::fromUtf8("\xC2\xB0");
    const QStringList stderrLines = {
        // rtl_device.cpp verbose_ppm_set(), the gain setters, and the SoapySDR gain step.
        QStringLiteral("Tuner error set to -5 ppm."),
        QStringLiteral("Tuner error set to -19 ppm."),
        QStringLiteral("Tuner error set to -150 ppm."),
        QStringLiteral("Tuner error set to -200 ppm."),
        QStringLiteral("Tuner gain set to -1.00 dB."),
        QStringLiteral("Tuner gain set to -9.90 dB."),
        QStringLiteral("Tuner manual gain (nearest): -9.9 dB."),
        QStringLiteral("Tuner manual gain (nearest): -5.4 dB."),
        QStringLiteral("SoapySDR: adjusted gain LNA from -12.00 dB to -10.00 dB."),
        QStringLiteral("SoapySDR: adjusted gain IF1 from -25.50 dB to -24.00 dB."),
        // dsd_dibit.c and dsd_frame_sync.c: the slicer and sync statistics.
        QStringLiteral("[SLICE-DECODE] d0:24.9% d1:25.1% d2:25.0% d3:25.0% avg:-0.12 range:[-2.95,2.91] (n=4800)"),
        QStringLiteral("[SLICER] d0:24.9% d1:25.1% d2:25.0% d3:25.0% avg:-0.12 range:[-25.31,24.80] (n=4800)"),
        QStringLiteral("[SYNC] range:[-2.9,2.9] dc:-0.05 ratio(1:3)=2400:2400"),
        QStringLiteral("[SYNC] range:[-15.3,14.8] dc:-0.25 ratio(1:3)=2400:2400"),
        // demod_pipeline.cpp: the CQPSK I/Q debug dump.
        QStringLiteral("[IQ] (0.52,-0.25|-25") + deg + QStringLiteral(",0.58) (-0.31,-1.92|-99") + deg
            + QStringLiteral(",1.94) "),
        // dsd_gps.c: LIP, the three Lat/Lon reports, NMEA, Harris and NXDN, with either degree glyph.
        QStringLiteral("Src: 1234; Lat: 32.12345") + deg + QStringLiteral("S Lon: 115.12345") + deg
            + QStringLiteral("W (-32.12345, -115.12345); Spd: 0 km/h; Dir: 0") + deg,
        QStringLiteral(" Lat: 32.12345 degS Lon: 115.12345 degW (-32.12345, -115.12345)"),
        QStringLiteral(" Lat: -32.12345") + deg + QStringLiteral("S Lon: -115.12345") + deg
            + QStringLiteral("W (-32.12345, -115.12345) "),
        QStringLiteral(" GPS: -32.123450") + deg + QStringLiteral(", -115.123450") + deg
            + QStringLiteral("; SPD: 0 knots; 0.000000 kph;"),
        QStringLiteral(" Harris GPS: -32.123456 deg, -115.123456 deg; HEADING: 090 deg; TIME: 12:00:00 UTC;"),
        QStringLiteral(" GPS: (-32.123456") + deg + QStringLiteral(", -115.123456") + deg
            + QStringLiteral(") Speed: 0.0 k/h; COG: 0.0; Elevation: -25; Date: 2026/10/28; Time: 12:00;"),
        // dmr_pdu.c dmr_lrrp_print_details() (J3) and the NMEA LOCN report.
        QStringLiteral(" Lat: -32.12345 Lon: -115.12345 (-32.12345, -115.12345)"),
        QStringLiteral(" Altitude: -25.50m"),
        QStringLiteral(" NMEA / LOCN; Source: 1234; 2026/10/28 12:00:00 (-32.12345") + deg
            + QStringLiteral(", -115.12345") + deg + QStringLiteral(");"),
        QStringLiteral(" NMEA / LOCN; Source: 1234; (-32.12345 deg, -115.12345 deg);"),
        // m17.c: power, the UDP result, GNSS.
        QStringLiteral(" PWR: -51.2 dB SQL HIT: 0;"),
        QStringLiteral(" UDP IP Frame CRC: BEEF; UDP RETURN: -1: LEN: 25; SENT: 61;"),
        QStringLiteral(" GPS: (-32.123450, -115.123450); Altitude: -25.5 m; Speed: 0.0 km/h; Bearing: 0 Degrees;"),
        // dmr_csbk.c: the broadcast local time and offset.
        QStringLiteral(" Date: 10.28; UTC Time: 02:15:00; Local: -3:15:00; Offset: -5;"),
        QStringLiteral(" UTC Time: 02:15:00; Local: -10:15:00; Offset: -12;"),
        // p25_lcw.c, p25p1_pdu_trunking.c, p25p2_vpdu.c: identifier offsets and the local time offset.
        QStringLiteral(" Channel Identifier Update; Iden: 1; BW: 5; TX Offset: -25; Spacing: 64; Base: 851006250;"),
        QStringLiteral(" Channel Identifier Update VU; Iden: 1; BW: 5; TX Offset: -255; Spacing: 64; Base: 851006250;"),
        QStringLiteral("  IDEN [1] Type [1] Base Freq [34040000] (170200000 Hz) TX Offset [-25] Spacing [100]"),
        QStringLiteral("  IDEN [1] Type [1] Base Freq [34040000] (170200000 Hz) TX Offset [-8191] Spacing [100]"),
        QStringLiteral("  Local Time Offset: -5.0 Hours;"),
        QStringLiteral("  Local Time Offset: -9.5 Hours;"),
        // tetra_trunk_sm.c and udp_audio.c/m17_udp.c: result codes.
        QStringLiteral("[TETRA SM] tune failed (result -1)"),
        QStringLiteral("[TETRA SM] return failed (result -22)"),
        QStringLiteral(" UDP Broadcast Set Error -1"),
        // config_schema.c dsdcfg_diags_print(), with config_user_validation.cpp's range warnings.
        QStringLiteral("/sdcard/dsd-neo.ini:12: warning [input] rtl_ppm: Value -1500 is out of range [-1000, 1000]"),
        QStringLiteral("/sdcard/dsd-neo.ini:14: warning [input] rtl_sql: Value -150 is out of range [-120, 0]"),
        QStringLiteral("/sdcard/dsd-neo.ini:15: warning [input] input_warn_db: Value -250.5 is out of range [-200, 0]"),
    };
    QTemporaryDir dir;
    {
        QByteArray bytes;
        QStringList expected;
        for (const auto& line : stderrLines) {
            bytes += line.toUtf8() + '\n';
            expected << QStringLiteral("[stderr] [info] ") + line;
        }
        const auto records = streamRecords(bytes, dir.path());
        check(records.size() == expected.size(), "a corpus line was lost or split by the stream capture");
        for (qsizetype i = 0; i < std::min(records.size(), expected.size()); ++i) {
            checkLabelled(records.at(i) == expected.at(i), "corpus line dropped: " + stderrLines.at(i).toUtf8());
            checkLabelled(DiagnosticsLog::redact(expected.at(i)) == expected.at(i),
                          "stored corpus line dropped: " + stderrLines.at(i).toUtf8());
        }
    }

    struct DecoderLine {
        dsd_neo_log_level_t level;
        const char* levelName;
        QByteArray text;
    };

    const DecoderLine decoderLines[] = {
        // rtl_device.cpp: the replay resampler.
        {LOG_LEVEL_INFO, "info",
         "IQ replay: resampling the 2048000 Hz capture to 1536000 Hz (L/M=3/4, -5.25 ppm, 32 taps per phase, group "
         "delay 0.50 ms)."},
        {LOG_LEVEL_INFO, "info",
         "IQ replay: resampling the 2400000 Hz capture to 1536000 Hz (L/M=16/25, -0.52 ppm, 24 taps per phase, group "
         "delay 0.31 ms)."},
        // rtl_sdr_fm.cpp: ppm corrections, the automatic ppm loop, the existing whole-line ppm and squelch lines.
        {LOG_LEVEL_INFO, "info", "PPM correction applied: -19 (reconfigured 851012500 Hz)."},
        {LOG_LEVEL_INFO, "info", "PPM correction applied: -150."},
        {LOG_LEVEL_INFO, "info", "AUTO-PPM: src=1 pwr=-25.3 dB snr=12.0 dB df=-120.5 Hz ppm -5->-19"},
        {LOG_LEVEL_INFO, "info", "AUTO-PPM: src=2 pwr=-51.0 dB snr=-1.5 dB df=-1520.0 Hz ppm -150->-200"},
        {LOG_LEVEL_INFO, "info", "Requested RTL PPM Error Set to -19"},
        {LOG_LEVEL_INFO, "info", "NOTICE: PPM correction request -150 failed (rc=-1); keeping applied value -19."},
        {LOG_LEVEL_INFO, "info",
         "Device does not support frequency (PPM) correction; configured ppm -150 will have no effect."},
        {LOG_LEVEL_INFO, "info",
         "Rate chain: capture=2048000 Hz resampled to 1536000 Hz (L/M=3/4, -5.25 ppm), base_decim=2, post=4 -> "
         "demod=48000 Hz; resampler bypassed -> output=48000 Hz."},
        // args.c: the low input threshold and the P25 timing options echoed as given.
        {LOG_LEVEL_INFO, "info", "NOTICE: Low input warning threshold: -50.0 dBFS"},
        {LOG_LEVEL_INFO, "info", "NOTICE: Low input warning threshold (env): -25.5 dBFS"},
        {LOG_LEVEL_INFO, "info", "NOTICE: P25: VC grace set to -1.50s (CLI)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: P25: Min follow dwell set to -2.00s (CLI)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: P25: Grant->Voice timeout set to -5.25s (CLI)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: P25: MAC hold set to -9.00s (CLI)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: P25: Force-release extra set to -1.25s (CLI)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: P25: Force-release margin set to -0.50s (CLI)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: P25p1: Error-hold threshold set to -12.5% (CLI)."},
        {LOG_LEVEL_INFO, "info", "NOTICE: P25p1: Error-hold seconds set to -2.50s (CLI)."},
        // channel_scan.c and trunk_scan.c: a row's squelch.
        {LOG_LEVEL_WARN, "warn",
         "WARNING: Scan channel 3 (851.012500 MHz): --squelch-db -100 cannot gate digital acquisition without a radio "
         "input; here it gates only the analog input monitor (-8) and the carrier activity it stamps."},
        // rtl_demod_config.cpp, logged by rtl_sdr_fm.cpp: a negative analog width refused.
        {LOG_LEVEL_ERROR, "error",
         "NFM bandwidth -12500 Hz is negative; set 0 for the 12.5 kHz default or a width from 4000 to 25000 Hz. The "
         "retune keeps the current receive profile."},
    };
    auto& log = DiagnosticsLog::instance();
    for (const auto& line : decoderLines) {
        dsd_neo_log_write(line.level, "%s\n", line.text.constData());
        const auto ring = log.snapshot();
        const QString expected =
            QStringLiteral("[decoder] [%1] ").arg(QLatin1String(line.levelName)) + QString::fromUtf8(line.text) + ' ';
        checkLabelled(!ring.isEmpty() && ring.last() == expected, "decoder corpus line dropped: " + line.text);
        checkLabelled(DiagnosticsLog::redact(expected) == expected, "stored decoder corpus line dropped: " + line.text);
    }
    const auto counts = DiagnosticsLog::measurementTemplateCountsForTest();
    check(counts.first == 44 && counts.second == counts.first,
          "a measurement template was refused or the list changed");
}

/* J2: a number leaves the key scan only as a labelled producer field, so the forms a command line takes still drop:
 * the reviewer's "args: -19 ppm" (-1 with RC4 key 9) and its units, punctuation and trailing text; the same with no
 * label; every argument echo of a negative-looking key, bare, labelled, JSON, list or shell-quoted; a key beside or
 * inside a measurement template; a template echoed on the command-line path; a field outside its producer's range or
 * precision; and a template's field context without its label. */
static void
measurementExemptionsKeepKeyForms() {
    QStringList records;
    const QStringList tails = {QStringLiteral(" ppm"),      QStringLiteral(" dBFS"),   QStringLiteral(" dBm"),
                               QStringLiteral(" dB"),       QStringLiteral(" Hz"),     QStringLiteral(" kHz"),
                               QStringLiteral(" MHz"),      QStringLiteral(" ppm."),   QStringLiteral(";"),
                               QStringLiteral(")"),         QStringLiteral(">"),       QStringLiteral("%"),
                               QStringLiteral("-"),         QStringLiteral("."),       QStringLiteral(" s"),
                               QStringLiteral(" ppm here"), QStringLiteral(" Hours;"), QStringLiteral(" m;"),
                               QStringLiteral(""),          QStringLiteral(".5 dB"),   QStringLiteral(".12345")};
    const QStringList contexts = {QString(),
                                  QStringLiteral("args: "),
                                  QStringLiteral("argv: "),
                                  QStringLiteral("cmd: "),
                                  QStringLiteral("command line: "),
                                  QStringLiteral("dsd-neo "),
                                  QStringLiteral("[")};
    for (const auto& context : contexts) {
        for (const auto& tail : tails) {
            records << context + QStringLiteral("-19") + tail;
            records << context + QStringLiteral("-55") + tail;
        }
    }
    const QStringList keyForms = {QStringLiteral("-1 9"),      QStringLiteral("-19"),    QStringLiteral("-5 12"),
                                  QStringLiteral("-512"),      QStringLiteral("-2 -5"),  QStringLiteral("-912"),
                                  QStringLiteral("-25 1A2B"),  QStringLiteral("-31A2B"), QStringLiteral("-81A2B"),
                                  QStringLiteral("-1 9 -5 12")};
    for (const auto& form : keyForms) {
        const QStringList parts = form.split(QLatin1Char(' '));
        QStringList quoted;
        QStringList single;
        for (const auto& part : parts) {
            quoted << QStringLiteral("\"") + part + QStringLiteral("\"");
            single << QStringLiteral("'") + part + QStringLiteral("'");
        }
        records << form << QStringLiteral("args: ") + form << QStringLiteral("argv: ") + form
                << QStringLiteral("cmd: ") + form << QStringLiteral("command line: ") + form
                << QStringLiteral("dsd-neo -i rtl ") + form
                << QStringLiteral("[") + quoted.join(QStringLiteral(", ")) + QStringLiteral("]")
                << QStringLiteral("[") + parts.join(QStringLiteral(", ")) + QStringLiteral("]")
                << single.join(QLatin1Char(' '))
                << QStringLiteral("{\"args\": [") + quoted.join(QLatin1Char(',')) + QStringLiteral("]}");
        // Beside or inside a measurement template, and a template echoed on the command-line path.
        records << QStringLiteral(" Lat: -32.12345 Lon: -115.12345 (-32.12345, -115.12345) ") + form
                << QStringLiteral(" PWR: -51.2 dB ") + form << QStringLiteral(" Offset: -5; ") + form
                << QStringLiteral(" UDP Broadcast Set Error ") + form
                << QStringLiteral("SoapySDR: adjusted gain ") + form + QStringLiteral(" from -12.00 dB to -10.00 dB.")
                << QStringLiteral("Src: 1; Lat: 32.12345 ") + form
                       + QStringLiteral(" Lon: 115.12345 degW (-32.12345, "
                                        "-115.12345)")
                << QStringLiteral("Tuner error set to -5 ppm. ") + form;
    }
    // A template on the command-line path, or with a field outside its producer's range or precision, or the field
    // context without the label.
    records << QStringLiteral("args: Tuner error set to -19 ppm.") << QStringLiteral("argv: Offset: -19;")
            << QStringLiteral("cmd: Lat: -32.12345 Lon: -115.12345 (-32.12345, -115.12345)")
            << QStringLiteral("command line: PWR: -51.2 dB") << QStringLiteral("\"-19\" Offset: -19;")
            << QStringLiteral("Tuner error set to -201 ppm.") << QStringLiteral("Tuner error set to -912 ppm.")
            << QStringLiteral("Tuner error set to -19 ppm") << QStringLiteral("  Local Time Offset: -5.00 Hours;")
            << QStringLiteral(" Lat: -32.1234 Lon: -115.12345 (-32.12345, -115.12345)")
            << QStringLiteral("  IDEN [1] TX Offset [-91234] Spacing [100]") << QStringLiteral(" [-25] Spacing [100]")
            << QStringLiteral(" (-32.12345, -115.12345)") << QStringLiteral(" Offset: -19")
            << QStringLiteral(" UDP Broadcast Set Error -1 9") << QStringLiteral(" UDP Broadcast Set Error -19 x")
            << QStringLiteral(" PWR: -51.2 dB key") << QStringLiteral(" TX Offset: -19; Spacing: 64; Base: 1; secret");
    for (const auto& record : records) {
        checkLabelled(DiagnosticsLog::redact(QStringLiteral("[stderr] [info] ") + record) == kDropped,
                      "key form kept beside the measurement rules: " + record.toUtf8());
        checkLabelled(DiagnosticsLog::redact(QStringLiteral("[decoder] [info] ") + record + '\n') == kDropped,
                      "key form kept beside the measurement rules (runtime log): " + record.toUtf8());
    }
    // Whole-line templates hold only for their own producer's source and level.
    for (const auto& record : {QStringLiteral("[decoder] [info] Tuner error set to -19 ppm.\n"),
                               QStringLiteral("[stderr] [info] PPM correction applied: -19."),
                               QStringLiteral("[host] [info] NOTICE: Low input warning threshold: -50.0 dBFS")}) {
        checkLabelled(DiagnosticsLog::redact(record) == kDropped,
                      "whole-line template from another producer kept: " + record.toUtf8());
    }
}

/* The decoder's frame-sync lines (printFrameSync(), print_dmr_sync_polarity()) survive in every polarity, as records and
 * through the stream capture, though some inverted labels read as a bundled key option ("-P25p1", "-YSF", "-TETRA").
 * Only the label itself is exempt, and only after "Sync: ": a key form beside it, or the same text elsewhere, drops. */
static void
syncLabelsSurvive() {
    const QStringList labels = {
        QStringLiteral("-DMR  "),        QStringLiteral("+DMR  "),        QStringLiteral("-M17 EOT"),
        QStringLiteral("-M17 STR"),      QStringLiteral("-dPMR "),        QStringLiteral("-EDACS"),
        QStringLiteral("-X2-TDMA "),     QStringLiteral("-NXDN96 "),      QStringLiteral("-P25p1 "),
        QStringLiteral("+P25p1"),        QStringLiteral("-P25p2"),        QStringLiteral("-YSF "),
        QStringLiteral("-TETRA NDB"),    QStringLiteral("-TETRA SB"),     QStringLiteral("-TETRA CB"),
        QStringLiteral("-DSTAR VOICE "), QStringLiteral("-DSTAR HEADER"), QStringLiteral("-PV   "),
        QStringLiteral("-PV_C "),        QStringLiteral("-M17 LSF"),
    };
    QByteArray bytes;
    QStringList expected;
    for (const auto& label : labels) {
        const QString line = QStringLiteral("12:34:56 Sync: ") + label + QStringLiteral(" mod: GFSK");
        const QString record = QStringLiteral("[stderr] [info] ") + line;
        checkLabelled(DiagnosticsLog::redact(record) == record, "sync line dropped: " + line.toUtf8());
        bytes += line.toUtf8() + '\n';
        expected << record;
    }
    QTemporaryDir dir;
    check(streamRecords(bytes, dir.path()) == expected, "sync lines dropped by the stream capture");

    for (const auto& record : {
             QStringLiteral("[stderr] [info] 12:34:56 Sync: -YSF -2 1A2B"),
             QStringLiteral("[stderr] [info] Sync: -P25p1 -ZT1F"),
             QStringLiteral("[stderr] [info] Sync: -TETRA-2 1A2B"),
             QStringLiteral("[stderr] [info] Sync: -P25p1x"),
             QStringLiteral("[stderr] [info] args: -P25p1"),
             QStringLiteral("[stderr] [info] -YSF"),
             QStringLiteral("[stderr] [info] Sync: -DMR  key 1A2B"),
         }) {
        checkLabelled(DiagnosticsLog::redact(record) == kDropped,
                      "key form beside a sync label kept: " + record.toUtf8());
    }
    QDir(dir.path()).removeRecursively();
    const auto records = streamRecords("Sync: -YSF -2\n1A2B\nSync: -YSF \n", dir.path());
    check(records == QStringList({kDropped, kDropped, QStringLiteral("[stderr] [info] Sync: -YSF ")}),
          "a key form beside a sync label, or its value, kept through the stream capture");
}

/* Records the Android app writes from Kotlin (#782), byte-identical to the golden records in
 * tests/android/LooperWatchdogTest.kt and ScreenTraceTest.kt (run_jvm_tests.py checks): a watchdog incident whose
 * stack names dispatchKeyEvent and AES, which the Kotlin side breaks up, and a screen change record. */
#define DIAGNOSTICS_GOLDEN_WATCHDOG                                                                                    \
    "watchdog: stall 1.1 of 3 at uptime 12.345 s: main thread blocked 1501 ms, state BLOCKED; 2 touches delivered "    \
    "since the probe; activity 2 resumed, window 3 attached; stack of 3 frames\n"                                      \
    "watchdog: stall 1.2 of 3: screen as of 40 ms before: mode dim_between_calls, delay 30 s, session 7, "             \
    "visible, focused, top, not multi-window, screen on, wants dim and keep on, engaged by nothing\n"                  \
    "watchdog: stall 1.3 of 3: main stack 1-3: android.view.ViewRootImpl.dispatchK*yEvent(ViewRootImpl.java:7012) "    \
    "| javax.crypto.A*S.init(Native Method) | android.os.Looper.loop(Looper.java:-1)"
#define DIAGNOSTICS_GOLDEN_SCREEN                                                                                      \
    "screen: change 1: dim on, brightness 0.010 requested, 0.010 read back; mode dim_between_calls, delay 30 s, "      \
    "session 7, visible, focused, top, not multi-window, screen on, wants dim and keep on, engaged by nothing; "       \
    "activity 2 resumed, window 3 attached"

/* The Android app's watchdog, screen trace and teardown records reach the log whole and within the record limit:
 * the key rule must not drop an incident for a framework method's name, so the Kotlin side breaks such words up. */
static void
androidHostRecordsSurvive() {
    QStringList records = QString::fromUtf8(DIAGNOSTICS_GOLDEN_WATCHDOG).split(QLatin1Char('\n'));
    records << QString::fromUtf8(DIAGNOSTICS_GOLDEN_SCREEN)
            << QStringLiteral("watchdog: stall 1.4: main thread resumed after 9200 ms; 3 touches delivered while it "
                              "waited, 2 of them by the capture")
            << QStringLiteral("screen: change 4 (recorded 400 ms late, replacing 2 changes not recorded): dim off, "
                              "brightness none requested, none read back")
            << QStringLiteral("screen: change 4: traversal after 100 ms, draw after 100 ms on window 2")
            << QStringLiteral("screen: change 1: no draw within 2000 ms; no traversal")
            << QStringLiteral("screen: touch while dimmed: passed on, dim still on; 2 more since, not recorded")
            << QStringLiteral("screen: touch while dimmed: not taken: the screen policy follows another activity")
            << QStringLiteral("diagnostics: 10 host records dropped while waiting for native diagnostics")
            << QStringLiteral("teardown: activity 2 destroying at uptime 1234.567 s")
            << QStringLiteral("teardown: stopService took 3 ms")
            << QStringLiteral("teardown: Qt event loop returned at uptime 1234.890 s")
            << QStringLiteral("teardown: ~RadioReferenceModel took 1012 ms")
            << QStringLiteral("teardown: main() returning at uptime 1236.010 s; flushing, at most 500 ms");
    check(records.size() == 16, "golden watchdog incident is not three records");
    for (const auto& text : records) {
        const QString record = QStringLiteral("[host] [info] ") + text;
        checkLabelled(DiagnosticsLog::redact(record) == record, "Android host record changed: " + text.toUtf8());
        checkLabelled(record.toUtf8().size() <= 512, "Android host record over 512 bytes: " + text.toUtf8());
    }
    // Unbroken, the same stack frame would take its whole record with it.
    check(DiagnosticsLog::redact(QStringLiteral("[host] [info] android.view.ViewRootImpl.dispatchKeyEvent"))
              == kDropped,
          "the key rule no longer drops dispatchKeyEvent; the Kotlin breaking-up can be revisited");
}

/* The Android teardown's final flush is reported by the next start from a marker file, once, and only when the
 * marker is well formed; the record survives redaction. */
static void
teardownFlushMarkerRoundTrip() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("teardown-flush.marker"));
    check(dsd_qt::teardownFlushMarkerTake(path).isEmpty(), "a missing flush marker reported something");
    check(dsd_qt::teardownFlushMarkerWrite(path, 37, dsd_qt::TeardownFlush::Completed),
          "the flush marker was not written");
    const QString completed = dsd_qt::teardownFlushMarkerTake(path);
    check(completed == QStringLiteral("teardown: the previous process's final diagnostics flush took 37 ms"),
          "the completed flush marker reads wrong");
    check(!QFile::exists(path) && dsd_qt::teardownFlushMarkerTake(path).isEmpty(), "the flush marker reported twice");
    check(dsd_qt::teardownFlushMarkerWrite(path, 500, dsd_qt::TeardownFlush::TimedOut),
          "the flush marker was not written");
    const QString timedOut = dsd_qt::teardownFlushMarkerTake(path);
    check(timedOut
              == QStringLiteral("teardown: the previous process's final diagnostics flush took 500 ms, and timed out"),
          "the timed-out flush marker reads wrong");
    // A flush that ended in its bound but could not write or sync is no timeout (#782).
    check(dsd_qt::teardownFlushMarkerWrite(path, 3, dsd_qt::TeardownFlush::Failed), "the flush marker was not written");
    const QString failed = dsd_qt::teardownFlushMarkerTake(path);
    check(failed
              == QStringLiteral(
                  "teardown: the previous process's final diagnostics flush took 3 ms, and failed to write"),
          "the failed flush marker reads wrong");
    for (const QString& record : {completed, timedOut, failed}) {
        const QString line = QStringLiteral("[host] [info] ") + record;
        check(DiagnosticsLog::redact(line) == line, "the flush marker record does not survive redaction");
    }
    for (const QByteArray& bad : {QByteArray("flush x completed"), QByteArray("flush 12"),
                                  QByteArray("junk 1 completed"), QByteArray("flush 12 maybe"), QByteArray()}) {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly) && file.write(bad) == bad.size(), "could not write a bad marker");
        file.close();
        check(dsd_qt::teardownFlushMarkerTake(path).isEmpty() && !QFile::exists(path),
              "a malformed flush marker was reported or kept");
    }
}

/* Only a whole producer record is exempt: anything around or inside it, or the same text from another producer,
 * keeps the key rule. */
static void
producerErrorLinesDoNotCarryKeys() {
    const QStringList records = {
        QStringLiteral("[stderr] [info] -b197 usb_claim_interface error -1"),
        QStringLiteral("[stderr] [info] usb_claim_interface error -1 -b197"),
        QStringLiteral("[stderr] [info] usb_claim_interface error -1-b197"),
        QStringLiteral("[stderr] [info] usb_claim_interface error -1\n-b197"),
        QStringLiteral("[stderr] [info] usb_claim_interface error -1 key"),
        QStringLiteral("[stderr] [info] usb_claim_interface error -91234"),
        QStringLiteral("[stderr] [info] usb_claim_interface error -9123"),
        QStringLiteral("[stderr] [info] usb_claim_interface error -1234"),
        QStringLiteral("[host] [info] engine run returned -1 -b197"),
        QStringLiteral("[host] [info] engine run returned -1234"),
        QStringLiteral("[stderr] [info] engine run returned -1"),
        QStringLiteral("[decoder] [error] libusb: error [op_open] failed with error -91234\n"),
        QStringLiteral("[stderr] [info] rtlsdr_i2c_write_reg failed with -1"),
        QStringLiteral("[host] [info] usb_claim_interface error -1"),
        QStringLiteral("[stderr] [error] usb_claim_interface error -1"),
        QStringLiteral("[decoder] [error] usb_claim_interface error -1\n"),
        QStringLiteral("[decoder] [error] Failed to apply RTL-SDR center frequency 465975000 Hz (rc=-1). -b197\n"),
        QStringLiteral("[decoder] [error] Failed to apply RTL-SDR center frequency -1A2B Hz (rc=-1).\n"),
        QStringLiteral("[decoder] [error] libusb: error [op_open] -b197 failed, errno=13\n"),
        QStringLiteral("[decoder] [error] libusb: error [op_open] key 1A2B failed with error -1\n"),
        QStringLiteral("[decoder] [error] libusb: error [op_open] failed -2 1A2B error -1\n"),
        QStringLiteral("[decoder] [error] libusb: error [op_open] failed\n-b197 error -1\n"),
        QStringLiteral("[decoder] [error] libusb: libusb: error [op_open] failed with error -1\n"),
        QStringLiteral("[decoder] [info] libusb: error [op_open] failed with error -1\n"),
        QStringLiteral("[stderr] [info] libusb: error [op_open] failed with error -1"),
    };
    for (const auto& record : records) {
        checkLabelled(DiagnosticsLog::redact(record) == kDropped, "mixed producer record kept: " + record.toUtf8());
    }
}

/* Every key-taking option of src/runtime/cli/args.c, in each form getopt and the long-option pass accept, and as a
 * host could echo an argument list. The values are short enough that the hex rule cannot hide a miss. */
static void
everyKeyOptionFormIsDropped() {
    const QStringList shortOptions = {"1", "2", "!", "@", "5", "9", "A", "S", "H", "R", "b"};
    const QStringList longOptions = {"--dmr-baofeng-pc5",   "--dmr-csi-ee72", "--tetra-key",   "--m17-aes-key",
                                     "--m17-scrambler-key", "--api-token",    "--rdio-api-key"};
    const QString first = QStringLiteral("1A2B");
    const QString second = QStringLiteral("3C4D");
    // String-valued options (API credentials) take any text, so their values need not look like keys.
    const QString word = QStringLiteral("s3cretValue");

    struct Form {
        const char* name;
        QString text;
    };

    QTemporaryDir dir;
    auto& log = DiagnosticsLog::instance();
    const auto checkForms = [&](const QString& option, const QList<Form>& forms) {
        for (const auto& form : forms) {
            const QByteArray label = "key form kept: " + option.toUtf8() + " " + form.name;
            for (const auto& context : {QString(), QStringLiteral("args: ")}) {
                checkLabelled(DiagnosticsLog::redact(QStringLiteral("[stderr] [info] ") + context + form.text)
                                  == kDropped,
                              label);
                checkLabelled(DiagnosticsLog::redact(QStringLiteral("[decoder] [error] ") + context + form.text + '\n')
                                  == kDropped,
                              label + " (runtime log)");
            }
            // The runtime log keeps a logical message whole, whatever lines it spans.
            dsd_neo_log_write(LOG_LEVEL_ERROR, "%s\n", form.text.toUtf8().constData());
            const auto ring = log.snapshot();
            checkLabelled(!ring.isEmpty() && ring.last() == kDropped, label + " (through the log tap)");
            // The stderr pump splits the stream into lines before redaction, so a value may continue on a later line.
            QDir(dir.path()).removeRecursively();
            const auto records = streamRecords(form.text.toUtf8() + '\n', dir.path());
            check(!records.isEmpty(), "stream capture produced no record");
            for (const auto& record : records) {
                checkLabelled(record == kDropped, label + " (through the stream capture)");
            }
        }
    };
    for (const auto& option : shortOptions) {
        const QString flag = QStringLiteral("-") + option;
        checkForms(flag, {
                             {"attached", flag + first},
                             {"separated", flag + ' ' + first},
                             {"quoted multi-word", flag + QStringLiteral(" '") + first + ' ' + second + '\''},
                             {"bundled", QStringLiteral("-ZT") + option + first},
                             {"bundled separated", QStringLiteral("-Z") + option + ' ' + first},
                             {"shell-quoted", '\'' + flag + QStringLiteral("' '") + first + '\''},
                             {"JSON list", QStringLiteral("[\"") + flag + QStringLiteral("\",\"") + first + "\"]"},
                             {"list", '[' + flag + QStringLiteral(", ") + first + ']'},
                             {"multiline", flag + ' ' + first + '\n' + second},
                             {"multiline CRLF", flag + ' ' + first + QStringLiteral("\r\n") + second},
                             {"value on the next line", flag + '\n' + first + ' ' + second},
                             {"text value on the next line", flag + '\n' + word},
                             {"text value split across lines", flag + ' ' + word + '\n' + word},
                             {"quoted value on the next line", flag + QStringLiteral("\n'my ") + word + " phrase'"},
                             {"quoted value split across lines",
                              flag + QStringLiteral(" 'my ") + word + QStringLiteral("\nphrase ") + word + '\''},
                         });
    }
    for (const auto& option : longOptions) {
        checkForms(option,
                   {
                       {"attached", option + '=' + first},
                       {"separated", option + ' ' + first},
                       {"quoted multi-word", option + QStringLiteral(" '") + first + ' ' + second + '\''},
                       {"list", '[' + option + QStringLiteral(", ") + first + ']'},
                       {"multiline", option + ' ' + first + '\n' + second},
                       {"value on the next line", option + '\n' + first + ' ' + second},
                       {"text value on the next line", option + '\n' + word},
                       {"text value attached and split", option + '=' + word + '\n' + word},
                       {"text value split across lines", option + ' ' + word + '\n' + word},
                       {"quoted value on the next line", option + QStringLiteral("\n\"my ") + word + " phrase\""},
                       {"quoted value split across lines",
                        option + QStringLiteral(" 'my ") + word + QStringLiteral("\nphrase ") + word + '\''},
                   });
    }
}

/* The stream capture keeps the pump's line rules, and the continuation rule ends at the first line that is not a
 * key value. */
static void
streamCaptureLineRules() {
    QTemporaryDir dir;
    QByteArray bytes = "first\r\nsecond\n\nthird\r";
    bytes += QByteArray(1100, 'x') + "\n" + "3C4D\n";
    bytes += "-2 1A2B\n3C4D 5E6F\n0x 11, 22\n\x1b[1m3C4D\x1b[0m\tAB\nTuner gain set to 30 dB.\n5E6F\n";
    // A quote opened in a dropped record runs on to its close, but no further than one record's bound.
    bytes += "--api-token 'never closed\n" + QByteArray(600, 'w').replace(100, 1, " ") + "\n"
             + QByteArray(600, 'v').replace(100, 1, " ") + "\nSampling at 1536000 S/s.\n";
    // A line inside that quote dropped for its own sake (" -1") is still the value: the quote runs on through it.
    bytes += "--api-token 'alpha\nbeta -1 gamma\nfinal words'\nTuner gain set to 30 dB.\n";
    // An apostrophe inside a word opens no quote.
    bytes += "Invalid key: don't\nTuner gain set to 30 dB.\nlast";
    const auto records = streamRecords(bytes, dir.path());
    const QStringList expected = {
        QStringLiteral("[stderr] [info] first"),
        QStringLiteral("[stderr] [info] second"),
        QStringLiteral("[stderr] [info] third"),
        QStringLiteral("[stderr] [info] [oversized diagnostic omitted]"),
        kDropped,
        kDropped,
        kDropped,
        kDropped,
        kDropped,
        QStringLiteral("[stderr] [info] Tuner gain set to 30 dB."),
        QStringLiteral("[stderr] [info] 5E6F"),
        kDropped,
        kDropped,
        kDropped,
        QStringLiteral("[stderr] [info] Sampling at 1536000 S/s."),
        kDropped,
        kDropped,
        kDropped,
        QStringLiteral("[stderr] [info] Tuner gain set to 30 dB."),
        kDropped,
        QStringLiteral("[stderr] [info] Tuner gain set to 30 dB."),
        QStringLiteral("[stderr] [info] last"),
    };
    check(records == expected, "stream capture line rules changed");
}

/* An oversized line is omitted unread, but a quote it opens or closes is followed through it: the rest of a quoted key
 * value on the next line drops, and a quote closed inside it no longer holds the lines after. */
static void
oversizedLineKeepsItsQuote() {
    const QString omitted = QStringLiteral("[stderr] [info] [oversized diagnostic omitted]");
    const QString kept = QStringLiteral("[stderr] [info] w2 w3");
    const QString after = QStringLiteral("[stderr] [info] Tuner gain set to 30 dB.");

    struct Case {
        const char* name;
        QByteArray bytes;
        QStringList expected;
    };

    const Case cases[] = {
        {"quote opened in the cleared bytes",
         "--api-token 'w1 " + QByteArray(1100, 'w') + "\nw2 w3'\nTuner gain set to 30 dB.\n",
         {omitted, kDropped, after}},
        {"quote opened in the skipped bytes",
         QByteArray(1100, 'x') + " --api-token 'w1 abc\nw2 w3'\nTuner gain set to 30 dB.\n",
         {omitted, kDropped, after}},
        {"quote closed in the skipped bytes",
         "--api-token 'w1 " + QByteArray(1100, 'w') + "' tail\nw2 w3\n",
         {omitted, kept}},
        {"carried quote closed inside it",
         "--api-token 'alpha\n" + QByteArray(1100, 'y') + "' end\nw2 w3\n",
         {kDropped, omitted, kept}},
        {"carried quote still open after it",
         "--api-token 'alpha\n" + QByteArray(1100, 'y') + "\nw2 w3'\nTuner gain set to 30 dB.\n",
         {kDropped, omitted, kDropped, after}},
    };
    for (const auto& c : cases) {
        QTemporaryDir dir;
        checkLabelled(streamRecords(c.bytes, dir.path()) == c.expected, QByteArray("oversized line quote: ") + c.name);
    }
}

// --- Host records (issue #782) ---

// The host section of an export: the rows between its heading and the full log's.
static QStringList
hostSection(const QString& text) {
    const auto rows = text.split('\n');
    const auto end = rows.indexOf(QLatin1String(kAllHeading));
    if (rows.value(0) != QLatin1String(kHostHeading) || end < 1) {
        return {};
    }
    return rows.mid(1, end - 1);
}

// A fast scan's decoder output pushed every host and session line out of the 2000-line ring and the saved tail, so the
// export that should have explained a failure had none of them, live or after a restart.
static void
hostRecordsOutliveABusySession() {
    QTemporaryDir dir;
    g_clock = 0;
    QStringList expected;
    for (int i = 0; i < 10; ++i) {
        expected.append(QStringLiteral("[host] [info] service: lifecycle step %1").arg(i));
    }
    // A session record the ring does not pin at its front.
    expected.append(QStringLiteral("[session] [info] usb access requested: usb, boot 3"));
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        DiagnosticsLogModel model(&log);
        for (int i = 0; i < 10; ++i) {
            log.submit("host", "info", QStringLiteral("service: lifecycle step %1").arg(i));
        }
        log.noteSessionAttempt(QStringLiteral("usb, boot 3"), DiagnosticsLog::AttemptStage::UsbAccess);
        for (int i = 0; i < 3000; ++i) {
            log.submit("decoder", "info",
                       QStringLiteral("Sync: +P25p1 hop %1 ").arg(i) + QString(150, QLatin1Char('q')));
            if (i % 100 == 99) {
                g_clock += DiagnosticsLog::kWriteIntervalMs;
                log.pump();
            }
        }
        check(!log.snapshot().join('\n').contains(QStringLiteral("lifecycle step 9")),
              "the fixture did not push the host records out of the ring");
        const auto live = hostSection(model.allText());
        for (const auto& line : expected) {
            check(live.contains(line), "a host record is missing from the live export's host section");
        }
        check(model.allText().endsWith(log.snapshot().join('\n')), "the full log does not follow the host records");
        // Share redacts the export line by line again.
        for (const char* heading : {kHostHeading, kAllHeading}) {
            check(DiagnosticsLog::redact(QLatin1String(heading)) == QLatin1String(heading),
                  "Share's redaction drops an export heading");
        }
        check(log.flush(), "busy session flush");
        check(log.ioStats().rotations >= 2, "the fixture did not rotate the main tail");
    }
    check(!(readFile(dir.filePath("tail.prev.log")) + readFile(dir.filePath("tail.log"))).contains("lifecycle step 9"),
          "the fixture did not push the host records out of the main tail");
    DiagnosticsLog reloaded(dir.path(), manualOptions());
    DiagnosticsLogModel model(&reloaded);
    model.refresh();
    const auto previous = hostSection(model.allText());
    check(previous.value(0) == QLatin1String("--- previous run ---"), "the previous run's host records are not marked");
    for (const auto& line : expected) {
        check(previous.contains(line), "a host record is missing from the previous-run export");
    }
}

// The host store keeps the main tail's rules at its own sizes: 500 records of at most 512 bytes, redacted, in two
// segments of at most 32 KiB that each start with their process and session lines.
static void
hostStoreKeepsItsBounds() {
    QTemporaryDir dir;
    g_clock = 0;
    const QString secret = QString(16, QLatin1Char('A')) + QString(16, QLatin1Char('B'));
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        log.noteProcess(QStringLiteral("1, test os, boot 7"));
        log.noteSessionAttempt(QStringLiteral("rtltcp"));
        log.submit("host", "info", QString(600, QChar(0x03bb)));
        log.submit("host", "error", "Invalid key: " + secret);
        auto rows = log.hostSnapshot();
        check(rows.size() == 4 && rows.at(2).toUtf8().size() <= 512
                  && !rows.at(2).contains(QChar::ReplacementCharacter),
              "a host record over 512 bytes");
        check(rows.value(3) == QLatin1String(kDropped), "a key-bearing host record was kept");
        for (int i = 0; i < 700; ++i) {
            log.submit("session", "info", QStringLiteral("seq %1 ").arg(i) + QString(300, QLatin1Char('h')));
            if (i % 50 == 49) {
                g_clock += DiagnosticsLog::kWriteIntervalMs;
                log.pump();
            }
        }
        rows = log.hostSnapshot();
        check(rows.size() == DiagnosticsLog::kHostRingLimit && rows.first().contains(QStringLiteral("seq 200 "))
                  && rows.last().contains(QStringLiteral("seq 699 ")),
              "the host ring is not the newest 500 records");
        check(log.flush(), "host store flush");
    }
    for (const char* name : {"host.log", "host.prev.log"}) {
        const auto bytes = readFile(dir.filePath(QLatin1String(name)));
        const auto lines = bytes.split('\n');
        check(bytes.size() <= DiagnosticsLog::kHostSegmentLimit, "a host segment over its cap");
        check(lines.size() > 2 && lines.at(0).startsWith("[process] [info] pid 1, test os, boot 7")
                  && lines.at(1).contains("--- session starting: rtltcp ---"),
              "a host segment does not start with its process and session");
        checkAbsent(bytes, hexRepresentations(secret.toUtf8()), "a host segment contains a key representation");
    }
    DiagnosticsLog reloaded(dir.path(), manualOptions());
    const auto rows = reloaded.hostSnapshot();
    check(rows.size() <= DiagnosticsLog::kHostRingLimit && rows.value(0) == QLatin1String("--- previous run ---")
              && rows.last().contains(QStringLiteral("seq 699 ")),
          "the reloaded host records lost the newest");
    // The current segment's header repeats the process and session lines the previous segment holds.
    check(rows.join('\n').count(QStringLiteral("pid 1, test os, boot 7")) == 1
              && rows.join('\n').count(QStringLiteral("--- session starting: rtltcp ---")) == 1,
          "a segment header was reloaded as a record");
    int last = -1;
    static const QRegularExpression seq(QStringLiteral("seq (\\d+) "));
    for (const auto& row : rows) {
        const auto match = seq.match(row);
        if (match.hasMatch()) {
            check(match.captured(1).toInt() > last, "reloaded host records out of order");
            last = match.captured(1).toInt();
        }
    }
}

// A segment untouched for seven days expires, a kill's temporaries go, and a reloaded record is redacted again.
static void
hostStoreExpiryAndReload() {
    QTemporaryDir dir;
    writeFile(dir.filePath("host.log.AbC123"), "orphan");
    writeFile(dir.filePath("host.prev.log"), "[host] [info] stale host record\n");
    QFile stale(dir.filePath("host.prev.log"));
    check(stale.open(QIODevice::ReadWrite)
          && stale.setFileTime(QDateTime::currentDateTimeUtc().addDays(-8), QFileDevice::FileModificationTime));
    stale.close();
    writeFile(dir.filePath("host.log"), "[host] [info] fresh host record\n[host] [info] -1 1A2B\n");
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    check(!QFile::exists(dir.filePath("host.log.AbC123")), "an orphaned host temporary kept");
    check(!QFile::exists(dir.filePath("host.prev.log")), "an expired host segment kept");
    const QStringList expected = {QStringLiteral("--- previous run ---"),
                                  QStringLiteral("[host] [info] fresh host record"), QLatin1String(kDropped)};
    check(log.hostSnapshot() == expected, "the saved host records did not reload, redacted");
}

// Clear removes the host records with the rest: from the ring, the queue and both host segments.
static void
clearRemovesTheHostRecords() {
    QTemporaryDir dir;
    g_clock = 0;
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        DiagnosticsLogModel model(&log);
        log.submit("host", "info", "cleared host record");
        check(log.flush(), "pre-clear flush");
        check(readFile(dir.filePath("host.log")).contains("cleared host record"), "the host record was not saved");
        log.submit("session", "info", "queued host record");
        model.clear();
        check(log.hostSnapshot().isEmpty() && !model.allText().contains("cleared host record"),
              "Clear kept the host records");
        log.submit("host", "info", "kept host record");
        check(log.flush(), "post-clear flush");
        const auto saved = readFile(dir.filePath("host.prev.log")) + readFile(dir.filePath("host.log"));
        check(!saved.contains("cleared host record") && !saved.contains("queued host record")
                  && saved.contains("kept host record"),
              "Clear did not remove the saved host records");
    }
    DiagnosticsLog reopened(dir.path(), manualOptions());
    const auto text = exportOf(reopened);
    check(!text.contains("cleared host record") && !text.contains("queued host record")
              && text.contains("kept host record"),
          "cleared host records came back after a restart");
}

// A slow load of the saved records must not hold up a host record from another thread: Android's main thread submits
// them through nativeHostDiagnostic, and the tap's install used to load under the lock that guards them.
static std::mutex g_loadMutex;
static std::condition_variable g_loadChanged;
static bool g_loadEntered = false;
static bool g_loadReleased = false;
static bool g_loadDone = false;
static std::thread::id g_loadThread;

static void
blockedLoad() {
    std::unique_lock<std::mutex> lock(g_loadMutex);
    g_loadEntered = true;
    g_loadThread = std::this_thread::get_id();
    g_loadChanged.notify_all();
    // Bounded: where the submit below waits for the load, the load ends after this and the check reports it.
    g_loadChanged.wait_for(lock, std::chrono::seconds(3), [] { return g_loadReleased; });
    g_loadDone = true;
}

// Re-review N2: the platform's lines after the process line (Android: the exit records) are reported durable, so the
// platform marks them logged, only once they are synced to the host log: on the build thread, after the publish.
static const char kPlatformLine[] = "exit: platform line that must be durable first";
static QString g_platformHostLog;
static int g_platformLogged = 0;
static bool g_platformLineOnDisk = false;
static bool g_platformLoggedAfterPublish = false;

static std::pair<QString, QStringList>
platformFacts() {
    return {QString(), {QString::fromLatin1(kPlatformLine)}};
}

static void
platformLinesLogged() {
    QFile host(g_platformHostLog);
    const bool onDisk = host.open(QIODevice::ReadOnly) && host.readAll().contains(kPlatformLine);
    const bool published = DiagnosticsLog::installedForTest();
    std::lock_guard<std::mutex> lock(g_loadMutex);
    g_platformLineOnDisk = onDisk;
    g_platformLoggedAfterPublish = published;
    ++g_platformLogged;
    g_loadChanged.notify_all();
}

// installTap() runs on Android's main thread (the service's nativeInit), as do the host records, the startup-window
// arming and the teardown flush. None of them waits for the log's construction: the tap returns at once, the records
// and the arming are held and applied at the publish, and a flush waits for the publish only within its own bound.
static void
installTapDoesNotWaitForTheLoad() {
    DiagnosticsLog::setLoadHookForTest(blockedLoad);
    g_platformHostLog =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/diagnostics/host.log");
    DiagnosticsLog::setPlatformFactsProvider(platformFacts, platformLinesLogged);
    DiagnosticsLog::installTap();
    {
        std::lock_guard<std::mutex> lock(g_loadMutex);
        check(!g_loadDone, "installTap() waited for the saved records to load");
    }
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(g_loadMutex);
        entered = g_loadChanged.wait_for(lock, std::chrono::seconds(10), [] { return g_loadEntered; });
    }
    check(entered, "the log's load did not start");
    check(!DiagnosticsLog::installedForTest(), "the log was published before its load finished");
    {
        // On Android QStandardPaths' cache has no lock, and the Qt thread resolves its own paths meanwhile: the build
        // thread must not resolve any.
        std::lock_guard<std::mutex> lock(g_loadMutex);
        check(g_loadThread != std::this_thread::get_id()
                  && DiagnosticsLog::directoryResolvedOnForTest() == std::this_thread::get_id(),
              "the log's directory was not resolved on installTap()'s calling thread");
    }
    DiagnosticsLog::submitHostDiagnostic(QStringLiteral("usb: reported during the load"));
    DiagnosticsLog::armStartupDiagnostics();
    dsd_neo_log_write(LOG_LEVEL_INFO, "decoder line during the load\n");
    // Held records keep the producer-line exemptions: they are formatted with their source and level, as submitted.
    DiagnosticsLog::submitHostDiagnostic(QStringLiteral("service: engine configuration returned -2"));
    DiagnosticsLog::submitHostDiagnostic(QStringLiteral("engine run returned -1"));
    dsd_neo_log_write(LOG_LEVEL_INFO, "NOTICE: PPM correction request -12 failed (rc=-1); keeping applied value -2.\n");
    dsd_neo_log_write(LOG_LEVEL_ERROR, "libusb: error [handle_events] backend handle_events failed with error -1\n");
    DiagnosticsLog::submitHostDiagnostic(QStringLiteral("service: args -2 1A2B"));
    dsd_neo_log_write(LOG_LEVEL_ERROR, "engine run returned -1 -H 1A2B\n");
    // A thread that writes to the log directly (the stderr pump) waits for its construction: it must then write
    // after this run's process line and the records held for it.
    bool publishedFirst = false;
    std::thread direct([&publishedFirst] {
        auto& log = DiagnosticsLog::instance();
        publishedFirst = DiagnosticsLog::installedForTest();
        log.submit("stderr", "info", "direct line during the load");
    });
    check(!DiagnosticsLog::flushDiagnostics(50), "a flush reported durable before the log was published");
    check(DiagnosticsLog::flushDiagnosticsOutcome(50) == DiagnosticsLog::FlushOutcome::TimedOut,
          "a flush the publish outran is a timeout");
    // #782: the quit's flush waits its whole bound, unless a final flush follows the teardown (Android's main()):
    // then it only asks for one, so the teardown holds one bounded wait.
    {
        const auto timed = [] {
            const uint64_t start = dsd_realtime_mono_ms();
            DiagnosticsLog::flushAtQuit();
            return dsd_realtime_mono_ms() - start;
        };
        check(DiagnosticsLog::quitFlushBoundMs() == DiagnosticsLog::kShutdownFlushMs
                  && timed() + 50 >= static_cast<uint64_t>(DiagnosticsLog::kShutdownFlushMs),
              "the quit's own flush did not wait for its bound");
        // Its bound, not the time it took: a test thread a loaded runner preempts would make any ceiling on the
        // elapsed time fail (#782 gate r2 H5). A bound of 0 asks for the flush and returns, the log being unpublished.
        DiagnosticsLog::setFinalFlushAfterQuit(true);
        check(DiagnosticsLog::quitFlushBoundMs() == 0,
              "the quit's flush waits though a final flush follows the teardown");
        DiagnosticsLog::flushAtQuit();
        DiagnosticsLog::setFinalFlushAfterQuit(false);
    }
    // A flush asked for before the publish is honoured once it happens, within its bound.
    bool flushed = false;
    std::thread flusher([&flushed] { flushed = DiagnosticsLog::flushDiagnostics(10000); });
    {
        std::lock_guard<std::mutex> lock(g_loadMutex);
        check(!g_loadDone, "a host record, startup-window request or flush waited for the saved records to load");
        check(g_platformLogged == 0, "the platform was told its lines are durable while the log was loading");
        g_loadReleased = true;
    }
    g_loadChanged.notify_all();
    flusher.join();
    direct.join();
    check(flushed, "a flush asked for during the load was not honoured");
    {
        std::unique_lock<std::mutex> lock(g_loadMutex);
        const bool logged = g_loadChanged.wait_for(lock, std::chrono::seconds(15), [] { return g_platformLogged > 0; });
        check(logged && g_platformLogged == 1, "the platform was not told once that its lines are durable");
        check(g_platformLoggedAfterPublish, "the platform was told its lines are durable before the publish");
        check(g_platformLineOnDisk, "the platform was told its lines are durable before they were in the host log");
    }
    check(publishedFirst, "a direct writer got the log before it was published");
    DiagnosticsLog::setLoadHookForTest(nullptr);
    check(DiagnosticsLog::installedForTest(), "the log was not published");
    auto& log = DiagnosticsLog::instance();
    const auto ring = log.snapshot().join('\n');
    const auto host = log.hostSnapshot().join('\n');
    check(ring.count(QStringLiteral("usb: reported during the load")) == 1
              && host.count(QStringLiteral("usb: reported during the load")) == 1,
          "a host record that arrived during the load was not kept once");
    check(ring.count(QStringLiteral("[decoder] [info] decoder line during the load")) == 1
              && !host.contains(QStringLiteral("decoder line during the load")),
          "a decoder record that arrived during the load was not kept once");
    check(log.startupWindowOpenForTest(), "a startup-window request made during the load was lost");
    check(host.count(QString::fromLatin1(kPlatformLine)) == 1, "the platform's line is not in the host records once");
    const QStringList verbatim = {
        QStringLiteral("[host] [info] service: engine configuration returned -2"),
        QStringLiteral("[host] [info] engine run returned -1"),
        QStringLiteral(
            "[decoder] [info] NOTICE: PPM correction request -12 failed (rc=-1); keeping applied value -2. "),
        QStringLiteral("[decoder] [error] libusb: error [handle_events] backend handle_events failed with error -1 "),
    };
    const auto rows = log.snapshot();
    for (const auto& line : verbatim) {
        check(rows.contains(line), "a producer line held during the load was not kept verbatim");
    }
    check(!ring.contains(QStringLiteral("1A2B")) && !host.contains(QStringLiteral("1A2B"))
              && ring.count(QLatin1String(kDropped)) >= 2,
          "a key-bearing record held during the load was kept");
    qsizetype process = -1;
    for (qsizetype i = 0; i < rows.size() && process < 0; ++i) {
        if (rows.at(i).startsWith(QStringLiteral("[process] [info] pid "))) {
            process = i;
        }
    }
    const auto heldAt = rows.indexOf(QStringLiteral("[host] [info] usb: reported during the load"));
    const auto directAt = rows.indexOf(QStringLiteral("[stderr] [info] direct line during the load"));
    check(process >= 0 && process < heldAt && heldAt < directAt,
          "a record written during the build came before this run's process line or the held records");
}

// A host pair that cannot rotate notes it where the host records are read, so decoder output cannot evict the note.
static void
hostRotationFailureIsNoted() {
    QTemporaryDir dir;
    check(QDir(dir.path()).mkpath(QStringLiteral("host.prev.log/inner")), "fixture directory");
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    for (int i = 0; i < 400; ++i) {
        log.submit("host", "info", QStringLiteral("seq %1 ").arg(i) + QString(100, QLatin1Char('r')));
    }
    log.flush();
    for (int i = 0; i < 2100; ++i) {
        log.submit("decoder", "info", QStringLiteral("decoder %1").arg(i));
    }
    check(QFileInfo(dir.filePath("host.log")).size() <= DiagnosticsLog::kHostSegmentLimit, "host segment past its cap");
    check(log.hostSnapshot().join('\n').count(QStringLiteral("saved host and session log could not be rotated")) == 1,
          "a host rotation failure is not in the host records");
}

int
main(int argc, char** argv) {
    QTemporaryDir earlyDir;
    qputenv("XDG_DATA_HOME", earlyDir.path().toUtf8());
    dsd_neo_log_set_sink(DSD_NEO_LOG_SINK_STDERR);
    // USB status may arrive before Qt establishes its app-data location and
    // before nativeInit selects the platform sink or starts the stderr pump.
    DiagnosticsLog::submitHostDiagnostic(QStringLiteral("usb: permission pending before init"));
    QGuiApplication app(argc, argv);
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.path().toUtf8());
    installTapDoesNotWaitForTheLoad();
    earlyHostCaptureAndBridge();
    clearWithConcurrentSubmit();
    inactiveModelDefersResets();
    clearAndReopen();
    test_live_formatted_keys();
    realKeySources();
    producerErrorLinesSurvive();
    producerErrorLinesDoNotCarryKeys();
    negativeDecimalsSurvive();
    negativeMeasurementsSurvive();
    negativeMeasurementsDoNotCarryKeys();
    measurementCorpusSurvives();
    measurementExemptionsKeepKeyForms();
    syncLabelsSurvive();
    androidHostRecordsSurvive();
    teardownFlushMarkerRoundTrip();
    everyKeyOptionFormIsDropped();
    streamCaptureLineRules();
    oversizedLineKeepsItsQuote();
    appendKeepsTheFile();
    writeVolumeIsTheLogItself();
    rotationKeepsTheBound();
    legacyTailIsSplit();
    crashFragmentIsDropped();
    crashZeroRunIsDropped();
    flushReportsFailures();
    platformLinesReportedOnlyWhenDurable();
    syncCadenceAndStartupWindow();
    segmentsStartWithTheirContext();
    contextLineWrittenOnce();
    oneMarkerPerAttempt();
    failingSyncBacksOff();
    clearForgetsTheCleared();
    pinnedContextOutlivesTheRing();
    interruptedLegacySplit();
    failedLegacySplitKeepsTheTail();
    lostSegmentDoesNotSpin();
    pinsFollowTheCurrentSession();
    pinnedSessionBelongsToThePinnedProcess();
    identicalSessionLinesAcrossRuns();
    reloadCutoffKeepsBothContextLines();
    identicalSessionLinesPinOnce();
    previousRunBootFact();
    contextSurvivesTheRingLimit();
    unicodeDirectory();
    orphansAndExpiry();
    rotationFailureKeepsTheBound();
    hostRecordsOutliveABusySession();
    hostStoreKeepsItsBounds();
    hostStoreExpiryAndReload();
    clearRemovesTheHostRecords();
    hostRotationFailureIsNoted();
    {
        DiagnosticsLog log(dir.path());
        DiagnosticsLogModel model(&log);
        log.submit("tap", "info", "decoder record");
        log.submit("stderr", "info", "stderr record");
        log.submit("host", "info", "host record");
        model.refresh();
        check(model.rowCount() == 3);
        model.setPaused(true);
        log.submit("host", "info", "pending");
        model.refresh();
        check(model.rowCount() == 3 && model.pendingCount() == 1);
        model.setPaused(false);
        check(model.rowCount() == 4 && model.pendingCount() == 0);
        const QString secret = QString(16, QLatin1Char('A')) + QString(16, QLatin1Char('B'));
        log.submit("stderr", "error", "Invalid key: " + secret + "malformed");
        log.submit("tap", "info", secret);
        checkStoredDiagnostics(log, model, dir.path(), hexRepresentations(secret.toUtf8()), "decoder record");
    }
    {
        DiagnosticsLog log(dir.path());
        DiagnosticsLogModel model(&log);
        model.refresh();
        const auto previous = model.allText();
        check(previous.contains("previous run") && previous.contains("decoder record")
              && previous.contains("stderr record") && previous.contains("host record"));
        model.clear();
        std::vector<std::thread> threads;
        threads.reserve(4);
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                for (int j = 0; j < 600; ++j) {
                    log.submit("host", "info", QString(600, QLatin1Char('x')));
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        model.refresh();
        check(model.rowCount() == 2000);
        for (const auto& line : model.allText().split('\n')) {
            check(line.toUtf8().size() <= 512);
        }
        log.flush();
        QFile tail(dir.filePath("tail.log"));
        check(tail.size() <= 256LL * 1024);
        model.clear();
        check(model.rowCount() == 0);
        log.submit("host", "info", QString(600, QChar(0x03bb)));
        model.refresh();
        for (const auto& rows : {log.snapshot(), log.hostSnapshot()}) {
            check(rows.size() == 1 && rows.first().toUtf8().size() <= 512
                  && !rows.first().contains(QChar::ReplacementCharacter));
        }
    }
    QFile expired(dir.filePath("tail.log"));
    check(expired.open(QIODevice::ReadWrite));
    check(expired.setFileTime(QDateTime::currentDateTimeUtc().addDays(-8), QFileDevice::FileModificationTime));
    expired.close();
    {
        DiagnosticsLog log(dir.path());
        check(log.snapshot().isEmpty());
        check(!QFile::exists(dir.filePath("tail.log")));
    }
    return failures ? 1 : 0;
}
