// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArrayView>
#include <QChar>
#include <QFlags>
#include <QHash>
#include <QIODevice>
#include <QLatin1String>
#include <QVariant>
#include <Qt>
#include <stddef.h>
#include <utility>
#include "diagnostics_log.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <limits>
#include <system_error>
#include <thread>
#include "realtime_clock.h"

namespace dsd_qt {
namespace {
constexpr int ringLimit = 2000;
constexpr int queueLimit = 2000;
constexpr int flushResultLimit = 64;
// At most this many ring lines are kept at the front through eviction (see appendRingLocked()).
constexpr int pinnedLimit = 8;
const char kTailName[] = "tail.log";
const char kPrevName[] = "tail.prev.log";
const char kHostName[] = "host.log";
const char kHostPrevName[] = "host.prev.log";
const char kPreviousRun[] = "--- previous run ---";
std::atomic<DiagnosticsLog::LoadHook> g_loadHook{nullptr};
constexpr QByteArrayView kProcessPrefix = "[process] [info] pid ";
constexpr QByteArrayView kSessionPrefix = "[session] [info] --- session starting";

// A record that arrived before installTap() published the log, formatted (prefix, redaction, bound) exactly as a
// submit() would have, so the producer-line rules see the same record.
struct PendingRecord {
    QByteArray record;
    bool host = false;
};

struct HostDiagnostics {
    std::mutex mutex;
    std::condition_variable published;
    QList<PendingRecord> pending;
    // Set by installTap(), on its calling thread: what the publish records, and where the log lives.
    bool installed = false;
    QString facts;
    QStringList following;
    QString directory;
    std::thread::id directoryThread;
    // A startup-window request that arrived before the publish.
    bool armPending = false;
    // Set once, under the mutex, after the pending records are in; read without it on the fast path.
    std::atomic<bool> ready{false};
    DiagnosticsLog::PlatformFactsProvider provider = nullptr;
    DiagnosticsLog::PlatformLinesLogged logged = nullptr;
};

HostDiagnostics&
hostDiagnostics() {
    // JNI producers may arrive before Qt initialization and outlive its teardown.
    static auto* host = new HostDiagnostics;
    return *host;
}

/** @brief The newest @p limit bytes of @p text, cut at a line start. */
QByteArray
lineAlignedTail(QByteArray text, qint64 limit) {
    if (text.size() > limit) {
        text = text.right(limit);
        text.remove(0, text.indexOf('\n') + 1);
    }
    return text;
}

/** @brief Drops an unterminated last record: what a crash part-way through an append leaves. */
QByteArray
withoutFragment(QByteArray text) {
    if (!text.isEmpty() && !text.endsWith('\n')) {
        text.truncate(text.lastIndexOf('\n') + 1);
    }
    return text;
}

/** @brief Up to @p limit trailing bytes of the file at @p path; empty if it cannot be read. */
QByteArray
readTail(const QString& path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    if (file.size() > limit && !file.seek(file.size() - limit)) {
        return {};
    }
    return file.readAll();
}

/** @brief Replaces @p path with @p bytes: data synced before the rename, the rename synced after it. */
bool
replaceFile(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        return false;
    }
    dsd_fsync_parent_dir(QFile::encodeName(path).constData());
    return true;
}

/** @brief The "boot N" field of a process record, or -1 when it has none. */
qint64
bootOf(const QByteArray& record) {
    static const QRegularExpression boot(QStringLiteral("\\bboot (\\d+)\\b"));
    const auto match = boot.match(QString::fromUtf8(record));
    return match.hasMatch() ? match.captured(1).toLongLong() : -1;
}

const char kDroppedRecord[] = "[redacted sensitive diagnostic]";

/**
 * @brief What marks a record as key-bearing: a key word, or a key-taking CLI option.
 *
 * getopt takes a short option's value attached or as the next argument, and bundles it behind options that take none
 * (`-ZT1F`), so a dash-led token counts when a key option's letter follows only options that take no argument
 * (src/runtime/cli/args.c's option string) or letters that are no option at all. After a letter that takes one, the
 * rest of the token is that letter's value: "-DMR" is -D with "MR". A decimal such as "-3.2 dB" is no option cluster:
 * getopt stops at the '.'. A host may echo arguments as a list or quoted, so a quote, `[` or `,` can lead the token too.
 * The long options without a key word in their name are listed.
 */
const QRegularExpression&
keyLabel() {
    static const QRegularExpression labelled(QStringLiteral(
        "(?i)(key|secret|password|token|\\b(?:K[1-4]|RC4|AES|Hytera|scrambler)\\b|(?:^|\\s)-(?:H|b|R|1|!)\\S*"
        "|--dmr-(?:baofeng-pc5|csi-ee72)"
        "|(?-i:(?:^|[\\s\"'\\[,])-[~yhaepPqYWrlZTF0348EO^Nju]*?[12!@59ASHRb]))"));
    return labelled;
}

/**
 * @brief @p text without the decoder's own frame-sync labels, which the key rule must not read as options.
 *
 * printFrameSync() (src/dsp/dsd_frame_sync.c) and print_dmr_sync_polarity() (src/protocol/dmr/dmr_bs.c) print
 * "Sync: <label>", and an inverted label starts with '-'. Some read as a key option bundled behind ones that take no
 * argument ("-P25p1" is -P, then -2 with "5p1"), so each is exempt as an exact whole token in that place, and only
 * there: anything else on the line is still judged.
 */
QString
withoutSyncLabels(const QString& text) {
    static const QRegularExpression labels(
        QStringLiteral("(?<=\\bSync: )-(?:DMR|dPMR|DSTAR|EDACS|M17|P25p1|P25p2|PV|PV_C|TETRA|X2-TDMA|YSF)(?=\\s|\\z)"));
    return QString(text).remove(labels);
}

/*
 * Lines that name why a USB receiver failed, which the key rule would drop for their negative code (" error -1"
 * reads as option -1), and a few lines that report a signed setting. Each is one producer's fixed text, whole:
 * the source and level formatRecord() gives it, then the producer's format, in which only the fields vary. Accepted
 * conversions: %d, %i and %u, a fixed-point %f (precision, flags and an l length allowed), and %s, whose text must not
 * hold a key label itself (isProducerErrorLine()); an entry with any other is left out (producerLineCountsForTest()
 * reports it).
 */
struct ProducerLine {
    const char* record;
    const char* format;
};

const ProducerLine kProducerLines[] = {
    // librtlsdr (android/third_party/librtlsdr/src/librtlsdr.c), through the stderr pump. Its register helpers print
    // "%s failed with %d" with their own __FUNCTION__.
    {"[stderr] [info] ", "usb_claim_interface error %d"},
    {"[stderr] [info] ", "usb_open error %d"},
    {"[stderr] [info] ", "libusb_set_option error %d"},
    {"[stderr] [info] ", "libusb_wrap_sys_device error %d"},
    {"[stderr] [info] ", "rtlsdr_read_array failed with %d"},
    {"[stderr] [info] ", "rtlsdr_write_array failed with %d"},
    {"[stderr] [info] ", "rtlsdr_read_reg failed with %d"},
    {"[stderr] [info] ", "rtlsdr_write_reg failed with %d"},
    {"[stderr] [info] ", "rtlsdr_demod_read_reg failed with %d"},
    {"[stderr] [info] ", "rtlsdr_demod_write_reg failed with %d"},
    // dsd-neo's rtl/usb lines with an rc field (src/io/radio), through the runtime log or stderr.
    {"[decoder] [warn] ",
     "WARNING: Failed to adjust fs/4 capture center for actual device rate: center=%u, capture=%u Hz (rc=%d)."},
    {"[decoder] [info] ", "NOTICE: PPM correction request %d failed (rc=%d); keeping applied value %d."},
    {"[decoder] [error] ", "Failed to apply RTL-SDR center frequency %u Hz (rc=%d)."},
    {"[decoder] [error] ", "Failed to apply RTL-SDR sample rate %u Hz (rc=%d)."},
    {"[decoder] [error] ", "Failed to return the RTL-SDR to center frequency %u Hz (rc=%d)."},
    {"[decoder] [error] ", "Failed to return the RTL-SDR to sample rate %u Hz (rc=%d)."},
    {"[decoder] [error] ", "The RTL-SDR could not be put back on %u Hz after a retune refused for the analog channel "
                           "width (rc=%d); the stream stops."},
    {"[decoder] [error] ", "Failed to apply initial RTL-SDR center frequency %u Hz (rc=%d)."},
    {"[decoder] [error] ", "Failed to apply initial RTL-SDR sample rate %u Hz (rc=%d)."},
    {"[decoder] [info] ", "NOTICE: Retune applied with warning: %u Hz (rc=%d)."},
    {"[decoder] [info] ", "NOTICE: Retune failed: %u Hz (rc=%d)."},
    {"[decoder] [info] ", "NOTICE: PPM correction applied but reconfigure to %u Hz failed (rc=%d)."},
    {"[decoder] [warn] ", "WARNING: Retune tuner gain apply failed (rc=%d); continuing retune"},
    {"[decoder] [error] ", "RTL-SDR: could not allocate USB transfer buffers (rc=%d)"},
    {"[stderr] [info] ", "WARNING: Failed to read the tuner's gain table (rc=%d); gain unchanged."},
    {"[decoder] [error] ", "RTL-SDR: could not start the sample-progress monitor (rc=%d); the stream does not start"},
    // The Android service's run and configure results (DecoderService.kt), through nativeHostDiagnostic().
    {"[host] [info] ", "engine run returned %d"},
    {"[host] [info] ", "service: engine configuration returned %d"},
    // Signed settings (#782 gate r1 G5): the requested ppm (rtl_sdr_fm.cpp), and a scan row's squelch, -100..0 dB, on a
    // digital channel without a radio input (channel_scan.c, trunk_scan.c).
    {"[decoder] [info] ", "Requested RTL PPM Error Set to %d"},
    {"[decoder] [info] ", "Device does not support frequency (PPM) correction; configured ppm %d will have no effect."},
    {"[decoder] [warn] ",
     "WARNING: Scan channel %d (%.6lf MHz): --squelch-db %d cannot gate digital acquisition without "
     "a radio input; here it gates only the analog input monitor (-8) and the carrier activity it "
     "stamps."},
    {"[decoder] [warn] ", "WARNING: Trunk scan target '%s': --squelch-db %d cannot gate digital acquisition without a "
                          "radio input; here it gates only the analog input monitor (-8) and the carrier activity it "
                          "stamps."},
};

/*
 * libusb's own log lines, which the USB receiver's log callback forwards to the runtime log unchanged: they start with
 * libusb's header, "libusb: <level> [<function>] ". The message after it is libusb's usbi_err/usbi_warn text, and only
 * one ending in its code (", errno=%d" or a negative "%d") is exempt, and then only if the text before the code is
 * itself free of key labels.
 */
constexpr char kLibusbRecordPrefix[] = "libusb: ";

// The runtime log's newline ends a fresh record; a saved one has it as the space redact() makes of it.
const char kRecordEnd[] = "(?:\\n| )?\\z";
/* Error codes, errno values and PPM corrections (clamped to +/-200) fit in three digits. The bound keeps a signed field
 * from passing for a short key option with a longer digit-only value attached ("-91234"). What remains: a producer
 * line whose code is a key option with a value of at most two digits ("error -912") would be kept. */
const char kSignedField[] = "-?[0-9]{1,3}";

struct ProducerTemplates {
    QRegularExpression fixed;
    int listed = 0;
    int accepted = 0;
};

/**
 * @brief The pattern for the conversion that starts at @p at in @p format, and where it ends (@p end); empty when it is
 * not one producerTemplates() accepts.
 */
QString
conversionPattern(const QString& format, qsizetype at, qsizetype* end) {
    static const QRegularExpression spec(QStringLiteral("%([-+ 0#]*)(?:\\.([0-9]+))?(l?)([diufs])"));
    const auto match =
        spec.match(format, at, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
    if (!match.hasMatch()) {
        return {};
    }
    *end = match.capturedEnd();
    const QChar conversion = match.captured(4).at(0);
    const bool precise = !match.captured(2).isEmpty();
    if (conversion == QLatin1Char('f')) {
        // A printed measurement or frequency in MHz: at most four integer digits, then exactly the precision's.
        const QString sign =
            match.captured(1).contains(QLatin1Char('+')) ? QStringLiteral("[-+]") : QStringLiteral("-?");
        return sign + QStringLiteral("[0-9]{1,4}\\.[0-9]{%1}").arg(precise ? match.captured(2) : QStringLiteral("6"));
    }
    if (precise || !match.captured(1).isEmpty() || !match.captured(3).isEmpty()) {
        return {};
    }
    if (conversion == QLatin1Char('d') || conversion == QLatin1Char('i')) {
        return QLatin1String(kSignedField);
    }
    if (conversion == QLatin1Char('u')) {
        return QStringLiteral("[0-9]{1,10}");
    }
    // A name the producer quotes (a scan target's id): one token, captured so isProducerErrorLine() can judge it.
    return QStringLiteral("([^\\s'\"]{1,64})");
}

/** @brief The producer lines as one anchored pattern; an entry with a conversion it does not accept is left out. */
const ProducerTemplates&
producerTemplates() {
    static const ProducerTemplates templates = [] {
        ProducerTemplates built;
        QStringList alternatives;
        for (const auto& line : kProducerLines) {
            ++built.listed;
            QString pattern = QRegularExpression::escape(QLatin1String(line.record));
            const QString format = QLatin1String(line.format);
            bool accepted = true;
            qsizetype literal = 0;
            for (qsizetype at = format.indexOf(QLatin1Char('%')); at >= 0 && accepted;
                 at = format.indexOf(QLatin1Char('%'), literal)) {
                pattern += QRegularExpression::escape(format.mid(literal, at - literal));
                qsizetype end = at;
                const QString field = conversionPattern(format, at, &end);
                accepted = !field.isEmpty();
                pattern += field;
                literal = end;
            }
            if (accepted) {
                ++built.accepted;
                alternatives.append(pattern + QRegularExpression::escape(format.mid(literal)));
            }
        }
        built.fixed = QRegularExpression(QStringLiteral("\\A(?:") + alternatives.join(QLatin1Char('|'))
                                         + QStringLiteral(")") + QLatin1String(kRecordEnd));
        return built;
    }();
    return templates;
}

/**
 * @brief Whether @p text is one whole producer error line (see kProducerLines and kLibusbRecordPrefix). A %s field's
 * text is the producer's to quote, so a key label inside it keeps the key rule.
 */
bool
isProducerErrorLine(const QString& text) {
    const auto fixed = producerTemplates().fixed.match(text);
    if (fixed.hasMatch()) {
        for (int group = 1; group <= fixed.lastCapturedIndex(); ++group) {
            if (fixed.captured(group).contains(keyLabel())) {
                return false;
            }
        }
        return true;
    }
    static const QRegularExpression libusb(QStringLiteral("\\A\\[decoder\\] \\[(?:error|warn)\\] ")
                                           + QRegularExpression::escape(QLatin1String(kLibusbRecordPrefix))
                                           + QStringLiteral("(?:error|warning) \\[[A-Za-z_][A-Za-z0-9_]*\\] "
                                                            "(.*?)(?:errno=|(?=-))")
                                           + QLatin1String(kSignedField) + QLatin1String(kRecordEnd));
    const auto match = libusb.match(text);
    return match.hasMatch() && !match.captured(1).contains(keyLabel());
}

/*
 * Signed measurements inside producer lines (#782 gate r3 J2, J3). A negative number whose first digit is a key option's
 * letter (-1, -2, -5, -9, or one bundled behind -0, -3, -4, -8) reads to getopt as that option with the rest of the
 * token or the next one as its value, and a key loader may take a value with a fraction or a unit after it ("-55.3":
 * -5 with "5.3", which tyt_ep_aes_keystream_creation() reads as 5). No shape of the number tells a measurement from a
 * key, so none is exempt by its shape: a number leaves the key scan only as a field of a template copied from the
 * producer that prints it, with that producer's label before it.
 *
 * Each template is a run of one producer's printf format (or of the consecutive prints one function makes for a line).
 * A whole line (`record` set) must be the whole record after that formatRecord() prefix, as kProducerLines are; a
 * fragment (`record` null) may stand anywhere in a record, must open with literal text and hold a letter (its label),
 * and a field that ends it must end the record. In a match, the numeric fields are removed before the key scan; the
 * label, the rest of the record and any %s text are still scanned, so a key beside or inside a measurement drops the
 * record. Integer fields are bounded by kSignedField or the entry's own range (a ppm is at most 200 either way), real
 * ones by their exact precision. A record on the command-line echo path (isArgumentEcho()) gets no exemption at all.
 *
 * What remains: a template is matched by its text alone, so a command line that spells a producer's label and then a
 * key-form number in the field's place ("Tuner error set to -19 ppm.") is kept. No producer echoes its arguments
 * without one of the markers isArgumentEcho() knows, and every such marker turns the exemptions off.
 *
 * Keep each template in step with its producer: tests/ui/test_ui_qt_diagnostics_log.cpp holds one rendered line per
 * template and the key forms beside them.
 */
struct MeasurementTemplate {
    const char* record;      // the formatRecord() prefix of a whole line; null for a fragment
    const char* format;      // the producer's printf text, UTF-8
    const char* signedField; // the %d/%i/%ld pattern for this producer's range; null for kSignedField
};

// A ppm the stream applies or reports: clamped to +/-200 (clamp_requested_ppm(), rtl_sdr_fm.cpp).
constexpr char kPpmField[] = "-?(?:[0-9]{1,2}|1[0-9]{2}|200)";
// A P25 identifier's transmit offset in spacing units: 13 bits and a sign (p25_signed_offset_units()).
constexpr char kTxOffsetField[] = "-?[0-9]{1,4}";
// A value a configuration file or a demodulator setting gave, echoed in a range or refusal message.
constexpr char kConfigField[] = "-?[0-9]{1,10}";
// A socket call's result: 0 or -1 (SOCKET_ERROR).
constexpr char kSocketResultField[] = "-?[0-9]";

const MeasurementTemplate kMeasurementTemplates[] = {
    // rtl_device.cpp, through the stderr pump: the applied ppm and the tuner gains (E4000 and FC0012/FC0013 tables go
    // below 0 dB), and a SoapySDR gain moved to the driver's step.
    {"[stderr] [info] ", "Tuner error set to %i ppm.", kPpmField},
    {"[stderr] [info] ", "Tuner gain set to %0.2f dB.", nullptr},
    {"[stderr] [info] ", "Tuner manual gain (nearest): %0.1f dB.", nullptr},
    {"[stderr] [info] ", "SoapySDR: adjusted gain %s from %.2f dB to %.2f dB.", nullptr},
    // rtl_device.cpp and rtl_sdr_fm.cpp, through the runtime log: the replay resampler's ratio error, ppm corrections,
    // and the automatic ppm loop.
    {"[decoder] [info] ",
     "IQ replay: resampling the %u Hz capture to %u Hz (L/M=%d/%d, %+.2f ppm, %d taps per phase, group delay %.2f ms).",
     nullptr},
    {"[decoder] [info] ", "PPM correction applied: %d (reconfigured %u Hz).", kPpmField},
    {"[decoder] [info] ", "PPM correction applied: %d.", kPpmField},
    {"[decoder] [info] ", "AUTO-PPM: src=%d pwr=%.1f dB snr=%.1f dB df=%.1f Hz ppm %d->%d", kPpmField},
    // args.c: the low input warning threshold, and the P25 timing options echoed as given (only the CC grace is
    // clamped to 0..120 s).
    {"[decoder] [info] ", "NOTICE: Low input warning threshold: %.1f dBFS", nullptr},
    {"[decoder] [info] ", "NOTICE: Low input warning threshold (env): %.1f dBFS", nullptr},
    {"[decoder] [info] ", "NOTICE: P25: VC grace set to %.2fs (CLI).", nullptr},
    {"[decoder] [info] ", "NOTICE: P25: Min follow dwell set to %.2fs (CLI).", nullptr},
    {"[decoder] [info] ", "NOTICE: P25: Grant->Voice timeout set to %.2fs (CLI).", nullptr},
    {"[decoder] [info] ", "NOTICE: P25: MAC hold set to %.2fs (CLI).", nullptr},
    {"[decoder] [info] ", "NOTICE: P25: Force-release extra set to %.2fs (CLI).", nullptr},
    {"[decoder] [info] ", "NOTICE: P25: Force-release margin set to %.2fs (CLI).", nullptr},
    {"[decoder] [info] ", "NOTICE: P25p1: Error-hold threshold set to %.1f%% (CLI).", nullptr},
    {"[decoder] [info] ", "NOTICE: P25p1: Error-hold seconds set to %.2fs (CLI).", nullptr},
    // Slicer and sync statistics (dsd_dibit.c, dsd_frame_sync.c) and the I/Q debug dump (demod_pipeline.cpp).
    {nullptr, "range:[%.2f,%.2f] (n=", nullptr},
    {nullptr, "[SYNC] range:[%.1f,%.1f] dc:", nullptr},
    {nullptr, "(%.2f,%.2f|%.0f\xC2\xB0,%.2f)", nullptr},
    // Positions: LIP and Harris/NMEA reports (dsd_gps.c), DMR LRRP and NMEA LOCN (dmr_pdu.c), M17 GNSS (m17.c). A %s is
    // the degree glyph (dsd_degrees_glyph(): the sign or " deg") and the hemisphere letter, or the optional time.
    {nullptr, "Lat: %.5lf%s%s Lon: %.5lf%s%s (%.5lf, %.5lf)", nullptr},
    {nullptr, "GPS: %f%s, %f%s;", nullptr},
    {nullptr, "Harris GPS: %.6lf%s, %.6lf%s;", nullptr},
    {nullptr, "GPS: (%.6f%s, %.6f%s)", nullptr},
    {nullptr, "Elevation: %d;", nullptr},
    {nullptr, "Lat: %.5lf Lon: %.5lf (%.5lf, %.5lf)", nullptr},
    {nullptr, "Altitude: %.2lfm", nullptr},
    {nullptr, "NMEA / LOCN; Source: %u;%s (%.5lf%s, %.5lf%s);", nullptr},
    {nullptr, "GPS: (%f, %f);", nullptr},
    {nullptr, "Altitude: %.1f m;", nullptr},
    // Protocol fields: M17's power and UDP result (m17.c), DMR's local time and offset (dmr_csbk.c), P25 identifier
    // offsets (p25_lcw.c, p25p1_pdu_trunking.c) and P25p2's local time offset (p25p2_vpdu.c).
    {nullptr, "PWR: %.1f dB", nullptr},
    {nullptr, "UDP RETURN: %d: LEN: %d; SENT: %d;", nullptr},
    {nullptr, "Local: %02d:%02d:%02d;", nullptr},
    {nullptr, "Offset: %d;", nullptr},
    {nullptr, "TX Offset: %d; Spacing: ", kTxOffsetField},
    {nullptr, "TX Offset [%d] Spacing [", kTxOffsetField},
    {nullptr, "Local Time Offset: %.01f Hours;", nullptr},
    // Result codes: TETRA's tuning hook (tetra_trunk_sm.c), and a UDP socket option (udp_audio.c, m17_udp.c).
    {nullptr, "failed (result %d)", nullptr},
    {nullptr, "UDP Broadcast Set Error %d", kSocketResultField},
    // The rate chain's replay resampling (rtl_sdr_fm.cpp), configuration range warnings (config_user_validation.cpp,
    // printed by dsdcfg_diags_print()), and a negative analog width refused (rtl_demod_config.cpp).
    {nullptr, "(L/M=%d/%d, %+.2f ppm)", nullptr},
    {nullptr, "Value %g is out of range [%d, %d]", kConfigField},
    {nullptr, "Value %d is out of range [%d, %d]", kConfigField},
    {nullptr, "bandwidth %d Hz is negative;", kConfigField},
};

struct MeasurementPatterns {
    QList<QRegularExpression> patterns;
    int listed = 0;
    int accepted = 0;
};

/**
 * @brief The pattern for the measurement template conversion that starts at @p at in @p format, and where it ends
 * (@p end); empty when it is not one the templates accept. @p numeric says whether it is a field the match removes.
 */
QString
measurementConversion(const QString& format, qsizetype at, const char* signedField, qsizetype* end, bool* numeric) {
    static const QRegularExpression spec(QStringLiteral("%([-+ 0#]*)([0-9]*)(?:\\.([0-9]+))?(l{0,2})([diufgXxs%])"));
    const auto match =
        spec.match(format, at, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
    if (!match.hasMatch()) {
        return {};
    }
    *end = match.capturedEnd();
    *numeric = true;
    const QChar conversion = match.captured(5).at(0);
    // A width pads with spaces unless it is a zero fill or left-justified.
    const bool padded = !match.captured(2).isEmpty() && !match.captured(1).contains(QLatin1Char('0'))
                        && !match.captured(1).contains(QLatin1Char('-'));
    const QString pad = padded ? QStringLiteral(" *") : QString();
    const QString sign = match.captured(1).contains(QLatin1Char('+')) ? QStringLiteral("[-+]") : QStringLiteral("-?");
    if (conversion == QLatin1Char('%')) {
        *numeric = false;
        return QStringLiteral("%");
    }
    if (conversion == QLatin1Char('s')) {
        *numeric = false;
        return QStringLiteral("(?:.{0,64}?)");
    }
    if (conversion == QLatin1Char('f')) {
        const QString precision = match.captured(3).isEmpty() ? QStringLiteral("6") : match.captured(3);
        return pad + QStringLiteral("(") + sign + QStringLiteral("[0-9]{1,6}")
               + (precision.toInt() > 0 ? QStringLiteral("\\.[0-9]{%1}").arg(precision) : QString())
               + QStringLiteral(")");
    }
    if (conversion == QLatin1Char('g')) {
        return pad + QStringLiteral("(") + sign + QStringLiteral("[0-9]{1,7}(?:\\.[0-9]{1,6})?(?:e[-+][0-9]{2,3})?)");
    }
    if (conversion == QLatin1Char('u')) {
        return pad + QStringLiteral("([0-9]{1,10})");
    }
    if (conversion == QLatin1Char('X') || conversion == QLatin1Char('x')) {
        return pad + QStringLiteral("([0-9A-Fa-f]{1,16})");
    }
    if (match.captured(1).contains(QLatin1Char('+'))) {
        return {}; // No producer prints a signed integer with "%+d"; one that does needs its own pattern.
    }
    QString field = QString::fromLatin1(signedField ? signedField : kSignedField);
    if (!signedField && !match.captured(4).isEmpty()) {
        field = QStringLiteral("-?[0-9]{1,19}");
    }
    return pad + QStringLiteral("(") + field + QStringLiteral(")");
}

/** @brief The measurement templates, built; an entry the rules above refuse is left out and counted. */
const MeasurementPatterns&
measurementPatterns() {
    static const MeasurementPatterns built = [] {
        MeasurementPatterns out;
        for (const auto& entry : kMeasurementTemplates) {
            ++out.listed;
            const QString format = QString::fromUtf8(entry.format);
            QString pattern;
            QString literals;
            qsizetype firstField = -1;
            bool accepted = true;
            bool endsInField = false;
            qsizetype literal = 0;
            for (qsizetype at = format.indexOf(QLatin1Char('%')); at >= 0 && accepted;
                 at = format.indexOf(QLatin1Char('%'), literal)) {
                literals += format.mid(literal, at - literal);
                pattern += QRegularExpression::escape(format.mid(literal, at - literal));
                qsizetype end = at;
                bool numeric = false;
                const QString field = measurementConversion(format, at, entry.signedField, &end, &numeric);
                accepted = !field.isEmpty();
                pattern += field;
                if (numeric && firstField < 0) {
                    firstField = at;
                }
                endsInField = numeric && end == format.size();
                literal = end;
            }
            literals += format.mid(literal);
            pattern += QRegularExpression::escape(format.mid(literal));
            // The label: literal text before the first field, with a letter in it, or (a fragment's punctuation-led
            // field context) a symbol no command line is written with elsewhere in the template.
            const QString label = format.left(std::max<qsizetype>(firstField, 0));
            const auto letter = [](QChar c) { return c.isLetter(); };
            const auto symbol = [](QChar c) { return c.unicode() > 0x7f; };
            const bool labelled = std::any_of(label.cbegin(), label.cend(), letter)
                                  || (entry.record == nullptr && !label.isEmpty()
                                      && std::any_of(literals.cbegin(), literals.cend(), symbol));
            if (!accepted || firstField <= 0 || !labelled) {
                continue;
            }
            if (entry.record) {
                pattern = QStringLiteral("\\A") + QRegularExpression::escape(QLatin1String(entry.record)) + pattern
                          + QLatin1String(kRecordEnd);
            } else if (endsInField) {
                pattern += QStringLiteral("(?=") + QLatin1String(kRecordEnd) + QStringLiteral(")");
            }
            out.patterns.append(QRegularExpression(pattern));
            ++out.accepted;
        }
        return out;
    }();
    return built;
}

/**
 * @brief Whether @p text echoes a command line: a label for one, or a JSON, shell-quoted or list form with a quoted
 * option in it. (A bare bracketed list, "[-1, 9]", has no producer label before its numbers, so no template exempts
 * them anyway.)
 */
bool
isArgumentEcho(const QString& text) {
    static const QRegularExpression echo(
        QStringLiteral("(?i)\\b(?:argv|args?|arguments|argc|cmd(?:line)?|command[ _-]?line|exec(?:ve|vp|l)?)\\b"
                       "|[\"']-"));
    return text.contains(echo);
}

/** @brief @p text without the numeric fields of the measurement templates it holds (see kMeasurementTemplates). */
QString
withoutMeasurementFields(const QString& text) {
    QList<std::pair<qsizetype, qsizetype>> spans;
    for (const auto& pattern : measurementPatterns().patterns) {
        auto matches = pattern.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            for (int group = 1; group <= match.lastCapturedIndex(); ++group) {
                if (match.capturedLength(group) > 0) {
                    spans.append({match.capturedStart(group), match.capturedLength(group)});
                }
            }
        }
    }
    std::sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    QString out = text;
    qsizetype floor = out.size();
    for (const auto& [start, length] : spans) {
        // Overlapping matches of two templates remove the same field once.
        const qsizetype stop = std::min(start + length, floor);
        if (stop > start) {
            out.remove(start, stop - start);
            floor = start;
        }
    }
    return out;
}

/**
 * @brief Whether @p text holds a key label once the decoder's own sync labels and measurement fields are set aside;
 * a command-line echo gets neither set aside.
 */
bool
holdsKeyLabel(const QString& text) {
    if (!text.contains(keyLabel())) {
        return false;
    }
    if (isArgumentEcho(text)) {
        return true;
    }
    return withoutMeasurementFields(withoutSyncLabels(text)).contains(keyLabel());
}

QString
withoutAnsi(QString text) {
    static const QRegularExpression ansi(QStringLiteral("\\x1b\\[[0-?]*[ -/]*[@-~]"));
    return text.remove(ansi);
}

/**
 * @brief Whether a captured line holds nothing but what a key value is written with: hex digits, separators and
 * control characters.
 */
bool
isKeyContinuation(const QString& text) {
    static const QRegularExpression continuation(QStringLiteral("\\A[\\s\\p{Cc}0-9A-Fa-fxX,:;._'\"()\\[\\]-]*\\z"));
    return continuation.match(withoutAnsi(text)).hasMatch();
}

/** @brief Whether a captured line is one token, which a string-valued option (an API token) takes whole. */
bool
isSingleToken(const QString& text) {
    static const QRegularExpression token(QStringLiteral("\\A\\s*\\S+\\s*\\z"));
    return token.match(text).hasMatch();
}

/** @brief Whether a captured line starts a quoted string, as a multi-word value given on the next line does. */
bool
startsQuoted(const QString& text) {
    const QString trimmed = text.trimmed();
    return trimmed.startsWith(QLatin1Char('\'')) || trimmed.startsWith(QLatin1Char('"'));
}

/**
 * @brief The quote open after @p c, inside @p open (0: none) and after @p previous (0: @p c starts the text).
 *
 * A quote opens only at the start of a token, so an apostrophe inside a word ("don't") opens nothing; it closes at the
 * same character anywhere.
 */
char16_t
quoteAfter(char16_t open, char16_t previous, char16_t c) {
    if (open != 0) {
        return c == open ? 0 : open;
    }
    if ((c == u'\'' || c == u'"')
        && (previous == 0 || QChar(previous).isSpace() || QStringLiteral("=[,(").contains(QChar(previous)))) {
        return c;
    }
    return 0;
}

/**
 * @brief The quote left open at the end of @p text, starting inside @p open (0: none).
 *
 * A quote opens only at the start of a token, so an apostrophe inside a word ("don't") opens nothing; it closes at the
 * same character anywhere.
 */
char16_t
quoteLeftOpen(const QString& text, char16_t open) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        open = quoteAfter(open, i == 0 ? 0 : text.at(i - 1).unicode(), text.at(i).unicode());
    }
    return open;
}

