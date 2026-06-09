/*
 * Copyright (c) 2025, STACKIT GmbH & Co. KG
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

#include "northd.h"

#include "en-advertised-route-sync.h"
#include "en-lr-nat.h"
#include "en-lr-stateful.h"
#include "lb.h"
#include "openvswitch/hmap.h"
#include "openvswitch/vlog.h"
#include "ovn-util.h"
#include "util.h"

VLOG_DEFINE_THIS_MODULE(en_advertised_route_sync);

struct ar_entry {
    struct hmap_node hmap_node;

    const struct ovn_datapath *od;       /* Datapath the route is
                                          * advertised on. */
    const struct ovn_port *op;           /* Port the route is advertised
                                          * on. */
    char *ip_prefix;
    const struct ovn_port *tracked_port; /* If set, the port whose chassis
                                          * advertises this route with a
                                          * higher priority. */
    /* Optional backend service selector. Populated for LB-derived routes
     * when northd has per-backend information (ip_port_mappings on the
     * Load_Balancer). All three fields must be set together or all left
     * unset: a partial selector could match an unrelated Service_Monitor
     * row, so the entire tuple is omitted when the LB protocol is not
     * accepted by lb_service_monitor_protocol_supported(). */
    char *tracked_service_ip;
    int64_t tracked_service_port;
    bool has_tracked_service_port;
    char *tracked_service_protocol;
    enum route_source source;
};

/* Add a new entries to the to-be-advertised routes.
 * Takes ownership of ip_prefix. */
static struct ar_entry *
ar_entry_add_nocopy(struct hmap *routes, const struct ovn_datapath *od,
                    const struct ovn_port *op, char *ip_prefix,
                    const struct ovn_port *tracked_port,
                    enum route_source source)
{
    struct ar_entry *route_e = xzalloc(sizeof *route_e);

    route_e->od = od;
    route_e->op = op;
    route_e->ip_prefix = ip_prefix;
    route_e->tracked_port = tracked_port;
    route_e->source = source;
    uint32_t hash = uuid_hash(&od->sdp->sb_dp->header_.uuid);
    hash = hash_string(op->sb->logical_port, hash);
    hash = hash_string(ip_prefix, hash);
    hmap_insert(routes, &route_e->hmap_node, hash);

    return route_e;
}

/* Add a new entries to the to-be-advertised routes.
 * Makes a copy of ip_prefix. */
static struct ar_entry *
ar_entry_add(struct hmap *routes, const struct ovn_datapath *od,
             const struct ovn_port *op, const char *ip_prefix,
             const struct ovn_port *tracked_port,
             enum route_source source)
{
    return ar_entry_add_nocopy(routes, od, op, xstrdup(ip_prefix),
                               tracked_port, source);
}

/* Find an ar_entry whose (datapath, logical_port, ip_prefix,
 * tracked_port, tracked_service_ip, tracked_service_port,
 * tracked_service_protocol) tuple matches the full key. The SB
 * Advertised_Route unique index includes the service selector
 * columns, so multiple rows for the same VIP IP and
 * backend LSP are allowed when they differ by per-backend
 * selector, so ar_entry_find must compare the full key. */
static struct ar_entry *
ar_entry_find(struct hmap *route_map,
              const struct sbrec_datapath_binding *sb_db,
              const struct sbrec_port_binding *logical_port,
              const char *ip_prefix,
              const struct sbrec_port_binding *tracked_port,
              const char *tracked_service_ip,
              bool has_tracked_service_port,
              int64_t tracked_service_port,
              const char *tracked_service_protocol)
{
    struct ar_entry *route_e;
    uint32_t hash;

    hash = uuid_hash(&sb_db->header_.uuid);
    hash = hash_string(logical_port->logical_port, hash);
    hash = hash_string(ip_prefix, hash);

    HMAP_FOR_EACH_WITH_HASH (route_e, hmap_node, hash, route_map) {
        if (!uuid_equals(&sb_db->header_.uuid,
                         &route_e->od->sdp->sb_dp->header_.uuid)) {
            continue;
        }
        if (!uuid_equals(&logical_port->header_.uuid,
                         &route_e->op->sb->header_.uuid)) {
            continue;
        }
        if (strcmp(ip_prefix, route_e->ip_prefix)) {
            continue;
        }

        if (tracked_port) {
            if (!route_e->tracked_port ||
                    tracked_port != route_e->tracked_port->sb) {
                continue;
            }
        } else if (route_e->tracked_port) {
            continue;
        }

        if (!nullable_string_is_equal(tracked_service_ip,
                                      route_e->tracked_service_ip)) {
            continue;
        }
        if (has_tracked_service_port != route_e->has_tracked_service_port) {
            continue;
        }
        if (has_tracked_service_port &&
            tracked_service_port != route_e->tracked_service_port) {
            continue;
        }
        if (!nullable_string_is_equal(tracked_service_protocol,
                                      route_e->tracked_service_protocol)) {
            continue;
        }

        return route_e;
    }

    return NULL;
}

static void
ar_entry_free(struct ar_entry *route_e)
{
    free(route_e->ip_prefix);
    free(route_e->tracked_service_ip);
    free(route_e->tracked_service_protocol);
    free(route_e);
}

/* Attach a per-backend service selector (ip, l4 port, protocol) to a
 * previously added ar_entry. All three parameters are required:
 * a partial selector could match an unrelated Service_Monitor row
 * on the same (ip, port) for a different protocol.
 *
 * protocol must be one of the protocols accepted by
 * lb_service_monitor_protocol_supported(), or the caller must leave the entire
 * selector unset (i.e. not invoke this helper) for LB protocols
 * outside that set. */
static void
ar_entry_set_service_selector(struct ar_entry *route_e,
                               const char *ip, int64_t port,
                               const char *protocol)
{
    if (!ip || !protocol) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);
        VLOG_WARN_RL(&rl, "Cannot set partial service selector: "
                     "ip=%s protocol=%s", ip ? ip : "(null)",
                     protocol ? protocol : "(null)");
        return;
    }

    route_e->tracked_service_ip = xstrdup(ip);
    route_e->tracked_service_port = port;
    route_e->has_tracked_service_port = true;
    route_e->tracked_service_protocol = xstrdup(protocol);
}

static void
ar_entry_copy_service_selector(struct ar_entry *dst,
                               const struct ar_entry *src)
{
    if (src->tracked_service_ip) {
        dst->tracked_service_ip = xstrdup(src->tracked_service_ip);
    }
    if (src->has_tracked_service_port) {
        dst->tracked_service_port = src->tracked_service_port;
        dst->has_tracked_service_port = true;
    }
    if (src->tracked_service_protocol) {
        dst->tracked_service_protocol = xstrdup(src->tracked_service_protocol);
    }
}

