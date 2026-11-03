/* Copyright (c) 2025, Red Hat, Inc.
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
#include "lib/ovn-util.h"
#include "lib/simap.h"
#include "openvswitch/hmap.h"
#include "openvswitch/ofp-ct.h"
#include "openvswitch/rconn.h"
#include "openvswitch/vlog.h"
#include "ovn/logical-fields.h"
#include "ovn-sb-idl.h"
#include "controller/ecmp-next-hop-monitor.h"

VLOG_DEFINE_THIS_MODULE(ecmp_nexthop_monitor);

/* Next-hops of the SB ECMP_Nexthop table that are in use on local
 * datapaths. */
static struct hmap ecmp_nexthop;

/* Next-hops that went away but whose conntrack flush has not been sent to
 * the switch yet.  They stay here until ofctrl is able to send it. */
static struct hmap ecmp_nexthop_pending_flush;

struct ecmp_nexthop_data {
    struct hmap_node hmap_node;
    uint16_t zone_id;
    char *nexthop;
    char *mac;
};

void ecmp_nexthop_init(void)
{
    hmap_init(&ecmp_nexthop);
    hmap_init(&ecmp_nexthop_pending_flush);
}

static void
ecmp_nexthop_destroy_entry(struct ecmp_nexthop_data *e)
{
    free(e->nexthop);
    free(e->mac);
    free(e);
}

static void
ecmp_nexthop_destroy_map(struct hmap *map)
{
    struct ecmp_nexthop_data *e;
    HMAP_FOR_EACH_POP (e, hmap_node, map) {
        ecmp_nexthop_destroy_entry(e);
    }
    hmap_destroy(map);
}

void ecmp_nexthop_destroy(void)
{
    ecmp_nexthop_destroy_map(&ecmp_nexthop);
    ecmp_nexthop_destroy_map(&ecmp_nexthop_pending_flush);
}

static uint32_t
ecmp_nexthop_hash(const char *nexthop, const char *mac,
                  const uint16_t zone_id)
{
    uint32_t hash = hash_string(nexthop, 0);
    hash = hash_add(hash, hash_string(mac, 0));
    hash = hash_add(hash, zone_id);

    return hash_finish(hash, 64);
}

static struct ecmp_nexthop_data *
ecmp_nexthop_alloc_entry(const char *nexthop, const char *mac,
                         const uint16_t zone_id, struct hmap *map)
{
    struct ecmp_nexthop_data *e = xmalloc(sizeof *e);
    e->nexthop = xstrdup(nexthop);
    e->mac = xstrdup(mac);
    e->zone_id = zone_id;

    hmap_insert(map, &e->hmap_node, ecmp_nexthop_hash(nexthop, mac, zone_id));

    return e;
}

static struct ecmp_nexthop_data *
ecmp_nexthop_find_entry(const char *nexthop, const char *mac,
                        const uint16_t zone_id, const struct hmap *map)
{
    uint32_t hash = ecmp_nexthop_hash(nexthop, mac, zone_id);

    struct ecmp_nexthop_data *e;
    HMAP_FOR_EACH_WITH_HASH (e, hmap_node, hash, map) {
        if (!strcmp(e->nexthop, nexthop) &&
            !strcmp(e->mac, mac) && e->zone_id == zone_id) {
            return e;
        }
    }
    return NULL;
}

#define OVN_CT_ECMP_ETH_LOW     (((1ULL << OVN_CT_ECMP_ETH_1ST_BIT) - 1) << 32)
#define OVN_CT_ECMP_ETH_HIGH    ((1ULL << (OVN_CT_ECMP_ETH_END_BIT - 63)) - 1)

static void
ecmp_nexthop_monitor_flush_ct_entry(const struct rconn *swconn,
                                    const char *mac, uint16_t zone_id,
                                    struct ovs_list *msgs)
{
    struct eth_addr ea;
    if (!ovs_scan(mac, ETH_ADDR_SCAN_FMT, ETH_ADDR_SCAN_ARGS(ea))) {
        return;
    }

    ovs_u128 mask = {
        /* ct_label.ecmp_reply_eth BITS[32-79] */
        .u64.hi = OVN_CT_ECMP_ETH_HIGH,
        .u64.lo = OVN_CT_ECMP_ETH_LOW,
    };

