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
#include "en-tr.h"
#include "lib/inc-proc-eng.h"
#include "lib/ovn-ic-nb-idl.h"
#include "lib/ovn-ic-sb-idl.h"
#include "lib/ovn-nb-idl.h"
#include "openvswitch/shash.h"
#include "openvswitch/vlog.h"
#include "ovn-ic.h"
#include "ovsdb-idl.h"
#include "smap.h"

VLOG_DEFINE_THIS_MODULE(en_ic_tr);

static void
tr_run(struct ic_context *ctx, struct hmap *dp_tnlids,
       struct shash *isb_tr_dps)
{
    /*
     * Warning: ovnisb_unlocked should not be used to insert data on IC_SB
     * which can cause a constraint violation, as an example, inserting data to
     * IC-SB datapath_binding.
     */
    const struct nbrec_logical_router *lr;

    if (ctx->ovnnb_txn) {
        struct shash nb_tres = SHASH_INITIALIZER(&nb_tres);
        NBREC_LOGICAL_ROUTER_FOR_EACH (lr, ctx->ovnnb_idl) {
            const char *tr_name = smap_get(&lr->options, "interconn-tr");
            if (tr_name) {
                shash_add(&nb_tres, tr_name, lr);
            }
        }

        const struct icnbrec_transit_router *tr;
        ICNBREC_TRANSIT_ROUTER_FOR_EACH (tr, ctx->ovninb_idl) {
            lr = shash_find_and_delete(&nb_tres, tr->name);
            if (!lr) {
                lr = nbrec_logical_router_insert(ctx->ovnnb_txn);
                nbrec_logical_router_set_name(lr, tr->name);
                nbrec_logical_router_update_options_setkey(
                    lr, "interconn-tr", tr->name);
            }
            char *uuid_str = uuid_to_string(&tr->header_.uuid);
            struct icsbrec_datapath_binding *isb_dp = shash_find_data(
                isb_tr_dps, uuid_str);
            free(uuid_str);

            if (isb_dp) {
                char *tnl_key_str = xasprintf("%"PRId64, isb_dp->tunnel_key);
                nbrec_logical_router_update_options_setkey(
                    lr, "requested-tnl-key", tnl_key_str);
                free(tnl_key_str);
            }
        }

        struct shash_node *node;
        SHASH_FOR_EACH (node, &nb_tres) {
            nbrec_logical_router_delete(node->data);
        }
        shash_destroy(&nb_tres);
    }

    /* Sync TR between INB and ISB.  This is performed after syncing with AZ
     * SB, to avoid uncommitted ISB datapath tunnel key to be synced back to
     * AZ. */
    if (ctx->ovnisb_txn &&
        is_az_leader(ctx->ovnisb_txn)) {
        /* Create ISB Datapath_Binding */
        const struct icnbrec_transit_router *tr;
        ICNBREC_TRANSIT_ROUTER_FOR_EACH (tr, ctx->ovninb_idl) {
            char *uuid_str = uuid_to_string(&tr->header_.uuid);
            struct icsbrec_datapath_binding *isb_dp =
                shash_find_and_delete(isb_tr_dps, uuid_str);
            free(uuid_str);

            if (!isb_dp) {
                int dp_key = allocate_dp_key(dp_tnlids, false,
                                             "transit router datapath");
                if (!dp_key) {
                    continue;
                }

                isb_dp = icsbrec_datapath_binding_insert(ctx->ovnisb_txn);
                icsbrec_datapath_binding_set_tunnel_key(isb_dp, dp_key);
                icsbrec_datapath_binding_set_nb_ic_uuid(isb_dp,
                                                        &tr->header_.uuid, 1);
                icsbrec_datapath_binding_set_type(isb_dp, "transit-router");
            }
        }

        struct shash_node *node;
        SHASH_FOR_EACH (node, isb_tr_dps) {
            icsbrec_datapath_binding_delete(node->data);
        }
    }
}