static void
advertised_route_table_sync(
    struct ovsdb_idl_txn *ovnsb_txn,
    const struct sbrec_advertised_route_table *sbrec_advertised_route_table,
    const struct hmap *routes,
    const struct hmap *dynamic_routes);

void *
en_advertised_route_sync_init(struct engine_node *node OVS_UNUSED,
                              struct engine_arg *arg OVS_UNUSED)
{
    return NULL;
}

void
en_advertised_route_sync_cleanup(void *data OVS_UNUSED)
{
}

enum engine_node_state
en_advertised_route_sync_run(struct engine_node *node, void *data OVS_UNUSED)
{
    struct routes_data *routes_data
        = engine_get_input_data("routes", node);
    struct dynamic_routes_data *dynamic_routes_data
        = engine_get_input_data("dynamic_routes", node);
    const struct engine_context *eng_ctx = engine_get_context();
    const struct sbrec_advertised_route_table *sbrec_advertised_route_table =
        EN_OVSDB_GET(engine_get_input("SB_advertised_route", node));

    advertised_route_table_sync(eng_ctx->ovnsb_idl_txn,
                                sbrec_advertised_route_table,
                                &routes_data->parsed_routes,
                                &dynamic_routes_data->routes);
    return EN_UPDATED;
}

/* Track the ovn_datapath that is relevant to dynamic routing computation. */
static void
dynamic_routes_track_od(struct dynamic_routes_data *data,
                        const struct ovn_datapath *od)
{
    uuidset_insert(od->nbr ? &data->nb_lr : &data->nb_ls, &od->key);
}

/* Install a parsed_route on advertising_od that forwards ip_address (a
 * LB VIP or NAT external IP) through advertising_op to tracked_port,
 * where tracked_port must be a peer LRP on the shared LS so that its
 * first matching-family network address is a valid nexthop.
 *
 * Used by the connected-neighbour redistribution paths
 * (build_{lb,nat}_connected_routes) so the advertising LR can
 * forward to the peer's VIPs and external IPs, not just advertise
 * reachability for them.
 *
 * For distributed NAT the tracked_port is the backend's LSP (not an LRP) and
 * doesn't carry lrp_networks - in that case this function is a no-op. Such
 * deployments still rely on the existing ARP-resolved data path.
 *
 * Silently no-ops when:
 *   - tracked_port is not an LRP (no nexthop derivable from this hop), or
 *   - the prefix string fails to parse, or
 *   - the peer LRP carries no address of the prefix's IP family.
 *
 * When advertising_op is unnumbered for the nexthop's family, lrp_addr_s
 * is NULL. */
static void
add_redistribute_parsed_route(struct hmap *parsed_routes_out,
                              const struct ovn_datapath *advertising_od,
                              const struct ovn_port *advertising_op,
                              const struct ovn_port *tracked_port,
                              const char *ip_address,
                              enum route_source source)
{
    if (!tracked_port || !tracked_port->nbrp) {
        /* Not an LRP-typed tracked port (e.g. distributed NAT bound to a
         * specific LSP). No nexthop available from this hop. */
        return;
    }

    /* Parse the prefix (the VIP/FIP). */
    struct in6_addr prefix;
    if (!ip46_parse(ip_address, &prefix)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "Failed to parse IP address '%s' for %s "
                     "redistribute forwarding route on datapath %s",
                     ip_address,
                     source == ROUTE_SOURCE_LB ? "LB" : "NAT",
                     advertising_od->nbr ? advertising_od->nbr->name
                                         : "<unknown>");
        return;
    }
    bool is_v6 = !IN6_IS_ADDR_V4MAPPED(&prefix);
    unsigned int plen = is_v6 ? 128 : 32;

    /* Choose the nexthop from the peer LRP's first matching-family address. */
    const char *nexthop_s = NULL;
    if (!is_v6 && tracked_port->lrp_networks.n_ipv4_addrs) {
        nexthop_s = tracked_port->lrp_networks.ipv4_addrs[0].addr_s;
    } else if (is_v6 && tracked_port->lrp_networks.n_ipv6_addrs) {
        nexthop_s = tracked_port->lrp_networks.ipv6_addrs[0].addr_s;
    }
    if (!nexthop_s) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "No %s address on tracked port %s for %s "
                     "redistribute forwarding route (prefix %s)",
                     is_v6 ? "IPv6" : "IPv4",
                     tracked_port->key,
                     source == ROUTE_SOURCE_LB ? "LB" : "NAT",
                     ip_address);
        return;
    }

    /* If advertising_op has an address in the nexthop's family, use it as
     * eth.src. Otherwise (unnumbered LRP) leave lrp_addr_s NULL so the
     * emitted route omits REG_SRC_IPV{4,6}. ARP resolution still works:
     * the LS-level ls_in_arp_rsp responder matches on arp.tpa alone. */
    const char *lrp_addr_s = lrp_find_member_ip(advertising_op, nexthop_s);

    struct in6_addr *nexthop = xmalloc(sizeof *nexthop);
    if (!ip46_parse(nexthop_s, nexthop)) {
        free(nexthop);
        return;
    }

    parsed_route_add(advertising_od, nexthop, &prefix, plen,
                     false,
                     lrp_addr_s, advertising_op,
                     0,
                     false,
                     false,
                     false,
                     NULL,
                     source,
                     false,
                     NULL,
                     tracked_port,
                     parsed_routes_out);
}

/* This function adds a new route for each entry in lr_nat record
 * to "routes". Logical port of the route is set to "advertising_op" and
 * tracked port is set to NAT's distributed gw port. If NAT doesn't have
 * DGP (for example if it's set on gateway router), no tracked port will
 * be set.
 *
 * If parsed_routes_out is non-NULL, also installs a local forwarding
 * parsed_route on advertising_op->od for each NAT external IP whose
 * nexthop is available from tracked_port (i.e. a peer LRP). This is the
 * connected-neighbour redistribution case where the advertising LR
 * needs to forward to the peer's LR. */
static void
build_nat_route_for_port(const struct ovn_port *advertising_op,
                         const struct lr_nat_record *lr_nat,
                         const struct hmap *ls_ports,
                         struct hmap *routes,
                         struct hmap *parsed_routes_out)
{
    const struct ovn_datapath *advertising_od = advertising_op->od;

