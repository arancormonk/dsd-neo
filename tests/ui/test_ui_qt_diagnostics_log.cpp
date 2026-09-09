// SPDX-License-Identifier: GPL-3.0-or-later
#include <QAbstractListModel>
#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QList>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <Qt>
#include <QtEnvironmentVariables>
#include <QtGlobal>
#include <cstdio>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/log.h>
#include <string.h>
#include <thread>
#include <vector>
#include "command_bridge.h"
#include "diagnostics_log.h"
extern "C" int dsd_test_diagnostics_drain(dsd_opts*, dsd_state*);
#include "../../src/app_control/commands_internal.h"
#include "session_args.h"
extern "C" int dsd_test_diagnostics_direct_key_applied(dsd_opts*, dsd_state*, int, const char*);
#include <memory>
#include "test_support.h"
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

static void
checkAbsent(const QByteArray& text, const QList<QByteArray>& representations, const char* surface) {
    const auto folded = text.toLower();
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
checkHexState(const dsd_opts* opts, const dsd_state* state, const QByteArray& key) {
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
    check(opts->dmr_mute_encL == 0 && opts->dmr_mute_encR == 0 && opts->unmute_encrypted_p25 == 0,
          "encrypted audio flags mismatch");
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
    check(text == ring, "allText does not contain the captured ring");
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
    // key types on both paths. The keyring survives every direct overlay.
    for (int path = 0; path < 2; ++path) {
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
            opts->dmr_mute_encL = opts->dmr_mute_encR = opts->unmute_encrypted_p25 = 1;
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
            checkHexState(opts.get(), state.get(), activeHex);
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
                dsd_key_set_free(&before);
                dsd_key_set_free(&after);
                checkAbsent(captured, representations, "CLI error diagnostic contains a key representation");
                log.submit("stderr", "info", QString::fromUtf8(captured));
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
              && model.allText().count("--- session starting ---") == 1,
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

static void
earlyHostCaptureAndBridge() {
    {
        auto& log = DiagnosticsLog::instance();
        DiagnosticsLogModel model(&log);
        model.refresh();
        check(model.allText().count("usb: permission pending before init") == 1, "pre-init USB status captured once");
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
    freeState(state.get());
    DSD_SECURE_ZERO(state.get(), sizeof(*state));
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
    DiagnosticsLog::installTap();
    earlyHostCaptureAndBridge();
    clearWithConcurrentSubmit();
    realKeySources();
    test_live_formatted_keys();
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
        check(model.allText().toUtf8().size() <= 512 && !model.allText().contains(QChar::ReplacementCharacter));
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
