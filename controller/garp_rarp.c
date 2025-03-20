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

#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>

#include "controller/encaps.h"
#include "controller/local_data.h"
#include "controller/lport.h"
#include "hash.h"
#include "mac-binding-index.h"
#include "openvswitch/hmap.h"
#include "openvswitch/vlog.h"
#include "ovn/lex.h"
#include "packets.h"
#include "smap.h"
#include "sset.h"
#include "openvswitch/shash.h"
#include "garp_rarp.h"
#include "ovn-sb-idl.h"
#include "vswitch-idl.h"

VLOG_DEFINE_THIS_MODULE(garp_rarp);
static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);

/* Get localnet vifs, local l3gw ports and ofport for localnet patch ports. */
static void
get_localnet_vifs_l3gwports(
    struct ovsdb_idl_index *sbrec_port_binding_by_datapath,
    struct ovsdb_idl_index *sbrec_port_binding_by_name,
    const struct ovsrec_bridge *br_int,
    const struct sbrec_chassis *chassis,
    const struct hmap *local_datapaths,
    struct sset *localnet_vifs,
    struct sset *local_l3gw_ports)
{
    for (int i = 0; i < br_int->n_ports; i++) {
        const struct ovsrec_port *port_rec = br_int->ports[i];
        if (!strcmp(port_rec->name, br_int->name)) {
            continue;
        }
        const char *tunnel_id = smap_get(&port_rec->external_ids,
                                          "ovn-chassis-id");
        if (tunnel_id &&
                encaps_tunnel_id_match(tunnel_id, chassis->name, NULL, NULL)) {
            continue;
        }
        const char *localnet = smap_get(&port_rec->external_ids,
                                        "ovn-localnet-port");
        if (localnet) {
            continue;
        }
        for (int j = 0; j < port_rec->n_interfaces; j++) {
            const struct ovsrec_interface *iface_rec = port_rec->interfaces[j];
            if (!iface_rec->n_ofport) {
                continue;
            }
            /* Get localnet vif. */
            const char *iface_id = smap_get(&iface_rec->external_ids,
                                            "iface-id");
            if (!iface_id) {
                continue;
            }
            const struct sbrec_port_binding *pb
                = lport_lookup_by_name(sbrec_port_binding_by_name, iface_id);
            if (!pb || pb->chassis != chassis) {
                continue;
            }
            if (!iface_rec->link_state ||
                    strcmp(iface_rec->link_state, "up")) {
                continue;
            }
            struct local_datapath *ld
                = get_local_datapath(local_datapaths,
                                     pb->datapath->tunnel_key);
            if (ld && ld->localnet_port) {
                sset_add(localnet_vifs, iface_id);
            }
        }
    }

    struct sbrec_port_binding *target = sbrec_port_binding_index_init_row(
        sbrec_port_binding_by_datapath);

    const struct local_datapath *ld;
    HMAP_FOR_EACH (ld, hmap_node, local_datapaths) {
        const struct sbrec_port_binding *pb;

        if (!ld->localnet_port) {
            continue;
        }

        /* Get l3gw ports.  Consider port bindings with type "l3gateway"
         * that connect to gateway routers (if local), and consider port
         * bindings of type "patch" since they might connect to
         * distributed gateway ports with NAT addresses. */

        sbrec_port_binding_index_set_datapath(target, ld->datapath);
        SBREC_PORT_BINDING_FOR_EACH_EQUAL (pb, target,
                                           sbrec_port_binding_by_datapath) {
            if ((!strcmp(pb->type, "l3gateway") && pb->chassis == chassis)
                || !strcmp(pb->type, "patch")) {
                sset_add(local_l3gw_ports, pb->logical_port);
            }
        }
    }
    sbrec_port_binding_index_destroy_row(target);
}


/* Extracts the mac, IPv4 and IPv6 addresses, and logical port from
 * 'addresses' which should be of the format 'MAC [IP1 IP2 ..]
 * [is_chassis_resident("LPORT_NAME")]', where IPn should be a valid IPv4
 * or IPv6 address, and stores them in the 'ipv4_addrs' and 'ipv6_addrs'
 * fields of 'laddrs'.  The logical port name is stored in 'lport'.
 *
 * Returns true if at least 'MAC' is found in 'address', false otherwise.
 *
 * The caller must call destroy_lport_addresses() and free(*lport). */
