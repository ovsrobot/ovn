/* Copyright (c) 2023, Red Hat, Inc.
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
#include <stdbool.h>

#include "lport.h"
#include "mac_cache.h"
#include "openvswitch/hmap.h"
#include "openvswitch/vlog.h"
#include "ovn/logical-fields.h"
#include "ovn-sb-idl.h"

VLOG_DEFINE_THIS_MODULE(mac_cache);

static uint32_t
mac_cache_mb_data_hash(const struct mac_cache_mb_data *mb_data);
static inline bool
mac_cachce_mb_data_equals(const struct mac_cache_mb_data *a,
                          const struct mac_cache_mb_data *b);
static struct mac_cache_mac_binding *
mac_cache_mac_binding_find_by_mb_data(struct mac_cache_data *data,
                                      const struct mac_cache_mb_data *mb_data);
static bool
mac_cache_mb_data_from_sbrec(struct mac_cache_mb_data *data,
                              const struct sbrec_mac_binding *mb,
                              struct ovsdb_idl_index *sbrec_pb_by_name);

bool
mac_cache_threshold_add(struct mac_cache_data *data,
                        const struct sbrec_datapath_binding *dp)
{
    struct mac_cache_threshold *threshold =
            mac_cache_threshold_find(data, &dp->header_.uuid);
    if (threshold) {
        return true;
    }

    uint64_t mb_threshold = smap_get_uint(&dp->external_ids,
                                          "mac_binding_age_threshold", 0);
    if (!mb_threshold) {
        return false;
    }

    threshold = xmalloc(sizeof *threshold);
    threshold->uuid = dp->header_.uuid;
    threshold->value = mb_threshold * 1000;

    hmap_insert(&data->mb_thresholds, &threshold->hmap_node,
                uuid_hash(&dp->header_.uuid));

    return true;
}

struct mac_cache_threshold *
mac_cache_threshold_find(struct mac_cache_data *data, const struct uuid *uuid)
{
    uint32_t hash = uuid_hash(uuid);

    struct mac_cache_threshold *threshold;
    HMAP_FOR_EACH_WITH_HASH (threshold, hmap_node, hash,
                             &data->mb_thresholds) {
        if (uuid_equals(&threshold->uuid, uuid)) {
            return threshold;
        }
    }

    return NULL;
}

void
mac_cache_threshold_remove(struct mac_cache_data *data,
                           struct mac_cache_threshold *threshold)
{
    hmap_remove(&data->mb_thresholds, &threshold->hmap_node);
    free(threshold);
}

void
mac_cache_thresholds_destroy(struct mac_cache_data *data)
{
    struct mac_cache_threshold *threshold;
    HMAP_FOR_EACH_POP (threshold, hmap_node, &data->mb_thresholds) {
        free(threshold);
    }
}

void
mac_cachce_mac_binding_add(struct mac_cache_data *data,
                           const struct sbrec_mac_binding *mb,
                           struct ovsdb_idl_index *sbrec_pb_by_name)
{
    struct mac_cache_mb_data mb_data;
    if (!mac_cache_mb_data_from_sbrec(&mb_data, mb, sbrec_pb_by_name)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "Couldn't parse MAC binding: ip=%s, mac=%s, "
                     "logical_port=%s", mb->ip, mb->mac, mb->logical_port);
        return;
    }

    struct mac_cache_mac_binding *mc_mb =
            mac_cache_mac_binding_find_by_mb_data(data, &mb_data);

    if (!mc_mb) {
        mc_mb = xmalloc(sizeof *mc_mb);
        hmap_insert(&data->mac_bindings, &mc_mb->hmap_node,
                    mac_cache_mb_data_hash(&mb_data));
    }

    mc_mb->sbrec_mb = mb;
    mc_mb->data = mb_data;
}

void
mac_cache_mac_binding_remove(struct mac_cache_data *data,
                             const struct sbrec_mac_binding *mb,
                             struct ovsdb_idl_index *sbrec_pb_by_name)
{
    struct mac_cache_mb_data mb_data;
    if (!mac_cache_mb_data_from_sbrec(&mb_data, mb, sbrec_pb_by_name)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "Couldn't parse MAC binding: ip=%s, mac=%s, "
                     "logical_port=%s", mb->ip, mb->mac, mb->logical_port);
        return;
    }

    struct mac_cache_mac_binding *mc_mb =
            mac_cache_mac_binding_find_by_mb_data(data, &mb_data);
    if (!mc_mb) {
        return;
    }

    hmap_remove(&data->mac_bindings, &mc_mb->hmap_node);
    free(mc_mb);
}

bool
mac_cache_sb_mac_binding_updated(const struct sbrec_mac_binding *mb)
{
    bool updated = false;
    for (size_t i = 0; i < SBREC_MAC_BINDING_N_COLUMNS; i++) {
        /* Ignore timestamp update as this does not affect the existing nodes
         * at all. */
        if (i == SBREC_MAC_BINDING_COL_TIMESTAMP) {
            continue;
        }
        updated |= sbrec_mac_binding_is_updated(mb, i);
    }

    return updated || sbrec_mac_binding_is_deleted(mb);
}

void
mac_cache_mac_bindings_destroy(struct mac_cache_data *data)
{
    struct mac_cache_mac_binding *mc_mb;
    HMAP_FOR_EACH_POP (mc_mb, hmap_node, &data->mac_bindings) {
        free(mc_mb);
    }
}

static uint32_t
mac_cache_mb_data_hash(const struct mac_cache_mb_data *mb_data)
{
    uint32_t hash = 0;

    hash = hash_add(hash, mb_data->port_key);
    hash = hash_add(hash, mb_data->dp_key);
    hash = hash_add_in6_addr(hash, &mb_data->ip);
    hash = hash_add64(hash, eth_addr_to_uint64(mb_data->mac));

    return hash_finish(hash, 32);
}

static inline bool
mac_cachce_mb_data_equals(const struct mac_cache_mb_data *a,
                          const struct mac_cache_mb_data *b)
{
    return a->port_key == b->port_key &&
           a->dp_key == b->dp_key &&
           ipv6_addr_equals(&a->ip, &b->ip) &&
           eth_addr_equals(a->mac, b->mac);
}

static bool
mac_cache_mb_data_from_sbrec(struct mac_cache_mb_data *data,
                              const struct sbrec_mac_binding *mb,
                              struct ovsdb_idl_index *sbrec_pb_by_name)
{
    const struct sbrec_port_binding *pb =
            lport_lookup_by_name(sbrec_pb_by_name, mb->logical_port);

    if (!pb || !pb->datapath) {
        return false;
    }

    if (!ip46_parse(mb->ip, &data->ip)) {
        return false;
    }

    if (!eth_addr_from_string(mb->mac, &data->mac)) {
        return false;
    }

    data->dp_key = mb->datapath->tunnel_key;
    data->port_key = pb->tunnel_key;

    return true;
}

static struct mac_cache_mac_binding *
mac_cache_mac_binding_find_by_mb_data(struct mac_cache_data *data,
                                      const struct mac_cache_mb_data *mb_data)
{
    uint32_t hash = mac_cache_mb_data_hash(mb_data);

    struct mac_cache_mac_binding *mb;
    HMAP_FOR_EACH_WITH_HASH (mb, hmap_node, hash, &data->mac_bindings) {
        if (mac_cachce_mb_data_equals(&mb->data, mb_data)) {
            return mb;
        }
    }

    return NULL;
}
