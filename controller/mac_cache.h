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

#ifndef OVN_MAC_CACHE_H
#define OVN_MAC_CACHE_H

#include <stdint.h>

#include "openvswitch/hmap.h"
#include "ovn-sb-idl.h"
#include "openvswitch/ofp-flow.h"

struct mac_cache_data {
    /* 'struct threshold_node' by datapath UUID for MAC bindings. */
    struct hmap mb_thresholds;
    /* 'struct mac_binding_node' by 'mac_binding_data' that are
     * local and have threshold > 0. */
    struct hmap mac_bindings;
};

struct mac_cache_threshold {
    struct hmap_node hmap_node;
    /* Datapath UUID. */
    struct uuid uuid;
    /* Aging threshold in ms. */
    uint64_t value;
};

struct mac_cache_mb_data {
    uint32_t port_key;
    uint32_t dp_key;
    struct in6_addr ip;
    struct eth_addr mac;
};

struct mac_cache_mac_binding {
    struct hmap_node hmap_node;
    /* Common data to identify MAC binding. */
    struct mac_cache_mb_data data;
    /* Reference to the SB MAC binding record. */
    const struct sbrec_mac_binding *sbrec_mb;
};

bool mac_cache_threshold_add(struct mac_cache_data *data,
                             const struct sbrec_datapath_binding *dp);
void mac_cache_thresholds_destroy(struct mac_cache_data *data);
bool mac_cache_threshold_replace(struct mac_cache_data *data,
                                 const struct sbrec_datapath_binding *dp);
void mac_cache_mac_binding_add(struct mac_cache_data *data,
                                const struct sbrec_mac_binding *mb,
                                struct ovsdb_idl_index *sbrec_pb_by_name);
struct mac_cache_mac_binding *
mac_cachce_mac_binding_find(struct mac_cache_data *data,
                            const struct sbrec_mac_binding *mb,
                            struct ovsdb_idl_index *sbrec_pb_by_name);
void mac_cache_mac_binding_remove(struct mac_cache_data *data,
                                  const struct sbrec_mac_binding *mb,
                                  struct ovsdb_idl_index *sbrec_pb_by_name);
void mac_cache_mac_bindings_destroy(struct mac_cache_data *data);
bool mac_cache_sb_mac_binding_updated(const struct sbrec_mac_binding *mb);

void
mac_cache_mb_stats_process_flow_stats(struct ovs_list *stats_list,
                                      struct ofputil_flow_stats *ofp_stats);
void mac_cache_mb_stats_destroy(struct ovs_list *stats_list);
void mac_cache_mb_stats_run(struct ovs_list *stats_list, uint64_t *req_delay,
                            void *data);

#endif /* controller/mac_cache.h */