QByteArray
formatRecord(const QString& source, const QString& level, const QString& text) {
    auto line = DiagnosticsLog::redact(QStringLiteral("[%1] [%2] %3").arg(source, level, text)).toUtf8();
    if (line.size() > 512) {
        int end = 512;
        while (end > 0 && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80) {
            --end;
        }
        line.truncate(end);
    }
    return line;
}

/** @brief Whether records from @p source are host lifecycle records, kept in the host store as well. */
bool
isHostSource(const QString& source) {
    return source == QLatin1String("host") || source == QLatin1String("session") || source == QLatin1String("process");
}

QString
baseProcessFacts() {
    QString facts = QStringLiteral("%1, %2 (%3)")
                        .arg(QCoreApplication::applicationPid())
                        .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    const QString version = QCoreApplication::applicationVersion();
    if (!version.isEmpty()) {
        facts += QStringLiteral(", app ") + version;
    }
    return facts;
}

/**
 * @brief The context lines at @p contextAt that the ring limit drops from @p lines, and the first kept line.
 *
 * Every slot a context line takes moves the cutoff, so the set is settled against the final cutoff.
 */
QStringList
droppedContext(const QList<QByteArray>& lines, std::initializer_list<qsizetype> contextAt, qsizetype* keepFrom) {
    QStringList context;
    for (int reserved = 0;;) {
        *keepFrom = std::max<qsizetype>(0, lines.size() - (ringLimit - 1 - reserved));
        context.clear();
        for (const qsizetype at : contextAt) {
            if (at >= 0 && at < *keepFrom) {
                context.append(DiagnosticsLog::redact(QString::fromUtf8(lines.at(at))));
            }
        }
        if (context.size() == reserved) {
            return context;
        }
        reserved = static_cast<int>(context.size());
    }
}