enum engine_node_state
en_tr_run(struct engine_node *node, void *data OVS_UNUSED)
{
    const struct engine_context *eng_ctx = engine_get_context();
    struct ic_context *ctx = eng_ctx->client_ctx;
    struct ed_type_dp_enum *dp = engine_get_input_data("dp_enum", node);

    /* runned_az is resolved by the upstream en_az node.  Without an AZ there
     * is nothing to sync (mirrors the previous main-loop gating). */
    if (!ctx->runned_az) {
        return EN_UNCHANGED;
    }

    /* tr_run() destructively consumes the transit-router datapath shash, so
     * pass it a copy to keep en_dp_enum's authoritative map intact.  The
     * shared tunnel-key allocator (dp_tnlids) is passed directly. */
    struct shash isb_tr_dps;
    dp_enum_shash_clone(&isb_tr_dps, &dp->isb_tr_dps);
    tr_run(ctx, &dp->dp_tnlids, &isb_tr_dps);
    shash_destroy(&isb_tr_dps);

    return EN_UPDATED;
}

/* Only transit-router mirror logical routers (options:interconn-tr) affect
 * en_tr.  A change to such a logical router is left to a full recompute
 * (returns EN_UNHANDLED); any other logical router is irrelevant to en_tr, so
 * its change is handled as a no-op, avoiding a recompute on unrelated NB
 * Logical_Router updates. */
enum engine_input_handler_result
en_tr_nb_logical_router_handler(struct engine_node *node,
                                void *data OVS_UNUSED)
{
    const struct engine_context *eng_ctx = engine_get_context();
    struct ic_context *ctx = eng_ctx->client_ctx;

    if (!ctx->runned_az) {
        return EN_HANDLED_UNCHANGED;
    }

    const struct nbrec_logical_router_table *tbl =
        EN_OVSDB_GET(engine_get_input("NB_logical_router", node));
    const struct nbrec_logical_router *lr;
    NBREC_LOGICAL_ROUTER_TABLE_FOR_EACH_TRACKED (lr, tbl) {
        if (smap_get(&lr->options, "interconn-tr")) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

/* IC-SB Datapath_Binding: tr_run() reads only transit-router datapath
 * bindings (it mirrors their tunnel_key into the NB logical router as
 * requested-tnl-key, via en_dp_enum's isb_tr_dps map, and deletes orphans).
 * Transit-switch datapath bindings - the bulk of the churn here - do not
 * affect en_tr and are a no-op.  This is the trigger that the en_dp_enum edge
 * used to provide before it was reduced to an ordering-only (no-op) input: a
 * transit-router binding change still forces a recompute so the freshly
 * en_dp_enum-rebuilt isb_tr_dps is consumed.  A change to the type column
 * cannot be classified (the old type is gone), so recompute to be safe. */
enum engine_input_handler_result
en_tr_icsb_datapath_binding_handler(struct engine_node *node,
                                    void *data OVS_UNUSED)
{
    const struct engine_context *eng_ctx = engine_get_context();
    struct ic_context *ctx = eng_ctx->client_ctx;

    if (!ctx->runned_az) {
        return EN_HANDLED_UNCHANGED;
    }

    const struct icsbrec_datapath_binding_table *tbl =
        EN_OVSDB_GET(engine_get_input("ICSB_datapath_binding", node));
    const struct icsbrec_datapath_binding *isb_dp;
    ICSBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (isb_dp, tbl) {
        if (icsbrec_datapath_binding_is_new(isb_dp) ||
            icsbrec_datapath_binding_is_deleted(isb_dp)) {
            /* A new/deleted transit-router binding is the trigger en_tr needs;
             * a transit-switch binding (the common churn) is a no-op. Classify
             * by the row's own type, which is available on tracked deleted
             * rows too - unlike track_is_updated(), which reports every
             * column as updated on insert. */
            if (ic_dp_get_type(isb_dp) == IC_ROUTER) {
                return EN_UNHANDLED;
            }
        } else if (ic_dp_get_type(isb_dp) == IC_ROUTER ||
                   ovsdb_idl_track_is_updated(
                       &isb_dp->header_,
                       &icsbrec_datapath_binding_col_type)) {
            /* A modified transit-router binding (e.g. tunnel_key/nb_ic_uuid),
             * or a type change that can no longer be classified, forces a
             * recompute. */
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

void *
en_tr_init(struct engine_node *node OVS_UNUSED,
           struct engine_arg *arg OVS_UNUSED)
{
    return NULL;
}

void
en_tr_cleanup(void *data OVS_UNUSED)
{
}
