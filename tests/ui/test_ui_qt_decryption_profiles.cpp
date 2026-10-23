// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QIODevice>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/log.h>
#include <qflags.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "decoder_host.h"
#include "decryption_profiles_model.h"
#include "json_store.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "session_args.h"

static void
expect(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "Decryption profile regression failed at line %d\n", line);
        std::abort();
    }
}

#define check(condition) expect(!!(condition), __LINE__)

void
LFSRN(const char*, char*, dsd_state*) {}

/* A legacy ProVoice (-fp) or dPMR (-fm) system's key migrates as a mixed profile, so a key of any type stays usable.
   Read as dPMR, a ProVoice key of any type but the dPMR scrambler failed the profile's own validation and stopped the
   migration of every later system. */
static void
test_unprofiled_legacy_migration(dsd_qt::DecryptionProfilesModel& profiles, dsd_qt::SavedSystemsModel& systems,
                                 const QString& secret) {
    for (const char* flag : {"-fp", "-fm"}) {
        check(systems.add({{"name", QStringLiteral("Legacy %1").arg(QLatin1String(flag))},
                           {"sourceType", "rtltcp"},
                           {"host", "127.0.0.1"},
                           {"port", 1234},
                           {"freqMhz", "856.0"},
                           {"decodeFlag", flag},
                           {"encKeyType", "hex"},
                           {"encKeyValue", secret},
                           {"encForceKey", 0}}));
        const QString uid = systems.get(systems.count() - 1).value("uid").toString();
        const int migrated = profiles.count();
        check(profiles.migrateLegacySystems());
        check(profiles.count() == migrated + 1
              && systems.getByUid(uid).value("decryptionProfileUid").toString() == uid);
        check(profiles.get(uid).value("protocol").toString() == "mixed");
        QString error;
        check(profiles.configuration(uid, &error).value("encKeyValue").toString() == secret && error.isEmpty());
    }
}

/* The protocol session_args_profile_compatible() expects for @p flags: the one profile protocol it accepts, or mixed
   when it accepts every one. */
static QString
session_expected_protocol(const QString& flags) {
    QStringList accepted;
    for (const char* protocol : {"p25", "dmr", "nxdn", "dpmr", "m17", "dstar", "ysf"}) {
        if (dsd_qt::session_args_profile_compatible({{"decryptionProtocol", protocol}, {"decodeFlag", flags}})) {
            accepted << QLatin1String(protocol);
        }
    }
    return accepted.size() == 7 ? QStringLiteral("mixed") : accepted.value(0);
}

/* Util.decryptionProtocol() steers the QML editor and selector; session_args_profile_compatible() decides at start.
   They must read ProVoice (-fp) and dPMR (-fm) alike: both mixed, ProVoice because no profile protocol describes it,
   dPMR because an NXDN profile's scrambler also serves it. */
static void
test_qml_and_session_protocols_agree() {
    QFile file(QStringLiteral(DSD_QML_UI_DIR "/Util.js"));
    check(file.open(QIODevice::ReadOnly));
    QString source = QString::fromUtf8(file.readAll());
    source.remove(QStringLiteral(".pragma library"));
    QJSEngine engine;
    engine.installExtensions(QJSEngine::TranslationExtension);
    check(!engine.evaluate(source, file.fileName()).isError());
    QJSValue decryptionProtocol = engine.globalObject().property(QStringLiteral("decryptionProtocol"));
    check(decryptionProtocol.isCallable());
    for (const char* flag : {"-fp", "-fm"}) {
        const QString qml = decryptionProtocol.call({QJSValue(QLatin1String(flag))}).toString();
        check(qml == "mixed" && session_expected_protocol(QLatin1String(flag)) == qml);
    }
    // The cross-check itself tells protocols apart.
    check(decryptionProtocol.call({QJSValue(QStringLiteral("-fs"))}).toString() == "dmr"
          && session_expected_protocol(QStringLiteral("-fs")) == "dmr");
}

namespace {
class ReplayTestHost : public dsd_qt::DecoderHost {
  public:
    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return {};
    }

    bool
    start(const QStringList&) override {
        return false;
    }

    void
    stop() override {}
};
} // namespace