/**
 * @brief Until installTap() has published the log, keeps the record as submit() would format it and returns true.
 *
 * Do not resolve AppDataLocation before Qt has its application identity, and do not wait for the log's construction:
 * only bounded, redacted records survive until the publish.
 */
bool
holdUntilPublished(const QString& source, const QString& level, const QString& text) {
    auto& host = hostDiagnostics();
    if (host.ready.load(std::memory_order_acquire)) {
        return false;
    }
    // Formatted outside the lock: redaction is the costly part.
    PendingRecord held{formatRecord(source, level, text), isHostSource(source)};
    std::lock_guard<std::mutex> lock(host.mutex);
    if (host.ready.load(std::memory_order_relaxed)) {
        return false;
    }
    host.pending.append(std::move(held));
    if (host.pending.size() > 2000) {
        host.pending.removeFirst();
    }
    return true;
}

void
capture(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    static const char* levels[] = {"error", "warn", "info", "debug"};
    const QString source = QStringLiteral("decoder");
    const QString levelName = QString::fromLatin1(levels[std::min(static_cast<int>(level), 3)]);
    const QString body = QString::fromUtf8(text);
    if (!holdUntilPublished(source, levelName, body)) {
        DiagnosticsLog::instance().submit(source, levelName, body);
    }
}

