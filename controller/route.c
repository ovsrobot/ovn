/*
 * Copyright (c) 2025, Canonical, Ltd.
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

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "vswitch-idl.h"
#include "openvswitch/hmap.h"
#include "openvswitch/vlog.h"
#include "openvswitch/ofp-parse.h"

#include "lib/ovn-sb-idl.h"
#include "lib/ovn-util.h"

#include "binding.h"
#include "ha-chassis.h"
#include "local_data.h"
#include "route.h"

#include "route-table.h"

VLOG_DEFINE_THIS_MODULE(exchange);

#define PRIORITY_DEFAULT 1000
#define PRIORITY_LOCAL_BOUND 100

struct sm_lb_key {
    struct hmap_node node;
    const char *logical_port;
    const char *chassis_name;
    const char *ip;
    int64_t port;
    const char *protocol;
    bool online;
};

/* Per-candidate state for an LB-derived Advertised_Route that is
 * eligible for Service_Monitor gating.  Keyed by vip_ip so the LB
 * table scan can update matching candidates without parsing backends
 * for irrelevant VIPs. */
struct lb_route_gate {
    struct hmap_node node;
    const struct sbrec_advertised_route *route;
    char *vip_ip;
    const char *tracked_lp;
    bool seen_monitor;
    bool any_online;
};

/* Extract the VIP IP string from a route ip_prefix (e.g. "172.16.1.20"
 * from prefix/plen).  Writes the result to buf and returns buf on
 * success, or NULL on failure. */
static const char *
route_prefix_to_ip_str(const struct in6_addr *prefix, char *buf, size_t buflen)
{
    if (IN6_IS_ADDR_V4MAPPED(prefix)) {
        if (inet_ntop(AF_INET, &prefix->s6_addr[12], buf, buflen)) {
            return buf;
        }
    } else {
        if (inet_ntop(AF_INET6, &prefix->s6_addr, buf, buflen)) {
            return buf;
        }
    }
    return NULL;
}

/* First pass: scan Advertised_Route rows and build a gate entry for
 * each LB-derived route whose tracked_port is local.  The returned
 * hmap is keyed by vip_ip so the LB table scan can update matching
 * candidates without parsing backends for irrelevant VIPs.
 *
 * If no gate candidates are found the hmap is empty and the caller
 * can skip the SM index build and LB table scan entirely. */
static void
build_lb_route_gates(struct hmap *gates,
                     const struct sbrec_advertised_route_table *ar_table,
                     struct ovsdb_idl_index *sbrec_port_binding_by_name,
                     const struct sbrec_chassis *chassis,
                     struct hmap *announce_routes)
{
    const struct sbrec_advertised_route *route;
    SBREC_ADVERTISED_ROUTE_TABLE_FOR_EACH (route, ar_table) {
        const char *source = smap_get(&route->external_ids, "source");
        if (!source || strcmp(source, "lb")) {
            continue;
        }
        if (!route->tracked_port) {
            continue;
        }
        if (!lport_is_local(sbrec_port_binding_by_name, chassis,
                            route->tracked_port->logical_port)) {
            continue;
        }
        struct advertise_datapath_entry *ad =
            advertise_datapath_find(announce_routes, route->datapath);
        if (!ad) {
            continue;
        }

        struct in6_addr prefix;
        unsigned int plen;
        if (!ip46_parse_cidr(route->ip_prefix, &prefix, &plen)) {
            continue;
        }
        char vip_ip_buf[INET6_ADDRSTRLEN];
        const char *vip_ip = route_prefix_to_ip_str(&prefix, vip_ip_buf,
                                                     sizeof vip_ip_buf);
        if (!vip_ip) {
            continue;
        }

        struct lb_route_gate *g = xmalloc(sizeof *g);
        *g = (struct lb_route_gate) {
            .route = route,
            .vip_ip = xstrdup(vip_ip),
            .tracked_lp = route->tracked_port->logical_port,
            .seen_monitor = false,
            .any_online = false,
        };
        hmap_insert(gates, &g->node, hash_string(g->vip_ip, 0));
    }
}

