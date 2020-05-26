/* Copyright (c) 2015, 2016, 2017 Nicira, Inc.
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
#include "binding.h"
#include "ha-chassis.h"
#include "lflow.h"
#include "lport.h"
#include "patch.h"

#include "lib/bitmap.h"
#include "openvswitch/poll-loop.h"
#include "lib/sset.h"
#include "lib/util.h"
#include "lib/netdev.h"
#include "lib/vswitch-idl.h"
#include "openvswitch/hmap.h"
#include "openvswitch/vlog.h"
#include "lib/chassis-index.h"
#include "lib/ovn-sb-idl.h"
#include "ovn-controller.h"

VLOG_DEFINE_THIS_MODULE(binding);

#define OVN_QOS_TYPE "linux-htb"

struct qos_queue {
    struct hmap_node node;
    uint32_t queue_id;
    uint32_t max_rate;
    uint32_t burst;
};

void
binding_register_ovs_idl(struct ovsdb_idl *ovs_idl)
{
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_open_vswitch);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_open_vswitch_col_bridges);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_bridge);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_name);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_ports);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_port);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_interfaces);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_qos);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_interface);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_external_ids);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_bfd);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_bfd_status);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_status);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_qos);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_qos_col_type);
}

static void
add_local_datapath__(struct ovsdb_idl_index *sbrec_datapath_binding_by_key,
                     struct ovsdb_idl_index *sbrec_port_binding_by_datapath,
                     struct ovsdb_idl_index *sbrec_port_binding_by_name,
                     const struct sbrec_datapath_binding *datapath,
                     bool has_local_l3gateway, int depth,
                     struct hmap *local_datapaths)
{
    uint32_t dp_key = datapath->tunnel_key;
    struct local_datapath *ld = get_local_datapath(local_datapaths, dp_key);
    if (ld) {
        if (has_local_l3gateway) {
            ld->has_local_l3gateway = true;
        }
        return;
    }

    ld = xzalloc(sizeof *ld);
    hmap_insert(local_datapaths, &ld->hmap_node, dp_key);
    ld->datapath = datapath;
    ld->localnet_port = NULL;
    ld->has_local_l3gateway = has_local_l3gateway;

    if (depth >= 100) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "datapaths nested too deep");
        return;
    }

    struct sbrec_port_binding *target =
        sbrec_port_binding_index_init_row(sbrec_port_binding_by_datapath);
    sbrec_port_binding_index_set_datapath(target, datapath);

    const struct sbrec_port_binding *pb;
    SBREC_PORT_BINDING_FOR_EACH_EQUAL (pb, target,
                                       sbrec_port_binding_by_datapath) {
        if (!strcmp(pb->type, "patch") || !strcmp(pb->type, "l3gateway")) {
            const char *peer_name = smap_get(&pb->options, "peer");
            if (peer_name) {
                const struct sbrec_port_binding *peer;

                peer = lport_lookup_by_name(sbrec_port_binding_by_name,
                                            peer_name);

                if (peer && peer->datapath) {
                    if (!strcmp(pb->type, "patch")) {
                        /* Add the datapath to local datapath only for patch
                         * ports. For l3gateway ports, since gateway router
                         * resides on one chassis, we don't need to add.
                         * Otherwise, all other chassis might create patch
                         * ports between br-int and the provider bridge. */
                        add_local_datapath__(sbrec_datapath_binding_by_key,
                                             sbrec_port_binding_by_datapath,
                                             sbrec_port_binding_by_name,
                                             peer->datapath, false,
                                             depth + 1, local_datapaths);
                    }
                    ld->n_peer_ports++;
                    if (ld->n_peer_ports > ld->n_allocated_peer_ports) {
                        ld->peer_ports =
                            x2nrealloc(ld->peer_ports,
                                       &ld->n_allocated_peer_ports,
                                       sizeof *ld->peer_ports);
                    }
                    ld->peer_ports[ld->n_peer_ports - 1].local = pb;
                    ld->peer_ports[ld->n_peer_ports - 1].remote = peer;
                }
            }
        }
    }
    sbrec_port_binding_index_destroy_row(target);
}

static void
add_local_datapath(struct ovsdb_idl_index *sbrec_datapath_binding_by_key,
                   struct ovsdb_idl_index *sbrec_port_binding_by_datapath,
                   struct ovsdb_idl_index *sbrec_port_binding_by_name,
                   const struct sbrec_datapath_binding *datapath,
                   bool has_local_l3gateway, struct hmap *local_datapaths)
{
    add_local_datapath__(sbrec_datapath_binding_by_key,
                         sbrec_port_binding_by_datapath,
                         sbrec_port_binding_by_name,
                         datapath, has_local_l3gateway, 0, local_datapaths);
}

static void
get_qos_params(const struct sbrec_port_binding *pb, struct hmap *queue_map)
{
    uint32_t max_rate = smap_get_int(&pb->options, "qos_max_rate", 0);
    uint32_t burst = smap_get_int(&pb->options, "qos_burst", 0);
    uint32_t queue_id = smap_get_int(&pb->options, "qdisc_queue_id", 0);

    if ((!max_rate && !burst) || !queue_id) {
        /* Qos is not configured for this port. */
        return;
    }

    struct qos_queue *node = xzalloc(sizeof *node);
    hmap_insert(queue_map, &node->node, hash_int(queue_id, 0));
    node->max_rate = max_rate;
    node->burst = burst;
    node->queue_id = queue_id;
}

static const struct ovsrec_qos *
get_noop_qos(struct ovsdb_idl_txn *ovs_idl_txn,
             const struct ovsrec_qos_table *qos_table)
{
    const struct ovsrec_qos *qos;
    OVSREC_QOS_TABLE_FOR_EACH (qos, qos_table) {
        if (!strcmp(qos->type, "linux-noop")) {
            return qos;
        }
    }

    if (!ovs_idl_txn) {
        return NULL;
    }
    qos = ovsrec_qos_insert(ovs_idl_txn);
    ovsrec_qos_set_type(qos, "linux-noop");
    return qos;
}

static bool
set_noop_qos(struct ovsdb_idl_txn *ovs_idl_txn,
             const struct ovsrec_port_table *port_table,
             const struct ovsrec_qos_table *qos_table,
             struct sset *egress_ifaces)
{
    if (!ovs_idl_txn) {
        return false;
    }

    const struct ovsrec_qos *noop_qos = get_noop_qos(ovs_idl_txn, qos_table);
    if (!noop_qos) {
        return false;
    }

    const struct ovsrec_port *port;
    size_t count = 0;

    OVSREC_PORT_TABLE_FOR_EACH (port, port_table) {
        if (sset_contains(egress_ifaces, port->name)) {
            ovsrec_port_set_qos(port, noop_qos);
            count++;
        }
        if (sset_count(egress_ifaces) == count) {
            break;
        }
    }
    return true;
}

static void
set_qos_type(struct netdev *netdev, const char *type)
{
    int error = netdev_set_qos(netdev, type, NULL);
    if (error) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "%s: could not set qdisc type \"%s\" (%s)",
                     netdev_get_name(netdev), type, ovs_strerror(error));
    }
}

