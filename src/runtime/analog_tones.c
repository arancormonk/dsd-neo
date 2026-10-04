// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <string.h>

/*
 * The standard 50-tone EIA/TIA CTCSS table in tenths of a hertz, ascending, with 150.0 Hz, the
 * tone many radios offer as a 51st, in its place between 146.2 and 151.4 Hz. It sits 1.4 Hz from
 * 151.4 Hz, closer than any other two tones; the detector keeps the two apart with gates of half
 * that distance (src/dsp/analog_ctcss.c).
 */
static const uint16_t k_ctcss_tones_tenths[] = {
    670,  693,  719,  744,  770,  797,  825,  854,  885,  915,  948,  974,  1000, 1035, 1072, 1109, 1148,
    1188, 1230, 1273, 1318, 1365, 1413, 1462, 1500, 1514, 1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773,
    1799, 1835, 1862, 1899, 1928, 1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};

_Static_assert(sizeof(k_ctcss_tones_tenths) / sizeof(k_ctcss_tones_tenths[0]) == DSD_CTCSS_TONE_COUNT,
               "the CTCSS table holds the standard 50 tones and 150.0 Hz");

int
dsd_ctcss_tone_count(void) {
    return DSD_CTCSS_TONE_COUNT;
}

int
dsd_ctcss_tone_tenths(int index) {
    if (index < 0 || index >= DSD_CTCSS_TONE_COUNT) {
        return -1;
    }
    return (int)k_ctcss_tones_tenths[index];
}