static void
destroy_lb_route_gates(struct hmap *gates)
{
    struct lb_route_gate *g;
    HMAP_FOR_EACH_POP (g, node, gates) {
        free(g->vip_ip);
        free(g);
    }
    hmap_destroy(gates);
}

/* Scan LB VIPs but parse backend strings only for VIP IPs that
 * appear in the gate set.  For each matching backend, look up the
 * SM index and update the gate's health state. */
static void
evaluate_lb_route_gates(struct hmap *gates,
                        const struct hmap *sm_lb_index,
                        const struct sbrec_load_balancer_table *lb_table,
                        const char *chassis_name)
{
    const struct sbrec_load_balancer *lb;
    SBREC_LOAD_BALANCER_TABLE_FOR_EACH (lb, lb_table) {
        const char *protocol = lb->protocol ? lb->protocol : "tcp";
        struct smap_node *node;
        SMAP_FOR_EACH (node, &lb->vips) {
            char *vip_ip = NULL;
            struct in6_addr vip_addr;
            uint16_t vip_port;
            int vip_af;
            if (!ip_address_and_port_from_lb_key(node->key, &vip_ip,
                                                 &vip_addr, &vip_port,
                                                 &vip_af)) {
                continue;
            }

            uint32_t hash = hash_string(vip_ip, 0);
            bool needed = false;
            struct lb_route_gate *g;
            HMAP_FOR_EACH_WITH_HASH (g, node, hash, gates) {
                if (!strcmp(g->vip_ip, vip_ip)) {
                    needed = true;
                    break;
                }
            }
            if (!needed) {
                free(vip_ip);
                continue;
            }

            char *vips_copy = xstrdup(node->value);
            char *saveptr = NULL;
            for (char *tok = strtok_r(vips_copy, ",", &saveptr);
                 tok; tok = strtok_r(NULL, ",", &saveptr)) {
                char *backend_ip = NULL;
                struct in6_addr backend_addr;
                uint16_t backend_port = 0;
                int backend_af;
                if (!ip_address_and_port_from_lb_key(
                        tok, &backend_ip, &backend_addr,
                        &backend_port, &backend_af)) {
                    free(backend_ip);
                    continue;
                }

                HMAP_FOR_EACH_WITH_HASH (g, node, hash, gates) {
                    if (strcmp(g->vip_ip, vip_ip)) {
                        continue;
                    }

                    uint32_t sm_hash = hash_string(g->tracked_lp, 0);
                    sm_hash = hash_string(chassis_name, sm_hash);
                    sm_hash = hash_string(backend_ip, sm_hash);
                    sm_hash = hash_int((uint32_t) backend_port, sm_hash);
                    sm_hash = hash_string(protocol, sm_hash);

                    struct sm_lb_key *k;
                    HMAP_FOR_EACH_WITH_HASH (k, node, sm_hash,
                                             sm_lb_index) {
                        if (k->port != backend_port ||
                            strcmp(k->logical_port, g->tracked_lp) ||
                            strcmp(k->chassis_name, chassis_name) ||
                            strcmp(k->ip, backend_ip) ||
                            strcmp(k->protocol, protocol)) {
                            continue;
                        }
                        g->seen_monitor = true;
                        g->any_online |= k->online;
                    }
                }
                free(backend_ip);
            }
            free(vips_copy);
            free(vip_ip);
        }
    }
}

/* Look up the gate decision for a specific route. Returns:
 *  -1 if no gate exists (route is not LB-derived or not local-bound)
 *   0 if gate says withdraw (seen_monitor && !any_online)
 *   1 if gate says install */
static int
lb_route_gate_decision(const struct hmap *gates,
                       const struct sbrec_advertised_route *route)
{
    const struct lb_route_gate *g;
    HMAP_FOR_EACH (g, node, gates) {
        if (g->route == route) {
            if (g->seen_monitor && !g->any_online) {
                return 0;
            }
            return 1;
        }
    }
    return -1;
}

static bool
route_exchange_relevant_port(const struct sbrec_port_binding *pb)
{
    return pb && smap_get_bool(&pb->options, "dynamic-routing", false);
}