static void
test_replay_migration(dsd_qt::SavedSystemsModel& systems) {
    ReplayTestHost host;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    check(QDir().mkpath(cache));
    const QString oldPath = cache + "/legacy.wav";
    QFile file(oldPath);
    const QByteArray contents("test replay copy");
    check(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size());
    file.close();
    check(systems.add({{"name", "Replay migration"}, {"sourceType", "file"}, {"filePath", oldPath}}));
    const int index = systems.count() - 1;
    check(systems.migrateCachedReplayFiles(&host));
    const QString durable = systems.get(index).value("filePath").toString();
    check(durable != oldPath && QFile::exists(oldPath));
    check(host.documentInfo(durable).value("sizeBytes").toLongLong() == contents.size());
    check(QFile::remove(oldPath));
    dsd_qt::SavedSystemsModel reopened;
    check(reopened.get(index).value("filePath").toString() == durable && QFile::exists(durable));
    check(systems.migrateCachedReplayFiles(&host));
    check(systems.get(index).value("filePath").toString() == durable);
}

/* Issue #526: an analog (nfm or am) scan entry keeps the decryption choice the editor hides on it, but it uses no
   profile, so a profile only analog entries still name counts no use and can be removed: a frequency entry on the am
   protocol and a saved -fM system entry here. A digital entry naming it still holds it. */
static void
test_analog_scan_entries_hold_no_profile(dsd_qt::DecryptionProfilesModel& profiles, dsd_qt::SavedSystemsModel& systems,
                                         dsd_qt::ScanListsModel& scans, const QVariantMap& key) {
    check(profiles
              .saveProfile({{"uid", "profile-analog"},
                            {"label", "Hidden on analog"},
                            {"protocol", "p25"},
                            {"mode", "automatic"},
                            {"keys", QVariantList{key}}})
              .value("ok")
              .toBool());
    check(systems.add({{"name", "Tower"},
                       {"sourceType", "rtltcp"},
                       {"host", "127.0.0.1"},
                       {"port", 1234},
                       {"freqMhz", "118.3"},
                       {"decodeFlag", "-fM"}}));
    const QString towerUid = systems.get(systems.count() - 1).value("uid").toString();
    const auto named = [](QVariantMap entry) {
        entry.insert("decryptionMode", "profile");
        entry.insert("decryptionProfileUid", "profile-analog");
        return entry;
    };
    const QVariantList analog{named({{"kind", "freq"}, {"protocol", "am"}, {"freqMhz", "118.3"}}),
                              named({{"kind", "freq"}, {"protocol", "nfm"}, {"freqMhz", "154.43"}}),
                              named({{"kind", "system"}, {"systemUid", towerUid}})};
    check(scans.add({{"name", "Mixed"}, {"entries", analog}}));
    const int list = scans.count() - 1;
    check(profiles.useCount("profile-analog") == 0);

    QVariantList mixed = analog;
    mixed << named({{"kind", "freq"}, {"protocol", "p25"}, {"freqMhz", "851.0125"}});
    check(scans.update(list, {{"entries", mixed}}));
    check(profiles.useCount("profile-analog") == 1);
    check(!profiles.removeProfile("profile-analog").value("ok").toBool());

    check(scans.update(list, {{"entries", analog}}));
    check(profiles.removeProfile("profile-analog").value("ok").toBool());
    check(scans.remove(list));
}

