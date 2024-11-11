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
#include "lib/ovn-util.h"
#include "lib/simap.h"
#include "openvswitch/hmap.h"
#include "openvswitch/ofp-ct.h"
#include "openvswitch/rconn.h"
#include "openvswitch/vlog.h"
#include "ovn/logical-fields.h"
#include "ovn-sb-idl.h"
#include "controller/ecmp-next-hop-monitor.h"

VLOG_DEFINE_THIS_MODULE(ecmp_next_hop_monitor);

static struct smap ecmp_nexthop;

void ecmp_nexthop_init(void)
{
    smap_init(&ecmp_nexthop);
}

void ecmp_nexthop_destroy(void)
{
    smap_destroy(&ecmp_nexthop);
}

static void
ecmp_nexthop_monitor_flush_ct_entry(const struct rconn *swconn,
                                    const char *mac, struct ovs_list *msgs)
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
    struct ofpbuf *msg = ofp_ct_match_encode(&match, NULL,
                                             rconn_get_version(swconn));
    ovs_list_push_back(msgs, &msg->list_node);
}

void
ecmp_nexthop_monitor_run(const struct sbrec_ecmp_nexthop_table *enh_table,
                         const struct rconn *swconn, struct ovs_list *msgs)
{
    struct sset ecmp_sb_only = SSET_INITIALIZER(&ecmp_sb_only);
    struct simap mac_count = SIMAP_INITIALIZER(&mac_count);

    struct smap_node *node;
    SMAP_FOR_EACH_SAFE (node, &ecmp_nexthop) {
        simap_increase(&mac_count, node->value, 1);
    }

    const struct sbrec_ecmp_nexthop *sbrec_ecmp_nexthop;
    SBREC_ECMP_NEXTHOP_TABLE_FOR_EACH (sbrec_ecmp_nexthop, enh_table) {
        smap_replace(&ecmp_nexthop, sbrec_ecmp_nexthop->nexthop,
                     sbrec_ecmp_nexthop->mac);
        sset_add(&ecmp_sb_only, sbrec_ecmp_nexthop->nexthop);
    }

    SMAP_FOR_EACH_SAFE (node, &ecmp_nexthop) {
        /* Do not flush CT entries if the share the same mac address. */
        if (!sset_contains(&ecmp_sb_only, node->key)) {
            if (simap_get(&mac_count, node->value) == 1) {
                ecmp_nexthop_monitor_flush_ct_entry(swconn, node->value, msgs);
            }
            smap_remove_node(&ecmp_nexthop, node);
        }
    }

    sset_destroy(&ecmp_sb_only);
    simap_destroy(&mac_count);
}
