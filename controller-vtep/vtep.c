/* Copyright (c) 2015 Nicira, Inc.
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

#include "vtep.h"

#include "lib/hash.h"
#include "lib/hmapx.h"
#include "openvswitch/hmap.h"
#include "openvswitch/shash.h"
#include "lib/ovn-util.h"
#include "lib/smap.h"
#include "lib/sset.h"
#include "lib/util.h"
#include "ovn-controller-vtep.h"
#include "openvswitch/vlog.h"
#include "lib/ovn-sb-idl.h"
#include "vtep/vtep-idl.h"

VLOG_DEFINE_THIS_MODULE(vtep);

struct vtep_rec_physical_locator_list_entry {
    struct ovs_list locators_node;
    const struct vteprec_physical_locator *vteprec_ploc;
};

struct mmr_hash_node_data {
    const struct vteprec_mcast_macs_remote *mmr;
    struct shash physical_locators;
};

struct lp_mac_ip_binding {
    const char *lp;
    struct shash mac_ip_lp;
};

/*
 * Scans through the Binding table in ovnsb, and updates the vtep logical
 * switch tunnel keys and the 'Ucast_Macs_Remote' table in the VTEP
 * database.
 *
 */

/* Searches the 'chassis_rec->encaps' for the first vtep tunnel
 * configuration, returns the 'ip'.  Unless duplicated, the returned
 * pointer cannot live past current vtep_run() execution. */
static const char *
get_chassis_vtep_ip(const struct sbrec_chassis *chassis_rec)
{
    if (chassis_rec) {
        size_t i;

        for (i = 0; i < chassis_rec->n_encaps; i++) {
            if (!strcmp(chassis_rec->encaps[i]->type, "vxlan")) {
                return chassis_rec->encaps[i]->ip;
            }
        }
    }

    return NULL;
}

/* Creates a new 'Ucast_Macs_Remote'. */
static struct vteprec_ucast_macs_remote *
create_umr(struct ovsdb_idl_txn *vtep_idl_txn, const char *mac,
           const struct vteprec_logical_switch *vtep_ls)
{
    struct vteprec_ucast_macs_remote *new_umr =
        vteprec_ucast_macs_remote_insert(vtep_idl_txn);

    vteprec_ucast_macs_remote_set_MAC(new_umr, mac);
    vteprec_ucast_macs_remote_set_logical_switch(new_umr, vtep_ls);

    return new_umr;
}

/* Creates a new 'Physical_Locator'. */
static struct vteprec_physical_locator *
create_pl(struct ovsdb_idl_txn *vtep_idl_txn, const char *chassis_ip)
{
    struct vteprec_physical_locator *new_pl =
        vteprec_physical_locator_insert(vtep_idl_txn);

    vteprec_physical_locator_set_dst_ip(new_pl, chassis_ip);
    vteprec_physical_locator_set_encapsulation_type(new_pl,
                                                    VTEP_ENCAP_TYPE);

    return new_pl;
}

/* Creates a new 'Mcast_Macs_Remote'. */
static void
vtep_create_mmr(struct ovsdb_idl_txn *vtep_idl_txn, const char *mac,
                const struct vteprec_logical_switch *vtep_ls,
                const struct vteprec_physical_locator_set *ploc_set)
{
    struct vteprec_mcast_macs_remote *new_mmr =
       vteprec_mcast_macs_remote_insert(vtep_idl_txn);

    VLOG_DBG("Inserting MMR for LS '%s'", vtep_ls->name);
    vteprec_mcast_macs_remote_set_MAC(new_mmr, mac);
    vteprec_mcast_macs_remote_set_logical_switch(new_mmr, vtep_ls);
    vteprec_mcast_macs_remote_set_locator_set(new_mmr, ploc_set);
}

/* Compares previous and new mmr locator sets and returns true if they
 * differ and false otherwise. This function also preps a new locator
 * set for database write.
 *
 * 'locators_list' is the new set of locators for the associated
 * 'Mcast_Macs_Remote' entry passed in and is queried to generate the
 * new set of locators in vtep database format. */
static bool
vtep_process_pls(const struct ovs_list *locators_list,
                 const struct mmr_hash_node_data *mmr_ext,
                 struct vteprec_physical_locator **locators)
{
    size_t n_locators_prev = 0;
    size_t n_locators_new = ovs_list_size(locators_list);
    bool locator_lists_differ = false;

    if (mmr_ext) {
        n_locators_prev = mmr_ext->mmr->locator_set->n_locators;
    }
    if (n_locators_prev != n_locators_new) {
        locator_lists_differ = true;
    }

