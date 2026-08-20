// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Pick a generated CSV from the imports directory. See csv_picker.h.
 */

#include "csv_picker.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference_import.h>

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define CSV_PICKER_PATH_SEP '\\'
#else
#define CSV_PICKER_PATH_SEP '/'
#endif

typedef struct {
    const char* kind;
    const char* dir;
    char (*paths)[CSV_PICKER_PATH_MAX];
    char (*labels)[CSV_PICKER_LABEL_MAX];
    int max;
    int count;
} CsvCollectCtx;

static int
csv_collect_entry(const char* name, void* user) {
    CsvCollectCtx* ctx = (CsvCollectCtx*)user;
    if (ctx->count >= ctx->max) {
        return 1; /* full; stop the walk */
    }
    const size_t len = (name != NULL) ? strlen(name) : 0U;
    if (len < 5U || strcmp(name + (len - 4U), ".csv") != 0) {
        return 0;
    }

    char path[CSV_PICKER_PATH_MAX];
    const int n = DSD_SNPRINTF(path, sizeof path, "%s%c%s", ctx->dir, CSV_PICKER_PATH_SEP, name);
    if (n <= 0 || (size_t)n >= sizeof path) {
        return 0; /* a truncated path would name a different file */
    }

    dsd_rr_provenance prov;
    DSD_MEMSET(&prov, 0, sizeof prov);
    if (dsd_rr_provenance_read(path, &prov) != 0) {
        return 0; /* no sidecar: kind unknown, not offered */
    }
    if (strcmp(prov.kind, ctx->kind) != 0) {
        return 0;
    }
    (void)DSD_SNPRINTF(ctx->paths[ctx->count], CSV_PICKER_PATH_MAX, "%s", path);
    (void)DSD_SNPRINTF(ctx->labels[ctx->count], CSV_PICKER_LABEL_MAX, "%s", name);
    ctx->count++;
    return 0;
}

/*
 * Sort the two arrays in lockstep, in place: sorting labels alone would open a
 * different file than the row shows.
 *
 * Deliberately a selection sort over the caller's arrays rather than a qsort
 * over an array of {path,label} pairs. qsort takes no context, so pairing means
 * materialising the pairs, and CSV_PICKER_MAX of them is 128 * (1024 + 160) =
 * ~148 KB - a stack frame this menu callback has no business taking on the
 * 1 MB-default stacks of the win-msvc-* and Android builds. One row-sized temp
 * (~1.2 KB) and at most 128 swaps cost nothing at this bound.
 */
static void
csv_rows_sort(char paths[][CSV_PICKER_PATH_MAX], char labels[][CSV_PICKER_LABEL_MAX], int count) {
    char tmp_path[CSV_PICKER_PATH_MAX];
    char tmp_label[CSV_PICKER_LABEL_MAX];

    for (int i = 0; i + 1 < count; i++) {
        int lo = i;
        for (int j = i + 1; j < count; j++) {
            if (strcmp(labels[j], labels[lo]) < 0) {
                lo = j;
            }
        }
        if (lo == i) {
            continue;
        }
        DSD_MEMCPY(tmp_path, paths[i], sizeof tmp_path);
        DSD_MEMCPY(tmp_label, labels[i], sizeof tmp_label);
        DSD_MEMCPY(paths[i], paths[lo], sizeof tmp_path);
        DSD_MEMCPY(labels[i], labels[lo], sizeof tmp_label);
        DSD_MEMCPY(paths[lo], tmp_path, sizeof tmp_path);
        DSD_MEMCPY(labels[lo], tmp_label, sizeof tmp_label);
    }
}

int
ui_csv_picker_collect(const char* dir, const char* kind, char paths[][CSV_PICKER_PATH_MAX],
                      char labels[][CSV_PICKER_LABEL_MAX], int max) {
    if (dir == NULL || dir[0] == '\0' || kind == NULL || paths == NULL || labels == NULL || max <= 0) {
        return 0;
    }
    CsvCollectCtx ctx;
    ctx.kind = kind;
    ctx.dir = dir;
    ctx.paths = paths;
    ctx.labels = labels;
    ctx.max = max;
    ctx.count = 0;
    if (dsd_dir_list(dir, csv_collect_entry, &ctx) != 0 && ctx.count == 0) {
        return 0;
    }
    csv_rows_sort(paths, labels, ctx.count);
    return ctx.count;
}

/* ---- The chooser-or-prompt flow ------------------------------------------ */

