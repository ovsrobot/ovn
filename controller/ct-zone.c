/* Copyright (c) 2024, Red Hat, Inc.
 *
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

#include "ct-zone.h"
#include "local_data.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(ct_zone);

static void
ct_zone_restore(const struct sbrec_datapath_binding_table *dp_table,
                struct ct_zone_ctx *ctx, const char *name, int zone);
static void ct_zone_add_pending(struct shash *pending_ct_zones,
                                enum ct_zone_pending_state state,
                                struct ct_zone *zone, bool add,
                                const char *name);
static int ct_zone_get_snat(const struct sbrec_datapath_binding *dp);
static bool ct_zone_assign_unused(struct ct_zone_ctx *ctx,
                                  const char *zone_name, int *scan_start);
static bool ct_zone_remove(struct ct_zone_ctx *ctx, const char *name);
static void ct_zone_add(struct ct_zone_ctx *ctx, const char *name,
                        uint16_t zone, bool set_pending);

void
ct_zones_restore(struct ct_zone_ctx *ctx,
                 const struct ovsrec_open_vswitch_table *ovs_table,
                 const struct sbrec_datapath_binding_table *dp_table,
                 const struct ovsrec_bridge *br_int)
{
    memset(ctx->bitmap, 0, sizeof ctx->bitmap);
    bitmap_set1(ctx->bitmap, 0); /* Zone 0 is reserved. */

    struct shash_node *pending_node;
    SHASH_FOR_EACH (pending_node, &ctx->pending) {
        struct ct_zone_pending_entry *ctpe = pending_node->data;

        if (ctpe->add) {
            ct_zone_restore(dp_table, ctx, pending_node->name,
                            ctpe->ct_zone.zone);
        }
    }

    const struct ovsrec_open_vswitch *cfg;
    cfg = ovsrec_open_vswitch_table_first(ovs_table);
    if (!cfg) {
        return;
    }

    if (!br_int) {
        /* If the integration bridge hasn't been defined, assume that
         * any existing ct-zone definitions aren't valid. */
        return;
    }

    struct smap_node *node;
    SMAP_FOR_EACH (node, &br_int->external_ids) {
        if (strncmp(node->key, "ct-zone-", 8)) {
            continue;
        }

        const char *user = node->key + 8;
        if (!user[0]) {
            continue;
        }

        if (shash_find(&ctx->pending, user)) {
            continue;
        }

        unsigned int zone;
        if (!str_to_uint(node->value, 10, &zone)) {
            continue;
        }

        ct_zone_restore(dp_table, ctx, user, zone);
    }
}