/**
 * @brief The log's directory, resolved on the calling thread.
 *
 * QStandardPaths is resolved only here. On Android its location cache has no lock, and the Qt thread resolves its own
 * paths at the same time, so installTap() resolves this on its calling thread and the build only reads the result.
 */
QString
resolveDirectory() {
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/diagnostics");
    auto& host = hostDiagnostics();
    std::lock_guard<std::mutex> lock(host.mutex);
    host.directoryThread = std::this_thread::get_id();
    return directory;
}

/** @brief The directory installTap() resolved, or one resolved here when the log is built before installTap(). */
QString
logDirectory() {
    {
        auto& host = hostDiagnostics();
        std::lock_guard<std::mutex> lock(host.mutex);
        if (!host.directory.isEmpty()) {
            return host.directory;
        }
    }
    return resolveDirectory();
}
} // namespace

DiagnosticsLog&
DiagnosticsLog::instance() {
    // Intentionally process-lifetime: producers and the installed tap can outlive Qt UI objects.
    static DiagnosticsLog* log = create();
    return *log;
}

DiagnosticsLog*
DiagnosticsLog::create() {
    auto* log = new DiagnosticsLog(logDirectory());
    // Published as part of the construction once installTap() has run: a thread that waited for the construction (the
    // stderr pump, a session note) then writes after this run's process line and the records held for it.
    publish(*log);
    return log;
}

void
DiagnosticsLog::publish(DiagnosticsLog& log) {
    auto& host = hostDiagnostics();
    std::lock_guard<std::mutex> lock(host.mutex);
    if (!host.installed || host.ready.load(std::memory_order_relaxed)) {
        return;
    }
    // The process line first: it is where this run starts in the saved tail.
    log.noteProcess(host.facts, host.following);
    for (const auto& held : host.pending) {
        log.enqueue(held.record, held.host);
    }
    host.pending.clear();
    if (host.armPending) {
        host.armPending = false;
        log.armStartupWindow();
    }
    host.ready.store(true, std::memory_order_release);
    host.published.notify_all();
}

void
DiagnosticsLog::setPlatformFactsProvider(PlatformFactsProvider provider, PlatformLinesLogged logged) {
    auto& host = hostDiagnostics();
    std::lock_guard<std::mutex> lock(host.mutex);
    host.provider = provider;
    host.logged = logged;
}

void
DiagnosticsLog::notePlatformLinesDurable() {
    auto& host = hostDiagnostics();
    PlatformLinesLogged logged = nullptr;
    {
        std::lock_guard<std::mutex> lock(host.mutex);
        if (!host.following.isEmpty()) {
            logged = host.logged;
        }
    }
    // The publish queued the lines: a completed flush has them appended and synced.
    if (logged != nullptr) {
        (void)instance().reportWhenDurable(kPlatformLinesFlushMs, logged);
    }
}

bool
DiagnosticsLog::reportWhenDurable(int timeoutMs, PlatformLinesLogged logged) {
    // The flush only makes the writer settle them: its own verdict covers everything written since the last one, and a
    // failure an earlier flush already reported (it took the lines and lost them) is not in it (#782 gate r2 H4).
    (void)flush(timeoutMs);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_platform != PlatformLines::Durable) {
            return false;
        }
    }
    logged();
    return true;
}

void
DiagnosticsLog::installTap() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        auto& host = hostDiagnostics();
        PlatformFactsProvider provider = nullptr;
        {
            std::lock_guard<std::mutex> lock(host.mutex);
            provider = host.provider;
        }
        // Outside the host lock: the Android provider calls into the platform.
        QString facts = baseProcessFacts();
        QStringList following;
        if (provider) {
            auto extra = provider();
            if (!extra.first.isEmpty()) {
                facts += QStringLiteral(", ") + extra.first;
            }
            following = std::move(extra.second);
        }
        // Everything Qt-specific the build needs is resolved here, on the calling thread; the build thread only does
        // file I/O on the result.
        const QString directory = resolveDirectory();
        {
            std::lock_guard<std::mutex> lock(host.mutex);
            host.installed = true;
            host.facts = std::move(facts);
            host.following = std::move(following);
            host.directory = directory;
        }
        // From here LOG_* records are captured: held until the publish, then submitted.
        dsd_neo_log_set_tap(capture, nullptr);
        // The log is built on a thread of its own. Creating the directory and loading the saved records is file I/O,
        // and on Android the caller can be the main thread (the service's nativeInit). Until the publish, records,
        // startup-window requests and flushes wait in the host state, bounded. A log built before this call is
        // published by the thread, which then waits for the platform's lines to be durable. Built on the calling
        // thread instead, nothing waits there: the lines are not marked logged, so the next start logs them again.
        try {
            std::thread([] {
                publish(instance());
                notePlatformLinesDurable();
            }).detach();
        } catch (const std::system_error&) {
            publish(instance());
        }
    });
}

