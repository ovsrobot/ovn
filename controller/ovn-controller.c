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

#include "ovn-controller.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "bfd.h"
#include "binding.h"
#include "chassis.h"
#include "command-line.h"
#include "compiler.h"
#include "daemon.h"
#include "dirs.h"
#include "openvswitch/dynamic-string.h"
#include "encaps.h"
#include "fatal-signal.h"
#include "ip-mcast.h"
#include "openvswitch/hmap.h"
#include "lflow.h"
#include "lib/vswitch-idl.h"
#include "lport.h"
#include "ofctrl.h"
#include "openvswitch/vconn.h"
#include "openvswitch/vlog.h"
#include "ovn/actions.h"
#include "lib/chassis-index.h"
#include "lib/extend-table.h"
#include "lib/ip-mcast-index.h"
#include "lib/mcast-group-index.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-util.h"
#include "patch.h"
#include "physical.h"
#include "pinctrl.h"
#include "openvswitch/poll-loop.h"
#include "lib/bitmap.h"
#include "lib/hash.h"
#include "smap.h"
#include "sset.h"
#include "stream-ssl.h"
#include "stream.h"
#include "unixctl.h"
#include "util.h"
#include "timeval.h"
#include "timer.h"
#include "stopwatch.h"
#include "lib/inc-proc-eng.h"

VLOG_DEFINE_THIS_MODULE(main);

static unixctl_cb_func ovn_controller_exit;
static unixctl_cb_func ct_zone_list;
static unixctl_cb_func extend_table_list;
static unixctl_cb_func inject_pkt;
static unixctl_cb_func ovn_controller_conn_show;
static unixctl_cb_func engine_recompute_cmd;

#define DEFAULT_BRIDGE_NAME "br-int"
#define DEFAULT_PROBE_INTERVAL_MSEC 5000
#define OFCTRL_DEFAULT_PROBE_INTERVAL_SEC 5

#define CONTROLLER_LOOP_STOPWATCH_NAME "ovn-controller-flow-generation"

static char *parse_options(int argc, char *argv[]);
OVS_NO_RETURN static void usage(void);

/* Pending packet to be injected into connected OVS. */
struct pending_pkt {
    /* Setting 'conn' indicates that a request is pending. */
    struct unixctl_conn *conn;
    char *flow_s;
};

struct local_datapath *
get_local_datapath(const struct hmap *local_datapaths, uint32_t tunnel_key)
{
    struct hmap_node *node = hmap_first_with_hash(local_datapaths, tunnel_key);
    return (node
            ? CONTAINER_OF(node, struct local_datapath, hmap_node)
            : NULL);
}

uint32_t
get_tunnel_type(const char *name)
{
    if (!strcmp(name, "geneve")) {
        return GENEVE;
    } else if (!strcmp(name, "stt")) {
        return STT;
    } else if (!strcmp(name, "vxlan")) {
        return VXLAN;
    }

    return 0;
}

const struct ovsrec_bridge *
get_bridge(const struct ovsrec_bridge_table *bridge_table, const char *br_name)
{
    const struct ovsrec_bridge *br;
    OVSREC_BRIDGE_TABLE_FOR_EACH (br, bridge_table) {
        if (!strcmp(br->name, br_name)) {
            return br;
        }
    }
    return NULL;
}

static void
update_sb_monitors(struct ovsdb_idl *ovnsb_idl,
                   const struct sbrec_chassis *chassis,
                   const struct sset *local_ifaces,
                   struct hmap *local_datapaths,
                   bool monitor_all)
{
    /* Monitor Port_Bindings rows for local interfaces and local datapaths.
     *
     * Monitor Logical_Flow, MAC_Binding, Multicast_Group, and DNS tables for
     * local datapaths.
     *
     * Monitor Controller_Event rows for local chassis.
     *
     * Monitor IP_Multicast for local datapaths.
     *
     * Monitor IGMP_Groups for local chassis.
     *
     * We always monitor patch ports because they allow us to see the linkages
     * between related logical datapaths.  That way, when we know that we have
     * a VIF on a particular logical switch, we immediately know to monitor all
     * the connected logical routers and logical switches. */
    struct ovsdb_idl_condition pb = OVSDB_IDL_CONDITION_INIT(&pb);
    struct ovsdb_idl_condition lf = OVSDB_IDL_CONDITION_INIT(&lf);
    struct ovsdb_idl_condition mb = OVSDB_IDL_CONDITION_INIT(&mb);
    struct ovsdb_idl_condition mg = OVSDB_IDL_CONDITION_INIT(&mg);
    struct ovsdb_idl_condition dns = OVSDB_IDL_CONDITION_INIT(&dns);
    struct ovsdb_idl_condition ce =  OVSDB_IDL_CONDITION_INIT(&ce);
    struct ovsdb_idl_condition ip_mcast = OVSDB_IDL_CONDITION_INIT(&ip_mcast);
    struct ovsdb_idl_condition igmp = OVSDB_IDL_CONDITION_INIT(&igmp);

    if (monitor_all) {
        ovsdb_idl_condition_add_clause_true(&pb);
        ovsdb_idl_condition_add_clause_true(&lf);
        ovsdb_idl_condition_add_clause_true(&mb);
        ovsdb_idl_condition_add_clause_true(&mg);
        ovsdb_idl_condition_add_clause_true(&dns);
        ovsdb_idl_condition_add_clause_true(&ce);
        ovsdb_idl_condition_add_clause_true(&ip_mcast);
        ovsdb_idl_condition_add_clause_true(&igmp);
        goto out;
    }

    sbrec_port_binding_add_clause_type(&pb, OVSDB_F_EQ, "patch");
    /* XXX: We can optimize this, if we find a way to only monitor
     * ports that have a Gateway_Chassis that point's to our own
     * chassis */
    sbrec_port_binding_add_clause_type(&pb, OVSDB_F_EQ, "chassisredirect");
    sbrec_port_binding_add_clause_type(&pb, OVSDB_F_EQ, "external");
    if (chassis) {
        /* This should be mostly redundant with the other clauses for port
         * bindings, but it allows us to catch any ports that are assigned to
         * us but should not be.  That way, we can clear their chassis
         * assignments. */
        sbrec_port_binding_add_clause_chassis(&pb, OVSDB_F_EQ,
                                              &chassis->header_.uuid);

        /* Ensure that we find out about l2gateway and l3gateway ports that
         * should be present on this chassis.  Otherwise, we might never find
         * out about those ports, if their datapaths don't otherwise have a VIF
         * in this chassis. */
        const char *id = chassis->name;
        const struct smap l2 = SMAP_CONST1(&l2, "l2gateway-chassis", id);
        sbrec_port_binding_add_clause_options(&pb, OVSDB_F_INCLUDES, &l2);
        const struct smap l3 = SMAP_CONST1(&l3, "l3gateway-chassis", id);
        sbrec_port_binding_add_clause_options(&pb, OVSDB_F_INCLUDES, &l3);

        sbrec_controller_event_add_clause_chassis(&ce, OVSDB_F_EQ,
                                                  &chassis->header_.uuid);
        sbrec_igmp_group_add_clause_chassis(&igmp, OVSDB_F_EQ,
                                            &chassis->header_.uuid);
    }
    if (local_ifaces) {
        const char *name;
        SSET_FOR_EACH (name, local_ifaces) {
            sbrec_port_binding_add_clause_logical_port(&pb, OVSDB_F_EQ, name);
            sbrec_port_binding_add_clause_parent_port(&pb, OVSDB_F_EQ, name);
        }
    }
    if (local_datapaths) {
        const struct local_datapath *ld;
        HMAP_FOR_EACH (ld, hmap_node, local_datapaths) {
            struct uuid *uuid = CONST_CAST(struct uuid *,
                                           &ld->datapath->header_.uuid);
            sbrec_port_binding_add_clause_datapath(&pb, OVSDB_F_EQ, uuid);
            sbrec_logical_flow_add_clause_logical_datapath(&lf, OVSDB_F_EQ,
                                                           uuid);
            sbrec_mac_binding_add_clause_datapath(&mb, OVSDB_F_EQ, uuid);
            sbrec_multicast_group_add_clause_datapath(&mg, OVSDB_F_EQ, uuid);
            sbrec_dns_add_clause_datapaths(&dns, OVSDB_F_INCLUDES, &uuid, 1);
            sbrec_ip_multicast_add_clause_datapath(&ip_mcast, OVSDB_F_EQ,
                                                   uuid);
        }
    }

out:
    sbrec_port_binding_set_condition(ovnsb_idl, &pb);
    sbrec_logical_flow_set_condition(ovnsb_idl, &lf);
    sbrec_mac_binding_set_condition(ovnsb_idl, &mb);
    sbrec_multicast_group_set_condition(ovnsb_idl, &mg);
    sbrec_dns_set_condition(ovnsb_idl, &dns);
    sbrec_controller_event_set_condition(ovnsb_idl, &ce);
    sbrec_ip_multicast_set_condition(ovnsb_idl, &ip_mcast);
    sbrec_igmp_group_set_condition(ovnsb_idl, &igmp);
    ovsdb_idl_condition_destroy(&pb);
    ovsdb_idl_condition_destroy(&lf);
    ovsdb_idl_condition_destroy(&mb);
    ovsdb_idl_condition_destroy(&mg);
    ovsdb_idl_condition_destroy(&dns);
    ovsdb_idl_condition_destroy(&ce);
    ovsdb_idl_condition_destroy(&ip_mcast);
    ovsdb_idl_condition_destroy(&igmp);
}

static const char *
br_int_name(const struct ovsrec_open_vswitch *cfg)
{
    return smap_get_def(&cfg->external_ids, "ovn-bridge", DEFAULT_BRIDGE_NAME);
}

static const struct ovsrec_bridge *
create_br_int(struct ovsdb_idl_txn *ovs_idl_txn,
              const struct ovsrec_open_vswitch_table *ovs_table)
{
    if (!ovs_idl_txn) {
        return NULL;
    }

    const struct ovsrec_open_vswitch *cfg;
    cfg = ovsrec_open_vswitch_table_first(ovs_table);
    if (!cfg) {
        return NULL;
    }
    const char *bridge_name = br_int_name(cfg);

    ovsdb_idl_txn_add_comment(ovs_idl_txn,
            "ovn-controller: creating integration bridge '%s'", bridge_name);

    struct ovsrec_interface *iface;
    iface = ovsrec_interface_insert(ovs_idl_txn);
    ovsrec_interface_set_name(iface, bridge_name);
    ovsrec_interface_set_type(iface, "internal");

    struct ovsrec_port *port;
    port = ovsrec_port_insert(ovs_idl_txn);
    ovsrec_port_set_name(port, bridge_name);
    ovsrec_port_set_interfaces(port, &iface, 1);

    struct ovsrec_bridge *bridge;
    bridge = ovsrec_bridge_insert(ovs_idl_txn);
    ovsrec_bridge_set_name(bridge, bridge_name);
    ovsrec_bridge_set_fail_mode(bridge, "secure");
    const struct smap oc = SMAP_CONST1(&oc, "disable-in-band", "true");
    ovsrec_bridge_set_other_config(bridge, &oc);
    ovsrec_bridge_set_ports(bridge, &port, 1);

    struct ovsrec_bridge **bridges;
    size_t bytes = sizeof *bridges * cfg->n_bridges;
    bridges = xmalloc(bytes + sizeof *bridges);
    memcpy(bridges, cfg->bridges, bytes);
    bridges[cfg->n_bridges] = bridge;
    ovsrec_open_vswitch_verify_bridges(cfg);
    ovsrec_open_vswitch_set_bridges(cfg, bridges, cfg->n_bridges + 1);
    free(bridges);

    return bridge;
}