static void
setup_qos(const char *egress_iface, struct hmap *queue_map)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 5);
    struct netdev *netdev_phy;

    if (!egress_iface) {
        /* Queues cannot be configured. */
        return;
    }

    int error = netdev_open(egress_iface, NULL, &netdev_phy);
    if (error) {
        VLOG_WARN_RL(&rl, "%s: could not open netdev (%s)",
                     egress_iface, ovs_strerror(error));
        return;
    }

    /* Check current qdisc. */
    const char *qdisc_type;
    struct smap qdisc_details;

    smap_init(&qdisc_details);
    if (netdev_get_qos(netdev_phy, &qdisc_type, &qdisc_details) != 0 ||
        qdisc_type[0] == '\0') {
        smap_destroy(&qdisc_details);
        netdev_close(netdev_phy);
        /* Qos is not supported. */
        return;
    }
    smap_destroy(&qdisc_details);

    /* If we're not actually being requested to do any QoS:
     *
     *     - If the current qdisc type is OVN_QOS_TYPE, then we clear the qdisc
     *       type to "".  Otherwise, it's possible that our own leftover qdisc
     *       settings could cause strange behavior on egress.  Also, QoS is
     *       expensive and may waste CPU time even if it's not really in use.
     *
     *       OVN isn't the only software that can configure qdiscs, and
     *       physical interfaces are shared resources, so there is some risk in
     *       this strategy: we could disrupt some other program's QoS.
     *       Probably, to entirely avoid this possibility we would need to add
     *       a configuration setting.
     *
     *     - Otherwise leave the qdisc alone. */
    if (hmap_is_empty(queue_map)) {
        if (!strcmp(qdisc_type, OVN_QOS_TYPE)) {
            set_qos_type(netdev_phy, "");
        }
        netdev_close(netdev_phy);
        return;
    }

    /* Configure qdisc. */
    if (strcmp(qdisc_type, OVN_QOS_TYPE)) {
        set_qos_type(netdev_phy, OVN_QOS_TYPE);
    }

    /* Check and delete if needed. */
    struct netdev_queue_dump dump;
    unsigned int queue_id;
    struct smap queue_details;
    struct qos_queue *sb_info;
    struct hmap consistent_queues;

    smap_init(&queue_details);
    hmap_init(&consistent_queues);
    NETDEV_QUEUE_FOR_EACH (&queue_id, &queue_details, &dump, netdev_phy) {
        bool is_queue_needed = false;

        HMAP_FOR_EACH_WITH_HASH (sb_info, node, hash_int(queue_id, 0),
                                 queue_map) {
            is_queue_needed = true;
            if (sb_info->max_rate ==
                smap_get_int(&queue_details, "max-rate", 0)
                && sb_info->burst == smap_get_int(&queue_details, "burst", 0)) {
                /* This queue is consistent. */
                hmap_insert(&consistent_queues, &sb_info->node,
                            hash_int(queue_id, 0));
                break;
            }
        }

        if (!is_queue_needed) {
            error = netdev_delete_queue(netdev_phy, queue_id);
            if (error) {
                VLOG_WARN_RL(&rl, "%s: could not delete queue %u (%s)",
                             egress_iface, queue_id, ovs_strerror(error));
            }
        }
    }

    /* Create/Update queues. */
    HMAP_FOR_EACH (sb_info, node, queue_map) {
        if (hmap_contains(&consistent_queues, &sb_info->node)) {
            hmap_remove(&consistent_queues, &sb_info->node);
            continue;
        }

        smap_clear(&queue_details);
        smap_add_format(&queue_details, "max-rate", "%d", sb_info->max_rate);
        smap_add_format(&queue_details, "burst", "%d", sb_info->burst);
        error = netdev_set_queue(netdev_phy, sb_info->queue_id,
                                 &queue_details);
        if (error) {
            VLOG_WARN_RL(&rl, "%s: could not configure queue %u (%s)",
                         egress_iface, sb_info->queue_id, ovs_strerror(error));
        }
    }
    smap_destroy(&queue_details);
    hmap_destroy(&consistent_queues);
    netdev_close(netdev_phy);
}

static void
destroy_qos_map(struct hmap *qos_map)
{
    struct qos_queue *qos_queue;
    HMAP_FOR_EACH_POP (qos_queue, node, qos_map) {
        free(qos_queue);
    }

    hmap_destroy(qos_map);
}

/*
 * Get the encap from the chassis for this port. The interface
 * may have an external_ids:encap-ip=<encap-ip> set; if so we
 * get the corresponding encap from the chassis.
 * If "encap-ip" external-ids is not set, we'll not bind the port
 * to any specific encap rec. and we'll pick up a tunnel port based on
 * the chassis name alone for the port.
 */
static struct sbrec_encap *
sbrec_get_port_encap(const struct sbrec_chassis *chassis_rec,
                     const struct ovsrec_interface *iface_rec)
{

    if (!iface_rec) {
        return NULL;
    }

    const char *encap_ip = smap_get(&iface_rec->external_ids, "encap-ip");
    if (!encap_ip) {
        return NULL;
    }

    struct sbrec_encap *best_encap = NULL;
    uint32_t best_type = 0;
    for (int i = 0; i < chassis_rec->n_encaps; i++) {
        if (!strcmp(chassis_rec->encaps[i]->ip, encap_ip)) {
            uint32_t tun_type = get_tunnel_type(chassis_rec->encaps[i]->type);
            if (tun_type > best_type) {
                best_type = tun_type;
                best_encap = chassis_rec->encaps[i];
            }
        }
    }
    return best_encap;
}

static void
add_localnet_egress_interface_mappings(
        const struct sbrec_port_binding *port_binding,
        struct shash *bridge_mappings, struct sset *egress_ifaces)
{
    const char *network = smap_get(&port_binding->options, "network_name");
    if (!network) {
        return;
    }

    struct ovsrec_bridge *br_ln = shash_find_data(bridge_mappings, network);
    if (!br_ln) {
        return;
    }

    /* Add egress-ifaces from the connected bridge */
    for (size_t i = 0; i < br_ln->n_ports; i++) {
        const struct ovsrec_port *port_rec = br_ln->ports[i];

        for (size_t j = 0; j < port_rec->n_interfaces; j++) {
            const struct ovsrec_interface *iface_rec;

            iface_rec = port_rec->interfaces[j];
            bool is_egress_iface = smap_get_bool(&iface_rec->external_ids,
                                                 "ovn-egress-iface", false);
            if (!is_egress_iface) {
                continue;
            }
            sset_add(egress_ifaces, iface_rec->name);
        }
    }
}

static bool
is_network_plugged(const struct sbrec_port_binding *binding_rec,
                   struct shash *bridge_mappings)
{
    const char *network = smap_get(&binding_rec->options, "network_name");
    return network ? !!shash_find_data(bridge_mappings, network) : false;
}

static void
update_ld_localnet_port(const struct sbrec_port_binding *binding_rec,
                        struct shash *bridge_mappings,
                        struct sset *egress_ifaces,
                        struct hmap *local_datapaths)
{
    /* Ignore localnet ports for unplugged networks. */
    if (!is_network_plugged(binding_rec, bridge_mappings)) {
        return;
    }

    add_localnet_egress_interface_mappings(binding_rec,
            bridge_mappings, egress_ifaces);

    struct local_datapath *ld
        = get_local_datapath(local_datapaths,
                             binding_rec->datapath->tunnel_key);
    if (!ld) {
        return;
    }

    if (ld->localnet_port && strcmp(ld->localnet_port->logical_port,
                                    binding_rec->logical_port)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "localnet port '%s' already set for datapath "
                     "'%"PRId64"', skipping the new port '%s'.",
                     ld->localnet_port->logical_port,
                     binding_rec->datapath->tunnel_key,
                     binding_rec->logical_port);
        return;
    }
    ld->localnet_port = binding_rec;
}

static void
update_local_lport_ids(struct sset *local_lport_ids,
                       const struct sbrec_port_binding *binding_rec)
{
        char buf[16];
        snprintf(buf, sizeof(buf), "%"PRId64"_%"PRId64,
                 binding_rec->datapath->tunnel_key,
                 binding_rec->tunnel_key);
        sset_add(local_lport_ids, buf);
}

static void
remove_local_lport_ids(const struct sbrec_port_binding *binding_rec,
                       struct sset *local_lport_ids)
{
        char buf[16];
        snprintf(buf, sizeof(buf), "%"PRId64"_%"PRId64,
                 binding_rec->datapath->tunnel_key,
                 binding_rec->tunnel_key);
        sset_find_and_delete(local_lport_ids, buf);
}