    if (n_locators_new) {
        int i = 0;
        struct vtep_rec_physical_locator_list_entry *ploc_entry;
        LIST_FOR_EACH (ploc_entry, locators_node, locators_list) {
            locators[i] = (struct vteprec_physical_locator *)
                           ploc_entry->vteprec_ploc;
            if (mmr_ext && !shash_find_data(&mmr_ext->physical_locators,
                                            locators[i]->dst_ip)) {
                locator_lists_differ = true;
            }
            i++;
        }
    }

    return locator_lists_differ;
}

/* Creates a new 'Mcast_Macs_Remote' entry or modifies existing if needed
 * and also cleans up out-dated remote mcast mac entries as needed. */
static void
vtep_update_mmr(struct ovsdb_idl_txn *vtep_idl_txn,
                struct ovs_list *locators_list,
                const struct vteprec_logical_switch *vtep_ls,
                const struct mmr_hash_node_data *mmr_ext)
{
    struct vteprec_physical_locator **locators = NULL;
    size_t n_locators_new = ovs_list_size(locators_list);
    bool mmr_changed;

    locators = xmalloc(n_locators_new * sizeof *locators);
    mmr_changed = vtep_process_pls(locators_list, mmr_ext, locators);

    if (mmr_changed) {
        if (n_locators_new) {
            const struct vteprec_physical_locator_set *ploc_set =
                vteprec_physical_locator_set_insert(vtep_idl_txn);

            vteprec_physical_locator_set_set_locators(ploc_set, locators,
                                                      n_locators_new);

            if (!mmr_ext) {  /* create new mmr */
                vtep_create_mmr(vtep_idl_txn, "unknown-dst", vtep_ls,
                                ploc_set);
            } else {  /* update existing mmr */
                vteprec_mcast_macs_remote_set_locator_set(mmr_ext->mmr,
                                                          ploc_set);
            }

        } else if (mmr_ext) {  /* remove old mmr */
            vteprec_mcast_macs_remote_delete(mmr_ext->mmr);
        }
    }

    free(locators);
}

/* Updates the vtep Logical_Switch table entries' tunnel keys based
 * on the port bindings. */
static void
vtep_lswitch_run(struct shash *vtep_pbs, struct sset *vtep_pswitches,
                 struct shash *vtep_lswitches)
{
    struct sset used_ls = SSET_INITIALIZER(&used_ls);
    struct shash_node *node;

    /* Collects the logical switch bindings from port binding entries.
     * Since the binding module has already guaranteed that each vtep
     * logical switch is bound only to one ovn-sb logical datapath,
     * we can just iterate and assign tunnel key to vtep logical switch. */
    SHASH_FOR_EACH (node, vtep_pbs) {
        const struct sbrec_port_binding *port_binding_rec = node->data;
        const char *pswitch_name = smap_get(&port_binding_rec->options,
                                            "vtep-physical-switch");
        const char *lswitch_name = smap_get(&port_binding_rec->options,
                                            "vtep-logical-switch");
        const struct vteprec_logical_switch *vtep_ls;

        /* If 'port_binding_rec->chassis' exists then 'pswitch_name'
         * and 'lswitch_name' must also exist. */
        if (!pswitch_name || !lswitch_name) {
            /* This could only happen when someone directly modifies the
             * database,  (e.g. using ovn-sbctl). */
            VLOG_ERR("logical port (%s) with no 'options:vtep-physical-"
                     "switch' or 'options:vtep-logical-switch' specified "
                     "is bound to chassis (%s).",
                     port_binding_rec->logical_port,
                     port_binding_rec->chassis->name);
            continue;
        }
        vtep_ls = shash_find_data(vtep_lswitches, lswitch_name);
        /* Also checks 'pswitch_name' since the same 'lswitch_name' could
         * exist in multiple vtep database instances and be bound to different
         * ovn logical networks. */
        if (vtep_ls && sset_find(vtep_pswitches, pswitch_name)) {
            int64_t tnl_key;

            if (sset_find(&used_ls, lswitch_name)) {
                continue;
            }

            tnl_key = port_binding_rec->datapath->tunnel_key;
            if ((vtep_ls->n_tunnel_key && vtep_ls->tunnel_key[0] != tnl_key)
                || !vtep_ls->n_tunnel_key) {
                VLOG_DBG("set vtep logical switch (%s) tunnel key to %"PRId64,
                         vtep_ls->name, tnl_key);
                vteprec_logical_switch_set_tunnel_key(vtep_ls, &tnl_key, 1);
            }

            /* OVN is expected to always use source node replication mode,
             * hence the replication mode is hard-coded for each logical
             * switch in the context of ovn-controller-vtep. */
            if (!vtep_ls->replication_mode
                || strcmp(vtep_ls->replication_mode, "source_node")) {

                vteprec_logical_switch_set_replication_mode(vtep_ls,
                                                            "source_node");
            }

            sset_add(&used_ls, lswitch_name);
        }
    }
    /* Resets the tunnel keys for unused vtep logical switches. */
    SHASH_FOR_EACH (node, vtep_lswitches) {
        if (!sset_find(&used_ls, node->name)) {
            int64_t tnl_key = 0;
            vteprec_logical_switch_set_tunnel_key(node->data, &tnl_key, 1);
        }
    }
    sset_destroy(&used_ls);
}