static const struct ovsrec_bridge *
get_br_int(const struct ovsrec_bridge_table *bridge_table,
           const struct ovsrec_open_vswitch_table *ovs_table)
{
    const struct ovsrec_open_vswitch *cfg;
    cfg = ovsrec_open_vswitch_table_first(ovs_table);
    if (!cfg) {
        return NULL;
    }

    return get_bridge(bridge_table, br_int_name(cfg));
}

static const struct ovsrec_bridge *
process_br_int(struct ovsdb_idl_txn *ovs_idl_txn,
               const struct ovsrec_bridge_table *bridge_table,
               const struct ovsrec_open_vswitch_table *ovs_table)
{
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table,
                                                    ovs_table);
    if (!br_int) {
        br_int = create_br_int(ovs_idl_txn, ovs_table);
    }
    if (br_int && ovs_idl_txn) {
        const struct ovsrec_open_vswitch *cfg;
        cfg = ovsrec_open_vswitch_table_first(ovs_table);
        ovs_assert(cfg);
        const char *datapath_type = smap_get(&cfg->external_ids,
                                             "ovn-bridge-datapath-type");
        /* Check for the datapath_type and set it only if it is defined in
         * cfg. */
        if (datapath_type && strcmp(br_int->datapath_type, datapath_type)) {
            ovsrec_bridge_set_datapath_type(br_int, datapath_type);
        }
    }
    return br_int;
}

static const char *
get_ovs_chassis_id(const struct ovsrec_open_vswitch_table *ovs_table)
{
    const struct ovsrec_open_vswitch *cfg
        = ovsrec_open_vswitch_table_first(ovs_table);
    const char *chassis_id = cfg ? smap_get(&cfg->external_ids, "system-id")
                                 : NULL;

    if (!chassis_id) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "'system-id' in Open_vSwitch database is missing.");
    }

    return chassis_id;
}

/* Iterate address sets in the southbound database.  Create and update the
 * corresponding symtab entries as necessary. */
static void
addr_sets_init(const struct sbrec_address_set_table *address_set_table,
               struct shash *addr_sets)
{
    const struct sbrec_address_set *as;
    SBREC_ADDRESS_SET_TABLE_FOR_EACH (as, address_set_table) {
        expr_const_sets_add(addr_sets, as->name,
                            (const char *const *) as->addresses,
                            as->n_addresses, true);
    }
}

static void
addr_sets_update(const struct sbrec_address_set_table *address_set_table,
                 struct shash *addr_sets, struct sset *new,
                 struct sset *deleted, struct sset *updated)
{
    const struct sbrec_address_set *as;
    SBREC_ADDRESS_SET_TABLE_FOR_EACH_TRACKED (as, address_set_table) {
        if (sbrec_address_set_is_deleted(as)) {
            expr_const_sets_remove(addr_sets, as->name);
            sset_add(deleted, as->name);
        } else {
            expr_const_sets_add(addr_sets, as->name,
                                (const char *const *) as->addresses,
                                as->n_addresses, true);
            if (sbrec_address_set_is_new(as)) {
                sset_add(new, as->name);
            } else {
                sset_add(updated, as->name);
            }
        }
    }
}

/* Iterate port groups in the southbound database.  Create and update the
 * corresponding symtab entries as necessary. */
 static void
port_groups_init(const struct sbrec_port_group_table *port_group_table,
                 struct shash *port_groups)
{
    const struct sbrec_port_group *pg;
    SBREC_PORT_GROUP_TABLE_FOR_EACH (pg, port_group_table) {
        expr_const_sets_add(port_groups, pg->name,
                            (const char *const *) pg->ports,
                            pg->n_ports, false);
    }
}

static void
port_groups_update(const struct sbrec_port_group_table *port_group_table,
                   struct shash *port_groups, struct sset *new,
                   struct sset *deleted, struct sset *updated)
{
    const struct sbrec_port_group *pg;
    SBREC_PORT_GROUP_TABLE_FOR_EACH_TRACKED (pg, port_group_table) {
        if (sbrec_port_group_is_deleted(pg)) {
            expr_const_sets_remove(port_groups, pg->name);
            sset_add(deleted, pg->name);
        } else {
            expr_const_sets_add(port_groups, pg->name,
                                (const char *const *) pg->ports,
                                pg->n_ports, false);
            if (sbrec_port_group_is_new(pg)) {
                sset_add(new, pg->name);
            } else {
                sset_add(updated, pg->name);
            }
        }
    }
}

static void
update_ssl_config(const struct ovsrec_ssl_table *ssl_table)
{
    const struct ovsrec_ssl *ssl = ovsrec_ssl_table_first(ssl_table);

    if (ssl) {
        stream_ssl_set_key_and_cert(ssl->private_key, ssl->certificate);
        stream_ssl_set_ca_cert_file(ssl->ca_cert, ssl->bootstrap_ca_cert);
    }
}

static int
get_ofctrl_probe_interval(struct ovsdb_idl *ovs_idl)
{
    const struct ovsrec_open_vswitch *cfg = ovsrec_open_vswitch_first(ovs_idl);
    return !cfg ? OFCTRL_DEFAULT_PROBE_INTERVAL_SEC :
                  smap_get_int(&cfg->external_ids,
                               "ovn-openflow-probe-interval",
                               OFCTRL_DEFAULT_PROBE_INTERVAL_SEC);
}

/* Retrieves the pointer to the OVN Southbound database from 'ovs_idl' and
 * updates 'sbdb_idl' with that pointer. */
static void
update_sb_db(struct ovsdb_idl *ovs_idl, struct ovsdb_idl *ovnsb_idl,
             bool *monitor_all_p)
{
    const struct ovsrec_open_vswitch *cfg = ovsrec_open_vswitch_first(ovs_idl);
    if (!cfg) {
        return;
    }

    /* Set remote based on user configuration. */
    const char *remote = smap_get(&cfg->external_ids, "ovn-remote");
    ovsdb_idl_set_remote(ovnsb_idl, remote, true);

    /* Set probe interval, based on user configuration and the remote. */
    int default_interval = (remote && !stream_or_pstream_needs_probes(remote)
                            ? 0 : DEFAULT_PROBE_INTERVAL_MSEC);
    int interval = smap_get_int(&cfg->external_ids,
                                "ovn-remote-probe-interval", default_interval);
    ovsdb_idl_set_probe_interval(ovnsb_idl, interval);

    bool monitor_all = smap_get_bool(&cfg->external_ids, "ovn-monitor-all",
                                     false);
    if (monitor_all) {
        /* Always call update_sb_monitors when monitor_all is true.
         * Otherwise, don't call it here, because there would be unnecessary
         * extra cost. Instead, it is called after the engine execution only
         * when it is necessary. */
        update_sb_monitors(ovnsb_idl, NULL, NULL, NULL, true);
    }
    if (monitor_all_p) {
        *monitor_all_p = monitor_all;
    }
}

static void
update_ct_zones(const struct sset *lports, const struct hmap *local_datapaths,
                struct simap *ct_zones, unsigned long *ct_zone_bitmap,
                struct shash *pending_ct_zones)
{
    struct simap_node *ct_zone, *ct_zone_next;
    int scan_start = 1;
    const char *user;
    struct sset all_users = SSET_INITIALIZER(&all_users);

    SSET_FOR_EACH(user, lports) {
        sset_add(&all_users, user);
    }

    /* Local patched datapath (gateway routers) need zones assigned. */
    const struct local_datapath *ld;
    HMAP_FOR_EACH (ld, hmap_node, local_datapaths) {
        /* XXX Add method to limit zone assignment to logical router
         * datapaths with NAT */
        char *dnat = alloc_nat_zone_key(&ld->datapath->header_.uuid, "dnat");
        char *snat = alloc_nat_zone_key(&ld->datapath->header_.uuid, "snat");
        sset_add(&all_users, dnat);
        sset_add(&all_users, snat);
        free(dnat);
        free(snat);
    }

    /* Delete zones that do not exist in above sset. */
    SIMAP_FOR_EACH_SAFE(ct_zone, ct_zone_next, ct_zones) {
        if (!sset_contains(&all_users, ct_zone->name)) {
            VLOG_DBG("removing ct zone %"PRId32" for '%s'",
                     ct_zone->data, ct_zone->name);

            struct ct_zone_pending_entry *pending = xmalloc(sizeof *pending);
            pending->state = CT_ZONE_DB_QUEUED; /* Skip flushing zone. */
            pending->zone = ct_zone->data;
            pending->add = false;
            shash_add(pending_ct_zones, ct_zone->name, pending);

            bitmap_set0(ct_zone_bitmap, ct_zone->data);
            simap_delete(ct_zones, ct_zone);
        }
    }

    /* xxx This is wasteful to assign a zone to each port--even if no
     * xxx security policy is applied. */

    /* Assign a unique zone id for each logical port and two zones
     * to a gateway router. */
    SSET_FOR_EACH(user, &all_users) {
        int zone;

        if (simap_contains(ct_zones, user)) {
            continue;
        }

        /* We assume that there are 64K zones and that we own them all. */
        zone = bitmap_scan(ct_zone_bitmap, 0, scan_start, MAX_CT_ZONES + 1);
        if (zone == MAX_CT_ZONES + 1) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "exhausted all ct zones");
            return;
        }
        scan_start = zone + 1;

        VLOG_DBG("assigning ct zone %"PRId32" to '%s'", zone, user);

        struct ct_zone_pending_entry *pending = xmalloc(sizeof *pending);
        pending->state = CT_ZONE_OF_QUEUED;
        pending->zone = zone;
        pending->add = true;
        shash_add(pending_ct_zones, user, pending);

        bitmap_set1(ct_zone_bitmap, zone);
        simap_put(ct_zones, user, zone);
    }

    sset_destroy(&all_users);
}

static void
commit_ct_zones(const struct ovsrec_bridge *br_int,
                struct shash *pending_ct_zones)
{
    struct smap ct_add_ids = SMAP_INITIALIZER(&ct_add_ids);
    struct sset ct_del_ids = SSET_INITIALIZER(&ct_del_ids);

    struct shash_node *iter;
    SHASH_FOR_EACH(iter, pending_ct_zones) {
        struct ct_zone_pending_entry *ctzpe = iter->data;

        /* The transaction is open, so any pending entries in the
         * CT_ZONE_DB_QUEUED must be sent and any in CT_ZONE_DB_QUEUED
         * need to be retried. */
        if (ctzpe->state != CT_ZONE_DB_QUEUED
            && ctzpe->state != CT_ZONE_DB_SENT) {
            continue;
        }

        char *user_str = xasprintf("ct-zone-%s", iter->name);
        if (ctzpe->add) {
            char *zone_str = xasprintf("%"PRId32, ctzpe->zone);
            struct smap_node *node =
                smap_get_node(&br_int->external_ids, user_str);
            if (!node || strcmp(node->value, zone_str)) {
                smap_add_nocopy(&ct_add_ids, user_str, zone_str);
                user_str = NULL;
                zone_str = NULL;
            }
            free(zone_str);
        } else {
            if (smap_get(&br_int->external_ids, user_str)) {
                sset_add(&ct_del_ids, user_str);
            }
        }
        free(user_str);

        ctzpe->state = CT_ZONE_DB_SENT;
    }