uint32_t
advertise_route_hash(const struct in6_addr *dst,
                     const struct in6_addr *nexthop, unsigned int plen)
{
    uint32_t hash = hash_add_in6_addr(0, dst);
    hash = hash_add_in6_addr(hash, nexthop);
    return hash_int(plen, hash);
}

struct advertise_route_entry
advertise_route_from_route_data(const struct route_data *rd)
{
    struct advertise_route_entry re = (struct advertise_route_entry) {
        .addr = rd->rta_dst,
        .plen = rd->rtm_dst_len,
        .priority = rd->rta_priority,
    };

    if (!ovs_list_is_empty(&rd->nexthops)) {
        const struct route_data_nexthop *rdnh =
            CONTAINER_OF(ovs_list_front(&rd->nexthops),
                         const struct route_data_nexthop, nexthop_node);
        re.nexthop = rdnh->addr;
    }

    return re;
}

const struct sbrec_port_binding*
route_exchange_find_port(struct ovsdb_idl_index *sbrec_port_binding_by_name,
                         const struct sbrec_chassis *chassis,
                         const struct sbrec_port_binding *pb,
                         const char **dynamic_routing_port_name)
{
    if (dynamic_routing_port_name) {
        *dynamic_routing_port_name = NULL;
    }

    if (!pb) {
        return NULL;
    }
    if (route_exchange_relevant_port(pb)) {
        if (dynamic_routing_port_name) {
            *dynamic_routing_port_name =
                smap_get(&pb->options, "dynamic-routing-port-name");
        }
        return pb;
    }

    const struct sbrec_port_binding *cr_pb =
        lport_get_cr_port(sbrec_port_binding_by_name, pb, NULL);

    if (!cr_pb) {
        return NULL;
    }

    if (dynamic_routing_port_name) {
        *dynamic_routing_port_name =
            smap_get(&cr_pb->options, "dynamic-routing-port-name");
    }

    if (!lport_pb_is_chassis_resident(chassis, cr_pb)) {
        return NULL;
    }

    if (route_exchange_relevant_port(cr_pb)) {
        return cr_pb;
    }
    return NULL;
}

struct advertise_datapath_entry *
advertise_datapath_find(const struct hmap *datapaths,
                        const struct sbrec_datapath_binding *db)
{
    struct advertise_datapath_entry *ade;
    HMAP_FOR_EACH_WITH_HASH (ade, node, db->tunnel_key, datapaths) {
        if (ade->db == db) {
            return ade;
        }
    }
    return NULL;
}

static void
build_port_mapping(struct smap *mapping, const char *port_mapping)
{
    if (!port_mapping) {
        return;
    }

    char *tokstr, *orig, *key, *value;

    orig = tokstr = xstrdup(port_mapping);
    while (ofputil_parse_key_value(&tokstr, &key, &value)) {
        if (!*value) {
          static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
          VLOG_WARN_RL(&rl, "dynamic-routing-port-mapping setting '%s' is "
                            "not valid.", tokstr);
          break;
        }
        smap_add(mapping, key, value);
    }
    free(orig);
}

static const char *
ifname_from_port_name(const struct smap *port_mapping,
                      struct shash *local_bindings,
                      const struct sbrec_chassis *chassis,
                      const char *port_name)
{
    const char *iface = smap_get(port_mapping, port_name);
    if (iface) {
        return iface;
    }

    const struct binding_lport *b_lport =
        local_binding_get_primary_lport(local_binding_find(local_bindings,
                                                           port_name));

    if (!b_lport || !lport_pb_is_chassis_resident(chassis, b_lport->pb)) {
        return NULL;
    }

    return b_lport->lbinding->iface->name;
}

static void
advertise_datapath_cleanup(struct advertise_datapath_entry *ad)
{
    struct advertise_route_entry *ar;
    HMAP_FOR_EACH_SAFE (ar, node, &ad->routes) {
        hmap_remove(&ad->routes, &ar->node);
        free(ar);
    }
    hmap_destroy(&ad->routes);
    smap_destroy(&ad->bound_ports);
    free(ad);
}