/* Updates the vtep 'Ucast_Macs_Remote' and 'Mcast_Macs_Remote' tables based
 * on non-vtep port bindings. */
static void
vtep_macs_run(struct ovsdb_idl_txn *vtep_idl_txn,
              struct shash *ucast_macs_rmts,
              struct shash *mcast_macs_rmts,
              struct shash *physical_locators,
              struct shash *vtep_lswitches,
              struct shash *non_vtep_pbs,
              struct shash *vtep_pbs,
              struct shash *sbrec_lp_mac_binding,
              struct sset *vtep_pswitches)
{
    struct shash_node *node;
    struct hmap ls_map;

    /* Maps from ovn logical datapath tunnel key (which is also the vtep
     * logical switch tunnel key) to the corresponding vtep logical switch
     * instance.  Also, the shash map 'added_macs' is used for checking
     * duplicated MAC addresses in the same ovn logical datapath. 'mmr_ext'
     * is used to track mmr info per LS that needs creation/update and
     * 'locators_list' collects the new physical locators to be bound for
     * an mmr_ext; 'physical_locators' is used to track existing locators and
     * filter duplicates per logical switch. */
    struct ls_hash_node {
        struct hmap_node hmap_node;

        const struct vteprec_logical_switch *vtep_ls;
        struct shash added_macs;

        struct ovs_list locators_list;
        struct shash physical_locators;
        struct mmr_hash_node_data *mmr_ext;
    };

    hmap_init(&ls_map);
    SHASH_FOR_EACH (node, vtep_lswitches) {
        const struct vteprec_logical_switch *vtep_ls = node->data;
        struct ls_hash_node *ls_node;

        if (!vtep_ls->n_tunnel_key) {
            continue;
        }
        ls_node = xmalloc(sizeof *ls_node);
        ls_node->vtep_ls = vtep_ls;
        shash_init(&ls_node->added_macs);
        shash_init(&ls_node->physical_locators);
        ovs_list_init(&ls_node->locators_list);
        ls_node->mmr_ext = NULL;
        hmap_insert(&ls_map, &ls_node->hmap_node,
                    hash_uint64((uint64_t) vtep_ls->tunnel_key[0]));
    }

    const char *dp, *peer;
    const struct sbrec_port_binding *lrp_pb, *peer_pb;

    SHASH_FOR_EACH (node, non_vtep_pbs) {
        const struct sbrec_port_binding *port_binding_rec = node->data;
        const struct sbrec_chassis *chassis_rec;
        struct ls_hash_node *ls_node;
        const char *chassis_ip;
        int64_t tnl_key;
        size_t i;

        chassis_rec = port_binding_rec->chassis;
        if (!chassis_rec) {
            continue;
        }

        if (!strcmp(port_binding_rec->type, "chassisredirect")) {
            dp = smap_get(&port_binding_rec->options, "distributed-port");
            lrp_pb = shash_find_data(non_vtep_pbs, dp);
            if (!lrp_pb) {
                continue;
            }

            peer = smap_get(&lrp_pb->options, "peer");
            if (!peer) {
                continue;
            }

            peer_pb = shash_find_data(non_vtep_pbs, peer);
            if (!peer_pb) {
                continue;
            }
            tnl_key = peer_pb->datapath->tunnel_key;
        } else if (!strcmp(port_binding_rec->type, "external")) {
            /* External ports sits actually behind remote VTEPs
             * but port itself is bound to one of gateway nodes
             * to provide DHCP/Metadata. Skip port_binding
             * information for such ports, as its does not specify
             * real node location. Use dynamically learned Mac_Binding
             * records from remote VTEPs */
            continue;
        } else {
            tnl_key = port_binding_rec->datapath->tunnel_key;
        }

        HMAP_FOR_EACH_WITH_HASH (ls_node, hmap_node,
                                 hash_uint64((uint64_t) tnl_key),
                                 &ls_map) {
            if (ls_node->vtep_ls->tunnel_key[0] == tnl_key) {
                break;
            }
        }
        /* If 'ls_node' is NULL, that means no vtep logical switch is
         * attached to the corresponding ovn logical datapath, so pass.
         */
        if (!ls_node) {
            continue;
        }

        chassis_ip = get_chassis_vtep_ip(chassis_rec);
        /* Unreachable chassis, continue. */
        if (!chassis_ip) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
            VLOG_INFO_RL(&rl, "VTEP tunnel encap on chassis (%s) not found",
                         chassis_rec->name);
            continue;
        }

