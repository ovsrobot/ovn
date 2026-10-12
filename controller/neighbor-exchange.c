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
#include <errno.h>

#include <linux/neighbour.h>

#include "host-if-monitor.h"
#include "neighbor.h"
#include "neighbor-exchange.h"
#include "neighbor-exchange-netlink.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/vlog.h"
#include "ovn-util.h"
#include "ovn-sb-idl.h"
#include "timeval.h"
#include "packets.h"
#include "vec.h"
#include "unixctl.h"

VLOG_DEFINE_THIS_MODULE(neighbor_exchange);

static uint32_t evpn_remote_vtep_hash(const struct in6_addr *ip,
                                      uint16_t port, uint32_t vni);
static void evpn_remote_vtep_add(struct hmap *remote_vteps, struct in6_addr ip,
                                 uint16_t port, uint32_t vni);
static struct evpn_remote_vtep *evpn_remote_vtep_find(
    const struct hmap *remote_vteps, const struct in6_addr *ip,
    uint16_t port, uint32_t vni);
static void evpn_static_entry_add(struct hmap *static_entries,
                                  struct eth_addr mac, struct in6_addr ip,
                                  uint32_t vni, uint32_t nh_id);
static struct evpn_static_entry *evpn_static_entry_find(
    const struct hmap *static_entries, struct eth_addr mac,
    struct in6_addr ip, uint32_t vni, uint32_t nh_id);
static uint32_t evpn_static_entry_hash(const struct eth_addr *mac,
                                       const struct in6_addr *ip,
                                       uint32_t vni, uint32_t nh_id);

/* Last neighbor_exchange netlink operation. */
static int neighbor_exchange_nl_status;
static struct vector pending_withdrawals =
    VECTOR_EMPTY_INITIALIZER(struct neighbor_interface_monitor *);
static long long withdrawal_retry_at;
static int withdrawal_error;
#define NEIGHBOR_WITHDRAW_RETRY_MS 1000

#define CLEAR_NEIGHBOR_EXCHANGE_NL_STATUS() \
    do {                                    \
        neighbor_exchange_nl_status = 0;    \
    } while (0)

#define SET_NEIGHBOR_EXCHANGE_NL_STATUS(error)     \
    do {                                           \
        if (!neighbor_exchange_nl_status) {        \
            neighbor_exchange_nl_status = (error); \
            if (error) {                           \
                poll_immediate_wake();             \
            }                                      \
        }                                          \
    } while (0)

/* Withdraw a precise subset of the preceding advertised state.  Resolving
 * the name again prevents an old cached ifindex from deleting state on a
 * newly created, unrelated interface which reused that index. */
static int
withdraw_announcements(const struct neighbor_interface_monitor *nim,
                       const struct hmap *neighbors)
{
    if (hmap_is_empty(neighbors)) {
        return 0;
    }
    int32_t if_index = if_nametoindex(nim->if_name);
    if (if_index) {
        return ne_nl_withdraw_neigh(nim->family, if_index, neighbors);
    }
    return errno == ENODEV || errno == ENXIO || errno == ENOENT ? 0 : errno;
}

static void
copy_announcement(struct hmap *neighbors,
                   const struct advertise_neighbor_entry *an)
{
    struct advertise_neighbor_entry *copy = xmemdup(an, sizeof *an);
    hmap_insert(neighbors, &copy->node,
                 advertise_neigh_hash(&copy->lladdr, &copy->addr));
}

static void
clear_announcements(struct hmap *neighbors)
{
    struct advertise_neighbor_entry *an;
    HMAP_FOR_EACH_POP (an, node, neighbors) {
        free(an);
    }
    hmap_destroy(neighbors);
}

static void
queue_withdrawal(const struct neighbor_interface_monitor *nim,
                 const struct hmap *neighbors, int error)
{
    struct neighbor_interface_monitor *pending = NULL;
    struct neighbor_interface_monitor *candidate;
    VECTOR_FOR_EACH (&pending_withdrawals, candidate) {
        if (candidate->family == nim->family
            && !strcmp(candidate->if_name, nim->if_name)) {
            pending = candidate;
            break;
        }
    }
    if (!pending) {
        pending = xzalloc(sizeof *pending);
        pending->family = nim->family;
        pending->type = nim->type;
        pending->vni = nim->vni;
        ovs_strlcpy(pending->if_name, nim->if_name, sizeof pending->if_name);
        hmap_init(&pending->announced_neighbors);
        vector_push(&pending_withdrawals, &pending);
    }
    const struct advertise_neighbor_entry *an;
    HMAP_FOR_EACH (an, node, neighbors) {
        if (!advertise_neigh_find(&pending->announced_neighbors, an->lladdr,
                                  &an->addr)) {
            copy_announcement(&pending->announced_neighbors, an);
        }
    }
    if (!withdrawal_error) {
        withdrawal_error = error;
        withdrawal_retry_at = time_msec() + NEIGHBOR_WITHDRAW_RETRY_MS;
    }
    poll_timer_wait_until(withdrawal_retry_at);
}

