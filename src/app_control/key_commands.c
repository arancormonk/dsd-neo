// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/dmr_key_map.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "command_dispatch.h"
#include "key_commands.h"

static dsd_mutex_t result_mutex;
static atomic_int mutex_state = 0;
static dsd_app_decryption_result retained;

static void
lock_result(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&mutex_state, &expected, 1)) {
        (void)dsd_mutex_init(&result_mutex);
        atomic_store(&mutex_state, 2);
    } else {
        while (atomic_load(&mutex_state) != 2) {
            dsd_thread_yield();
        }
    }
    dsd_mutex_lock(&result_mutex);
}

int
dsd_app_decryption_result_get(dsd_app_decryption_result* out) {
    if (!out) {
        return 0;
    }
    lock_result();
    *out = retained;
    dsd_mutex_unlock(&result_mutex);
    return out->sequence != 0;
}

void
dsd_app_publish_decryption_result(const struct dsd_app_command* command, int status) {
    if (!command || command->id != DSD_APP_CMD_DECRYPTION_APPLY) {
        return;
    }
    dsd_app_decryption_result result = {0};
    if (command->n >= offsetof(dsd_app_decryption_payload, tune_generation)) {
        DSD_MEMCPY(&result.request_id, command->data + offsetof(dsd_app_decryption_payload, request_id),
                   sizeof(result.request_id));
        DSD_MEMCPY(&result.session_generation, command->data + offsetof(dsd_app_decryption_payload, session_generation),
                   sizeof(result.session_generation));
    }
    if (command->n >= offsetof(dsd_app_decryption_payload, source)) {
        int32_t scope;
        DSD_MEMCPY(&scope, command->data + offsetof(dsd_app_decryption_payload, scope), sizeof(scope));
        result.scope = scope == DSD_APP_KEY_SCOPE_TARGET ? DSD_APP_KEY_SCOPE_TARGET : DSD_APP_KEY_SCOPE_DEFAULTS;
    }
    result.status = status > DSD_APP_KEY_APPLIED ? DSD_APP_KEY_INVALID : status;
    lock_result();
    result.sequence = retained.sequence == UINT64_MAX ? 1 : retained.sequence + 1;
    retained = result;
    dsd_mutex_unlock(&result_mutex);
}

static int
payload_text_terminated(const dsd_app_decryption_payload* p) {
    const struct {
        const char* text;
        size_t capacity;
    } strings[] = {{p->target_id, sizeof(p->target_id)}, {p->profile_ref, sizeof(p->profile_ref)},
                   {p->value, sizeof(p->value)},         {p->keys_hex, sizeof(p->keys_hex)},
                   {p->keys_dec, sizeof(p->keys_dec)},   {p->map_file, sizeof(p->map_file)}};

    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i) {
        if (!memchr(strings[i].text, 0, strings[i].capacity)) {
            return 0;
        }
    }
    return 1;
}