        const struct vteprec_physical_locator *pl =
            shash_find_data(physical_locators, chassis_ip);
        if (!pl) {
            pl = create_pl(vtep_idl_txn, chassis_ip);
            shash_add(physical_locators, chassis_ip, pl);
        }

        const struct vteprec_physical_locator *ls_pl =
            shash_find_data(&ls_node->physical_locators, chassis_ip);
        if (!ls_pl) {
            struct vtep_rec_physical_locator_list_entry *ploc_entry =
                xmalloc(sizeof *ploc_entry);
            ploc_entry->vteprec_ploc = pl;
            ovs_list_push_back(&ls_node->locators_list,
                               &ploc_entry->locators_node);
            shash_add(&ls_node->physical_locators, chassis_ip, pl);
        }

        if (!ls_node->mmr_ext) {
            char *mac_tnlkey = xasprintf("%s_%"PRId64, "unknown-dst", tnl_key);
            ls_node->mmr_ext = shash_find_data(mcast_macs_rmts, mac_tnlkey);

            if (ls_node->mmr_ext &&
                ls_node->mmr_ext->mmr->logical_switch == ls_node->vtep_ls) {

                /* Delete the entry from the hash table so the mmr does not get
                * removed from the DB later on during stale checking. */
                shash_find_and_delete(mcast_macs_rmts, mac_tnlkey);
            }
            free(mac_tnlkey);
        }