void
ct_zones_update(const struct sset *local_lports,
                const struct hmap *local_datapaths, struct ct_zone_ctx *ctx)
{
    int scan_start = 1;
    const char *user;
    struct sset all_users = SSET_INITIALIZER(&all_users);
    struct simap req_snat_zones = SIMAP_INITIALIZER(&req_snat_zones);
    unsigned long *unreq_snat_zones_map = bitmap_allocate(MAX_CT_ZONES);
    struct simap unreq_snat_zones = SIMAP_INITIALIZER(&unreq_snat_zones);

    const char *local_lport;
    SSET_FOR_EACH (local_lport, local_lports) {
        sset_add(&all_users, local_lport);
    }

    /* Local patched datapath (gateway routers) need zones assigned. */
    const struct local_datapath *ld;
    HMAP_FOR_EACH (ld, hmap_node, local_datapaths) {
        /* XXX Add method to limit zone assignment to logical router
         * datapaths with NAT */
        const char *name = smap_get(&ld->datapath->external_ids, "name");
        if (!name) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_ERR_RL(&rl, "Missing name for datapath '"UUID_FMT"' "
                        "skipping zone assignment.",
                        UUID_ARGS(&ld->datapath->header_.uuid));
            continue;
        }

        char *dnat = alloc_nat_zone_key(name, "dnat");
        char *snat = alloc_nat_zone_key(name, "snat");
        sset_add(&all_users, dnat);
        sset_add(&all_users, snat);

        int req_snat_zone = ct_zone_get_snat(ld->datapath);
        if (req_snat_zone >= 0) {
            simap_put(&req_snat_zones, snat, req_snat_zone);
        }
        free(dnat);
        free(snat);
    }

    /* Delete zones that do not exist in above sset. */
    struct shash_node *node;
    SHASH_FOR_EACH_SAFE (node, &ctx->current) {
        struct ct_zone *ct_zone = node->data;
        if (!sset_contains(&all_users, node->name)) {
            ct_zone_remove(ctx, node->name);
        } else if (!simap_find(&req_snat_zones, node->name)) {
            bitmap_set1(unreq_snat_zones_map, ct_zone->zone);
            simap_put(&unreq_snat_zones, node->name, ct_zone->zone);
        }
    }

    /* Prioritize requested CT zones */
    struct simap_node *snat_req_node;
    SIMAP_FOR_EACH (snat_req_node, &req_snat_zones) {
        /* Determine if someone already had this zone auto-assigned.
         * If so, then they need to give up their assignment since
         * that zone is being explicitly requested now.
         */
        if (bitmap_is_set(unreq_snat_zones_map, snat_req_node->data)) {
            struct simap_node *unreq_node;
            SIMAP_FOR_EACH_SAFE (unreq_node, &unreq_snat_zones) {
                if (unreq_node->data == snat_req_node->data) {
                    ct_zone_remove(ctx, unreq_node->name);
                    simap_delete(&unreq_snat_zones, unreq_node);
                }
            }

            /* Set this bit to 0 so that if multiple datapaths have requested
             * this zone, we don't needlessly double-detect this condition.
             */
            bitmap_set0(unreq_snat_zones_map, snat_req_node->data);
        }

        struct ct_zone *ct_zone = shash_find_data(&ctx->current,
                                                  snat_req_node->name);
        if (node && ct_zone->zone != snat_req_node->data) {
            ct_zone_remove(ctx, snat_req_node->name);
        }
        ct_zone_add(ctx, snat_req_node->name, snat_req_node->data, true);
    }

    /* xxx This is wasteful to assign a zone to each port--even if no
     * xxx security policy is applied. */

    /* Assign a unique zone id for each logical port and two zones
     * to a gateway router. */
    SSET_FOR_EACH (user, &all_users) {
        if (shash_find(&ctx->current, user)) {
            continue;
        }

        ct_zone_assign_unused(ctx, user, &scan_start);
    }

    simap_destroy(&req_snat_zones);
    simap_destroy(&unreq_snat_zones);
    sset_destroy(&all_users);
    bitmap_free(unreq_snat_zones_map);
}

void
ct_zones_commit(const struct ovsrec_bridge *br_int,
                struct shash *pending_ct_zones)
{
    struct shash_node *iter;
    SHASH_FOR_EACH (iter, pending_ct_zones) {
        struct ct_zone_pending_entry *ctzpe = iter->data;

        /* The transaction is open, so any pending entries in the
         * CT_ZONE_DB_QUEUED must be sent and any in CT_ZONE_DB_QUEUED
         * need to be retried. */
        if (ctzpe->state != CT_ZONE_DB_QUEUED
            && ctzpe->state != CT_ZONE_DB_SENT) {
            continue;
        }

        char *user_str = xasprintf("ct-zone-%s", iter->name);
        if (ctzpe->add) {
            char *zone_str = xasprintf("%"PRIu16, ctzpe->ct_zone.zone);
            struct smap_node *node =
                    smap_get_node(&br_int->external_ids, user_str);
            if (!node || strcmp(node->value, zone_str)) {
                ovsrec_bridge_update_external_ids_setkey(br_int,
                                                         user_str, zone_str);
            }
            free(zone_str);
        } else {
            if (smap_get(&br_int->external_ids, user_str)) {
                ovsrec_bridge_update_external_ids_delkey(br_int, user_str);
            }
        }
        free(user_str);

        ctzpe->state = CT_ZONE_DB_SENT;
    }
}

