/*
 * Copyright (c) 2024 Canonical
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

#include <errno.h>
#include <net/if.h>

#include "openvswitch/vlog.h"

#include "lib/ovn-sb-idl.h"

#include "binding.h"
#include "ha-chassis.h"
#include "lb.h"
#include "local_data.h"
#include "route-exchange.h"
#include "route-exchange-netlink.h"


VLOG_DEFINE_THIS_MODULE(route_exchange);
static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);

/* While the linux kernel can handle 2^32 routing tables, only so many can fit
 * in the corresponding VRF interface name. */
#define MAX_TABLE_ID 1000000000

static struct sset _maintained_vrfs = SSET_INITIALIZER(&_maintained_vrfs);

bool
route_exchange_relevant_port(const struct sbrec_port_binding *pb) {
    return (pb && pb->type && !strcmp(pb->type, "l3gateway") &&
                (smap_get_bool(&pb->options, "redistribute-lb-vips", false) ||
                 smap_get_bool(&pb->options, "redistribute-nat", false)));
}

static void
extract_nat_addresses(const struct sbrec_port_binding *pb,
                      struct route_exchange_ctx_in *r_ctx_in,
                      uint32_t table_id, struct hmap *host_routes)
{
    if (!pb || !pb->n_nat_addresses) {
        return;
    }
    VLOG_DBG("extract_nat_addresses: considering lport %s", pb->logical_port);

    for (size_t i = 0; i < pb->n_nat_addresses; i++) {
        struct lport_addresses *laddrs = xzalloc(sizeof *laddrs);
        char *lport = NULL;

        if (!extract_addresses_with_port(
                pb->nat_addresses[i], laddrs, &lport)) {
            VLOG_DBG("extract_nat_addresses: no addresses");
            goto cleanup;
        }
        if (lport) {
            const struct sbrec_port_binding *lport_pb = lport_lookup_by_name(
                    r_ctx_in->sbrec_port_binding_by_name, lport);
            if (!lport_pb || !lport_pb->chassis) {
                VLOG_DBG("extract_nat_addresses: cannot find lport %s",
                         lport);
                goto cleanup;
            }
            enum en_lport_type lport_pb_type = get_lport_type(lport_pb);
            if (((lport_pb_type == LP_VIF ||
                  lport_pb_type == LP_CHASSISREDIRECT) &&
                 lport_pb->chassis != r_ctx_in->chassis_rec) ||
                 !ha_chassis_group_is_active(lport_pb->ha_chassis_group,
                                             r_ctx_in->active_tunnels,
                                             r_ctx_in->chassis_rec)) {
                VLOG_DBG("extract_nat_addresses: ignoring non-local lport %s",
                         lport);
                goto cleanup;
            }
        }
        for (size_t j = 0; j < laddrs->n_ipv4_addrs; j++) {
            struct in6_addr addr;
            in6_addr_set_mapped_ipv4(&addr, laddrs->ipv4_addrs[j].addr);
            host_route_insert(host_routes, table_id, &addr);
        }
        for (size_t j = 0; j < laddrs->n_ipv6_addrs; j++) {
            host_route_insert(host_routes, table_id,
                              &laddrs->ipv6_addrs[j].addr);
        }

cleanup:
        destroy_lport_addresses(laddrs);
        free(laddrs);
        if (lport) {
            free(lport);
        }
    }
}

static void
extract_lb_vips(const struct sbrec_datapath_binding *dpb,
                struct hmap *lbs_by_dp_hmap,
                const struct route_exchange_ctx_in *r_ctx_in,
                uint32_t table_id, struct hmap *host_routes)
{
    struct load_balancers_by_dp *lbs_by_dp
        = load_balancers_by_dp_find(lbs_by_dp_hmap, dpb);
    if (!lbs_by_dp) {
        return;
    }

    for (size_t i = 0; i < lbs_by_dp->n_dp_lbs; i++) {
        const struct sbrec_load_balancer *sbrec_lb
            = lbs_by_dp->dp_lbs[i];

        if (!sbrec_lb) {
            return;
        }

        struct ovn_controller_lb *lb
            = ovn_controller_lb_find(r_ctx_in->local_lbs,
                                     &sbrec_lb->header_.uuid);

        if (!lb || !lb->slb) {
            return;
        }

        VLOG_DBG("considering lb for route leaking: %s", lb->slb->name);
        for (i = 0; i < lb->n_vips; i++) {
            VLOG_DBG("considering lb for route leaking: %s vip_str=%s",
                      lb->slb->name, lb->vips[i].vip_str);
            host_route_insert(host_routes, table_id, &lb->vips[i].vip);
        }
    }
}