    for (size_t i = 0; i < lr_nat->n_nat_entries; i++) {
        const struct ovn_nat *nat = &lr_nat->nat_entries[i];
        if (!nat->is_valid) {
            continue;
        }

        if (!smap_get_bool(&nat->nb->options,
                           "dynamic-routing-advertise", true)) {
            continue;
        }

        const struct ovn_port *tracked_port =
            nat->is_distributed
            ? ovn_port_find(ls_ports, nat->nb->logical_port)
            : nat->l3dgw_port;

        /* NAT routes carry no service selector, so pass NULL/0/NULL for
         * the selector portion of the dedup key. */
        if (!ar_entry_find(routes, advertising_od->sdp->sb_dp,
                           advertising_op->sb,
                           nat->nb->external_ip,
                           tracked_port ? tracked_port->sb : NULL,
                           NULL, false, 0, NULL)) {
            ar_entry_add(routes, advertising_od, advertising_op,
                         nat->nb->external_ip, tracked_port,
                         ROUTE_SOURCE_NAT);
        }

        if (parsed_routes_out) {
            add_redistribute_parsed_route(parsed_routes_out, advertising_od,
                                          advertising_op, tracked_port,
                                          nat->nb->external_ip,
                                          ROUTE_SOURCE_NAT);
        }
    }
}

/* Generate routes for NAT external IPs in lr_nat, for each ovn port
 * in "od" that has enabled redistribution of NAT addresses.
 *
 * No forwarding route is needed because the LR owns the NAT and
 * its own NAT pipeline handles ingress for the external IP. */
static void
build_nat_routes(const struct ovn_datapath *od,
                 const struct lr_nat_record *lr_nat,
                 const struct hmap *ls_ports,
                 struct hmap *routes)
{
    const struct ovn_port *op;
    HMAP_FOR_EACH (op, dp_node, &od->ports) {
        if (!drr_mode_NAT_is_set(op->dynamic_routing_redistribute)) {
            continue;
        }

        build_nat_route_for_port(op, lr_nat, ls_ports, routes,
                                 NULL);
    }
}

/* Similar to build_nat_routes, this function generates routes for nat records
 * in neighboring routers. For each ovn port in "od" that has enabled
 * redistribution of NAT adresses, look up their neighbors (either directly
 * connected routers, or routers connected through common LS) and advertise
 * thier external NAT IPs too.*/
static void
build_nat_connected_routes(
    const struct ovn_datapath *od,
    const struct lr_stateful_table *lr_stateful_table,
    const struct hmap *ls_ports,
    struct dynamic_routes_data *data)
{
    const struct ovn_port *op;
    HMAP_FOR_EACH (op, dp_node, &od->ports) {
        if (!drr_mode_NAT_is_set(op->dynamic_routing_redistribute)) {
            continue;
        }

        if (!op->peer) {
            continue;
        }

        struct ovn_datapath *peer_od = op->peer->od;
        ovs_assert(peer_od->nbs || peer_od->nbr);
        /* Track the peer datapath for any changes. */
        dynamic_routes_track_od(data, peer_od);

        /* This is a directly connected LR peer. */
        if (peer_od->nbr) {
            const struct lr_stateful_record *peer_lr_stateful =
                lr_stateful_table_find_by_uuid(lr_stateful_table,
                                                 peer_od->key);
            if (!peer_lr_stateful) {
                continue;
            }

            /* Advertise peer's NAT routes via the local port too, and
             * install forwarding routes so we can reach the
             * peer's external IPs. */
            build_nat_route_for_port(op, peer_lr_stateful->lrnat_rec,
                                     ls_ports, &data->routes,
                                     &data->parsed_routes);
            continue;
        }

        /* This peer is LSP, we need to check all connected router ports
         * for NAT.*/
        const struct ovn_port *rp;
        VECTOR_FOR_EACH (&peer_od->router_ports, rp) {
            if (rp->peer == op) {
                /* Skip advertising router. */
                continue;
            }

            const struct lr_stateful_record *peer_lr_stateful =
                lr_stateful_table_find_by_uuid(lr_stateful_table,
                                                rp->peer->od->key);
            if (!peer_lr_stateful) {
                continue;
            }

            /* Advertise peer's NAT routes via the local port too, and
             * install forwarding routes so we can reach the
             * peer's external IPs. */
            build_nat_route_for_port(op, peer_lr_stateful->lrnat_rec,
                                     ls_ports, &data->routes,
                                     &data->parsed_routes);
            /* Track the LR datapath on the other side of LS
             * for any changes. */
            dynamic_routes_track_od(data, rp->peer->od);
        }
    }
}

/* For each LB attached to peer_lr_nbr, emit one Advertised_Route per
 * (VIP, backend LSP) pair, plus one forwarding parsed_route per VIP.
 * Backends without ip_port_mappings fall back to one Advertised_Route
 * per VIP with fallback_tracked_port in place of a per-backend LSP.
 * The forwarding route is emitted once per VIP regardless of backend
 * count: the data-plane forwarding decision is independent of which
 * backend ends up serving the flow. */
static void
build_lb_lr_routes(const struct ovn_port *advertising_op,
                   const struct ovn_port *fallback_tracked_port,
                   const struct nbrec_logical_router *peer_lr_nbr,
                   const struct hmap *lb_datapaths_map,
                   const struct hmap *ls_ports,
                   struct hmap *routes,
                   struct hmap *parsed_routes_out)
{
    const struct ovn_datapath *advertising_od = advertising_op->od;

    if (!peer_lr_nbr) {
        return;
    }

    for (size_t i = 0; i < peer_lr_nbr->n_load_balancer; i++) {
        const struct nbrec_load_balancer *nbrec_lb =
            peer_lr_nbr->load_balancer[i];
        if (!smap_get_bool(&nbrec_lb->options,
                           "dynamic-routing-advertise", true)) {
            continue;
        }
        const struct uuid *lb_uuid = &nbrec_lb->header_.uuid;
        const struct ovn_lb_datapaths *lb_dps =
            ovn_lb_datapaths_find(lb_datapaths_map, lb_uuid);
        if (!lb_dps) {
            continue;
        }
        const struct ovn_northd_lb *lb = lb_dps->lb;
        for (size_t v = 0; v < lb->n_vips; v++) {
            const struct ovn_lb_vip *vip = &lb->vips[v];
            const struct ovn_northd_lb_vip *vip_nb = &lb->vips_nb[v];

            if (parsed_routes_out) {
                add_redistribute_parsed_route(
                    parsed_routes_out, advertising_od, advertising_op,
                    fallback_tracked_port, vip->vip_str, ROUTE_SOURCE_LB);
            }

            /* Protocols not accepted by
             * lb_service_monitor_protocol_supported() can never produce a
             * matching Service_Monitor row, and a partial selector
             * (ip+port, no protocol) risks matching an unrelated
             * monitor on the same ip/port for a different protocol.
             * Leave the whole selector unset in that case. */
            bool proto_supported =
                lb_service_monitor_protocol_supported(lb->proto);

            bool emitted_any = false;
            for (size_t b = 0; b < vip_nb->n_backends; b++) {
                const char *lsp_name = vip_nb->backends_nb[b].logical_port;
                if (!lsp_name) {
                    continue;
                }
                const struct ovn_port *backend_op =
                    ovn_port_find(ls_ports, lsp_name);
                if (!backend_op) {
                    continue;
                }
                struct ar_entry *route_e =
                    ar_entry_add(routes, advertising_od, advertising_op,
                                 vip->vip_str, backend_op, ROUTE_SOURCE_LB);
                if (proto_supported) {
                    const struct ovn_lb_backend *backend =
                        vector_get_ptr(&vip->backends, b);
                    ar_entry_set_service_selector(route_e, backend->ip_str,
                                                  backend->port, lb->proto);
                }
                emitted_any = true;
            }
            if (!emitted_any) {
                ar_entry_add(routes, advertising_od, advertising_op,
                             vip->vip_str, fallback_tracked_port,
                             ROUTE_SOURCE_LB);
            }
        }
    }
}

