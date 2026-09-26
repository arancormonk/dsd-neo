// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/dmr_key_map.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/path_policy.h>
#include <dsd-neo/runtime/scan_options.h>
#include <iterator>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

namespace {
struct Bytes {
    Bytes() = default;
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;
    std::vector<char> data;

    ~Bytes() {
        if (!data.empty()) {
            DSD_SECURE_ZERO(data.data(), data.size());
        }
    }
};

struct Targets {
    Targets() = default;
    Targets(const Targets&) = delete;
    Targets& operator=(const Targets&) = delete;
    dsd_trunk_scan_target_list list{};

    ~Targets() { dsd_trunk_scan_target_list_reset(&list); }
};

struct Reference {
    size_t offset = 0, length = 0;
    unsigned int row = 0;
    dsd_app_scan_file_kind kind = DSD_APP_SCAN_FILE_CHANNEL;
    std::string field, path, resolved, prefix;
};

struct Document {
    Bytes bytes;
    Targets targets;
    std::vector<Reference> refs;
};

int
fail(char* err, size_t size, const char* reason) {
    if (err && size) {
        DSD_SNPRINTF(err, size, "%s", reason);
    }
    return -1;
}

bool
space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

char*
trim(char* p, bool unquote) {
    while (space(*p)) {
        ++p;
    }
    size_t n = std::strlen(p);
    while (n && space(p[n - 1])) {
        p[--n] = 0;
    }
    if (unquote && n >= 2 && p[0] == '"' && p[n - 1] == '"') {
        p[n - 1] = 0;
        ++p;
    }
    return p;
}

std::string
lower(const char* p) {
    std::string result(p);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; });
    return result;
}

int
readBytes(const char* path, Bytes& bytes, char* err, size_t size) {
    char resolved[2048];
    FILE* fp = path ? dsd_path_fopen_user_read_file(path, resolved, sizeof resolved) : nullptr;
    if (!fp) {
        return fail(err, size, "Cannot open CSV. Select an existing imported file.");
    }
    const auto closeFile = [](FILE* f) { std::fclose(f); };
    std::unique_ptr<FILE, decltype(closeFile)> file(fp, closeFile);
    if (std::fseek(fp, 0, SEEK_END)) {
        return fail(err, size, "Cannot read CSV.");
    }
    const long length = std::ftell(fp);
    if (length < 0 || length > 64L * 1024 * 1024 || std::fseek(fp, 0, SEEK_SET)) {
        return fail(err, size, "CSV exceeds the 64 MiB import limit or cannot be read.");
    }
    bytes.data.resize(static_cast<size_t>(length) + 1);
    if (std::fread(bytes.data.data(), 1, static_cast<size_t>(length), fp) != static_cast<size_t>(length)
        || std::memchr(bytes.data.data(), 0, static_cast<size_t>(length))) {
        return fail(err, size, "CSV contains a NUL byte or could not be read completely.");
    }
    return 0;
}

int
addRef(Document& doc, const char* base, unsigned int row, const char* field, const char* path,
       dsd_app_scan_file_kind kind, size_t offset, size_t length, const std::string& prefix = {}) {
    if (!*path) {
        return 0;
    }
    char resolved[2048];
    if (dsd_path_resolve_relative_to_file(base, path, resolved, sizeof resolved)) {
        return -1;
    }
    doc.refs.push_back({offset, length, row, kind, field, path, resolved, prefix});
    return 0;
}

struct OptionsContext {
    Document* doc;
    const char* base;
    unsigned int row;
    size_t offset;
};