/* Keep immutable ownership tuples across engine cache replacement.  A
 * restored source cancels its preceding failed withdrawal; a NULL current
 * set is used during shutdown, when all queued withdrawals must complete. */
int
neighbor_exchange_retry_withdrawals(const struct vector *current_interfaces)
{
    if (vector_is_empty(&pending_withdrawals)) {
        withdrawal_error = 0;
        return 0;
    }
    if (time_msec() < withdrawal_retry_at) {
        poll_timer_wait_until(withdrawal_retry_at);
        return withdrawal_error;
    }

    int ret = 0;
    for (size_t i = 0; i < vector_len(&pending_withdrawals);) {
        struct neighbor_interface_monitor *pending =
            vector_get(&pending_withdrawals, i,
                        struct neighbor_interface_monitor *);
        if (current_interfaces) {
            const struct neighbor_interface_monitor *current;
            VECTOR_FOR_EACH (current_interfaces, current) {
                if (pending->family != current->family
                    || strcmp(pending->if_name, current->if_name)) {
                    continue;
                }
                struct advertise_neighbor_entry *an;
                HMAP_FOR_EACH_SAFE (an, node, &pending->announced_neighbors) {
                    if (advertise_neigh_find(&current->announced_neighbors,
                                             an->lladdr, &an->addr)) {
                        hmap_remove(&pending->announced_neighbors, &an->node);
                        free(an);
                    }
                }
            }
        }
        int err = withdraw_announcements(pending,
                                          &pending->announced_neighbors);
        if (!err) {
            clear_announcements(&pending->announced_neighbors);
            free(pending);
            vector_remove_fast(&pending_withdrawals, i, NULL);
        } else {
            if (!ret) {
                ret = err;
            }
            i++;
        }
    }
    withdrawal_error = ret;
    if (ret) {
        withdrawal_retry_at = time_msec() + NEIGHBOR_WITHDRAW_RETRY_MS;
        poll_timer_wait_until(withdrawal_retry_at);
    }
    return ret;
}

void
neighbor_exchange_destroy(void)
{
    struct neighbor_interface_monitor *pending;
    VECTOR_FOR_EACH (&pending_withdrawals, pending) {
        clear_announcements(&pending->announced_neighbors);
        free(pending);
    }
    vector_destroy(&pending_withdrawals);
    withdrawal_error = 0;
}

/* Reconcile removed interfaces as well as ordinary updates.  Their absence
 * from the new monitored set otherwise prevents the normal sync from
 * withdrawing their old announcements. */
int
neighbor_exchange_withdraw_removed(const struct vector *old_interfaces,
                                   const struct vector *new_interfaces)
{
    int ret = 0;
    const struct neighbor_interface_monitor *old;
    VECTOR_FOR_EACH (old_interfaces, old) {
        const struct hmap *keep = NULL;
        const struct neighbor_interface_monitor *new;
        VECTOR_FOR_EACH (new_interfaces, new) {
            if (old->family == new->family
                && !strcmp(old->if_name, new->if_name)) {
                keep = &new->announced_neighbors;
                break;
            }
        }

        struct hmap withdrawn = HMAP_INITIALIZER(&withdrawn);
        const struct advertise_neighbor_entry *an;
        HMAP_FOR_EACH (an, node, &old->announced_neighbors) {
            if (!keep || !advertise_neigh_find(keep, an->lladdr, &an->addr)) {
                copy_announcement(&withdrawn, an);
            }
        }
        int err = withdraw_announcements(old, &withdrawn);
        if (err) {
            queue_withdrawal(old, &withdrawn, err);
        }
        if (!ret) {
            ret = err;
        }
        clear_announcements(&withdrawn);
    }
    return ret;
}

/* Used before releasing a controller-owned source at graceful shutdown.
 * Only configured MAC/IP tuples in this datapath's advertised desired set
 * are candidates, and netlink verifies actual OVN ownership before
 * deleting. */
