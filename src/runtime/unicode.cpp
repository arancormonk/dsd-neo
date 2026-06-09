// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unicode/ASCII fallback utilities implementation.
 */

#include <ctype.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/unicode.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <windows.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#define HAVE_LANGINFO 1
#include <langinfo.h>
#endif

static int g_unicode_cached = 0;
static int g_unicode_supported = 0;
static int g_locale_inited = 0;
#if DSD_PLATFORM_WIN_NATIVE
static int g_block_glyphs_cached = 0;
static int g_block_glyphs_supported = 0;
#endif

static char g_cached_locale[128] = {0};
#if DSD_PLATFORM_WIN_NATIVE
static unsigned int g_cached_console_cp = 0;
#endif

static int
env_truthy(const char* name) {
    const char* v = getenv(name);
    if (!v || !*v) {
        return 0;
    }
    return (*v == '1' || *v == 'y' || *v == 'Y' || *v == 't' || *v == 'T');
}

static int
str_icontains(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) {
        return 0;
    }
    size_t nlen = strlen(needle);
    for (size_t i = 0; haystack[i]; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            unsigned char hc = (unsigned char)haystack[i + j];
            unsigned char nc = (unsigned char)needle[j];
            if (!hc) {
                break;
            }
            if (tolower(hc) != tolower(nc)) {
                break;
            }
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

#if DSD_PLATFORM_WIN_NATIVE
static int
env_nonempty(const char* name) {
    const char* v = getenv(name);
    return v && *v;
}

static int
wide_ascii_equal_ci(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        wchar_t ca = *a++;
        wchar_t cb = *b++;
        if (ca >= L'A' && ca <= L'Z') {
            ca = (wchar_t)(ca - L'A' + L'a');
        }
        if (cb >= L'A' && cb <= L'Z') {
            cb = (wchar_t)(cb - L'A' + L'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == L'\0' && *b == L'\0';
}

static int
windows_console_font_face(wchar_t face[LF_FACESIZE]) {
    if (!face) {
        return 0;
    }
    face[0] = L'\0';

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == NULL || out == INVALID_HANDLE_VALUE) {
        return 0;
    }

    CONSOLE_FONT_INFOEX info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (!GetCurrentConsoleFontEx(out, FALSE, &info)) {
        return 0;
    }

    size_t i = 0;
    for (; i + 1 < LF_FACESIZE && info.FaceName[i] != L'\0'; i++) {
        face[i] = info.FaceName[i];
    }
    face[i] = L'\0';
    return face[0] != L'\0';
}

static int
windows_font_has_block_glyphs(const wchar_t* face) {
    if (!face || face[0] == L'\0') {
        return 0;
    }

    if (wide_ascii_equal_ci(face, L"Terminal") || wide_ascii_equal_ci(face, L"Raster Fonts")) {
        return 0;
    }

    static const wchar_t required[] = {
        (wchar_t)0x2581, (wchar_t)0x2582, (wchar_t)0x2583, (wchar_t)0x2584,
        (wchar_t)0x2585, (wchar_t)0x2586, (wchar_t)0x2587, (wchar_t)0x2588,
    };
    WORD glyphs[sizeof(required) / sizeof(required[0])];
    DSD_MEMSET(glyphs, 0, sizeof(glyphs));

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        return 0;
    }

    HFONT font = CreateFontW(0, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_DONTCARE, face);
    if (!font) {
        DeleteDC(dc);
        return 0;
    }

    HGDIOBJ old_font = SelectObject(dc, font);
    DWORD rc = GetGlyphIndicesW(dc, required, (int)(sizeof(required) / sizeof(required[0])), glyphs,
                                GGI_MARK_NONEXISTING_GLYPHS);
    int supported = (rc != GDI_ERROR);
    if (supported) {
        for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
            if (glyphs[i] == 0xFFFFu) {
                supported = 0;
                break;
            }
        }
    }

    if (old_font) {
        SelectObject(dc, old_font);
    }
    DeleteObject(font);
    DeleteDC(dc);
    return supported;
}

static int
windows_console_block_glyphs_supported(void) {
    if (env_nonempty("WT_SESSION")) {
        return 1;
    }

    wchar_t face[LF_FACESIZE];
    if (!windows_console_font_face(face)) {
        return 0;
    }
    return windows_font_has_block_glyphs(face);
}
#endif

static int
locale_is_utf8(void) {
#if HAVE_LANGINFO
    const char* codeset = nl_langinfo(CODESET);
    if (codeset && (dsd_strcasecmp(codeset, "UTF-8") == 0 || dsd_strcasecmp(codeset, "UTF8") == 0)) {
        return 1;
    }
#endif

    const char* loc = setlocale(LC_CTYPE, NULL);
    if (loc && (str_icontains(loc, "UTF-8") || str_icontains(loc, "UTF8") || str_icontains(loc, "65001"))) {
        return 1;
    }
    return 0;
}

void
dsd_unicode_init_locale(void) {
    if (g_locale_inited) {
        return;
    }
    g_locale_inited = 1;

    /* Initialize locale from the user's environment first. */
    setlocale(LC_CTYPE, "");

    if (env_truthy("DSD_FORCE_ASCII")) {
        return;
    }

#if DSD_PLATFORM_WIN_NATIVE
    /* Prefer UTF-8 console code pages on native Windows terminals (best effort). */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    if (locale_is_utf8()) {
        g_unicode_cached = 0;
        return;
    }

#if DSD_PLATFORM_WIN_NATIVE
    static const char* const candidates[] = {".UTF8", ".UTF-8", "C.UTF-8", "en_US.UTF-8", NULL};
#else
    static const char* const candidates[] = {"C.UTF-8", "C.UTF8", "en_US.UTF-8", "en_US.UTF8", NULL};
#endif
    for (int i = 0; candidates[i] != NULL; i++) {
        if (setlocale(LC_CTYPE, candidates[i]) != NULL && locale_is_utf8()) {
            break;
        }
    }

    /* Locale/codepage changes can affect support detection; recompute on next query. */
    g_unicode_cached = 0;
}