    /* Update the bridge external IDs only if really needed (i.e., we must
     * add a value or delete one). Rebuilding the external IDs map at
     * every run is a costly operation when having lots of ct_zones.
     */
    if (!smap_is_empty(&ct_add_ids) || !sset_is_empty(&ct_del_ids)) {
        struct smap new_ids = SMAP_INITIALIZER(&new_ids);

        struct smap_node *id;
        SMAP_FOR_EACH (id, &br_int->external_ids) {
            if (sset_find_and_delete(&ct_del_ids, id->key)) {
                continue;
            }

            if (smap_get(&ct_add_ids, id->key)) {
                continue;
            }

            smap_add(&new_ids, id->key, id->value);
        }

        struct smap_node *next_id;
        SMAP_FOR_EACH_SAFE (id, next_id, &ct_add_ids) {
            smap_replace(&new_ids, id->key, id->value);
            smap_remove_node(&ct_add_ids, id);
        }

        ovsrec_bridge_verify_external_ids(br_int);
        ovsrec_bridge_set_external_ids(br_int, &new_ids);
        smap_destroy(&new_ids);
    }

    ovs_assert(smap_is_empty(&ct_add_ids));
    ovs_assert(sset_is_empty(&ct_del_ids));

    smap_destroy(&ct_add_ids);
    sset_destroy(&ct_del_ids);
}

static void
restore_ct_zones(const struct ovsrec_bridge_table *bridge_table,
                 const struct ovsrec_open_vswitch_table *ovs_table,
                 struct simap *ct_zones, unsigned long *ct_zone_bitmap)
{
    const struct ovsrec_open_vswitch *cfg;
    cfg = ovsrec_open_vswitch_table_first(ovs_table);
    if (!cfg) {
        return;
    }

    const struct ovsrec_bridge *br_int;
    br_int = get_bridge(bridge_table, br_int_name(cfg));
    if (!br_int) {
        /* If the integration bridge hasn't been defined, assume that
         * any existing ct-zone definitions aren't valid. */
        return;
    }

    struct smap_node *node;
    SMAP_FOR_EACH(node, &br_int->external_ids) {
        if (strncmp(node->key, "ct-zone-", 8)) {
            continue;
        }

        const char *user = node->key + 8;
        int zone = atoi(node->value);

        if (user[0] && zone) {
            VLOG_DBG("restoring ct zone %"PRId32" for '%s'", zone, user);
            bitmap_set1(ct_zone_bitmap, zone);
            simap_put(ct_zones, user, zone);
        }
    }
}

static int64_t
get_nb_cfg(const struct sbrec_sb_global_table *sb_global_table)
{
    const struct sbrec_sb_global *sb
        = sbrec_sb_global_table_first(sb_global_table);
    return sb ? sb->nb_cfg : 0;
}

static const char *
get_transport_zones(const struct ovsrec_open_vswitch_table *ovs_table)
{
    const struct ovsrec_open_vswitch *cfg
        = ovsrec_open_vswitch_table_first(ovs_table);
    return smap_get_def(&cfg->external_ids, "ovn-transport-zones", "");
}

static void
ctrl_register_ovs_idl(struct ovsdb_idl *ovs_idl)
{
    /* We do not monitor all tables by default, so modules must register
     * their interest explicitly. */
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_open_vswitch);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_open_vswitch_col_external_ids);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_open_vswitch_col_bridges);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_interface);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_bfd);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_bfd_status);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_type);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_options);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_ofport);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_port);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_interfaces);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_external_ids);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_bridge);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_ports);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_name);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_fail_mode);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_other_config);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_external_ids);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_ssl);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_ssl_col_bootstrap_ca_cert);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_ssl_col_ca_cert);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_ssl_col_certificate);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_ssl_col_private_key);
    chassis_register_ovs_idl(ovs_idl);
    encaps_register_ovs_idl(ovs_idl);
    binding_register_ovs_idl(ovs_idl);
    bfd_register_ovs_idl(ovs_idl);
    physical_register_ovs_idl(ovs_idl);
}

#define SB_NODES \
    SB_NODE(chassis, "chassis") \
    SB_NODE(encap, "encap") \
    SB_NODE(address_set, "address_set") \
    SB_NODE(port_group, "port_group") \
    SB_NODE(multicast_group, "multicast_group") \
    SB_NODE(datapath_binding, "datapath_binding") \
    SB_NODE(port_binding, "port_binding") \
    SB_NODE(mac_binding, "mac_binding") \
    SB_NODE(logical_flow, "logical_flow") \
    SB_NODE(dhcp_options, "dhcp_options") \
    SB_NODE(dhcpv6_options, "dhcpv6_options") \
    SB_NODE(dns, "dns")

enum sb_engine_node {
#define SB_NODE(NAME, NAME_STR) SB_##NAME,
    SB_NODES
#undef SB_NODE
};

#define SB_NODE(NAME, NAME_STR) ENGINE_FUNC_SB(NAME);
    SB_NODES
#undef SB_NODE

#define OVS_NODES \
    OVS_NODE(open_vswitch, "open_vswitch") \
    OVS_NODE(bridge, "bridge") \
    OVS_NODE(port, "port") \
    OVS_NODE(interface, "interface") \
    OVS_NODE(qos, "qos")

enum ovs_engine_node {
#define OVS_NODE(NAME, NAME_STR) OVS_##NAME,
    OVS_NODES
#undef OVS_NODE
};

#define OVS_NODE(NAME, NAME_STR) ENGINE_FUNC_OVS(NAME);
    OVS_NODES
#undef OVS_NODE

struct ed_type_ofctrl_is_connected {
    bool connected;
};

static void *
en_ofctrl_is_connected_init(struct engine_node *node OVS_UNUSED,
                            struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_ofctrl_is_connected *data = xzalloc(sizeof *data);
    return data;
}

static void
en_ofctrl_is_connected_cleanup(void *data OVS_UNUSED)
{
}

static void
en_ofctrl_is_connected_run(struct engine_node *node, void *data)
{
    struct ed_type_ofctrl_is_connected *of_data = data;
    if (of_data->connected != ofctrl_is_connected()) {
        of_data->connected = !of_data->connected;
        engine_set_node_state(node, EN_UPDATED);
        return;
    }
    engine_set_node_state(node, EN_VALID);
}

struct ed_type_addr_sets {
    struct shash addr_sets;
    bool change_tracked;
    struct sset new;
    struct sset deleted;
    struct sset updated;
};

static void *
en_addr_sets_init(struct engine_node *node OVS_UNUSED,
                  struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_addr_sets *as = xzalloc(sizeof *as);

    shash_init(&as->addr_sets);
    as->change_tracked = false;
    sset_init(&as->new);
    sset_init(&as->deleted);
    sset_init(&as->updated);
    return as;
}

static void
en_addr_sets_cleanup(void *data)
{
    struct ed_type_addr_sets *as = data;
    expr_const_sets_destroy(&as->addr_sets);
    shash_destroy(&as->addr_sets);
    sset_destroy(&as->new);
    sset_destroy(&as->deleted);
    sset_destroy(&as->updated);
}

static void
en_addr_sets_run(struct engine_node *node, void *data)
{
    struct ed_type_addr_sets *as = data;

    sset_clear(&as->new);
    sset_clear(&as->deleted);
    sset_clear(&as->updated);
    expr_const_sets_destroy(&as->addr_sets);

    struct sbrec_address_set_table *as_table =
        (struct sbrec_address_set_table *)EN_OVSDB_GET(
            engine_get_input("SB_address_set", node));

    addr_sets_init(as_table, &as->addr_sets);

    as->change_tracked = false;
    engine_set_node_state(node, EN_UPDATED);
}

static bool
addr_sets_sb_address_set_handler(struct engine_node *node, void *data)
{
    struct ed_type_addr_sets *as = data;

    sset_clear(&as->new);
    sset_clear(&as->deleted);
    sset_clear(&as->updated);

    struct sbrec_address_set_table *as_table =
        (struct sbrec_address_set_table *)EN_OVSDB_GET(
            engine_get_input("SB_address_set", node));

    addr_sets_update(as_table, &as->addr_sets, &as->new,
                     &as->deleted, &as->updated);

    if (!sset_is_empty(&as->new) || !sset_is_empty(&as->deleted) ||
            !sset_is_empty(&as->updated)) {
        engine_set_node_state(node, EN_UPDATED);
    } else {
        engine_set_node_state(node, EN_VALID);
    }

    as->change_tracked = true;
    return true;
}

struct ed_type_port_groups{
    struct shash port_groups;
    bool change_tracked;
    struct sset new;
    struct sset deleted;
    struct sset updated;
};

static void *
en_port_groups_init(struct engine_node *node OVS_UNUSED,
                    struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_port_groups *pg = xzalloc(sizeof *pg);

    shash_init(&pg->port_groups);
    pg->change_tracked = false;
    sset_init(&pg->new);
    sset_init(&pg->deleted);
    sset_init(&pg->updated);
    return pg;
}

static void
en_port_groups_cleanup(void *data)
{
    struct ed_type_port_groups *pg = data;
    expr_const_sets_destroy(&pg->port_groups);
    shash_destroy(&pg->port_groups);
    sset_destroy(&pg->new);
    sset_destroy(&pg->deleted);
    sset_destroy(&pg->updated);
}

static void
en_port_groups_run(struct engine_node *node, void *data)
{
    struct ed_type_port_groups *pg = data;

    sset_clear(&pg->new);
    sset_clear(&pg->deleted);
    sset_clear(&pg->updated);
    expr_const_sets_destroy(&pg->port_groups);

    struct sbrec_port_group_table *pg_table =
        (struct sbrec_port_group_table *)EN_OVSDB_GET(
            engine_get_input("SB_port_group", node));

    port_groups_init(pg_table, &pg->port_groups);

    pg->change_tracked = false;
    engine_set_node_state(node, EN_UPDATED);
}

static bool
port_groups_sb_port_group_handler(struct engine_node *node, void *data)
{
    struct ed_type_port_groups *pg = data;

    sset_clear(&pg->new);
    sset_clear(&pg->deleted);
    sset_clear(&pg->updated);

    struct sbrec_port_group_table *pg_table =
        (struct sbrec_port_group_table *)EN_OVSDB_GET(
            engine_get_input("SB_port_group", node));

    port_groups_update(pg_table, &pg->port_groups, &pg->new,
                     &pg->deleted, &pg->updated);

    if (!sset_is_empty(&pg->new) || !sset_is_empty(&pg->deleted) ||
            !sset_is_empty(&pg->updated)) {
        engine_set_node_state(node, EN_UPDATED);
    } else {
        engine_set_node_state(node, EN_VALID);
    }

    pg->change_tracked = true;
    return true;
}