/* Local bindings. binding.c module binds the logical port (represented by
 * Port_Binding rows) and sets the 'chassis' column when it is sees the
 * OVS interface row (of type "" or "internal") with the
 * external_ids:iface-id=<logical_port name> set.
 *
 * This module also manages the other port_bindings.
 *
 * To better manage the local bindings with the associated OVS interfaces,
 * 'struct local_binding' is used. A shash of these local bindings is
 * maintained with the 'external_ids:iface-id' as the key to the shash.
 *
 * struct local_binding (defined in binding.h) has 3 main fields:
 *    - type
 *    - OVS interface row object
 *    - Port_Binding row object
 *
 * An instance of 'struct local_binding' can be one of 3 types.
 *
 *  BT_VIF:     Represent a local binding for an OVS interface of
 *              type "" or "internal" with the external_ids:iface-id
 *              set.
 *
 *              This can be a
 *                 * probable local binding - external_ids:iface-id is
 *                   set, but the corresponding Port_Binding row is not
 *                   created or is not visible to the local ovn-controller
 *                   instance.
 *
 *                 * a local binding - external_ids:iface-id is set and
 *                   which is already bound to the corresponding Port_Binding
 *                   row.
 *
 *              It maintains a list of children
 *              (of type BT_CONTAINER/BT_VIRTUAL) if any.
 *
 *  BT_CONTAINER:   Represents a local binding which has a parent of type
 *                  BT_VIF. Its Port_Binding row's 'parent' column is set to
 *                  its parent's Port_Binding. It shares the OVS interface row
 *                  with the parent.
 *                  Each ovn-controller when it sees a container Port_Binding,
 *                  it creates 'struct local_binding' for the parent
 *                  Port_Binding and for its even if the OVS interface row for
 *                  the parent is not present.
 *
 *  BT_VIRTUAL: Represents a local binding which has a parent of type BT_VIF.
 *              Its Port_Binding type is "virtual" and it shares the OVS
 *              interface row with the parent.
 *              Port_Binding of type "virtual" is claimed by pinctrl module
 *              when it sees the ARP packet from the parent's VIF.
 *
 *
 *  An object of 'struct local_binding' is created:
 *    - For each interface that has iface-id configured with the type - BT_VIF.
 *
 *    - For each container Port Binding (of type BT_CONTAINER) and its
 *      parent Port_Binding (of type BT_VIF), no matter if
 *      they are bound to this chassis i.e even if OVS interface row for the
 *      parent is not present.
 *
 *   - For each 'virtual' Port Binding (of type BT_VIRTUAL) provided its parent
 *     is bound to this chassis.
 */

static struct local_binding *
local_binding_create(const char *name, const struct ovsrec_interface *iface,
                     const struct sbrec_port_binding *pb,
                     enum local_binding_type type)
{
    struct local_binding *lbinding = xzalloc(sizeof *lbinding);
    lbinding->name = xstrdup(name);
    lbinding->type = type;
    lbinding->pb = pb;
    lbinding->iface = iface;
    shash_init(&lbinding->children);
    return lbinding;
}

static void
local_binding_add(struct shash *local_bindings, struct local_binding *lbinding)
{
    shash_add(local_bindings, lbinding->name, lbinding);
}

static void
local_binding_destroy(struct local_binding *lbinding)
{
    local_bindings_destroy(&lbinding->children);

    free(lbinding->name);
    free(lbinding);
}

void
local_bindings_init(struct shash *local_bindings)
{
    shash_init(local_bindings);
}

void
local_bindings_destroy(struct shash *local_bindings)
{
    struct shash_node *node, *next;
    SHASH_FOR_EACH_SAFE (node, next, local_bindings) {
        struct local_binding *lbinding = node->data;
        local_binding_destroy(lbinding);
        shash_delete(local_bindings, node);
    }

    shash_destroy(local_bindings);
}

static
void local_binding_delete(struct shash *local_bindings,
                          struct local_binding *lbinding)
{
    shash_find_and_delete(local_bindings, lbinding->name);
    local_binding_destroy(lbinding);
}

static void
local_binding_add_child(struct local_binding *lbinding,
                        struct local_binding *child)
{
    local_binding_add(&lbinding->children, child);
}

static struct local_binding *
local_binding_find_child(struct local_binding *lbinding,
                         const char *child_name)
{
    return local_binding_find(&lbinding->children, child_name);
}

static bool
is_lport_vif(const struct sbrec_port_binding *pb)
{
    return !pb->type[0];
}

static bool
is_lport_container(const struct sbrec_port_binding *pb)
{
    return !pb->type[0] && pb->parent_port && pb->parent_port[0];
}

/* Corresponds to each Port_Binding.type. */
enum en_lport_type {
    LP_UNKNOWN,
    LP_VIF,
    LP_PATCH,
    LP_L3GATEWAY,
    LP_LOCALNET,
    LP_LOCALPORT,
    LP_L2GATEWAY,
    LP_VTEP,
    LP_CHASSISREDIRECT,
    LP_VIRTUAL,
    LP_EXTERNAL,
    LP_REMOTE
};

static enum en_lport_type
get_lport_type(const struct sbrec_port_binding *pb)
{
    if (is_lport_vif(pb)) {
        return LP_VIF;
    } else if (!strcmp(pb->type, "patch")) {
        return LP_PATCH;
    } else if (!strcmp(pb->type, "chassisredirect")) {
        return LP_CHASSISREDIRECT;
    } else if (!strcmp(pb->type, "l3gateway")) {
        return LP_L3GATEWAY;
    } else if (!strcmp(pb->type, "localnet")) {
        return LP_LOCALNET;
    } else if (!strcmp(pb->type, "localport")) {
        return LP_LOCALPORT;
    } else if (!strcmp(pb->type, "l2gateway")) {
        return LP_L2GATEWAY;
    } else if (!strcmp(pb->type, "virtual")) {
        return LP_VIRTUAL;
    } else if (!strcmp(pb->type, "external")) {
        return LP_EXTERNAL;
    } else if (!strcmp(pb->type, "remote")) {
        return LP_REMOTE;
    } else if (!strcmp(pb->type, "vtep")) {
        return LP_VTEP;
    }

    return LP_UNKNOWN;
}

/* Returns false if lport is not claimed due to 'sb_readonly'.
 * Returns true otherwise.
 */
static bool
claim_lport(const struct sbrec_port_binding *pb,
            const struct sbrec_chassis *chassis_rec,
            const struct ovsrec_interface *iface_rec,
            bool sb_readonly)
{
    if (pb->chassis != chassis_rec) {
        if (sb_readonly) {
            return false;
        }

        if (pb->chassis) {
            VLOG_INFO("Changing chassis for lport %s from %s to %s.",
                    pb->logical_port, pb->chassis->name,
                    chassis_rec->name);
        } else {
            VLOG_INFO("Claiming lport %s for this chassis.", pb->logical_port);
        }
        for (int i = 0; i < pb->n_mac; i++) {
            VLOG_INFO("%s: Claiming %s", pb->logical_port, pb->mac[i]);
        }

        sbrec_port_binding_set_chassis(pb, chassis_rec);
    }

    /* Check if the port encap binding, if any, has changed */
    struct sbrec_encap *encap_rec =
        sbrec_get_port_encap(chassis_rec, iface_rec);
    if (encap_rec && pb->encap != encap_rec) {
        if (sb_readonly) {
            return false;
        }
        sbrec_port_binding_set_encap(pb, encap_rec);
    }

    return true;
}

/* Returns false if lport is not released due to 'sb_readonly'.
 * Returns true otherwise.
 */
static bool
release_lport(const struct sbrec_port_binding *pb, bool sb_readonly)
{
    if (!pb) {
        return true;
    }

    if (pb->encap) {
        if (sb_readonly) {
            return false;
        }
        sbrec_port_binding_set_encap(pb, NULL);
    }

    if (pb->chassis) {
        if (sb_readonly) {
            return false;
        }
        sbrec_port_binding_set_chassis(pb, NULL);
    }

    if (pb->virtual_parent) {
        if (sb_readonly) {
            return false;
        }
        sbrec_port_binding_set_virtual_parent(pb, NULL);
    }

    VLOG_INFO("Releasing lport %s from this chassis.", pb->logical_port);
    return true;
}

static bool
is_lbinding_set(struct local_binding *lbinding)
{
    return lbinding && lbinding->pb && lbinding->iface;
}

static bool
is_lbinding_this_chassis(struct local_binding *lbinding,
                         const struct sbrec_chassis *chassis)
{
    return lbinding && lbinding->pb && lbinding->pb->chassis == chassis;
}

static bool
can_bind_on_this_chassis(const struct sbrec_chassis *chassis_rec,
                         const char *requested_chassis)
{
    return !requested_chassis || !requested_chassis[0]
           || !strcmp(requested_chassis, chassis_rec->name)
           || !strcmp(requested_chassis, chassis_rec->hostname);
}

/* Returns 'true' if the 'lbinding' has children of type BT_CONTAINER,
 * 'false' otherwise. */
static bool
is_lbinding_container_parent(struct local_binding *lbinding)
{
    struct shash_node *node;
    SHASH_FOR_EACH (node, &lbinding->children) {
        struct local_binding *l = node->data;
        if (l->type == BT_CONTAINER) {
            return true;
        }
    }

    return false;
}

