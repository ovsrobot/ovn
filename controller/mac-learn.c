/* Copyright (c) 2020, Red Hat, Inc.
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

#include "mac-learn.h"

/* OpenvSwitch lib includes. */
#include "dp-packet.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/vlog.h"
#include "lib/packets.h"
#include "lib/smap.h"
#include "lib/timeval.h"

VLOG_DEFINE_THIS_MODULE(mac_learn);

#define MAX_MAC_BINDINGS 1000
#define MAX_FDB_ENTRIES  1000
#define BUFFER_QUEUE_DEPTH 4

static size_t keys_ip_hash(uint32_t dp_key, uint32_t port_key,
                               struct in6_addr *);
static struct mac_binding *mac_binding_find(const struct hmap *mac_bindings,
                                            uint32_t dp_key,
                                            uint32_t port_key,
                                            struct in6_addr *ip, size_t hash);
static size_t fdb_entry_hash(uint32_t dp_key, struct eth_addr *);

static struct fdb_entry *fdb_entry_find(struct hmap *fdbs, uint32_t dp_key,
                                        struct eth_addr *mac, size_t hash);

/* mac_binding functions. */
void
ovn_mac_bindings_init(struct hmap *mac_bindings)
{
    hmap_init(mac_bindings);
}

void
ovn_mac_bindings_destroy(struct hmap *mac_bindings)
{
    struct mac_binding *mb;
    HMAP_FOR_EACH_POP (mb, hmap_node, mac_bindings) {
        free(mb);
    }
    hmap_destroy(mac_bindings);
}

struct mac_binding *
ovn_mac_binding_add(struct hmap *mac_bindings, uint32_t dp_key,
                    uint32_t port_key, struct in6_addr *ip,
                    struct eth_addr mac, uint32_t timestamp_offset,
                    bool limited_capacity)
{
    uint32_t hash = keys_ip_hash(dp_key, port_key, ip);

    struct mac_binding *mb =
        mac_binding_find(mac_bindings, dp_key, port_key, ip, hash);
    if (!mb) {
        if (limited_capacity && hmap_count(mac_bindings) >= MAX_MAC_BINDINGS) {
            return NULL;
        }

        mb = xmalloc(sizeof *mb);
        mb->dp_key = dp_key;
        mb->port_key = port_key;
        mb->ip = *ip;
        mb->expire = time_msec() + timestamp_offset;
        hmap_insert(mac_bindings, &mb->hmap_node, hash);
    }
    mb->mac = mac;

    return mb;
}

/* This is called from ovn-controller main context */
void
ovn_mac_binding_wait(struct hmap *mac_bindings)
{
    struct mac_binding *mb;

    HMAP_FOR_EACH (mb, hmap_node, mac_bindings) {
        poll_timer_wait_until(mb->expire);
    }
}

void
ovn_mac_binding_remove(struct mac_binding *mb, struct hmap *mac_bindings)
{
    hmap_remove(mac_bindings, &mb->hmap_node);
    free(mb);
}

bool
ovn_mac_binding_is_expired(const struct mac_binding *mb, long long now)
{
    return now >= mb->expire;
}

/* fdb functions. */
void
ovn_fdb_init(struct hmap *fdbs)
{
    hmap_init(fdbs);
}

void
ovn_fdbs_flush(struct hmap *fdbs)
{
    struct fdb_entry *fdb_e;
    HMAP_FOR_EACH_POP (fdb_e, hmap_node, fdbs) {
        free(fdb_e);
    }
}

void
ovn_fdbs_destroy(struct hmap *fdbs)
{
   ovn_fdbs_flush(fdbs);
   hmap_destroy(fdbs);
}

struct fdb_entry *
ovn_fdb_add(struct hmap *fdbs, uint32_t dp_key, struct eth_addr mac,
            uint32_t port_key)
{
    uint32_t hash = fdb_entry_hash(dp_key, &mac);

    struct fdb_entry *fdb_e =
        fdb_entry_find(fdbs, dp_key, &mac, hash);
    if (!fdb_e) {
        if (hmap_count(fdbs) >= MAX_FDB_ENTRIES) {
            return NULL;
        }

        fdb_e = xzalloc(sizeof *fdb_e);
        fdb_e->dp_key = dp_key;
        fdb_e->mac = mac;
        hmap_insert(fdbs, &fdb_e->hmap_node, hash);
    }
    fdb_e->port_key = port_key;

    return fdb_e;

}

/* mac_binding related static functions. */

static size_t
keys_ip_hash(uint32_t dp_key, uint32_t port_key, struct in6_addr *ip)
{
    return hash_bytes(ip, sizeof *ip, hash_2words(dp_key, port_key));
}

static struct mac_binding *
mac_binding_find(const struct hmap *mac_bindings, uint32_t dp_key,
                 uint32_t port_key, struct in6_addr *ip, size_t hash)
{
    struct mac_binding *mb;
    HMAP_FOR_EACH_WITH_HASH (mb, hmap_node, hash, mac_bindings) {
        if (mb->dp_key == dp_key && mb->port_key == port_key &&
            IN6_ARE_ADDR_EQUAL(&mb->ip, ip)) {
            return mb;
        }
    }

    return NULL;
}