int
optionRef(void* ptr, const char* option, const char* path, size_t offset, size_t length, int whole) {
    auto& ctx = *static_cast<OptionsContext*>(ptr);

    static const struct {
        const char* name;
        dsd_app_scan_file_kind kind;
    } options[] = {{"-K", DSD_APP_SCAN_FILE_KEYS_HEX},
                   {"-k", DSD_APP_SCAN_FILE_KEYS_DEC},
                   {"-G", DSD_APP_SCAN_FILE_GROUP},
                   {"--dmr-tg-key-csv", DSD_APP_SCAN_FILE_DMR_MAP}};

    const auto found = std::find_if(std::begin(options), std::end(options), [option](const auto& candidate) {
        return std::strcmp(option, candidate.name) == 0;
    });
    if (found == std::end(options)) {
        return -1;
    }
    return addRef(*ctx.doc, ctx.base, ctx.row, option, path, found->kind, ctx.offset + offset, length,
                  whole ? std::string(option) + "=" : std::string());
}

struct Columns {
    bool channel = false;
    std::vector<std::string> names;
    int paths[3] = {-1, -1, -1};
};

int
splitFields(char* raw, bool channel, std::vector<char*>& fields) {
    if (!channel) {
        char* cells[32];
        size_t count = 0;
        if (dsd_trunk_scan_split_csv_fields(raw, cells, 32, &count)) {
            return -1;
        }
        fields.assign(cells, cells + count);
        return 0;
    }
    char* cell = raw;
    do {
        fields.push_back(cell);
        char* next = std::strchr(cell, ',');
        if (!next) {
            break;
        }
        *next = 0;
        cell = next + 1;
    } while (true);
    return 0;
}

std::string
fieldName(const Columns& columns, size_t index) {
    if (!columns.channel) {
        // chan_csv is positional, not a recognized optional header. An extra
        // column with that name is opaque user data to the engine.
        if (index != 3 && index < columns.names.size() && columns.names[index] == "chan_csv") {
            return {};
        }
        return index < columns.names.size() ? columns.names[index] : std::string();
    }
    const char* names[] = {"keys_hex_csv", "keys_dec_csv", "options"};
    for (size_t i = 0; i < 3; ++i) {
        if (columns.paths[i] >= 0 && static_cast<size_t>(columns.paths[i]) == index) {
            return names[i];
        }
    }
    return {};
}

int
inspectField(Document& doc, const char* base, unsigned int row, const std::string& field, const char* value,
             size_t offset) {
    if (!*value) {
        return 0;
    }
    if (field == "options" || field == "relevant_cli_switches") {
        OptionsContext ctx{&doc, base, row, offset};
        return dsd_scan_options_visit_files(value, &ctx, optionRef);
    }

    static const struct {
        const char* name;
        dsd_app_scan_file_kind kind;
    } paths[] = {{"chan_csv", DSD_APP_SCAN_FILE_CHANNEL},
                 {"p25_bandplan_csv", DSD_APP_SCAN_FILE_BANDPLAN},
                 {"keys_hex_csv", DSD_APP_SCAN_FILE_KEYS_HEX},
                 {"keys_dec_csv", DSD_APP_SCAN_FILE_KEYS_DEC}};

    const auto found =
        std::find_if(std::begin(paths), std::end(paths), [&field](const auto& p) { return field == p.name; });
    return found == std::end(paths)
               ? 0
               : addRef(doc, base, row, found->name, value, found->kind, offset, std::strlen(value));
}

bool
inactiveChannelKeys(const Columns& columns, const std::vector<char*>& fields) {
    if (!columns.channel || dsd_csv_channel_has_slot(fields.front())) {
        return false;
    }
    const int options = columns.paths[2];
    return options < 0 || static_cast<size_t>(options) >= fields.size()
           || !*trim(fields[static_cast<size_t>(options)], false);
}