static bool
extract_addresses_with_port(const char *addresses,
                            struct lport_addresses *laddrs,
                            char **lport)
{
    int ofs;
    if (!extract_addresses(addresses, laddrs, &ofs)) {
        return false;
    } else if (!addresses[ofs]) {
        return true;
    }

    struct lexer lexer;
    lexer_init(&lexer, addresses + ofs);
    lexer_get(&lexer);

    if (lexer.error || lexer.token.type != LEX_T_ID
        || !lexer_match_id(&lexer, "is_chassis_resident")) {
        VLOG_INFO_RL(&rl, "invalid syntax '%s' in address", addresses);
        lexer_destroy(&lexer);
        return true;
    }

    if (!lexer_match(&lexer, LEX_T_LPAREN)) {
        VLOG_INFO_RL(&rl, "Syntax error: expecting '(' after "
                          "'is_chassis_resident' in address '%s'", addresses);
        lexer_destroy(&lexer);
        return false;
    }

    if (lexer.token.type != LEX_T_STRING) {
        VLOG_INFO_RL(&rl,
                    "Syntax error: expecting quoted string after "
                    "'is_chassis_resident' in address '%s'", addresses);
        lexer_destroy(&lexer);
        return false;
    }

    *lport = xstrdup(lexer.token.s);

    lexer_get(&lexer);
    if (!lexer_match(&lexer, LEX_T_RPAREN)) {
        VLOG_INFO_RL(&rl, "Syntax error: expecting ')' after quoted string in "
                          "'is_chassis_resident()' in address '%s'",
                          addresses);
        lexer_destroy(&lexer);
        return false;
    }

    lexer_destroy(&lexer);
    return true;
}

static void
consider_nat_address(struct ovsdb_idl_index *sbrec_port_binding_by_name,
                     const char *nat_address,
                     const struct sbrec_port_binding *pb,
                     struct sset *nat_address_keys,
                     const struct sbrec_chassis *chassis,
                     const struct sset *active_tunnels,
                     struct shash *nat_addresses,
                     struct sset *non_local_lports,
                     struct sset *local_lports)
{
    struct lport_addresses *laddrs = xmalloc(sizeof *laddrs);
    char *lport = NULL;
    if (!extract_addresses_with_port(nat_address, laddrs, &lport)
        || (!lport && !strcmp(pb->type, "patch"))) {
        destroy_lport_addresses(laddrs);
        free(laddrs);
        free(lport);
        return;
    }
    if (lport) {
        if (!lport_is_chassis_resident(sbrec_port_binding_by_name,
                chassis, active_tunnels, lport)) {
            sset_add(non_local_lports, lport);
            destroy_lport_addresses(laddrs);
            free(laddrs);
            free(lport);
            return;
        } else {
            sset_add(local_lports, lport);
        }
    }
    free(lport);

    int i;
    for (i = 0; i < laddrs->n_ipv4_addrs; i++) {
        char *name = xasprintf("%s-%s", pb->logical_port,
                                        laddrs->ipv4_addrs[i].addr_s);
        sset_add(nat_address_keys, name);
        free(name);
    }
    if (laddrs->n_ipv4_addrs == 0) {
        char *name = xasprintf("%s-noip", pb->logical_port);
        sset_add(nat_address_keys, name);
        free(name);
    }
    shash_add(nat_addresses, pb->logical_port, laddrs);
}

static void
get_nat_addresses_and_keys(struct ovsdb_idl_index *sbrec_port_binding_by_name,
                           struct sset *nat_address_keys,
                           struct sset *local_l3gw_ports,
                           const struct sbrec_chassis *chassis,
                           const struct sset *active_tunnels,
                           struct shash *nat_addresses,
                           struct sset *non_local_lports,
                           struct sset *local_lports)
{
    const char *gw_port;
    SSET_FOR_EACH (gw_port, local_l3gw_ports) {
        const struct sbrec_port_binding *pb;

        pb = lport_lookup_by_name(sbrec_port_binding_by_name, gw_port);
        if (!pb) {
            continue;
        }

        if (pb->n_nat_addresses) {
            for (int i = 0; i < pb->n_nat_addresses; i++) {
                consider_nat_address(sbrec_port_binding_by_name,
                                     pb->nat_addresses[i], pb,
                                     nat_address_keys, chassis,
                                     active_tunnels,
                                     nat_addresses,
                                     non_local_lports,
                                     local_lports);
            }
        } else {
            /* Continue to support options:nat-addresses for version
             * upgrade. */
            const char *nat_addresses_options = smap_get(&pb->options,
                                                         "nat-addresses");
            if (nat_addresses_options) {
                consider_nat_address(sbrec_port_binding_by_name,
                                     nat_addresses_options, pb,
                                     nat_address_keys, chassis,
                                     active_tunnels,
                                     nat_addresses,
                                     non_local_lports,
                                     local_lports);
            }
        }
    }
}