int
dsd_unicode_supported(void) {
    if (env_truthy("DSD_FORCE_ASCII")) {
        g_unicode_supported = 0;
        g_unicode_cached = 1;
        return 0;
    }
    if (env_truthy("DSD_FORCE_UTF8")) {
        g_unicode_supported = 1;
        g_unicode_cached = 1;
        return 1;
    }

    dsd_unicode_init_locale();

    const char* loc = setlocale(LC_CTYPE, NULL);
    if (!loc) {
        loc = "";
    }
#if DSD_PLATFORM_WIN_NATIVE
    unsigned int cp = GetConsoleOutputCP();
    if (g_unicode_cached && strcmp(loc, g_cached_locale) == 0 && cp == g_cached_console_cp) {
        return g_unicode_supported;
    }
    g_cached_console_cp = cp;
#else
    if (g_unicode_cached && strcmp(loc, g_cached_locale) == 0) {
        return g_unicode_supported;
    }
#endif
    DSD_STRNCPY(g_cached_locale, loc, sizeof(g_cached_locale) - 1);
    g_cached_locale[sizeof(g_cached_locale) - 1] = '\0';

    g_unicode_supported = locale_is_utf8() ? 1 : 0;
#if DSD_PLATFORM_WIN_NATIVE
    /* Even without a UTF-8 CRT locale, a UTF-8 console code page can render UTF-8 bytes. */
    if (g_unicode_supported == 0 && cp == CP_UTF8) {
        g_unicode_supported = 1;
    }
#endif
    g_unicode_cached = 1;
    return g_unicode_supported;
}

const char*
dsd_unicode_or_ascii(const char* unicode_str, const char* ascii_str) {
    return dsd_unicode_supported() ? unicode_str : ascii_str;
}

int
dsd_unicode_block_glyphs_supported(void) {
    if (env_truthy("DSD_FORCE_ASCII")) {
        return 0;
    }
    if (env_truthy("DSD_FORCE_UTF8")) {
        return 1;
    }
    if (!dsd_unicode_supported()) {
        return 0;
    }

#if DSD_PLATFORM_WIN_NATIVE
    if (g_block_glyphs_cached) {
        return g_block_glyphs_supported;
    }
    g_block_glyphs_supported = windows_console_block_glyphs_supported() ? 1 : 0;
    g_block_glyphs_cached = 1;
    return g_block_glyphs_supported;
#else
    return 1;
#endif
}

const char*
dsd_degrees_glyph(void) {
    return dsd_unicode_supported() ? "\xC2\xB0" : " deg";
}

static void
append_ascii_text(char* out, size_t out_sz, size_t* oi, const char* rep) {
    for (size_t k = 0; rep[k] && *oi + 1 < out_sz; ++k) {
        out[(*oi)++] = rep[k];
    }
}

static size_t
utf8_sequence_advance(unsigned char c) {
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

static int
replace_known_utf8_sequence(const char* in, size_t* i, char* out, size_t out_sz, size_t* oi) {
    if ((unsigned char)in[*i] == 0xE2) {
        if ((unsigned char)in[*i + 1] == 0x89 && (unsigned char)in[*i + 2] == 0x88) {
            out[(*oi)++] = '~';
            *i += 3;
            return 1;
        }
        if ((unsigned char)in[*i + 1] == 0x80
            && ((unsigned char)in[*i + 2] == 0x93 || (unsigned char)in[*i + 2] == 0x94)) {
            out[(*oi)++] = '-';
            *i += 3;
            return 1;
        }
        if ((unsigned char)in[*i + 1] == 0x80 && (unsigned char)in[*i + 2] == 0xA6) {
            if (*oi + 3 < out_sz) {
                out[(*oi)++] = '.';
                out[(*oi)++] = '.';
                out[(*oi)++] = '.';
            }
            *i += 3;
            return 1;
        }
        return 0;
    }

    if ((unsigned char)in[*i] == 0xC2) {
        if ((unsigned char)in[*i + 1] == 0xB0) {
            append_ascii_text(out, out_sz, oi, " deg");
            *i += 2;
            return 1;
        }
        if ((unsigned char)in[*i + 1] == 0xB5) {
            out[(*oi)++] = 'u';
            *i += 2;
            return 1;
        }
        return 0;
    }

    if ((unsigned char)in[*i] == 0xC3 && (unsigned char)in[*i + 1] == 0x97) {
        out[(*oi)++] = 'x';
        *i += 2;
        return 1;
    }

    return 0;
}

static char*
copy_unicode_or_truncate(const char* in, char* out, size_t out_sz) {
    size_t n = strlen(in);
    if (n >= out_sz) {
        n = out_sz - 1;
    }
    DSD_MEMCPY(out, in, n);
    out[n] = '\0';
    return out;
}

char*
dsd_ascii_fallback(const char* in, char* out, size_t out_sz) {
    if (!in || !out || out_sz == 0) {
        return out;
    }
    if (dsd_unicode_supported()) {
        return copy_unicode_or_truncate(in, out, out_sz);
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
        if (replace_known_utf8_sequence(in, &i, out, out_sz, &oi)) {
            continue;
        }

        /* Unknown non-ASCII: replace with '?' */
        out[oi++] = '?';
        i += utf8_sequence_advance(c);
    }
    out[oi] = '\0';
    return out;
}