    ovs_be32 lo = get_unaligned_be32((void *)&ea.be16[1]);
    ovs_u128 nexthop = {
        .u64.hi = ntohs(ea.be16[0]),
        .u64.lo = (uint64_t) ntohl(lo) << 32,
    };

    struct ofp_ct_match match = {
        .labels = nexthop,
        .labels_mask = mask,
    };
    struct ofpbuf *msg = ofp_ct_match_encode(&match, &zone_id,
                                             rconn_get_version(swconn));
    ovs_list_push_back(msgs, &msg->list_node);
}

/* Updates the monitored next-hops from 'enh_table'.  A monitored next-hop
 * that is no longer there is moved to the pending flush set; its conntrack
 * entries are flushed by ecmp_nexthop_monitor_flush(). */
void
ecmp_nexthop_monitor_run(const struct sbrec_ecmp_nexthop_table *enh_table,
                         const struct hmap *local_datapaths,
                         const struct shash *current_ct_zones)
{
    struct hmap sb_ecmp_nexthop = HMAP_INITIALIZER(&sb_ecmp_nexthop);
    const struct sbrec_ecmp_nexthop *sbrec_ecmp_nexthop;

    ovs_assert(local_datapaths);

    SBREC_ECMP_NEXTHOP_TABLE_FOR_EACH (sbrec_ecmp_nexthop, enh_table) {
        if (!strcmp(sbrec_ecmp_nexthop->mac, "")) {
            continue;
        }
        if (!get_local_datapath(local_datapaths,
                                sbrec_ecmp_nexthop->datapath->tunnel_key)) {
            continue;
        }
        const char *dp_name = smap_get(
                &sbrec_ecmp_nexthop->datapath->external_ids, "name");
        if (!dp_name) {
            continue;
        }

        char *name = alloc_nat_zone_key(dp_name, "dnat");
        struct ct_zone *ct_zone = shash_find_data(current_ct_zones, name);
        free(name);

        if (!ct_zone) {
            continue;
        }

        if (!ecmp_nexthop_find_entry(sbrec_ecmp_nexthop->nexthop,
                                     sbrec_ecmp_nexthop->mac, ct_zone->zone,
                                     &ecmp_nexthop)) {
            ecmp_nexthop_alloc_entry(sbrec_ecmp_nexthop->nexthop,
                                     sbrec_ecmp_nexthop->mac,
                                     ct_zone->zone, &ecmp_nexthop);
        }
        ecmp_nexthop_alloc_entry(sbrec_ecmp_nexthop->nexthop,
                                 sbrec_ecmp_nexthop->mac, ct_zone->zone,
                                 &sb_ecmp_nexthop);
    }

    struct ecmp_nexthop_data *e;
    HMAP_FOR_EACH_SAFE (e, hmap_node, &ecmp_nexthop) {
        if (!ecmp_nexthop_find_entry(e->nexthop, e->mac, e->zone_id,
                                     &sb_ecmp_nexthop)) {
            hmap_remove(&ecmp_nexthop, &e->hmap_node);
            if (ecmp_nexthop_find_entry(e->nexthop, e->mac, e->zone_id,
                                        &ecmp_nexthop_pending_flush)) {
                ecmp_nexthop_destroy_entry(e);
            } else {
                hmap_insert(&ecmp_nexthop_pending_flush, &e->hmap_node,
                            ecmp_nexthop_hash(e->nexthop, e->mac,
                                              e->zone_id));
            }
        }
    }
    ecmp_nexthop_destroy_map(&sb_ecmp_nexthop);
}

bool
ecmp_nexthop_monitor_has_pending_flush(void)
{
    return !hmap_is_empty(&ecmp_nexthop_pending_flush);
}

/* Appends to 'msgs' a conntrack flush for each pending next-hop and forgets
 * them.  The caller must make sure that 'msgs' are going to be sent to the
 * switch. */
void
ecmp_nexthop_monitor_flush(const struct rconn *swconn, struct ovs_list *msgs)
{
    struct ecmp_nexthop_data *e;
    HMAP_FOR_EACH_POP (e, hmap_node, &ecmp_nexthop_pending_flush) {
        VLOG_DBG("Flushing CT for ECMP next-hop %s (MAC %s) in zone %"PRIu16,
                 e->nexthop, e->mac, e->zone_id);
        ecmp_nexthop_monitor_flush_ct_entry(swconn, e->mac, e->zone_id,
                                            msgs);
        ecmp_nexthop_destroy_entry(e);
    }
}