void
route_exchange_run(struct route_exchange_ctx_in *r_ctx_in,
                   struct route_exchange_ctx_out *r_ctx_out)
{
    struct sset old_maintained_vrfs = SSET_INITIALIZER(&old_maintained_vrfs);
    sset_swap(&_maintained_vrfs, &old_maintained_vrfs);
    struct hmap *lbs_by_dp_hmap
        = load_balancers_by_dp_init(r_ctx_in->local_datapaths,
                                    r_ctx_in->lb_table);

    /* Extract all NAT- and LB VIP-addresses associated with lports resident on
     * the current chassis to allow full sync of leaked routing tables. */
    const struct local_datapath *ld;
    HMAP_FOR_EACH (ld, hmap_node, r_ctx_in->local_datapaths) {
        if (!ld->n_peer_ports || ld->is_switch) {
            continue;
        }

        bool maintain_vrf = false;
        bool lbs_sync = false;
        struct hmap local_host_routes_for_current_dp
            = HMAP_INITIALIZER(&local_host_routes_for_current_dp);

        /* This is a LR datapath, find LRPs with route exchange options. */
        for (size_t i = 0; i < ld->n_peer_ports; i++) {
            const struct sbrec_port_binding *local_peer
                = ld->peer_ports[i].local;
            if (!local_peer || !route_exchange_relevant_port(local_peer)) {
                continue;
            }

            maintain_vrf |= smap_get_bool(&local_peer->options,
                                          "maintain-vrf", false);
            lbs_sync |= smap_get_bool(&local_peer->options,
                                    "redistribute-lb-vips",
                                    false);
            if (smap_get_bool(&local_peer->options,
                              "redistribute-nat",
                              false)) {
                extract_nat_addresses(local_peer, r_ctx_in,
                                      ld->datapath->tunnel_key,
                                      &local_host_routes_for_current_dp);
            }
        }

        if (lbs_sync) {
            extract_lb_vips(ld->datapath, lbs_by_dp_hmap, r_ctx_in,
                            ld->datapath->tunnel_key,
                            &local_host_routes_for_current_dp);
        }

        /* While tunnel_key would most likely never be negative, the compiler
         * has opinions if we don't check before using it in snprintf below. */
        if (ld->datapath->tunnel_key < 0 ||
            ld->datapath->tunnel_key > MAX_TABLE_ID) {
            VLOG_WARN_RL(&rl,
                         "skip route sync for datapath "UUID_FMT", "
                         "tunnel_key %"PRIi64" would make VRF interface name "
                         "overflow.",
                         UUID_ARGS(&ld->datapath->header_.uuid),
                         ld->datapath->tunnel_key);
            goto out;
        }
        char vrf_name[IFNAMSIZ + 1];
        snprintf(vrf_name, sizeof vrf_name, "ovnvrf%"PRIi64,
                 ld->datapath->tunnel_key);

        if (maintain_vrf) {
            int error = re_nl_create_vrf(vrf_name, ld->datapath->tunnel_key);
            if (error && error != EEXIST) {
                VLOG_WARN_RL(&rl,
                             "Unable to create VRF %s for datapath "UUID_FMT
                             ": %s.",
                             vrf_name, UUID_ARGS(&ld->datapath->header_.uuid),
                             ovs_strerror(error));
                goto out;
            }
            sset_add(&_maintained_vrfs, vrf_name);
        }
        if (!hmap_is_empty(&local_host_routes_for_current_dp)) {
            tracked_datapath_add(ld->datapath, TRACKED_RESOURCE_NEW,
                                 r_ctx_out->tracked_re_datapaths);
        }
        re_nl_sync_routes(ld->datapath->tunnel_key, vrf_name,
                          &local_host_routes_for_current_dp);

out:
        host_routes_destroy(&local_host_routes_for_current_dp);
    }

    /* Remove VRFs previously maintained by us not found in the above loop. */
    const char *vrf_name;
    SSET_FOR_EACH_SAFE (vrf_name, &old_maintained_vrfs) {
        if (!sset_find(&_maintained_vrfs, vrf_name)) {
            re_nl_delete_vrf(vrf_name);
        }
        sset_delete(&old_maintained_vrfs, SSET_NODE_FROM_NAME(vrf_name));
    }
    sset_destroy(&old_maintained_vrfs);

    load_balancers_by_dp_cleanup(lbs_by_dp_hmap);
}

static void
route_exchange_cleanup__(bool cleanup)
{
    const char *vrf_name;
    SSET_FOR_EACH_SAFE (vrf_name, &_maintained_vrfs) {
        if (cleanup) {
            re_nl_delete_vrf(vrf_name);
        } else {
            sset_delete(&_maintained_vrfs, SSET_NODE_FROM_NAME(vrf_name));
        }
    }
    if (!cleanup) {
        sset_destroy(&_maintained_vrfs);
    }
}

void
route_exchange_cleanup(void)
{
    route_exchange_cleanup__(true);
}

void
route_exchange_destroy(void)
{
    route_exchange_cleanup__(false);
}