struct ed_type_runtime_data {
    /* Contains "struct local_datapath" nodes. */
    struct hmap local_datapaths;

    /* Contains "struct local_binding" nodes. */
    struct shash local_bindings;

    /* Contains the name of each logical port resident on the local
     * hypervisor.  These logical ports include the VIFs (and their child
     * logical ports, if any) that belong to VMs running on the hypervisor,
     * l2gateway ports for which options:l2gateway-chassis designates the
     * local hypervisor, and localnet ports. */
    struct sset local_lports;

    /* Contains the same ports as local_lports, but in the format:
     * <datapath-tunnel-key>_<port-tunnel-key> */
    struct sset local_lport_ids;
    struct sset active_tunnels;

    struct sset egress_ifaces;
    struct smap local_iface_ids;
};

static void *
en_runtime_data_init(struct engine_node *node OVS_UNUSED,
                     struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_runtime_data *data = xzalloc(sizeof *data);

    hmap_init(&data->local_datapaths);
    sset_init(&data->local_lports);
    sset_init(&data->local_lport_ids);
    sset_init(&data->active_tunnels);
    sset_init(&data->egress_ifaces);
    smap_init(&data->local_iface_ids);
    local_bindings_init(&data->local_bindings);
    return data;
}

static void
en_runtime_data_cleanup(void *data)
{
    struct ed_type_runtime_data *rt_data = data;

    sset_destroy(&rt_data->local_lports);
    sset_destroy(&rt_data->local_lport_ids);
    sset_destroy(&rt_data->active_tunnels);
    sset_destroy(&rt_data->egress_ifaces);
    smap_destroy(&rt_data->local_iface_ids);
    struct local_datapath *cur_node, *next_node;
    HMAP_FOR_EACH_SAFE (cur_node, next_node, hmap_node,
                        &rt_data->local_datapaths) {
        free(cur_node->peer_ports);
        hmap_remove(&rt_data->local_datapaths, &cur_node->hmap_node);
        free(cur_node);
    }
    hmap_destroy(&rt_data->local_datapaths);
    local_bindings_destroy(&rt_data->local_bindings);
}

static void
init_binding_ctx(struct engine_node *node,
                 struct ed_type_runtime_data *rt_data,
                 struct binding_ctx_in *b_ctx_in,
                 struct binding_ctx_out *b_ctx_out)
{
    struct ovsrec_open_vswitch_table *ovs_table =
        (struct ovsrec_open_vswitch_table *)EN_OVSDB_GET(
            engine_get_input("OVS_open_vswitch", node));
    struct ovsrec_bridge_table *bridge_table =
        (struct ovsrec_bridge_table *)EN_OVSDB_GET(
            engine_get_input("OVS_bridge", node));
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);

    ovs_assert(br_int && chassis_id);

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");

    const struct sbrec_chassis *chassis
        = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    struct ovsrec_port_table *port_table =
        (struct ovsrec_port_table *)EN_OVSDB_GET(
            engine_get_input("OVS_port", node));

    struct ovsrec_interface_table *iface_table =
        (struct ovsrec_interface_table *)EN_OVSDB_GET(
            engine_get_input("OVS_interface", node));

    struct ovsrec_qos_table *qos_table =
        (struct ovsrec_qos_table *)EN_OVSDB_GET(
            engine_get_input("OVS_qos", node));

    struct sbrec_port_binding_table *pb_table =
        (struct sbrec_port_binding_table *)EN_OVSDB_GET(
            engine_get_input("SB_port_binding", node));

    struct ovsdb_idl_index *sbrec_datapath_binding_by_key =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_datapath_binding", node),
                "key");

    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    struct ovsdb_idl_index *sbrec_port_binding_by_datapath =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "datapath");

    b_ctx_in->ovnsb_idl_txn = engine_get_context()->ovnsb_idl_txn;
    b_ctx_in->ovs_idl_txn = engine_get_context()->ovs_idl_txn;
    b_ctx_in->sbrec_datapath_binding_by_key = sbrec_datapath_binding_by_key;
    b_ctx_in->sbrec_port_binding_by_datapath = sbrec_port_binding_by_datapath;
    b_ctx_in->sbrec_port_binding_by_name = sbrec_port_binding_by_name;
    b_ctx_in->port_table = port_table;
    b_ctx_in->iface_table = iface_table;
    b_ctx_in->qos_table = qos_table;
    b_ctx_in->port_binding_table = pb_table;
    b_ctx_in->br_int = br_int;
    b_ctx_in->chassis_rec = chassis;
    b_ctx_in->active_tunnels = &rt_data->active_tunnels;
    b_ctx_in->bridge_table = bridge_table;
    b_ctx_in->ovs_table = ovs_table;

    b_ctx_out->local_datapaths = &rt_data->local_datapaths;
    b_ctx_out->local_lports = &rt_data->local_lports;
    b_ctx_out->local_lport_ids = &rt_data->local_lport_ids;
    b_ctx_out->egress_ifaces = &rt_data->egress_ifaces;
    b_ctx_out->local_bindings = &rt_data->local_bindings;
    b_ctx_out->local_iface_ids = &rt_data->local_iface_ids;
}

static void
en_runtime_data_run(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data = data;
    struct hmap *local_datapaths = &rt_data->local_datapaths;
    struct sset *local_lports = &rt_data->local_lports;
    struct sset *local_lport_ids = &rt_data->local_lport_ids;
    struct sset *active_tunnels = &rt_data->active_tunnels;

    static bool first_run = true;
    if (first_run) {
        /* don't cleanup since there is no data yet */
        first_run = false;
    } else {
        struct local_datapath *cur_node, *next_node;
        HMAP_FOR_EACH_SAFE (cur_node, next_node, hmap_node, local_datapaths) {
            free(cur_node->peer_ports);
            hmap_remove(local_datapaths, &cur_node->hmap_node);
            free(cur_node);
        }
        hmap_clear(local_datapaths);
        local_bindings_destroy(&rt_data->local_bindings);
        sset_destroy(local_lports);
        sset_destroy(local_lport_ids);
        sset_destroy(active_tunnels);
        sset_destroy(&rt_data->egress_ifaces);
        smap_destroy(&rt_data->local_iface_ids);
        sset_init(local_lports);
        sset_init(local_lport_ids);
        sset_init(active_tunnels);
        sset_init(&rt_data->egress_ifaces);
        smap_init(&rt_data->local_iface_ids);
        local_bindings_init(&rt_data->local_bindings);
    }

    struct binding_ctx_in b_ctx_in;
    struct binding_ctx_out b_ctx_out;
    init_binding_ctx(node, rt_data, &b_ctx_in, &b_ctx_out);

    struct ed_type_ofctrl_is_connected *ed_ofctrl_is_connected =
        engine_get_input_data("ofctrl_is_connected", node);
    if (ed_ofctrl_is_connected->connected) {
        /* Calculate the active tunnels only if have an an active
         * OpenFlow connection to br-int.
         * If we don't have a connection to br-int, it could mean
         * ovs-vswitchd is down for some reason and the BFD status
         * in the Interface rows could be stale. So its better to
         * consider 'active_tunnels' set to be empty if it's not
         * connected. */
        bfd_calculate_active_tunnels(b_ctx_in.br_int, active_tunnels);
    }

    binding_run(&b_ctx_in, &b_ctx_out);

    engine_set_node_state(node, EN_UPDATED);
}

static bool
runtime_data_ovs_interface_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data = data;
    struct binding_ctx_in b_ctx_in;
    struct binding_ctx_out b_ctx_out;
    init_binding_ctx(node, rt_data, &b_ctx_in, &b_ctx_out);

    bool changed = false;
    if (!binding_handle_ovs_interface_changes(&b_ctx_in, &b_ctx_out,
                                              &changed)) {
        return false;
    }

    if (changed) {
        engine_set_node_state(node, EN_UPDATED);
    }

    return true;
}

static bool
runtime_data_noop_handler(struct engine_node *node OVS_UNUSED,
                          void *data OVS_UNUSED)
{
    return true;
}

static bool
runtime_data_sb_port_binding_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data = data;
    struct binding_ctx_in b_ctx_in;
    struct binding_ctx_out b_ctx_out;
    init_binding_ctx(node, rt_data, &b_ctx_in, &b_ctx_out);
    if (!b_ctx_in.chassis_rec) {
        return false;
    }

    bool changed = false;
    if (!binding_handle_port_binding_changes(&b_ctx_in, &b_ctx_out,
                                             &changed)) {
        return false;
    }

    if (changed) {
        engine_set_node_state(node, EN_UPDATED);
    }

    return true;
}

static bool
runtime_data_sb_datapath_binding_handler(struct engine_node *node OVS_UNUSED,
                                         void *data OVS_UNUSED)
{
    struct sbrec_datapath_binding_table *dp_table =
        (struct sbrec_datapath_binding_table *)EN_OVSDB_GET(
            engine_get_input("SB_datapath_binding", node));
    const struct sbrec_datapath_binding *dp;
    struct ed_type_runtime_data *rt_data = data;

    SBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (dp, dp_table) {
        if (sbrec_datapath_binding_is_deleted(dp)) {
            if (get_local_datapath(&rt_data->local_datapaths,
                                   dp->tunnel_key)) {
                return false;
            }
        }
    }

    return true;
}

/* Connection tracking zones. */
struct ed_type_ct_zones {
    unsigned long bitmap[BITMAP_N_LONGS(MAX_CT_ZONES)];
    struct shash pending;
    struct simap current;
};

static void *
en_ct_zones_init(struct engine_node *node, struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_ct_zones *data = xzalloc(sizeof *data);
    struct ovsrec_open_vswitch_table *ovs_table =
        (struct ovsrec_open_vswitch_table *)EN_OVSDB_GET(
            engine_get_input("OVS_open_vswitch", node));
    struct ovsrec_bridge_table *bridge_table =
        (struct ovsrec_bridge_table *)EN_OVSDB_GET(
            engine_get_input("OVS_bridge", node));

    shash_init(&data->pending);
    simap_init(&data->current);

    memset(data->bitmap, 0, sizeof data->bitmap);
    bitmap_set1(data->bitmap, 0); /* Zone 0 is reserved. */
    restore_ct_zones(bridge_table, ovs_table, &data->current, data->bitmap);
    return data;
}

static void
en_ct_zones_cleanup(void *data)
{
    struct ed_type_ct_zones *ct_zones_data = data;

    simap_destroy(&ct_zones_data->current);
    shash_destroy(&ct_zones_data->pending);
}

static void
en_ct_zones_run(struct engine_node *node, void *data)
{
    struct ed_type_ct_zones *ct_zones_data = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    update_ct_zones(&rt_data->local_lports, &rt_data->local_datapaths,
                    &ct_zones_data->current, ct_zones_data->bitmap,
                    &ct_zones_data->pending);

    engine_set_node_state(node, EN_UPDATED);
}

/* The data in the ct_zones node is always valid (i.e., no stale pointers). */
static bool
en_ct_zones_is_valid(struct engine_node *node OVS_UNUSED)
{
    return true;
}

struct ed_type_mff_ovn_geneve {
    enum mf_field_id mff_ovn_geneve;
};