static struct advertise_datapath_entry *
advertised_datapath_alloc(const struct sbrec_datapath_binding *datapath)
{
    struct advertise_datapath_entry *ad = xmalloc(sizeof *ad);
    *ad = (struct advertise_datapath_entry) {
        .db = datapath,
        .routes = HMAP_INITIALIZER(&ad->routes),
        .bound_ports = SMAP_INITIALIZER(&ad->bound_ports),
    };

    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 10);

    const char *nh4 =
        smap_get(&datapath->external_ids, "dynamic-routing-v4-prefix-nexthop");
    if (nh4 && !ip46_parse(nh4, &ad->ipv4_nexthop)) {
        memset(&ad->ipv4_nexthop, 0, sizeof ad->ipv4_nexthop);
        VLOG_WARN_RL(&rl, "Couldn't parse IPv4 prefix nexthop %s, "
                     "routes will be installed as blackhole.", nh4);
    }

    const char *nh6 =
        smap_get(&datapath->external_ids, "dynamic-routing-v6-prefix-nexthop");
    if (nh6 && !ipv6_parse(nh6, &ad->ipv6_nexthop)) {
        memset(&ad->ipv6_nexthop, 0, sizeof ad->ipv6_nexthop);
        VLOG_WARN_RL(&rl, "Couldn't parse IPv6 prefix nexthop %s, "
                     "routes will be installed as blackhole.", nh6);
    }

    return ad;
}

static void
route_record_status(struct route_ctx_out *r_ctx_out,
                    const struct sbrec_advertised_route *route,
                    const char *status)
{
    struct advertised_route_status *s = xmalloc(sizeof *s);
    *s = (struct advertised_route_status) {
        .route = route,
        .status = status,
    };
    hmap_insert(r_ctx_out->advertised_route_status, &s->node,
                hash_pointer(route, 0));
}

void
advertised_route_status_clear(struct hmap *statuses)
{
    struct advertised_route_status *s;
    HMAP_FOR_EACH_POP (s, node, statuses) {
        free(s);
    }
}

void
route_run(struct route_ctx_in *r_ctx_in,
          struct route_ctx_out *r_ctx_out)
{
    const struct local_datapath *ld;
    struct smap port_mapping = SMAP_INITIALIZER(&port_mapping);

    build_port_mapping(&port_mapping, r_ctx_in->dynamic_routing_port_mapping);

    HMAP_FOR_EACH (ld, hmap_node, r_ctx_in->local_datapaths) {
        if (vector_is_empty(&ld->peer_ports) || ld->is_switch) {
            continue;
        }
        struct advertise_datapath_entry *ad = NULL;
        bool lr_has_port_name_filter = false;

        /* This is a LR datapath, find LRPs with route exchange options
         * that are bound locally. */
        const struct peer_ports *peers;
        VECTOR_FOR_EACH_PTR (&ld->peer_ports, peers) {
            const struct sbrec_port_binding *local_peer = peers->local;
            const char *port_name;

            const struct sbrec_port_binding *repb =
                route_exchange_find_port(r_ctx_in->sbrec_port_binding_by_name,
                                         r_ctx_in->chassis,
                                         local_peer, &port_name);
            if (port_name) {
                lr_has_port_name_filter = true;
            }
            if (!repb) {
                continue;
            }

            if (!ad) {
                ad = advertised_datapath_alloc(ld->datapath);
            }

            ad->maintain_vrf |=
                smap_get_bool(&repb->options,
                              "dynamic-routing-maintain-vrf",
                              false);

            const char *vrf_name = smap_get(&repb->options,
                                            "dynamic-routing-vrf-name");
            if (vrf_name && strlen(vrf_name) >= IFNAMSIZ) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);
                VLOG_WARN_RL(&rl, "Ignoring vrf name %s, since it is too long."
                             "Maximum length is %d characters", vrf_name,
                             IFNAMSIZ);
                vrf_name = NULL;
            }
            if (vrf_name) {
                memcpy(ad->vrf_name, vrf_name, strlen(vrf_name) + 1);
            } else {
                snprintf(ad->vrf_name, sizeof ad->vrf_name, "ovnvrf%"PRIu32,
                         route_get_table_id(ad->db));
            }