/* Own-LR entry point used by the own-LR (gateway-router/DGP) path,
 * which doesn't currently route through a peer LR's LBs. Emits one
 * Advertised_Route per IP in lb_ips with tracked_port as-is.
 *
 * If parsed_routes_out is non-NULL, also installs one local forwarding
 * parsed_route per VIP, used by the connected-neighbour redistribution
 * case so the advertising LR can reach the peer's VIPs. */
static void
build_lb_route_for_port(const struct ovn_port *advertising_op,
                        const struct ovn_port *tracked_port,
                        const struct ovn_lb_ip_set *lb_ips,
                        struct hmap *routes,
                        struct hmap *parsed_routes_out)
{
    const struct ovn_datapath *advertising_od = advertising_op->od;

    const char *ip_address;
    SSET_FOR_EACH (ip_address, &lb_ips->ips_v4_adv) {
        ar_entry_add(routes, advertising_od, advertising_op,
                     ip_address, tracked_port, ROUTE_SOURCE_LB);
        if (parsed_routes_out) {
            add_redistribute_parsed_route(parsed_routes_out, advertising_od,
                                          advertising_op, tracked_port,
                                          ip_address, ROUTE_SOURCE_LB);
        }
    }
    SSET_FOR_EACH (ip_address, &lb_ips->ips_v6_adv) {
        ar_entry_add(routes, advertising_od, advertising_op,
                     ip_address, tracked_port, ROUTE_SOURCE_LB);
        if (parsed_routes_out) {
            add_redistribute_parsed_route(parsed_routes_out, advertising_od,
                                          advertising_op, tracked_port,
                                          ip_address, ROUTE_SOURCE_LB);
        }
    }
}

/* Similar to build_lb_routes, this function generates routes for LB VIPs
 * of neighboring routers. For each ovn port in "od" that has enabled
 * redistribution of LB VIPs, look up their neighbors (either directly
 * routers, or routers connected through common LS) and advertise their
 * LB VIPs too.*/
static void
build_lb_connected_routes(const struct ovn_datapath *od,
                          const struct hmap *lb_datapaths_map,
                          const struct hmap *ls_ports,
                          struct dynamic_routes_data *data)
{
    const struct ovn_port *op;
    HMAP_FOR_EACH (op, dp_node, &od->ports) {
        if (!drr_mode_LB_is_set(op->dynamic_routing_redistribute)) {
            continue;
        }

        if (!op->peer) {
            continue;
        }

        struct ovn_datapath *peer_od = op->peer->od;
        ovs_assert(peer_od->nbs || peer_od->nbr);
        /* Track the peer datapath for any changes. */
        dynamic_routes_track_od(data, peer_od);

        /* This is directly connected LR peer. */
        if (peer_od->nbr) {
            build_lb_lr_routes(op, op->peer, peer_od->nbr,
                               lb_datapaths_map, ls_ports,
                               &data->routes, &data->parsed_routes);
            continue;
        }

        /* This peer is LSP, we need to check all connected router ports for
         * LBs.*/
        struct ovn_port *rp;
        VECTOR_FOR_EACH (&peer_od->router_ports, rp) {
            if (rp->peer == op) {
                /* no need to check for LBs on ovn_port that initiated this
                 * function.*/
                continue;
            }
            build_lb_lr_routes(op, rp->peer, rp->peer->od->nbr,
                               lb_datapaths_map, ls_ports,
                               &data->routes, &data->parsed_routes);
            /* Track the LR datapath on the other side of LS
             * for any changes. */
            dynamic_routes_track_od(data, rp->peer->od);
        }
    }
}

static void
build_lb_routes(const struct ovn_datapath *od,
                const struct ovn_lb_ip_set *lb_ips,
                struct hmap *routes)
{
    const struct ovn_port *op;
    HMAP_FOR_EACH (op, dp_node, &od->ports) {
        if (!drr_mode_LB_is_set(op->dynamic_routing_redistribute)) {
            continue;
        }

        /* Traffic processed by a load balancer is:
         * - handled by the chassis where a gateway router is bound
         * OR
         * - always redirected to a distributed gateway router port
         *
         * Advertise the LB IPs via all 'op' if this is a gateway router or
         * throuh all DGPs of this distributed router otherwise. */

        if (od->is_gw_router) {
            build_lb_route_for_port(op, NULL, lb_ips, routes,
                                    NULL);
        } else {
            struct ovn_port *dgp;
            VECTOR_FOR_EACH (&od->l3dgw_ports, dgp) {
                build_lb_route_for_port(op, dgp, lb_ips, routes,
                                        NULL);
            }
        }
    }
}

static void
publish_lport_addresses(struct hmap *routes,
                        const struct ovn_datapath *od,
                        const struct ovn_port *logical_port,
                        const struct lport_addresses *addresses,
                        const struct ovn_port *tracking_port)
{
    for (size_t i = 0; i < addresses->n_ipv4_addrs; i++) {
        const struct ipv4_netaddr *addr = &addresses->ipv4_addrs[i];
        ar_entry_add(routes, od, logical_port, addr->addr_s,
                     tracking_port, ROUTE_SOURCE_CONNECTED_AS_HOST);
    }
    for (size_t i = 0; i < addresses->n_ipv6_addrs; i++) {
        if (in6_is_lla(&addresses->ipv6_addrs[i].network)) {
            continue;
        }
        const struct ipv6_netaddr *addr = &addresses->ipv6_addrs[i];
        ar_entry_add(routes, od, logical_port, addr->addr_s,
                     tracking_port, ROUTE_SOURCE_CONNECTED_AS_HOST);
    }
}

