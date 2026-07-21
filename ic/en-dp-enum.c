/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "en-dp-enum.h"
#include "lib/inc-proc-eng.h"
#include "lib/ovn-ic-nb-idl.h"
#include "lib/ovn-ic-sb-idl.h"
#include "lib/ovn-util.h"
#include "openvswitch/hmap.h"
#include "openvswitch/shash.h"
#include "openvswitch/vlog.h"
#include "ovn-ic.h"
#include "ovsdb-idl.h"
#include "uuid.h"

VLOG_DEFINE_THIS_MODULE(en_ic_dp_enum);

static void
enumerate_datapaths(struct ic_context *ctx, struct hmap *dp_tnlids,
                    struct shash *isb_ts_dps, struct shash *isb_tr_dps)
{
    const struct icsbrec_datapath_binding *isb_dp;
    ICSBREC_DATAPATH_BINDING_FOR_EACH (isb_dp, ctx->ovnisb_idl) {
        ovn_add_tnlid(dp_tnlids, isb_dp->tunnel_key);

        enum ic_datapath_type dp_type = ic_dp_get_type(isb_dp);
        if (dp_type == IC_ROUTER) {
            char *uuid_str = uuid_to_string(isb_dp->nb_ic_uuid);
            shash_add(isb_tr_dps, uuid_str, isb_dp);
            free(uuid_str);
        } else {
            shash_add(isb_ts_dps, isb_dp->transit_switch, isb_dp);
        }
    }
}


enum engine_node_state
en_dp_enum_run(struct engine_node *node OVS_UNUSED, void *data_)
{
    const struct engine_context *eng_ctx = engine_get_context();
    struct ic_context *ctx = eng_ctx->client_ctx;
    struct ed_type_dp_enum *data = data_;

    /* Clear the previous iteration's state and rebuild from the current IC-SB
     * datapath bindings. */
    ovn_destroy_tnlids(&data->dp_tnlids);
    hmap_init(&data->dp_tnlids);
    shash_clear(&data->isb_ts_dps);
    shash_clear(&data->isb_tr_dps);

    enumerate_datapaths(ctx, &data->dp_tnlids, &data->isb_ts_dps,
                        &data->isb_tr_dps);

    return EN_UPDATED;
}

/* Adds 'isb_dp_tracked' to the datapath maps and the tunnel-key allocator.
 *
 * 'isb_dp_tracked' is a tracked change from the *unlocked* IC-SB IDL: that IDL
 * is the passive observer that sees every change, including ovn-ic's own
 * locked-transaction writes (which the locked IDL does not re-track), so it is
 * the right source for change detection.  But the maps must hold rows from the
 * *locked* IDL, because enumerate_datapaths() (the recompute path) builds them
 * from ctx->ovnisb_idl and ts_sync_scope() deletes datapath bindings via the
 * locked transaction; a mix of locked and unlocked row objects breaks
 * dp_enum_remove()'s identity check and left a dangling map entry that
 * ts_sync_scope()'s GC double-deleted.  Bridge the two IDLs by the stable
 * _uuid: store the locked-IDL row for the same DB row.
 *
 * The maps are keyed by transit-switch name / IC-NB uuid.  Duplicate IC-SB
 * Datapath_Bindings for the same key can appear transiently (e.g. leader
 * races during instability), so use shash_replace() rather than shash_add():
 * this keeps at most one node per key, so the map never holds more than one
 * pointer for a key. */
static bool
dp_enum_add(struct ed_type_dp_enum *data, struct ic_context *ctx,
            const struct icsbrec_datapath_binding *isb_dp_tracked)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

    const struct icsbrec_datapath_binding *isb_dp =
        icsbrec_datapath_binding_get_for_uuid(ctx->ovnisb_idl,
                                              &isb_dp_tracked->header_.uuid);
    if (!isb_dp) {
        /* The locked IDL has not caught up with this row yet (e.g. its
         * connection is mid-reconnect, or IC-SB is transiently paused).  We
         * cannot store a consistent locked-IDL row, so signal the caller to
         * fall back to a full recompute rather than leave the map missing this
         * entry (which would desync en_ts/en_tr and duplicate NB ports). */
        return false;
    }

    ovn_add_tnlid(&data->dp_tnlids, isb_dp->tunnel_key);

    if (ic_dp_get_type(isb_dp) == IC_ROUTER) {
        char *uuid_str = uuid_to_string(isb_dp->nb_ic_uuid);
        if (shash_replace(&data->isb_tr_dps, uuid_str, isb_dp)) {
            VLOG_WARN_RL(&rl, "duplicate IC-SB transit-router datapath "
                         "binding for %s", uuid_str);
        }
        free(uuid_str);
    } else {
        if (shash_replace(&data->isb_ts_dps, isb_dp->transit_switch, isb_dp)) {
            VLOG_WARN_RL(&rl, "duplicate IC-SB transit-switch datapath "
                         "binding for %s", isb_dp->transit_switch);
        }
    }
    return true;
}