static void *
en_mff_ovn_geneve_init(struct engine_node *node OVS_UNUSED,
                       struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_mff_ovn_geneve *data = xzalloc(sizeof *data);
    return data;
}

static void
en_mff_ovn_geneve_cleanup(void *data OVS_UNUSED)
{
}

static void
en_mff_ovn_geneve_run(struct engine_node *node, void *data)
{
    struct ed_type_mff_ovn_geneve *ed_mff_ovn_geneve = data;
    enum mf_field_id mff_ovn_geneve = ofctrl_get_mf_field_id();
    if (ed_mff_ovn_geneve->mff_ovn_geneve != mff_ovn_geneve) {
        ed_mff_ovn_geneve->mff_ovn_geneve = mff_ovn_geneve;
        engine_set_node_state(node, EN_UPDATED);
        return;
    }
    engine_set_node_state(node, EN_VALID);
}

struct ed_type_flow_output {
    /* desired flows */
    struct ovn_desired_flow_table flow_table;
    /* group ids for load balancing */
    struct ovn_extend_table group_table;
    /* meter ids for QoS */
    struct ovn_extend_table meter_table;
    /* conjunction id offset */
    uint32_t conj_id_ofs;
    /* lflow resource cross reference */
    struct lflow_resource_ref lflow_resource_ref;
};

static void init_physical_ctx(struct engine_node *node,
                              struct ed_type_runtime_data *rt_data,
                              struct physical_ctx *p_ctx)
{
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    struct sbrec_multicast_group_table *multicast_group_table =
        (struct sbrec_multicast_group_table *)EN_OVSDB_GET(
            engine_get_input("SB_multicast_group", node));

    struct sbrec_port_binding_table *port_binding_table =
        (struct sbrec_port_binding_table *)EN_OVSDB_GET(
            engine_get_input("SB_port_binding", node));

    struct sbrec_chassis_table *chassis_table =
        (struct sbrec_chassis_table *)EN_OVSDB_GET(
            engine_get_input("SB_chassis", node));

    struct ed_type_mff_ovn_geneve *ed_mff_ovn_geneve =
        engine_get_input_data("mff_ovn_geneve", node);

    struct ovsrec_open_vswitch_table *ovs_table =
        (struct ovsrec_open_vswitch_table *)EN_OVSDB_GET(
            engine_get_input("OVS_open_vswitch", node));
    struct ovsrec_bridge_table *bridge_table =
        (struct ovsrec_bridge_table *)EN_OVSDB_GET(
            engine_get_input("OVS_bridge", node));
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);
    const char *chassis_id = chassis_get_id();
    const struct sbrec_chassis *chassis = NULL;
    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    if (chassis_id) {
        chassis = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    }

    ovs_assert(br_int && chassis);

    struct ed_type_ct_zones *ct_zones_data =
        engine_get_input_data("ct_zones", node);
    struct simap *ct_zones = &ct_zones_data->current;

    p_ctx->sbrec_port_binding_by_name = sbrec_port_binding_by_name;
    p_ctx->port_binding_table = port_binding_table;
    p_ctx->mc_group_table = multicast_group_table;
    p_ctx->br_int = br_int;
    p_ctx->chassis_table = chassis_table;
    p_ctx->chassis = chassis;
    p_ctx->active_tunnels = &rt_data->active_tunnels;
    p_ctx->local_datapaths = &rt_data->local_datapaths;
    p_ctx->local_lports = &rt_data->local_lports;
    p_ctx->ct_zones = ct_zones;
    p_ctx->mff_ovn_geneve = ed_mff_ovn_geneve->mff_ovn_geneve;
}

static void init_lflow_ctx(struct engine_node *node,
                           struct ed_type_runtime_data *rt_data,
                           struct ed_type_flow_output *fo,
                           struct lflow_ctx_in *l_ctx_in,
                           struct lflow_ctx_out *l_ctx_out)
{
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    struct ovsdb_idl_index *sbrec_mc_group_by_name_dp =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_multicast_group", node),
                "name_datapath");

    struct sbrec_dhcp_options_table *dhcp_table =
        (struct sbrec_dhcp_options_table *)EN_OVSDB_GET(
            engine_get_input("SB_dhcp_options", node));

    struct sbrec_dhcpv6_options_table *dhcpv6_table =
        (struct sbrec_dhcpv6_options_table *)EN_OVSDB_GET(
            engine_get_input("SB_dhcpv6_options", node));

    struct sbrec_mac_binding_table *mac_binding_table =
        (struct sbrec_mac_binding_table *)EN_OVSDB_GET(
            engine_get_input("SB_mac_binding", node));

    struct sbrec_logical_flow_table *logical_flow_table =
        (struct sbrec_logical_flow_table *)EN_OVSDB_GET(
            engine_get_input("SB_logical_flow", node));

    struct sbrec_multicast_group_table *multicast_group_table =
        (struct sbrec_multicast_group_table *)EN_OVSDB_GET(
            engine_get_input("SB_multicast_group", node));

    const char *chassis_id = chassis_get_id();
    const struct sbrec_chassis *chassis = NULL;
    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    if (chassis_id) {
        chassis = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    }

    ovs_assert(chassis);

    struct ed_type_addr_sets *as_data =
        engine_get_input_data("addr_sets", node);
    struct shash *addr_sets = &as_data->addr_sets;

    struct ed_type_port_groups *pg_data =
        engine_get_input_data("port_groups", node);
    struct shash *port_groups = &pg_data->port_groups;

    l_ctx_in->sbrec_multicast_group_by_name_datapath =
        sbrec_mc_group_by_name_dp;
    l_ctx_in->sbrec_port_binding_by_name = sbrec_port_binding_by_name;
    l_ctx_in->dhcp_options_table  = dhcp_table;
    l_ctx_in->dhcpv6_options_table = dhcpv6_table;
    l_ctx_in->mac_binding_table = mac_binding_table;
    l_ctx_in->logical_flow_table = logical_flow_table;
    l_ctx_in->mc_group_table = multicast_group_table;
    l_ctx_in->chassis = chassis;
    l_ctx_in->local_datapaths = &rt_data->local_datapaths;
    l_ctx_in->addr_sets = addr_sets;
    l_ctx_in->port_groups = port_groups;
    l_ctx_in->active_tunnels = &rt_data->active_tunnels;
    l_ctx_in->local_lport_ids = &rt_data->local_lport_ids;

    l_ctx_out->flow_table = &fo->flow_table;
    l_ctx_out->group_table = &fo->group_table;
    l_ctx_out->meter_table = &fo->meter_table;
    l_ctx_out->lfrr = &fo->lflow_resource_ref;
    l_ctx_out->conj_id_ofs = &fo->conj_id_ofs;
}

static void *
en_flow_output_init(struct engine_node *node OVS_UNUSED,
                    struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_flow_output *data = xzalloc(sizeof *data);

    ovn_desired_flow_table_init(&data->flow_table);
    ovn_extend_table_init(&data->group_table);
    ovn_extend_table_init(&data->meter_table);
    data->conj_id_ofs = 1;
    lflow_resource_init(&data->lflow_resource_ref);
    return data;
}

static void
en_flow_output_cleanup(void *data)
{
    struct ed_type_flow_output *flow_output_data = data;
    ovn_desired_flow_table_destroy(&flow_output_data->flow_table);
    ovn_extend_table_destroy(&flow_output_data->group_table);
    ovn_extend_table_destroy(&flow_output_data->meter_table);
    lflow_resource_destroy(&flow_output_data->lflow_resource_ref);
}

static void
en_flow_output_run(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    struct ovsrec_open_vswitch_table *ovs_table =
        (struct ovsrec_open_vswitch_table *)EN_OVSDB_GET(
            engine_get_input("OVS_open_vswitch", node));
    struct ovsrec_bridge_table *bridge_table =
        (struct ovsrec_bridge_table *)EN_OVSDB_GET(
            engine_get_input("OVS_bridge", node));
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);
    const char *chassis_id = chassis_get_id();

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");

    const struct sbrec_chassis *chassis = NULL;
    if (chassis_id) {
        chassis = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    }

    ovs_assert(br_int && chassis);

    struct ed_type_flow_output *fo = data;
    struct ovn_desired_flow_table *flow_table = &fo->flow_table;
    struct ovn_extend_table *group_table = &fo->group_table;
    struct ovn_extend_table *meter_table = &fo->meter_table;
    uint32_t *conj_id_ofs = &fo->conj_id_ofs;
    struct lflow_resource_ref *lfrr = &fo->lflow_resource_ref;

    static bool first_run = true;
    if (first_run) {
        first_run = false;
    } else {
        ovn_desired_flow_table_clear(flow_table);
        ovn_extend_table_clear(group_table, false /* desired */);
        ovn_extend_table_clear(meter_table, false /* desired */);
        lflow_resource_clear(lfrr);
    }

    *conj_id_ofs = 1;
    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, rt_data, fo, &l_ctx_in, &l_ctx_out);
    lflow_run(&l_ctx_in, &l_ctx_out);

    struct physical_ctx p_ctx;
    init_physical_ctx(node, rt_data, &p_ctx);

    physical_run(&p_ctx, &fo->flow_table);

    engine_set_node_state(node, EN_UPDATED);
}

static bool
flow_output_sb_logical_flow_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ovsrec_open_vswitch_table *ovs_table =
        (struct ovsrec_open_vswitch_table *)EN_OVSDB_GET(
            engine_get_input("OVS_open_vswitch", node));
    struct ovsrec_bridge_table *bridge_table =
        (struct ovsrec_bridge_table *)EN_OVSDB_GET(
            engine_get_input("OVS_bridge", node));
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);
    ovs_assert(br_int);

    struct ed_type_flow_output *fo = data;
    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, rt_data, fo, &l_ctx_in, &l_ctx_out);

    bool handled = lflow_handle_changed_flows(&l_ctx_in, &l_ctx_out);

    engine_set_node_state(node, EN_UPDATED);
    return handled;
}

static bool
flow_output_sb_mac_binding_handler(struct engine_node *node, void *data)
{
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    struct sbrec_mac_binding_table *mac_binding_table =
        (struct sbrec_mac_binding_table *)EN_OVSDB_GET(
            engine_get_input("SB_mac_binding", node));

    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    const struct hmap *local_datapaths = &rt_data->local_datapaths;

    struct ed_type_flow_output *fo = data;
    struct ovn_desired_flow_table *flow_table = &fo->flow_table;

    lflow_handle_changed_neighbors(sbrec_port_binding_by_name,
            mac_binding_table, local_datapaths, flow_table);

    engine_set_node_state(node, EN_UPDATED);
    return true;
}