static uint32_t
garp_rarp_node_hash(const struct eth_addr *ea, uint32_t dp_key,
                    uint32_t port_key)
{
    return hash_bytes(ea, sizeof *ea, hash_2words(dp_key, port_key));
}

static uint32_t
garp_rarp_node_hash_struct(const struct garp_rarp_node *n)
{
    return garp_rarp_node_hash(&n->ea, n->dp_key, n->port_key);
}

static void
garp_rarp_node_add(struct hmap *map, const struct eth_addr ea, ovs_be32 ip,
                   uint32_t dp_key, uint32_t port_key)
{
    struct garp_rarp_node *grn = xmalloc(sizeof *grn);
    grn->ea = ea;
    grn->ipv4 = ip;
    grn->announce_time = time_msec() + 1000;
    grn->backoff = 1000; /* msec. */
    grn->dp_key = dp_key;
    grn->port_key = port_key;
    hmap_insert(map, &grn->hmap_node, garp_rarp_node_hash_struct(grn));
}

/* Searches for a given garp_rarp_node in a hmap. Ignores the announce_time
 * and backoff field since they might be different based on runtime. */
static struct garp_rarp_node*
garp_rarp_lookup(const struct hmap *map, const struct garp_rarp_node *search)
{
    struct garp_rarp_node *n;
    uint32_t hash = garp_rarp_node_hash_struct(search);
    HMAP_FOR_EACH_WITH_HASH (n, hmap_node, hash, map) {
        if (!eth_addr_equals(search->ea, n->ea)) {
            continue;
        }

        if (search->ipv4 != n->ipv4) {
            continue;
        }

        if (search->dp_key != n->dp_key) {
            continue;
        }

        if (search->port_key != n->port_key) {
            continue;
        }

        return n;
    }
    return NULL;
}

/* Update or add an IP-MAC binding for 'logical_port'.
 * Caller should make sure that 'ovnsb_idl_txn' is valid. */
static void
mac_binding_add_to_sb(struct ovsdb_idl_txn *ovnsb_idl_txn,
                      struct ovsdb_idl_index *sbrec_mac_binding_by_lport_ip,
                      const char *logical_port,
                      const struct sbrec_datapath_binding *dp,
                      struct eth_addr ea, const char *ip,
                      bool update_only)
{
    /* Convert ethernet argument to string form for database. */
    char mac_string[ETH_ADDR_STRLEN + 1];
    snprintf(mac_string, sizeof mac_string, ETH_ADDR_FMT, ETH_ADDR_ARGS(ea));

    const struct sbrec_mac_binding *b =
            mac_binding_lookup(sbrec_mac_binding_by_lport_ip,
                               logical_port, ip);
    if (!b) {
        if (update_only) {
            return;
        }
        b = sbrec_mac_binding_insert(ovnsb_idl_txn);
        sbrec_mac_binding_set_logical_port(b, logical_port);
        sbrec_mac_binding_set_ip(b, ip);
        sbrec_mac_binding_set_datapath(b, dp);
    }

    if (strcmp(b->mac, mac_string)) {
        sbrec_mac_binding_set_mac(b, mac_string);

        /* For backward compatibility check if timestamp column is available
         * in SB DB. */
        if (sbrec_server_has_mac_binding_table_col_timestamp(
                ovsdb_idl_txn_get_idl(ovnsb_idl_txn))) {
            VLOG_DBG("Setting MAC binding timestamp for "
                     "ip:%s mac:%s port:%s to %lld",
                     b->ip, b->mac, logical_port, time_wall_msec());
            sbrec_mac_binding_set_timestamp(b, time_wall_msec());
        }
    }
}

/* Simulate the effect of a GARP on local datapaths, i.e., create MAC_Bindings
 * on peer router datapaths.
 */
static void
send_garp_locally(const struct garp_rarp_ctx_in *r_ctx_in,
                  const struct sbrec_port_binding *in_pb,
                  struct eth_addr ea, ovs_be32 ip)
{
    if (!r_ctx_in->ovnsb_idl_txn) {
        return;
    }

    const struct local_datapath *ldp =
        get_local_datapath(r_ctx_in->local_datapaths,
                           in_pb->datapath->tunnel_key);