int
neighbor_exchange_withdraw_port(const struct sbrec_port_binding *pb,
                                const struct vector *interfaces)
{
    int64_t vni = ovn_smap_get_llong(&pb->datapath->external_ids,
                                     "dynamic-routing-vni", -1);
    if (!ovn_is_valid_vni(vni)) {
        return 0;
    }

    int ret = 0;
    for (size_t i = 0; i < pb->n_mac; i++) {
        struct lport_addresses addresses;
        if (!extract_lsp_addresses(pb->mac[i], &addresses)) {
            continue;
        }
        const struct neighbor_interface_monitor *nim;
        VECTOR_FOR_EACH (interfaces, nim) {
            if (nim->vni != vni) {
                continue;
            }
            struct hmap withdrawn = HMAP_INITIALIZER(&withdrawn);
            const struct advertise_neighbor_entry *an;
            HMAP_FOR_EACH (an, node, &nim->announced_neighbors) {
                if (!eth_addr_equals(an->lladdr, addresses.ea)) {
                    continue;
                }
                bool ours = ipv6_is_zero(&an->addr);
                for (size_t j = 0; !ours && j < addresses.n_ipv4_addrs; j++) {
                    ours = IN6_IS_ADDR_V4MAPPED(&an->addr)
                           && in6_addr_get_mapped_ipv4(&an->addr)
                              == addresses.ipv4_addrs[j].addr;
                }
                for (size_t j = 0; !ours && j < addresses.n_ipv6_addrs; j++) {
                    ours = ipv6_addr_equals(&an->addr,
                                            &addresses.ipv6_addrs[j].addr);
                }
                if (ours) {
                    copy_announcement(&withdrawn, an);
                }
            }
            int err = withdraw_announcements(nim, &withdrawn);
            if (err) {
                queue_withdrawal(nim, &withdrawn, err);
            }
            if (!ret) {
                ret = err;
            }
            clear_announcements(&withdrawn);
        }
        destroy_lport_addresses(&addresses);
    }
    return ret;
}

void
neighbor_exchange_run(const struct neighbor_exchange_ctx_in *n_ctx_in,
                      struct neighbor_exchange_ctx_out *n_ctx_out)
{
    struct neighbor_interface_monitor *nim;

    struct sset if_names = SSET_INITIALIZER(&if_names);
    VECTOR_FOR_EACH (n_ctx_in->monitored_interfaces, nim) {
        sset_add(&if_names, nim->if_name);
    }
    host_if_monitor_update_watches(&if_names);
    sset_destroy(&if_names);

    CLEAR_NEIGHBOR_EXCHANGE_NL_STATUS();
    neighbor_exchange_retry_withdrawals(n_ctx_in->monitored_interfaces);
    VECTOR_FOR_EACH (n_ctx_in->monitored_interfaces, nim) {
        int32_t if_index = host_if_monitor_ifname_toindex(nim->if_name);

        if (!if_index) {
            continue;
        }

        struct vector received_neighbors =
            VECTOR_EMPTY_INITIALIZER(struct ne_nl_received_neigh);
        SET_NEIGHBOR_EXCHANGE_NL_STATUS(
            ne_nl_sync_neigh(nim->family, if_index, &nim->announced_neighbors,
                             &received_neighbors)
        );

        struct ne_nl_received_neigh *ne;
        switch (nim->type) {
        case NEIGH_IFACE_BRIDGE:
            VECTOR_FOR_EACH_PTR (&received_neighbors, ne) {
                if (ne_is_valid_static_arp(ne)) {
                    if (!evpn_static_entry_find(n_ctx_out->static_arps,
                                                ne->lladdr, ne->addr,
                                                nim->vni, 0)) {
                        evpn_static_entry_add(n_ctx_out->static_arps,
                                              ne->lladdr, ne->addr,
                                              nim->vni, 0);
                    }
                }
            }
            break;
        case NEIGH_IFACE_VXLAN:
            VECTOR_FOR_EACH_PTR (&received_neighbors, ne) {
                if (ne_is_valid_remote_vtep(ne)) {
                    uint16_t port = ne->port ? ne->port : DEFAULT_VXLAN_PORT;
                    if (!evpn_remote_vtep_find(n_ctx_out->remote_vteps,
                                               &ne->addr, port, nim->vni)) {
                        evpn_remote_vtep_add(n_ctx_out->remote_vteps, ne->addr,
                                             port, nim->vni);
                    }
                }

                if (ne_is_valid_static_fdb(ne)) {
                    if (!evpn_static_entry_find(n_ctx_out->static_fdbs,
                                                ne->lladdr, ne->addr,
                                                nim->vni, ne->nh_id)) {
                        evpn_static_entry_add(n_ctx_out->static_fdbs,
                                              ne->lladdr, ne->addr,
                                              nim->vni, ne->nh_id);
                    }
                }
            }
            break;
        case NEIGH_IFACE_LOOPBACK:
            /* No learning from the loopback interface required. */
            break;
        }

        vector_push(n_ctx_out->neighbor_table_watches, &if_index);
        vector_destroy(&received_neighbors);
    }
}