static bool
flow_output_sb_port_binding_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    struct ed_type_flow_output *fo = data;
    struct ovn_desired_flow_table *flow_table = &fo->flow_table;

    /* XXX: now we handle port-binding changes for physical flow processing
     * only, but port-binding change can have impact to logical flow
     * processing, too, in below circumstances:
     *
     *  - When a port-binding for a lport is inserted/deleted but the lflow
     *    using that lport doesn't change.
     *
     *    This can happen only when the lport name is used by ACL match
     *    condition, which is specified by user. Even in that case, if the port
     *    is actually bound on the current chassis it will trigger recompute on
     *    that chassis since ovs interface would be updated. So the only
     *    situation this would have real impact is when user defines an ACL
     *    that includes lport that is not on current chassis, and there is a
     *    port-binding creation/deletion related to that lport.e.g.: an ACL is
     *    defined:
     *
     *    to-lport 1000 'outport=="A" && inport=="B"' allow-related
     *
     *    If "A" is on current chassis, but "B" is lport that hasn't been
     *    created yet. When a lport "B" is created and bound on another
     *    chassis, the ACL will not take effect on the current chassis until a
     *    recompute is triggered later. This case doesn't seem to be a problem
     *    for real world use cases because usually lport is created before
     *    being referenced by name in ACLs.
     *
     *  - When is_chassis_resident(<lport>) is used in lflow. In this case the
     *    port binding is not a regular VIF. It can be either "patch" or
     *    "external", with ha-chassis-group assigned.  In current
     *    "runtime_data" handling, port-binding changes for these types always
     *    trigger recomputing. So it is fine even if we do not handle it here.
     *    (due to the ovsdb tracking support for referenced table changes,
     *    ha-chassis-group changes will appear as port-binding change).
     *
     *  - When a mac-binding doesn't change but the port-binding related to
     *    that mac-binding is deleted. In this case the neighbor flow generated
     *    for the mac-binding should be deleted. This would not cause any real
     *    issue for now, since the port-binding related to mac-binding is
     *    always logical router port, and any change to logical router port
     *    would just trigger recompute.
     *
     * Although there is no correctness issue so far (except the unusual ACL
     * use case, which doesn't seem to be a real problem), it might be better
     * to handle this more gracefully, without the need to consider these
     * tricky scenarios.  One approach is to maintain a mapping between lport
     * names and the lflows that uses them, and reprocess the related lflows
     * when related port-bindings change.
     */
    struct physical_ctx p_ctx;
    init_physical_ctx(node, rt_data, &p_ctx);

    physical_handle_port_binding_changes(&p_ctx, flow_table);

    engine_set_node_state(node, EN_UPDATED);
    return true;
}

static bool
flow_output_sb_multicast_group_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    struct ed_type_flow_output *fo = data;
    struct ovn_desired_flow_table *flow_table = &fo->flow_table;

    struct physical_ctx p_ctx;
    init_physical_ctx(node, rt_data, &p_ctx);

    physical_handle_mc_group_changes(&p_ctx, flow_table);

    engine_set_node_state(node, EN_UPDATED);
    return true;

}

static bool
_flow_output_resource_ref_handler(struct engine_node *node, void *data,
                                  enum ref_type ref_type)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    struct ed_type_addr_sets *as_data =
        engine_get_input_data("addr_sets", node);

    struct ed_type_port_groups *pg_data =
        engine_get_input_data("port_groups", node);

    struct ovsrec_open_vswitch_table *ovs_table =
        (struct ovsrec_open_vswitch_table *)EN_OVSDB_GET(
            engine_get_input("OVS_open_vswitch", node));
    struct ovsrec_bridge_table *bridge_table =
        (struct ovsrec_bridge_table *)EN_OVSDB_GET(
            engine_get_input("OVS_bridge", node));
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);
    const char *chassis_id = chassis_get_id();

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    const struct sbrec_chassis *chassis = NULL;
    if (chassis_id) {
        chassis = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    }

    ovs_assert(br_int && chassis);

    struct ed_type_flow_output *fo = data;

    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, rt_data, fo, &l_ctx_in, &l_ctx_out);

    bool changed;
    const char *ref_name;
    struct sset *new, *updated, *deleted;

    switch (ref_type) {
        case REF_TYPE_ADDRSET:
            /* XXX: The change_tracked check may be added to inc-proc
             * framework. */
            if (!as_data->change_tracked) {
                return false;
            }
            new = &as_data->new;
            updated = &as_data->updated;
            deleted = &as_data->deleted;
            break;
        case REF_TYPE_PORTGROUP:
            if (!pg_data->change_tracked) {
                return false;
            }
            new = &pg_data->new;
            updated = &pg_data->updated;
            deleted = &pg_data->deleted;
            break;
        default:
            OVS_NOT_REACHED();
    }


    SSET_FOR_EACH (ref_name, deleted) {
        if (!lflow_handle_changed_ref(ref_type, ref_name, &l_ctx_in,
                                      &l_ctx_out, &changed)) {
            return false;
        }
        if (changed) {
            engine_set_node_state(node, EN_UPDATED);
        }
    }
    SSET_FOR_EACH (ref_name, updated) {
        if (!lflow_handle_changed_ref(ref_type, ref_name, &l_ctx_in,
                                      &l_ctx_out, &changed)) {
            return false;
        }
        if (changed) {
            engine_set_node_state(node, EN_UPDATED);
        }
    }
    SSET_FOR_EACH (ref_name, new) {
        if (!lflow_handle_changed_ref(ref_type, ref_name, &l_ctx_in,
                                      &l_ctx_out, &changed)) {
            return false;
        }
        if (changed) {
            engine_set_node_state(node, EN_UPDATED);
        }
    }

    return true;
}

static bool
flow_output_addr_sets_handler(struct engine_node *node, void *data)
{
    return _flow_output_resource_ref_handler(node, data, REF_TYPE_ADDRSET);
}

static bool
flow_output_port_groups_handler(struct engine_node *node, void *data)
{
    return _flow_output_resource_ref_handler(node, data, REF_TYPE_PORTGROUP);
}

struct ovn_controller_exit_args {
    bool *exiting;
    bool *restart;
};

int
main(int argc, char *argv[])
{
    struct unixctl_server *unixctl;
    bool exiting;
    bool restart;
    struct ovn_controller_exit_args exit_args = {&exiting, &restart};
    int retval;

    ovs_cmdl_proctitle_init(argc, argv);
    ovn_set_program_name(argv[0]);
    service_start(&argc, &argv);
    char *ovs_remote = parse_options(argc, argv);
    fatal_ignore_sigpipe();

    daemonize_start(true);

    char *abs_unixctl_path = get_abs_unix_ctl_path(NULL);
    retval = unixctl_server_create(abs_unixctl_path, &unixctl);
    free(abs_unixctl_path);
    if (retval) {
        exit(EXIT_FAILURE);
    }
    unixctl_command_register("exit", "", 0, 1, ovn_controller_exit,
                             &exit_args);

    daemonize_complete();

    patch_init();
    pinctrl_init();
    lflow_init();

    /* Connect to OVS OVSDB instance. */
    struct ovsdb_idl_loop ovs_idl_loop = OVSDB_IDL_LOOP_INITIALIZER(
        ovsdb_idl_create(ovs_remote, &ovsrec_idl_class, false, true));
    ctrl_register_ovs_idl(ovs_idl_loop.idl);
    ovsdb_idl_get_initial_snapshot(ovs_idl_loop.idl);

    /* Configure OVN SB database. */
    struct ovsdb_idl_loop ovnsb_idl_loop = OVSDB_IDL_LOOP_INITIALIZER(
        ovsdb_idl_create_unconnected(&sbrec_idl_class, true));
    ovsdb_idl_set_leader_only(ovnsb_idl_loop.idl, false);

    unixctl_command_register("connection-status", "", 0, 0,
                             ovn_controller_conn_show, ovnsb_idl_loop.idl);

    struct ovsdb_idl_index *sbrec_chassis_by_name
        = chassis_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_multicast_group_by_name_datapath
        = mcast_group_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_port_binding_by_name
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_port_binding_col_logical_port);
    struct ovsdb_idl_index *sbrec_port_binding_by_key
        = ovsdb_idl_index_create2(ovnsb_idl_loop.idl,
                                  &sbrec_port_binding_col_tunnel_key,
                                  &sbrec_port_binding_col_datapath);
    struct ovsdb_idl_index *sbrec_port_binding_by_datapath
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_port_binding_col_datapath);
    struct ovsdb_idl_index *sbrec_datapath_binding_by_key
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_datapath_binding_col_tunnel_key);
    struct ovsdb_idl_index *sbrec_mac_binding_by_lport_ip
        = ovsdb_idl_index_create2(ovnsb_idl_loop.idl,
                                  &sbrec_mac_binding_col_logical_port,
                                  &sbrec_mac_binding_col_ip);
    struct ovsdb_idl_index *sbrec_ip_multicast
        = ip_mcast_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_igmp_group
        = igmp_group_index_create(ovnsb_idl_loop.idl);

    ovsdb_idl_track_add_all(ovnsb_idl_loop.idl);
    ovsdb_idl_omit_alert(ovnsb_idl_loop.idl, &sbrec_chassis_col_nb_cfg);

    /* Omit the external_ids column of all the tables except for -
     *  - DNS. pinctrl.c uses the external_ids column of DNS,
     *    which it shouldn't. This should be removed.
     *
     *  - Chassis - chassis.c copies the chassis configuration from
     *              local open_vswitch table to the external_ids of
     *              chassis.
     *
     *  - Datapath_binding - lflow.c is using this to check if the datapath
     *                       is switch or not. This should be removed.
     * */

    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_sb_global_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_logical_flow_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_port_binding_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_ssl_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl,
                   &sbrec_gateway_chassis_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_ha_chassis_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl,
                   &sbrec_ha_chassis_group_col_external_ids);

    /* We don't want to monitor Connection table at all. So omit all the
     * columns. */
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_inactivity_probe);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_is_connected);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_max_backoff);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_other_config);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_read_only);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_role);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_status);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_target);

    update_sb_monitors(ovnsb_idl_loop.idl, NULL, NULL, NULL, false);

    stopwatch_create(CONTROLLER_LOOP_STOPWATCH_NAME, SW_MS);

    /* Define inc-proc-engine nodes. */
    ENGINE_NODE_CUSTOM_DATA(ct_zones, "ct_zones");
    ENGINE_NODE(runtime_data, "runtime_data");
    ENGINE_NODE(mff_ovn_geneve, "mff_ovn_geneve");
    ENGINE_NODE(ofctrl_is_connected, "ofctrl_is_connected");
    ENGINE_NODE(flow_output, "flow_output");
    ENGINE_NODE(addr_sets, "addr_sets");
    ENGINE_NODE(port_groups, "port_groups");

#define SB_NODE(NAME, NAME_STR) ENGINE_NODE_SB(NAME, NAME_STR);
    SB_NODES
#undef SB_NODE

#define OVS_NODE(NAME, NAME_STR) ENGINE_NODE_OVS(NAME, NAME_STR);
    OVS_NODES