void
DiagnosticsLog::submitHostDiagnostic(const QString& text) {
    const QString source = QStringLiteral("host");
    const QString level = QStringLiteral("info");
    if (!holdUntilPublished(source, level, text)) {
        // Published means the pending records are already in, so this one follows them.
        instance().submit(source, level, text);
    }
}

void
DiagnosticsLog::armStartupDiagnostics() {
    auto& host = hostDiagnostics();
    if (!host.ready.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(host.mutex);
        if (!host.ready.load(std::memory_order_relaxed)) {
            // Applied by the publish.
            host.armPending = true;
            return;
        }
    }
    instance().armStartupWindow();
}

bool
DiagnosticsLog::flushDiagnostics(int timeoutMs) {
    return flushDiagnosticsOutcome(timeoutMs) == FlushOutcome::Completed;
}

namespace {
std::atomic<bool> g_finalFlushAfterQuit{false};
} // namespace

void
DiagnosticsLog::setFinalFlushAfterQuit(bool owned) {
    g_finalFlushAfterQuit.store(owned, std::memory_order_release);
}

int
DiagnosticsLog::quitFlushBoundMs() {
    return g_finalFlushAfterQuit.load(std::memory_order_acquire) ? 0 : kShutdownFlushMs;
}

void
DiagnosticsLog::flushAtQuit() {
    (void)flushDiagnostics(quitFlushBoundMs());
}

DiagnosticsLog::FlushOutcome
DiagnosticsLog::flushDiagnosticsOutcome(int timeoutMs) {
    auto& host = hostDiagnostics();
    if (!host.ready.load(std::memory_order_acquire)) {
        // The bound covers the wait for the publish too.
        const quint64 start = static_cast<quint64>(dsd_realtime_mono_ms());
        std::unique_lock<std::mutex> lock(host.mutex);
        const auto ready = [&host] { return host.ready.load(std::memory_order_relaxed); };
        if (timeoutMs < 0) {
            host.published.wait(lock, ready);
        } else if (!host.published.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) {
            return FlushOutcome::TimedOut;
        } else {
            const quint64 waited = static_cast<quint64>(dsd_realtime_mono_ms()) - start;
            timeoutMs = waited >= static_cast<quint64>(timeoutMs) ? 0 : timeoutMs - static_cast<int>(waited);
        }
    }
    return instance().flushOutcome(timeoutMs);
}

bool
DiagnosticsLog::installedForTest() {
    return hostDiagnostics().ready.load(std::memory_order_acquire);
}

std::thread::id
DiagnosticsLog::directoryResolvedOnForTest() {
    auto& host = hostDiagnostics();
    std::lock_guard<std::mutex> lock(host.mutex);
    return host.directoryThread;
}

QString
DiagnosticsLog::redact(QString text) {
    // Drop whole key-bearing records, including malformed values and labelled
    // short decimal/basic keys. Strip ANSI before matching to prevent evasion.
    text = withoutAnsi(std::move(text));
    if (holdsKeyLabel(text) && !isProducerErrorLine(text)) {
        return QLatin1String(kDroppedRecord);
    }
    static const QRegularExpression hex(QStringLiteral("(?i)[0-9a-f]{10,}"));
    text.replace(hex, QStringLiteral("[redacted]"));
    text.replace(QChar('\r'), QChar(' '));
    text.replace(QChar('\n'), QChar(' '));
    return text;
}

std::pair<int, int>
DiagnosticsLog::producerLineCountsForTest() {
    return {producerTemplates().listed, producerTemplates().accepted};
}

std::pair<int, int>
DiagnosticsLog::measurementTemplateCountsForTest() {
    return {measurementPatterns().listed, measurementPatterns().accepted};
}

DiagnosticsLog::Segments::Segments(const char* currentName, const char* previousName, qint64 segmentLimit,
                                   const char* what)
    : current(currentName), previous(previousName), limit(segmentLimit), label(what) {}

DiagnosticsLog::DiagnosticsLog(const QString& directory) : DiagnosticsLog(directory, TestOptions{}) {}

DiagnosticsLog::DiagnosticsLog(const QString& directory, TestOptions options)
    : m_tail(kTailName, kPrevName, kSegmentLimit, "saved log"),
      m_host(kHostName, kHostPrevName, kHostSegmentLimit, "saved host and session log"), m_directory(directory),
      m_options(std::move(options)) {
    QDir().mkpath(directory);
    load();
    if (!m_options.manualDrain) {
        m_writer = std::thread(&DiagnosticsLog::writeLoop, this);
    }
}

DiagnosticsLog::~DiagnosticsLog() {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_stop = true;
    if (m_options.manualDrain) {
        step(lock, true);
        return;
    }
    lock.unlock();
    m_ready.notify_one();
    m_writer.join();
}

QString
DiagnosticsLog::filePath(const char* name) const {
    return m_directory + QLatin1Char('/') + QLatin1String(name);
}

quint64
DiagnosticsLog::nowMs() const {
    return m_options.clockMs ? m_options.clockMs() : static_cast<quint64>(dsd_realtime_mono_ms());
}

bool
DiagnosticsLog::inStartupWindow(quint64 now) const {
    return now < m_startupUntil;
}

bool
DiagnosticsLog::startupWindowOpenForTest() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return inStartupWindow(nowMs());
}

void
DiagnosticsLog::setLoadHookForTest(LoadHook hook) {
    g_loadHook.store(hook);
}

void
DiagnosticsLog::load() {
    if (const LoadHook hook = g_loadHook.load()) {
        hook();
    }
    cleanDirectory();
    seedRing(loadSegments(m_tail));
    qsizetype hostCurrentAt = 0;
    const QByteArray host = loadSegments(m_host, &hostCurrentAt);
    seedHostRing(host, hostCurrentAt);
}

void
DiagnosticsLog::cleanDirectory() const {
    QDir dir(m_directory);
    // Temporaries a kill or crash left behind: QSaveFile's from older builds' whole-tail rewrites and from
    // normalization below. Nothing else in the directory is touched.
    static const QRegularExpression orphan(QStringLiteral("^(tail|host)(\\.prev)?\\.log\\.[A-Za-z0-9]{6}$"));
    for (const auto& name : dir.entryList(QDir::Files | QDir::Hidden)) {
        if (orphan.match(name).hasMatch()) {
            dir.remove(name);
        }
    }
    for (const auto& path : {filePath(kPrevName), filePath(kTailName), filePath(kHostPrevName), filePath(kHostName)}) {
        QFileInfo info(path);
        if (info.exists() && info.lastModified().secsTo(realtimeCurrentDateTime()) > 7LL * 24 * 3600) {
            QFile::remove(path);
        }
    }
}

QByteArray
DiagnosticsLog::loadSegments(const Segments& segments, qsizetype* currentAt) const {
    const QString tailPath = filePath(segments.current);
    const QString prevPath = filePath(segments.previous);
    const qint64 limit = segments.limit;
    const qint64 total = 2 * limit;
    QByteArray prev = withoutFragment(readTail(prevPath, 2 * total));
    QByteArray tail = withoutFragment(readTail(tailPath, 2 * total));
    if (prev.size() > limit || tail.size() > limit) {
        // An older build's single tail (up to 256 KiB), or anything else over a segment: split at line starts
        // into the two bounded segments, keeping no more than before. A current segment over the cap is an
        // older build's tail that was never split (this writer rotates first), so it alone is the record: a
        // previous segment beside it can only be the prefix a split interrupted by a crash already wrote.
        const QByteArray all = lineAlignedTail(tail.size() > limit ? tail : prev + tail, total);
        tail = lineAlignedTail(all, limit);
        prev = lineAlignedTail(all.left(all.size() - tail.size()), limit);
        // The current segment is rewritten only once the previous one holds its prefix: until then the unsplit tail
        // stays the record, and the next load splits it again.
        const bool prevWritten =
            prev.isEmpty() ? (!QFileInfo::exists(prevPath) || QFile::remove(prevPath)) : replaceFile(prevPath, prev);
        if (prevWritten) {
            (void)replaceFile(tailPath, tail);
        }
    }
    const QByteArray all = prev + tail;
    QByteArray bounded = lineAlignedTail(all, total);
    if (currentAt) {
        // Where the current segment starts in what is returned.
        *currentAt = std::max<qsizetype>(0, prev.size() - (all.size() - bounded.size()));
    }
    return bounded;
}

void
DiagnosticsLog::seedRing(const QByteArray& text) {
    QList<QByteArray> lines;
    for (const auto& line : text.split('\n')) {
        // NUL bytes are what a crash leaves where data the filesystem had sized was never written.
        if (!line.isEmpty() && !line.contains('\0')) {
            lines.append(line);
        }
    }
    if (lines.isEmpty()) {
        return;
    }
    // The previous run's identity, read before the ring limit can drop it. A session line belongs to the process line
    // before it, so a newer process line forgets an older run's session.
    qsizetype lastProcessAt = -1;
    qsizetype lastSessionAt = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(kProcessPrefix)) {
            lastProcessAt = i;
            lastSessionAt = -1;
        } else if (lines.at(i).startsWith(kSessionPrefix)) {
            lastSessionAt = i;
        }
    }
    if (lastProcessAt >= 0) {
        m_previousProcessLine = lines.at(lastProcessAt);
    }

    // Context the ring limit would drop goes back in right after the divider, so Copy and Share keep
    // which process, boot and session the remaining lines belong to.
    qsizetype keepFrom = 0;
    const QStringList context = droppedContext(lines, {lastProcessAt, lastSessionAt}, &keepFrom);
    // The divider and the previous run's identity (those very lines, wherever they sit) stay at the front while this
    // run's records arrive.
    m_protectedPrevious.append(pushRingLocked(QLatin1String(kPreviousRun)));
    for (const auto& line : context) {
        m_protectedPrevious.append(pushRingLocked(line));
    }
    for (qsizetype i = keepFrom; i < lines.size(); ++i) {
        const quint64 id = pushRingLocked(redact(QString::fromUtf8(lines.at(i))));
        if (i == lastProcessAt || i == lastSessionAt) {
            m_protectedPrevious.append(id);
        }
    }
    m_generation = static_cast<quint64>(m_ring.size());
    m_pinned = static_cast<int>(1 + context.size());
}