static void
publish_host_routes_for_virtual_ports(struct ovn_port *port,
                                      const struct hmap *ls_ports,
                                      const struct ovn_port *op,
                                      struct dynamic_routes_data *data)
{
    ovs_assert(port->sb);

    const char *virtual_parent = port->sb->virtual_parent;
    if (!virtual_parent) {
        return;
    }

    struct ovn_port *vp = ovn_port_find(ls_ports, virtual_parent);
    if (!vp) {
        return;
    }

    /* Track the virtual parent datapath for any changes. */
    dynamic_routes_track_od(data, vp->od);

    for (size_t i = 0; i < port->n_lsp_addrs; i++) {
        publish_lport_addresses(&data->routes, op->od, op,
                                &port->lsp_addrs[i], vp);
    }
}

/* Collect all IP addresses connected to the out_port of a route.
 * This traverses all LSPs on the LS connected to the out_port. */
static void
publish_host_routes(struct dynamic_routes_data *data,
                    const struct hmap *ls_ports, const struct ovn_port *op)
{
    if (!op->peer) {
        return;
    }

    struct ovn_datapath *peer_od = op->peer->od;
    ovs_assert(peer_od->nbs || peer_od->nbr);
    /* Track the peer datapath for any changes. */
    dynamic_routes_track_od(data, peer_od);

    if (peer_od->nbr) {
        /* This is a LRP directly connected to another LRP. */
        const struct ovn_port *lrp = op->peer;
        if (smap_get_bool(&lrp->nbrp->options,
                          "dynamic-routing-advertise", true)) {
            publish_lport_addresses(&data->routes, op->od, op,
                                    &lrp->lrp_networks, lrp);
        }
        return;
    }

    struct ovn_port *port;
    HMAP_FOR_EACH (port, dp_node, &peer_od->ports) {
        if (!smap_get_bool(&port->nbsp->options,
                           "dynamic-routing-advertise", true)) {
            continue;
        }

        if (port->peer && port->peer->nbrp) {
            /* This is a LSP connected to an LRP */
            const struct ovn_port *lrp = port->peer;
            if (!smap_get_bool(&lrp->nbrp->options,
                               "dynamic-routing-advertise", true)) {
                continue;
            }
            publish_lport_addresses(&data->routes, op->od, op,
                                    &lrp->lrp_networks, lrp);
            /* Track the LR datapath on the other side of LS
             * for any changes. */
            dynamic_routes_track_od(data, lrp->od);
        } else if (port->nbsp && !strcmp(port->nbsp->type, "virtual")) {
            publish_host_routes_for_virtual_ports(port, ls_ports, op, data);
        } else {
            /* This is just a plain LSP */
            for (size_t i = 0; i < port->n_lsp_addrs; i++) {
                publish_lport_addresses(&data->routes, op->od, op,
                                        &port->lsp_addrs[i], port);
            }
        }
    }
}

static void
build_connected_as_host_routes(const struct ovn_datapath *od,
                               const struct hmap *ls_ports,
                               struct dynamic_routes_data *data)
{
    const struct ovn_port *op;
    HMAP_FOR_EACH (op, dp_node, &od->ports) {
        enum dynamic_routing_redistribute_mode drr =
            op->dynamic_routing_redistribute;
        if (!drr_mode_CONNECTED_AS_HOST_is_set(drr)) {
            continue;
        }

        publish_host_routes(data, ls_ports, op);
    }
}

void *
en_dynamic_routes_init(struct engine_node *node OVS_UNUSED,
                       struct engine_arg *arg OVS_UNUSED)
{
    struct dynamic_routes_data *data = xmalloc(sizeof *data);
    *data = (struct dynamic_routes_data) {
        .routes = HMAP_INITIALIZER(&data->routes),
        .parsed_routes = HMAP_INITIALIZER(&data->parsed_routes),
        .nb_lr = UUIDSET_INITIALIZER(&data->nb_lr),
        .nb_ls = UUIDSET_INITIALIZER(&data->nb_ls),
        .tracked = false,
        .trk_data.trk_created_parsed_routes =
            HMAPX_INITIALIZER(&data->trk_data.trk_created_parsed_routes),
        .trk_data.trk_deleted_parsed_routes =
            HMAPX_INITIALIZER(&data->trk_data.trk_deleted_parsed_routes),
    };

    return data;
}

static void
dynamic_routes_clear_tracked(struct dynamic_routes_data *data)
{
    hmapx_clear(&data->trk_data.trk_created_parsed_routes);
    struct hmapx_node *hmapx_node;
    HMAPX_FOR_EACH_SAFE (hmapx_node,
                         &data->trk_data.trk_deleted_parsed_routes) {
        parsed_route_free(hmapx_node->data);
        hmapx_delete(&data->trk_data.trk_deleted_parsed_routes, hmapx_node);
    }
    data->tracked = false;
}

static void
en_dynamic_routes_clear(struct dynamic_routes_data *data)
{
    struct ar_entry *ar;
    HMAP_FOR_EACH_POP (ar, hmap_node, &data->routes) {
        ar_entry_free(ar);
    }

    struct parsed_route *pr;
    HMAP_FOR_EACH_POP (pr, key_node, &data->parsed_routes) {
        parsed_route_free(pr);
    }

    dynamic_routes_clear_tracked(data);

    uuidset_clear(&data->nb_lr);
    uuidset_clear(&data->nb_ls);
}

static void
dynamic_routes_prepare_rebuild(struct dynamic_routes_data *data,
                               struct hmap *old_parsed_routes);

static void
dynamic_routes_diff_parsed(struct dynamic_routes_data *data,
                           struct hmap *old_parsed_routes);

/* Save current parsed_routes into *old_parsed_routes and reinitialise
 * data->parsed_routes for a full rebuild. */
static void
dynamic_routes_prepare_rebuild(struct dynamic_routes_data *data,
                               struct hmap *old_parsed_routes)
{
    dynamic_routes_clear_tracked(data);

    hmap_swap(old_parsed_routes, &data->parsed_routes);
    hmap_init(&data->parsed_routes);

    struct ar_entry *ar;
    HMAP_FOR_EACH_POP (ar, hmap_node, &data->routes) {
        ar_entry_free(ar);
    }
    uuidset_clear(&data->nb_lr);
    uuidset_clear(&data->nb_ls);
}

void
en_dynamic_routes_cleanup(void *data_)
{
    struct dynamic_routes_data *data = data_;

    en_dynamic_routes_clear(data);
    hmap_destroy(&data->routes);
    hmap_destroy(&data->parsed_routes);
    hmapx_destroy(&data->trk_data.trk_created_parsed_routes);
    hmapx_destroy(&data->trk_data.trk_deleted_parsed_routes);
    uuidset_destroy(&data->nb_lr);
    uuidset_destroy(&data->nb_ls);
}