    ovs_assert(ldp);
    for (size_t i = 0; i < ldp->n_peer_ports; i++) {
        const struct sbrec_port_binding *local = ldp->peer_ports[i].local;
        const struct sbrec_port_binding *remote = ldp->peer_ports[i].remote;

        /* Skip "ingress" port. */
        if (local == in_pb) {
            continue;
        }

        bool update_only = !smap_get_bool(&remote->datapath->external_ids,
                                          "always_learn_from_arp_request",
                                          true);

        struct ds ip_s = DS_EMPTY_INITIALIZER;

        ip_format_masked(ip, OVS_BE32_MAX, &ip_s);
        mac_binding_add_to_sb(r_ctx_in->ovnsb_idl_txn,
                              r_ctx_in->sbrec_mac_binding_by_lport_ip,
                              remote->logical_port, remote->datapath,
                              ea, ds_cstr(&ip_s), update_only);
        ds_destroy(&ip_s);
    }
}

/* Add or update a vif for which GARPs need to be announced. */
static void
send_garp_rarp_update(const struct garp_rarp_ctx_in *r_ctx_in,
                      const struct sbrec_port_binding *binding_rec,
                      struct shash *nat_addresses,
                      struct hmap *garp_rarp_data)
{
    /* Skip localports as they don't need to be announced */
    if (!strcmp(binding_rec->type, "localport")) {
        return;
    }

    /* Update GARP for NAT IP if it exists.  Consider port bindings with type
     * "l3gateway" for logical switch ports attached to gateway routers, and
     * port bindings with type "patch" for logical switch ports attached to
     * distributed gateway ports. */
    if (!strcmp(binding_rec->type, "l3gateway")
        || !strcmp(binding_rec->type, "patch")) {
        struct lport_addresses *laddrs = NULL;
        while ((laddrs = shash_find_and_delete(nat_addresses,
                                               binding_rec->logical_port))) {
            int i;
            for (i = 0; i < laddrs->n_ipv4_addrs; i++) {
                garp_rarp_node_add(garp_rarp_data, laddrs->ea,
                              laddrs->ipv4_addrs[i].addr,
                              binding_rec->datapath->tunnel_key,
                              binding_rec->tunnel_key);
                send_garp_locally(r_ctx_in, binding_rec, laddrs->ea,
                                  laddrs->ipv4_addrs[i].addr);
            }
            /*
             * Send RARPs even if we do not have a ipv4 address as it e.g.
             * happens on ipv6 only ports.
             */
            if (laddrs->n_ipv4_addrs == 0) {
                garp_rarp_node_add(garp_rarp_data, laddrs->ea,
                              0, binding_rec->datapath->tunnel_key,
                              binding_rec->tunnel_key);
            }
            destroy_lport_addresses(laddrs);
            free(laddrs);
        }
        return;
    }

    /* Add GARP for new vif. */
    int i;
    for (i = 0; i < binding_rec->n_mac; i++) {
        struct lport_addresses laddrs;
        ovs_be32 ip = 0;
        if (!extract_lsp_addresses(binding_rec->mac[i], &laddrs)) {
            continue;
        }

        if (laddrs.n_ipv4_addrs) {
            ip = laddrs.ipv4_addrs[0].addr;
        }

        garp_rarp_node_add(garp_rarp_data,
                      laddrs.ea, ip,
                      binding_rec->datapath->tunnel_key,
                      binding_rec->tunnel_key);
        if (ip) {
            send_garp_locally(r_ctx_in, binding_rec, laddrs.ea, ip);
        }

        destroy_lport_addresses(&laddrs);
        break;
    }
}

static struct garp_rarp_node*
garp_rarp_node_clone(const struct garp_rarp_node *src)
{
    struct garp_rarp_node *dst = xzalloc(sizeof *dst);
    dst->ea = src->ea;
    dst->ipv4 = src->ipv4;
    dst->announce_time = time_msec() + 1000;
    dst->backoff = 1000;
    dst->dp_key = src->dp_key;
    dst->port_key = src->port_key;
    return dst;
}

static void
garp_rarp_clear(struct garp_rarp_ctx_in *r_ctx_in,
                struct garp_rarp_ctx_out *r_ctx_out)
{
    struct garp_rarp_node *grn;
    HMAP_FOR_EACH_POP (grn, hmap_node, r_ctx_out->garp_rarp_data) {
        garp_rarp_node_free(grn);
    }
    hmap_clear(r_ctx_out->garp_rarp_data);
    sset_clear(r_ctx_in->non_local_lports);
    sset_clear(r_ctx_in->local_lports);
}