void
DiagnosticsLog::seedHostRing(const QByteArray& text, qsizetype currentAt) {
    QStringList lines;
    // The current segment starts with the process and session lines in force when it rotated (its header). Where the
    // previous segment already holds those very lines they are repeats, not records, and are skipped. A new start of
    // the same source as the first record after a rotation reads exactly like its header, and is skipped with it.
    QByteArray lastProcess;
    QByteArray lastSession;
    bool inHeader = false;
    qsizetype at = 0;
    for (const auto& line : text.split('\n')) {
        const qsizetype lineAt = at;
        at += line.size() + 1;
        if (line.isEmpty() || line.contains('\0')) {
            continue;
        }
        if (lineAt == currentAt && lineAt > 0) {
            inHeader = true;
        }
        if (inHeader
            && ((line.startsWith(kProcessPrefix) && line == lastProcess)
                || (line.startsWith(kSessionPrefix) && line == lastSession))) {
            continue;
        }
        inHeader = false;
        if (line.startsWith(kProcessPrefix)) {
            lastProcess = line;
            lastSession.clear();
        } else if (line.startsWith(kSessionPrefix)) {
            lastSession = line;
        }
        lines.append(redact(QString::fromUtf8(line)));
    }
    // The newest records that fit beside the divider.
    lines = lines.mid(std::max<qsizetype>(0, lines.size() - (kHostRingLimit - 1)));
    if (lines.isEmpty()) {
        return;
    }
    m_hostRing.append(QLatin1String(kPreviousRun));
    m_hostRing.append(lines);
    m_hostPrevious = static_cast<int>(lines.size());
}

void
DiagnosticsLog::enqueue(const QByteArray& record, bool host, Pin pin) {
    // Queue lock orders concurrent submit operations, but no I/O holds it.
    std::lock_guard<std::mutex> queueLock(m_queueMutex);
    enqueueLocked(record, host, pin);
}

void
DiagnosticsLog::enqueueLocked(const QByteArray& record, bool host, Pin pin) {
    {
        const QString line = QString::fromUtf8(record);
        std::lock_guard<std::mutex> ringLock(m_ringMutex);
        appendRingLocked(line, pin);
        if (host) {
            appendHostRingLocked(line);
        }
    }
    // Bounded backpressure: retaining newest queued records matches ring semantics.
    if (m_queue.size() >= queueLimit) {
        m_queue.pop_front();
    }
    m_queue.push_back(record + '\n');
    if (host) {
        if (m_hostQueue.size() >= static_cast<size_t>(kHostRingLimit)) {
            m_hostQueue.pop_front();
            // The platform lines sit at the front of the host queue from the publish on: one may be the dropped record.
            if (m_platform == PlatformLines::Queued) {
                m_platform = PlatformLines::Lost;
            }
        }
        m_hostQueue.push_back(record + '\n');
    }
    m_ready.notify_one();
}

void
DiagnosticsLog::ringNote(const QString& line, bool host) {
    std::lock_guard<std::mutex> ringLock(m_ringMutex);
    appendRingLocked(line);
    if (host) {
        // Also where the host records are read, so decoder output cannot evict it.
        appendHostRingLocked(line);
    }
}

quint64
DiagnosticsLog::pushRingLocked(const QString& line) {
    m_ring.append(line);
    m_ringIds.append(++m_lastRingId);
    return m_lastRingId;
}

void
DiagnosticsLog::appendRingLocked(const QString& line, Pin pin) {
    const quint64 id = pushRingLocked(line);
    if (pin == Pin::Process) {
        m_protectedProcess = id;
    } else if (pin == Pin::Session) {
        m_protectedSession = id;
    }
    while (m_ring.size() > ringLimit) {
        // A pinned line no longer protected (a session line a newer one replaced) is the oldest line: it goes first.
        int released = -1;
        for (int i = 0; i < m_pinned && released < 0; ++i) {
            if (!isProtectedLocked(i)) {
                released = i;
            }
        }
        if (released >= 0) {
            m_ring.removeAt(released);
            m_ringIds.removeAt(released);
            --m_pinned;
            continue;
        }
        if (m_pinned >= m_ring.size()) {
            break;
        }
        // Otherwise evict the oldest unpinned line; a protected line reaching the front is pinned there instead.
        // Repeats of a protected line's text (the same source started again) are other lines, evicted like any other.
        if (m_pinned < pinnedLimit && isProtectedLocked(m_pinned)) {
            ++m_pinned;
            continue;
        }
        m_ring.removeAt(m_pinned);
        m_ringIds.removeAt(m_pinned);
    }
    ++m_generation;
}

void
DiagnosticsLog::appendHostRingLocked(const QString& line) {
    m_hostRing.append(line);
    while (m_hostRing.size() > kHostRingLimit) {
        if (m_hostPrevious == 0) {
            m_hostRing.removeFirst();
            continue;
        }
        // The previous run's oldest record, and its divider with the last of them.
        m_hostRing.removeAt(1);
        if (--m_hostPrevious == 0) {
            m_hostRing.removeFirst();
        }
    }
}

bool
DiagnosticsLog::isProtectedLocked(int index) const {
    const quint64 id = m_ringIds.at(index);
    return id == m_protectedProcess || id == m_protectedSession || m_protectedPrevious.contains(id);
}

void
DiagnosticsLog::submit(const QString& source, const QString& level, const QString& text) {
    enqueue(formatRecord(source, level, text), isHostSource(source));
}

DiagnosticsLog::StreamCapture::StreamCapture(DiagnosticsLog& log, QString source)
    : m_log(log), m_source(std::move(source)) {}

void
DiagnosticsLog::StreamCapture::feed(const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == '\n' || c == '\r') {
            if (m_oversized) {
                m_log.submit(m_source, QStringLiteral("info"), QStringLiteral("[oversized diagnostic omitted]"));
                m_oversized = false;
                // Omitted unread, it may have named a key option, and a quoted value it opened runs on.
                m_afterDropped = true;
                m_quote = m_oversizedQuote;
                m_quotedSize = 0;
            }
            if (!m_line.empty()) {
                emitLine();
            }
            continue;
        }
        if (m_oversized) {
            // Skipped unread, but followed for quotes. A quote is ASCII, and no byte of a multibyte character is a
            // quote or a space.
            const auto byte = static_cast<unsigned char>(c);
            m_oversizedQuote = quoteAfter(m_oversizedQuote, m_oversizedLast, byte);
            m_oversizedLast = byte < 0x80 ? byte : u'x';
            continue;
        }
        m_line.push_back(c);
        if (m_line.size() >= 1024U) {
            // Do not split a secret-bearing record into unlabelled fragments.
            m_oversized = true;
            // Inside a quote a dropped record left open, the line is still that value.
            const QString cleared = withoutAnsi(QString::fromUtf8(m_line.c_str()));
            m_oversizedQuote = quoteLeftOpen(cleared, m_afterDropped ? m_quote : 0);
            m_oversizedLast = cleared.isEmpty() ? 0 : cleared.back().unicode();
            m_line.clear();
        }
    }
}

void
DiagnosticsLog::StreamCapture::finish() {
    if (!m_line.empty()) {
        emitLine();
    }
}

void
DiagnosticsLog::StreamCapture::emitLine() {
    const QString text = QString::fromUtf8(m_line.c_str());
    m_line.clear();
    QByteArray record = formatRecord(m_source, QStringLiteral("info"), text);
    if (record == kDroppedRecord) {
        // Dropped for its own sake inside a quote an earlier dropped record opened, it is still that value: the quote
        // runs on through it.
        if (m_afterDropped && m_quote != 0) {
            followQuote(withoutAnsi(text));
        } else {
            m_quote = quoteLeftOpen(withoutAnsi(text), 0);
            m_quotedSize = 0;
        }
        m_afterDropped = true;
    } else if (continuesValue(text)) {
        // The rest of the dropped record's value, split off at a newline before redaction could see it whole.
        record = kDroppedRecord;
    } else {
        m_afterDropped = false;
    }
    m_log.enqueue(record, isHostSource(m_source));
}

void
DiagnosticsLog::StreamCapture::followQuote(const QString& plain) {
    m_quote = quoteLeftOpen(plain, m_quote);
    // A quote that never closes ends with the bound a single line has: past it, the lines are judged one by one.
    m_quotedSize += plain.size();
    if (m_quotedSize > 1024) {
        m_quote = 0;
    }
}

bool
DiagnosticsLog::StreamCapture::continuesValue(const QString& text) {
    if (!m_afterDropped) {
        return false;
    }
    const QString plain = withoutAnsi(text);
    if (m_quote != 0) {
        followQuote(plain);
        return true;
    }
    if (isKeyContinuation(plain) || isSingleToken(plain) || startsQuoted(plain)) {
        m_quote = quoteLeftOpen(plain, 0);
        m_quotedSize = 0;
        return true;
    }
    return false;
}

void
DiagnosticsLog::noteProcess(const QString& facts, const QStringList& followingLines) {
    const QByteArray record =
        formatRecord(QStringLiteral("process"), QStringLiteral("info"), QStringLiteral("pid ") + facts);
    QByteArray previous;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_processNoted) {
            return;
        }
        m_processNoted = true;
        m_processLine = record;
        previous = m_previousProcessLine;
    }
    enqueue(record, true, Pin::Process);
    if (!followingLines.isEmpty()) {
        // Queued together with their state, so the writer step that takes them knows it carries them.
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (const auto& line : followingLines) {
            enqueueLocked(formatRecord(QStringLiteral("process"), QStringLiteral("info"), line), true);
        }
        m_platform = PlatformLines::Queued;
    }
    // A fact about the log, not a cause: the boot counter moved between the previous run's process line and this one.
    const qint64 before = bootOf(previous);
    const qint64 now = bootOf(record);
    if (before >= 0 && now >= 0 && before != now) {
        submit(QStringLiteral("process"), QStringLiteral("info"),
               QStringLiteral("previous run: Android has restarted since (boot %1 -> %2)").arg(before).arg(now));
    }
}