/* One heap context outlives the chooser, which borrows every string it holds. */
typedef struct {
    char paths[CSV_PICKER_MAX][CSV_PICKER_PATH_MAX];
    char labels[CSV_PICKER_MAX][CSV_PICKER_LABEL_MAX];
    const char* items[CSV_PICKER_MAX + 1]; /* +1 for the "Enter a path..." row */
    int count;                             /* files, not counting the escape row */
    char prompt_title[96];
    size_t cap;
    ui_prompt_string_done_fn on_done;
    void* user_ctx;
} CsvPickerCtx;

static const char k_csv_picker_enter_path[] = "Enter a path...";

/*
 * The prompt's own done-callback, so the context outlives the prompt.
 *
 * ui_prompt_open_string_async() keeps the title BY POINTER for the whole life of
 * the prompt (menu_prompts.c, `g_prompt.title = title;`) and ui_prompt_render()
 * dereferences it on every frame. Freeing the context that owns the title right
 * after opening the prompt - which is what this used to do - left that pointer
 * dangling into a >128 KB block glibc returns to the OS, so the next render
 * faulted. The caller's callback is forwarded from here and the context is freed
 * exactly once, after the prompt has stopped reading it.
 */
static void
csv_picker_prompt_done(void* user, const char* text) {
    CsvPickerCtx* ctx = (CsvPickerCtx*)user;
    if (ctx == NULL) {
        return;
    }
    const ui_prompt_string_done_fn cb = ctx->on_done;
    void* cb_ctx = ctx->user_ctx;
    free(ctx);
    if (cb != NULL) {
        cb(cb_ctx, text);
    }
}

/** @brief Open the fall-back path prompt. Takes ownership of @p ctx. */
static void
csv_picker_prompt(CsvPickerCtx* ctx) {
    /* On an allocation failure inside the prompt this answers cancel
       synchronously, which frees ctx through csv_picker_prompt_done(); nothing
       may touch ctx after this call either way. */
    ui_prompt_open_string_async(ctx->prompt_title, NULL, ctx->cap, csv_picker_prompt_done, ctx);
}

static void
csv_picker_chooser_done(void* user, int sel) {
    CsvPickerCtx* ctx = (CsvPickerCtx*)user;
    if (ctx == NULL) {
        return;
    }
    if (sel < 0) {
        /* Cancel: report it to the caller as a cancelled prompt would. */
        if (ctx->on_done != NULL) {
            ctx->on_done(ctx->user_ctx, NULL);
        }
        free(ctx);
        return;
    }
    if (sel < ctx->count) {
        if (ctx->on_done != NULL) {
            ctx->on_done(ctx->user_ctx, ctx->paths[sel]);
        }
        free(ctx);
        return;
    }
    /* The final "Enter a path..." row: fall back to the prompt, which takes
       ownership of the context and frees it when it completes. */
    csv_picker_prompt(ctx);
}

/* cppcheck miscounts the parameters across the ui_prompt_string_done_fn callback
   typedef and reports arg 5 as unnamed in the declaration; menu_prompts.c
   suppresses the same false positive on its identical async signatures. */
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_csv_import_picker_open(const char* kind, const char* prompt_title, size_t cap, ui_prompt_string_done_fn on_done,
                          void* user_ctx) {
    const char* dir = dsd_user_imports_dir();

    CsvPickerCtx* ctx = (CsvPickerCtx*)calloc(1U, sizeof(*ctx));
    if (ctx == NULL) {
        /* Out of memory for the chooser context is not a reason to strand the
           user: fall back to the plain prompt. */
        ui_prompt_open_string_async(prompt_title, NULL, cap, on_done, user_ctx);
        return;
    }
    (void)DSD_SNPRINTF(ctx->prompt_title, sizeof ctx->prompt_title, "%s", (prompt_title != NULL) ? prompt_title : "");
    ctx->cap = cap;
    ctx->on_done = on_done;
    ctx->user_ctx = user_ctx;
    ctx->count = ui_csv_picker_collect(dir, kind, ctx->paths, ctx->labels, CSV_PICKER_MAX);
    /* collect() never returns out of [0, CSV_PICKER_MAX], but bound it explicitly
       so the index into items[] below is provably in range. */
    if (ctx->count < 0) {
        ctx->count = 0;
    } else if (ctx->count > CSV_PICKER_MAX) {
        ctx->count = CSV_PICKER_MAX;
    }

    if (ctx->count == 0) {
        /* No imports of this kind: the picker adds nothing, so prompt directly.
           The prompt owns the context from here. */
        csv_picker_prompt(ctx);
        return;
    }

    for (int i = 0; i < ctx->count; i++) {
        ctx->items[i] = ctx->labels[i];
    }
    ctx->items[ctx->count] = k_csv_picker_enter_path;
    ui_chooser_start(prompt_title, ctx->items, ctx->count + 1, csv_picker_chooser_done, ctx);
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed
