// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the session-args builder — the ':'-delimited rtl input specs and
 * flag set a saved system starts with. These strings silently mistune a session
 * when malformed, which is why they are assembled in C++ under test rather than
 * in QML-side JavaScript. The bias-tee tri-state matrix is the load-bearing
 * case: a per-system explicit Off must survive an app-wide On. */

#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <cmath>
#include <limits>
#include <qtenvironmentvariables.h>
#include <stdio.h>
#include <utility>
#include "app_prefs.h"
#include "decoder_host.h"
#include "recording_sessions.h"

#include <dsd-neo/runtime/analog_channel.h>
#include "dsd-neo/core/safe_api.h"
#include "saved_systems_model.h"
#include "session_args.h"

using dsd_qt::session_args_build;
using dsd_qt::session_args_freq_valid;
using dsd_qt::SessionArgPrefs;
using dsd_qt::SessionArgsError;

namespace {

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

QVariantMap
usb_system(void) {
    QVariantMap sys;
    sys.insert(QStringLiteral("sourceType"), QStringLiteral("usb"));
    sys.insert(QStringLiteral("freqMhz"), QStringLiteral("851.375"));
    sys.insert(QStringLiteral("trunking"), false);
    return sys;
}

QString
input_spec(const QStringList& args) {
    const qsizetype i = args.indexOf(QStringLiteral("-i"));
    return (i >= 0 && i + 1 < args.size()) ? args.at(i + 1) : QString();
}

void
test_tg_lockout_preference(void) {
    auto system = usb_system();
    SessionArgPrefs prefs;
    SessionArgsError error = SessionArgsError::None;
    auto args = session_args_build(system, prefs, &error);
    expect("saved lockouts default", args.count("--tg-lockout-persist") == 1);
    prefs.persistTgLockouts = false;
    args = session_args_build(system, prefs, &error);
    expect("temporary lockout args", args.count("--tg-lockout-session") == 1 && !args.contains("--tg-lockout-persist"));
    QString scanError;
    args = dsd_qt::session_args_scan_build(system, "851.375", "scan.csv", prefs, &scanError);
    expect("scan temporary lockout args", scanError.isEmpty() && args.count("--tg-lockout-session") == 1);
    prefs.persistTgLockouts = true;
    args = dsd_qt::session_args_scan_build(system, "851.375", "scan.csv", prefs, &scanError);
    expect("scan saved lockout args", scanError.isEmpty() && args.count("--tg-lockout-persist") == 1);
    system["extraArgs"] = "--tg-lockout-session";
    args = session_args_build(system, prefs, &error);
    expect("extra args override default", args.indexOf("--tg-lockout-session") > args.indexOf("--tg-lockout-persist"));
}

/* Relaxed CRC is an app preference: every session the app starts gets -F (a saved system, an Explore or search map, a
 * scan list) unless the options the user typed name the CRC policy themselves (-F or --strict-crc, the system's own or
 * the app-wide Extra options), which win. */
void
test_relaxed_crc_preference(void) {
    auto sys = usb_system();
    SessionArgPrefs prefs;
    SessionArgsError error = SessionArgsError::None;
    auto args = session_args_build(sys, prefs, &error);
    expect("relaxed CRC is off by default", !prefs.relaxedCrc);
    expect("off adds no CRC option",
           error == SessionArgsError::None && !args.contains("-F") && !args.contains("--strict-crc"));
    prefs.relaxedCrc = true;
    args = session_args_build(sys, prefs, &error);
    expect("on adds -F once", error == SessionArgsError::None && args.count("-F") == 1);
    sys["extraArgs"] = "--strict-crc";
    args = session_args_build(sys, prefs, &error);
    expect("a system's --strict-crc wins", args.count("--strict-crc") == 1 && !args.contains("-F"));
    sys["extraArgs"] = "-t 9 -F";
    args = session_args_build(sys, prefs, &error);
    expect("a system's own -F is not doubled", args.count("-F") == 1);
    sys["extraArgs"] = "";
    prefs.extraArgs = "--strict-crc";
    args = session_args_build(sys, prefs, &error);
    expect("the app-wide --strict-crc wins", args.count("--strict-crc") == 1 && !args.contains("-F"));
    prefs.extraArgs = "--strict-crc-typo";
    args = session_args_build(sys, prefs, &error);
    expect("only the exact option counts", args.count("-F") == 1);
    prefs.extraArgs.clear();

    /* An Explore session is a map that was never saved; the same builder starts it. */
    QVariantMap explore;
    explore["sourceType"] = "rtltcp";
    explore["host"] = "127.0.0.1";
    explore["port"] = 1234;
    explore["freqMhz"] = "851.0125";
    args = session_args_build(explore, prefs, &error);
    expect("an explore session gets -F", error == SessionArgsError::None && args.count("-F") == 1);

    /* A scan list: the -F is the configured value its rows' own -F or --strict-crc run over. */
    QString scanError;
    args = dsd_qt::session_args_scan_build(sys, "851.375", "scan.csv", prefs, &scanError);
    expect("a scan list gets -F once", scanError.isEmpty() && args.count("-F") == 1);
    prefs.extraArgs = "--strict-crc";
    args = dsd_qt::session_args_scan_build(sys, "851.375", "scan.csv", prefs, &scanError);
    expect("the app-wide --strict-crc wins on a scan list",
           scanError.isEmpty() && !args.contains("-F") && args.count("--strict-crc") == 1);
    prefs.extraArgs.clear();
    prefs.relaxedCrc = false;
    args = dsd_qt::session_args_scan_build(sys, "851.375", "scan.csv", prefs, &scanError);
    expect("off adds nothing to a scan list", scanError.isEmpty() && !args.contains("-F"));
}

/* One mapping from the live AppPrefs to a session's defaults, which the saved-system builder and the scan-list starter
 * share. */
void
test_prefs_mapping(void) {
    const SessionArgPrefs defaults;
    const SessionArgPrefs none = dsd_qt::session_args_prefs(nullptr);
    expect("no preferences: the defaults", none.gainDb == defaults.gainDb && none.relaxedCrc == defaults.relaxedCrc
                                               && none.extraArgs == defaults.extraArgs);
    dsd_qt::AppPrefs prefs;
    prefs.setGainDb(41);
    prefs.setPpm(7);
    prefs.setBandwidthKhz(24);
    prefs.setBiasTee(true);
    prefs.setSkipEncrypted(false);
    prefs.setPersistTgLockouts(false);
    prefs.setAutoPpm(true);
    prefs.setRelaxedCrc(true);
    prefs.setHangtimeSec(3.5);
    prefs.setExtraArgs(QStringLiteral("-Z"));
    prefs.setRecordCalls(true);
    prefs.setRecordUnknownOnly(true);
    const SessionArgPrefs mapped = dsd_qt::session_args_prefs(&prefs);
    expect("the call recording preferences map, never a folder",
           mapped.recordCalls && mapped.recordUnknownOnly && mapped.recordingFolder.isEmpty());
    expect("call recording is off by default", !none.recordCalls && !none.recordUnknownOnly);
    expect("every preference maps",
           mapped.gainDb == 41 && mapped.ppm == 7 && mapped.bandwidthKhz == 24 && mapped.biasTee
               && !mapped.skipEncrypted && !mapped.persistTgLockouts && mapped.autoPpm && mapped.relaxedCrc
               && std::fabs(mapped.hangtimeSec - 3.5) < 1e-9 && mapped.extraArgs == QStringLiteral("-Z"));
    prefs.setGainDb(defaults.gainDb);
    prefs.setPpm(defaults.ppm);
    prefs.setBandwidthKhz(defaults.bandwidthKhz);
    prefs.setBiasTee(defaults.biasTee);
    prefs.setSkipEncrypted(defaults.skipEncrypted);
    prefs.setPersistTgLockouts(defaults.persistTgLockouts);
    prefs.setAutoPpm(defaults.autoPpm);
    prefs.setRelaxedCrc(defaults.relaxedCrc);
    prefs.setHangtimeSec(defaults.hangtimeSec);
    prefs.setExtraArgs(defaults.extraArgs);
    prefs.setRecordCalls(defaults.recordCalls);
    prefs.setRecordUnknownOnly(defaults.recordUnknownOnly);
}

/* Issue #708: the call recording a session's argv names, from the store's folder and the two preferences, before the
 * extra options so they can add to it; once in a scan list's argv. */
void
test_recording_args(void) {
    auto sys = usb_system();
    sys["extraArgs"] = QStringLiteral("-Z");
    SessionArgPrefs prefs;
    prefs.recordCalls = true;
    prefs.recordUnknownOnly = true;
    SessionArgsError error = SessionArgsError::None;
    auto args = session_args_build(sys, prefs, &error);
    expect("no folder: no call recording at all", error == SessionArgsError::None && !args.contains("-7")
                                                      && !args.contains("--wav-journal") && !args.contains("-P")
                                                      && !args.contains("--record-unknown-only"));

    prefs.recordingFolder = QStringLiteral("/data/Call recordings/County (abcd1234)");
    args = session_args_build(sys, prefs, &error);
    const QStringList want{QStringLiteral("-7"),
                           prefs.recordingFolder,
                           QStringLiteral("--wav-journal"),
                           prefs.recordingFolder + QStringLiteral("/recordings.jsonl"),
                           QStringLiteral("-P"),
                           QStringLiteral("--record-unknown-only"),
                           QStringLiteral("-Z")};
    const qsizetype at = args.indexOf(QStringLiteral("-7"));
    expect("folder, journal, -P and unknown-only, then the extra options, after the hang time",
           error == SessionArgsError::None && at > args.indexOf(QStringLiteral("-t"))
               && args.mid(at, want.size()) == want && args.count(QStringLiteral("-7")) == 1);

    prefs.recordCalls = false;
    prefs.recordUnknownOnly = false;
    args = session_args_build(sys, prefs, &error);
    expect("Record calls off still names the folder and journal, so an extra -P records there",
           args.contains("-7") && args.contains("--wav-journal") && !args.contains("-P")
               && !args.contains("--record-unknown-only"));

    // Unknown-only travels with -P: off while Record calls is off, whatever its own switch says.
    for (const bool record : {false, true}) {
        for (const bool unknown : {false, true}) {
            prefs.recordCalls = record;
            prefs.recordUnknownOnly = unknown;
            args = session_args_build(sys, prefs, &error);
            expect(qPrintable(QStringLiteral("Record calls %1, unknown-only %2").arg(record).arg(unknown)),
                   args.contains(QStringLiteral("-P")) == record
                       && args.contains(QStringLiteral("--record-unknown-only")) == (record && unknown));
        }
    }
    prefs.recordUnknownOnly = false;

    prefs.recordCalls = true;
    prefs.extraArgs = QStringLiteral("-Z");
    QString scanError;
    args = dsd_qt::session_args_scan_build(sys, "851.375", "scan.csv", prefs, &scanError);
    const qsizetype scanAt = args.indexOf(QStringLiteral("-7"));
    expect("a scan list names the folder once, before the scan and extra options",
           scanError.isEmpty() && args.count(QStringLiteral("-7")) == 1 && args.count(QStringLiteral("-P")) == 1
               && args.count(QStringLiteral("--wav-journal")) == 1 && scanAt >= 0
               && scanAt < args.indexOf(QStringLiteral("--trunk-scan")) && scanAt < args.indexOf(QStringLiteral("-Z"))
               && args.value(scanAt + 1) == prefs.recordingFolder);
}

/* Issue #708: the app owns where call recordings go and when they are deleted, so extra options that say otherwise
 * refuse the start, in every place they can be typed and in either option form; -w only while Record calls is on. */
void
test_recording_refused_extras(void) {
    using dsd_qt::session_args_recording_extra_error;
    for (const auto& tokens :
         QStringList{"-7 /x", "-7/x", "-F -7 /x", "--wav-journal /x/j.jsonl", "--wav-journal=/x/j.jsonl",
                     "--rdio-api-delete-after-upload", "--rdio-api-delete-after-upload=1",
                     // A switch takes no value: the -7 after it is an option, as the decoder reads it.
                     "--record-unknown-only -7 /x", "--enc-lockout -7 /x",
                     // --config takes the next argument only when it is not an option.
                     "--config -7 /x", "--frontend=none -7 /x"}) {
        expect(qPrintable(QStringLiteral("refused: %1").arg(tokens)),
               session_args_recording_extra_error(tokens, false) == SessionArgsError::CallRecordingOption);
    }
    for (const auto& tokens : QStringList{"-w out.wav", "-wout.wav", "-Z -w out.wav"}) {
        expect(qPrintable(QStringLiteral("-w refused with Record calls: %1").arg(tokens)),
               session_args_recording_extra_error(tokens, true) == SessionArgsError::CallRecordingStaticWav);
        expect(qPrintable(QStringLiteral("-w kept without Record calls: %1").arg(tokens)),
               session_args_recording_extra_error(tokens, false) == SessionArgsError::None);
    }
    for (const auto& tokens : QStringList{"", "-P", "--record-unknown-only", "--input-level-warn-db -7",
                                          "--frame-log -7", "-c -7", "-o -w", "--api-token -7", "--config cfg.ini -P",
                                          "--wav-journalx /x", "--rdio-api-delete-after-uploads", "-Z -- -7 /x"}) {
        expect(qPrintable(QStringLiteral("kept: %1").arg(tokens)),
               session_args_recording_extra_error(tokens, true) == SessionArgsError::None);
    }
    // getopt reads a group letter by letter, and a "--" that is a value ends nothing; one that is no value ends the
    // options.
    for (const auto& tokens : QStringList{"-F7/x", "-o -- -7 /x"}) {
        expect(qPrintable(QStringLiteral("refused as getopt reads it: %1").arg(tokens)),
               session_args_recording_extra_error(tokens, false) == SessionArgsError::CallRecordingOption);
    }
    for (const auto& tokens : QStringList{"-o x -- -7 /x", "-o -- -- -7 /x"}) {
        expect(qPrintable(QStringLiteral("kept as getopt reads it: %1").arg(tokens)),
               session_args_recording_extra_error(tokens, true) == SessionArgsError::None);
    }

    // With no storage for call recordings, an extra -P would record outside it; nothing else is refused for that.
    expect("-P with no storage", session_args_recording_extra_error(QStringLiteral("-Z -P"), false, true)
                                     == SessionArgsError::CallRecordingNoStorage);
    expect("-P as a value is not",
           session_args_recording_extra_error(QStringLiteral("-c -P"), false, true) == SessionArgsError::None);
    expect("no -P, nothing refused",
           session_args_recording_extra_error(QStringLiteral("-Z"), true, true) == SessionArgsError::None);

    auto sys = usb_system();
    SessionArgPrefs prefs;
    SessionArgsError error = SessionArgsError::None;
    sys["extraArgs"] = QStringLiteral("-P");
    prefs.recordingUnavailable = true;
    expect("an extra -P with no storage",
           session_args_build(sys, prefs, &error).isEmpty() && error == SessionArgsError::CallRecordingNoStorage);
    QString noStorage;
    prefs.extraArgs = QStringLiteral("-P");
    expect("in a scan list too",
           dsd_qt::session_args_scan_build(sys, "851.375", "scan.csv", prefs, &noStorage).isEmpty()
               && noStorage == dsd_qt::session_args_error_text(SessionArgsError::CallRecordingNoStorage));
    prefs.extraArgs.clear();
    prefs.recordingUnavailable = false;
    expect("a validation build asks no storage", !session_args_build(sys, prefs, &error).isEmpty());
    sys["extraArgs"] = QStringLiteral("-7 /x");
    expect("a system's own extra options",
           session_args_build(sys, prefs, &error).isEmpty() && error == SessionArgsError::CallRecordingOption);
    sys.remove("extraArgs");
    prefs.extraArgs = QStringLiteral("--wav-journal=/x");
    expect("the app-wide extra options",
           session_args_build(sys, prefs, &error).isEmpty() && error == SessionArgsError::CallRecordingOption);
    prefs.extraArgs = QStringLiteral("-w out.wav");
    prefs.recordCalls = true;
    expect("-w beside Record calls",
           session_args_build(sys, prefs, &error).isEmpty() && error == SessionArgsError::CallRecordingStaticWav);
    QString scanError;
    expect("a scan list's extra options",
           dsd_qt::session_args_scan_build(sys, "851.375", "scan.csv", prefs, &scanError).isEmpty()
               && scanError == dsd_qt::session_args_error_text(SessionArgsError::CallRecordingStaticWav));
    prefs.extraArgs = QStringLiteral("--rdio-api-delete-after-upload");
    expect("delete after upload in a scan list",
           dsd_qt::session_args_scan_build(sys, "851.375", "scan.csv", prefs, &scanError).isEmpty()
               && scanError == dsd_qt::session_args_error_text(SessionArgsError::CallRecordingOption));

    dsd_qt::AppPrefs appPrefs;
    appPrefs.setExtraArgs(QStringLiteral("-w out.wav"));
    appPrefs.setRecordCalls(true);
    const dsd_qt::SessionArgsBuilder builder(&appPrefs);
    const auto result = builder.build(usb_system());
    expect("the builder names the refusal for the start screen",
           !result.value("ok").toBool()
               && result.value("error").toString() == QStringLiteral("call-recording-static-wav")
               && result.value("errorText").toString()
                      == dsd_qt::session_args_error_text(SessionArgsError::CallRecordingStaticWav));
    appPrefs.setRecordCalls(false);
    expect("and lets it start once Record calls is off", builder.build(usb_system()).value("ok").toBool());
    appPrefs.setExtraArgs(QString());
}

/* Issue #708: a start asks the call recording store for the session's folder before the host starts, names it in the
 * argv, and frees it when the start did not happen. The caller says what the start is: a saved system has its own
 * folder; Explore and Signal search share one, whatever uid the map carries (an edited Explore start can carry the
 * uid of the saved system it was copied from). An invalid system asks for nothing. */
void
test_recording_sessions(void) {
    class Sessions : public dsd_qt::RecordingSessions {
      public:
        QStringList calls;
        QString folder = QStringLiteral("/recordings/folder");

        QString
        beginSession(const QString& kind, const QString& uid, const QString& name) override {
            calls.append(QStringLiteral("begin %1 %2 %3").arg(kind, uid, name));
            return folder;
        }

        void
        endSession() override {
            calls.append(QStringLiteral("end"));
        }
    } sessions;

    class Host : public dsd_qt::DecoderHost {
      public:
        QStringList* calls = nullptr;
        QStringList received;
        bool accept = true;

        bool
        isRunning() const override {
            return false;
        }

        QString
        statusText() const override {
            return {};
        }

        bool
        start(const QStringList& args) override {
            received = args;
            calls->append(QStringLiteral("start"));
            return accept;
        }

        void
        noteStartAttempt(const QString& sourceType) override {
            calls->append(QStringLiteral("note ") + sourceType);
        }

        void
        stop() override {}
    } host;

    host.calls = &sessions.calls;
    dsd_qt::SessionArgsBuilder builder(nullptr);
    builder.setRecordingSessions(&sessions);
    auto sys = usb_system();
    sys["uid"] = QStringLiteral("sys-1");
    sys["name"] = QStringLiteral("County");
    expect("a saved system starts", builder.start(sys, &host, QStringLiteral("saved")).value("started").toBool());
    expect("its folder is asked for before the host starts",
           sessions.calls
               == QStringList(
                   {QStringLiteral("begin saved sys-1 County"), QStringLiteral("note usb"), QStringLiteral("start")}));
    const qsizetype folderAt = host.received.indexOf(QStringLiteral("-7"));
    const qsizetype journalAt = host.received.indexOf(QStringLiteral("--wav-journal"));
    expect("the session records into that folder",
           folderAt >= 0 && host.received.value(folderAt + 1) == QStringLiteral("/recordings/folder"));
    expect("and journals there",
           journalAt >= 0
               && host.received.value(journalAt + 1) == QStringLiteral("/recordings/folder/recordings.jsonl"));

    sessions.calls.clear();
    (void)builder.start(sys, &host, QStringLiteral("search"));
    expect("a signal search records into Explore",
           sessions.calls.value(0) == QStringLiteral("begin explore sys-1 County"));
    sessions.calls.clear();
    (void)builder.start(sys, &host, QStringLiteral("explore"));
    expect("so does an Explore start carrying a saved system's uid",
           sessions.calls.value(0) == QStringLiteral("begin explore sys-1 County"));
    sessions.calls.clear();
    sys.remove("uid");
    (void)builder.start(sys, &host, QStringLiteral("saved"));
    expect("and a map no one saved", sessions.calls.value(0).startsWith(QStringLiteral("begin explore ")));

    sessions.calls.clear();
    sessions.folder.clear();
    expect("with no storage to record to",
           builder.start(sys, &host, QStringLiteral("explore")).value("started").toBool());
    expect("the session still starts, recording nothing",
           !host.received.contains(QStringLiteral("-7")) && !host.received.contains(QStringLiteral("--wav-journal"))
               && !host.received.contains(QStringLiteral("-P")));
    sessions.folder = QStringLiteral("/recordings/folder");

    sessions.calls.clear();
    host.accept = false;
    expect("a refused start", !builder.start(sys, &host, QStringLiteral("explore")).value("started").toBool());
    expect("frees the folder", sessions.calls.size() == 4 && sessions.calls.value(3) == QStringLiteral("end"));

    sessions.calls.clear();
    sys["freqMhz"] = QStringLiteral("not a frequency");
    (void)builder.start(sys, &host, QStringLiteral("explore"));
    expect("an invalid system asks for nothing", sessions.calls.isEmpty());

    // No storage: an extra -P would record outside it, so the start is refused and frees what it asked for.
    sessions.calls.clear();
    sessions.folder.clear();
    sys["freqMhz"] = QStringLiteral("851.375");
    sys["extraArgs"] = QStringLiteral("-P");
    host.accept = true;
    const auto noStorage = builder.start(sys, &host, QStringLiteral("explore"));
    expect("an extra -P with no storage refuses the start",
           noStorage.value("error").toString() == QStringLiteral("call-recording-option")
               && noStorage.value("errorText").toString()
                      == dsd_qt::session_args_error_text(SessionArgsError::CallRecordingNoStorage)
               && !noStorage.value("started").toBool());
    expect("and frees the session it began",
           sessions.calls.size() == 2 && sessions.calls.value(1) == QStringLiteral("end"));
    expect("and notes no start attempt: the device was never asked",
           !sessions.calls.contains(QStringLiteral("note usb")));
    sessions.folder = QStringLiteral("/recordings/folder");

    sessions.calls.clear();
    sys["extraArgs"] = QStringLiteral("-7 /elsewhere");
    const auto refused = builder.start(sys, &host, QStringLiteral("explore"));
    expect("extra options naming the folder refuse the start, asking for nothing",
           refused.value("error").toString() == QStringLiteral("call-recording-option")
               && !refused.value("started").toBool() && sessions.calls.isEmpty());
}

/* The QML-facing builder reads the preference from the live AppPrefs, as a start does. */
void
test_relaxed_crc_app_preference(void) {
    class Host : public dsd_qt::DecoderHost {
      public:
        QStringList received;

        bool
        isRunning() const override {
            return false;
        }

        QString
        statusText() const override {
            return {};
        }

        bool
        start(const QStringList& args) override {
            received = args;
            return true;
        }

        void
        stop() override {}
    } host;

    dsd_qt::AppPrefs prefs;
    expect("the preference is a property", prefs.setProperty("relaxedCrc", true));
    dsd_qt::SessionArgsBuilder builder(&prefs);
    auto sys = usb_system();
    expect("start succeeds", builder.start(sys, &host, QStringLiteral("saved")).value("started").toBool());
    expect("a started system gets -F", host.received.count("-F") == 1);
    sys["extraArgs"] = "--strict-crc";
    expect("start with --strict-crc", builder.start(sys, &host, QStringLiteral("saved")).value("started").toBool());
    expect("the system's --strict-crc wins at start", !host.received.contains("-F"));
    {
        dsd_qt::AppPrefs reopened;
        expect("the preference survives a restart", reopened.property("relaxedCrc").toBool());
    }
    expect("the preference turns off", prefs.setProperty("relaxedCrc", false));
    sys.remove("extraArgs");
    expect("start without the preference",
           builder.start(sys, &host, QStringLiteral("saved")).value("started").toBool());
    expect("off starts without -F", !host.received.contains("-F"));
}

void
test_hangtime(void) {
    auto sys = usb_system();
    SessionArgPrefs prefs;
    SessionArgsError error = SessionArgsError::None;
    auto args = session_args_build(sys, prefs, &error);
    expect("default hang time", args.count("-t") == 1 && args.value(args.indexOf("-t") + 1) == "2.0");
    prefs.hangtimeSec = 3.5;
    args = session_args_build(sys, prefs, &error);
    expect("preference hang time", args.value(args.indexOf("-t") + 1) == "3.5");
    sys["hangtime"] = "1.5";
    sys["extraArgs"] = "-t 9 -F";
    prefs.extraArgs = "-t 8";
    args = session_args_build(sys, prefs, &error);
    expect("system override precedes extras",
           error == SessionArgsError::None && args.count("-t") == 3
               && args.mid(args.indexOf("-t")) == QStringList({"-t", "1.5", "-t", "9", "-F", "-t", "8"}));
    dsd_qt::SessionArgsBuilder builder(nullptr);
    for (const auto& value : QStringList{"abc", "-1", "45", "nan", "inf"}) {
        sys["hangtime"] = value;
        expect("invalid hang time refuses argv",
               session_args_build(sys, prefs, &error).isEmpty() && error == SessionArgsError::Hangtime);
        const auto result = builder.build(sys);
        expect("hang time has its own validation category",
               !result.value("ok").toBool() && result.value("error") == "hangtime"
                   && result.value("errorText") == "Enter hang time in seconds from 0 to 30.");
    }
    for (const auto& value : QStringList{"0", "30", " 2.5 "}) {
        sys["hangtime"] = value;
        expect("boundary hang times accepted", !session_args_build(sys, prefs, &error).isEmpty());
    }
    sys["hangtime"] = "";
    prefs.hangtimeSec = std::numeric_limits<double>::quiet_NaN();
    expect("nonfinite preference refuses argv",
           session_args_build(sys, prefs, &error).isEmpty() && error == SessionArgsError::Hangtime);
}

void
test_freq_validation(void) {
    expect("plain MHz parses", session_args_freq_valid(QStringLiteral("851.375")));
    expect("surrounding whitespace is tolerated", session_args_freq_valid(QStringLiteral(" 851.375 ")));
    expect("trailing junk is refused", !session_args_freq_valid(QStringLiteral("851.375M")));
    expect("zero is refused", !session_args_freq_valid(QStringLiteral("0")));
    expect("negative is refused", !session_args_freq_valid(QStringLiteral("-851")));
    expect("empty is refused", !session_args_freq_valid(QString()));
    expect("highest tunable frequency is accepted", session_args_freq_valid(QStringLiteral("4294.967295")));
    expect("lowest tunable frequency is accepted", session_args_freq_valid(QStringLiteral("0.000001")));
    expect("one hertz past the ceiling is refused", !session_args_freq_valid(QStringLiteral("4294.967296")));
    expect("far above the ceiling is refused", !session_args_freq_valid(QStringLiteral("5000")));
    expect("a value rounding to 0 Hz is refused", !session_args_freq_valid(QStringLiteral("0.0000001")));
    expect("infinity is refused", !session_args_freq_valid(QStringLiteral("inf")));
    /* Same exact rounding as the runtime's "<text>M" spec parse (dsd_parse_freq_hz). */
    expect("7-decimal text rounding above the ceiling is refused",
           !session_args_freq_valid(QStringLiteral("4294.9672955")));
    expect("7-decimal text rounding to the ceiling is accepted",
           session_args_freq_valid(QStringLiteral("4294.9672954")));
    expect("6-decimal ceiling is accepted", session_args_freq_valid(QStringLiteral("4294.967295")));
    expect("half a hertz rounds up to 1 Hz and is accepted", session_args_freq_valid(QStringLiteral("0.0000005")));
    expect("under half a hertz rounds to 0 and is refused", !session_args_freq_valid(QStringLiteral("0.0000004")));

    SessionArgsError error = SessionArgsError::None;
    QVariantMap sys = usb_system();
    sys.insert(QStringLiteral("freqMhz"), QStringLiteral("garbage"));
    expect("bad frequency refuses the build",
           session_args_build(sys, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::Frequency);

    dsd_qt::SessionArgsBuilder builder(nullptr);
    sys.insert(QStringLiteral("freqMhz"), QStringLiteral("5000"));
    const auto result = builder.build(sys);
    expect("out-of-range frequency error names the range",
           !result.value("ok").toBool() && result.value("error") == "frequency"
               && result.value("errorText") == "Enter a frequency from 0.000001 to 4294.967295 MHz");
}

/*
 * An explore session is an ordinary session over a map that was never saved, so
 * the only thing keeping it honest is the shape of that map. Two properties are
 * load-bearing and neither is visible anywhere else: no -T, because a trunker
 * would take the tuner straight back from the user who came here to drive it,
 * and no decode flag, because the point is to hear whatever it lands on.
 */
void
test_explore_system(void) {
    QVariantMap sys;
    sys.insert(QStringLiteral("name"), QStringLiteral("Exploring"));
    sys.insert(QStringLiteral("sourceType"), QStringLiteral("usb"));
    sys.insert(QStringLiteral("host"), QString());
    sys.insert(QStringLiteral("port"), 0);
    sys.insert(QStringLiteral("freqMhz"), QStringLiteral("855.0000"));
    sys.insert(QStringLiteral("decodeFlag"), QString());
    sys.insert(QStringLiteral("trunking"), false);

    SessionArgsError error = SessionArgsError::None;
    const QStringList args = session_args_build(sys, SessionArgPrefs(), &error);
    expect("an explore map builds", error == SessionArgsError::None);
    expect("explore tunes where it was told", input_spec(args) == QStringLiteral("rtl:0:855.0000M:30:0:48:0:2"));
    expect("explore never follows calls across channels", !args.contains(QStringLiteral("-T")));
    expect("explore leaves the decoder to work it out", !args.contains(QStringLiteral("-f1"))
                                                            && !args.contains(QStringLiteral("-fs"))
                                                            && !args.contains(QStringLiteral("-ft")));

    // Over rtl_tcp it is the same session with a different front end. The host
    // lands in the ':'-delimited spec verbatim, so stray whitespace from the
    // soft keyboard or a paste must be trimmed here — "10.0.2.2 " resolves to
    // nothing and the start fails with an opaque input error.
    sys.insert(QStringLiteral("sourceType"), QStringLiteral("rtltcp"));
    sys.insert(QStringLiteral("host"), QStringLiteral("10.0.2.2  "));
    sys.insert(QStringLiteral("port"), 1234);
    const QStringList remote = session_args_build(sys, SessionArgPrefs(), &error);
    expect("explore builds over rtl_tcp", error == SessionArgsError::None);
    expect("explore reaches the remote tuner, host trimmed",
           input_spec(remote) == QStringLiteral("rtltcp:10.0.2.2:1234:855.0000M:30:0:48:0:2"));

    // A frequency that never made it into the prefs must be refused here, the
    // same as a saved system's would be, rather than starting a mistuned session.
    sys.insert(QStringLiteral("freqMhz"), QString());
    expect("an explore map with no frequency is refused",
           session_args_build(sys, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::Frequency);
}

/*
 * Issue #525: the NFM chip (-fA, the analog monitor) sits in the shared decode catalog, so the setup wizard and a
 * saved system can carry it, not only the Radio sheet. It has to reach the session as the bare -fA, on a local dongle
 * and over rtl_tcp, with no trunking flag: the monitor follows no control channel.
 */
void
test_nfm_system(void) {
    QVariantMap sys = usb_system();
    sys.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fA"));
    SessionArgsError error = SessionArgsError::None;
    const QStringList args = session_args_build(sys, SessionArgPrefs(), &error);
    expect("an NFM system builds", error == SessionArgsError::None);
    expect("the NFM chip reaches the session as -fA", args.count(QStringLiteral("-fA")) == 1);
    expect("an NFM system follows no calls", !args.contains(QStringLiteral("-T")));
    expect("an NFM system names no digital decoder", !args.contains(QStringLiteral("-fa"))
                                                         && !args.contains(QStringLiteral("-ft"))
                                                         && !args.contains(QStringLiteral("-fs")));
    expect("an NFM system tunes where it was told", input_spec(args) == QStringLiteral("rtl:0:851.375M:30:0:48:0:2"));

    sys.insert(QStringLiteral("sourceType"), QStringLiteral("rtltcp"));
    sys.insert(QStringLiteral("host"), QStringLiteral("10.0.2.2"));
    sys.insert(QStringLiteral("port"), 1234);
    const QStringList remote = session_args_build(sys, SessionArgPrefs(), &error);
    expect("an NFM system builds over rtl_tcp", error == SessionArgsError::None);
    expect("NFM over rtl_tcp is still -fA",
           remote.count(QStringLiteral("-fA")) == 1 && !remote.contains(QStringLiteral("-T")));
    expect("NFM over rtl_tcp reaches the remote tuner",
           input_spec(remote) == QStringLiteral("rtltcp:10.0.2.2:1234:851.375M:30:0:48:0:2"));
}

/*
 * Issue #524: the AM chip (-fM) demodulates from the radio's I/Q. A system on a radio source reaches the session with
 * it; one on a network or file source, which the wizard no longer saves but an older save may hold, fails with its own
 * reason instead of reaching the engine, which would refuse -fM on audio that arrives demodulated.
 */
void
test_am_system(void) {
    QVariantMap sys = usb_system();
    sys.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fM"));
    SessionArgsError error = SessionArgsError::None;
    const QStringList args = session_args_build(sys, SessionArgPrefs(), &error);
    expect("an AM system builds on a USB dongle", error == SessionArgsError::None);
    expect("the AM chip reaches the session as -fM", args.count(QStringLiteral("-fM")) == 1);
    expect("an AM system follows no calls", !args.contains(QStringLiteral("-T")));

    for (const char* source : {"tcp", "udp", "file"}) {
        QVariantMap pcm = sys;
        pcm.insert(QStringLiteral("sourceType"), QString::fromLatin1(source));
        pcm.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        pcm.insert(QStringLiteral("port"), 7355);
        pcm.insert(QStringLiteral("path"), QStringLiteral("/tmp/capture.wav"));
        error = SessionArgsError::None;
        expect("an AM system on a network or file source is refused",
               session_args_build(pcm, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::AmNeedsRadio);
    }
    expect("the refusal says why",
           session_args_error_text(SessionArgsError::AmNeedsRadio).contains(QStringLiteral("AM needs a radio source")));

    /* Among other tokens, as an import could save it; and a flag that merely contains the letters is not AM. */
    QVariantMap composite = sys;
    composite.insert(QStringLiteral("sourceType"), QStringLiteral("tcp"));
    composite.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    composite.insert(QStringLiteral("port"), 7355);
    composite.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fM -Y"));
    expect("a composite AM flag is refused off a radio",
           session_args_build(composite, SessionArgPrefs(), &error).isEmpty()
               && error == SessionArgsError::AmNeedsRadio);
    composite.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fA"));
    error = SessionArgsError::None;
    expect("NFM still builds on TCP audio",
           !session_args_build(composite, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::None);

    /* TETRA (-fT) reads its symbols from a radio's I/Q, or from a symbol capture file of them: on network audio or an
       audio file it is refused here, as the engine refuses it, with its own reason. */
    QVariantMap tetra = usb_system();
    tetra.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fT"));
    error = SessionArgsError::None;
    expect("a TETRA system builds on a USB dongle",
           !session_args_build(tetra, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::None);
    /* With no bandwidth of its own a TETRA system takes 72 kHz, the rate -fT takes: TETRA runs only at a multiple of
       18 kHz, and the app-wide default is 48. Its own bandwidth still wins. */
    expect("a TETRA system without its own bandwidth runs at 72 kHz",
           input_spec(session_args_build(tetra, SessionArgPrefs(), &error)).contains(QStringLiteral(":72:")));
    QVariantMap tetraOwnBw = tetra;
    tetraOwnBw.insert(QStringLiteral("bandwidthKhz"), 48);
    expect("a TETRA system's own bandwidth wins",
           input_spec(session_args_build(tetraOwnBw, SessionArgPrefs(), &error)).contains(QStringLiteral(":48:")));
    QVariantMap dmrDefault = usb_system();
    dmrDefault.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fs"));
    expect("other modes keep the app-wide default bandwidth",
           input_spec(session_args_build(dmrDefault, SessionArgPrefs(), &error)).contains(QStringLiteral(":48:")));
    for (const char* source : {"tcp", "udp", "file"}) {
        QVariantMap pcm = tetra;
        pcm.insert(QStringLiteral("sourceType"), QString::fromLatin1(source));
        pcm.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        pcm.insert(QStringLiteral("port"), 7355);
        pcm.insert(QStringLiteral("filePath"), QStringLiteral("/tmp/capture.wav"));
        error = SessionArgsError::None;
        expect("a TETRA system on network audio or an audio file is refused",
               session_args_build(pcm, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::TetraNeedsIq);
    }
    for (const char* capture : {"/tmp/tetra.bin", "/tmp/tetra.raw", "/tmp/tetra.sym"}) {
        QVariantMap symbols = tetra;
        symbols.insert(QStringLiteral("sourceType"), QStringLiteral("file"));
        symbols.insert(QStringLiteral("filePath"), QString::fromLatin1(capture));
        error = SessionArgsError::None;
        const QStringList args = session_args_build(symbols, SessionArgPrefs(), &error);
        expect("a TETRA system replays a symbol capture",
               !args.isEmpty() && error == SessionArgsError::None && input_spec(args) == QString::fromLatin1(capture));
    }
    expect("the TETRA refusal says why", session_args_error_text(SessionArgsError::TetraNeedsIq)
                                             .startsWith(QStringLiteral("TETRA needs a radio source")));
    const dsd_qt::SessionArgsBuilder tetraBuilder(nullptr);
    QVariantMap tcpTetra = tetra;
    tcpTetra.insert(QStringLiteral("sourceType"), QStringLiteral("tcp"));
    tcpTetra.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    tcpTetra.insert(QStringLiteral("port"), 7355);
    expect("the TETRA start refusal carries its category",
           tetraBuilder.build(tcpTetra).value(QStringLiteral("error")).toString() == QStringLiteral("tetra-needs-iq"));

    /* Issue #580: a restored backup leaves every replay system without its recording. Such a system is refused before
       it starts (never "-i" with an empty path), for that reason, ahead of what its decode mode needs of the file: a
       TETRA one must not be told to choose a radio. AM still needs a radio whatever the file. */
    for (const char* flag : {"", "-ft", "-fT"}) {
        for (const char* path : {"", "   "}) {
            QVariantMap replay;
            replay.insert(QStringLiteral("sourceType"), QStringLiteral("file"));
            replay.insert(QStringLiteral("filePath"), QString::fromLatin1(path));
            replay.insert(QStringLiteral("decodeFlag"), QString::fromLatin1(flag));
            error = SessionArgsError::None;
            expect(qPrintable(QStringLiteral("a file system without its recording is refused for that (%1)")
                                  .arg(QString::fromLatin1(flag))),
                   session_args_build(replay, SessionArgPrefs(), &error).isEmpty()
                       && error == SessionArgsError::RecordingMissing);
            expect("the missing-recording start refusal carries its category",
                   tetraBuilder.build(replay).value(QStringLiteral("error")).toString()
                       == QStringLiteral("recording-missing"));
        }
    }
    QVariantMap amReplay;
    amReplay.insert(QStringLiteral("sourceType"), QStringLiteral("file"));
    amReplay.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fM"));
    error = SessionArgsError::None;
    expect("AM on a file still needs a radio, recording or not",
           session_args_build(amReplay, SessionArgPrefs(), &error).isEmpty()
               && error == SessionArgsError::AmNeedsRadio);
    expect(
        "the missing-recording refusal says how to fix it",
        session_args_error_text(SessionArgsError::RecordingMissing)
            == QStringLiteral("This system's recording is missing. Edit the system and choose its recording again."));
}

/*
 * Issue #524: the engine holds the AM channel (the 6 kHz default) to the DSP bandwidth a USB or rtl_tcp spec carries
 * and refuses to start at 4 or 6 kHz, where no AM width fits. A system with that pair fails here with its own reason,
 * whether the bandwidth is its own or the app-wide default, and the wizard's gate says the same. A bandwidth the engine
 * does not run is its 48 kHz default, Airspy is held where its device sets the rate, and a digital mode is not held.
 */
void
test_am_bandwidth(void) {
    char fits[64];
    expect("the refusal lists the bandwidths the AM default fits",
           dsd_analog_width_fitting_rtl_bandwidths(DSD_ANALOG_AM_WIDTH_DEFAULT_HZ, fits, sizeof fits) == 0
               && session_args_error_text(SessionArgsError::AmBandwidth)
                      .contains(QStringLiteral("Set the bandwidth to %1 kHz").arg(QString::fromLatin1(fits))));

    for (const char* source : {"usb", "rtltcp"}) {
        QVariantMap sys = usb_system();
        sys.insert(QStringLiteral("sourceType"), QString::fromLatin1(source));
        sys.insert(QStringLiteral("host"), QStringLiteral("10.0.2.2"));
        sys.insert(QStringLiteral("port"), 1234);
        sys.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fM"));
        for (const int khz : {4, 6}) {
            sys.insert(QStringLiteral("bandwidthKhz"), khz);
            SessionArgsError error = SessionArgsError::None;
            expect("AM at a 4 or 6 kHz DSP bandwidth is refused",
                   session_args_build(sys, SessionArgPrefs(), &error).isEmpty()
                       && error == SessionArgsError::AmBandwidth);
            expect("the gate refuses it too",
                   !dsd_qt::session_args_am_fits_bandwidth(QString::fromLatin1(source), khz));
        }
        for (const int khz : {8, 12, 16, 24, 48, 72, 5, 10}) {
            sys.insert(QStringLiteral("bandwidthKhz"), khz);
            SessionArgsError error = SessionArgsError::None;
            const QStringList args = session_args_build(sys, SessionArgPrefs(), &error);
            expect("AM builds at 8 kHz and up, and at a bandwidth the engine runs at 48 kHz",
                   !args.isEmpty() && error == SessionArgsError::None);
        }

        /* No override: the app-wide default is the bandwidth the spec carries. */
        sys.remove(QStringLiteral("bandwidthKhz"));
        SessionArgPrefs narrow;
        narrow.bandwidthKhz = 6;
        SessionArgsError error = SessionArgsError::None;
        expect("AM under a 6 kHz app default is refused",
               session_args_build(sys, narrow, &error).isEmpty() && error == SessionArgsError::AmBandwidth);
        sys.insert(QStringLiteral("bandwidthKhz"), 12);
        error = SessionArgsError::None;
        expect("the system's own bandwidth wins over the app default",
               !session_args_build(sys, narrow, &error).isEmpty() && error == SessionArgsError::None);

        sys.insert(QStringLiteral("bandwidthKhz"), 6);
        sys.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fs"));
        error = SessionArgsError::None;
        expect("a digital mode at 6 kHz is not held to the AM channel",
               !session_args_build(sys, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::None);
    }

    QVariantMap airspy = usb_system();
    airspy.insert(QStringLiteral("sourceType"), QStringLiteral("airspy"));
    airspy.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fM"));
    airspy.insert(QStringLiteral("bandwidthKhz"), 6);
    SessionArgsError error = SessionArgsError::None;
    expect("Airspy is held at stream start, not here",
           !session_args_build(airspy, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::None);
    expect("a non-radio source is the I/Q refusal's, not this one",
           dsd_qt::session_args_am_fits_bandwidth(QStringLiteral("tcp"), 6));

    const dsd_qt::SessionArgsBuilder builder(nullptr);
    expect("the wizard gate names the fix", builder.amBandwidthError(QStringLiteral("usb"), 6)
                                                == session_args_error_text(SessionArgsError::AmBandwidth));
    expect("the wizard gate passes 8 kHz", builder.amBandwidthError(QStringLiteral("usb"), 8).isEmpty());
    expect("an empty bandwidth field follows the 48 kHz default",
           builder.amBandwidthError(QStringLiteral("rtltcp"), -1).isEmpty());
    const QVariantMap result = builder.build(QVariantMap{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                                                         {QStringLiteral("freqMhz"), QStringLiteral("118.1")},
                                                         {QStringLiteral("decodeFlag"), QStringLiteral("-fM")},
                                                         {QStringLiteral("bandwidthKhz"), 4}});
    expect("the start refusal carries its category and text",
           !result.value(QStringLiteral("ok")).toBool()
               && result.value(QStringLiteral("error")).toString() == QStringLiteral("am-bandwidth")
               && result.value(QStringLiteral("errorText")).toString()
                      == session_args_error_text(SessionArgsError::AmBandwidth));
    expect("the start refusal names the bandwidths that fit, 72 kHz among them",
           result.value(QStringLiteral("bandwidthChoices")).toString() == QString::fromLatin1(fits)
               && QString::fromLatin1(fits) == QStringLiteral("8, 12, 16, 24, 48 or 72"));
    expect("only the AM bandwidth refusal carries the choices",
           !builder.build(usb_system()).contains(QStringLiteral("bandwidthChoices")));
}

void
test_defaults_and_overrides(void) {
    SessionArgsError error = SessionArgsError::None;
    const QStringList defaults = session_args_build(usb_system(), SessionArgPrefs(), &error);
    expect("default build succeeds", error == SessionArgsError::None);
    expect("defaults fill the rtl spec", input_spec(defaults) == QStringLiteral("rtl:0:851.375M:30:0:48:0:2"));
    expect("frontend none is requested",
           defaults.mid(0, 2) == QStringList{QStringLiteral("--frontend"), QStringLiteral("none")});
    expect("pulse output is requested", defaults.contains(QStringLiteral("-o")));
    expect("skip-encrypted default adds the lockout", defaults.contains(QStringLiteral("--enc-lockout")));
    expect("auto-ppm stays off by default", !defaults.contains(QStringLiteral("--auto-ppm")));
    expect("no trunking flag without trunking", !defaults.contains(QStringLiteral("-T")));

    QVariantMap sys = usb_system();
    sys.insert(QStringLiteral("gainDb"), 36);
    sys.insert(QStringLiteral("ppm"), QStringLiteral("-2"));
    sys.insert(QStringLiteral("bandwidthKhz"), 12);
    sys.insert(QStringLiteral("trunking"), true);
    sys.insert(QStringLiteral("decodeFlag"), QStringLiteral("-f1 -mq"));
    sys.insert(QStringLiteral("extraArgs"), QStringLiteral("-C chan.csv"));
    SessionArgPrefs prefs;
    prefs.autoPpm = true;
    prefs.extraArgs = QStringLiteral("--wav-dir /tmp");
    const QStringList args = session_args_build(sys, prefs, &error);
    expect("override build succeeds", error == SessionArgsError::None);
    expect("overrides land in the rtl spec", input_spec(args) == QStringLiteral("rtl:0:851.375M:36:-2:12:0:2"));
    expect("multi-flag decode chip splits",
           args.contains(QStringLiteral("-f1")) && args.contains(QStringLiteral("-mq")));
    expect("trunking adds -T", args.contains(QStringLiteral("-T")));
    expect("auto-ppm pref adds the flag", args.contains(QStringLiteral("--auto-ppm")));
    expect("system and app extra args both land",
           args.contains(QStringLiteral("chan.csv")) && args.contains(QStringLiteral("--wav-dir")));
}

void
test_airspy_bandwidth(void) {
    const struct {
        int requested;
        int expected;
    } cases[] = {{25, 24},
                 {4, 4},
                 {6, 6},
                 {8, 8},
                 {12, 12},
                 {16, 16},
                 {24, 24},
                 {48, 48},
                 {1, 4},
                 {10, 8},
                 {20, 16},
                 {100, 48},
                 /* 72 kHz (TETRA's) is no Airspy bandwidth: no Airspy sample rate halves to it. */
                 {72, 48},
                 {60, 48},
                 {0, 48},
                 {-1, 48},
                 {std::numeric_limits<int>::min(), 48},
                 {std::numeric_limits<int>::max(), 48}};

    for (const auto& row : cases) {
        auto sys = usb_system();
        sys["sourceType"] = "airspy";
        SessionArgPrefs prefs;
        prefs.bandwidthKhz = row.requested;
        const QString expected = QStringLiteral("airspy:851.375M:%1:0:2").arg(row.expected);
        SessionArgsError error;
        expect("Airspy preference bandwidth snaps to a supported spec",
               input_spec(session_args_build(sys, prefs, &error)) == expected && error == SessionArgsError::None);
        QString scanError;
        expect("Airspy scan preference bandwidth snaps to a supported spec",
               input_spec(dsd_qt::session_args_scan_build(sys, "851.375", "scan.csv", prefs, &scanError)) == expected
                   && scanError.isEmpty());
        if (row.requested > 0) {
            sys["bandwidthKhz"] = row.requested;
            prefs.bandwidthKhz = 6;
            expect("Airspy system bandwidth overrides preference and snaps",
                   input_spec(session_args_build(sys, prefs, &error)) == expected && error == SessionArgsError::None);
            expect("Airspy scan bandwidth overrides preference and snaps",
                   input_spec(dsd_qt::session_args_scan_build(sys, "851.375", "scan.csv", prefs, &scanError))
                           == expected
                       && scanError.isEmpty());
        }
    }
    auto sys = usb_system();
    SessionArgPrefs prefs;
    prefs.bandwidthKhz = 25;
    expect("USB bandwidth retains its existing spec",
           input_spec(session_args_build(sys, prefs, nullptr)) == "rtl:0:851.375M:30:0:25:0:2");
    sys["sourceType"] = "rtltcp";
    sys["host"] = "localhost";
    sys["port"] = 1234;
    expect("RTL-TCP bandwidth retains its existing spec",
           input_spec(session_args_build(sys, prefs, nullptr)) == "rtltcp:localhost:1234:851.375M:30:0:25:0:2");
    /* 72 kHz reaches the RTL spec as it is: the engine runs TETRA there. */
    prefs.bandwidthKhz = 72;
    expect("RTL-TCP carries 72 kHz",
           input_spec(session_args_build(sys, prefs, nullptr)) == "rtltcp:localhost:1234:851.375M:30:0:72:0:2");
    auto usb = usb_system();
    usb["bandwidthKhz"] = 72;
    usb["decodeFlag"] = "-fT";
    expect("USB carries 72 kHz",
           input_spec(session_args_build(usb, SessionArgPrefs(), nullptr)) == "rtl:0:851.375M:30:0:72:0:2");
}

void
test_csv_args(void) {
    SessionArgsError error = SessionArgsError::None;

    /* CSV paths must be discrete argv elements: the extraArgs field is
     * whitespace-split, so a path with a space can only survive here. */
    QVariantMap sys = usb_system();
    sys.insert(QStringLiteral("chanCsvPath"), QStringLiteral("/data/imports/chan map.csv"));
    sys.insert(QStringLiteral("groupCsvPath"), QStringLiteral("/data/imports/county.csv"));
    sys.insert(QStringLiteral("keyCsvPath"), QStringLiteral("/data/imports/keys.csv"));
    sys.insert(QStringLiteral("keyCsvHex"), false);
    sys.insert(QStringLiteral("p25BandplanCsvPath"), QStringLiteral("/data/imports/band plan.csv"));
    sys.insert(QStringLiteral("srcCsvPath"), QStringLiteral("/data/imports/radio IDs.csv"));
    sys.insert(QStringLiteral("extraArgs"), QStringLiteral("--wav-dir /tmp"));
    const QStringList args = session_args_build(sys, SessionArgPrefs(), &error);
    expect("csv build succeeds", error == SessionArgsError::None);
    qsizetype at = args.indexOf(QStringLiteral("--p25-bandplan"));
    expect("band plan path follows --p25-bandplan intact",
           at >= 0 && at + 1 < args.size() && args.at(at + 1) == QStringLiteral("/data/imports/band plan.csv"));
    at = args.indexOf(QStringLiteral("--src-csv"));
    expect("source path follows --src-csv intact",
           at >= 0 && at + 1 < args.size() && args.at(at + 1) == QStringLiteral("/data/imports/radio IDs.csv"));
    at = args.indexOf(QStringLiteral("-C"));
    expect("chan path follows -C intact",
           at >= 0 && at + 1 < args.size() && args.at(at + 1) == QStringLiteral("/data/imports/chan map.csv"));
    at = args.indexOf(QStringLiteral("-G"));
    expect("group path follows -G",
           at >= 0 && at + 1 < args.size() && args.at(at + 1) == QStringLiteral("/data/imports/county.csv"));
    at = args.indexOf(QStringLiteral("-k"));
    expect("dec keys use -k",
           at >= 0 && at + 1 < args.size() && args.at(at + 1) == QStringLiteral("/data/imports/keys.csv"));
    expect("dec keys never emit -K", !args.contains(QStringLiteral("-K")));
    expect("csv args coexist with extra args", args.contains(QStringLiteral("--wav-dir")));

    sys.insert(QStringLiteral("keyCsvHex"), true);
    const QStringList hexArgs = session_args_build(sys, SessionArgPrefs(), &error);
    expect("hex keys use -K", hexArgs.contains(QStringLiteral("-K")));
    expect("hex keys never emit -k", !hexArgs.contains(QStringLiteral("-k")));

    /* Absent or empty fields emit nothing — legacy systems keep their argv. */
    const QStringList bare = session_args_build(usb_system(), SessionArgPrefs(), &error);
    expect("no csv fields emit no csv flags",
           !bare.contains(QStringLiteral("-C")) && !bare.contains(QStringLiteral("-G"))
               && !bare.contains(QStringLiteral("-k")) && !bare.contains(QStringLiteral("-K"))
               && !bare.contains(QStringLiteral("--p25-bandplan")) && !bare.contains(QStringLiteral("--src-csv")));
}

void
test_ppm_shapes(void) {
    SessionArgsError error = SessionArgsError::None;
    QVariantMap sys = usb_system();
    sys.insert(QStringLiteral("ppm"), QStringLiteral("+5"));
    const QStringList plus = session_args_build(sys, SessionArgPrefs(), &error);
    expect("explicit plus sign is normalized",
           error == SessionArgsError::None && input_spec(plus) == QStringLiteral("rtl:0:851.375M:30:5:48:0:2"));

    sys.insert(QStringLiteral("ppm"), QStringLiteral("abc"));
    expect("junk ppm refuses the build",
           session_args_build(sys, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::Ppm);
}

void
test_bias_tee_tristate(void) {
    SessionArgsError error = SessionArgsError::None;
    SessionArgPrefs prefOn;
    prefOn.biasTee = true;
    SessionArgPrefs prefOff;

    QVariantMap sys = usb_system();
    // Follow (-1): the app-wide pref decides.
    sys.insert(QStringLiteral("biasTee"), -1);
    expect("follow + pref on powers the tee",
           input_spec(session_args_build(sys, prefOn, &error)).endsWith(QStringLiteral(":bias")));
    expect("follow + pref off leaves the tee off",
           !input_spec(session_args_build(sys, prefOff, &error)).endsWith(QStringLiteral(":bias")));

    // Explicit off (0) must survive a global on — the hardware-protection case.
    sys.insert(QStringLiteral("biasTee"), 0);
    expect("explicit off beats the app-wide on",
           !input_spec(session_args_build(sys, prefOn, &error)).endsWith(QStringLiteral(":bias")));

    // Explicit on (1) needs no global blessing.
    sys.insert(QStringLiteral("biasTee"), 1);
    expect("explicit on works with the pref off",
           input_spec(session_args_build(sys, prefOff, &error)).endsWith(QStringLiteral(":bias")));

    // Legacy bool stores: true was an explicit choice, false the untouched
    // default whose observed behavior followed the pref.
    sys.insert(QStringLiteral("biasTee"), true);
    expect("legacy true powers the tee",
           input_spec(session_args_build(sys, prefOff, &error)).endsWith(QStringLiteral(":bias")));
    sys.insert(QStringLiteral("biasTee"), false);
    expect("legacy false keeps following the pref",
           input_spec(session_args_build(sys, prefOn, &error)).endsWith(QStringLiteral(":bias")));
}

void
test_network_and_file_sources(void) {
    SessionArgsError error = SessionArgsError::None;

    QVariantMap rtltcp = usb_system();
    rtltcp.insert(QStringLiteral("sourceType"), QStringLiteral("rtltcp"));
    rtltcp.insert(QStringLiteral("host"), QStringLiteral("192.168.1.10"));
    rtltcp.insert(QStringLiteral("port"), 1234);
    rtltcp.insert(QStringLiteral("biasTee"), 1);
    expect("rtltcp spec carries host, port, tail and bias",
           input_spec(session_args_build(rtltcp, SessionArgPrefs(), &error))
               == QStringLiteral("rtltcp:192.168.1.10:1234:851.375M:30:0:48:0:2:bias"));

    QVariantMap udp;
    udp.insert(QStringLiteral("sourceType"), QStringLiteral("udp"));
    udp.insert(QStringLiteral("port"), 7355);
    expect("udp spec binds all interfaces",
           input_spec(session_args_build(udp, SessionArgPrefs(), &error)) == QStringLiteral("udp:0.0.0.0:7355"));
    expect("udp needs no frequency", error == SessionArgsError::None);

    QVariantMap tcp;
    tcp.insert(QStringLiteral("sourceType"), QStringLiteral("tcp"));
    tcp.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    tcp.insert(QStringLiteral("port"), 7355);
    expect("tcp spec carries host and port",
           input_spec(session_args_build(tcp, SessionArgPrefs(), &error)) == QStringLiteral("tcp:127.0.0.1:7355"));

    QVariantMap file;
    file.insert(QStringLiteral("sourceType"), QStringLiteral("file"));
    file.insert(QStringLiteral("filePath"), QStringLiteral("/sdcard/capture.wav"));
    expect("file source passes the path through",
           input_spec(session_args_build(file, SessionArgPrefs(), &error)) == QStringLiteral("/sdcard/capture.wav"));
}

void
test_keys() {
    using namespace dsd_qt;
    const QStringList types{"basic", "hex", "rc4", "scrambler"};
    const QStringList values{"0", "0011223344", "ABC", "32767"};
    const QStringList flags{"-b", "-H", "-1", "-R"};
    for (int i = 0; i < types.size(); ++i) {
        auto sys = usb_system();
        sys["encKeyType"] = types[i];
        sys["encKeyValue"] = values[i];
        sys["encForceKey"] = i % 3;
        SessionArgsError error;
        const auto args = session_args_build(sys, SessionArgPrefs(), &error);
        const auto at = args.indexOf(flags[i]);
        expect("direct key is a discrete argument",
               error == SessionArgsError::None && at >= 0 && at + 1 < args.size() && args[at + 1] == values[i]);
        expect("force flags are discrete", args.contains("-4") == (i % 3 == 1) && args.contains("-0") == (i % 3 == 2));
        sys["keyCsvPath"] = "keys.csv";
        expect("direct key and CSV refused",
               session_args_build(sys, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::KeyConflict);
    }
    expect("hex normalization", session_args_key_hex_normalize(" 0x ab cd\t ") == "ABCD");
    expect("basic bounds", session_args_key_valid("basic", "255") && !session_args_key_valid("basic", "256")
                               && !session_args_key_valid("basic", "-1"));
    expect("hex widths", session_args_key_valid("hex", QString(32, '0'))
                             && session_args_key_valid("hex", QString(64, '0'))
                             && !session_args_key_valid("hex", QString(11, '0')));
    expect("rc4 bounds", session_args_key_valid("rc4", "0") && session_args_key_valid("rc4", QString(16, 'F'))
                             && !session_args_key_valid("rc4", QString(17, 'F')));
    expect("scrambler bounds",
           session_args_key_valid("scrambler", "0") && !session_args_key_valid("scrambler", "32768"));
    expect("invalid types and text", !session_args_key_valid("other", "0") && !session_args_key_valid("basic", "1junk")
                                         && !session_args_key_valid("rc4", "xyz"));
    auto sys = usb_system();
    sys["encKeyValue"] = "orphan";
    SessionArgsError error;
    expect("orphan key refused", session_args_build(sys, SessionArgPrefs(), &error).isEmpty());
}

void
test_retained_key_validation_boundary() {
    dsd_qt::SavedSystemsModel systems;
    auto sys = usb_system();
    const QString secret = QStringLiteral("C1D2E3F405");
    sys["encKeyType"] = "rc4";
    sys["encKeyValue"] = secret;
    systems.add(sys);
    dsd_qt::SessionArgsBuilder builder(nullptr);
    builder.setSavedSystems(&systems);
    const auto result = builder.build(systems.get(systems.count() - 1));
    expect("retained key validates without returning secret arguments",
           result.value("ok").toBool() && !result.contains("args")
               && !QJsonDocument::fromVariant(result).toJson().contains(secret.toUtf8()));

    class Host : public dsd_qt::DecoderHost {
      public:
        QStringList received;

        bool
        isRunning() const override {
            return false;
        }

        QString
        statusText() const override {
            return {};
        }

        bool
        start(const QStringList& args) override {
            received = args;
            return true;
        }

        void
        stop() override {}
    } host;

    auto started = builder.start(systems.get(systems.count() - 1), &host, QStringLiteral("saved"));
    const int key = host.received.indexOf("-1");
    expect("C++ start resolves the retained key into host arguments",
           started.value("started").toBool() && key >= 0 && host.received.value(key + 1) == secret);
    expect("start result never exposes private arguments",
           !started.contains("args") && !QJsonDocument::fromVariant(started).toJson().contains(secret.toUtf8()));
    host.received.clear();
    sys["freqMhz"] = "invalid";
    started = builder.start(sys, &host, QStringLiteral("saved"));
    expect("invalid session never starts",
           !started.value("ok").toBool() && !started.value("started").toBool() && host.received.isEmpty());
}

void
test_extra_short_options(void) {
    // Grouping is refused in both gates, including groups ending in an option
    // with an attached argument. The single-option attached form stays usable.
    for (const auto& token :
         QStringList{"-FT", "-FY", "-Fiother-input", "-FTZ", "-FCT.csv", "-Ff1", "-Fmq", "-Fv2", "-FZ"}) {
        expect("shared gate refuses grouped short options", !dsd_qt::session_args_extra_safe(token));
        auto sys = usb_system();
        sys["extraArgs"] = token;
        SessionArgsError error = SessionArgsError::None;
        expect("saved extra group refuses argv",
               session_args_build(sys, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::UnsafeOption);
        sys.remove("extraArgs");
        SessionArgPrefs prefs;
        prefs.extraArgs = token;
        expect("preference extra group refuses argv",
               session_args_build(sys, prefs, &error).isEmpty() && error == SessionArgsError::UnsafeOption);
    }
    expect("rejection explains separate short options",
           dsd_qt::session_args_error_text(SessionArgsError::UnsafeOption).contains("each short option separately"));
    for (const auto& token : QStringList{"-F -e -v2", "-GFT.csv", "-ft", "-mq", "--enc-lockout"}) {
        expect("single options and attached arguments remain accepted", dsd_qt::session_args_extra_safe(token));
        auto sys = usb_system();
        sys["extraArgs"] = token;
        SessionArgsError error = SessionArgsError::UnsafeOption;
        expect("permitted extra options still build argv",
               !session_args_build(sys, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::None);
    }
}

/* Every error value the builder can report has a sentence for the rejection UI (the Qt start error shows it), and
 * None has none: a value without its text would show an empty or generic start error. */
void
test_every_error_has_text() {
    const auto last = static_cast<int>(dsd_qt::SessionArgsErrorLast);
    expect("None has no text", dsd_qt::session_args_error_text(SessionArgsError::None).isEmpty());
    for (int value = static_cast<int>(SessionArgsError::None) + 1; value <= last; value++) {
        if (dsd_qt::session_args_error_text(static_cast<SessionArgsError>(value)).trimmed().isEmpty()) {
            DSD_FPRINTF(stderr, "FAIL: SessionArgsError %d has no error text\n", value);
            g_failures++;
        }
    }
    expect("the last error value is an extra -P with no storage for call recordings",
           dsd_qt::SessionArgsErrorLast == SessionArgsError::CallRecordingNoStorage);
    expect("a value past the last one has no text",
           dsd_qt::session_args_error_text(static_cast<SessionArgsError>(last + 1)).isEmpty());
}

} // namespace

static void
test_radio_settings_args() {
    auto system = usb_system();
    system["sourceType"] = "rtltcp";
    system["host"] = "127.0.0.1";
    system["port"] = 1234;
    system["decodeFlag"] = "-fA";
    system["extraArgs"] = "-f1 -mg --squelch=-80";
    SessionArgPrefs prefs;
    prefs.extraArgs = "-mq --squelch off";
    const auto legacy = session_args_build(system, prefs, nullptr);
    expect("advanced-option baseline is valid", !legacy.isEmpty());
    system["radioSettings"] = QVariantMap{};
    expect("legacy systems keep their argument order", session_args_build(system, prefs, nullptr) == legacy);
    for (const auto& sql : QStringList{"-65", "off", "auto+12", "noise+8", "1"}) {
        system["radioSettings"] = QVariantMap{{"decodeFlag", "-fA"}, {"modulation", 0}, {"squelch", sql}};
        const auto args = session_args_build(system, prefs, nullptr);
        expect("confirmed controls override old system and global extras in decoder/modulation/squelch order",
               args == legacy + QStringList{"-ma", "-fA", "-mc", "--squelch", sql});
    }
    system["decodeFlag"] = "";
    system["radioSettings"] = QVariantMap{{"decodeFlag", ""}};
    const auto automatic = session_args_build(system, prefs, nullptr);
    expect("Auto clears any old manual modulation lock and explicitly resets the decoder",
           automatic.mid(automatic.size() - 2) == QStringList{"-ma", "-fa"});
    system["decodeFlag"] = "-fA";
    for (const auto& bad : QList<QVariant>{QStringLiteral("--show-keys"), QVariantMap{{"modulation", -1}},
                                           QVariantMap{{"squelch", "noise+99"}}, QVariantMap{{"decodeFlag", "-fM"}}}) {
        system["radioSettings"] = bad;
        SessionArgsError error = SessionArgsError::None;
        expect("malformed or inconsistent saved controls cannot bypass start validation",
               session_args_build(system, prefs, &error).isEmpty() && error == SessionArgsError::UnsafeOption);
    }
}

/* Grouped hex key material (issue reports: byte-spaced, NBSP, '-'/':' groups, 0X) is the key the decoder takes, on
   every hex type and in argv; decimal keys keep their own form. The profile protocol rule is shared with QML. */
static void
test_grouped_hex_keys() {
    const QString plain = QStringLiteral("0123456789ABCDEF0000000000000000FEDCBA98765432101122334455667788");
    QString grouped = QStringLiteral("0X");
    for (int at = 0; at < plain.size(); at += 2) {
        if (at) {
            grouped += at % 16 == 0 ? QChar(0x00A0) : at % 8 == 0 ? QChar('-') : at % 4 == 0 ? QChar(':') : QChar(' ');
        }
        grouped += plain.mid(at, 2).toLower();
    }
    expect("grouped hex normalises", dsd_qt::session_args_key_hex_normalize(grouped) == plain);
    expect("Unicode spaces normalise", dsd_qt::session_args_key_hex_normalize(QStringLiteral("0x12") + QChar(0x2009)
                                                                              + "ab" + QChar(0x3000) + "CD\t")
                                           == "12ABCD");
    expect("grouped hex is valid", dsd_qt::session_args_key_valid("hex", grouped)
                                       && dsd_qt::session_args_key_valid("m17Aes", grouped)
                                       && dsd_qt::session_args_key_valid("rc4", "12:34-56 78"));
    expect("hex types normalise", dsd_qt::session_args_key_value_normalize("m17Scrambler", "0X12-34") == "1234"
                                      && dsd_qt::session_args_key_value_normalize("hex", grouped) == plain);
    // Decimal kinds: trimmed only, no grouping (baseline).
    expect("decimal keys trim", dsd_qt::session_args_key_value_normalize("basic", " 73 ") == "73");
    expect("decimal keys take no grouping",
           !dsd_qt::session_args_key_valid("basic", "1-2") && !dsd_qt::session_args_key_valid("scrambler", "1:2"));
    auto sys = usb_system();
    sys["encKeyType"] = "hex";
    sys["encKeyValue"] = grouped;
    SessionArgsError error = SessionArgsError::None;
    const auto args = session_args_build(sys, SessionArgPrefs(), &error);
    const auto at = args.indexOf("-H");
    expect("grouped key starts as plain hex",
           error == SessionArgsError::None && at >= 0 && args.value(at + 1) == plain);

    // EncryptionEditor's protocol rule in C++: a grouped NXDN AES-256 key is accepted, a grouped 10-digit P25 key is
    // not. The four-argument form QML already calls stays.
    dsd_qt::SessionArgsBuilder builder(nullptr);
    expect("grouped NXDN AES-256 accepted", builder.keyError("hex", grouped, "", 0, "nxdn").isEmpty());
    expect("grouped 10-digit P25 hex refused",
           builder.keyError("hex", "12 34-56:78 90", "", 0, "p25") == "P25 AES needs 32 or 64 hex digits.");
    expect("NXDN AES needs 64 digits",
           builder.keyError("hex", QString(32, 'A'), "", 0, "nxdn") == "NXDN AES needs 64 hex digits.");
    expect("no protocol, no protocol rule", builder.keyError("hex", "12 34-56:78 90", "", 0).isEmpty()
                                                && builder.keyError("hex", "12 34-56:78 90", "", 0, "dmr").isEmpty());
    expect("shape errors still apply", builder.keyError("hex", QString(11, 'A'), "", 0, "dmr")
                                           == dsd_qt::session_args_error_text(SessionArgsError::KeyHex));
    expect("QML keeps the four-argument keyError",
           dsd_qt::SessionArgsBuilder::staticMetaObject.indexOfMethod("keyError(QString,QString,QString,int)") >= 0
               && dsd_qt::SessionArgsBuilder::staticMetaObject.indexOfMethod(
                      "keyError(QString,QString,QString,int,QString)")
                      >= 0);
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir data;
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    /* AppPrefs persists through QSettings: keep it in the scratch directory. */
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, data.path());
    test_retained_key_validation_boundary();
    test_keys();
    test_extra_short_options();
    for (const auto& token : {QStringLiteral("--show-keys"), QStringLiteral("--show-keys=1")}) {
        expect("shared scan token gate refuses key display", !dsd_qt::session_args_extra_safe(token));
        auto sys = usb_system();
        sys.insert(QStringLiteral("extraArgs"), token);
        expect("key display is refused", session_args_build(sys, SessionArgPrefs(), nullptr).isEmpty());
        sys.remove(QStringLiteral("extraArgs"));
        SessionArgPrefs prefs;
        prefs.extraArgs = token;
        expect("global key display is refused", session_args_build(sys, prefs, nullptr).isEmpty());
    }
    test_freq_validation();
    test_hangtime();
    test_tg_lockout_preference();
    test_relaxed_crc_preference();
    test_relaxed_crc_app_preference();
    test_recording_sessions();
    test_prefs_mapping();
    test_recording_args();
    test_recording_refused_extras();
    test_defaults_and_overrides();
    test_nfm_system();
    test_am_system();
    test_am_bandwidth();
    test_every_error_has_text();
    test_airspy_bandwidth();
    test_csv_args();
    test_ppm_shapes();
    test_bias_tee_tristate();
    test_network_and_file_sources();
    test_explore_system();
    test_radio_settings_args();
    test_grouped_hex_keys();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
