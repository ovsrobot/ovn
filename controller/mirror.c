/* Copyright (c) 2022 Red Hat, Inc.
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
#include <unistd.h>

/* library headers */
#include "lib/sset.h"
#include "lib/util.h"

/* OVS includes. */
#include "lib/vswitch-idl.h"
#include "openvswitch/vlog.h"

/* OVN includes. */
#include "lib/ovn-sb-idl.h"
#include "mirror.h"

VLOG_DEFINE_THIS_MODULE(port_mirror);

/* Static function declarations */

static const struct ovsrec_port *
get_port_for_iface(const struct ovsrec_interface *iface,
                  const struct ovsrec_bridge *br_int)
{
    for (size_t i = 0; i < br_int->n_ports; i++) {
        const struct ovsrec_port *p = br_int->ports[i];
        for (size_t j = 0; j < p->n_interfaces; j++) {
            if (!strcmp(iface->name, p->interfaces[j]->name)) {
                return p;
            }
        }
    }
    return NULL;
}

static void
mirror_create(const struct sbrec_port_binding *pb,
              struct port_mirror_ctx *pm_ctx,
              const struct ovsrec_mirror *mirror)
{
    if (pb->up[0] == true) {
        VLOG_INFO("Mirror rule(s) present for %s ", pb->logical_port);
        /* Loop through the mirror rules */
        for (int i =0; i < pb->n_mirror_rules; i++) {
            /* check if the mirror already exists in OVS DB */
            bool create_mirror = true;
            OVSREC_MIRROR_TABLE_FOR_EACH (mirror, pm_ctx->mirror_table) {
                if (!strcmp(pb->mirror_rules[i]->name, mirror->name)) {
                      /* Mirror with same name already exists
                       * No need to create mirror
                       */
                      create_mirror = false;
                      break;
                }
            }

            if (create_mirror) {

                struct smap options = SMAP_INITIALIZER(&options);
                char *port_name, *key;

                key = xasprintf("%ld",(long int)pb->mirror_rules[i]->index);
                smap_add(&options, "remote_ip", pb->mirror_rules[i]->sink);

                if (!strcmp(pb->mirror_rules[i]->type, "gre")) {
                    /* Set the GRE key */
                    smap_add(&options, "key", key);

                } else if (!strcmp(pb->mirror_rules[i]->type, "erspan")) {
                    /* Set the ERSPAN index */
                    smap_add(&options, "key", key);
                    smap_add(&options, "erspan_idx", key);
                    smap_add(&options, "erspan_ver","1");

                }
                struct ovsrec_interface *iface =
                          ovsrec_interface_insert(pm_ctx->ovs_idl_txn);
                port_name = xasprintf("ovn-%s-%s",
                                       pb->mirror_rules[i]->type,
                                       pb->mirror_rules[i]->name);

                ovsrec_interface_set_name(iface, port_name);
                ovsrec_interface_set_type(iface, pb->mirror_rules[i]->type);
                ovsrec_interface_set_options(iface, &options);

                struct ovsrec_port *port =
                                  ovsrec_port_insert(pm_ctx->ovs_idl_txn);
                ovsrec_port_set_name(port, port_name);
                ovsrec_port_set_interfaces(port, &iface, 1);

                ovsrec_bridge_update_ports_addvalue(pm_ctx->br_int, port);

                smap_destroy(&options);
                free(port_name);
                free(key);

                VLOG_INFO("Creating Mirror in OVS DB");
                mirror = ovsrec_mirror_insert(pm_ctx->ovs_idl_txn);
                ovsrec_mirror_set_name(mirror,pb->mirror_rules[i]->name);
                ovsrec_mirror_update_output_port_addvalue(mirror, port);
                ovsrec_bridge_update_mirrors_addvalue(pm_ctx->br_int,
                                                                 mirror);
            }

            const struct ovsrec_interface *iface_rec;
            const char *iface_id;
            /* find the interface corresponding to the pb->logical_port */
            OVSREC_INTERFACE_TABLE_FOR_EACH (iface_rec,
                                             pm_ctx->iface_table) {
                iface_id = smap_get(&iface_rec->external_ids, "iface-id");
                if (iface_id) {
                    if (!strcmp(iface_id, pb->logical_port)) {
                        VLOG_INFO("Found the interface mapped to physical");
                        break;
                    }
                }
            }

            const struct ovsrec_port *p =
                              get_port_for_iface(iface_rec, pm_ctx->br_int);
            if (p) {
                if (!strcmp(pb->mirror_rules[i]->filter,"from-lport")) {
                    ovsrec_mirror_update_select_src_port_addvalue(mirror, p);
                } else if (!strcmp(pb->mirror_rules[i]->filter,"to-lport")) {
                    ovsrec_mirror_update_select_dst_port_addvalue(mirror, p);
                } else {
                    ovsrec_mirror_update_select_src_port_addvalue(mirror, p);
                    ovsrec_mirror_update_select_dst_port_addvalue(mirror, p);
                }
            }
        }
    }
}