void
DiagnosticsLog::noteSessionAttempt(const QString& context, AttemptStage stage) {
    const quint64 now = nowMs();
    const bool usbAccess = !context.isEmpty() && stage == AttemptStage::UsbAccess;
    bool mark = !usbAccess;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (context.isEmpty()) {
            // The plain marker of the Starting transition, unless an explicit start note just described this attempt.
            mark = !(m_attemptAbsorb && now - m_startNoteAt < kAttemptDedupeMs);
            m_attemptAbsorb = false;
        } else if (usbAccess) {
            m_attemptAbsorb = false;
        } else {
            m_startNoteAt = now;
            m_attemptAbsorb = true;
        }
        m_startupUntil = now + kStartupSyncWindowMs;
    }
    if (usbAccess) {
        // Not a session marker: the start that follows records its own, and a request whose start never comes (denied,
        // abandoned) leaves nothing a later start could be mistaken for.
        submit(QStringLiteral("session"), QStringLiteral("info"),
               QStringLiteral("usb access requested: %1").arg(context));
    } else if (mark) {
        const QByteArray record =
            formatRecord(QStringLiteral("session"), QStringLiteral("info"),
                         context.isEmpty() ? QStringLiteral("--- session starting ---")
                                           : QStringLiteral("--- session starting: %1 ---").arg(context));
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_sessionLine = record;
        }
        enqueue(record, true, Pin::Session);
    }
    // Best effort: a slow or failing store must never hold up listening.
    (void)flush(kAttemptFlushMs);
}

void
DiagnosticsLog::armStartupWindow() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_startupUntil = nowMs() + kStartupSyncWindowMs;
    m_ready.notify_one();
}

QStringList
DiagnosticsLog::snapshot(quint64* generation) const {
    std::lock_guard<std::mutex> lock(m_ringMutex);
    if (generation) {
        *generation = m_generation;
    }
    return m_ring;
}

QStringList
DiagnosticsLog::hostSnapshot() const {
    std::lock_guard<std::mutex> lock(m_ringMutex);
    return m_hostRing;
}

quint64
DiagnosticsLog::clear() {
    // Use submit's lock order. The clear precedes every subsequently queued
    // record, including when an older batch is already being written.
    std::lock_guard<std::mutex> queueLock(m_queueMutex);
    std::lock_guard<std::mutex> ringLock(m_ringMutex);
    m_queue.clear();
    m_hostQueue.clear();
    m_clearPending = true;
    if (m_platform == PlatformLines::Queued) {
        m_platform = PlatformLines::Lost;
    }
    // The cleared session's marker must not come back as a later segment's header.
    m_sessionLine.clear();
    m_ring.clear();
    m_ringIds.clear();
    m_pinned = 0;
    m_hostRing.clear();
    m_hostPrevious = 0;
    m_ready.notify_one();
    return ++m_generation;
}

bool
DiagnosticsLog::flush(int timeoutMs) {
    return flushOutcome(timeoutMs) == FlushOutcome::Completed;
}

DiagnosticsLog::FlushOutcome
DiagnosticsLog::flushOutcome(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    const quint64 ticket = ++m_flushRequested;
    if (m_options.manualDrain) {
        step(lock, true);
        return flushResult(ticket) ? FlushOutcome::Completed : FlushOutcome::Failed;
    }
    m_ready.notify_one();
    const auto done = [this, ticket] { return m_flushDone >= ticket; };
    if (timeoutMs < 0) {
        m_flushed.wait(lock, done);
    } else if (!m_flushed.wait_for(lock, std::chrono::milliseconds(timeoutMs), done)) {
        return FlushOutcome::TimedOut;
    }
    return flushResult(ticket) ? FlushOutcome::Completed : FlushOutcome::Failed;
}

bool
DiagnosticsLog::flushResult(quint64 ticket) const {
    const auto covering =
        std::find_if(m_flushResults.cbegin(), m_flushResults.cend(),
                     [ticket](const std::pair<quint64, bool>& result) { return result.first >= ticket; });
    return covering != m_flushResults.cend() && covering->second;
}

void
DiagnosticsLog::completeFlushes(quint64 through, bool ok) {
    m_flushDone = through;
    m_flushResults.emplace_back(through, ok);
    if (m_flushResults.size() > static_cast<size_t>(flushResultLimit)) {
        m_flushResults.erase(m_flushResults.begin());
    }
    m_flushed.notify_all();
}

DiagnosticsLog::IoStats
DiagnosticsLog::ioStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_tail.stats;
}

void
DiagnosticsLog::setIoFaultForTest(IoFault fault) {
    std::lock_guard<std::mutex> lock(m_stepMutex);
    m_fault = fault;
}

bool
DiagnosticsLog::syncDue(const Segments& segments, quint64 now) {
    return segments.dirtyBytes > 0 && now - segments.lastSync >= kSyncIntervalMs && now >= segments.syncRetryAt;
}

bool
DiagnosticsLog::dueLocked(quint64 now) const {
    const bool drainDue =
        (!m_queue.empty() || !m_hostQueue.empty()) && (inStartupWindow(now) || now - m_lastDrain >= kWriteIntervalMs);
    return m_clearPending || m_flushRequested > m_flushDone || drainDue || syncDue(m_tail, now) || syncDue(m_host, now);
}

bool
DiagnosticsLog::pump() {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    if (!m_options.manualDrain || !dueLocked(nowMs())) {
        return false;
    }
    step(lock, false);
    return true;
}

void
DiagnosticsLog::trimFragment(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() == 0) {
        return;
    }
    char last = '\n';
    if (!file.seek(file.size() - 1) || !file.getChar(&last) || last == '\n') {
        return;
    }
    // A crash part-way through an append: cut back to the last whole record so the next one is not
    // joined to the fragment.
    const QByteArray data = readTail(path, file.size());
    file.close();
    (void)QFile::resize(path, data.lastIndexOf('\n') + 1);
}

void
DiagnosticsLog::closeCurrent(Segments& segments) {
    // Bytes still unsynced in a file being given up on are past syncing: the failure, not a retry, is what the next
    // flush reports. Without this, a dirty count with no file kept the writer due forever.
    if (segments.dirtyBytes > 0) {
        segments.fail();
        segments.dirtyBytes = 0;
    }
    segments.file.reset();
}

void
DiagnosticsLog::openCurrent(Segments& segments) {
    if (segments.file) {
        return;
    }
    const QString path = filePath(segments.current);
    const bool existed = QFile::exists(path);
    if (existed) {
        trimFragment(path);
    }
    auto file = std::make_unique<QFile>(path);
    // Unbuffered: a sync must cover every byte written, with nothing left in a userspace buffer.
    if (m_fault == IoFault::SyncAndOpen
        || !file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered)) {
        segments.fail();
        return;
    }
    segments.fileSize = file->size();
    segments.file = std::move(file);
    if (!existed) {
        dsd_fsync_parent_dir(QFile::encodeName(path).constData());
    }
}

bool
DiagnosticsLog::appendBytes(Segments& segments, const QByteArray& bytes) const {
    if (!segments.file) {
        return false;
    }
    if (m_fault == IoFault::Write || segments.file->write(bytes) != bytes.size()) {
        // Reopened (and any partial record trimmed) on the next append.
        segments.fail();
        closeCurrent(segments);
        return false;
    }
    segments.fileSize += bytes.size();
    segments.dirtyBytes += static_cast<quint64>(bytes.size());
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++segments.stats.appends;
    segments.stats.bytes += static_cast<quint64>(bytes.size());
    return true;
}

bool
DiagnosticsLog::syncCurrent(Segments& segments) const {
    if (!segments.file) {
        return false;
    }
    // On Windows QFile's handle() is a C runtime descriptor over the file, so this is _commit(), a real flush to disk.
    if (m_fault == IoFault::Sync || m_fault == IoFault::SyncAndOpen || dsd_fsync(segments.file->handle()) != 0) {
        segments.fail();
        // Retried after an interval, never in a loop: a store that keeps failing must not spin the writer.
        segments.syncRetryAt = nowMs() + kSyncIntervalMs;
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ++segments.stats.syncFailures;
        return false;
    }
    segments.dirtyBytes = 0;
    segments.lastSync = nowMs();
    // Storage recovered: the startup window's per-append syncs must not wait out an earlier failure's backoff.
    segments.syncRetryAt = 0;
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++segments.stats.syncs;
    return true;
}

void
DiagnosticsLog::rotate(Segments& segments, const QByteArray& header) {
    // The current segment's data must be on disk before it becomes the previous segment.
    if (segments.dirtyBytes > 0) {
        (void)syncCurrent(segments);
    }
    closeCurrent(segments);
    const QString tailPath = filePath(segments.current);
    const QString prevPath = filePath(segments.previous);
    QFile::remove(prevPath);
    if (!QFile::rename(tailPath, prevPath)) {
        // Keep the bound: start the current segment over rather than let it grow.
        QFile file(tailPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            (void)dsd_fsync(file.handle());
        }
        if (!segments.rotationFailureNoted) {
            segments.rotationFailureNoted = true;
            ringNote(QStringLiteral("[host] [warn] diagnostics: the %1 could not be rotated; its older lines "
                                    "were dropped")
                         .arg(QLatin1String(segments.label)),
                     &segments == &m_host);
        }
    }
    dsd_fsync_parent_dir(QFile::encodeName(tailPath).constData());
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ++segments.stats.rotations;
    }
    openCurrent(segments);
    // Each segment starts with the process and session it belongs to.
    if (segments.file && !header.isEmpty()) {
        (void)appendBytes(segments, header);
    }
}

bool
DiagnosticsLog::clearFiles(Segments& segments) const {
    // What the cleared file held unsynced no longer matters: Clear removes it.
    segments.dirtyBytes = 0;
    segments.file.reset();
    const QString tailPath = filePath(segments.current);
    const QString prevPath = filePath(segments.previous);
    bool ok = !QFile::exists(prevPath) || QFile::remove(prevPath);
    QFile file(tailPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || m_fault == IoFault::Sync
        || dsd_fsync(file.handle()) != 0) {
        ok = false;
    }
    dsd_fsync_parent_dir(QFile::encodeName(tailPath).constData());
    segments.fileSize = 0;
    segments.dirtyBytes = 0;
    if (!ok) {
        // A clear that did not reach the disk is not durable, and the next flush says so.
        segments.fail();
    }
    return ok;
}

QByteArray
DiagnosticsLog::headerExcluding(const std::deque<QByteArray>::const_iterator& from,
                                const std::deque<QByteArray>::const_iterator& end) const {
    // A context line still waiting in the batch (the process or session line itself) is written once, as the
    // record it is, not again in the header ahead of it.
    QByteArray header;
    for (const auto& line : m_header.split('\n')) {
        const QByteArray entry = line + '\n';
        if (!line.isEmpty() && std::find(from, end, entry) == end) {
            header += entry;
        }
    }
    return header;
}

bool
DiagnosticsLog::writeBatch(Segments& segments, const std::deque<QByteArray>& batch) {
    openCurrent(segments);
    if (!segments.file) {
        return false;
    }
    QByteArray pending;
    // An empty segment (a new tail, or the first one after Clear) starts with its context like a rotated one.
    if (segments.fileSize == 0) {
        pending = headerExcluding(batch.cbegin(), batch.cend());
    }
    for (auto it = batch.cbegin(); it != batch.cend(); ++it) {
        const QByteArray& record = *it;
        if (segments.fileSize + pending.size() + record.size() > segments.limit
            && segments.fileSize + pending.size() > 0) {
            if (!pending.isEmpty() && !appendBytes(segments, pending)) {
                return false;
            }
            pending.clear();
            rotate(segments, headerExcluding(it, batch.cend()));
            if (!segments.file) {
                return false;
            }
        }
        pending += record;
    }
    return pending.isEmpty() || appendBytes(segments, pending);
}

void
DiagnosticsLog::step(std::unique_lock<std::mutex>& lock, bool forceSync) {
    StepWork work;
    work.batch.swap(m_queue);
    work.hostBatch.swap(m_hostQueue);
    work.clear = m_clearPending;
    m_clearPending = false;
    const quint64 through = m_flushRequested;
    work.flushing = through > m_flushDone;
    if (m_platform == PlatformLines::Queued) {
        work.platformLines = true;
        m_platform = PlatformLines::Taken;
    }
    work.now = nowMs();
    work.window = inStartupWindow(work.now);
    m_lastDrain = work.now;
    for (const QByteArray* line : {&m_processLine, &m_sessionLine}) {
        if (!line->isEmpty()) {
            work.header += *line + '\n';
        }
    }
    lock.unlock();
    PlatformLines platform = PlatformLines::None;
    const bool ok = writeStep(work, forceSync, &platform);
    lock.lock();
    if (platform != PlatformLines::None && m_platform == PlatformLines::Taken) {
        m_platform = platform;
    }
    if (work.flushing && through > m_flushDone) {
        completeFlushes(through, ok);
    }
}

bool
DiagnosticsLog::syncIfDue(Segments& segments, const StepWork& work, bool forceSync) const {
    const bool due = forceSync || work.window || work.clear || work.now - segments.lastSync >= kSyncIntervalMs;
    if (segments.dirtyBytes > 0 && due && (forceSync || work.now >= segments.syncRetryAt)) {
        return syncCurrent(segments);
    }
    return true;
}

bool
DiagnosticsLog::takeFlushResult(bool ok) {
    // A flush is durable only if nothing failed since the last one completed.
    for (const Segments* segments : {&m_tail, &m_host}) {
        ok = ok && !segments->ioFailed && segments->dirtyBytes == 0;
    }
    m_tail.ioFailed = false;
    m_host.ioFailed = false;
    return ok;
}

DiagnosticsLog::PlatformLines
DiagnosticsLog::settlePlatformLines(const StepWork& work, quint64 failuresBefore, bool clearedBefore) {
    if (work.platformLines) {
        // This step's host write holds them: any host failure from its start on can have lost them.
        m_platformUnsynced = true;
        m_platformFailures = failuresBefore;
    } else if (clearedBefore && m_platformUnsynced) {
        // Clear dropped them unsynced.
        m_platformUnsynced = false;
        return PlatformLines::Lost;
    }
    if (!m_platformUnsynced) {
        return PlatformLines::None;
    }
    if (m_host.failures != m_platformFailures) {
        m_platformUnsynced = false;
        return PlatformLines::Lost;
    }
    if (m_host.dirtyBytes == 0) {
        m_platformUnsynced = false;
        return PlatformLines::Durable;
    }
    return PlatformLines::None;
}

bool
DiagnosticsLog::writeStep(const StepWork& work, bool forceSync, PlatformLines* platform) {
    std::lock_guard<std::mutex> stepLock(m_stepMutex);
    m_header = work.header;
    const quint64 hostFailuresBefore = m_host.failures;
    bool ok = true;
    if (work.clear) {
        ok = clearFiles(m_tail);
        ok = clearFiles(m_host) && ok;
    }
    if (!work.batch.empty()) {
        ok = writeBatch(m_tail, work.batch) && ok;
    }
    if (!work.hostBatch.empty()) {
        ok = writeBatch(m_host, work.hostBatch) && ok;
    }
    ok = syncIfDue(m_tail, work, forceSync) && ok;
    ok = syncIfDue(m_host, work, forceSync) && ok;
    *platform = settlePlatformLines(work, hostFailuresBefore, work.clear);
    return work.flushing ? takeFlushResult(ok) : ok;
}

bool
DiagnosticsLog::drainedLocked() const {
    return m_queue.empty() && m_hostQueue.empty() && !m_clearPending && m_flushRequested == m_flushDone;
}

quint64
DiagnosticsLog::writerWaitLocked(quint64 now) const {
    quint64 waitMs = std::numeric_limits<quint64>::max();
    if (!m_queue.empty() || !m_hostQueue.empty()) {
        waitMs = kWriteIntervalMs - (now - m_lastDrain);
    }
    for (const Segments* segments : {&m_tail, &m_host}) {
        if (segments->dirtyBytes > 0) {
            const quint64 due = std::max<quint64>(segments->lastSync + kSyncIntervalMs, segments->syncRetryAt);
            waitMs = std::min<quint64>(waitMs, due > now ? due - now : 0U);
        }
    }
    return waitMs;
}

void
DiagnosticsLog::writeLoop() {
    // Only this thread writes the persistent segments (outside manual-drain tests).
    std::unique_lock<std::mutex> lock(m_queueMutex);
    for (;;) {
        const quint64 now = nowMs();
        if (m_stop || dueLocked(now)) {
            const bool stopping = m_stop;
            step(lock, stopping || m_flushRequested > m_flushDone);
            if (stopping && drainedLocked()) {
                break;
            }
            continue;
        }
        const quint64 waitMs = writerWaitLocked(now);
        if (waitMs == std::numeric_limits<quint64>::max()) {
            m_ready.wait(lock);
        } else {
            m_ready.wait_for(lock, std::chrono::milliseconds(waitMs));
        }
    }
}

DiagnosticsLogModel::DiagnosticsLogModel(DiagnosticsLog* log, QObject* parent)
    : QAbstractListModel(parent), m_log(log ? log : &DiagnosticsLog::instance()) {}

int
DiagnosticsLogModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant
DiagnosticsLogModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    return role == Qt::UserRole + 1 || role == Qt::DisplayRole ? m_rows.at(index.row()) : QVariant();
}

QHash<int, QByteArray>
DiagnosticsLogModel::roleNames() const {
    return {{Qt::UserRole + 1, "line"}};
}

void
DiagnosticsLogModel::setPaused(bool value) {
    if (value == m_paused) {
        return;
    }
    m_paused = value;
    Q_EMIT pausedChanged();
    refresh();
}

void
DiagnosticsLogModel::markSessionStarting() {
    // Arms the startup window; the plain marker is skipped when the start path already noted this attempt.
    m_log->noteSessionAttempt(QString());
}

void
DiagnosticsLogModel::setActive(bool value) {
    if (value == m_active) {
        return;
    }
    m_active = value;
    Q_EMIT activeChanged();
    if (!m_active) {
        return;
    }
    if (m_stale) {
        // One reset for everything consumed while hidden. Paused, that is the lines as they were at the
        // pause (the pending count covers the rest); otherwise the newest lines.
        QStringList rows = m_held;
        if (!m_paused) {
            quint64 generation = 0;
            rows = m_log->snapshot(&generation);
            m_seen = generation;
        }
        showRows(rows);
    }
    refresh();
}

void
DiagnosticsLogModel::showRows(const QStringList& rows) {
    m_held.clear();
    m_stale = false;
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

void
DiagnosticsLogModel::refresh() {
    quint64 generation = 0;
    const auto rows = m_log->snapshot(&generation);
    const int pending = m_paused ? static_cast<int>(std::min<quint64>(generation - m_seen, 2000)) : 0;
    if (pending != m_pending) {
        m_pending = pending;
        Q_EMIT pendingCountChanged();
    }
    if (m_paused || generation == m_seen) {
        return;
    }
    m_seen = generation;
    if (!m_active) {
        // Consumed as an active model would (a pause counts from here), shown when a view opens.
        m_held = rows;
        m_stale = true;
        return;
    }
    showRows(rows);
}

QString
DiagnosticsLogModel::allText() const {
    // The host records first: what explains a failure, however much decoder output the full log below lost them to.
    QStringList rows = m_log->hostSnapshot();
    if (!rows.isEmpty()) {
        rows.prepend(QStringLiteral("--- host and session records ---"));
        rows.append(QStringLiteral("--- all records ---"));
    }
    rows.append(m_log->snapshot());
    return rows.join(QLatin1Char('\n'));
}

void
DiagnosticsLogModel::copyAll() const {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(allText());
    }
}

void
DiagnosticsLogModel::clear() {
    const quint64 clearedGeneration = m_log->clear();
    m_held.clear();
    m_stale = false;
    beginResetModel();
    m_rows.clear();
    m_seen = clearedGeneration;
    endResetModel();
    m_pending = 0;
    Q_EMIT pendingCountChanged();
}
} // namespace dsd_qt