        for (i = 0; i < port_binding_rec->n_mac; i++) {
            const struct vteprec_ucast_macs_remote *umr;
            const struct sbrec_port_binding *conflict;
            struct lport_addresses laddrs;

            if (!extract_lsp_addresses(port_binding_rec->mac[i], &laddrs)) {
                continue;
            };

            char *mac = laddrs.ea_s;

            /* Checks for duplicate MAC in the same vtep logical switch. */
            conflict = shash_find_data(&ls_node->added_macs, mac);
            if (conflict) {
                VLOG_WARN("MAC address (%s) has already been known to be "
                          "on logical port (%s) in the same logical "
                          "datapath, so just ignore this logical port (%s)",
                          mac, conflict->logical_port,
                          port_binding_rec->logical_port);
                continue;
            }
            shash_add(&ls_node->added_macs, mac, port_binding_rec);

            char *mac_ip_tnlkey = xasprintf("%s_%s_%"PRId64, mac, chassis_ip,
                                            tnl_key);
            umr = shash_find_data(ucast_macs_rmts, mac_ip_tnlkey);
            /* If finds the 'umr' entry for the mac, ip, and tnl_key, deletes
             * the entry from shash so that it is not garbage collected.
             *
             * If not found, creates a new 'umr' entry. */
            if (umr && umr->logical_switch == ls_node->vtep_ls) {
                shash_find_and_delete(ucast_macs_rmts, mac_ip_tnlkey);
            } else {
                const struct vteprec_ucast_macs_remote *new_umr;
                new_umr = create_umr(vtep_idl_txn, mac, ls_node->vtep_ls);
                vteprec_ucast_macs_remote_set_locator(new_umr, pl);
            }
            free(mac_ip_tnlkey);
            destroy_lport_addresses(&laddrs);
        }
    }

    /* Handle dynamically leart MACs from remote VTEPs registered in
     * Mac_Binding table. */
    SHASH_FOR_EACH (node, vtep_pbs) {
        const struct sbrec_port_binding *port_binding_rec = node->data;
        const struct sbrec_chassis *chassis_rec;
        struct ls_hash_node *ls_node;
        const char *chassis_ip;
        int64_t tnl_key;

        chassis_rec = port_binding_rec->chassis;
        if (!chassis_rec) {
            continue;
        }

        const char *pswitch_name = smap_get(&port_binding_rec->options,
                                            "vtep-physical-switch");
        /* Ignore macs learned by ourselfs */
        if (sset_find(vtep_pswitches, pswitch_name)) {
            continue;
        }
        tnl_key = port_binding_rec->datapath->tunnel_key;

        HMAP_FOR_EACH_WITH_HASH (ls_node, hmap_node,
                                 hash_uint64((uint64_t) tnl_key),
                                 &ls_map) {
            if (ls_node->vtep_ls->tunnel_key[0] == tnl_key) {
                break;
            }
        }
        /* If 'ls_node' is NULL, that means no vtep logical switch is
         * attached to the corresponding ovn logical datapath, so pass.
         */
        if (!ls_node) {
            continue;
        }

        chassis_ip = get_chassis_vtep_ip(chassis_rec);
        /* Unreachable chassis, continue. */
        if (!chassis_ip) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
            VLOG_INFO_RL(&rl, "VTEP tunnel encap on chassis (%s) not found",
                         chassis_rec->name);
            continue;
        }

        const struct vteprec_physical_locator *pl =
            shash_find_data(physical_locators, chassis_ip);
        if (!pl) {
            pl = create_pl(vtep_idl_txn, chassis_ip);
            shash_add(physical_locators, chassis_ip, pl);
        }

        const struct vteprec_physical_locator *ls_pl =
            shash_find_data(&ls_node->physical_locators, chassis_ip);
        if (!ls_pl) {
            struct vtep_rec_physical_locator_list_entry *ploc_entry =
                xmalloc(sizeof *ploc_entry);
            ploc_entry->vteprec_ploc = pl;
            ovs_list_push_back(&ls_node->locators_list,
                               &ploc_entry->locators_node);
            shash_add(&ls_node->physical_locators, chassis_ip, pl);
        }


        struct lp_mac_ip_binding *miplpb = shash_find_data(
            sbrec_lp_mac_binding, port_binding_rec->logical_port);
        struct shash_node *miplb_node;
        if (!miplpb) {
            continue;
        }
        SHASH_FOR_EACH (miplb_node, &miplpb->mac_ip_lp) {
            const struct sbrec_mac_binding * mb = miplb_node->data;
            /* Ignore MACs from other networks (datapathese) */
            if (port_binding_rec->datapath->tunnel_key !=
                    mb->datapath->tunnel_key) {
                continue;
            }
            const struct vteprec_ucast_macs_remote *umr;
            const struct sbrec_port_binding *conflict;

            char *mac = mb->mac;

            /* Checks for duplicate MAC in the same vtep logical switch. */
            conflict = shash_find_data(&ls_node->added_macs, mac);
            if (conflict) {
                VLOG_WARN("MAC address (%s) has already been known to be "
                          "on logical port (%s) in the same logical "
                          "datapath, so just ignore this logical port (%s)",
                          mac, conflict->logical_port,
                          port_binding_rec->logical_port);
                continue;
            }
            shash_add(&ls_node->added_macs, mac, port_binding_rec);

            char *mac_ip_tnlkey = xasprintf("%s_%s_%"PRId64, mac, chassis_ip,
                                            tnl_key);
            umr = shash_find_data(ucast_macs_rmts, mac_ip_tnlkey);
            /* If finds the 'umr' entry for the mac, ip, and tnl_key, deletes
             * the entry from shash so that it is not garbage collected.
             *
             * If not found, creates a new 'umr' entry. */
            if (umr && umr->logical_switch == ls_node->vtep_ls) {
                shash_find_and_delete(ucast_macs_rmts, mac_ip_tnlkey);
            } else {
                const struct vteprec_ucast_macs_remote *new_umr;
                new_umr = create_umr(vtep_idl_txn, mac, ls_node->vtep_ls);
                vteprec_ucast_macs_remote_set_locator(new_umr, pl);
            }
            free(mac_ip_tnlkey);
        }
    }

    /* Removes all remaining 'umr's, since they do not exist anymore. */
    SHASH_FOR_EACH (node, ucast_macs_rmts) {
        vteprec_ucast_macs_remote_delete(node->data);
    }
    struct ls_hash_node *iter;
    HMAP_FOR_EACH_SAFE (iter, hmap_node, &ls_map) {
        struct vtep_rec_physical_locator_list_entry *ploc_entry;
        vtep_update_mmr(vtep_idl_txn, &iter->locators_list,
                        iter->vtep_ls, iter->mmr_ext);
        LIST_FOR_EACH_POP (ploc_entry, locators_node,
                          &iter->locators_list) {
            free(ploc_entry);
        }
        hmap_remove(&ls_map, &iter->hmap_node);
        shash_destroy(&iter->added_macs);
        shash_destroy(&iter->physical_locators);
        free(iter);
    }
    hmap_destroy(&ls_map);

    /* Clean stale 'Mcast_Macs_Remote' */
    struct mmr_hash_node_data *mmr_ext;
    SHASH_FOR_EACH (node, mcast_macs_rmts) {
        mmr_ext = node->data;
        vteprec_mcast_macs_remote_delete(mmr_ext->mmr);
    }
}

/* Resets all logical switches' 'tunnel_key' to NULL */
static bool
vtep_lswitch_cleanup(struct ovsdb_idl *vtep_idl)
{
    const struct vteprec_logical_switch *vtep_ls;
    bool done = true;

    VTEPREC_LOGICAL_SWITCH_FOR_EACH (vtep_ls, vtep_idl) {
        if (vtep_ls->n_tunnel_key) {
            vteprec_logical_switch_set_tunnel_key(vtep_ls, NULL, 0);
            done = false;
        }
    }

    return done;
}