static int
valid_payload(const dsd_app_decryption_payload* p) {
    const uint32_t fields = DSD_APP_DECRYPTION_MATERIAL | DSD_APP_DECRYPTION_MAP | DSD_APP_DECRYPTION_FORCE;
    if (!p->request_id || !p->session_generation || (p->fields & ~fields) || p->scope < DSD_APP_KEY_SCOPE_DEFAULTS
        || p->scope > DSD_APP_KEY_SCOPE_TARGET || !payload_text_terminated(p)) {
        return 0;
    }
    if (strspn(p->profile_ref, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")
        != strlen(p->profile_ref)) {
        return 0;
    }
    if ((p->fields & DSD_APP_DECRYPTION_FORCE) && (p->force < 0 || p->force > 255 || (p->force > 1 && p->force < 32))) {
        return 0;
    }
    return 1;
}

static int
prepare_keys(const dsd_app_decryption_payload* p, dsd_key_set* keys) {
    if (!(p->fields & DSD_APP_DECRYPTION_MATERIAL)) {
        return DSD_APP_KEY_APPLIED;
    }
    keys->present = 1;
    if (p->source == DSD_APP_KEY_SOURCE_COLLECTION) {
        keys->keyloader = 1;
        if ((p->keys_hex[0] || p->keys_dec[0]) && dsd_key_set_load_csv(keys, p->keys_hex, p->keys_dec, 0)) {
            return DSD_APP_KEY_FILE_ERROR;
        }
    } else if (p->source == DSD_APP_KEY_SOURCE_DIRECT || p->source == DSD_APP_KEY_SOURCE_DIRECT_OVERLAY) {
        if (p->key_type < DSD_APP_KEY_TYPE_BASIC || p->key_type > DSD_APP_KEY_TYPE_M17_AES
            || dsd_key_set_load_typed(keys, (dsd_key_type)p->key_type, p->value) != DSD_KEY_DIRECT_OK) {
            return DSD_APP_KEY_INVALID;
        }
    } else if (p->source != DSD_APP_KEY_SOURCE_NONE) {
        return DSD_APP_KEY_INVALID;
    }
    DSD_SNPRINTF(keys->profile_ref, sizeof(keys->profile_ref), "%s", p->profile_ref);
    return DSD_APP_KEY_APPLIED;
}

static int
apply_defaults(dsd_opts* opts, dsd_state* state, const dsd_app_decryption_payload* p, dsd_key_set* keys,
               const dsd_dmr_key_map* map) {
    if (p->fields & DSD_APP_DECRYPTION_MATERIAL) {
        if (p->source == DSD_APP_KEY_SOURCE_DIRECT_OVERLAY) {
            if (dsd_scan_keys_apply_direct(state, (dsd_key_type)p->key_type, p->value) != DSD_KEY_DIRECT_OK) {
                return DSD_APP_KEY_INVALID;
            }
        } else if (state->scan_keys_active_set) {
            /* All allocation happened during preparation. Replacing the owned
             * baseline cannot disturb effective target keys or fail halfway. */
            dsd_key_set_free(&state->scan_keys_baseline);
            state->scan_keys_baseline = *keys;
            state->scan_keys_baseline.present = 0;
            DSD_MEMSET(keys, 0, sizeof(*keys));
        } else {
            dsd_key_set_install(state, keys);
        }
        dsd_key_apply_mute_policy(opts, state);
    }
    if (p->fields & DSD_APP_DECRYPTION_MAP) {
        const int suspended = dsd_scan_maps_suspend(state);
        (void)dsd_dmr_key_map_install(state, map);
        if (suspended) {
            dsd_scan_maps_resume(state);
        }
    }
    if (p->fields & DSD_APP_DECRYPTION_FORCE) {
        state->M = p->force;
    }
    if (p->fields) {
        dsd_enc_lockout_bump_key_epoch(state);
        dsd_trunk_scan_hook_enc_lockout_clear_snapshots(state);
    }
    return DSD_APP_KEY_APPLIED;
}

static int
vendor_context_active(const dsd_state* state) {
    /* These flags are outside key_set ownership; replacing ordinary material
     * cannot claim to have cleared the active vendor keystream. */
    return state->tyt_ap || state->tyt_bp || state->tyt_ep || state->baofeng_ap || state->csi_ee || state->retevis_ap
           || state->ken_sc || state->any_bp || state->straight_ks || state->vertex_ks_count;
}

static int
validate_live_target(const dsd_app_decryption_payload* p, const dsd_state* state) {
    if (p->scope == DSD_APP_KEY_SCOPE_TARGET
        && (!p->target_id[0] || strcmp(state->trunk_scan_active_id, p->target_id) != 0
            || !dsd_trunk_tuning_frame_is_current(p->tune_generation)
            || p->key_epoch != state->enc_lockout_key_epoch)) {
        return DSD_APP_KEY_STALE;
    }
    return vendor_context_active(state) ? DSD_APP_KEY_UNAVAILABLE : DSD_APP_KEY_APPLIED;
}

static int
apply_target(dsd_opts* opts, dsd_state* state, const dsd_app_decryption_payload* p, const dsd_key_set* keys,
             const dsd_dmr_key_map* map) {
    if (p->source == DSD_APP_KEY_SOURCE_DIRECT_OVERLAY) {
        return DSD_APP_KEY_UNAVAILABLE;
    }
    const uint32_t fields = ((p->fields & DSD_APP_DECRYPTION_MATERIAL) ? DSD_TRUNK_KEY_MATERIAL : 0U)
                            | ((p->fields & DSD_APP_DECRYPTION_MAP) ? DSD_TRUNK_KEY_MAP : 0U)
                            | ((p->fields & DSD_APP_DECRYPTION_FORCE) ? DSD_TRUNK_KEY_FORCE : 0U);
    const int status = dsd_trunk_scan_hook_decryption_apply(opts, state, p->target_id, p->tune_generation, fields, keys,
                                                            map, p->force);
    return status == DSD_TRUNK_KEY_APPLIED ? DSD_APP_KEY_APPLIED
           : status == DSD_TRUNK_KEY_STALE ? DSD_APP_KEY_STALE
           : status == DSD_TRUNK_KEY_BUSY  ? DSD_APP_KEY_BUSY
                                           : DSD_APP_KEY_UNAVAILABLE;
}

static int
prepare_map(const dsd_app_decryption_payload* p, dsd_dmr_key_map* map) {
    return (p->fields & DSD_APP_DECRYPTION_MAP) && p->map_file[0] && dsd_dmr_key_map_load(p->map_file, map);
}

int
dsd_app_apply_decryption(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* command) {
    if (!opts || !state || !command || command->n != sizeof(dsd_app_decryption_payload)) {
        return DSD_APP_KEY_INVALID;
    }
    dsd_app_decryption_payload p;
    DSD_MEMCPY(&p, command->data, sizeof(p));
    dsd_key_set keys = {0};
    dsd_dmr_key_map map = {0};
    int result = DSD_APP_KEY_INVALID;
    if (!valid_payload(&p)) {
        goto done;
    }
    if (p.session_generation != dsd_app_command_session_generation()) {
        result = DSD_APP_KEY_STALE;
        goto done;
    }
    if (dsd_exitflag_load()) {
        result = DSD_APP_KEY_CANCELLED;
        goto done;
    }
    if (!p.fields) {
        result = DSD_APP_KEY_APPLIED;
        goto done;
    }
    result = validate_live_target(&p, state);
    if (result != DSD_APP_KEY_APPLIED) {
        goto done;
    }
    result = prepare_keys(&p, &keys);
    if (result != DSD_APP_KEY_APPLIED) {
        goto done;
    }
    if (prepare_map(&p, &map)) {
        result = DSD_APP_KEY_FILE_ERROR;
        goto done;
    }
    if (dsd_exitflag_load()) {
        result = DSD_APP_KEY_CANCELLED;
        goto done;
    }
    if (p.scope == DSD_APP_KEY_SCOPE_DEFAULTS) {
        result = apply_defaults(opts, state, &p, &keys, &map);
    } else {
        result = apply_target(opts, state, &p, &keys, &map);
    }
done:
    dsd_key_set_free(&keys);
    DSD_SECURE_ZERO(&p, sizeof(p));
    return result;
}