int
inspectLine(Document& doc, const char* base, Columns& columns, char* raw, unsigned int row, size_t offset, char* err,
            size_t size) {
    if (row == 1 && columns.channel) {
        return dsd_csv_channel_path_columns(raw, columns.paths) ? fail(err, size, "Invalid channel-map header.") : 0;
    }
    std::vector<char*> fields;
    if (splitFields(raw, columns.channel, fields)) {
        return fail(err, size, "Too many target fields.");
    }
    if (row == 1) {
        std::transform(fields.begin(), fields.end(), std::back_inserter(columns.names),
                       [](char* cell) { return lower(trim(cell, true)); });
        return 0;
    }
    const bool inactive = inactiveChannelKeys(columns, fields);
    for (size_t col = 0; col < fields.size(); ++col) {
        const auto field = fieldName(columns, col);
        if (inactive && field != "options") {
            continue;
        }
        const char* value = trim(fields[col], !columns.channel);
        if (inspectField(doc, base, row, field, value, offset + static_cast<size_t>(value - raw))) {
            if (err && size) {
                DSD_SNPRINTF(err, size, "row %u: %s has an invalid path or option operand", row, field.c_str());
            }
            return -1;
        }
    }
    return 0;
}

int
inspectDocument(const char* path, const char* base, bool channel, Document& doc, char* err, size_t size) {
    if (err && size) {
        err[0] = 0;
    }
    if (readBytes(path, doc.bytes, err, size)) {
        return -1;
    }
    if (!base) {
        base = path;
    }
    if (!channel && dsd_trunk_scan_load_targets_csv(path, nullptr, &doc.targets.list, err, size)) {
        return -1;
    }
    const size_t length = doc.bytes.data.size() - 1;
    Columns columns;
    columns.channel = channel;
    unsigned int row = 0;
    for (size_t begin = 0; begin < length;) {
        size_t end = begin;
        while (end < length && doc.bytes.data[end] != '\n') {
            ++end;
        }
        Bytes line;
        line.data.resize(end - begin + 1);
        std::memcpy(line.data.data(), doc.bytes.data.data() + begin, end - begin);
        if (inspectLine(doc, base, columns, line.data.data(), ++row, begin, err, size)) {
            return -1;
        }
        begin = end + 1;
    }
    return 0;
}

const char*
targetTypeName(dsd_trunk_scan_target_type type) {
    switch (type) {
        case DSD_TRUNK_SCAN_TARGET_P25_TRUNK: return "p25-trunk";
        case DSD_TRUNK_SCAN_TARGET_DMR_TRUNK: return "dmr-trunk";
        case DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL: return "dmr-conventional";
        case DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK: return "nxdn-trunk";
        case DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL: return "nxdn-conventional";
        case DSD_TRUNK_SCAN_TARGET_NXDN48_TRUNK: return "nxdn48-trunk";
        case DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL: return "nxdn48-conventional";
        case DSD_TRUNK_SCAN_TARGET_P25_CONVENTIONAL: return "p25-conventional";
    }
    return "";
}

dsd_app_scan_csv_target
describe(const dsd_trunk_scan_target& t) {
    dsd_app_scan_csv_target result{};
    DSD_SNPRINTF(result.id, sizeof result.id, "%s", t.id);
    DSD_SNPRINTF(result.type, sizeof result.type, "%s", targetTypeName(t.type));
    const char* modulation = "";
    switch (t.modulation) {
        case DSD_TRUNK_SCAN_MODULATION_AUTO: modulation = "auto"; break;
        case DSD_TRUNK_SCAN_MODULATION_C4FM: modulation = "c4fm"; break;
        case DSD_TRUNK_SCAN_MODULATION_CQPSK: modulation = "cqpsk"; break;
        case DSD_TRUNK_SCAN_MODULATION_GFSK: modulation = "gfsk"; break;
        default: break;
    }
    DSD_SNPRINTF(result.modulation, sizeof result.modulation, "%s", modulation);
    result.frequency_hz = t.frequency_hz;
    result.row = t.csv_row;
    result.dwell_ms = t.dwell_is_set ? t.dwell_ms : -1;
    result.hold_ms = t.activity_hold_is_set ? t.activity_hold_ms : -1;
    result.gain_db = t.rtl_gain_is_set ? t.rtl_gain_db : -1;
    result.squelch_db_set = (t.row_options.present & DSD_SCAN_OPT_SQUELCH) ? 1 : 0;
    result.squelch_db = result.squelch_db_set ? t.row_options.squelch_db : 0;
    return result;
}