#undef OVS_NODE

    /* Add dependencies between inc-proc-engine nodes. */

    engine_add_input(&en_addr_sets, &en_sb_address_set,
                     addr_sets_sb_address_set_handler);
    engine_add_input(&en_port_groups, &en_sb_port_group,
                     port_groups_sb_port_group_handler);

    engine_add_input(&en_flow_output, &en_addr_sets,
                     flow_output_addr_sets_handler);
    engine_add_input(&en_flow_output, &en_port_groups,
                     flow_output_port_groups_handler);
    engine_add_input(&en_flow_output, &en_runtime_data, NULL);
    engine_add_input(&en_flow_output, &en_ct_zones, NULL);
    engine_add_input(&en_flow_output, &en_mff_ovn_geneve, NULL);

    engine_add_input(&en_flow_output, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_flow_output, &en_ovs_bridge, NULL);

    engine_add_input(&en_flow_output, &en_sb_chassis, NULL);
    engine_add_input(&en_flow_output, &en_sb_encap, NULL);
    engine_add_input(&en_flow_output, &en_sb_multicast_group,
                     flow_output_sb_multicast_group_handler);
    engine_add_input(&en_flow_output, &en_sb_port_binding,
                     flow_output_sb_port_binding_handler);
    engine_add_input(&en_flow_output, &en_sb_mac_binding,
                     flow_output_sb_mac_binding_handler);
    engine_add_input(&en_flow_output, &en_sb_logical_flow,
                     flow_output_sb_logical_flow_handler);
    engine_add_input(&en_flow_output, &en_sb_dhcp_options, NULL);
    engine_add_input(&en_flow_output, &en_sb_dhcpv6_options, NULL);
    engine_add_input(&en_flow_output, &en_sb_dns, NULL);

    engine_add_input(&en_ct_zones, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_ct_zones, &en_ovs_bridge, NULL);
    engine_add_input(&en_ct_zones, &en_runtime_data, NULL);

    engine_add_input(&en_runtime_data, &en_ofctrl_is_connected, NULL);

    engine_add_input(&en_runtime_data, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_runtime_data, &en_ovs_bridge, NULL);
    engine_add_input(&en_runtime_data, &en_ovs_port,
                     runtime_data_noop_handler);
    engine_add_input(&en_runtime_data, &en_ovs_interface,
                     runtime_data_ovs_interface_handler);
    engine_add_input(&en_runtime_data, &en_ovs_qos, NULL);

    engine_add_input(&en_runtime_data, &en_sb_chassis, NULL);
    engine_add_input(&en_runtime_data, &en_sb_datapath_binding,
                     runtime_data_sb_datapath_binding_handler);
    engine_add_input(&en_runtime_data, &en_sb_port_binding,
                     runtime_data_sb_port_binding_handler);

    struct engine_arg engine_arg = {
        .sb_idl = ovnsb_idl_loop.idl,
        .ovs_idl = ovs_idl_loop.idl,
    };
    engine_init(&en_flow_output, &engine_arg);

    engine_ovsdb_node_add_index(&en_sb_chassis, "name", sbrec_chassis_by_name);
    engine_ovsdb_node_add_index(&en_sb_multicast_group, "name_datapath",
                                sbrec_multicast_group_by_name_datapath);
    engine_ovsdb_node_add_index(&en_sb_port_binding, "name",
                                sbrec_port_binding_by_name);
    engine_ovsdb_node_add_index(&en_sb_port_binding, "key",
                                sbrec_port_binding_by_key);
    engine_ovsdb_node_add_index(&en_sb_port_binding, "datapath",
                                sbrec_port_binding_by_datapath);
    engine_ovsdb_node_add_index(&en_sb_datapath_binding, "key",
                                sbrec_datapath_binding_by_key);

    struct ed_type_flow_output *flow_output_data =
        engine_get_internal_data(&en_flow_output);
    struct ed_type_ct_zones *ct_zones_data =
        engine_get_internal_data(&en_ct_zones);
    struct ed_type_runtime_data *runtime_data = NULL;

    ofctrl_init(&flow_output_data->group_table,
                &flow_output_data->meter_table,
                get_ofctrl_probe_interval(ovs_idl_loop.idl));

    unixctl_command_register("group-table-list", "", 0, 0,
                             extend_table_list,
                             &flow_output_data->group_table);

    unixctl_command_register("meter-table-list", "", 0, 0,
                             extend_table_list,
                             &flow_output_data->meter_table);

    unixctl_command_register("ct-zone-list", "", 0, 0,
                             ct_zone_list,
                             &ct_zones_data->current);

    struct pending_pkt pending_pkt = { .conn = NULL };
    unixctl_command_register("inject-pkt", "MICROFLOW", 1, 1, inject_pkt,
                             &pending_pkt);

    unixctl_command_register("recompute", "", 0, 0, engine_recompute_cmd,
                             NULL);

    unsigned int ovs_cond_seqno = UINT_MAX;
    unsigned int ovnsb_cond_seqno = UINT_MAX;

    /* Main loop. */
    exiting = false;
    restart = false;
    bool sb_monitor_all = false;
    while (!exiting) {
        engine_init_run();

        struct ovsdb_idl_txn *ovs_idl_txn = ovsdb_idl_loop_run(&ovs_idl_loop);
        unsigned int new_ovs_cond_seqno
            = ovsdb_idl_get_condition_seqno(ovs_idl_loop.idl);
        if (new_ovs_cond_seqno != ovs_cond_seqno) {
            if (!new_ovs_cond_seqno) {
                VLOG_INFO("OVS IDL reconnected, force recompute.");
                engine_set_force_recompute(true);
            }
            ovs_cond_seqno = new_ovs_cond_seqno;
        }

        update_sb_db(ovs_idl_loop.idl, ovnsb_idl_loop.idl, &sb_monitor_all);
        update_ssl_config(ovsrec_ssl_table_get(ovs_idl_loop.idl));
        ofctrl_set_probe_interval(get_ofctrl_probe_interval(ovs_idl_loop.idl));

        struct ovsdb_idl_txn *ovnsb_idl_txn
            = ovsdb_idl_loop_run(&ovnsb_idl_loop);
        unsigned int new_ovnsb_cond_seqno
            = ovsdb_idl_get_condition_seqno(ovnsb_idl_loop.idl);
        if (new_ovnsb_cond_seqno != ovnsb_cond_seqno) {
            if (!new_ovnsb_cond_seqno) {
                VLOG_INFO("OVNSB IDL reconnected, force recompute.");
                engine_set_force_recompute(true);
            }
            ovnsb_cond_seqno = new_ovnsb_cond_seqno;
        }

        struct engine_context eng_ctx = {
            .ovs_idl_txn = ovs_idl_txn,
            .ovnsb_idl_txn = ovnsb_idl_txn
        };

        engine_set_context(&eng_ctx);

        if (ovsdb_idl_has_ever_connected(ovnsb_idl_loop.idl)) {
            /* Contains the transport zones that this Chassis belongs to */
            struct sset transport_zones = SSET_INITIALIZER(&transport_zones);
            sset_from_delimited_string(&transport_zones,
                get_transport_zones(ovsrec_open_vswitch_table_get(
                                    ovs_idl_loop.idl)), ",");

            const struct ovsrec_bridge_table *bridge_table =
                ovsrec_bridge_table_get(ovs_idl_loop.idl);
            const struct ovsrec_open_vswitch_table *ovs_table =
                ovsrec_open_vswitch_table_get(ovs_idl_loop.idl);
            const struct sbrec_chassis_table *chassis_table =
                sbrec_chassis_table_get(ovnsb_idl_loop.idl);
            const struct ovsrec_bridge *br_int =
                process_br_int(ovs_idl_txn, bridge_table, ovs_table);
            const char *chassis_id = get_ovs_chassis_id(ovs_table);
            const struct sbrec_chassis *chassis = NULL;
            if (chassis_id) {
                chassis = chassis_run(ovnsb_idl_txn, sbrec_chassis_by_name,
                                      ovs_table, chassis_table, chassis_id,
                                      br_int, &transport_zones);
            }

            if (br_int) {
                ct_zones_data = engine_get_data(&en_ct_zones);
                if (ct_zones_data) {
                    ofctrl_run(br_int, &ct_zones_data->pending);
                }

                if (chassis) {
                    encaps_run(ovs_idl_txn,
                               bridge_table, br_int,
                               sbrec_chassis_table_get(ovnsb_idl_loop.idl),
                               chassis,
                               sbrec_sb_global_first(ovnsb_idl_loop.idl),
                               &transport_zones);

                    stopwatch_start(CONTROLLER_LOOP_STOPWATCH_NAME,
                                    time_msec());
                    if (ovnsb_idl_txn) {
                        if (!ofctrl_can_put()) {
                            /* When there are in-flight messages pending to
                             * ovs-vswitchd, we should hold on recomputing so
                             * that the previous flow installations won't be
                             * delayed.  However, we still want to try if
                             * recompute is not needed and we can quickly
                             * incrementally process the new changes, to avoid
                             * unnecessarily forced recomputes later on.  This
                             * is because the OVSDB change tracker cannot
                             * preserve tracked changes across iterations.  If
                             * change tracking is improved, we can simply skip
                             * this round of engine_run and continue processing
                             * acculated changes incrementally later when
                             * ofctrl_can_put() returns true. */
                            engine_run(false);
                        } else {
                            engine_run(true);
                        }
                    } else {
                        /* Even if there's no SB DB transaction available,
                         * try to run the engine so that we can handle any
                         * incremental changes that don't require a recompute.
                         * If a recompute is required, the engine will abort,
                         * triggerring a full run in the next iteration.
                         */
                        engine_run(false);
                    }
                    stopwatch_stop(CONTROLLER_LOOP_STOPWATCH_NAME,
                                   time_msec());
                    ct_zones_data = engine_get_data(&en_ct_zones);
                    if (ovs_idl_txn) {
                        if (ct_zones_data) {
                            commit_ct_zones(br_int, &ct_zones_data->pending);
                        }
                        bfd_run(ovsrec_interface_table_get(ovs_idl_loop.idl),
                                br_int, chassis,
                                sbrec_ha_chassis_group_table_get(
                                    ovnsb_idl_loop.idl),
                                sbrec_sb_global_table_get(ovnsb_idl_loop.idl));
                    }

                    flow_output_data = engine_get_data(&en_flow_output);
                    if (flow_output_data && ct_zones_data) {
                        ofctrl_put(&flow_output_data->flow_table,
                                   &ct_zones_data->pending,
                                   sbrec_meter_table_get(ovnsb_idl_loop.idl),
                                   get_nb_cfg(sbrec_sb_global_table_get(
                                                   ovnsb_idl_loop.idl)),
                                   engine_node_changed(&en_flow_output));
                    }
                    runtime_data = engine_get_data(&en_runtime_data);
                    if (runtime_data) {
                        patch_run(ovs_idl_txn,
                            ovsrec_bridge_table_get(ovs_idl_loop.idl),
                            ovsrec_open_vswitch_table_get(ovs_idl_loop.idl),
                            ovsrec_port_table_get(ovs_idl_loop.idl),
                            sbrec_port_binding_table_get(ovnsb_idl_loop.idl),
                            br_int, chassis, &runtime_data->local_datapaths);
                        pinctrl_run(ovnsb_idl_txn,
                                    sbrec_datapath_binding_by_key,
                                    sbrec_port_binding_by_datapath,
                                    sbrec_port_binding_by_key,
                                    sbrec_port_binding_by_name,
                                    sbrec_mac_binding_by_lport_ip,
                                    sbrec_igmp_group,
                                    sbrec_ip_multicast,
                                    sbrec_dns_table_get(ovnsb_idl_loop.idl),
                                    sbrec_controller_event_table_get(
                                        ovnsb_idl_loop.idl),
                                    sbrec_service_monitor_table_get(
                                        ovnsb_idl_loop.idl),
                                    br_int, chassis,
                                    &runtime_data->local_datapaths,
                                    &runtime_data->active_tunnels);
                        if (engine_node_changed(&en_runtime_data)) {
                            update_sb_monitors(ovnsb_idl_loop.idl, chassis,
                                               &runtime_data->local_lports,
                                               &runtime_data->local_datapaths,
                                               sb_monitor_all);
                        }
                    }
                }

            }

            if (!engine_has_run()) {
                if (engine_need_run()) {
                    VLOG_DBG("engine did not run, force recompute next time: "
                             "br_int %p, chassis %p", br_int, chassis);
                    engine_set_force_recompute(true);
                    poll_immediate_wake();
                } else {
                    VLOG_DBG("engine did not run, and it was not needed"
                             " either: br_int %p, chassis %p",
                             br_int, chassis);
                }
            } else if (engine_aborted()) {
                VLOG_DBG("engine was aborted, force recompute next time: "
                         "br_int %p, chassis %p", br_int, chassis);
                engine_set_force_recompute(true);
                poll_immediate_wake();
            } else {
                engine_set_force_recompute(false);
            }

            if (ovnsb_idl_txn && chassis) {
                int64_t cur_cfg = ofctrl_get_cur_cfg();
                if (cur_cfg && cur_cfg != chassis->nb_cfg) {
                    sbrec_chassis_set_nb_cfg(chassis, cur_cfg);
                }
            }


            if (pending_pkt.conn) {
                struct ed_type_addr_sets *as_data =
                    engine_get_data(&en_addr_sets);
                struct ed_type_port_groups *pg_data =
                    engine_get_data(&en_port_groups);
                if (br_int && chassis && as_data && pg_data) {
                    char *error = ofctrl_inject_pkt(br_int, pending_pkt.flow_s,
                        &as_data->addr_sets, &pg_data->port_groups);
                    if (error) {
                        unixctl_command_reply_error(pending_pkt.conn, error);
                        free(error);
                    } else {
                        unixctl_command_reply(pending_pkt.conn, NULL);
                    }
                } else {
                    VLOG_DBG("Pending_pkt conn but br_int %p or chassis "
                             "%p not ready.", br_int, chassis);
                    unixctl_command_reply_error(pending_pkt.conn,
                        "ovn-controller not ready.");
                }
                pending_pkt.conn = NULL;
                free(pending_pkt.flow_s);
            }

            sset_destroy(&transport_zones);

            if (br_int) {
                ofctrl_wait();
                pinctrl_wait(ovnsb_idl_txn);
            }
        }

        unixctl_server_run(unixctl);

        unixctl_server_wait(unixctl);
        if (exiting || pending_pkt.conn) {
            poll_immediate_wake();
        }

        if (!ovsdb_idl_loop_commit_and_wait(&ovnsb_idl_loop)) {
            VLOG_INFO("OVNSB commit failed, force recompute next time.");
            engine_set_force_recompute(true);
        }

        if (ovsdb_idl_loop_commit_and_wait(&ovs_idl_loop) == 1) {
            ct_zones_data = engine_get_data(&en_ct_zones);
            if (ct_zones_data) {
                struct shash_node *iter, *iter_next;
                SHASH_FOR_EACH_SAFE (iter, iter_next,
                                     &ct_zones_data->pending) {
                    struct ct_zone_pending_entry *ctzpe = iter->data;
                    if (ctzpe->state == CT_ZONE_DB_SENT) {
                        shash_delete(&ct_zones_data->pending, iter);
                        free(ctzpe);
                    }
                }
            }
        }

        ovsdb_idl_track_clear(ovnsb_idl_loop.idl);
        ovsdb_idl_track_clear(ovs_idl_loop.idl);
        poll_block();
        if (should_service_stop()) {
            exiting = true;
        }
    }

    engine_set_context(NULL);
    engine_cleanup();

    /* It's time to exit.  Clean up the databases if we are not restarting */
    if (!restart) {
        bool done = !ovsdb_idl_has_ever_connected(ovnsb_idl_loop.idl);
        while (!done) {
            update_sb_db(ovs_idl_loop.idl, ovnsb_idl_loop.idl, NULL);
            update_ssl_config(ovsrec_ssl_table_get(ovs_idl_loop.idl));

            struct ovsdb_idl_txn *ovs_idl_txn
                = ovsdb_idl_loop_run(&ovs_idl_loop);
            struct ovsdb_idl_txn *ovnsb_idl_txn
                = ovsdb_idl_loop_run(&ovnsb_idl_loop);

            const struct ovsrec_bridge_table *bridge_table
                = ovsrec_bridge_table_get(ovs_idl_loop.idl);
            const struct ovsrec_open_vswitch_table *ovs_table
                = ovsrec_open_vswitch_table_get(ovs_idl_loop.idl);

            const struct sbrec_port_binding_table *port_binding_table
                = sbrec_port_binding_table_get(ovnsb_idl_loop.idl);

            const struct ovsrec_bridge *br_int = get_br_int(bridge_table,
                                                            ovs_table);
            const char *chassis_id = chassis_get_id();
            const struct sbrec_chassis *chassis
                = (chassis_id
                   ? chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id)
                   : NULL);

            /* Run all of the cleanup functions, even if one of them returns
             * false. We're done if all of them return true. */
            done = binding_cleanup(ovnsb_idl_txn, port_binding_table, chassis);
            done = chassis_cleanup(ovnsb_idl_txn, chassis) && done;
            done = encaps_cleanup(ovs_idl_txn, br_int) && done;
            done = igmp_group_cleanup(ovnsb_idl_txn, sbrec_igmp_group) && done;
            if (done) {
                poll_immediate_wake();
            }

            ovsdb_idl_loop_commit_and_wait(&ovnsb_idl_loop);
            ovsdb_idl_loop_commit_and_wait(&ovs_idl_loop);
            poll_block();
        }
    }

    unixctl_server_destroy(unixctl);
    lflow_destroy();
    ofctrl_destroy();
    pinctrl_destroy();
    patch_destroy();

    ovsdb_idl_loop_destroy(&ovs_idl_loop);
    ovsdb_idl_loop_destroy(&ovnsb_idl_loop);

    free(ovs_remote);
    service_stop();

    exit(retval);
}