/* Removes the deleted 'isb_dp_tracked' (a tracked change from the unlocked
 * IC-SB IDL) from the datapath maps and the tunnel-key allocator.
 *
 * The maps store locked-IDL rows (see dp_enum_add()), so match by the stable
 * _uuid rather than the pointer: remove the map node only when the stored
 * (locked) row is the SAME DB row as the deleted (tracked) one.  With
 * duplicate bindings the node may already have been replaced by dp_enum_add()
 * with a different (live) row, so removing by name alone could drop that live
 * entry; the uuid guard keeps the map consistent and never dangling. */
static void
dp_enum_remove(struct ed_type_dp_enum *data,
               const struct icsbrec_datapath_binding *isb_dp_tracked)
{
    ovn_free_tnlid(&data->dp_tnlids, isb_dp_tracked->tunnel_key);

    struct shash *map;
    char *uuid_str = NULL;
    const char *key;
    if (ic_dp_get_type(isb_dp_tracked) == IC_ROUTER) {
        uuid_str = uuid_to_string(isb_dp_tracked->nb_ic_uuid);
        map = &data->isb_tr_dps;
        key = uuid_str;
    } else {
        map = &data->isb_ts_dps;
        key = isb_dp_tracked->transit_switch;
    }

    struct shash_node *node = shash_find(map, key);
    if (node) {
        const struct icsbrec_datapath_binding *stored = node->data;
        if (uuid_equals(&stored->header_.uuid,
                        &isb_dp_tracked->header_.uuid)) {
            shash_delete(map, node);
        }
    }
    free(uuid_str);
}

/* Incrementally maintains the datapath maps from tracked IC-SB
 * Datapath_Binding changes.  Inserts and deletes are handled in place.  A
 * modification that touches the tunnel key or either map key cannot be fixed
 * up incrementally (the tracked row only exposes the new values, so the old
 * key needed to remove the stale entry is unavailable); in that case we fall
 * back to a full recompute. */
enum engine_input_handler_result
en_dp_enum_icsb_datapath_binding_handler(struct engine_node *node, void *data_)
{
    struct ed_type_dp_enum *data = data_;
    struct ic_context *ctx = engine_get_context()->client_ctx;
    const struct icsbrec_datapath_binding_table *tbl =
        EN_OVSDB_GET(engine_get_input("ICSB_datapath_binding", node));
    const struct icsbrec_datapath_binding *isb_dp;
    bool updated = false;

    ICSBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (isb_dp, tbl) {
        if (icsbrec_datapath_binding_is_deleted(isb_dp)) {
            dp_enum_remove(data, isb_dp);
            updated = true;
        } else if (icsbrec_datapath_binding_is_new(isb_dp)) {
            if (!dp_enum_add(data, ctx, isb_dp)) {
                return EN_UNHANDLED;
            }
            updated = true;
        } else if (ovsdb_idl_track_is_updated(&isb_dp->header_,
                       &icsbrec_datapath_binding_col_tunnel_key) ||
                   ovsdb_idl_track_is_updated(&isb_dp->header_,
                       &icsbrec_datapath_binding_col_transit_switch) ||
                   ovsdb_idl_track_is_updated(&isb_dp->header_,
                       &icsbrec_datapath_binding_col_nb_ic_uuid)) {
            return EN_UNHANDLED;
        }
    }

    return updated ? EN_HANDLED_UPDATED : EN_HANDLED_UNCHANGED;
}

/* Shared change handler for the IC-NB Global input of nodes that only care
 * about its 'options' (e.g. vxlan_mode), not the nb_ic_cfg/sb_ic_cfg sequence
 * numbers that the sequence-number protocol bumps on every change.  Returns
 * EN_UNHANDLED (forcing a recompute) only when the options change or the row
 * is created/deleted; sequence-number-only updates are a no-op. */
enum engine_input_handler_result
en_ic_nb_global_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    const struct icnbrec_ic_nb_global_table *tbl =
        EN_OVSDB_GET(engine_get_input("ICNB_ic_nb_global", node));
    const struct icnbrec_ic_nb_global *icnb;

    ICNBREC_IC_NB_GLOBAL_TABLE_FOR_EACH_TRACKED (icnb, tbl) {
        if (icnbrec_ic_nb_global_is_new(icnb) ||
            icnbrec_ic_nb_global_is_deleted(icnb) ||
            ovsdb_idl_track_is_updated(&icnb->header_,
                                       &icnbrec_ic_nb_global_col_options)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

void
dp_enum_shash_clone(struct shash *dst, const struct shash *src)
{
    const struct shash_node *node;

    shash_init(dst);
    SHASH_FOR_EACH (node, src) {
        shash_add(dst, node->name, node->data);
    }
}

void *
en_dp_enum_init(struct engine_node *node OVS_UNUSED,
                struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_dp_enum *data = xzalloc(sizeof *data);

    hmap_init(&data->dp_tnlids);
    shash_init(&data->isb_ts_dps);
    shash_init(&data->isb_tr_dps);

    return data;
}

void
en_dp_enum_cleanup(void *data_)
{
    struct ed_type_dp_enum *data = data_;

    ovn_destroy_tnlids(&data->dp_tnlids);
    shash_destroy(&data->isb_ts_dps);
    shash_destroy(&data->isb_tr_dps);
}