static bool
release_local_binding_children(const struct sbrec_chassis *chassis_rec,
                               struct local_binding *lbinding,
                               bool sb_readonly)
{
    struct shash_node *node;
    SHASH_FOR_EACH (node, &lbinding->children) {
        struct local_binding *l = node->data;
        if (is_lbinding_this_chassis(l, chassis_rec)) {
            if (!release_lport(l->pb, sb_readonly)) {
                return false;
            }
        }

        /* Clear the local bindings' 'pb' and 'iface'. */
        l->pb = NULL;
        l->iface = NULL;
    }

    return true;
}

static bool
release_local_binding(const struct sbrec_chassis *chassis_rec,
                      struct local_binding *lbinding, bool sb_readonly)
{
    if (!release_local_binding_children(chassis_rec, lbinding,
                                        sb_readonly)) {
        return false;
    }

    if (is_lbinding_this_chassis(lbinding, chassis_rec)) {
        return release_lport(lbinding->pb, sb_readonly);
    }

    lbinding->pb = NULL;
    lbinding->iface = NULL;
    return true;
}

static bool
consider_vif_lport_(const struct sbrec_port_binding *pb,
                    bool can_bind, const char *vif_chassis,
                    struct binding_ctx_in *b_ctx_in,
                    struct binding_ctx_out *b_ctx_out,
                    struct local_binding *lbinding,
                    struct hmap *qos_map)
{
    bool lbinding_set = is_lbinding_set(lbinding);
    if (lbinding_set) {
        if (can_bind) {
            /* We can claim the lport. */
            if (!claim_lport(pb, b_ctx_in->chassis_rec, lbinding->iface,
                             !b_ctx_in->ovnsb_idl_txn)){
                return false;
            }

            add_local_datapath(b_ctx_in->sbrec_datapath_binding_by_key,
                            b_ctx_in->sbrec_port_binding_by_datapath,
                            b_ctx_in->sbrec_port_binding_by_name,
                            pb->datapath, false, b_ctx_out->local_datapaths);
            update_local_lport_ids(b_ctx_out->local_lport_ids, pb);
            if (lbinding->iface && qos_map && b_ctx_in->ovs_idl_txn) {
                get_qos_params(pb, qos_map);
            }
        } else {
            /* We could, but can't claim the lport. */
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                VLOG_INFO_RL(&rl,
                            "Not claiming lport %s, chassis %s "
                            "requested-chassis %s",
                            pb->logical_port,
                            b_ctx_in->chassis_rec->name,
                            vif_chassis);
        }
    }

    if (pb->chassis == b_ctx_in->chassis_rec) {
        /* Release the lport if there is no lbinding. */
        if (!lbinding_set || !can_bind) {
            return release_lport(pb, !b_ctx_in->ovnsb_idl_txn);
        }
    }

    return true;
}

static bool
consider_vif_lport(const struct sbrec_port_binding *pb,
                   struct binding_ctx_in *b_ctx_in,
                   struct binding_ctx_out *b_ctx_out,
                   struct local_binding *lbinding,
                   struct hmap *qos_map)
{
    const char *vif_chassis = smap_get(&pb->options, "requested-chassis");
    bool can_bind = can_bind_on_this_chassis(b_ctx_in->chassis_rec,
                                             vif_chassis);

    if (!lbinding) {
        lbinding = local_binding_find(b_ctx_out->local_bindings,
                                      pb->logical_port);
    }

    if (lbinding) {
        lbinding->pb = pb;
    }

    return consider_vif_lport_(pb, can_bind, vif_chassis, b_ctx_in,
                               b_ctx_out, lbinding, qos_map);
}

static bool
consider_container_lport(const struct sbrec_port_binding *pb,
                         struct binding_ctx_in *b_ctx_in,
                         struct binding_ctx_out *b_ctx_out,
                         struct hmap *qos_map)
{
    struct local_binding *parent_lbinding;
    parent_lbinding = local_binding_find(b_ctx_out->local_bindings,
                                         pb->parent_port);

    if (!parent_lbinding) {
        /* There is no local_binding for parent port. Create it
         * without OVS interface row. This is the only exception
         * for creating the 'struct local_binding' object without
         * corresponding OVS interface row.
         *
         * This is required for the following reasons:
         *   - If a logical port P1 is created and then
         *     few container ports - C1, C2, .. are created first by CMS.
         *   - And later when OVS interface row  is created for P1, then
         *     we want the these container ports also be claimed by the
         *     chassis.
         * */
        parent_lbinding = local_binding_create(pb->parent_port, NULL, NULL,
                                               BT_VIF);
        local_binding_add(b_ctx_out->local_bindings, parent_lbinding);
    }

    struct local_binding *container_lbinding =
        local_binding_find_child(parent_lbinding, pb->logical_port);

    if (!container_lbinding) {
        container_lbinding = local_binding_create(pb->logical_port,
                                                  parent_lbinding->iface,
                                                  pb, BT_CONTAINER);
        local_binding_add_child(parent_lbinding, container_lbinding);
    } else {
        ovs_assert(container_lbinding->type == BT_CONTAINER);
        container_lbinding->pb = pb;
        container_lbinding->iface = parent_lbinding->iface;
    }

    if (!parent_lbinding->pb) {
        parent_lbinding->pb = lport_lookup_by_name(
            b_ctx_in->sbrec_port_binding_by_name, pb->parent_port);

        if (parent_lbinding->pb) {
            /* Its possible that the parent lport is not considered yet.
             * So call consider_vif_lport() to process it first. */
            consider_vif_lport(parent_lbinding->pb, b_ctx_in, b_ctx_out,
                               parent_lbinding, qos_map);
        } else {
            /* The parent lport doesn't exist. Call release_lport() to
             * release the container lport, if it was bound earlier. */
            if (is_lbinding_this_chassis(container_lbinding,
                                         b_ctx_in->chassis_rec)) {
               return release_lport(pb, !b_ctx_in->ovnsb_idl_txn);
            }

            return true;
        }
    }

    const char *vif_chassis = smap_get(&parent_lbinding->pb->options,
                                       "requested-chassis");
    bool can_bind = can_bind_on_this_chassis(b_ctx_in->chassis_rec,
                                             vif_chassis);

    return consider_vif_lport_(pb, can_bind, vif_chassis, b_ctx_in, b_ctx_out,
                               container_lbinding, qos_map);
}

static bool
consider_virtual_lport(const struct sbrec_port_binding *pb,
                       struct binding_ctx_in *b_ctx_in,
                       struct binding_ctx_out *b_ctx_out,
                       struct hmap *qos_map)
{
    struct local_binding * parent_lbinding =
        pb->virtual_parent ? local_binding_find(b_ctx_out->local_bindings,
                                                pb->virtual_parent)
        : NULL;

    if (parent_lbinding && !parent_lbinding->pb) {
        parent_lbinding->pb = lport_lookup_by_name(
            b_ctx_in->sbrec_port_binding_by_name, pb->virtual_parent);

        if (parent_lbinding->pb) {
            /* Its possible that the parent lport is not considered yet.
             * So call consider_vif_lport() to process it first. */
            consider_vif_lport(parent_lbinding->pb, b_ctx_in, b_ctx_out,
                               parent_lbinding, qos_map);
        }
    }

    struct local_binding *virtual_lbinding = NULL;
    if (is_lbinding_this_chassis(parent_lbinding, b_ctx_in->chassis_rec)) {
        virtual_lbinding =
            local_binding_find_child(parent_lbinding, pb->logical_port);
        if (!virtual_lbinding) {
            virtual_lbinding = local_binding_create(pb->logical_port,
                                                    parent_lbinding->iface,
                                                    pb, BT_VIRTUAL);
            local_binding_add_child(parent_lbinding, virtual_lbinding);
        } else {
            ovs_assert(virtual_lbinding->type == BT_VIRTUAL);
            virtual_lbinding->pb = pb;
            virtual_lbinding->iface = parent_lbinding->iface;
        }
    }

    return consider_vif_lport_(pb, true, NULL, b_ctx_in, b_ctx_out,
                               virtual_lbinding, qos_map);
}

/* Considers either claiming the lport or releasing the lport
 * for non VIF lports.
 */