enum engine_node_state
en_dynamic_routes_run(struct engine_node *node, void *data)
{
    struct dynamic_routes_data *dynamic_routes_data = data;
    struct northd_data *northd_data = engine_get_input_data("northd", node);
    struct ed_type_lr_stateful *lr_stateful_data =
        engine_get_input_data("lr_stateful", node);

    struct hmap old_parsed_routes = HMAP_INITIALIZER(&old_parsed_routes);
    dynamic_routes_prepare_rebuild(dynamic_routes_data, &old_parsed_routes);

    const struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, &northd_data->lr_datapaths.datapaths) {
        if (!od->dynamic_routing) {
            continue;
        }

        /* Track the LR datapath with dynamic-routing=true for any changes. */
        dynamic_routes_track_od(data, od);

        build_connected_as_host_routes(od, &northd_data->ls_ports,
                                       dynamic_routes_data);

        const struct lr_stateful_record *lr_stateful_rec =
            lr_stateful_table_find_by_uuid(&lr_stateful_data->table, od->key);
        if (!lr_stateful_rec) {
            continue;
        }

        build_nat_routes(od, lr_stateful_rec->lrnat_rec,
                         &northd_data->ls_ports,
                         &dynamic_routes_data->routes);
        build_nat_connected_routes(od, &lr_stateful_data->table,
                                   &northd_data->ls_ports,
                                   dynamic_routes_data);

        build_lb_routes(od, lr_stateful_rec->lb_ips,
                        &dynamic_routes_data->routes);
        build_lb_connected_routes(od, &northd_data->lb_datapaths_map,
                                  &northd_data->ls_ports,
                                  dynamic_routes_data);
    }

    dynamic_routes_diff_parsed(dynamic_routes_data, &old_parsed_routes);
    hmap_destroy(&old_parsed_routes);

    return EN_UPDATED;
}

static void
dynamic_routes_diff_parsed(struct dynamic_routes_data *data,
                           struct hmap *old_parsed_routes)
{
    struct parsed_route *pr;
    HMAP_FOR_EACH (pr, key_node, old_parsed_routes) {
        pr->stale = true;
    }

    HMAP_FOR_EACH_SAFE (pr, key_node, &data->parsed_routes) {
        size_t hash = parsed_route_hash(pr);
        struct parsed_route *old_pr = parsed_route_lookup(
            old_parsed_routes, hash, pr);
        if (old_pr) {
            old_pr->stale = false;
            /* Swap in the pre-existing route so that pointers held by
             * group_ecmp_route remain valid.  Detach from
             * old_parsed_routes first: hmap_node is intrusive and
             * cannot live in two maps. */
            hmap_remove(old_parsed_routes, &old_pr->key_node);
            hmap_remove(&data->parsed_routes, &pr->key_node);
            hmap_insert(&data->parsed_routes, &old_pr->key_node, hash);
            parsed_route_free(pr);
        } else {
            hmapx_add(&data->trk_data.trk_created_parsed_routes, pr);
        }
    }

    HMAP_FOR_EACH_SAFE (pr, key_node, old_parsed_routes) {
        if (pr->stale) {
            hmapx_add(&data->trk_data.trk_deleted_parsed_routes, pr);
        }
    }

    if (!hmapx_is_empty(&data->trk_data.trk_created_parsed_routes)
        || !hmapx_is_empty(&data->trk_data.trk_deleted_parsed_routes)) {
        data->tracked = true;
    }
}