/* Removes all entries in the 'Ucast_Macs_Remote' table in the vtep database.
 * Returns true when all done (i.e. no entry to remove). */
static bool
vtep_ucast_macs_cleanup(struct ovsdb_idl *vtep_idl)
{
    const struct vteprec_ucast_macs_remote *umr;

    VTEPREC_UCAST_MACS_REMOTE_FOR_EACH (umr, vtep_idl) {
        vteprec_ucast_macs_remote_delete(umr);
        return false;
    }

    return true;
}

/* Removes all entries in the 'Mcast_Macs_Remote' table in vtep database.
 * Returns true when all done (i.e. no entry to remove). */
static bool
vtep_mcast_macs_cleanup(struct ovsdb_idl *vtep_idl)
{
    const struct vteprec_mcast_macs_remote *mmr;

    VTEPREC_MCAST_MACS_REMOTE_FOR_EACH (mmr, vtep_idl) {
        vteprec_mcast_macs_remote_delete(mmr);
        return false;
    }

    return true;
}

static const struct sbrec_port_binding *
find_pbs_for_logical_switch(struct shash *vtep_pbs,
                            struct sset *vtep_pswitches,
                            char *ls_name){

    struct shash_node *node;
    SHASH_FOR_EACH (node, vtep_pbs) {
        const struct sbrec_port_binding *port_binding_rec = node->data;
        const char *pswitch_name = smap_get(&port_binding_rec->options,
                                            "vtep-physical-switch");
        const char *lswitch_name = smap_get(&port_binding_rec->options,
                                            "vtep-logical-switch");
        if (!port_binding_rec->chassis) {
            continue;
        }

        /* If 'port_binding_rec->chassis' exists then 'pswitch_name'
         * and 'lswitch_name' must also exist. */
        if (!pswitch_name || !lswitch_name) {
            /* This could only happen when someone directly modifies the
             * database,  (e.g. using ovn-sbctl). */
            VLOG_ERR("logical port (%s) with no 'options:vtep-physical-"
                     "switch' or 'options:vtep-logical-switch' specified "
                     "is bound to chassis (%s).",
                     port_binding_rec->logical_port,
                     port_binding_rec->chassis->name);
            continue;
        }
        /* Make sure both logical_switch and physical_switch matches */
        if (!strcmp(ls_name, lswitch_name)) {
          if (sset_find(vtep_pswitches, port_binding_rec->chassis->name)) {
            return port_binding_rec;
          }
        }
    }
    return NULL;
}

/* Propagate dynamically learned MACs on local VTEPs to OVN SB. Where
 * later this information is used to create tunnels between neighbour
 * VTEPs.
*/
static void
vtep_local_macs(struct controller_vtep_ctx *ctx,
                struct shash *vtep_pbs,
                struct sset *vtep_pswitches,
                struct shash *vtep_lswitches,
                struct shash *sbrec_lp_mac_binding){

    if (!ctx->ovnsb_idl_txn) {
        return;
    }

    ovsdb_idl_txn_add_comment(ctx->ovnsb_idl_txn,
                              "ovn-controller-vtep: updating mac_binding");

    const struct vteprec_ucast_macs_local *vtep_uml;
    const struct sbrec_port_binding *port_binding_rec;
    const struct sbrec_mac_binding *mb;

    /* Collect local unicast MACs */
    VTEPREC_UCAST_MACS_LOCAL_FOR_EACH (vtep_uml, ctx->vtep_idl) {
        port_binding_rec = find_pbs_for_logical_switch(
            vtep_pbs, vtep_pswitches, vtep_uml->logical_switch->name);
        if (!port_binding_rec) {
            VLOG_ERR("Cannot find port_binding for dynamically learned MAC %s "
                     "in logical_switch %s", vtep_uml->MAC,
                     vtep_uml->logical_switch->name);
            continue;
        }

        struct lp_mac_ip_binding *miplpb = shash_find_data(
            sbrec_lp_mac_binding, port_binding_rec->logical_port);
        mb = NULL;
        if (miplpb) {
            char *mac_ip_lp_key = xasprintf("%s_%s_%s", vtep_uml->MAC,
                                            vtep_uml->ipaddr,
                                            port_binding_rec->logical_port);
            mb = shash_find_and_delete(&miplpb->mac_ip_lp, mac_ip_lp_key);
            free(mac_ip_lp_key);
        }

        if (!mb) {
            VLOG_DBG("Creating new mac_binding entry for mac %s",
                     vtep_uml->MAC);
            mb = sbrec_mac_binding_insert(ctx->ovnsb_idl_txn);
            sbrec_mac_binding_set_mac(mb, vtep_uml->MAC);
            sbrec_mac_binding_set_logical_port(
                mb, port_binding_rec->logical_port);
            sbrec_mac_binding_set_timestamp(mb, time_wall_msec());
        }
        sbrec_mac_binding_set_ip(mb, vtep_uml->ipaddr);
        sbrec_mac_binding_set_datapath(mb, port_binding_rec->datapath);
    }

    struct shash_node *node;
    struct shash_node *miplb_node;
    SHASH_FOR_EACH (node, vtep_lswitches) {
        port_binding_rec = find_pbs_for_logical_switch(vtep_pbs,
                                                       vtep_pswitches,
                                                       node->name);
        if (!port_binding_rec) {
            continue;
        }
        struct lp_mac_ip_binding *miplpb = shash_find_data(
            sbrec_lp_mac_binding, port_binding_rec->logical_port);
        if (!miplpb) {
            continue;
        }
        if (!&miplpb->mac_ip_lp) {
            continue;
        }
        SHASH_FOR_EACH (miplb_node, &miplpb->mac_ip_lp) {
            mb = miplb_node->data;
            VLOG_DBG("Removing mac_binding for stale VTEP mac %s", mb->mac);
            sbrec_mac_binding_delete(mb);
        }
    }
}