bool
safeReplacement(const char* path) {
    if (!path || !*path || *path == '-') {
        return false;
    }
    for (const char* p = path; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '/' || *p == '.'
              || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

int
validateReference(const Reference& ref) {
    dsd_csv_validation stats{};
    switch (ref.kind) {
        case DSD_APP_SCAN_FILE_CHANNEL: return dsd_csv_validate_chan_file(ref.resolved.c_str(), &stats);
        case DSD_APP_SCAN_FILE_BANDPLAN:
            return dsd_csv_validate_p25_bandplan_file(ref.resolved.c_str(), &stats) || !stats.accepted ? -1 : 0;
        case DSD_APP_SCAN_FILE_KEYS_HEX: return dsd_csv_validate_key_file_hex(ref.resolved.c_str(), &stats);
        case DSD_APP_SCAN_FILE_KEYS_DEC: return dsd_csv_validate_key_file_dec(ref.resolved.c_str(), &stats);
        case DSD_APP_SCAN_FILE_GROUP: return dsd_csv_validate_group_file(ref.resolved.c_str(), &stats);
        case DSD_APP_SCAN_FILE_DMR_MAP: {
            dsd_dmr_key_map map{};
            return dsd_dmr_key_map_load(ref.resolved.c_str(), &map);
        }
    }
    return -1;
}

int
validateCompleteReference(const Reference& ref, char* err, size_t size) {
    if (ref.kind == DSD_APP_SCAN_FILE_CHANNEL) {
        Document channel;
        char detail[256] = {};
        if (inspectDocument(ref.resolved.c_str(), nullptr, true, channel, detail, sizeof detail)) {
            if (err && size) {
                DSD_SNPRINTF(err, size, "row %u chan_csv: %s", ref.row, detail);
            }
            return -1;
        }
        const auto invalid = std::find_if(channel.refs.begin(), channel.refs.end(),
                                          [](const Reference& leaf) { return validateReference(leaf) != 0; });
        if (invalid != channel.refs.end()) {
            if (err && size) {
                DSD_SNPRINTF(err, size,
                             "row %u chan_csv: channel row %u %s companion is missing, unreadable or invalid", ref.row,
                             invalid->row, invalid->field.c_str());
            }
            return -1;
        }
    }
    if (validateReference(ref)) {
        if (err && size) {
            DSD_SNPRINTF(err, size, "row %u: %s companion is missing, unreadable or invalid", ref.row,
                         ref.field.c_str());
        }
        return -1;
    }
    return 0;
}
} // namespace

extern "C" int
dsd_app_scan_csv_inspect(const char* path, const char* base_path, int channel_map,
                         const dsd_app_scan_csv_callbacks* callbacks, char* err, size_t err_sz) {
    try {
        Document doc;
        if (inspectDocument(path, base_path, channel_map != 0, doc, err, err_sz)) {
            return -1;
        }
        if (callbacks && callbacks->target) {
            for (size_t i = 0; i < doc.targets.list.count; ++i) {
                const auto target = describe(doc.targets.list.targets[i]);
                callbacks->target(&target, callbacks->context);
            }
        }
        if (callbacks && callbacks->reference) {
            for (size_t i = 0; i < doc.refs.size(); ++i) {
                const auto& r = doc.refs[i];
                const dsd_app_scan_csv_reference ref{
                    i, r.row, r.kind, r.field.c_str(), r.path.c_str(), r.resolved.c_str()};
                callbacks->reference(&ref, callbacks->context);
            }
        }
        return 0;
    } catch (...) {
        return fail(err, err_sz, "Cannot allocate CSV inspection storage.");
    }
}

namespace {
int
replacementPaths(const Document& doc, const dsd_app_scan_csv_replacement* replacements, size_t count,
                 std::vector<const char*>& paths, char* err, size_t size) {
    if (count != doc.refs.size() || (count && !replacements)) {
        return fail(err, size, "Select all companion files.");
    }
    paths.resize(count, nullptr);
    for (size_t i = 0; i < count; ++i) {
        const auto& r = replacements[i];
        if (r.reference_index >= count || paths[r.reference_index] || !safeReplacement(r.relative_path)) {
            return fail(err, size, "Invalid companion replacement.");
        }
        paths[r.reference_index] = r.relative_path;
    }
    return 0;
}

struct Output {
    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    FILE* file = nullptr;
    char path[2048] = {};

    ~Output() {
        if (file) {
            std::fclose(file);
        }
        if (path[0]) {
            std::remove(path);
        }
    }
};

bool
writeReplacements(FILE* fp, const Document& doc, const std::vector<const char*>& paths) {
    size_t copied = 0;
    const size_t size = doc.bytes.data.size() - 1;
    for (size_t i = 0; i < doc.refs.size(); ++i) {
        const auto& r = doc.refs[i];
        if (r.offset < copied || r.offset > size || r.length > size - r.offset) {
            return false;
        }
        const auto replacement = r.prefix + paths[i];
        if (std::fwrite(doc.bytes.data.data() + copied, 1, r.offset - copied, fp) != r.offset - copied
            || std::fwrite(replacement.data(), 1, replacement.size(), fp) != replacement.size()) {
            return false;
        }
        copied = r.offset + r.length;
    }
    return std::fwrite(doc.bytes.data.data() + copied, 1, size - copied, fp) == size - copied;
}

int
writeDocument(const Document& doc, const char* destination, const std::vector<const char*>& paths, char* err,
              size_t size) {
    Output output;
    output.file = dsd_fopen_private_temp_for_replace(destination, output.path, sizeof output.path, "wb");
    if (!output.file) {
        return fail(err, size, "Cannot create private CSV staging file.");
    }
    bool ok = writeReplacements(output.file, doc, paths);
    if (std::fclose(output.file)) {
        ok = false;
    }
    output.file = nullptr;
    if (!ok || dsd_replace_file_with_temp(output.path, destination)) {
        return fail(err, size, "Cannot write complete target CSV.");
    }
    output.path[0] = 0;
    return 0;
}
} // namespace

extern "C" int
dsd_app_scan_csv_rewrite(const char* path, const char* base_path, int channel_map, const char* destination,
                         const dsd_app_scan_csv_replacement* replacements, size_t count, char* err, size_t err_sz) {
    try {
        if (!destination || (path && !std::strcmp(path, destination))) {
            return fail(err, err_sz, "Invalid CSV staging destination.");
        }
        Document doc;
        if (inspectDocument(path, base_path, channel_map != 0, doc, err, err_sz)) {
            return -1;
        }
        std::vector<const char*> paths;
        if (replacementPaths(doc, replacements, count, paths, err, err_sz)) {
            return -1;
        }
        return writeDocument(doc, destination, paths, err, err_sz);
    } catch (...) {
        return fail(err, err_sz, "Cannot allocate CSV rewrite storage.");
    }
}

extern "C" int
dsd_app_trunk_scan_validate_bundle(const char* path, int* count, uint32_t* first_hz, char* err, size_t err_sz) {
    if (count) {
        *count = 0;
    }
    if (first_hz) {
        *first_hz = 0;
    }
    try {
        Document doc;
        if (inspectDocument(path, nullptr, false, doc, err, err_sz)) {
            return -1;
        }
        const auto invalid = std::find_if(doc.refs.begin(), doc.refs.end(), [err, err_sz](const Reference& ref) {
            return validateCompleteReference(ref, err, err_sz) != 0;
        });
        if (invalid != doc.refs.end()) {
            return -1;
        }
        if (doc.targets.list.count > INT_MAX) {
            return fail(err, err_sz, "Too many targets.");
        }
        if (count) {
            *count = static_cast<int>(doc.targets.list.count);
        }
        if (first_hz && doc.targets.list.count) {
            *first_hz = doc.targets.list.targets[0].frequency_hz;
        }
        return 0;
    } catch (...) {
        return fail(err, err_sz, "Cannot allocate CSV validation storage.");
    }
}