enum engine_input_handler_result
dynamic_routes_lr_stateful_change_handler(struct engine_node *node,
                                          void *data_)
{
    /* We only actually use lr_stateful data if we expose individual host
     * routes. In this case we for now just recompute. */
    struct dynamic_routes_data *data = data_;
    struct ed_type_lr_stateful *lr_stateful_data =
        engine_get_input_data("lr_stateful", node);

    struct hmapx_node *hmapx_node;
    const struct lr_stateful_record *lr_stateful_rec;
    HMAPX_FOR_EACH (hmapx_node, &lr_stateful_data->trk_data.crupdated) {
        lr_stateful_rec = hmapx_node->data;
        if (uuidset_contains(&data->nb_lr, &lr_stateful_rec->nbr_uuid)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

enum engine_input_handler_result
dynamic_routes_northd_change_handler(struct engine_node *node, void *data_)
{
    struct dynamic_routes_data *data = data_;
    struct northd_data *northd_data = engine_get_input_data("northd", node);
    struct northd_tracked_data *trk_data = &northd_data->trk_data;
    if (!northd_has_tracked_data(trk_data)) {
        return EN_UNHANDLED;
    }

    struct hmapx_node *hmapx_node;
    struct ovn_datapath *od;
    HMAPX_FOR_EACH (hmapx_node, &trk_data->trk_routers.crupdated) {
        od = hmapx_node->data;
        if (uuidset_contains(&data->nb_lr, &od->key)) {
            return EN_UNHANDLED;
        }
    }

    HMAPX_FOR_EACH (hmapx_node, &trk_data->trk_routers.deleted) {
        od = hmapx_node->data;
        if (uuidset_contains(&data->nb_lr, &od->key)) {
            return EN_UNHANDLED;
        }
    }

    const struct ovn_port *op;
    HMAPX_FOR_EACH (hmapx_node, &trk_data->trk_lsps.created) {
        op = hmapx_node->data;
        if (uuidset_contains(&data->nb_ls, &op->od->key)) {
            return EN_UNHANDLED;
        }
    }

    HMAPX_FOR_EACH (hmapx_node, &trk_data->trk_lsps.updated) {
        op = hmapx_node->data;
        if (uuidset_contains(&data->nb_ls, &op->od->key)) {
            return EN_UNHANDLED;
        }
    }

    HMAPX_FOR_EACH (hmapx_node, &trk_data->trk_lsps.deleted) {
        op = hmapx_node->data;
        if (uuidset_contains(&data->nb_ls, &op->od->key)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static bool
should_advertise_route(const struct ovn_datapath *advertising_od,
                       const struct ovn_port *advertising_op,
                       enum route_source source)
{
    if (!advertising_od->dynamic_routing) {
        return false;
    }

    enum dynamic_routing_redistribute_mode drr =
        advertising_op->dynamic_routing_redistribute;

    switch (source) {
    case ROUTE_SOURCE_CONNECTED:
        /* The connected routes will be represented as host routes (/32 for
         * IPv4 and /128 for IPv6) when connected-as-host is configured. */
        return drr_mode_CONNECTED_is_set(drr) &&
               !drr_mode_CONNECTED_AS_HOST_is_set(drr);
    case ROUTE_SOURCE_STATIC:
        return drr_mode_STATIC_is_set(drr);
    case ROUTE_SOURCE_NAT:
        return drr_mode_NAT_is_set(drr);
    case ROUTE_SOURCE_LB:
        return drr_mode_LB_is_set(drr);
    case ROUTE_SOURCE_CONNECTED_AS_HOST:
        return drr_mode_CONNECTED_AS_HOST_is_set(drr);
    case ROUTE_SOURCE_IC_DYNAMIC:
        return drr_mode_IC_DYNAMIC_is_set(drr);
    case ROUTE_SOURCE_LEARNED:
        OVS_NOT_REACHED();
    default:
        OVS_NOT_REACHED();
    }
}

static void
advertised_route_table_sync(
    struct ovsdb_idl_txn *ovnsb_txn,
    const struct sbrec_advertised_route_table *sbrec_advertised_route_table,
    const struct hmap *routes,
    const struct hmap *dynamic_routes)
{
    struct hmap sync_routes = HMAP_INITIALIZER(&sync_routes);

    /* First build the set of non-dynamic routes that need sync-ing. */
    const struct parsed_route *route;
    HMAP_FOR_EACH (route, key_node, routes) {
        if (route->is_discard_route) {
            continue;
        }

        if (!route->dynamic_routing_advertise) {
            continue;
        }

        if (!should_advertise_route(route->od, route->out_port,
                                    route->source)) {
            continue;
        }

        if (prefix_is_link_local(&route->prefix, route->plen)) {
            continue;
        }

        const struct sbrec_port_binding *tracked_port =
            route->tracked_port ? route->tracked_port->sb : NULL;
        char *ip_prefix = normalize_v46_prefix(&route->prefix, route->plen);
 /* Parsed routes (static, connected, NAT) carry no per-backend
         * service selector, so pass NULL/0/NULL to compare against
         * existing sync entries on the (dp, lp, prefix, tracked_port)
         * portion of the key with empty selectors. */
        if (ar_entry_find(&sync_routes, route->od->sdp->sb_dp,
                          route->out_port->sb, ip_prefix,
                          tracked_port, NULL, false, 0, NULL)) {
            free(ip_prefix);
            continue;
        }
        ar_entry_add_nocopy(&sync_routes, route->od, route->out_port,
                            ip_prefix,
                            route->tracked_port,
                            route->source);
    }

    /* Then add the set of dynamic routes that need sync-ing. The SB
     * unique index includes the selector columns, so two rows with
     * the same VIP and backend LSP but different selectors are
     * distinct entries and both must land in sync_routes. */
    struct ar_entry *route_e;
    HMAP_FOR_EACH (route_e, hmap_node, dynamic_routes) {
        if (!should_advertise_route(route_e->od, route_e->op,
                                    route_e->source)) {
            continue;
        }

        const struct sbrec_port_binding *tracked_pb =
            route_e->tracked_port ? route_e->tracked_port->sb : NULL;
        if (ar_entry_find(&sync_routes, route_e->od->sdp->sb_dp,
                          route_e->op->sb,
                          route_e->ip_prefix, tracked_pb,
                          route_e->tracked_service_ip,
                          route_e->has_tracked_service_port,
                          route_e->tracked_service_port,
                          route_e->tracked_service_protocol)) {
            /* Exact duplicate of an entry already in sync_routes (e.g.
             * the snat/connected-as-host overlap, or two LB
             * configurations describing the same backend service).
             * Skip the redundant insert. */
            continue;
        }
        struct ar_entry *sync_e =
            ar_entry_add(&sync_routes, route_e->od, route_e->op,
                         route_e->ip_prefix, route_e->tracked_port,
                         route_e->source);
        /* Preserve the per-backend service selector across the copy
         * into sync_routes. build_lb_lr_routes sets it on route_e but
         * ar_entry_add starts the new sync_e with NULL/zero fields. */
        ar_entry_copy_service_selector(sync_e, route_e);
    }

    const struct sbrec_advertised_route *sb_route;
    SBREC_ADVERTISED_ROUTE_TABLE_FOR_EACH_SAFE (sb_route,
                                                sbrec_advertised_route_table) {
        bool have_port = sb_route->n_tracked_service_port > 0;
        int64_t sb_port = have_port ? sb_route->tracked_service_port[0] : 0;
        route_e = ar_entry_find(&sync_routes, sb_route->datapath,
                                sb_route->logical_port, sb_route->ip_prefix,
                                sb_route->tracked_port,
                                sb_route->tracked_service_ip,
                                have_port, sb_port,
                                sb_route->tracked_service_protocol);
        if (!route_e) {
            /* No matching entry in the to-emit set: the LB,
             * its backends, or the selector drifted. The
             * replacement row (if any) will be inserted below. */
            sbrec_advertised_route_delete(sb_route);
            continue;
        }
        /* Full-key match: nothing to update. */
        hmap_remove(&sync_routes, &route_e->hmap_node);
        ar_entry_free(route_e);
    }

    HMAP_FOR_EACH_POP (route_e, hmap_node, &sync_routes) {
        const struct sbrec_advertised_route *sr =
            sbrec_advertised_route_insert(ovnsb_txn);
        sbrec_advertised_route_set_datapath(sr, route_e->od->sdp->sb_dp);
        sbrec_advertised_route_set_logical_port(sr, route_e->op->sb);
        sbrec_advertised_route_set_ip_prefix(sr, route_e->ip_prefix);
        if (route_e->tracked_port && route_e->tracked_port->sb) {
            sbrec_advertised_route_set_tracked_port(sr,
                                                    route_e->tracked_port->sb);
        }
        if (route_e->tracked_service_ip) {
            sbrec_advertised_route_set_tracked_service_ip(
                sr, route_e->tracked_service_ip);
        }
        if (route_e->has_tracked_service_port) {
            int64_t port = route_e->tracked_service_port;
            sbrec_advertised_route_set_tracked_service_port(sr, &port, 1);
        }
        if (route_e->tracked_service_protocol) {
            sbrec_advertised_route_set_tracked_service_protocol(
                sr, route_e->tracked_service_protocol);
        }
        ar_entry_free(route_e);
    }

    hmap_destroy(&sync_routes);
}

struct advertised_mac_binding {
    struct hmap_node hmap_node;

    const struct sbrec_datapath_binding *dp;
    const struct sbrec_port_binding *sb;

    char *ip;
    char *mac;
};

static bool
evpn_ip_redistribution_enabled(const struct ovn_datapath *od)
{
    if (!od->has_evpn_vni) {
        return false;
    }

    enum neigh_redistribute_mode mode =
        parse_neigh_dynamic_redistribute(&od->nbs->other_config);
    return nrm_mode_IP_is_set(mode);
}

static uint32_t
advertised_mac_binding_get_hash(const struct sbrec_datapath_binding *dp,
                                const struct sbrec_port_binding *sb,
                                const char *ip, const char *mac)
{
    uint32_t hash = uuid_hash(&dp->header_.uuid);
    hash = hash_string(sb->logical_port, hash);
    hash = hash_string(ip, hash);
    hash = hash_string(mac, hash);

    return hash;
}

static struct advertised_mac_binding *
advertised_mac_binding_entry_find(struct hmap *map,
                                  const struct sbrec_datapath_binding *dp,
                                  const struct sbrec_port_binding *sb,
                                  const char *ip, const char *mac)
{
    uint32_t hash = advertised_mac_binding_get_hash(dp, sb, ip, mac);
    struct advertised_mac_binding *e;
    HMAP_FOR_EACH_WITH_HASH (e, hmap_node, hash, map) {
        if (uuid_equals(&sb->header_.uuid, &e->sb->header_.uuid) &&
            uuid_equals(&dp->header_.uuid, &e->dp->header_.uuid) &&
            !strcmp(e->ip, ip) && !strcmp(e->mac, mac)) {
            return e;
        }
    }

    return NULL;
}

static void
advertised_mac_binding_entry_add(struct hmap *map,
                                 const struct sbrec_datapath_binding *dp,
                                 const struct sbrec_port_binding *sb,
                                 const char *ip, const char *mac)
{
    struct advertised_mac_binding *e = xmalloc(sizeof *e);
    e->ip = xstrdup(ip);
    e->mac = xstrdup(mac);
    e->sb = sb;
    e->dp = dp;

    uint32_t hash = advertised_mac_binding_get_hash(dp, sb, ip, mac);
    hmap_insert(map, &e->hmap_node, hash);
}

static void
advertised_mac_binding_entry_destroy(struct advertised_mac_binding *e)
{
    free(e->ip);
    free(e->mac);
    free(e);
}

static void
advertised_mac_binding_add(struct hmap *map,
                           const struct sbrec_datapath_binding *dp,
                           const struct sbrec_port_binding *sb,
                           struct lport_addresses *addr)
{
    if (!addr) {
        return;
    }

    for (size_t i = 0; i < addr->n_ipv4_addrs; i++) {
        if (!advertised_mac_binding_entry_find(map, dp, sb,
                                               addr->ipv4_addrs[i].addr_s,
                                               addr->ea_s)) {
            advertised_mac_binding_entry_add(map, dp, sb,
                                             addr->ipv4_addrs[i].addr_s,
                                             addr->ea_s);
        }
    }

    for (size_t i = 0; i < addr->n_ipv6_addrs; i++) {
        if (prefix_is_link_local(&addr->ipv6_addrs[i].addr, 128)) {
            continue;
        }

        if (!advertised_mac_binding_entry_find(map, dp, sb,
                                               addr->ipv6_addrs[i].addr_s,
                                               addr->ea_s)) {
            advertised_mac_binding_entry_add(map, dp, sb,
                                             addr->ipv6_addrs[i].addr_s,
                                             addr->ea_s);
        }
    }
}

static void
build_advertised_mac_binding(const struct ovn_datapath *od, struct hmap *map)
{
    ovs_assert(od->nbs);

    if (!evpn_ip_redistribution_enabled(od)) {
        return;
    }

    struct ovn_port *op;
    HMAP_FOR_EACH (op, dp_node, &od->ports) {
        if (!op->sb) {
            continue;
        }

        if (!smap_get_bool(&op->nbsp->options,
                           "dynamic-routing-advertise", true)) {
            continue;
        }

        if (lsp_is_router(op->nbsp) && op->peer) {
            advertised_mac_binding_add(map, od->sdp->sb_dp, op->sb,
                                       &op->peer->lrp_networks);
        }

        if (!strcmp(op->nbsp->type, "")) { /* LSP */
            advertised_mac_binding_add(map, od->sdp->sb_dp, op->sb,
                                       op->lsp_addrs);
        }
    }
}

void *
en_advertised_mac_binding_sync_init(struct engine_node *node OVS_UNUSED,
                                    struct engine_arg *arg OVS_UNUSED)
{
    return NULL;
}

enum engine_node_state
en_advertised_mac_binding_sync_run(struct engine_node *node,
                                   void *data OVS_UNUSED)
{
    struct northd_data *northd_data = engine_get_input_data("northd", node);
    const struct sbrec_advertised_mac_binding_table *sbrec_adv_mb_table =
        EN_OVSDB_GET(engine_get_input("SB_advertised_mac_binding", node));
    const struct engine_context *eng_ctx = engine_get_context();

    struct hmap advertised_mac_binding_map =
        HMAP_INITIALIZER(&advertised_mac_binding_map);

    struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, &northd_data->ls_datapaths.datapaths) {
        build_advertised_mac_binding(od, &advertised_mac_binding_map);
    }

    struct advertised_mac_binding *e;
    const struct sbrec_advertised_mac_binding *sb_adv_mb;
    SBREC_ADVERTISED_MAC_BINDING_TABLE_FOR_EACH_SAFE (sb_adv_mb,
                                          sbrec_adv_mb_table) {
        e = advertised_mac_binding_entry_find(&advertised_mac_binding_map,
                                              sb_adv_mb->datapath,
                                              sb_adv_mb->logical_port,
                                              sb_adv_mb->ip, sb_adv_mb->mac);
        if (!e) {
            sbrec_advertised_mac_binding_delete(sb_adv_mb);
        } else {
            hmap_remove(&advertised_mac_binding_map, &e->hmap_node);
            advertised_mac_binding_entry_destroy(e);
        }
    }

    HMAP_FOR_EACH_POP (e, hmap_node, &advertised_mac_binding_map) {
        sb_adv_mb =
            sbrec_advertised_mac_binding_insert(eng_ctx->ovnsb_idl_txn);
        sbrec_advertised_mac_binding_set_datapath(sb_adv_mb, e->sb->datapath);
        sbrec_advertised_mac_binding_set_logical_port(sb_adv_mb, e->sb);
        sbrec_advertised_mac_binding_set_ip(sb_adv_mb, e->ip);
        sbrec_advertised_mac_binding_set_mac(sb_adv_mb, e->mac);
        advertised_mac_binding_entry_destroy(e);
    }

    hmap_destroy(&advertised_mac_binding_map);

    return EN_UPDATED;
}

void
en_advertised_mac_binding_sync_cleanup(void *data OVS_UNUSED)
{
}

enum engine_input_handler_result
northd_output_advertised_mac_binding_sync_handler(
    struct engine_node *node OVS_UNUSED, void *data OVS_UNUSED)
{
    return EN_HANDLED_UPDATED;
}