            if (!port_name) {
                /* No port-name set, so we learn routes from all ports. */
                smap_add_nocopy(&ad->bound_ports,
                                xstrdup(local_peer->logical_port), NULL);
            } else {
                /* If a port_name is set the we filter for the name as set in
                 * the port-mapping or the interface name of the local
                 * binding. If the port is not in the port_mappings and not
                 * bound locally we will not learn routes for this port. */
                const char *ifname = ifname_from_port_name(
                    &port_mapping, r_ctx_in->local_bindings,
                    r_ctx_in->chassis, port_name);
                if (ifname) {
                    smap_add(&ad->bound_ports, local_peer->logical_port,
                             ifname);
                }
                sset_add(r_ctx_out->filtered_ports, port_name);
            }
        }

        if (ad) {
            /* If at least one bound port has dynamic-routing-port-name
             * configured, ignore the ones that don't. */
            if (lr_has_port_name_filter) {
                struct smap_node *node;

                SMAP_FOR_EACH_SAFE (node, &ad->bound_ports) {
                    if (!node->value) {
                        smap_remove_node(&ad->bound_ports, node);
                    }
                }
            }

            tracked_datapath_add(ld->datapath, TRACKED_RESOURCE_NEW,
                                 r_ctx_out->tracked_re_datapaths);
            hmap_insert(r_ctx_out->announce_routes, &ad->node,
                        ad->db->tunnel_key);
        }
    }

    struct hmap lb_route_gates = HMAP_INITIALIZER(&lb_route_gates);
    build_lb_route_gates(&lb_route_gates,
                         r_ctx_in->advertised_route_table,
                         r_ctx_in->sbrec_port_binding_by_name,
                         r_ctx_in->chassis,
                         r_ctx_out->announce_routes);

    struct hmap sm_lb_index = HMAP_INITIALIZER(&sm_lb_index);
    if (!hmap_is_empty(&lb_route_gates) && r_ctx_in->service_monitor_table) {
        const struct sbrec_service_monitor *sm;
        SBREC_SERVICE_MONITOR_TABLE_FOR_EACH (
            sm, r_ctx_in->service_monitor_table) {
            if (!sm->type || strcmp(sm->type, "load-balancer")) {
                continue;
            }
            if (!sm->logical_port || !sm->chassis_name ||
                !sm->ip || !sm->protocol) {
                continue;
            }
            struct sm_lb_key *k = xmalloc(sizeof *k);
            uint32_t hash = hash_string(sm->logical_port, 0);
            hash = hash_string(sm->chassis_name, hash);
            hash = hash_string(sm->ip, hash);
            hash = hash_int((uint32_t) sm->port, hash);
            hash = hash_string(sm->protocol, hash);
            *k = (struct sm_lb_key) {
                .logical_port = sm->logical_port,
                .chassis_name = sm->chassis_name,
                .ip = sm->ip,
                .port = sm->port,
                .protocol = sm->protocol,
                .online = sm->status && !strcmp(sm->status, "online"),
            };
            hmap_insert(&sm_lb_index, &k->node, hash);
        }

        if (!hmap_is_empty(&sm_lb_index) &&
            r_ctx_in->load_balancer_table) {
            evaluate_lb_route_gates(&lb_route_gates, &sm_lb_index,
                                    r_ctx_in->load_balancer_table,
                                    r_ctx_in->chassis->name);
        }
    }

    const struct sbrec_advertised_route *route;
    SBREC_ADVERTISED_ROUTE_TABLE_FOR_EACH (route,
                                           r_ctx_in->advertised_route_table) {
        struct advertise_datapath_entry *ad =
            advertise_datapath_find(r_ctx_out->announce_routes,
                                    route->datapath);
        if (!ad) {
            continue;
        }

        struct in6_addr prefix;
        unsigned int plen;
        if (!ip46_parse_cidr(route->ip_prefix, &prefix, &plen)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);
            VLOG_WARN_RL(&rl, "bad 'ip_prefix' %s in route "
                         UUID_FMT, route->ip_prefix,
                         UUID_ARGS(&route->header_.uuid));
            continue;
        }

        if (!lport_is_local(r_ctx_in->sbrec_port_binding_by_name,
                            r_ctx_in->chassis,
                            route->logical_port->logical_port)) {
            sset_add(r_ctx_out->tracked_ports_remote,
                     route->logical_port->logical_port);
            continue;
        }
        sset_add(r_ctx_out->tracked_ports_local,
                 route->logical_port->logical_port);

        if (!smap_get_bool(&route->external_ids, "enabled", true)) {
            if (route->tracked_port) {
                if (lport_is_local(r_ctx_in->sbrec_port_binding_by_name,
                                   r_ctx_in->chassis,
                                   route->tracked_port->logical_port)) {
                    route_record_status(r_ctx_out, route,
                                        "withdrawn-admin");
                }
            }
            continue;
        }

        unsigned int priority = PRIORITY_DEFAULT;
        if (route->tracked_port) {
            bool redistribute_local_bound_only =
                smap_get_bool(&route->logical_port->options,
                              "dynamic-routing-redistribute-local-only",
                              false);
            if (lport_is_local(r_ctx_in->sbrec_port_binding_by_name,
                               r_ctx_in->chassis,
                               route->tracked_port->logical_port)) {
                priority = PRIORITY_LOCAL_BOUND;
                sset_add(r_ctx_out->tracked_ports_local,
                         route->tracked_port->logical_port);

                int gate = lb_route_gate_decision(&lb_route_gates, route);
                if (gate == 0) {
                    route_record_status(r_ctx_out, route,
                                        "withdrawn-monitor");
                    continue;
                }
                route_record_status(r_ctx_out, route, "advertised");
            } else {
                sset_add(r_ctx_out->tracked_ports_remote,
                         route->tracked_port->logical_port);
                if (redistribute_local_bound_only) {
                    continue;
                }
            }
        }

        struct in6_addr nexthop = IN6_IS_ADDR_V4MAPPED(&prefix)
                ? ad->ipv4_nexthop : ad->ipv6_nexthop;
        if (advertise_route_find(priority, &prefix, plen, &nexthop,
                                 &ad->routes)) {
            continue;
        }

        struct advertise_route_entry *ar = xmalloc(sizeof(*ar));
        *ar = (struct advertise_route_entry) {
            .addr = prefix,
            .plen = plen,
            .priority = priority,
            .nexthop = nexthop,
        };
        hmap_insert(&ad->routes, &ar->node,
                    advertise_route_hash(&ar->addr, &ar->nexthop, plen));
    }

    struct sm_lb_key *k;
    HMAP_FOR_EACH_POP (k, node, &sm_lb_index) {
        free(k);
    }
    hmap_destroy(&sm_lb_index);
    destroy_lb_route_gates(&lb_route_gates);

    smap_destroy(&port_mapping);
}

void
route_cleanup(struct hmap *announce_routes)
{
    struct advertise_datapath_entry *ad;
    HMAP_FOR_EACH_POP (ad, node, announce_routes) {
        advertise_datapath_cleanup(ad);
    }
}

uint32_t
route_get_table_id(const struct sbrec_datapath_binding *dp)
{
    int64_t vrf_id = ovn_smap_get_llong(&dp->external_ids,
                                        "dynamic-routing-vrf-id", -1);
    return (vrf_id >= 1 && vrf_id <= UINT32_MAX) ? vrf_id : dp->tunnel_key;
}

struct advertise_route_entry *
advertise_route_find(unsigned int priority, const struct in6_addr *prefix,
                     unsigned int plen, const struct in6_addr *nexthop,
                     const struct hmap *advertised_routes)
{
    uint32_t hash = advertise_route_hash(prefix, nexthop, plen);
    struct advertise_route_entry *ar;
    HMAP_FOR_EACH_WITH_HASH (ar, node, hash, advertised_routes) {
        if (ipv6_addr_equals(&ar->addr, prefix) &&
            ipv6_addr_equals(&ar->nexthop, nexthop) &&
            ar->plen == plen &&
            ar->priority == priority) {
            return ar;
        }
    }
    return NULL;
}