static bool
consider_nonvif_lport_(const struct sbrec_port_binding *pb,
                       bool our_chassis,
                       bool has_local_l3gateway,
                       struct binding_ctx_in *b_ctx_in,
                       struct binding_ctx_out *b_ctx_out)
{
    if (our_chassis) {
        sset_add(b_ctx_out->local_lports, pb->logical_port);
        add_local_datapath(b_ctx_in->sbrec_datapath_binding_by_key,
                           b_ctx_in->sbrec_port_binding_by_datapath,
                           b_ctx_in->sbrec_port_binding_by_name,
                           pb->datapath, has_local_l3gateway,
                           b_ctx_out->local_datapaths);

        update_local_lport_ids(b_ctx_out->local_lport_ids, pb);
        return claim_lport(pb, b_ctx_in->chassis_rec, NULL,
                           !b_ctx_in->ovnsb_idl_txn);
    } else if (pb->chassis == b_ctx_in->chassis_rec) {
        return release_lport(pb, !b_ctx_in->ovnsb_idl_txn);
    }

    return true;
}

static bool
consider_l2gw_lport(const struct sbrec_port_binding *pb,
                    struct binding_ctx_in *b_ctx_in,
                    struct binding_ctx_out *b_ctx_out)
{
    const char *chassis_id = smap_get(&pb->options, "l2gateway-chassis");
    bool our_chassis = chassis_id && !strcmp(chassis_id,
                                             b_ctx_in->chassis_rec->name);

    return consider_nonvif_lport_(pb, our_chassis, false, b_ctx_in, b_ctx_out);
}

static bool
consider_l3gw_lport(const struct sbrec_port_binding *pb,
                    struct binding_ctx_in *b_ctx_in,
                    struct binding_ctx_out *b_ctx_out)
{
    const char *chassis_id = smap_get(&pb->options, "l3gateway-chassis");
    bool our_chassis = chassis_id && !strcmp(chassis_id,
                                             b_ctx_in->chassis_rec->name);

    return consider_nonvif_lport_(pb, our_chassis, true, b_ctx_in, b_ctx_out);
}

static void
consider_localnet_lport(const struct sbrec_port_binding *pb,
                        struct binding_ctx_in *b_ctx_in,
                        struct binding_ctx_out *b_ctx_out,
                        struct hmap *qos_map)
{
    /* Add all localnet ports to local_lports so that we allocate ct zones
     * for them. */
    sset_add(b_ctx_out->local_lports, pb->logical_port);
    if (qos_map && b_ctx_in->ovs_idl_txn) {
        get_qos_params(pb, qos_map);
    }

    update_local_lport_ids(b_ctx_out->local_lport_ids, pb);
}

static bool
consider_ha_lport(const struct sbrec_port_binding *pb,
                  struct binding_ctx_in *b_ctx_in,
                  struct binding_ctx_out *b_ctx_out)
{
    bool our_chassis = false;
    bool is_ha_chassis = ha_chassis_group_contains(pb->ha_chassis_group,
                                                   b_ctx_in->chassis_rec);
    our_chassis = is_ha_chassis &&
                  ha_chassis_group_is_active(pb->ha_chassis_group,
                                             b_ctx_in->active_tunnels,
                                             b_ctx_in->chassis_rec);

    if (is_ha_chassis && !our_chassis) {
        /* If the chassis_rec is part of ha_chassis_group associated with
         * the port_binding 'pb', we need to add to the local_datapath
         * in even if its not active.
         *
         * If the chassis is active, consider_nonvif_lport_() takes care
         * of adding the datapath of this 'pb' to local datapaths.
         * */
        add_local_datapath(b_ctx_in->sbrec_datapath_binding_by_key,
                           b_ctx_in->sbrec_port_binding_by_datapath,
                           b_ctx_in->sbrec_port_binding_by_name,
                           pb->datapath, false,
                           b_ctx_out->local_datapaths);
    }

    return consider_nonvif_lport_(pb, our_chassis, false, b_ctx_in, b_ctx_out);
}

static bool
consider_cr_lport(const struct sbrec_port_binding *pb,
                  struct binding_ctx_in *b_ctx_in,
                  struct binding_ctx_out *b_ctx_out)
{
    return consider_ha_lport(pb, b_ctx_in, b_ctx_out);
}

static bool
consider_external_lport(const struct sbrec_port_binding *pb,
                        struct binding_ctx_in *b_ctx_in,
                        struct binding_ctx_out *b_ctx_out)
{
    return consider_ha_lport(pb, b_ctx_in, b_ctx_out);
}

/*
 * Builds local_bindings from the OVS interfaces.
 */
static void
build_local_bindings(struct binding_ctx_in *b_ctx_in,
                     struct binding_ctx_out *b_ctx_out)
{
    int i;
    for (i = 0; i < b_ctx_in->br_int->n_ports; i++) {
        const struct ovsrec_port *port_rec = b_ctx_in->br_int->ports[i];
        const char *iface_id;
        int j;

        if (!strcmp(port_rec->name, b_ctx_in->br_int->name)) {
            continue;
        }

        for (j = 0; j < port_rec->n_interfaces; j++) {
            const struct ovsrec_interface *iface_rec;

            iface_rec = port_rec->interfaces[j];
            iface_id = smap_get(&iface_rec->external_ids, "iface-id");
            int64_t ofport = iface_rec->n_ofport ? *iface_rec->ofport : 0;

            if (iface_id && ofport > 0) {
                struct local_binding *lbinding =
                    local_binding_find(b_ctx_out->local_bindings, iface_id);
                if (!lbinding) {
                    lbinding = local_binding_create(iface_id, iface_rec, NULL,
                                                    BT_VIF);
                    local_binding_add(b_ctx_out->local_bindings, lbinding);
                } else {
                    static struct vlog_rate_limit rl =
                        VLOG_RATE_LIMIT_INIT(1, 5);
                    VLOG_WARN_RL(
                        &rl,
                        "Invalid configuration: iface-id is configured on "
                        "interfaces : [%s] and [%s]. Ignoring the "
                        "configuration on interface [%s]",
                        lbinding->iface->name, iface_rec->name,
                        iface_rec->name);
                    ovs_assert(lbinding->type == BT_VIF);
                }

                sset_add(b_ctx_out->local_lports, iface_id);
                smap_replace(b_ctx_out->local_iface_ids, iface_rec->name,
                             iface_id);
            }

            /* Check if this is a tunnel interface. */
            if (smap_get(&iface_rec->options, "remote_ip")) {
                const char *tunnel_iface
                    = smap_get(&iface_rec->status, "tunnel_egress_iface");
                if (tunnel_iface) {
                    sset_add(b_ctx_out->egress_ifaces, tunnel_iface);
                }
            }
        }
    }
}

