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

#ifndef GARP_RARP_H
#define GARP_RARP_H 1

#include "openvswitch/hmap.h"
#include "openvswitch/types.h"

/* Contains a single mac and ip address that should be announced. */
struct garp_rarp_node {
    struct hmap_node hmap_node;
    struct eth_addr ea;          /* Ethernet address of port. */
    ovs_be32 ipv4;               /* Ipv4 address of port. */
    long long int announce_time; /* Next announcement in ms.
                                  * If LLONG_MAX there should be no
                                  * annoucement. */
    int backoff;                 /* Backoff timeout for the next
                                  * announcement (in msecs). */
    uint32_t dp_key;             /* Datapath used to output this GARP. */
    uint32_t port_key;           /* Port to inject the GARP into. */
};

struct garp_rarp_ctx_in {
    struct ovsdb_idl_txn *ovnsb_idl_txn;
    struct ovsdb_idl_index *sbrec_port_binding_by_datapath;
    struct ovsdb_idl_index *sbrec_port_binding_by_name;
    struct ovsdb_idl_index *sbrec_mac_binding_by_lport_ip;
    const struct sbrec_ecmp_nexthop_table *ecmp_nh_table;
    const struct ovsrec_bridge *br_int;
    const struct sbrec_chassis *chassis;
    const struct hmap *local_datapaths;
    const struct sset *active_tunnels;
    struct sset *non_local_lports;
    struct sset *local_lports;
};

struct garp_rarp_ctx_out {
    /* Contains struct garp_rarp_node. */
    struct hmap *garp_rarp_data;
};

void garp_rarp_run(struct garp_rarp_ctx_in *, struct garp_rarp_ctx_out *);
bool garp_rarp_sync(const struct hmap *source, struct hmap *dest,
                    bool reset_timers);
void garp_rarp_node_free(struct garp_rarp_node *);

#endif /* GARP_RARP_H */