void
ct_zones_pending_clear_commited(struct shash *pending)
{
    struct shash_node *iter;
    SHASH_FOR_EACH_SAFE (iter, pending) {
        struct ct_zone_pending_entry *ctzpe = iter->data;
        if (ctzpe->state == CT_ZONE_DB_SENT) {
            shash_delete(pending, iter);
            free(ctzpe);
        }
    }
}

/* Returns "true" when there is no need for full recompute. */
bool
ct_zone_handle_dp_update(struct ct_zone_ctx *ctx,
                         const struct sbrec_datapath_binding *dp)
{
    int req_snat_zone = ct_zone_get_snat(dp);
    if (req_snat_zone == -1) {
        /* datapath snat ct zone is not set.  This condition will also hit
         * when CMS clears the snat-ct-zone for the logical router.
         * In this case there is no harm in using the previosly specified
         * snat ct zone for this datapath.  Also it is hard to know
         * if this option was cleared or if this option is never set. */
        return true;
    }

    const char *name = smap_get(&dp->external_ids, "name");
    if (!name) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_ERR_RL(&rl, "Missing name for datapath '"UUID_FMT"' skipping"
                    "zone check.", UUID_ARGS(&dp->header_.uuid));
        return true;
    }

    /* Check if the requested snat zone has changed for the datapath
     * or not.  If so, then fall back to full recompute of
     * ct_zone engine. */
    char *snat_dp_zone_key = alloc_nat_zone_key(name, "snat");
    struct shash_node *node = shash_find(&ctx->current, snat_dp_zone_key);
    free(snat_dp_zone_key);

    if (!node) {
        return false;
    }

    struct ct_zone *ct_zone = node->data;
    if (ct_zone->zone != req_snat_zone) {
        /* The requested snat zone has changed. Trigger full recompute of
         * ct_zones engine. */
        return false;
    }

    return true;
}

/* Returns "true" if there was an update to the context. */
bool
ct_zone_handle_port_update(struct ct_zone_ctx *ctx, const char *name,
                           bool updated, int *scan_start)
{
    struct shash_node *node = shash_find(&ctx->current, name);
    if (updated && !node) {
        ct_zone_assign_unused(ctx, name, scan_start);
        return true;
    } else if (!updated && node && ct_zone_remove(ctx, node->name)) {
        return true;
    }

    return false;
}

uint16_t
ct_zone_find_zone(const struct shash *ct_zones, const char *name)
{
    struct ct_zone *ct_zone = shash_find_data(ct_zones, name);
    return ct_zone ? ct_zone->zone : 0;
}


static bool
ct_zone_assign_unused(struct ct_zone_ctx *ctx, const char *zone_name,
                      int *scan_start)
{
    /* We assume that there are 64K zones and that we own them all. */
    int zone = bitmap_scan(ctx->bitmap, 0, *scan_start, MAX_CT_ZONES + 1);
    if (zone == MAX_CT_ZONES + 1) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "exhausted all ct zones");
        return false;
    }

    *scan_start = zone + 1;
    ct_zone_add(ctx, zone_name, zone, true);

    return true;
}

static bool
ct_zone_remove(struct ct_zone_ctx *ctx, const char *name)
{
    struct shash_node *node = shash_find(&ctx->current, name);
    if (!node) {
        return false;
    }

    struct ct_zone *ct_zone = node->data;

    VLOG_DBG("removing ct zone %"PRIu16" for '%s'", ct_zone->zone, name);

    ct_zone_add_pending(&ctx->pending, CT_ZONE_OF_QUEUED,
                        ct_zone, false, name);
    bitmap_set0(ctx->bitmap, ct_zone->zone);
    shash_delete(&ctx->current, node);
    free(ct_zone);

    return true;
}