void
binding_run(struct binding_ctx_in *b_ctx_in, struct binding_ctx_out *b_ctx_out)
{
    if (!b_ctx_in->chassis_rec) {
        return;
    }

    struct shash bridge_mappings = SHASH_INITIALIZER(&bridge_mappings);
    struct hmap qos_map;

    hmap_init(&qos_map);
    if (b_ctx_in->br_int) {
        build_local_bindings(b_ctx_in, b_ctx_out);
    }

    struct hmap *qos_map_ptr =
        !sset_is_empty(b_ctx_out->egress_ifaces) ? &qos_map : NULL;

    struct ovs_list localnet_lports = OVS_LIST_INITIALIZER(&localnet_lports);

    struct localnet_lport {
        struct ovs_list list_node;
        const struct sbrec_port_binding *pb;
    };

    /* Run through each binding record to see if it is resident on this
     * chassis and update the binding accordingly.  This includes both
     * directly connected logical ports and children of those ports
     * (which also includes virtual ports).
     */
    const struct sbrec_port_binding *pb;
    SBREC_PORT_BINDING_TABLE_FOR_EACH (pb,
                                       b_ctx_in->port_binding_table) {
        enum en_lport_type lport_type = get_lport_type(pb);

        switch (lport_type) {
        case LP_PATCH:
        case LP_LOCALPORT:
        case LP_VTEP:
            update_local_lport_ids(b_ctx_out->local_lport_ids, pb);
            break;

        case LP_VIF:
            if (is_lport_container(pb)) {
                consider_container_lport(pb, b_ctx_in, b_ctx_out, qos_map_ptr);
            } else {
                consider_vif_lport(pb, b_ctx_in, b_ctx_out, NULL, qos_map_ptr);
            }
            break;

        case LP_VIRTUAL:
            consider_virtual_lport(pb, b_ctx_in, b_ctx_out, qos_map_ptr);
            break;

        case LP_L2GATEWAY:
            consider_l2gw_lport(pb, b_ctx_in, b_ctx_out);
            break;

        case LP_L3GATEWAY:
            consider_l3gw_lport(pb, b_ctx_in, b_ctx_out);
            break;

        case LP_CHASSISREDIRECT:
            consider_cr_lport(pb, b_ctx_in, b_ctx_out);
            break;

        case LP_EXTERNAL:
            consider_external_lport(pb, b_ctx_in, b_ctx_out);
            break;

        case LP_LOCALNET: {
            consider_localnet_lport(pb, b_ctx_in, b_ctx_out, qos_map_ptr);
            struct localnet_lport *lnet_lport = xmalloc(sizeof *lnet_lport);
            lnet_lport->pb = pb;
            ovs_list_push_back(&localnet_lports, &lnet_lport->list_node);
            break;
        }

        case LP_REMOTE:
            /* Nothing to be done for REMOTE type. */
            break;

        case LP_UNKNOWN: {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
            VLOG_WARN_RL(&rl,
                         "Unknown port binding type [%s] for port binding "
                         "[%s]. Does ovn-controller needs update ?",
                         pb->type, pb->logical_port);
            break;
        }
        }
    }

    add_ovs_bridge_mappings(b_ctx_in->ovs_table, b_ctx_in->bridge_table,
                            &bridge_mappings);

    /* Run through each localnet lport list to see if it is a localnet port
     * on local datapaths discovered from above loop, and update the
     * corresponding local datapath accordingly. */
    struct localnet_lport *lnet_lport;
    LIST_FOR_EACH_POP (lnet_lport, list_node, &localnet_lports) {
        update_ld_localnet_port(lnet_lport->pb, &bridge_mappings,
                                b_ctx_out->egress_ifaces,
                                b_ctx_out->local_datapaths);
        free(lnet_lport);
    }

    shash_destroy(&bridge_mappings);

    if (!sset_is_empty(b_ctx_out->egress_ifaces)
        && set_noop_qos(b_ctx_in->ovs_idl_txn, b_ctx_in->port_table,
                        b_ctx_in->qos_table, b_ctx_out->egress_ifaces)) {
        const char *entry;
        SSET_FOR_EACH (entry, b_ctx_out->egress_ifaces) {
            setup_qos(entry, &qos_map);
        }
    }

    destroy_qos_map(&qos_map);
}

/* Returns true if the database is all cleaned up, false if more work is
 * required. */
bool
binding_cleanup(struct ovsdb_idl_txn *ovnsb_idl_txn,
                const struct sbrec_port_binding_table *port_binding_table,
                const struct sbrec_chassis *chassis_rec)
{
    if (!ovnsb_idl_txn) {
        return false;
    }
    if (!chassis_rec) {
        return true;
    }

    const struct sbrec_port_binding *binding_rec;
    bool any_changes = false;
    SBREC_PORT_BINDING_TABLE_FOR_EACH (binding_rec, port_binding_table) {
        if (binding_rec->chassis == chassis_rec) {
            if (binding_rec->encap) {
                sbrec_port_binding_set_encap(binding_rec, NULL);
            }
            sbrec_port_binding_set_chassis(binding_rec, NULL);
            any_changes = true;
        }
    }

    if (any_changes) {
        ovsdb_idl_txn_add_comment(
            ovnsb_idl_txn,
            "ovn-controller: removing all port bindings for '%s'",
            chassis_rec->name);
    }

    return !any_changes;
}

static void
add_local_datapath_peer_port(const struct sbrec_port_binding *pb,
                             struct binding_ctx_in *b_ctx_in,
                             struct binding_ctx_out *b_ctx_out,
                             struct local_datapath *ld)
{
    const char *peer_name = smap_get(&pb->options, "peer");
    if (strcmp(pb->type, "patch") || !peer_name) {
        return;
    }

    const struct sbrec_port_binding *peer;
    peer = lport_lookup_by_name(b_ctx_in->sbrec_port_binding_by_name,
                                peer_name);

    if (!peer || !peer->datapath) {
        return;
    }

    bool present = false;
    for (size_t i = 0; i < ld->n_peer_ports; i++) {
        if (ld->peer_ports[i].local == pb) {
            present = true;
            break;
        }
    }

    if (!present) {
        ld->n_peer_ports++;
        if (ld->n_peer_ports > ld->n_allocated_peer_ports) {
            ld->peer_ports =
                x2nrealloc(ld->peer_ports,
                           &ld->n_allocated_peer_ports,
                           sizeof *ld->peer_ports);
        }
        ld->peer_ports[ld->n_peer_ports - 1].local = pb;
        ld->peer_ports[ld->n_peer_ports - 1].remote = peer;
    }

    struct local_datapath *peer_ld =
        get_local_datapath(b_ctx_out->local_datapaths,
                           peer->datapath->tunnel_key);
    if (!peer_ld) {
        add_local_datapath__(b_ctx_in->sbrec_datapath_binding_by_key,
                             b_ctx_in->sbrec_port_binding_by_datapath,
                             b_ctx_in->sbrec_port_binding_by_name,
                             peer->datapath, false,
                             1, b_ctx_out->local_datapaths);
        return;
    }

    for (size_t i = 0; i < peer_ld->n_peer_ports; i++) {
        if (peer_ld->peer_ports[i].local == peer) {
            return;
        }
    }

    peer_ld->n_peer_ports++;
    if (peer_ld->n_peer_ports > peer_ld->n_allocated_peer_ports) {
        peer_ld->peer_ports =
            x2nrealloc(peer_ld->peer_ports,
                        &peer_ld->n_allocated_peer_ports,
                        sizeof *peer_ld->peer_ports);
    }
    peer_ld->peer_ports[peer_ld->n_peer_ports - 1].local = peer;
    peer_ld->peer_ports[peer_ld->n_peer_ports - 1].remote = pb;
}

static void
remove_local_datapath_peer_port(const struct sbrec_port_binding *pb,
                                struct local_datapath *ld,
                                struct hmap *local_datapaths)
{
    size_t i = 0;
    for (i = 0; i < ld->n_peer_ports; i++) {
        if (ld->peer_ports[i].local == pb) {
            break;
        }
    }

    if (i == ld->n_peer_ports) {
        return;
    }

    const struct sbrec_port_binding *peer = ld->peer_ports[i].remote;

    /* Possible improvement: We can shrint the allocated peer ports
     * if (ld->n_peer_ports < ld->n_allocated_peer_ports / 2).
     */
    ld->peer_ports[i].local = ld->peer_ports[ld->n_peer_ports - 1].local;
    ld->peer_ports[i].remote = ld->peer_ports[ld->n_peer_ports - 1].remote;
    ld->n_peer_ports--;

    struct local_datapath *peer_ld =
        get_local_datapath(local_datapaths, peer->datapath->tunnel_key);
    if (peer_ld) {
        /* Remove the peer port from the peer datapath. The peer
         * datapath also tries to remove its peer lport, but that would
         * be no-op. */
        remove_local_datapath_peer_port(peer, peer_ld, local_datapaths);
    }
}

static void
remove_pb_from_local_datapath(const struct sbrec_port_binding *pb,
                              const struct sbrec_chassis *chassis_rec,
                              struct binding_ctx_out *b_ctx_out,
                              struct local_datapath *ld)
{
    remove_local_lport_ids(pb, b_ctx_out->local_lport_ids);
    if (!strcmp(pb->type, "patch") ||
        !strcmp(pb->type, "l3gateway")) {
        remove_local_datapath_peer_port(pb, ld, b_ctx_out->local_datapaths);
    } else if (!strcmp(pb->type, "localnet")) {
        if (ld->localnet_port && !strcmp(ld->localnet_port->logical_port,
                                         pb->logical_port)) {
            ld->localnet_port = NULL;
        }
    } else if (!strcmp(pb->type, "l3gateway")) {
        const char *chassis_id = smap_get(&pb->options,
                                          "l3gateway-chassis");
        if (chassis_id && !strcmp(chassis_id, chassis_rec->name)) {
            ld->has_local_l3gateway = false;
        }
    }
}

/* Considers the ovs iface 'iface_rec' for claiming.
 * This function should be called if the external_ids:iface-id
 * and 'ofport' are set for the 'iface_rec'.
 *
 * If the local_binding for this 'iface_rec' already exists and its
 * already claimed, then this function will be no-op.
 */
static bool
consider_iface_claim(const struct ovsrec_interface *iface_rec,
                     const char *iface_id,
                     struct binding_ctx_in *b_ctx_in,
                     struct binding_ctx_out *b_ctx_out,
                     struct hmap *qos_map)
{
    sset_add(b_ctx_out->local_lports, iface_id);
    smap_replace(b_ctx_out->local_iface_ids, iface_rec->name, iface_id);