int
dsd_ctcss_tone_index(int tenths_hz) {
    int lo = 0;
    int hi = DSD_CTCSS_TONE_COUNT - 1;
    while (lo <= hi) {
        const int mid = lo + ((hi - lo) / 2);
        const int value = (int)k_ctcss_tones_tenths[mid];
        if (value == tenths_hz) {
            return mid;
        }
        if (value < tenths_hz) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

static int
ctcss_format_with(const char* prefix, const char* suffix, int tenths_hz, char* buf, size_t buf_size) {
    if (buf == NULL || buf_size == 0U) {
        return -1;
    }
    buf[0] = '\0';
    if (tenths_hz < 1 || tenths_hz > 9999) {
        return -1;
    }
    const int written = DSD_SNPRINTF(buf, buf_size, "%s%d.%d%s", prefix, tenths_hz / 10, tenths_hz % 10, suffix);
    if (written < 0 || (size_t)written >= buf_size) {
        buf[0] = '\0';
        return -1;
    }
    return written;
}

int
dsd_ctcss_format(int tenths_hz, char* buf, size_t buf_size) {
    return ctcss_format_with("", "", tenths_hz, buf, buf_size);
}

int
dsd_ctcss_format_label(int tenths_hz, char* buf, size_t buf_size) {
    return ctcss_format_with("CTCSS ", " Hz", tenths_hz, buf, buf_size);
}

/* ------------------------------------------------------------------------------------------
 * DCS (issue #523)
 * ---------------------------------------------------------------------------------------- */

/* The standard 104-code DCS set, ascending, each code as its value (023 octal = 19). */
static const uint16_t k_dcs_codes[] = {
    0023, 0025, 0026, 0031, 0032, 0036, 0043, 0047, 0051, 0053, 0054, 0065, 0071, 0072, 0073, 0074, 0114, 0115,
    0116, 0122, 0125, 0131, 0132, 0134, 0143, 0145, 0152, 0155, 0156, 0162, 0165, 0172, 0174, 0205, 0212, 0223,
    0225, 0226, 0243, 0244, 0245, 0246, 0251, 0252, 0255, 0261, 0263, 0265, 0266, 0271, 0274, 0306, 0311, 0315,
    0325, 0331, 0332, 0343, 0346, 0351, 0356, 0364, 0365, 0371, 0411, 0412, 0413, 0423, 0431, 0432, 0445, 0446,
    0452, 0454, 0455, 0462, 0464, 0465, 0466, 0503, 0506, 0516, 0523, 0526, 0532, 0546, 0565, 0606, 0612, 0624,
    0627, 0631, 0632, 0654, 0662, 0664, 0703, 0712, 0723, 0731, 0732, 0734, 0743, 0754,
};

_Static_assert(sizeof(k_dcs_codes) / sizeof(k_dcs_codes[0]) == DSD_DCS_CODE_COUNT,
               "the DCS table holds exactly the standard 104 codes");

/* Golay (23,12) generator x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1, bit i the coefficient of x^i. */
#define DCS_GOLAY_GENERATOR 0xC75U
#define DCS_WORD_MASK       ((1U << DSD_DCS_WORD_BITS) - 1U)
/* The fixed data bits 9-11, "100" as printed most significant first: only bit 11 set. */
#define DCS_MARKER          0x800U
#define DCS_MARKER_FIELD    0xE00U

int
dsd_dcs_code_count(void) {
    return DSD_DCS_CODE_COUNT;
}

int
dsd_dcs_code(int index) {
    if (index < 0 || index >= DSD_DCS_CODE_COUNT) {
        return -1;
    }
    return (int)k_dcs_codes[index];
}

int
dsd_dcs_code_index(int code) {
    int lo = 0;
    int hi = DSD_DCS_CODE_COUNT - 1;
    while (lo <= hi) {
        const int mid = lo + ((hi - lo) / 2);
        const int value = (int)k_dcs_codes[mid];
        if (value == code) {
            return mid;
        }
        if (value < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/* Remainder of a(x) modulo the generator, for a(x) of degree below 23. */
static uint32_t
dcs_golay_remainder(uint32_t a) {
    for (int bit = DSD_DCS_WORD_BITS - 1; bit >= 11; bit--) {
        if ((a >> bit) & 1U) {
            a ^= DCS_GOLAY_GENERATOR << (bit - 11);
        }
    }
    return a;
}

/*
 * The word is d(x) + x^12 p(x) for the 12 data bits d and the 11 check bits p, and must be a
 * multiple of g(x). g(x) divides x^23 + 1, so x^23 = 1 modulo g, and multiplying
 * x^12 p(x) = d(x) (mod g) by x^11 gives p(x) = x^11 d(x) mod g.
 */
uint32_t
dsd_dcs_word(int code, int inverted) {
    if (code < 0 || code > DSD_DCS_CODE_MAX) {
        return 0U;
    }
    const uint32_t data = (uint32_t)code | DCS_MARKER;
    const uint32_t word = data | (dcs_golay_remainder(data << 11) << 12);
    return inverted ? (~word & DCS_WORD_MASK) : word;
}

static uint32_t
dcs_rotate_right(uint32_t word, int k) {
    if (k == 0) {
        return word;
    }
    return ((word >> k) | (word << (DSD_DCS_WORD_BITS - k))) & DCS_WORD_MASK;
}

/* The supported code whose word @p word is, or -1. */
static int
dcs_supported_code_of(uint32_t word) {
    if ((word & DCS_MARKER_FIELD) != DCS_MARKER) {
        return -1;
    }
    const int code = (int)(word & 0x1FFU);
    if (dsd_dcs_code_index(code) < 0 || dsd_dcs_word(code, 0) != word) {
        return -1;
    }
    return code;
}

int
dsd_dcs_match(uint32_t window, int* code, int* inverted) {
    window &= DCS_WORD_MASK;
    int best_code = -1;
    int best_inverted = 1;
    for (int k = 0; k < DSD_DCS_WORD_BITS; k++) {
        const uint32_t rotated = dcs_rotate_right(window, k);
        for (int inv = 0; inv < 2; inv++) {
            const int found = dcs_supported_code_of(inv ? (~rotated & DCS_WORD_MASK) : rotated);
            if (found < 0) {
                continue;
            }
            /* Normal polarity first, then the lowest code. */
            if (best_code < 0 || inv < best_inverted || (inv == best_inverted && found < best_code)) {
                best_code = found;
                best_inverted = inv;
            }
        }
    }
    if (best_code < 0) {
        return 0;
    }
    if (code) {
        *code = best_code;
    }
    if (inverted) {
        *inverted = best_inverted;
    }
    return 1;
}

int
dsd_dcs_canonical(int code, int inverted, int* canon_code, int* canon_inverted) {
    const uint32_t word = dsd_dcs_word(code, inverted);
    if (word == 0U || !dsd_dcs_match(word, canon_code, canon_inverted)) {
        return -1;
    }
    return 0;
}

/*
 * A code sent inverted sends the complement of its normal word, so the inverted spellings of a
 * signal are the normal spellings of its complement, and the other way round: the complement's
 * canonical member, in the other polarity, spells this signal too. In the standard set every
 * signal has exactly one spelling in each polarity, so the complement's canonical member is its
 * normal spelling, and in the other polarity it is this signal's one inverted spelling, never
 * the canonical (normal) one. RUNTIME_ANALOG_TONES checks that over every standard code in both
 * polarities.
 */
int
dsd_dcs_alias(int code, int inverted, int* alias_code, int* alias_inverted) {
    int canon_code = -1;
    int canon_inverted = -1;
    if (dsd_dcs_canonical(code, inverted, &canon_code, &canon_inverted) != 0) {
        return -1;
    }
    int other_code = -1;
    int other_inverted = -1;
    if (dsd_dcs_canonical(canon_code, canon_inverted ? 0 : 1, &other_code, &other_inverted) != 0) {
        return -1;
    }
    other_inverted = other_inverted ? 0 : 1;
    if (other_code == canon_code && other_inverted == canon_inverted) {
        return -1;
    }
    if (alias_code) {
        *alias_code = other_code;
    }
    if (alias_inverted) {
        *alias_inverted = other_inverted;
    }
    return 0;
}

static char
dcs_polarity_letter(int inverted) {
    return inverted ? 'I' : 'N';
}

/* Keeps @p buf terminated: an empty string unless the whole text fit. */
static int
dcs_format_result(int written, char* buf, size_t buf_size) {
    if (written < 0 || (size_t)written >= buf_size) {
        buf[0] = '\0';
        return -1;
    }
    return written;
}

int
dsd_dcs_format(int code, int inverted, char* buf, size_t buf_size) {
    if (buf == NULL || buf_size == 0U) {
        return -1;
    }
    buf[0] = '\0';
    if (code < 0 || code > DSD_DCS_CODE_MAX) {
        return -1;
    }
    const int written = DSD_SNPRINTF(buf, buf_size, "D%03o%c", (unsigned int)code, dcs_polarity_letter(inverted));
    return dcs_format_result(written, buf, buf_size);
}

int
dsd_dcs_format_label(int code, int inverted, char* buf, size_t buf_size) {
    if (buf == NULL || buf_size == 0U) {
        return -1;
    }
    buf[0] = '\0';
    int canon_code = -1;
    int canon_inverted = -1;
    int alias_code = -1;
    int alias_inverted = -1;
    if (dsd_dcs_canonical(code, inverted, &canon_code, &canon_inverted) != 0
        || dsd_dcs_alias(code, inverted, &alias_code, &alias_inverted) != 0) {
        return -1;
    }
    const int written = DSD_SNPRINTF(buf, buf_size, "DCS D%03o%c / D%03o%c", (unsigned int)canon_code,
                                     dcs_polarity_letter(canon_inverted), (unsigned int)alias_code,
                                     dcs_polarity_letter(alias_inverted));
    return dcs_format_result(written, buf, buf_size);
}

/* ------------------------------------------------------------------------------------------
 * Tone lists (issue #527)
 * ---------------------------------------------------------------------------------------- */

/* DCS bit (2 * index + polarity) of the set's words, as core/analog_tone.h lays them out. */
enum { TONE_SET_WORD_BITS = 64, TONE_SET_DCS_BITS = 2 * DSD_DCS_CODE_COUNT };

_Static_assert((int)DSD_CTCSS_TONE_COUNT <= (int)TONE_SET_WORD_BITS, "every supported tone has a bit");
_Static_assert(TONE_SET_DCS_BITS <= (int)(sizeof(((dsd_tone_set*)0)->dcs) * 8U), "every code spelling has a bit");

/* U+2026 HORIZONTAL ELLIPSIS, before the count of entries a display text leaves out. */
#define TONE_SET_MORE_MARK "\xE2\x80\xA6"

static int
tone_set_dcs_bit(const dsd_tone_set* set, int bit) {
    return (int)((set->dcs[bit / TONE_SET_WORD_BITS] >> (bit % TONE_SET_WORD_BITS)) & 1U);
}

static void
tone_set_dcs_put(dsd_tone_set* set, int bit) {
    set->dcs[bit / TONE_SET_WORD_BITS] |= (uint64_t)1 << (bit % TONE_SET_WORD_BITS);
}

static int
tone_set_ctcss_bit(const dsd_tone_set* set, int index) {
    return (int)((set->ctcss >> index) & 1U);
}

/* One parsed entry: a CTCSS table index, or a DCS table index and polarity. */
typedef struct {
    int is_dcs;
    int index;
    int inverted;
} tone_entry;

static int
tone_digit(char c) {
    return c >= '0' && c <= '9';
}

/* "100" or "100.0": one to three digits, then optionally '.' and exactly one digit. Tenths of a hertz, or -1. */
static int
tone_entry_ctcss_tenths(const char* text, size_t len) {
    size_t i = 0;
    int whole = 0;
    while (i < len && i < 3U && tone_digit(text[i])) {
        whole = (whole * 10) + (text[i] - '0');
        i++;
    }
    if (i == 0U) {
        return -1;
    }
    if (i == len) {
        return whole * 10;
    }
    if (text[i] != '.' || i + 2U != len || !tone_digit(text[i + 1U])) {
        return -1;
    }
    return (whole * 10) + (text[i + 1U] - '0');
}

/* "D023", "D023N" or "D023I", any case: the code's value and polarity, or -1. */
static int
tone_entry_dcs_code(const char* text, size_t len, int* inverted) {
    if ((len != 4U && len != 5U) || (text[0] != 'D' && text[0] != 'd')) {
        return -1;
    }
    int code = 0;
    for (size_t i = 1U; i < 4U; i++) {
        if (text[i] < '0' || text[i] > '7') {
            return -1;
        }
        code = (code * 8) + (text[i] - '0');
    }
    *inverted = 0;
    if (len == 5U) {
        const char polarity = text[4];
        if (polarity == 'I' || polarity == 'i') {
            *inverted = 1;
        } else if (polarity != 'N' && polarity != 'n') {
            return -1;
        }
    }
    return code;
}

/* The standard tone or code an entry of @p len bytes names; 0, or -1 when it names none. */
static int
tone_entry_read(const char* text, size_t len, tone_entry* out) {
    const int tenths = tone_entry_ctcss_tenths(text, len);
    if (tenths >= 0) {
        out->is_dcs = 0;
        out->index = dsd_ctcss_tone_index(tenths);
        out->inverted = 0;
        return out->index >= 0 ? 0 : -1;
    }
    int inverted = 0;
    const int code = tone_entry_dcs_code(text, len, &inverted);
    out->is_dcs = 1;
    out->index = code >= 0 ? dsd_dcs_code_index(code) : -1;
    out->inverted = inverted;
    return out->index >= 0 ? 0 : -1;
}

/* What the parser has seen so far: the set, and for each tone and each DCS signal (by its canonical code's table index)
   the entry number that listed it, 0 for none. */
typedef struct {
    dsd_tone_set set;
    int ctcss_entry[DSD_CTCSS_TONE_COUNT];
    int dcs_entry[DSD_DCS_CODE_COUNT];
} tone_list_parse;

static int
tone_list_error(char* err, size_t err_size, const char* text) {
    if (err && err_size > 0U) {
        DSD_SNPRINTF(err, err_size, "%s", text);
    }
    return -1;
}

/* Record a CTCSS entry, or say which earlier entry it repeats. */
static int
tone_list_add_ctcss(tone_list_parse* parse, const tone_entry* entry, int number, char* err, size_t err_size) {
    const int earlier = parse->ctcss_entry[entry->index];
    if (earlier != 0) {
        char tone[16];
        (void)dsd_ctcss_format(dsd_ctcss_tone_tenths(entry->index), tone, sizeof(tone));
        char why[DSD_TONE_LIST_ERROR_SIZE];
        DSD_SNPRINTF(why, sizeof(why), "entry %d repeats entry %d (%s Hz)", number, earlier, tone);
        return tone_list_error(err, err_size, why);
    }
    parse->ctcss_entry[entry->index] = number;
    parse->set.ctcss |= (uint64_t)1 << entry->index;
    return 0;
}

/* Record a DCS entry, or say which earlier entry sends the same signal, named by its canonical spelling. */
static int
tone_list_add_dcs(tone_list_parse* parse, const tone_entry* entry, int number, char* err, size_t err_size) {
    int canon_code = -1;
    int canon_inverted = 0;
    if (dsd_dcs_canonical(dsd_dcs_code(entry->index), entry->inverted, &canon_code, &canon_inverted) != 0) {
        return tone_list_error(err, err_size, "internal error: a standard code has no signal");
    }
    const int signal = dsd_dcs_code_index(canon_code);
    const int bit = (2 * entry->index) + entry->inverted;
    const int earlier = signal >= 0 ? parse->dcs_entry[signal] : 0;
    if (earlier != 0) {
        char name[16];
        (void)dsd_dcs_format(canon_code, canon_inverted, name, sizeof(name));
        char why[DSD_TONE_LIST_ERROR_SIZE];
        if (tone_set_dcs_bit(&parse->set, bit)) {
            DSD_SNPRINTF(why, sizeof(why), "entry %d repeats entry %d (%s)", number, earlier, name);
        } else {
            DSD_SNPRINTF(why, sizeof(why), "entry %d is the same DCS signal as entry %d (%s)", number, earlier, name);
        }
        return tone_list_error(err, err_size, why);
    }
    if (signal >= 0) {
        parse->dcs_entry[signal] = number;
    }
    tone_set_dcs_put(&parse->set, bit);
    return 0;
}

/* Read the entry of @p len bytes at @p text, the @p number-th. */
static int
tone_list_add(tone_list_parse* parse, const char* text, size_t len, int number, char* err, size_t err_size) {
    char why[DSD_TONE_LIST_ERROR_SIZE];
    if (len == 0U) {
        DSD_SNPRINTF(why, sizeof(why), "entry %d is empty", number);
        return tone_list_error(err, err_size, why);
    }
    tone_entry entry;
    if (tone_entry_read(text, len, &entry) != 0) {
        DSD_SNPRINTF(why, sizeof(why), "entry %d is not a standard CTCSS tone or DCS code", number);
        return tone_list_error(err, err_size, why);
    }
    return entry.is_dcs ? tone_list_add_dcs(parse, &entry, number, err, err_size)
                        : tone_list_add_ctcss(parse, &entry, number, err, err_size);
}

int
dsd_tone_set_parse(const char* text, dsd_tone_set* out, char* err, size_t err_size) {
    if (!text || !out) {
        return tone_list_error(err, err_size, "no list");
    }
    const size_t len = strlen(text);
    if (len == 0U) {
        return tone_list_error(err, err_size, "the list is empty");
    }
    if (len > (size_t)DSD_TONE_LIST_TEXT_MAX) {
        return tone_list_error(err, err_size, "the list is longer than 1023 characters");
    }
    if (strchr(text, ',') != NULL) {
        return tone_list_error(err, err_size, "use / between entries, not commas");
    }
    tone_list_parse parse;
    DSD_MEMSET(&parse, 0, sizeof(parse));
    int number = 1;
    for (const char* entry = text;; number++) {
        const char* slash = strchr(entry, '/');
        const size_t entry_len = slash ? (size_t)(slash - entry) : strlen(entry);
        if (tone_list_add(&parse, entry, entry_len, number, err, err_size) != 0) {
            return -1;
        }
        if (!slash) {
            break;
        }
        entry = slash + 1;
    }
    *out = parse.set;
    return 0;
}

/* The @p ordinal-th entry of @p set in list order (tones ascending, then codes ascending, N before I), written as
   dsd_tone_set_format() writes it, with " Hz" after a tone when @p unit is set. Returns its length, or -1 when the set
   has no such entry or @p buf is too small. */
static int
tone_set_entry_text(const dsd_tone_set* set, int ordinal, int unit, char* buf, size_t buf_size) {
    int seen = 0;
    for (int i = 0; i < DSD_CTCSS_TONE_COUNT; i++) {
        if (tone_set_ctcss_bit(set, i) && seen++ == ordinal) {
            return ctcss_format_with("", unit ? " Hz" : "", dsd_ctcss_tone_tenths(i), buf, buf_size);
        }
    }
    for (int bit = 0; bit < TONE_SET_DCS_BITS; bit++) {
        if (tone_set_dcs_bit(set, bit) && seen++ == ordinal) {
            return dsd_dcs_format(dsd_dcs_code(bit / 2), bit % 2, buf, buf_size);
        }
    }
    return -1;
}

int
dsd_tone_set_count(const dsd_tone_set* set) {
    if (!set) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < DSD_CTCSS_TONE_COUNT; i++) {
        count += tone_set_ctcss_bit(set, i);
    }
    for (int bit = 0; bit < TONE_SET_DCS_BITS; bit++) {
        count += tone_set_dcs_bit(set, bit);
    }
    return count;
}

int
dsd_tone_set_format(const dsd_tone_set* set, char* buf, size_t buf_size) {
    if (!set || !buf || buf_size == 0U) {
        return -1;
    }
    buf[0] = '\0';
    const int count = dsd_tone_set_count(set);
    size_t used = 0U;
    for (int k = 0; k < count; k++) {
        char entry[16];
        const int n = tone_set_entry_text(set, k, 0, entry, sizeof(entry));
        const int written = DSD_SNPRINTF(buf + used, buf_size - used, "%s%s", k ? "/" : "", entry);
        if (n < 0 || written < 0 || (size_t)written >= buf_size - used) {
            buf[0] = '\0';
            return -1;
        }
        used += (size_t)written;
    }
    return (int)used;
}

/* Room a display text needs to end with "/…+left" (the separator only after an entry). */
static size_t
tone_set_more_len(int left, int after_entry) {
    char more[24];
    const int n = DSD_SNPRINTF(more, sizeof(more), "%s" TONE_SET_MORE_MARK "+%d", after_entry ? "/" : "", left);
    return n > 0 ? (size_t)n : 0U;
}

int
dsd_tone_set_format_display(const dsd_tone_set* set, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0U) {
        return -1;
    }
    buf[0] = '\0';
    if (!set || buf_size < 24U) {
        return -1;
    }
    const int count = dsd_tone_set_count(set);
    size_t used = 0U;
    for (int k = 0; k < count; k++) {
        char entry[24];
        const int n = tone_set_entry_text(set, k, 1, entry, sizeof(entry));
        const size_t with_entry = used + (k ? 1U : 0U) + (size_t)(n > 0 ? n : 0);
        const size_t tail = (k + 1 < count) ? tone_set_more_len(count - k - 1, 1) : 0U;
        if (n <= 0 || with_entry + tail >= buf_size) {
            (void)DSD_SNPRINTF(buf + used, buf_size - used, "%s" TONE_SET_MORE_MARK "+%d", k ? "/" : "", count - k);
            return (int)strlen(buf);
        }
        (void)DSD_SNPRINTF(buf + used, buf_size - used, "%s%s", k ? "/" : "", entry);
        used = with_entry;
    }
    return (int)used;
}

/* The ASCII spelling of TONE_SET_MORE_MARK, the same length, so it is swapped in place. */
#define TONE_SET_MORE_MARK_ASCII "..."

_Static_assert(sizeof(TONE_SET_MORE_MARK) == sizeof(TONE_SET_MORE_MARK_ASCII),
               "the overflow mark and its ASCII spelling are the same length");

void
dsd_tone_display_to_ascii(char* text) {
    if (!text) {
        return;
    }
    const size_t mark_len = sizeof(TONE_SET_MORE_MARK) - 1U;
    for (char* mark = strstr(text, TONE_SET_MORE_MARK); mark; mark = strstr(mark + mark_len, TONE_SET_MORE_MARK)) {
        DSD_MEMCPY(mark, TONE_SET_MORE_MARK_ASCII, mark_len);
    }
}

int
dsd_tone_set_has_dcs(const dsd_tone_set* set) {
    if (!set) {
        return 0;
    }
    for (size_t w = 0; w < sizeof(set->dcs) / sizeof(set->dcs[0]); w++) {
        if (set->dcs[w] != 0U) {
            return 1;
        }
    }
    return 0;
}

int
dsd_tone_set_equal(const dsd_tone_set* a, const dsd_tone_set* b) {
    if (!a || !b) {
        return a == b;
    }
    if (a->ctcss != b->ctcss) {
        return 0;
    }
    for (size_t w = 0; w < sizeof(a->dcs) / sizeof(a->dcs[0]); w++) {
        if (a->dcs[w] != b->dcs[w]) {
            return 0;
        }
    }
    return 1;
}

/* @p set with each DCS entry written as the canonical spelling of its signal (dsd_dcs_canonical()). */
static void
tone_set_by_signal(const dsd_tone_set* set, dsd_tone_set* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    out->ctcss = set->ctcss;
    for (int bit = 0; bit < TONE_SET_DCS_BITS; bit++) {
        if (!tone_set_dcs_bit(set, bit)) {
            continue;
        }
        int canon_code = -1;
        int canon_inverted = 0;
        const int index = dsd_dcs_canonical(dsd_dcs_code(bit / 2), bit % 2, &canon_code, &canon_inverted) == 0
                              ? dsd_dcs_code_index(canon_code)
                              : -1;
        tone_set_dcs_put(out, index >= 0 ? (2 * index) + canon_inverted : bit);
    }
}

int
dsd_tone_set_same_signals(const dsd_tone_set* a, const dsd_tone_set* b) {
    if (!a || !b) {
        return a == b;
    }
    dsd_tone_set a_signals;
    dsd_tone_set b_signals;
    tone_set_by_signal(a, &a_signals);
    tone_set_by_signal(b, &b_signals);
    return dsd_tone_set_equal(&a_signals, &b_signals);
}

int
dsd_tone_set_contains_ctcss(const dsd_tone_set* set, int tenths_hz) {
    const int index = dsd_ctcss_tone_index(tenths_hz);
    return set && index >= 0 && tone_set_ctcss_bit(set, index);
}

int
dsd_tone_set_contains_dcs(const dsd_tone_set* set, int code, int inverted) {
    int want_code = -1;
    int want_inverted = 0;
    if (!set || dsd_dcs_canonical(code, inverted ? 1 : 0, &want_code, &want_inverted) != 0) {
        return 0;
    }
    for (int bit = 0; bit < TONE_SET_DCS_BITS; bit++) {
        int listed_code = -1;
        int listed_inverted = 0;
        if (tone_set_dcs_bit(set, bit)
            && dsd_dcs_canonical(dsd_dcs_code(bit / 2), bit % 2, &listed_code, &listed_inverted) == 0
            && listed_code == want_code && listed_inverted == want_inverted) {
            return 1;
        }
    }
    return 0;
}

static const char* const k_tone_filter_mode_names[] = {"off", "allow", "block"};

const char*
dsd_tone_filter_mode_name(int mode) {
    if (mode < DSD_TONE_FILTER_OFF || mode > DSD_TONE_FILTER_BLOCK) {
        return NULL;
    }
    return k_tone_filter_mode_names[mode];
}

int
dsd_tone_filter_mode_parse(const char* text, int* mode) {
    if (!text || !mode) {
        return -1;
    }
    for (int m = DSD_TONE_FILTER_OFF; m <= DSD_TONE_FILTER_BLOCK; m++) {
        if (dsd_strcasecmp(text, k_tone_filter_mode_names[m]) == 0) {
            *mode = m;
            return 0;
        }
    }
    return -1;
}

int
dsd_tone_filter_check(int mode, const char* list, dsd_tone_set* out, char* err, size_t err_size) {
    const char* name = dsd_tone_filter_mode_name(mode);
    if (!out) {
        return tone_list_error(err, err_size, "no policy");
    }
    if (!name) {
        return tone_list_error(err, err_size, "the mode must be off, allow or block");
    }
    dsd_tone_set set;
    DSD_MEMSET(&set, 0, sizeof(set));
    if (!list || list[0] == '\0') {
        if (mode != DSD_TONE_FILTER_OFF) {
            char why[DSD_TONE_LIST_ERROR_SIZE];
            DSD_SNPRINTF(why, sizeof(why), "%s needs a list of CTCSS tones or DCS codes, e.g. 67.0/100.0/D023N", name);
            return tone_list_error(err, err_size, why);
        }
    } else if (dsd_tone_set_parse(list, &set, err, err_size) != 0) {
        return -1;
    }
    *out = set;
    return 0;
}

/* RTL stream output kind that carries monitor audio: RTL_STREAM_OUTPUT_AUDIO_MONITOR in the IO
   header runtime may not include. */
enum { ANALOG_TONES_RTL_OUTPUT_AUDIO_MONITOR = 0 };

static int
analog_tone_input_carries_audio(const dsd_opts* opts) {
    switch (opts->audio_in_type) {
        case AUDIO_IN_PULSE:
        case AUDIO_IN_STDIN:
        case AUDIO_IN_WAV:
        case AUDIO_IN_UDP:
        case AUDIO_IN_TCP: return 1;
        case AUDIO_IN_RTL: return dsd_rtl_stream_metrics_hook_output_kind() == ANALOG_TONES_RTL_OUTPUT_AUDIO_MONITOR;
        default: return 0;
    }
}

int
dsd_analog_monitor_tap_active(const dsd_opts* opts) {
    if (opts == NULL || opts->analog_only != 1 || opts->monitor_input_audio != 1) {
        return 0;
    }
    return analog_tone_input_carries_audio(opts);
}

int
dsd_analog_tone_detection_active(const dsd_opts* opts) {
    /* CTCSS and DCS are FM signalling: the AM monitor (issue #524) has none to detect. */
    return dsd_analog_monitor_tap_active(opts) && opts->analog_demod == DSD_ANALOG_DEMOD_FM;
}

int
dsd_analog_tone_gate_in_force(const dsd_opts* opts, const dsd_state* state) {
    /* A verdict describes the reception only while detection runs: anywhere else no policy is in force. */
    if (opts == NULL || state == NULL || !dsd_analog_tone_detection_active(opts)) {
        return DSD_ANALOG_TONE_GATE_OFF;
    }
    const int gate = state->analog_rx.gate;
    if (gate == DSD_ANALOG_TONE_GATE_OFF) {
        /* The tap never publishes OFF under a list policy it runs, so OFF here means no verdict of this policy: the tap
           has no session to judge with (one it could not allocate), or has not read since the policy came on. The
           policy fails closed, as PENDING, until the tap's own verdict replaces it. */
        const int list_policy =
            (opts->analog_tone_filter == DSD_TONE_FILTER_ALLOW || opts->analog_tone_filter == DSD_TONE_FILTER_BLOCK)
            && dsd_tone_set_count(&opts->analog_tone_set) > 0;
        return list_policy ? DSD_ANALOG_TONE_GATE_PENDING : DSD_ANALOG_TONE_GATE_OFF;
    }
    return (gate > DSD_ANALOG_TONE_GATE_OFF && gate <= DSD_ANALOG_TONE_GATE_REJECTED) ? gate
                                                                                      : DSD_ANALOG_TONE_GATE_PENDING;
}

int
dsd_analog_tone_gate_passes(int gate) {
    return gate == DSD_ANALOG_TONE_GATE_OFF || gate == DSD_ANALOG_TONE_GATE_ALLOWED;
}