static void
ct_zone_add(struct ct_zone_ctx *ctx, const char *name, uint16_t zone,
            bool set_pending)
{
    VLOG_DBG("assigning ct zone %"PRIu16" for '%s'", zone, name);

    struct ct_zone *ct_zone = shash_find_data(&ctx->current, name);
    if (!ct_zone) {
        ct_zone = xmalloc(sizeof *ct_zone);
        shash_add(&ctx->current, name, ct_zone);
    }

    ct_zone->zone = zone;

    if (set_pending) {
        ct_zone_add_pending(&ctx->pending, CT_ZONE_OF_QUEUED,
                            ct_zone, true, name);
    }
    bitmap_set1(ctx->bitmap, zone);
}

static int
ct_zone_get_snat(const struct sbrec_datapath_binding *dp)
{
    return smap_get_int(&dp->external_ids, "snat-ct-zone", -1);
}

static void
ct_zone_add_pending(struct shash *pending_ct_zones,
                    enum ct_zone_pending_state state,
                    struct ct_zone *zone, bool add, const char *name)
{
    /* Its important that we add only one entry for the key 'name'.
     * Replace 'pending' with 'existing' and free up 'existing'.
     * Otherwise, we may end up in a continuous loop of adding
     * and deleting the zone entry in the 'external_ids' of
     * integration bridge.
     */
    struct ct_zone_pending_entry *entry =
            shash_find_data(pending_ct_zones, name);

    if (!entry) {
        entry = xmalloc(sizeof *entry);
        entry->state = CT_ZONE_STATE_NEW;

        shash_add(pending_ct_zones, name, entry);
    }

    *entry = (struct ct_zone_pending_entry) {
        .ct_zone = *zone,
        .state = MIN(entry->state, state),
        .add = add,
    };
}

/* Replaces a UUID prefix from 'uuid_zone' (if any) with the
 * corresponding Datapath_Binding.external_ids.name.  Returns it
 * as a new string that will be owned by the caller. */
static char *
ct_zone_name_from_uuid(const struct sbrec_datapath_binding_table *dp_table,
                       const char *uuid_zone)
{
    struct uuid uuid;
    if (!uuid_from_string_prefix(&uuid, uuid_zone)) {
        return NULL;
    }

    const struct sbrec_datapath_binding *dp =
            sbrec_datapath_binding_table_get_for_uuid(dp_table, &uuid);
    if (!dp) {
        return NULL;
    }

    const char *entity_name = smap_get(&dp->external_ids, "name");
    if (!entity_name) {
        return NULL;
    }

    return xasprintf("%s%s", entity_name, uuid_zone + UUID_LEN);
}

static void
ct_zone_restore(const struct sbrec_datapath_binding_table *dp_table,
                struct ct_zone_ctx *ctx, const char *name, int zone)
{
    VLOG_DBG("restoring ct zone %"PRId32" for '%s'", zone, name);

    char *new_name = ct_zone_name_from_uuid(dp_table, name);
    const char *current_name = name;
    if (new_name) {
        VLOG_DBG("ct zone %"PRId32" replace uuid name '%s' with '%s'",
                 zone, name, new_name);

        struct ct_zone ct_zone = {
            .zone = zone,
        };
        /* Make sure we remove the uuid one in the next OvS DB commit without
         * flush. */
        ct_zone_add_pending(&ctx->pending, CT_ZONE_DB_QUEUED,
                            &ct_zone, false, name);
        /* Store the zone in OvS DB with name instead of uuid without flush.
         * */
        ct_zone_add_pending(&ctx->pending, CT_ZONE_DB_QUEUED,
                            &ct_zone, true, new_name);
        current_name = new_name;
    }

    ct_zone_add(ctx, current_name, zone, false);
    free(new_name);
}