    struct local_binding *lbinding =
        local_binding_find(b_ctx_out->local_bindings, iface_id);

    if (!lbinding) {
        lbinding = local_binding_create(iface_id, iface_rec, NULL, BT_VIF);
        local_binding_add(b_ctx_out->local_bindings, lbinding);
    } else {
        lbinding->iface = iface_rec;
    }

    if (!lbinding->pb || strcmp(lbinding->name, lbinding->pb->logical_port)) {
        lbinding->pb = lport_lookup_by_name(
            b_ctx_in->sbrec_port_binding_by_name, lbinding->name);
        if (lbinding->pb && !strcmp(lbinding->pb->type, "virtual")) {
            lbinding->pb = NULL;
        }
    }

    if (lbinding->pb) {
        if (!consider_vif_lport(lbinding->pb, b_ctx_in, b_ctx_out,
                                lbinding, qos_map)) {
            return false;
        }
    }

    /* Update the child local_binding's iface (if any children) and try to
     *  claim the container lbindings. */
    struct shash_node *node;
    SHASH_FOR_EACH (node, &lbinding->children) {
        struct local_binding *child = node->data;
        child->iface = iface_rec;
        if (child->type == BT_CONTAINER) {
            if (!consider_container_lport(child->pb, b_ctx_in, b_ctx_out,
                                          qos_map)) {
                return false;
            }
        }
    }

    return true;
}

/* Considers the ovs interface 'iface_rec' for
 * releasing from this chassis if local_binding for this
 * 'iface_rec' (with 'iface_id' as key) already exists and
 * it is claimed by the chassis.
 *
 * The 'iface_id' could be cleared from the 'iface_rec'
 * and hence it is passed separately.
 *
 * This fuction should be called if
 *   - OVS interface 'iface_rec' is deleted.
 *   - OVS interface 'iface_rec' external_ids:iface-id is updated
 *     (with the old value being 'iface_id'.)
 *   - OVS interface ofport is reset to 0.
 * */
static bool
consider_iface_release(const struct ovsrec_interface *iface_rec,
                       const char *iface_id,
                       struct binding_ctx_in *b_ctx_in,
                       struct binding_ctx_out *b_ctx_out, bool *changed)
{
    struct local_binding *lbinding;
    lbinding = local_binding_find(b_ctx_out->local_bindings,
                                  iface_id);
    if (is_lbinding_this_chassis(lbinding, b_ctx_in->chassis_rec)) {

        if (!release_local_binding(b_ctx_in->chassis_rec, lbinding,
                                   !b_ctx_in->ovnsb_idl_txn)) {
            return false;
        }

        struct local_datapath *ld =
            get_local_datapath(b_ctx_out->local_datapaths,
                                lbinding->pb->datapath->tunnel_key);
        if (ld) {
            remove_pb_from_local_datapath(lbinding->pb,
                                            b_ctx_in->chassis_rec,
                                            b_ctx_out, ld);
        }

        /* Check if the lbinding has children of type PB_CONTAINER.
         * If so, don't delete the local_binding. */
        if (!is_lbinding_container_parent(lbinding)) {
            local_binding_delete(b_ctx_out->local_bindings, lbinding);
        }
        *changed = true;
    }

    sset_find_and_delete(b_ctx_out->local_lports, iface_id);
    smap_remove(b_ctx_out->local_iface_ids, iface_rec->name);

    return true;
}

static bool
is_iface_vif(const struct ovsrec_interface *iface_rec)
{
    if (iface_rec->type && iface_rec->type[0] &&
        strcmp(iface_rec->type, "internal")) {
        return false;
    }

    return true;
}

/* Returns true if the ovs interface changes were handled successfully,
 * false otherwise.
 */
bool
binding_handle_ovs_interface_changes(struct binding_ctx_in *b_ctx_in,
                                     struct binding_ctx_out *b_ctx_out,
                                     bool *changed)
{
    if (!b_ctx_in->chassis_rec) {
        return false;
    }

    bool handled = true;
    *changed = false;

    /* Run the tracked interfaces loop twice. One to handle deleted
     * changes. And another to handle add/update changes.
     * This will ensure correctness.
     *     *
     * We consider an OVS interface for release if one of the following
     * happens:
     *   1. OVS interface is deleted.
     *   2. external_ids:iface-id is cleared in which case we need to
     *      release the port binding corresponding to the previously set
     *      'old-iface-id' (which is stored in the smap
     *      'b_ctx_out->local_iface_ids').
     *   3. external_ids:iface-id is updated with a different value
     *      in which case we need to release the port binding corresponding
     *      to the previously set 'old-iface-id' (which is stored in the smap
     *      'b_ctx_out->local_iface_ids').
     *   4. ofport of the OVS interface is 0.
     *
     */
    const struct ovsrec_interface *iface_rec;
    OVSREC_INTERFACE_TABLE_FOR_EACH_TRACKED (iface_rec,
                                             b_ctx_in->iface_table) {
        if (!is_iface_vif(iface_rec)) {
            /* Right now are not handling ovs_interface changes of
             * other types. This can be enhanced to handle of
             * types - patch and tunnel. */
            handled = false;
            break;
        }

        const char *iface_id = smap_get(&iface_rec->external_ids, "iface-id");
        const char *old_iface_id = smap_get(b_ctx_out->local_iface_ids,
                                            iface_rec->name);
        const char *cleared_iface_id = NULL;
        if (!ovsrec_interface_is_deleted(iface_rec)) {
            int64_t ofport = iface_rec->n_ofport ? *iface_rec->ofport : 0;
            if (iface_id) {
                /* Check if iface_id is changed. If so we need to
                 * release the old port binding and associate this
                 * inteface to new port binding. */
                if (old_iface_id && strcmp(iface_id, old_iface_id)) {
                    cleared_iface_id = old_iface_id;
                } else if (!ofport) {
                    /* If ofport is 0, we need to release the iface if already
                     * claimed. */
                    cleared_iface_id = iface_id;
                }
            } else if (old_iface_id) {
                cleared_iface_id = old_iface_id;
            }
        } else {
            cleared_iface_id = iface_id;
        }

        if (cleared_iface_id) {
            handled = consider_iface_release(iface_rec, cleared_iface_id,
                                             b_ctx_in, b_ctx_out, changed);
        }

        if (!handled) {
            break;
        }
    }

    if (!handled) {
        /* This can happen if any non vif OVS interface is in the tracked
         * list or if consider_iface_release() returned false.
         * There is no need to process further. */
        return false;
    }

    struct hmap qos_map = HMAP_INITIALIZER(&qos_map);
    struct hmap *qos_map_ptr =
        sset_is_empty(b_ctx_out->egress_ifaces) ? NULL : &qos_map;

    /*
     * We consider an OVS interface for claiming if the following
     * 2 conditions are met:
     *   1. external_ids:iface-id is set.
     *   2. ofport of the OVS interface is > 0.
     *
     * So when an update of an OVS interface happens we see if these
     * conditions are still true. If so consider this interface for
     * claiming. This would be no-op if the update of the OVS interface
     * didn't change the above two fields.
     */
    OVSREC_INTERFACE_TABLE_FOR_EACH_TRACKED (iface_rec,
                                             b_ctx_in->iface_table) {
        /* Loop to handle create and update changes only. */
        if (ovsrec_interface_is_deleted(iface_rec)) {
            continue;
        }

        const char *iface_id = smap_get(&iface_rec->external_ids, "iface-id");
        int64_t ofport = iface_rec->n_ofport ? *iface_rec->ofport : 0;
        if (iface_id && ofport > 0) {
            *changed = true;
            handled = consider_iface_claim(iface_rec, iface_id, b_ctx_in,
                                           b_ctx_out, qos_map_ptr);
            if (!handled) {
                break;
            }
        }
    }

    if (handled && qos_map_ptr && set_noop_qos(b_ctx_in->ovs_idl_txn,
                                               b_ctx_in->port_table,
                                               b_ctx_in->qos_table,
                                               b_ctx_out->egress_ifaces)) {
        const char *entry;
        SSET_FOR_EACH (entry, b_ctx_out->egress_ifaces) {
            setup_qos(entry, &qos_map);
        }
    }

    destroy_qos_map(&qos_map);
    return handled;
}