static void
mirror_delete(const struct sbrec_port_binding *pb,
              struct port_mirror_ctx *pm_ctx,
              struct shash *pb_mirror_map,
              bool delete_all)
{
    struct sset pb_mirrors = SSET_INITIALIZER(&pb_mirrors);

    if (delete_all != true) {
        for (size_t i = 0; i < pb->n_mirror_rules ; i++) {
            sset_add(&pb_mirrors, pb->mirror_rules[i]->name);
        }
    }

    struct shash_node *mirror_node;
    SHASH_FOR_EACH (mirror_node, pb_mirror_map) {
        struct ovsrec_mirror *ovs_mirror = mirror_node->data;
        if (!sset_find(&pb_mirrors, ovs_mirror->name)) {
            /* Find if the mirror has other sources i*/
            if ((ovs_mirror->n_select_dst_port > 1) ||
                (ovs_mirror->n_select_src_port > 1)) {
                /* More than 1 source then just
                 * update the mirror table
                 */
                bool done = false;
                for (size_t i = 0; ((i < ovs_mirror->n_select_dst_port)
                                                   && (done == false)); i++) {
                    const struct ovsrec_port *port_rec =
                                               ovs_mirror->select_dst_port[i];
                    for (size_t j = 0; j < port_rec->n_interfaces; j++) {
                        const struct ovsrec_interface *iface_rec;

                        iface_rec = port_rec->interfaces[j];
                        const char *iface_id =
                                            smap_get(&iface_rec->external_ids,
                                                                  "iface-id");
                        if (!strcmp(iface_id,pb->logical_port)) {
                            ovsrec_mirror_update_select_dst_port_delvalue(
                                                        ovs_mirror, port_rec);
                            done = true;
                            break;
                        }
                    }
                }
                done = false;
                for (size_t i = 0; ((i < ovs_mirror->n_select_src_port)
                                                   && (done == false)); i++) {
                    const struct ovsrec_port *port_rec =
                                                ovs_mirror->select_src_port[i];
                    for (size_t j = 0; j < port_rec->n_interfaces; j++) {
                        const struct ovsrec_interface *iface_rec;

                        iface_rec = port_rec->interfaces[j];
                        const char *iface_id =
                                            smap_get(&iface_rec->external_ids,
                                                                  "iface-id");
                        if (!strcmp(iface_id,pb->logical_port)) {
                            ovsrec_mirror_update_select_src_port_delvalue(
                                                        ovs_mirror, port_rec);
                            done = true;
                            break;
                        }
                    }
                }
            } else {
                /*
                 * If only 1 source delete the output port
                 * and then delete the mirror completely
                 */
                VLOG_INFO("Only 1 source for the mirror. Hence delete it");
                ovsrec_bridge_update_ports_delvalue(pm_ctx->br_int,
                                                    ovs_mirror->output_port);
                ovsrec_bridge_update_mirrors_delvalue(pm_ctx->br_int,
                                                            ovs_mirror);
                ovsrec_port_delete(ovs_mirror->output_port);
                ovsrec_mirror_delete(ovs_mirror);
            }
        }
    }

    const char *used_node, *used_next;
    SSET_FOR_EACH_SAFE (used_node, used_next, &pb_mirrors) {
        sset_delete(&pb_mirrors, SSET_NODE_FROM_NAME(used_node));
    }
    sset_destroy(&pb_mirrors);
}

static void
find_port_specific_mirrors (const struct sbrec_port_binding *pb,
                            struct port_mirror_ctx *pm_ctx,
                            const struct ovsrec_mirror *mirror,
                            struct shash *pb_mirror_map)
{
    OVSREC_MIRROR_TABLE_FOR_EACH (mirror, pm_ctx->mirror_table) {
        for (size_t i = 0; i < mirror->n_select_dst_port; i++) {
            const struct ovsrec_port *port_rec = mirror->select_dst_port[i];
            for (size_t j = 0; j < port_rec->n_interfaces; j++) {
                const struct ovsrec_interface *iface_rec;
                iface_rec = port_rec->interfaces[j];
                const char *logical_port =
                    smap_get(&iface_rec->external_ids, "iface-id");
                if (!strcmp(logical_port, pb->logical_port)) {
                    shash_add_once(pb_mirror_map, mirror->name, mirror);
                }
            }
        }
        for (size_t i = 0; i < mirror->n_select_src_port; i++) {
            const struct ovsrec_port *port_rec = mirror->select_src_port[i];
            for (size_t j = 0; j < port_rec->n_interfaces; j++) {
                const struct ovsrec_interface *iface_rec;
                iface_rec = port_rec->interfaces[j];
                const char *logical_port =
                    smap_get(&iface_rec->external_ids, "iface-id");
                if (!strcmp(logical_port, pb->logical_port)) {
                    shash_add_once(pb_mirror_map, mirror->name, mirror);
                }
            }
        }
    }
}