int
neighbor_exchange_status_run(void)
{
    if (!vector_is_empty(&pending_withdrawals)) {
        if (time_msec() >= withdrawal_retry_at) {
            return withdrawal_error;
        }
        poll_timer_wait_until(withdrawal_retry_at);
    }
    return neighbor_exchange_nl_status;
}

void
evpn_remote_vteps_clear(struct hmap *remote_vteps)
{
    struct evpn_remote_vtep *vtep;
    HMAP_FOR_EACH_POP (vtep, hmap_node, remote_vteps) {
        free(vtep);
    }
}

void
evpn_remote_vtep_list(struct unixctl_conn *conn, int argc OVS_UNUSED,
                      const char *argv[] OVS_UNUSED, void *data_)
{
    struct hmap *remote_vteps = data_;
    struct ds ds = DS_EMPTY_INITIALIZER;

    struct evpn_remote_vtep *vtep;
    HMAP_FOR_EACH (vtep, hmap_node, remote_vteps) {
        ds_put_cstr(&ds, "IP: ");
        ipv6_format_mapped(&vtep->ip, &ds);
        ds_put_format(&ds, ", port: %"PRIu16", vni: %"PRIu32"\n",
                      vtep->port, vtep->vni);
    }

    unixctl_command_reply(conn, ds_cstr_ro(&ds));
    ds_destroy(&ds);
}

void
evpn_static_entries_clear(struct hmap *static_entries)
{
    struct evpn_static_entry *e;
    HMAP_FOR_EACH_POP (e, hmap_node, static_entries) {
        free(e);
    }
}

static void
evpn_remote_vtep_add(struct hmap *remote_vteps, struct in6_addr ip,
                     uint16_t port, uint32_t vni)
{
    struct evpn_remote_vtep *vtep = xmalloc(sizeof *vtep);
    *vtep = (struct evpn_remote_vtep) {
        .ip = ip,
        .port = port,
        .vni = vni,
    };

    hmap_insert(remote_vteps, &vtep->hmap_node,
                evpn_remote_vtep_hash(&ip, port, vni));
}

static struct evpn_remote_vtep *
evpn_remote_vtep_find(const struct hmap *remote_vteps,
                      const struct in6_addr *ip,
                      uint16_t port, uint32_t vni)
{
    uint32_t hash = evpn_remote_vtep_hash(ip, port, vni);

    struct evpn_remote_vtep *vtep;
    HMAP_FOR_EACH_WITH_HASH (vtep, hmap_node, hash, remote_vteps) {
        if (ipv6_addr_equals(&vtep->ip, ip) &&
            vtep->port == port && vtep->vni == vni) {
            return vtep;
        }
    }

    return NULL;
}

static uint32_t
evpn_remote_vtep_hash(const struct in6_addr *ip, uint16_t port,
                      uint32_t vni)
{
    uint32_t hash = 0;
    hash = hash_add_in6_addr(hash, ip);
    hash = hash_add(hash, port);
    hash = hash_add(hash, vni);

    return hash_finish(hash, 14);
}

static void
evpn_static_entry_add(struct hmap *static_entries, struct eth_addr mac,
                      struct in6_addr ip, uint32_t vni, uint32_t nh_id)
{
    struct evpn_static_entry *e = xmalloc(sizeof *e);
    *e = (struct evpn_static_entry) {
        .mac = mac,
        .ip = ip,
        .vni = vni,
        .nh_id = nh_id,
    };

    hmap_insert(static_entries, &e->hmap_node,
                evpn_static_entry_hash(&mac, &ip, vni, nh_id));
}

static struct evpn_static_entry *
evpn_static_entry_find(const struct hmap *static_entries, struct eth_addr mac,
                       struct in6_addr ip, uint32_t vni, uint32_t nh_id)
{
    uint32_t hash = evpn_static_entry_hash(&mac, &ip, vni, nh_id);

    struct evpn_static_entry *e;
    HMAP_FOR_EACH_WITH_HASH (e, hmap_node, hash, static_entries) {
        if (eth_addr_equals(e->mac, mac) &&
            ipv6_addr_equals(&e->ip, &ip) &&
            e->vni == vni &&
            e->nh_id == nh_id) {
            return e;
        }
    }

    return NULL;
}

static uint32_t
evpn_static_entry_hash(const struct eth_addr *mac, const struct in6_addr *ip,
                       uint32_t vni, uint32_t nh_id)
{
    uint32_t hash = 0;
    hash = hash_bytes(mac, sizeof *mac, hash);
    hash = hash_add_in6_addr(hash, ip);
    hash = hash_add(hash, vni);
    hash = hash_add(hash, nh_id);

    return hash_finish(hash, 30);
}