static void
handle_deleted_lport(const struct sbrec_port_binding *pb,
                     struct binding_ctx_in *b_ctx_in,
                     struct binding_ctx_out *b_ctx_out)
{
    struct local_datapath *ld =
        get_local_datapath(b_ctx_out->local_datapaths,
                           pb->datapath->tunnel_key);
    if (ld) {
        remove_pb_from_local_datapath(pb, b_ctx_in->chassis_rec,
                                      b_ctx_out, ld);
    }
}

static struct local_binding *
get_lbinding_for_lport(const struct sbrec_port_binding *pb,
                       enum en_lport_type lport_type,
                       struct binding_ctx_out *b_ctx_out)
{
    ovs_assert(lport_type == LP_VIF || lport_type == LP_VIRTUAL);

    if (lport_type == LP_VIF && !is_lport_container(pb)) {
        return local_binding_find(b_ctx_out->local_bindings, pb->logical_port);
    }

    struct local_binding *parent_lbinding = NULL;

    if (lport_type == LP_VIRTUAL) {
        parent_lbinding = local_binding_find(b_ctx_out->local_bindings,
                                             pb->virtual_parent);
    } else {
        parent_lbinding = local_binding_find(b_ctx_out->local_bindings,
                                             pb->parent_port);
    }

    return parent_lbinding
           ? local_binding_find(&parent_lbinding->children, pb->logical_port)
           : NULL;
}

static bool
handle_deleted_vif_lport(const struct sbrec_port_binding *pb,
                         enum en_lport_type lport_type,
                         struct binding_ctx_in *b_ctx_in,
                         struct binding_ctx_out *b_ctx_out,
                         bool *changed)
{
    struct local_binding *lbinding =
        get_lbinding_for_lport(pb, lport_type, b_ctx_out);

    if (lbinding) {
        lbinding->pb = NULL;
        /* The port_binding 'pb' is deleted. So there is no need to
         * clear the 'chassis' column of 'pb'. But we need to do
         * for the local_binding's children. */
        if (lbinding->type == BT_VIF &&
                !release_local_binding_children(b_ctx_in->chassis_rec,
                                                lbinding,
                                                !b_ctx_in->ovnsb_idl_txn)) {
            return false;
        }

        *changed = true;
    }

    handle_deleted_lport(pb, b_ctx_in, b_ctx_out);
    return true;
}

static bool
handle_updated_vif_lport(const struct sbrec_port_binding *pb,
                         enum en_lport_type lport_type,
                         struct binding_ctx_in *b_ctx_in,
                         struct binding_ctx_out *b_ctx_out,
                         struct hmap *qos_map,
                         bool *changed)
{
    bool claimed = (pb->chassis == b_ctx_in->chassis_rec);
    bool handled = true;

    if (lport_type == LP_VIRTUAL) {
        handled = consider_virtual_lport(pb, b_ctx_in, b_ctx_out, qos_map);
    } else if (lport_type == LP_VIF && is_lport_container(pb)) {
        handled = consider_container_lport(pb, b_ctx_in, b_ctx_out, qos_map);
    } else {
        handled = consider_vif_lport(pb, b_ctx_in, b_ctx_out, NULL, qos_map);
    }

    if (!handled) {
        return false;
    }

    bool now_claimed = (pb->chassis == b_ctx_in->chassis_rec);
    bool changed_ = (claimed != now_claimed);

    if (changed_) {
        *changed = true;
    }

    if (lport_type == LP_VIRTUAL ||
        (lport_type == LP_VIF && is_lport_container(pb)) || !changed_) {
        return true;
    }

    struct local_binding *lbinding =
        local_binding_find(b_ctx_out->local_bindings, pb->logical_port);

    ovs_assert(lbinding);

    struct shash_node *node;
    SHASH_FOR_EACH (node, &lbinding->children) {
        struct local_binding *child = node->data;
        if (child->type == BT_CONTAINER) {
            handled = consider_container_lport(child->pb, b_ctx_in, b_ctx_out,
                                               qos_map);
            if (!handled) {
                return false;
            }
        }
    }

    return true;
}

/* Returns true if the port binding changes resulted in local binding
 * updates, false otherwise.
 */
bool
binding_handle_port_binding_changes(struct binding_ctx_in *b_ctx_in,
                                    struct binding_ctx_out *b_ctx_out,
                                    bool *changed)
{
    bool handled = true;
    *changed = false;

    const struct sbrec_port_binding *pb;

    /* Run the tracked port binding loop twice. One to handle deleted
     * changes. And another to handle add/update changes.
     * This will ensure correctness.
     */
    SBREC_PORT_BINDING_TABLE_FOR_EACH_TRACKED (pb,
                                               b_ctx_in->port_binding_table) {
        if (!sbrec_port_binding_is_deleted(pb)) {
            continue;
        }

        enum en_lport_type lport_type = get_lport_type(pb);
        if (lport_type == LP_VIF || lport_type == LP_VIRTUAL) {
            handled = handle_deleted_vif_lport(pb, lport_type, b_ctx_in,
                                                b_ctx_out, changed);
        } else {
            handle_deleted_lport(pb, b_ctx_in, b_ctx_out);
        }

        if (!handled) {
            break;
        }
    }

    if (!handled) {
        return false;
    }

    struct hmap qos_map = HMAP_INITIALIZER(&qos_map);
    struct hmap *qos_map_ptr =
        sset_is_empty(b_ctx_out->egress_ifaces) ? NULL : &qos_map;

    SBREC_PORT_BINDING_TABLE_FOR_EACH_TRACKED (pb,
                                               b_ctx_in->port_binding_table) {
        /* Loop to handle create and update changes only. */
        if (sbrec_port_binding_is_deleted(pb)) {
            continue;
        }

        enum en_lport_type lport_type = get_lport_type(pb);

        struct local_datapath *ld =
            get_local_datapath(b_ctx_out->local_datapaths,
                               pb->datapath->tunnel_key);

        switch (lport_type) {
        case LP_VIF:
        case LP_VIRTUAL:
            handled = handle_updated_vif_lport(pb, lport_type, b_ctx_in,
                                               b_ctx_out, qos_map_ptr,
                                               changed);
            break;

        case LP_PATCH:
        case LP_LOCALPORT:
        case LP_VTEP:
            *changed = true;
            update_local_lport_ids(b_ctx_out->local_lport_ids, pb);
            if (lport_type ==  LP_PATCH) {
                /* Add the peer datapath to the local datapaths if it's
                    * not present yet.
                    */
                if (ld) {
                    add_local_datapath_peer_port(pb, b_ctx_in,
                                                    b_ctx_out, ld);
                }
            }
            break;

        case LP_L2GATEWAY:
            *changed = true;
            handled = consider_l2gw_lport(pb, b_ctx_in, b_ctx_out);
            break;

        case LP_L3GATEWAY:
            *changed = true;
            handled = consider_l3gw_lport(pb, b_ctx_in, b_ctx_out);
            break;

        case LP_CHASSISREDIRECT:
            *changed = true;
            handled = consider_cr_lport(pb, b_ctx_in, b_ctx_out);
            break;

        case LP_EXTERNAL:
            *changed = true;
            handled = consider_external_lport(pb, b_ctx_in, b_ctx_out);
            break;

        case LP_LOCALNET: {
            *changed = true;
            consider_localnet_lport(pb, b_ctx_in, b_ctx_out, qos_map_ptr);

            struct shash bridge_mappings =
                SHASH_INITIALIZER(&bridge_mappings);
            add_ovs_bridge_mappings(b_ctx_in->ovs_table,
                                    b_ctx_in->bridge_table,
                                    &bridge_mappings);
            update_ld_localnet_port(pb, &bridge_mappings,
                                    b_ctx_out->egress_ifaces,
                                    b_ctx_out->local_datapaths);
            shash_destroy(&bridge_mappings);
            break;
        }

        case LP_REMOTE:
        case LP_UNKNOWN:
            break;
        }

        if (!handled) {
            break;
        }
    }

    if (handled && qos_map_ptr && set_noop_qos(b_ctx_in->ovs_idl_txn,
                                               b_ctx_in->port_table,
                                               b_ctx_in->qos_table,
                                               b_ctx_out->egress_ifaces)) {
        const char *entry;
        SSET_FOR_EACH (entry, b_ctx_out->egress_ifaces) {
            setup_qos(entry, &qos_map);
        }
    }

    destroy_qos_map(&qos_map);
    return handled;
}