static QByteArray
storeBytes() {
    QFile file(dsd_qt::json_store_path("decryption_profiles.json"));
    check(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

static void
seedStore(const QJsonArray& rows) {
    check(dsd_qt::json_store_save_array(QStringLiteral("decryption_profiles.json"), rows));
}

static QStringList
compiledFiles() {
    QStringList files;
    QDirIterator it(dsd_qt::json_store_path("decryption"), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        files << it.next();
    }
    files.sort();
    return files;
}

static QJsonObject
storedRow(const QString& uid) {
    for (const auto& value : QJsonDocument::fromJson(storeBytes()).array()) {
        if (value.toObject().value("uid").toString() == uid) {
            return value.toObject();
        }
    }
    return {};
}

static const char kResaveError[] = "Open this decryption profile and save it again.";

/* Warnings the model logs, kept by the process-lifetime log tap main() installs. */
static QStringList&
warnings() {
    static QStringList captured;
    return captured;
}

static void
captureWarning(dsd_neo_log_level_t level, const char* text, void*) {
    if (level == LOG_LEVEL_WARN) {
        warnings().append(QString::fromUtf8(text));
    }
}

static int
warningsContaining(const QString& text) {
    return static_cast<int>(std::count_if(warnings().cbegin(), warnings().cend(),
                                          [&text](const QString& warning) { return warning.contains(text); }));
}

static QStringList
filesUnder(const QString& uid) {
    QStringList files = compiledFiles();
    const QString prefix = dsd_qt::json_store_path("decryption/" + uid + "/");
    files.erase(
        std::remove_if(files.begin(), files.end(), [&prefix](const QString& file) { return !file.startsWith(prefix); }),
        files.end());
    return files;
}

/* Rows saveProfile() wrote carry their compiled form: loading them compiles nothing and writes nothing. */
static void
test_compiled_rows_load_unchanged() {
    const QByteArray before = storeBytes();
    const QStringList files = compiledFiles();
    check(!before.isEmpty() && !files.isEmpty());
    dsd_qt::DecryptionProfilesModel loaded;
    check(loaded.count() > 0);
    check(storeBytes() == before && compiledFiles() == files);
}

/* Issue #580: a restored store carries profile definitions only. Loading compiles each through the editor's own
   validation into fresh files under decryption/<uid>/ and persists the compiled form with a runtime reference; a row
   that cannot compile (its key CSV did not come back) stays uncompiled and refuses to start. A row that already has a
   compiled form is kept as stored. */
static void
test_definitions_compile_at_load(const QString& secret) {
    const QJsonObject compiledRow = storedRow("profile-a");
    check(compiledRow.value("compiled").isObject());
    const QJsonObject key{
        {"uid", "key-r"}, {"label", "Dispatch"}, {"keyId", 17}, {"kind", "aes128"}, {"material", secret}};
    seedStore({compiledRow,
               QJsonObject{{"uid", "restored-direct"},
                           {"label", "Restored direct"},
                           {"protocol", "p25"},
                           {"mode", "direct"},
                           {"force", 0},
                           {"directType", "hex"},
                           {"directValue", secret}},
               QJsonObject{{"uid", "restored-managed"},
                           {"label", "Restored managed"},
                           {"protocol", "p25"},
                           {"mode", "automatic"},
                           {"force", 0},
                           {"keySource", "managed"},
                           {"keys", QJsonArray{key}}},
               QJsonObject{{"uid", "restored-vendor"},
                           {"label", "Restored vendor"},
                           {"protocol", "dmr"},
                           {"mode", "vendor"},
                           {"vendorType", "tytEp"},
                           {"vendorValue", secret},
                           {"runtimeRef", "kept-reference"},
                           {"compiled", QJsonArray{"-5", "forged"}}},
               QJsonObject{{"uid", "restored-csv"},
                           {"label", "Restored CSV"},
                           {"protocol", "p25"},
                           {"mode", "automatic"},
                           {"keySource", "csv"},
                           {"legacyCsvPath", dsd_qt::json_store_path("imports/absent.csv")}},
               /* Its key collection is written before its talkgroup overrides are refused (DMR only). */
               QJsonObject{{"uid", "restored-mapped"},
                           {"label", "Restored mapped"},
                           {"protocol", "p25"},
                           {"mode", "automatic"},
                           {"keySource", "managed"},
                           {"keys", QJsonArray{key}},
                           {"mappings", QJsonArray{QJsonObject{{"talkgroup", 100}, {"keyId", 17}}}}}});
    warnings().clear();
    dsd_qt::DecryptionProfilesModel loaded;
    check(loaded.count() == 6);
    check(storedRow("profile-a") == compiledRow);
    check(warningsContaining("could not be compiled") == 2 && warningsContaining("restored-csv") == 1
          && warningsContaining("restored-mapped") == 1);
    check(warningsContaining(secret) == 0 && warningsContaining(secret.left(16)) == 0);

    QString error;
    const auto direct = loaded.configuration("restored-direct", &error);
    check(error.isEmpty() && direct.value("encKeyType").toString() == "hex"
          && direct.value("encKeyValue").toString() == secret);
    const QJsonObject directRow = storedRow("restored-direct");
    const QString directRef = directRow.value("runtimeRef").toString();
    check(!directRef.isEmpty() && directRow.value("compiled").isObject());
    check(directRow.value("compiled").toObject().value("decryptionProfileRef").toString() == directRef);
    check(direct.value("decryptionProfileRef").toString() == directRef);

    const auto managed = loaded.configuration("restored-managed", &error);
    check(error.isEmpty());
    const QString hexPath = managed.value("keysHexCsvPath").toString();
    check(hexPath.startsWith(dsd_qt::json_store_path("decryption/restored-managed/")) && QFile::exists(hexPath));
    dsd_key_set keys = {};
    check(dsd_key_set_load_csv(&keys, hexPath.toUtf8().constData(), nullptr, 0) == 0);
    check(keys.count == 2 && keys.entries[0].index == 17 && keys.entries[1].index == 17 + 0x101);
    dsd_key_set_free(&keys);
    const QJsonObject managedRow = storedRow("restored-managed");
    const QString managedRef = managedRow.value("runtimeRef").toString();
    check(!managedRef.isEmpty() && managedRef != directRef);
    check(managedRow.value("compiled").toObject().value("keysHexCsvPath").toString() == hexPath);
    check(loaded.forRuntimeReference(managedRef).value("uid").toString() == "restored-managed");

    const auto vendor = loaded.configuration("restored-vendor", &error);
    check(error.isEmpty());
    check(vendor.value("decryptionVendorArgs").toStringList()
          == (QStringList{"-5", secret.left(16) + ' ' + secret.mid(16)}));
    check(storedRow("restored-vendor").value("runtimeRef").toString() == "kept-reference");
    check(vendor.value("decryptionProfileRef").toString() == "kept-reference");

    check(loaded.configuration("restored-csv", &error).isEmpty() && error == kResaveError);
    const QJsonObject csvRow = storedRow("restored-csv");
    check(!csvRow.contains("compiled") && !csvRow.contains("runtimeRef"));
    check(loaded.get("restored-csv").value("label").toString() == "Restored CSV");

    check(loaded.configuration("restored-mapped", &error).isEmpty() && error == kResaveError);
    check(!storedRow("restored-mapped").contains("compiled") && filesUnder("restored-mapped").isEmpty());

    /* The persisted compiled forms are used as stored the next time: nothing is compiled again. */
    const QByteArray persisted = storeBytes();
    const QStringList files = compiledFiles();
    dsd_qt::DecryptionProfilesModel reopened;
    check(storeBytes() == persisted && compiledFiles() == files);
    check(reopened.configuration("restored-managed", &error) == managed && error.isEmpty());
    check(reopened.configuration("restored-csv", &error).isEmpty() && error == kResaveError);
}

/* A compiled form that cannot be persisted is not used: its files are removed and the row stays uncompiled in memory
   and on disk, so the next load tries again. The data directory is made read only so the store cannot be replaced
   while the profile directory stays writable. */
static void
test_load_compile_persist_failure(const QString& secret) {
    const QJsonObject key{
        {"uid", "key-p"}, {"label", "Dispatch"}, {"keyId", 18}, {"kind", "aes128"}, {"material", secret}};
    seedStore({QJsonObject{{"uid", "unsaved-managed"},
                           {"label", "Unsaved managed"},
                           {"protocol", "p25"},
                           {"mode", "automatic"},
                           {"keySource", "managed"},
                           {"keys", QJsonArray{key}}}});
    const QByteArray before = storeBytes();
    check(QDir().mkpath(dsd_qt::json_store_path("decryption")));
    const QStringList files = compiledFiles();
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto permissions = QFile::permissions(directory);
    check(QFile::setPermissions(directory, QFile::ReadOwner | QFile::ExeOwner));
    QString error;
    /* Root (or CAP_DAC_OVERRIDE) writes through the mode bits: probe before relying on them. */
    QFile probe(directory + "/write-probe");
    if (probe.open(QIODevice::WriteOnly)) {
        probe.close();
        check(probe.remove() && QFile::setPermissions(directory, permissions));
        std::fputs("SKIP: the read-only data directory is still writable; load persist-failure checks skipped\n",
                   stderr);
    } else {
        warnings().clear();
        {
            dsd_qt::DecryptionProfilesModel loaded;
            check(QFile::setPermissions(directory, permissions));
            check(loaded.count() == 1);
            check(loaded.configuration("unsaved-managed", &error).isEmpty() && error == kResaveError);
        }
        check(warningsContaining("could not be persisted") == 1 && warningsContaining(secret) == 0);
        check(storeBytes() == before && compiledFiles() == files);
    }
    dsd_qt::DecryptionProfilesModel retried;
    check(retried.configuration("unsaved-managed", &error)
              .value("keysHexCsvPath")
              .toString()
              .startsWith(dsd_qt::json_store_path("decryption/unsaved-managed/")));
    check(error.isEmpty() && storedRow("unsaved-managed").value("compiled").isObject());
}

/* Everything below @p dir, for checking that nothing was written there. */
static QStringList
entriesBelow(const QString& dir) {
    QStringList entries;
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        entries << it.next();
    }
    return entries;
}

/* Issue #580: key files are written in the clear, so a compile (at load or on save) never writes through a symlinked
   decryption/ or decryption/<uid>/: it is refused, nothing lands at the link's target, and the profile asks to be saved
   again. With the links gone the same row compiles. */
static void
test_compile_refuses_linked_directories(const QString& secret) {
    const QJsonObject key{
        {"uid", "key-l"}, {"label", "Dispatch"}, {"keyId", 19}, {"kind", "aes128"}, {"material", secret}};
    const QJsonObject row{{"uid", "linked-managed"}, {"label", "Linked managed"}, {"protocol", "p25"},
                          {"mode", "automatic"},     {"keySource", "managed"},    {"keys", QJsonArray{key}}};
    const QString decryption = dsd_qt::json_store_path("decryption");
    const QString aside = decryption + ".aside";
    check(QDir().mkpath(decryption) && QFile::rename(decryption, aside));
    QTemporaryDir outside;
    check(outside.isValid());
    const QString linkedUid = decryption + "/linked-managed";
    const QString linkedSave = decryption + "/linked-saved";

    // decryption/ itself a link, then decryption/<uid>/.
    for (const bool linkTop : {true, false}) {
        if (linkTop) {
            check(QFile::link(outside.path(), decryption));
        } else {
            check(QDir().mkdir(decryption) && QFile::link(outside.path(), linkedUid)
                  && QFile::link(outside.path(), linkedSave));
        }
        seedStore({row});
        {
            dsd_qt::DecryptionProfilesModel loaded;
            QString error;
            check(loaded.configuration("linked-managed", &error).isEmpty() && error == kResaveError);
            check(!storedRow("linked-managed").contains("compiled"));
            const QVariantMap saved = loaded.saveProfile({{"uid", "linked-saved"},
                                                          {"label", "Linked saved"},
                                                          {"protocol", "p25"},
                                                          {"mode", "automatic"},
                                                          {"keys", QVariantList{key.toVariantMap()}}});
            check(!saved.value("ok").toBool());
        }
        check(entriesBelow(outside.path()).isEmpty()); // nothing written through the link
        if (linkTop) {
            check(QFile::remove(decryption));
        } else {
            check(QFile::remove(linkedUid) && QFile::remove(linkedSave) && QDir().rmdir(decryption));
        }
    }
    check(QFile::rename(aside, decryption));
    dsd_qt::DecryptionProfilesModel unlinked;
    QString error;
    check(unlinked.configuration("linked-managed", &error)
              .value("keysHexCsvPath")
              .toString()
              .startsWith(dsd_qt::json_store_path("decryption/linked-managed/"))
          && error.isEmpty());
}

/* The words of a 64-digit key, as the hex importer stores them. */
static const unsigned long long kAes256Words[] = {0x0123456789ABCDEFULL, 0x0ULL, 0xFEDCBA9876543210ULL,
                                                  0x1122334455667788ULL};
static const char kAes256Key[] = "0123456789ABCDEF0000000000000000FEDCBA98765432101122334455667788";

/* The same key as a reporter pastes it: 0X prefix, byte-spaced with a no-break space, dash and colon groups. */
static QString
groupedAes256Key() {
    const QString plain = QLatin1String(kAes256Key);
    QString grouped = QStringLiteral("0X");
    for (int at = 0; at < plain.size(); at += 2) {
        if (at) {
            grouped += at % 16 == 0 ? QChar(0x00A0) : at % 8 == 0 ? QChar('-') : at % 4 == 0 ? QChar(':') : QChar(' ');
        }
        grouped += plain.mid(at, 2).toLower();
    }
    return grouped;
}

/* Load @p path through the decoder's own hex key importer and report whether key ID @p id holds @p words. */
static bool
importsAes256(const QString& path, quint32 id) {
    auto* state = static_cast<dsd_state*>(std::calloc(1, sizeof(dsd_state)));
    check(state != nullptr);
    dsd_csv_validation stats = {};
    const bool imported = csvKeyImportHexPath(path.toUtf8().constData(), 0, state, &stats) == 0;
    static const quint32 offsets[] = {0, 0x101, 0x201, 0x301};
    bool words = imported && stats.accepted == 1;
    for (int i = 0; i < 4; ++i) {
        words = words && state->rkey_array[id + offsets[i]] == kAes256Words[i]
                && state->rkey_array_loaded[id + offsets[i]] == 1U;
    }
    dsd_state_ext_free_all(state);
    std::free(state);
    return words;
}

/* A profile's hex material drops every Unicode space, '-' and ':' grouping and a 0x/0X prefix; vendor grammars keep
   their own rules, the direct-key protocol rule is session_args', and configuration() hands out normalised material. */
static void
test_hex_normalisation(dsd_qt::DecryptionProfilesModel& profiles) {
    // Vendor grammars are untouched (baseline): grouping they never took stays refused, their own forms still work.
    QString dashed = QLatin1String(kAes256Key).left(32);
    dashed.insert(8, '-');
    check(!profiles
               .saveProfile({{"uid", "vendor-grouped"},
                             {"label", "Vendor grouped"},
                             {"protocol", "dmr"},
                             {"mode", "vendor"},
                             {"vendorType", "tytEp"},
                             {"vendorValue", dashed}})
               .value("ok")
               .toBool());
    check(!profiles
               .saveProfile({{"uid", "vendor-grouped"},
                             {"label", "Vendor grouped"},
                             {"protocol", "dmr"},
                             {"mode", "vendor"},
                             {"vendorType", "kenwood"},
                             {"vendorValue", "1-2"}})
               .value("ok")
               .toBool());
    check(profiles
              .saveProfile({{"uid", "vendor-grouped"},
                            {"label", "Vendor grouped"},
                            {"protocol", "dmr"},
                            {"mode", "vendor"},
                            {"vendorType", "tytEp"},
                            {"vendorValue", " 0x" + QString::fromLatin1(kAes256Key).left(32).toLower() + " "}})
              .value("ok")
              .toBool());
    QString error;
    check(profiles.configuration("vendor-grouped", &error).value("decryptionVendorArgs").toStringList()
          == (QStringList{"-5", QLatin1String(kAes256Key).left(16) + ' ' + QLatin1String(kAes256Key).mid(16, 16)}));

    const auto direct = [&profiles](const char* uid, const char* protocol, const QString& value) {
        return profiles.saveProfile({{"uid", uid},
                                     {"label", uid},
                                     {"protocol", protocol},
                                     {"mode", "direct"},
                                     {"directType", "hex"},
                                     {"directValue", value}});
    };
    check(direct("direct-nxdn", "nxdn", groupedAes256Key()).value("ok").toBool());
    const auto refused = direct("direct-p25-short", "p25", "12 34-56:78 90");
    check(!refused.value("ok").toBool() && refused.value("error").toString() == "P25 AES needs 32 or 64 hex digits.");

    // configuration() hands the session and a live apply the material as the decoder takes it; the stored row keeps
    // what was typed.
    check(direct("direct-p25", "p25", groupedAes256Key()).value("ok").toBool());
    const auto config = profiles.configuration("direct-p25", &error);
    check(error.isEmpty() && config.value("encKeyType").toString() == "hex"
          && config.value("encKeyValue").toString() == QLatin1String(kAes256Key));
    check(storedRow("direct-p25").value("directValue").toString() == groupedAes256Key());
    check(profiles
              .saveProfile({{"uid", "direct-basic"},
                            {"label", "Direct basic"},
                            {"protocol", "dmr"},
                            {"mode", "direct"},
                            {"directType", "basic"},
                            {"directValue", " 73 "}})
              .value("ok")
              .toBool());
    check(profiles.configuration("direct-basic", &error).value("encKeyValue").toString() == "73");
}

/* A P25 AES-256 managed key compiles to a hex collection the decoder's own importer files under its key ID and the
   three AES segment cells, for key ID 0 and a non-decimal ID, from grouped material too. */
static void
test_aes256_profile_imports(dsd_qt::DecryptionProfilesModel& profiles) {
    for (const quint32 id : {0U, 0xBEEFU}) {
        const QString uid = QStringLiteral("aes256-%1").arg(id, 0, 16);
        check(profiles
                  .saveProfile({{"uid", uid},
                                {"label", "AES-256"},
                                {"protocol", "p25"},
                                {"mode", "automatic"},
                                {"keys", QVariantList{QVariantMap{{"uid", "key-aes256"},
                                                                  {"label", "Dispatch"},
                                                                  {"keyId", id},
                                                                  {"kind", "aes256"},
                                                                  {"lookup", "id"},
                                                                  {"material", groupedAes256Key()}}}}})
                  .value("ok")
                  .toBool());
        QString error;
        const QString path = profiles.configuration(uid, &error).value("keysHexCsvPath").toString();
        check(error.isEmpty() && importsAes256(path, id));
    }
}

/* Issue #580 restore: an AES-256 profile definition (grouped material) compiles at load into a collection the
   importer reads back whole. */
static void
test_aes256_definition_compiles_at_load() {
    const QJsonObject key{{"uid", "key-restored"}, {"label", "Dispatch"}, {"keyId", 0x2A},
                          {"kind", "aes256"},      {"lookup", "id"},      {"material", groupedAes256Key()}};
    seedStore({QJsonObject{{"uid", "restored-aes256"},
                           {"label", "Restored AES-256"},
                           {"protocol", "p25"},
                           {"mode", "automatic"},
                           {"force", 0},
                           {"keySource", "managed"},
                           {"keys", QJsonArray{key}}}});
    dsd_qt::DecryptionProfilesModel loaded;
    QString error;
    const QString path = loaded.configuration("restored-aes256", &error).value("keysHexCsvPath").toString();
    check(error.isEmpty() && storedRow("restored-aes256").value("compiled").isObject() && importsAes256(path, 0x2A));
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    dsd_test_qt_isolate_paths();
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel scans;
    dsd_qt::DecryptionProfilesModel profiles;
    profiles.setReferences(&systems, &scans);
    const QString secret = QStringLiteral("1234567890ABCDEF1122334455667788");
    const QVariantMap entry{
        {"uid", "key-a"}, {"label", "Dispatch"}, {"keyId", 17}, {"kind", "aes128"}, {"material", secret}};
    auto saved = profiles.saveProfile({{"uid", "profile-a"},
                                       {"label", "System A"},
                                       {"protocol", "p25"},
                                       {"mode", "automatic"},
                                       {"keys", QVariantList{entry}}});
    check(saved.value("ok").toBool());
    QString error;
    const auto config = profiles.configuration("profile-a", &error);
    check(error.isEmpty());
    const QByteArray metadata = QJsonDocument::fromVariant(profiles.get("profile-a")).toJson();
    check(!metadata.contains(secret.toUtf8()) && !metadata.contains("material"));
    dsd_key_set keys = {};
    check(dsd_key_set_load_csv(&keys, config.value("keysHexCsvPath").toString().toUtf8().constData(), nullptr, 0) == 0);
    check(keys.count == 2 && keys.entries[0].index == 17 && keys.entries[1].index == 17 + 0x101);
    dsd_key_set_free(&keys);
    const QVariantMap collision{
        {"uid", "key-b"}, {"label", "Other"}, {"keyId", 17 + 0x101}, {"kind", "scalar"}, {"material", "1234"}};
    saved = profiles.saveProfile({{"uid", "profile-a"}, {"keys", QVariantList{entry, collision}}});
    check(!saved.value("ok").toBool());
    check(profiles.get("profile-a").value("keyCount").toInt() == 1);
    check(profiles.configuration("profile-a", &error) == config);

    systems.add({{"name", "Legacy"},
                 {"sourceType", "rtltcp"},
                 {"host", "127.0.0.1"},
                 {"port", 1234},
                 {"freqMhz", "851.5"},
                 {"decodeFlag", "-ft"},
                 {"encKeyType", "hex"},
                 {"encKeyValue", secret},
                 {"encForceKey", 0}});
    const QString systemUid = systems.get(0).value("uid").toString();
    check(profiles.migrateLegacySystems());
    check(systems.get(0).value("decryptionProfileUid").toString() == systemUid);
    check(systems.keyValueForUid(systemUid).isEmpty());
    check(profiles.configuration(systemUid, &error).value("encKeyValue").toString() == secret);
    const int migratedCount = profiles.count();
    check(profiles.migrateLegacySystems() && profiles.count() == migratedCount);
    test_unprofiled_legacy_migration(profiles, systems, secret);
    QFile store(dsd_qt::json_store_path("saved_systems.json"));
    check(store.open(QIODevice::ReadOnly) && !store.readAll().contains(secret.toUtf8()));
    check(!profiles.removeProfile(systemUid).value("ok").toBool());
    dsd_qt::SessionArgsBuilder builder(nullptr);
    builder.setSavedSystems(&systems);
    builder.setDecryptionProfiles(&profiles);
    check(builder.build(systems.get(0)).value("ok").toBool());
    check(!QJsonDocument::fromVariant(builder.build(systems.get(0))).toJson().contains(secret.toUtf8()));

    const QString file = dsd_qt::json_store_path("decryption_profiles.json");
    check(QFile::rename(file, file + ".saved"));
    check(QDir().mkdir(file));
    saved = profiles.saveProfile({{"uid", "profile-a"}, {"label", "Failed rename"}});
    check(!saved.value("ok").toBool() && profiles.get("profile-a").value("label").toString() == "System A");
    check(profiles.configuration("profile-a", &error) == config);
    check(QDir().rmdir(file) && QFile::rename(file + ".saved", file));
    dsd_qt::DecryptionProfilesModel reopened;
    check(reopened.count() == profiles.count());
    check(reopened.configuration(systemUid, &error).value("encKeyValue").toString() == secret);
    saved = profiles.saveProfile({{"uid", "vendor-test"},
                                  {"label", "Vendor"},
                                  {"protocol", "dmr"},
                                  {"mode", "vendor"},
                                  {"vendorType", "tytEp"},
                                  {"vendorValue", secret}});
    check(saved.value("ok").toBool());
    const auto vendor = profiles.configuration("vendor-test", &error);
    check(vendor.value("decryptionVendorArgs").toStringList()
          == (QStringList{"-5", secret.left(16) + ' ' + secret.mid(16)}));
    check(!QJsonDocument::fromVariant(profiles.get("vendor-test")).toJson().contains(secret.left(16).toUtf8()));
    check(!profiles.saveProfile({{"uid", "vendor-test"}, {"vendorType", "csi"}}).value("ok").toBool());
    check(!profiles.saveProfile({{"uid", "vendor-test"}, {"vendorType", "kenwood"}, {"vendorValue", "32768"}})
               .value("ok")
               .toBool());
    check(profiles.configuration("vendor-test", &error) == vendor);
    check(!dsd_qt::session_args_profile_compatible({{"decryptionProtocol", "m17"}, {"decodeFlag", "-fs"}}));
    check(dsd_qt::session_args_profile_compatible({{"decryptionProtocol", "dmr"}, {"decodeFlag", "-fs"}}));
    /* ProVoice (-fp) has no profile protocol: it is not dPMR, so it takes any profile; dPMR (-fm) also takes an NXDN
       profile's scrambler. */
    check(dsd_qt::session_args_profile_compatible({{"decryptionProtocol", "nxdn"}, {"decodeFlag", "-fp"}}));
    check(dsd_qt::session_args_profile_compatible({{"decryptionProtocol", "nxdn"}, {"decodeFlag", "-fm"}}));
    test_qml_and_session_protocols_agree();
    test_hex_normalisation(profiles);
    test_aes256_profile_imports(profiles);
    test_replay_migration(systems);
    test_analog_scan_entries_hold_no_profile(profiles, systems, scans, entry);
    dsd_neo_log_set_tap(captureWarning, nullptr);
    test_compiled_rows_load_unchanged();
    test_definitions_compile_at_load(secret);
    test_load_compile_persist_failure(secret);
    test_compile_refuses_linked_directories(secret);
    test_aes256_definition_compiles_at_load();
    return 0;
}