void
mirror_register_ovs_idl(struct ovsdb_idl *ovs_idl)
{
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_mirrors);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_mirror);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_mirror_col_name);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_mirror_col_output_port);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_mirror_col_select_dst_port);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_mirror_col_select_src_port);
}


void
ovn_port_mirror_init(struct shash *ovs_mirrors)
{
    shash_init(ovs_mirrors);
}

void
ovn_port_mirror_run(struct port_mirror_ctx *pm_ctx)
{
    struct shash_node *ovs_mirror_node, *ovs_mirror_next;
    SHASH_FOR_EACH_SAFE (ovs_mirror_node, ovs_mirror_next,
                                        pm_ctx->ovs_mirrors) {
        shash_delete(pm_ctx->ovs_mirrors, ovs_mirror_node);
    }

    const struct ovsrec_mirror *ovsmirror = NULL;
    OVSREC_MIRROR_TABLE_FOR_EACH (ovsmirror, pm_ctx->mirror_table) {
       shash_add(pm_ctx->ovs_mirrors, ovsmirror->name, ovsmirror);
    }

}

bool
ovn_port_mirror_handle_lport(const struct sbrec_port_binding *pb, bool removed,
                     struct port_mirror_ctx *pm_ctx)
{

    const struct ovsrec_mirror *mirror = NULL;
    struct shash port_ovs_mirrors = SHASH_INITIALIZER(&port_ovs_mirrors);

    /* Need to find if mirror needs update */
    find_port_specific_mirrors(pb, pm_ctx, mirror, &port_ovs_mirrors);

    if (removed == false) {
        if (((pb->n_mirror_rules == 0)
              && (shash_is_empty(&port_ovs_mirrors))) ||
              (pb->n_mirror_rules == shash_count(&port_ovs_mirrors))) {
            /* No mirror update */
        } else {
            /* Update Mirror */
            if (pb->n_mirror_rules > shash_count(&port_ovs_mirrors)) {
                /* create mirror,
                 * if mirror already exists only update selection
                 */
                mirror_create(pb, pm_ctx, mirror);
            } else {
                /* delete mirror,
                 * if mirror has other sources only update selection
                 */
                mirror_delete(pb, pm_ctx, &port_ovs_mirrors, false);
            }
        }
    } else {
           mirror_delete(pb, pm_ctx, &port_ovs_mirrors, true);
    }

    struct shash_node *ovs_mirror_node, *ovs_mirror_next;
    SHASH_FOR_EACH_SAFE (ovs_mirror_node, ovs_mirror_next,
                                              &port_ovs_mirrors) {
        shash_delete(&port_ovs_mirrors, ovs_mirror_node);
    }
    shash_destroy(&port_ovs_mirrors);

    return true;
}

bool
ovn_port_mirror_handle_update(struct port_mirror_ctx *pm_ctx)
{
    const struct sbrec_mirror *mirror = NULL;
    struct sset sb_mirrors = SSET_INITIALIZER(&sb_mirrors);
    SBREC_MIRROR_TABLE_FOR_EACH (mirror, pm_ctx->sb_mirror_table) {
        sset_add(&sb_mirrors, mirror->name);
    }

    struct shash_node *mirror_node;
    SHASH_FOR_EACH (mirror_node, pm_ctx->ovs_mirrors) {
        struct ovsrec_mirror *ovs_mirror = mirror_node->data;
        if (!sset_find(&sb_mirrors, ovs_mirror->name)) {
            /* Need to delete the mirror in OVS */
            VLOG_INFO("Delete mirror and remove port");
            ovsrec_bridge_update_ports_delvalue(pm_ctx->br_int,
                                                ovs_mirror->output_port);
            ovsrec_bridge_update_mirrors_delvalue(pm_ctx->br_int,
                                                  ovs_mirror);
            ovsrec_port_delete(ovs_mirror->output_port);
            ovsrec_mirror_delete(ovs_mirror);
        }
    }

    const char *used_node, *used_next;
    SSET_FOR_EACH_SAFE (used_node, used_next, &sb_mirrors) {
        sset_delete(&sb_mirrors, SSET_NODE_FROM_NAME(used_node));
    }
    sset_destroy(&sb_mirrors);

    return true;
}

void
ovn_port_mirror_destroy(struct shash *ovs_mirrors)
{
    struct shash_node *ovs_mirror_node, *ovs_mirror_next;
    SHASH_FOR_EACH_SAFE (ovs_mirror_node, ovs_mirror_next,
                                              ovs_mirrors) {
        shash_delete(ovs_mirrors, ovs_mirror_node);
    }
    shash_destroy(ovs_mirrors);
}