void
garp_rarp_run(struct garp_rarp_ctx_in *r_ctx_in,
              struct garp_rarp_ctx_out *r_ctx_out)
{
    garp_rarp_clear(r_ctx_in, r_ctx_out);

    struct sset localnet_vifs = SSET_INITIALIZER(&localnet_vifs);
    struct sset local_l3gw_ports = SSET_INITIALIZER(&local_l3gw_ports);
    struct sset nat_ip_keys = SSET_INITIALIZER(&nat_ip_keys);
    struct shash nat_addresses;

    shash_init(&nat_addresses);

    get_localnet_vifs_l3gwports(r_ctx_in->sbrec_port_binding_by_datapath,
                                r_ctx_in->sbrec_port_binding_by_name,
                                r_ctx_in->br_int, r_ctx_in->chassis,
                                r_ctx_in->local_datapaths,
                                &localnet_vifs, &local_l3gw_ports);

    get_nat_addresses_and_keys(r_ctx_in->sbrec_port_binding_by_name,
                               &nat_ip_keys, &local_l3gw_ports,
                               r_ctx_in->chassis, r_ctx_in->active_tunnels,
                               &nat_addresses, r_ctx_in->non_local_lports,
                               r_ctx_in->local_lports);

    /* Update send_garp_rarp_data. */
    const char *iface_id;
    SSET_FOR_EACH (iface_id, &localnet_vifs) {
        const struct sbrec_port_binding *pb = lport_lookup_by_name(
            r_ctx_in->sbrec_port_binding_by_name, iface_id);
        if (pb && !smap_get_bool(&pb->options, "disable_garp_rarp", false)) {
            send_garp_rarp_update(r_ctx_in, pb, &nat_addresses,
                                  r_ctx_out->garp_rarp_data);
        }
    }

    /* Update send_garp_rarp_data for nat-addresses. */
    const char *gw_port;
    SSET_FOR_EACH (gw_port, &local_l3gw_ports) {
        const struct sbrec_port_binding *pb = lport_lookup_by_name(
            r_ctx_in->sbrec_port_binding_by_name, gw_port);
        if (pb && !smap_get_bool(&pb->options, "disable_garp_rarp", false)) {
            send_garp_rarp_update(r_ctx_in, pb, &nat_addresses,
                                  r_ctx_out->garp_rarp_data);
        }
    }

    sset_destroy(&localnet_vifs);
    sset_destroy(&local_l3gw_ports);

    struct shash_node *iter;
    SHASH_FOR_EACH_SAFE (iter, &nat_addresses) {
        struct lport_addresses *laddrs = iter->data;
        destroy_lport_addresses(laddrs);
        shash_delete(&nat_addresses, iter);
        free(laddrs);
    }
    shash_destroy(&nat_addresses);

    sset_destroy(&nat_ip_keys);
}

/* Syncs a hmap of garp_rarp_node to another hmap. The elements in the
 * destination hmap are initialized with valid announce times based on the
 * current timestamp.
 * If source is NULL then destination will at most change the timer if
 * reset_timers is set.
 * If reset_timers is true then the timers of all entries in the destination
 * hmap will be reset to default values.
 * Returns true if the destination hmap was chnaged. */
bool
garp_rarp_sync(const struct hmap *source, struct hmap *dest, bool reset_timers)
{
    struct garp_rarp_node *grn;
    bool changed = false;

    HMAP_FOR_EACH_SAFE (grn, hmap_node, dest) {
        if (source && !garp_rarp_lookup(source, grn)) {
            hmap_remove(dest, &grn->hmap_node);
            garp_rarp_node_free(grn);
            changed = true;
        } else if (reset_timers) {
            grn->announce_time = time_msec() + 1000;
            grn->backoff = 1000;
            changed = true;
        }
    }

    if (source) {
        HMAP_FOR_EACH (grn, hmap_node, source) {
            if (!garp_rarp_lookup(dest, grn)) {
                uint32_t hash = garp_rarp_node_hash_struct(grn);
                hmap_insert(dest, &garp_rarp_node_clone(grn)->hmap_node, hash);
                changed = true;
            }
        }
    }

    return changed;
}

void
garp_rarp_node_free(struct garp_rarp_node *garp_rarp)
{
    free(garp_rarp);
}
