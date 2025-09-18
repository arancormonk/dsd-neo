// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unicode/ASCII fallback utilities implementation.
 */

#include <dsd-neo/runtime/unicode.h>

#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#define HAVE_LANGINFO 1
#include <langinfo.h>
#endif

static int g_unicode_cached = 0;
static int g_unicode_supported = 0;

static int
str_ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);
        if (ca != cb) {
            return 0;
        }
    }
    return *a == *b;
}

int
dsd_unicode_supported(void) {
    if (g_unicode_cached) {
        return g_unicode_supported;
    }

    const char* force_ascii = getenv("DSD_FORCE_ASCII");
    if (force_ascii && (*force_ascii == '1' || *force_ascii == 'y' || *force_ascii == 'Y')) {
        g_unicode_supported = 0;
        g_unicode_cached = 1;
        return 0;
    }
    const char* force_utf8 = getenv("DSD_FORCE_UTF8");
    if (force_utf8 && (*force_utf8 == '1' || *force_utf8 == 'y' || *force_utf8 == 'Y')) {
        g_unicode_supported = 1;
        g_unicode_cached = 1;
        return 1;
    }

    /* Ensure locale is initialized */
    setlocale(LC_CTYPE, "");

#if HAVE_LANGINFO
    const char* codeset = nl_langinfo(CODESET);
    if (codeset && (str_ieq(codeset, "UTF-8") || str_ieq(codeset, "UTF8"))) {
        g_unicode_supported = 1;
    } else {
        g_unicode_supported = (MB_CUR_MAX > 1) ? 1 : 0;
    }
#else
    g_unicode_supported = (MB_CUR_MAX > 1) ? 1 : 0;
#endif
    g_unicode_cached = 1;
    return g_unicode_supported;
}

const char*
dsd_degrees_glyph(void) {
    return dsd_unicode_supported() ? "\xC2\xB0" : " deg";
}

char*
dsd_ascii_fallback(const char* in, char* out, size_t out_sz) {
    if (!in || !out || out_sz == 0) {
        return out;
    }
    if (dsd_unicode_supported()) {
        /* Copy with truncation */
        size_t n = strlen(in);
        if (n >= out_sz) {
            n = out_sz - 1;
        }
        memcpy(out, in, n);
        out[n] = '\0';
        return out;
    }
    /* Replace a small set of common glyphs with ASCII */
    size_t oi = 0;
    for (size_t i = 0; in[i] && oi + 1 < out_sz;) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            out[oi++] = (char)c;
            i++;
            continue;
        }
        /* Multi-byte sequences we recognize */
        if ((unsigned char)in[i] == 0xE2) {
            /* E2 89 88: ≈  | E2 80 93: – | E2 80 94: — | E2 80 A6: … */
            if ((unsigned char)in[i + 1] == 0x89 && (unsigned char)in[i + 2] == 0x88) {
                out[oi++] = '~';
                i += 3;
                continue;
            }
            if ((unsigned char)in[i + 1] == 0x80 && (unsigned char)in[i + 2] == 0x93) {
                out[oi++] = '-';
                i += 3;
                continue;
            }
            if ((unsigned char)in[i + 1] == 0x80 && (unsigned char)in[i + 2] == 0x94) {
                out[oi++] = '-';
                i += 3;
                continue;
            }
            if ((unsigned char)in[i + 1] == 0x80 && (unsigned char)in[i + 2] == 0xA6) {
                if (oi + 3 < out_sz) {
                    out[oi++] = '.';
                    out[oi++] = '.';
                    out[oi++] = '.';
                }
                i += 3;
                continue;
            }
        } else if ((unsigned char)in[i] == 0xC2) {
            /* C2 B0: ° | C2 B5: µ */
            if ((unsigned char)in[i + 1] == 0xB0) {
                const char* rep = " deg";
                for (size_t k = 0; rep[k] && oi + 1 < out_sz; ++k) {
                    out[oi++] = rep[k];
                }
                i += 2;
                continue;
            }
            if ((unsigned char)in[i + 1] == 0xB5) {
                out[oi++] = 'u';
                i += 2;
                continue;
            }
        } else if ((unsigned char)in[i] == 0xC3) {
            /* C3 97: × */
            if ((unsigned char)in[i + 1] == 0x97) {
                out[oi++] = 'x';
                i += 2;
                continue;
            }
        }
        /* Unknown non-ASCII: replace with '?' */
        out[oi++] = '?';
        /* Skip continuation bytes */
        if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            i++;
        }
    }
    out[oi] = '\0';
    return out;
}