/* Updates vtep logical switch tunnel keys. */
void
vtep_run(struct controller_vtep_ctx *ctx)
{
    if (!ctx->vtep_idl_txn) {
        return;
    }

    struct sset vtep_pswitches = SSET_INITIALIZER(&vtep_pswitches);
    struct shash vtep_lswitches = SHASH_INITIALIZER(&vtep_lswitches);
    struct shash ucast_macs_rmts = SHASH_INITIALIZER(&ucast_macs_rmts);
    struct shash mcast_macs_rmts = SHASH_INITIALIZER(&mcast_macs_rmts);
    struct shash physical_locators = SHASH_INITIALIZER(&physical_locators);
    struct shash vtep_pbs = SHASH_INITIALIZER(&vtep_pbs);
    struct shash non_vtep_pbs = SHASH_INITIALIZER(&non_vtep_pbs);
    struct hmapx mcast_macs_ptrs = HMAPX_INITIALIZER(&mcast_macs_ptrs);
    const struct vteprec_physical_switch *vtep_ps;
    const struct vteprec_logical_switch *vtep_ls;
    const struct vteprec_ucast_macs_remote *umr;
    const struct sbrec_port_binding *port_binding_rec;
    const struct vteprec_mcast_macs_remote *mmr;

    /* Collects 'Physical_Switch's. */
    VTEPREC_PHYSICAL_SWITCH_FOR_EACH (vtep_ps, ctx->vtep_idl) {
        sset_add(&vtep_pswitches, vtep_ps->name);
    }

    /* Collects 'Logical_Switch's. */
    VTEPREC_LOGICAL_SWITCH_FOR_EACH (vtep_ls, ctx->vtep_idl) {
        shash_add(&vtep_lswitches, vtep_ls->name, vtep_ls);
    }

    /* Collects 'Ucast_Macs_Remote's. */
    VTEPREC_UCAST_MACS_REMOTE_FOR_EACH (umr, ctx->vtep_idl) {
        char *mac_ip_tnlkey =
            xasprintf("%s_%s_%"PRId64, umr->MAC,
                      umr->locator ? umr->locator->dst_ip : "",
                      umr->logical_switch && umr->logical_switch->n_tunnel_key
                          ? umr->logical_switch->tunnel_key[0] : INT64_MAX);

        shash_add(&ucast_macs_rmts, mac_ip_tnlkey, umr);
        free(mac_ip_tnlkey);
    }

    /* Collects 'Mcast_Macs_Remote's. */
    VTEPREC_MCAST_MACS_REMOTE_FOR_EACH (mmr, ctx->vtep_idl) {
        struct mmr_hash_node_data *mmr_ext = xmalloc(sizeof *mmr_ext);
        hmapx_add(&mcast_macs_ptrs, mmr_ext);
        char *mac_tnlkey =
            xasprintf("%s_%"PRId64, mmr->MAC,
                      mmr->logical_switch && mmr->logical_switch->n_tunnel_key
                          ? mmr->logical_switch->tunnel_key[0] : INT64_MAX);

        shash_add_once(&mcast_macs_rmts, mac_tnlkey, mmr_ext);
        mmr_ext->mmr = mmr;

        shash_init(&mmr_ext->physical_locators);
        for (size_t i = 0; i < mmr->locator_set->n_locators; i++) {
            shash_add(&mmr_ext->physical_locators,
                      mmr->locator_set->locators[i]->dst_ip,
                      mmr->locator_set->locators[i]);
        }

        free(mac_tnlkey);
    }

    /* Collects 'Physical_Locator's. */
    const struct vteprec_physical_locator *pl;
    VTEPREC_PHYSICAL_LOCATOR_FOR_EACH (pl, ctx->vtep_idl) {
        shash_add(&physical_locators, pl->dst_ip, pl);
    }

    /* Collects and classifies 'Port_Binding's. */
    SBREC_PORT_BINDING_FOR_EACH (port_binding_rec, ctx->ovnsb_idl) {
        struct shash *target =
            !strcmp(port_binding_rec->type, "vtep") ? &vtep_pbs
                                                    : &non_vtep_pbs;

        if (!port_binding_rec->chassis &&
            strcmp(port_binding_rec->type, "patch")) {
            continue;
        }
        shash_add(target, port_binding_rec->logical_port, port_binding_rec);
    }

    /* Construct logical_port to mac_binding */
    const struct sbrec_mac_binding *mb;
    struct shash sbrec_lp_mac_binding = SHASH_INITIALIZER(
        &sbrec_lp_mac_binding);
    SBREC_MAC_BINDING_FOR_EACH (mb, ctx->ovnsb_idl) {
        if (!mb->logical_port) {
            continue;
        }
        char *mac_ip_lp_key = xasprintf("%s_%s_%s", mb->mac, mb->ip,
                                        mb->logical_port);

        struct lp_mac_ip_binding *miplpb = shash_find_data(
            &sbrec_lp_mac_binding, mb->logical_port);
        if (!miplpb) {
            struct lp_mac_ip_binding *sbrec_mac_ip_lp_binding = xmalloc(
                sizeof *sbrec_mac_ip_lp_binding);
            shash_init(&sbrec_mac_ip_lp_binding->mac_ip_lp);
            sbrec_mac_ip_lp_binding->lp = mb->logical_port;
            shash_add(&sbrec_mac_ip_lp_binding->mac_ip_lp, mac_ip_lp_key, mb);
            shash_adda(&sbrec_lp_mac_binding, mb->logical_port,
                       sbrec_mac_ip_lp_binding);
        } else {
            shash_add(&miplpb->mac_ip_lp, mac_ip_lp_key, mb);
        }
        free(mac_ip_lp_key);
    }

    ovsdb_idl_txn_add_comment(ctx->vtep_idl_txn,
                              "ovn-controller-vtep: update logical switch "
                              "tunnel keys and 'ucast_macs_remote's");

    vtep_lswitch_run(&vtep_pbs, &vtep_pswitches, &vtep_lswitches);
    vtep_macs_run(ctx->vtep_idl_txn, &ucast_macs_rmts,
                  &mcast_macs_rmts, &physical_locators,
                  &vtep_lswitches, &non_vtep_pbs, &vtep_pbs,
                  &sbrec_lp_mac_binding, &vtep_pswitches);
    vtep_local_macs(ctx, &vtep_pbs, &vtep_pswitches, &vtep_lswitches,
                    &sbrec_lp_mac_binding);

    sset_destroy(&vtep_pswitches);
    shash_destroy(&vtep_lswitches);
    shash_destroy(&ucast_macs_rmts);
    struct hmapx_node *node;
    HMAPX_FOR_EACH (node, &mcast_macs_ptrs) {
        struct mmr_hash_node_data *mmr_ext = node->data;
        shash_destroy(&mmr_ext->physical_locators);
        free(mmr_ext);
    }
    hmapx_destroy(&mcast_macs_ptrs);
    shash_destroy(&mcast_macs_rmts);
    shash_destroy(&physical_locators);
    shash_destroy(&vtep_pbs);
    shash_destroy(&non_vtep_pbs);

    struct shash_node *lp_mb_node;
    SHASH_FOR_EACH (lp_mb_node, &sbrec_lp_mac_binding) {
        struct lp_mac_ip_binding *miplpb = lp_mb_node->data;
        shash_destroy(&miplpb->mac_ip_lp);
        free(miplpb);
    }
    shash_destroy(&sbrec_lp_mac_binding);
}

/* Cleans up all related entries in vtep.  Returns true when done (i.e. there
 * is no change made to 'ctx->vtep_idl'), otherwise returns false. */
bool
vtep_cleanup(struct controller_vtep_ctx *ctx)
{
    if (!ctx->vtep_idl_txn) {
        return false;
    }

    bool all_done;

    ovsdb_idl_txn_add_comment(ctx->vtep_idl_txn,
                              "ovn-controller-vtep: cleaning up vtep "
                              "configuration");
    all_done = vtep_lswitch_cleanup(ctx->vtep_idl);
    all_done = vtep_ucast_macs_cleanup(ctx->vtep_idl) && all_done;
    all_done = vtep_mcast_macs_cleanup(ctx->vtep_idl) && all_done;

    return all_done;
}