static char *
parse_options(int argc, char *argv[])
{
    enum {
        OPT_PEER_CA_CERT = UCHAR_MAX + 1,
        OPT_BOOTSTRAP_CA_CERT,
        VLOG_OPTION_ENUMS,
        OVN_DAEMON_OPTION_ENUMS,
        SSL_OPTION_ENUMS,
    };

    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        VLOG_LONG_OPTIONS,
        OVN_DAEMON_LONG_OPTIONS,
        STREAM_SSL_LONG_OPTIONS,
        {"peer-ca-cert", required_argument, NULL, OPT_PEER_CA_CERT},
        {"bootstrap-ca-cert", required_argument, NULL, OPT_BOOTSTRAP_CA_CERT},
        {NULL, 0, NULL, 0}
    };
    char *short_options = ovs_cmdl_long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();

        case 'V':
            ovs_print_version(OFP15_VERSION, OFP15_VERSION);
            exit(EXIT_SUCCESS);

        VLOG_OPTION_HANDLERS
        OVN_DAEMON_OPTION_HANDLERS
        STREAM_SSL_OPTION_HANDLERS

        case OPT_PEER_CA_CERT:
            stream_ssl_set_peer_ca_cert_file(optarg);
            break;

        case OPT_BOOTSTRAP_CA_CERT:
            stream_ssl_set_ca_cert_file(optarg, true);
            break;

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);

    argc -= optind;
    argv += optind;

    char *ovs_remote;
    if (argc == 0) {
        ovs_remote = xasprintf("unix:%s/db.sock", ovs_rundir());
    } else if (argc == 1) {
        ovs_remote = xstrdup(argv[0]);
    } else {
        VLOG_FATAL("exactly zero or one non-option argument required; "
                   "use --help for usage");
    }
    return ovs_remote;
}

static void
usage(void)
{
    printf("%s: OVN controller\n"
           "usage %s [OPTIONS] [OVS-DATABASE]\n"
           "where OVS-DATABASE is a socket on which the OVS OVSDB server is listening.\n",
               program_name, program_name);
    stream_usage("OVS-DATABASE", true, false, true);
    daemon_usage();
    vlog_usage();
    printf("\nOther options:\n"
           "  -h, --help              display this help message\n"
           "  -V, --version           display version information\n");
    exit(EXIT_SUCCESS);
}

static void
ovn_controller_exit(struct unixctl_conn *conn, int argc,
             const char *argv[], void *exit_args_)
{
    struct ovn_controller_exit_args *exit_args = exit_args_;
    *exit_args->exiting = true;
    *exit_args->restart = argc == 2 && !strcmp(argv[1], "--restart");
    unixctl_command_reply(conn, NULL);
}

static void
ct_zone_list(struct unixctl_conn *conn, int argc OVS_UNUSED,
             const char *argv[] OVS_UNUSED, void *ct_zones_)
{
    struct simap *ct_zones = ct_zones_;
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct simap_node *zone;

    SIMAP_FOR_EACH(zone, ct_zones) {
        ds_put_format(&ds, "%s %d\n", zone->name, zone->data);
    }

    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
}

static void
extend_table_list(struct unixctl_conn *conn, int argc OVS_UNUSED,
                 const char *argv[] OVS_UNUSED, void *extend_table_)
{
    struct ovn_extend_table *extend_table = extend_table_;
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct simap items = SIMAP_INITIALIZER(&items);

    struct ovn_extend_table_info *item;
    HMAP_FOR_EACH (item, hmap_node, &extend_table->existing) {
        simap_put(&items, item->name, item->table_id);
    }

    const struct simap_node **nodes = simap_sort(&items);
    size_t n_nodes = simap_count(&items);
    for (size_t i = 0; i < n_nodes; i++) {
        const struct simap_node *node = nodes[i];
        ds_put_format(&ds, "%s: %d\n", node->name, node->data);
    }

    free(nodes);
    simap_destroy(&items);

    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
}

static void
inject_pkt(struct unixctl_conn *conn, int argc OVS_UNUSED,
           const char *argv[], void *pending_pkt_)
{
    struct pending_pkt *pending_pkt = pending_pkt_;

    if (pending_pkt->conn) {
        unixctl_command_reply_error(conn, "already pending packet injection");
        return;
    }
    pending_pkt->conn = conn;
    pending_pkt->flow_s = xstrdup(argv[1]);
}

static void
ovn_controller_conn_show(struct unixctl_conn *conn, int argc OVS_UNUSED,
                         const char *argv[] OVS_UNUSED, void *idl_)
{
    const char *result = "not connected";
    const struct ovsdb_idl *idl = idl_;

    if (ovsdb_idl_is_connected(idl)) {
       result = "connected";
    }
    unixctl_command_reply(conn, result);
}

static void
engine_recompute_cmd(struct unixctl_conn *conn OVS_UNUSED, int argc OVS_UNUSED,
                     const char *argv[] OVS_UNUSED, void *arg OVS_UNUSED)
{
    VLOG_INFO("User triggered force recompute.");
    engine_set_force_recompute(true);
    poll_immediate_wake();
    unixctl_command_reply(conn, NULL);
}