/* fdb related static functions. */

static size_t
fdb_entry_hash(uint32_t dp_key, struct eth_addr *mac)
{
    uint64_t mac64 = eth_addr_to_uint64(*mac);
    return hash_2words(dp_key, hash_uint64(mac64));
}

static struct fdb_entry *
fdb_entry_find(struct hmap *fdbs, uint32_t dp_key,
               struct eth_addr *mac, size_t hash)
{
    struct fdb_entry *fdb_e;
    HMAP_FOR_EACH_WITH_HASH (fdb_e, hmap_node, hash, fdbs) {
        if (fdb_e->dp_key == dp_key && eth_addr_equals(fdb_e->mac, *mac)) {
            return fdb_e;
        }
    }

    return NULL;
}

static struct buffered_packets *
buffered_packets_find(struct hmap *hmap, uint64_t dp_key, uint64_t port_key,
                      struct in6_addr *ip, uint32_t hash)
{
    struct buffered_packets *mb;
    HMAP_FOR_EACH_WITH_HASH (mb, hmap_node, hash, hmap) {
        if (mb->dp_key == dp_key && mb->port_key == port_key &&
            IN6_ARE_ADDR_EQUAL(&mb->ip, ip)) {
            return mb;
        }
    }

    return NULL;
}

static void
buffered_packets_destroy(struct buffered_packets *bp)
{
    ovn_packet_data_list_destroy(&bp->queue);
    free(bp);
}

struct buffered_packets *
ovn_buffered_packets_add(struct hmap *hmap, uint64_t dp_key, uint64_t port_key,
                         struct in6_addr ip)
{
    struct buffered_packets *bp;

    uint32_t hash = keys_ip_hash(dp_key, port_key, &ip);
    bp = buffered_packets_find(hmap, dp_key, port_key, &ip, hash);
    if (!bp) {
        if (hmap_count(hmap) >= 1000) {
            return NULL;
        }

        bp = xmalloc(sizeof *bp);
        hmap_insert(hmap, &bp->hmap_node, hash);
        bp->ip = ip;
        bp->dp_key = dp_key;
        bp->port_key = port_key;
        ovs_list_init(&bp->queue);
    }

    bp->expire = time_msec() + OVN_BUFFERED_PACKETS_TIMEOUT;

    return bp;
}

void
ovn_buffered_packets_add_packet_data(struct buffered_packets *bp,
                                     struct ofpbuf ofpacts,
                                     struct dp_packet *packet)
{
    struct packet_data *pd = xmalloc(sizeof *pd);

    pd->ofpacts = ofpacts;
    pd->p = packet;

    if (ovs_list_size(&bp->queue) == BUFFER_QUEUE_DEPTH) {
        struct packet_data *p = CONTAINER_OF(ovs_list_pop_front(&bp->queue),
                                             struct packet_data, node);
        ovn_packet_data_destroy(p);
    }

    ovs_list_push_back(&bp->queue, &pd->node);
}

void
ovn_buffured_packets_prepare_ready(struct hmap *bp_hmap,
                                   const struct hmap *recent_mac_bindings,
                                   struct ovs_list *ready_packet_data)
{
    long long now = time_msec();

    struct buffered_packets *bp;
    HMAP_FOR_EACH_SAFE (bp, hmap_node, bp_hmap) {
        if (now > bp->expire) {
            hmap_remove(bp_hmap, &bp->hmap_node);
            buffered_packets_destroy(bp);
            continue;
        }

        uint32_t hash = keys_ip_hash(bp->dp_key, bp->port_key, &bp->ip);
        struct mac_binding *mb = mac_binding_find(recent_mac_bindings,
                                                  bp->dp_key, bp->port_key,
                                                  &bp->ip, hash);
        if (!mb) {
            continue;
        }

        struct packet_data *pd;
        LIST_FOR_EACH_POP (pd, node, &bp->queue) {
            struct eth_header *eth = dp_packet_data(pd->p);
            eth->eth_dst = mb->mac;

            ovs_list_push_back(ready_packet_data, &pd->node);
        }

        hmap_remove(bp_hmap, &bp->hmap_node);
        buffered_packets_destroy(bp);
    }
}

void
ovn_packet_data_destroy(struct packet_data *pd)
{
    dp_packet_delete(pd->p);
    ofpbuf_uninit(&pd->ofpacts);
    free(pd);
}

void
ovn_packet_data_list_destroy(struct ovs_list *list)
{
    struct packet_data *pd;
    LIST_FOR_EACH_POP (pd, node, list) {
        ovn_packet_data_destroy(pd);
    }
}

void
ovn_buffered_packets_hmap_destroy(struct hmap *hmap)
{
    struct buffered_packets *bp;
    HMAP_FOR_EACH_POP (bp, hmap_node, hmap) {
        buffered_packets_destroy(bp);
    }
}
