/*
 * Copyright (c) 2023, Red Hat, Inc.
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

#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>

/* OVS includes */
#include "include/openvswitch/hmap.h"
#include "lib/bitmap.h"
#include "lib/socket-util.h"
#include "lib/uuidset.h"
#include "openvswitch/util.h"
#include "openvswitch/vlog.h"
#include "stopwatch.h"

/* OVN includes */
#include "en-lb-data.h"
#include "en-lr-nat.h"
#include "en-lr-stateful.h"
#include "lib/inc-proc-eng.h"
#include "lib/lb.h"
#include "lib/ovn-nb-idl.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-util.h"
#include "lib/stopwatch-names.h"
#include "northd.h"

VLOG_DEFINE_THIS_MODULE(en_lr_stateful);

/* Static function declarations. */
static void lr_stateful_table_init(struct lr_stateful_table *);
static void lr_stateful_table_clear(struct lr_stateful_table *);
static void lr_stateful_table_destroy(struct lr_stateful_table *);
static struct lr_stateful_record *lr_stateful_table_find_(
    const struct lr_stateful_table *, const struct nbrec_logical_router *);
static struct lr_stateful_record *lr_stateful_table_find_by_index_(
    const struct lr_stateful_table *table, size_t od_index);

static void lr_stateful_table_build(struct lr_stateful_table *,
                                   const struct lr_nat_table *,
                                   const struct ovn_datapaths *lr_datapaths,
                                   const struct hmap *lb_datapaths_map,
                                   const struct hmap *lbgrp_datapaths_map);

static struct lr_stateful_input lr_stateful_get_input_data(
    struct engine_node *);

static struct lr_stateful_record *lr_stateful_record_create(
    struct lr_stateful_table *, const struct lr_nat_record *,
    const struct hmap *lb_datapaths_map,
    const struct hmap *lbgrp_datapaths_map);
static void lr_stateful_record_destroy(struct lr_stateful_record *);
static void lr_stateful_record_init(
    struct lr_stateful_record *,
    const struct hmap *lb_datapaths_map,
    const struct hmap *lbgrp_datapaths_map);

static void build_lrouter_lb_reachable_ips(struct lr_stateful_record *,
                                           const struct ovn_northd_lb *);
static void add_neigh_ips_to_lrouter(struct lr_stateful_record *,
                                     enum lb_neighbor_responder_mode,
                                     const struct sset *lb_ips_v4,
                                     const struct sset *lb_ips_v6);
static void remove_lrouter_lb_reachable_ips(struct lr_stateful_record *,
                                            enum lb_neighbor_responder_mode,
                                            const struct sset *lb_ips_v4,
                                            const struct sset *lb_ips_v6);
static void lr_stateful_build_vip_nats(struct lr_stateful_record *);

/* 'lr_stateful' engine node manages the NB logical router LB data.
 */
void *
en_lr_stateful_init(struct engine_node *node OVS_UNUSED,
                    struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_lr_stateful *data = xzalloc(sizeof *data);
    lr_stateful_table_init(&data->table);
    hmapx_init(&data->trk_data.crupdated);
    return data;
}

void
en_lr_stateful_cleanup(void *data_)
{
    struct ed_type_lr_stateful *data = data_;
    lr_stateful_table_destroy(&data->table);
    hmapx_destroy(&data->trk_data.crupdated);
}

void
en_lr_stateful_clear_tracked_data(void *data_)
{
    struct ed_type_lr_stateful *data = data_;

    hmapx_clear(&data->trk_data.crupdated);
}

void
en_lr_stateful_run(struct engine_node *node, void *data_)
{
    struct lr_stateful_input input_data = lr_stateful_get_input_data(node);
    struct ed_type_lr_stateful *data = data_;

    stopwatch_start(LR_STATEFUL_RUN_STOPWATCH_NAME, time_msec());

    lr_stateful_table_clear(&data->table);
    lr_stateful_table_build(&data->table, input_data.lr_nats,
                            input_data.lr_datapaths,
                            input_data.lb_datapaths_map,
                            input_data.lbgrp_datapaths_map);

    stopwatch_stop(LR_STATEFUL_RUN_STOPWATCH_NAME, time_msec());
    engine_set_node_state(node, EN_UPDATED);
}

bool
lr_stateful_northd_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    struct northd_data *northd_data = engine_get_input_data("northd", node);
    if (!northd_has_tracked_data(&northd_data->trk_data)) {
        return false;
    }

    /* lr_stateful node needs inputs for any changes to NAT and load balancers.
     * Changes to NAT is provided by the lr_nat tracked data and changes
     * to lbs and lb grps is provided by lb_data's tracked data.
     * So we don't need to do anything here for northd changes.
     * But we do need to access the datapaths and lb_datapaths from the
     * northd engine node and hence its an input.
     * */
    return true;
}

bool
lr_stateful_lb_data_handler(struct engine_node *node, void *data_)
{
    struct ed_type_lb_data *lb_data = engine_get_input_data("lb_data", node);
    if (!lb_data->tracked) {
        return false;
    }

    struct ed_type_lr_stateful *data = data_;
    struct lr_stateful_input input_data =
        lr_stateful_get_input_data(node);

    const struct tracked_lb_data *trk_lb_data = &lb_data->tracked_lb_data;
    const struct crupdated_od_lb_data *codlb;

    LIST_FOR_EACH (codlb, list_node, &trk_lb_data->crupdated_lr_lbs) {
        const struct ovn_datapath *od = ovn_datapath_find(
            &input_data.lr_datapaths->datapaths, &codlb->od_uuid);
        ovs_assert(od);

        struct lr_stateful_record *lr_stateful_rec =lr_stateful_table_find_(
            &data->table, od->nbr);
        if (!lr_stateful_rec) {
            const struct lr_nat_record *lrnat_rec = lr_nat_table_find_by_index(
                input_data.lr_nats, od->index);
            ovs_assert(lrnat_rec);

            lr_stateful_rec = lr_stateful_record_create(&data->table,
                                            lrnat_rec,
                                            input_data.lb_datapaths_map,
                                            input_data.lbgrp_datapaths_map);

            /* Add the lr_stateful_rec rec to the tracking data. */
            hmapx_add(&data->trk_data.crupdated, lr_stateful_rec);
            continue;
        }

        /* Update. */
        struct uuidset_node *uuidnode;
        UUIDSET_FOR_EACH (uuidnode, &codlb->assoc_lbs) {
            const struct ovn_lb_datapaths *lb_dps = ovn_lb_datapaths_find(
                input_data.lb_datapaths_map, &uuidnode->uuid);
            ovs_assert(lb_dps);

            /* Add the lb_ips of lb_dps to the od. */
            build_lrouter_lb_ips(lr_stateful_rec->lb_ips, lb_dps->lb);
            build_lrouter_lb_reachable_ips(lr_stateful_rec, lb_dps->lb);
        }

        UUIDSET_FOR_EACH (uuidnode, &codlb->assoc_lbgrps) {
            const struct ovn_lb_group_datapaths *lbgrp_dps =
                ovn_lb_group_datapaths_find(input_data.lbgrp_datapaths_map,
                                            &uuidnode->uuid);
            ovs_assert(lbgrp_dps);

            for (size_t j = 0; j < lbgrp_dps->lb_group->n_lbs; j++) {
                const struct uuid *lb_uuid
                    = &lbgrp_dps->lb_group->lbs[j]->nlb->header_.uuid;
                const struct ovn_lb_datapaths *lb_dps = ovn_lb_datapaths_find(
                    input_data.lb_datapaths_map, lb_uuid);
                ovs_assert(lb_dps);

                /* Add the lb_ips of lb_dps to the od. */
                build_lrouter_lb_ips(lr_stateful_rec->lb_ips, lb_dps->lb);
                build_lrouter_lb_reachable_ips(lr_stateful_rec, lb_dps->lb);
            }
        }

        /* Add the lr_stateful_rec rec to the tracking data. */
        hmapx_add(&data->trk_data.crupdated, lr_stateful_rec);
    }

    const struct crupdated_lb *clb;
    HMAP_FOR_EACH (clb, hmap_node, &trk_lb_data->crupdated_lbs) {
        const struct uuid *lb_uuid = &clb->lb->nlb->header_.uuid;
        const struct ovn_northd_lb *lb = clb->lb;

        const struct ovn_lb_datapaths *lb_dps = ovn_lb_datapaths_find(
            input_data.lb_datapaths_map, lb_uuid);
        ovs_assert(lb_dps);

        size_t index;
        BITMAP_FOR_EACH_1 (index, ods_size(input_data.lr_datapaths),
                           lb_dps->nb_lr_map) {
            const struct ovn_datapath *od =
                input_data.lr_datapaths->array[index];

            struct lr_stateful_record *lr_stateful_rec =
                lr_stateful_table_find_(&data->table, od->nbr);
            ovs_assert(lr_stateful_rec);

            /* Update the od->lb_ips with the deleted and inserted
             * vips (if any). */
            remove_ips_from_lb_ip_set(lr_stateful_rec->lb_ips, lb->routable,
                                      &clb->deleted_vips_v4,
                                      &clb->deleted_vips_v6);
            add_ips_to_lb_ip_set(lr_stateful_rec->lb_ips, lb->routable,
                                 &clb->inserted_vips_v4,
                                 &clb->inserted_vips_v6);

            remove_lrouter_lb_reachable_ips(lr_stateful_rec, lb->neigh_mode,
                                            &clb->deleted_vips_v4,
                                            &clb->deleted_vips_v6);
            add_neigh_ips_to_lrouter(lr_stateful_rec, lb->neigh_mode,
                                     &clb->inserted_vips_v4,
                                     &clb->inserted_vips_v6);

            /* Add the lr_stateful_rec rec to the tracking data. */
            hmapx_add(&data->trk_data.crupdated, lr_stateful_rec);
        }
    }

    const struct crupdated_lbgrp *crupdated_lbgrp;
    HMAP_FOR_EACH (crupdated_lbgrp, hmap_node,
                   &trk_lb_data->crupdated_lbgrps) {
        const struct uuid *lb_uuid = &crupdated_lbgrp->lbgrp->uuid;
        const struct ovn_lb_group_datapaths *lbgrp_dps =
            ovn_lb_group_datapaths_find(input_data.lbgrp_datapaths_map,
                                        lb_uuid);
        ovs_assert(lbgrp_dps);

        struct hmapx_node *hnode;
        HMAPX_FOR_EACH (hnode, &crupdated_lbgrp->assoc_lbs) {
            const struct ovn_northd_lb *lb = hnode->data;
            lb_uuid = &lb->nlb->header_.uuid;
            const struct ovn_lb_datapaths *lb_dps = ovn_lb_datapaths_find(
                input_data.lb_datapaths_map, lb_uuid);
            ovs_assert(lb_dps);
            for (size_t i = 0; i < lbgrp_dps->n_lr; i++) {
                const struct ovn_datapath *od = lbgrp_dps->lr[i];
                struct lr_stateful_record *lr_stateful_rec =
                    lr_stateful_table_find_(&data->table, od->nbr);
                ovs_assert(lr_stateful_rec);
                /* Add the lb_ips of lb_dps to the lr lb data. */
                build_lrouter_lb_ips(lr_stateful_rec->lb_ips, lb_dps->lb);
                build_lrouter_lb_reachable_ips(lr_stateful_rec, lb_dps->lb);

                /* Add the lr_stateful_rec rec to the tracking data. */
                hmapx_add(&data->trk_data.crupdated, lr_stateful_rec);
            }
        }
    }

    if (lr_stateful_has_tracked_data(&data->trk_data)) {
        struct hmapx_node *hmapx_node;
        /* For all the modified lr_stateful records (re)build the
         * vip nats. */
        HMAPX_FOR_EACH (hmapx_node, &data->trk_data.crupdated) {
            struct lr_stateful_record *lr_stateful_rec = hmapx_node->data;
            lr_stateful_build_vip_nats(lr_stateful_rec);
            lr_stateful_rec->has_lb_vip = od_has_lb_vip(lr_stateful_rec->od);
        }

        engine_set_node_state(node, EN_UPDATED);
    }

    return true;
}

bool
lr_stateful_lr_nat_handler(struct engine_node *node, void *data_)
{
    struct ed_type_lr_nat_data *lr_nat_data =
        engine_get_input_data("lr_nat", node);

    if (!lr_nat_has_tracked_data(&lr_nat_data->trk_data)) {
        return false;
    }

    struct ed_type_lr_stateful *data = data_;
    struct lr_stateful_input input_data =
        lr_stateful_get_input_data(node);
    struct hmapx_node *hmapx_node;

    HMAPX_FOR_EACH (hmapx_node, &lr_nat_data->trk_data.crupdated) {
        const struct lr_nat_record *lrnat_rec = hmapx_node->data;
        struct lr_stateful_record *lr_stateful_rec =
            lr_stateful_table_find_(&data->table, lrnat_rec->od->nbr);
        if (!lr_stateful_rec) {
            lr_stateful_rec = lr_stateful_record_create(&data->table,
                                            lrnat_rec,
                                            input_data.lb_datapaths_map,
                                            input_data.lbgrp_datapaths_map);
        } else {
            lr_stateful_build_vip_nats(lr_stateful_rec);
        }

        /* Add the lr_stateful_rec rec to the tracking data. */
        hmapx_add(&data->trk_data.crupdated, lr_stateful_rec);
    }

    if (lr_stateful_has_tracked_data(&data->trk_data)) {
        engine_set_node_state(node, EN_UPDATED);
    }

    return true;
}

const struct lr_stateful_record *
lr_stateful_table_find_by_index(const struct lr_stateful_table *table,
                                   size_t od_index)
{
    return lr_stateful_table_find_by_index_(table, od_index);
}

/* static functions. */
static void
lr_stateful_table_init(struct lr_stateful_table *table)
{
    *table = (struct lr_stateful_table) {
        .entries = HMAP_INITIALIZER(&table->entries),
    };
}

static void
lr_stateful_table_destroy(struct lr_stateful_table *table)
{
    lr_stateful_table_clear(table);
    hmap_destroy(&table->entries);
}

static void
lr_stateful_table_clear(struct lr_stateful_table *table)
{
    struct lr_stateful_record *lr_stateful_rec;
    HMAP_FOR_EACH_POP (lr_stateful_rec, key_node, &table->entries) {
        lr_stateful_record_destroy(lr_stateful_rec);
    }

    free(table->array);
    table->array = NULL;
}

static void
lr_stateful_table_build(struct lr_stateful_table *table,
                       const struct lr_nat_table *lr_nats,
                       const struct ovn_datapaths *lr_datapaths,
                       const struct hmap *lb_datapaths_map,
                       const struct hmap *lbgrp_datapaths_map)
{
    table->array = xrealloc(table->array,
                            ods_size(lr_datapaths) * sizeof *table->array);
    const struct lr_nat_record *lrnat_rec;
    LR_NAT_TABLE_FOR_EACH (lrnat_rec, lr_nats) {
        lr_stateful_record_create(table, lrnat_rec, lb_datapaths_map,
                                     lbgrp_datapaths_map);
    }
}

static struct lr_stateful_record *
lr_stateful_table_find_(const struct lr_stateful_table *table,
                  const struct nbrec_logical_router *nbr)
{
    struct lr_stateful_record *lr_stateful_rec;

    HMAP_FOR_EACH_WITH_HASH (lr_stateful_rec, key_node,
                             uuid_hash(&nbr->header_.uuid), &table->entries) {
        if (nbr == lr_stateful_rec->od->nbr) {
            return lr_stateful_rec;
        }
    }
    return NULL;
}

static struct lr_stateful_record *
lr_stateful_table_find_by_index_(const struct lr_stateful_table *table,
                                   size_t od_index)
{
    ovs_assert(od_index <= hmap_count(&table->entries));
    return table->array[od_index];
}

static struct lr_stateful_record *
lr_stateful_record_create(struct lr_stateful_table *table,
                         const struct lr_nat_record *lrnat_rec,
                         const struct hmap *lb_datapaths_map,
                         const struct hmap *lbgrp_datapaths_map)
{
    struct lr_stateful_record *lr_stateful_rec =
        xzalloc(sizeof *lr_stateful_rec);
    lr_stateful_rec->lrnat_rec = lrnat_rec;
    lr_stateful_rec->od = lrnat_rec->od;
    lr_stateful_record_init(lr_stateful_rec, lb_datapaths_map,
                               lbgrp_datapaths_map);

    hmap_insert(&table->entries, &lr_stateful_rec->key_node,
                uuid_hash(&lr_stateful_rec->od->nbr->header_.uuid));

    table->array[lr_stateful_rec->od->index] = lr_stateful_rec;
    return lr_stateful_rec;
}

static void
lr_stateful_record_destroy(struct lr_stateful_record *lr_stateful_rec)
{
    ovn_lb_ip_set_destroy(lr_stateful_rec->lb_ips);
    lr_stateful_rec->lb_ips = NULL;
    sset_destroy(&lr_stateful_rec->vip_nats);
    free(lr_stateful_rec);
}

static void
lr_stateful_record_init(struct lr_stateful_record *lr_stateful_rec,
                           const struct hmap *lb_datapaths_map,
                           const struct hmap *lbgrp_datapaths_map)
{
    /* Checking load balancer groups first, starting from the largest one,
     * to more efficiently copy IP sets. */
    size_t largest_group = 0;

    const struct nbrec_logical_router *nbr = lr_stateful_rec->od->nbr;
    for (size_t i = 1; i < nbr->n_load_balancer_group; i++) {
        if (nbr->load_balancer_group[i]->n_load_balancer >
                nbr->load_balancer_group[largest_group]->n_load_balancer) {
            largest_group = i;
        }
    }

    for (size_t i = 0; i < nbr->n_load_balancer_group; i++) {
        size_t idx = (i + largest_group) % nbr->n_load_balancer_group;

        const struct nbrec_load_balancer_group *nbrec_lb_group =
            nbr->load_balancer_group[idx];
        const struct uuid *lbgrp_uuid = &nbrec_lb_group->header_.uuid;

         const struct ovn_lb_group_datapaths *lbgrp_dps =
            ovn_lb_group_datapaths_find(lbgrp_datapaths_map,
                                        lbgrp_uuid);
        ovs_assert(lbgrp_dps);

        if (!lr_stateful_rec->lb_ips) {
            lr_stateful_rec->lb_ips =
                ovn_lb_ip_set_clone(lbgrp_dps->lb_group->lb_ips);
        } else {
            for (size_t j = 0; j < lbgrp_dps->lb_group->n_lbs; j++) {
                build_lrouter_lb_ips(lr_stateful_rec->lb_ips,
                                     lbgrp_dps->lb_group->lbs[j]);
            }
        }

        for (size_t j = 0; j < lbgrp_dps->lb_group->n_lbs; j++) {
            build_lrouter_lb_reachable_ips(lr_stateful_rec,
                                           lbgrp_dps->lb_group->lbs[j]);
        }
    }

    if (!lr_stateful_rec->lb_ips) {
        lr_stateful_rec->lb_ips = ovn_lb_ip_set_create();
    }

    for (size_t i = 0; i < nbr->n_load_balancer; i++) {
        const struct uuid *lb_uuid =
            &nbr->load_balancer[i]->header_.uuid;
        const struct ovn_lb_datapaths *lb_dps =
            ovn_lb_datapaths_find(lb_datapaths_map, lb_uuid);
        ovs_assert(lb_dps);
        build_lrouter_lb_ips(lr_stateful_rec->lb_ips, lb_dps->lb);
        build_lrouter_lb_reachable_ips(lr_stateful_rec, lb_dps->lb);
    }

    sset_init(&lr_stateful_rec->vip_nats);

    if (nbr->n_nat) {
        lr_stateful_build_vip_nats(lr_stateful_rec);
    }

    lr_stateful_rec->has_lb_vip = od_has_lb_vip(lr_stateful_rec->od);
}

static struct lr_stateful_input
lr_stateful_get_input_data(struct engine_node *node)
{
    struct northd_data *northd_data = engine_get_input_data("northd", node);
    struct ed_type_lr_nat_data *lr_nat_data =
        engine_get_input_data("lr_nat", node);

    return (struct lr_stateful_input) {
        .lr_datapaths = &northd_data->lr_datapaths,
        .lb_datapaths_map = &northd_data->lb_datapaths_map,
        .lbgrp_datapaths_map = &northd_data->lb_group_datapaths_map,
        .lr_nats = &lr_nat_data->lr_nats,
    };
}

static void
build_lrouter_lb_reachable_ips(struct lr_stateful_record *lr_stateful_rec,
                               const struct ovn_northd_lb *lb)
{
    add_neigh_ips_to_lrouter(lr_stateful_rec, lb->neigh_mode, &lb->ips_v4,
                             &lb->ips_v6);
}

static void
add_neigh_ips_to_lrouter(struct lr_stateful_record *lr_stateful_rec,
                         enum lb_neighbor_responder_mode neigh_mode,
                         const struct sset *lb_ips_v4,
                         const struct sset *lb_ips_v6)
{
    /* If configured to not reply to any neighbor requests for all VIPs
     * return early.
     */
    if (neigh_mode == LB_NEIGH_RESPOND_NONE) {
        return;
    }

    const char *ip_address;

    /* If configured to reply to neighbor requests for all VIPs force them
     * all to be considered "reachable".
     */
    if (neigh_mode == LB_NEIGH_RESPOND_ALL) {
        SSET_FOR_EACH (ip_address, lb_ips_v4) {
            sset_add(&lr_stateful_rec->lb_ips->ips_v4_reachable, ip_address);
        }
        SSET_FOR_EACH (ip_address, lb_ips_v6) {
            sset_add(&lr_stateful_rec->lb_ips->ips_v6_reachable, ip_address);
        }

        return;
    }

    /* Otherwise, a VIP is reachable if there's at least one router
     * subnet that includes it.
     */
    ovs_assert(neigh_mode == LB_NEIGH_RESPOND_REACHABLE);

    SSET_FOR_EACH (ip_address, lb_ips_v4) {
        struct ovn_port *op;
        ovs_be32 vip_ip4;
        if (ip_parse(ip_address, &vip_ip4)) {
            HMAP_FOR_EACH (op, dp_node, &lr_stateful_rec->od->ports) {
                if (lrouter_port_ipv4_reachable(op, vip_ip4)) {
                    sset_add(&lr_stateful_rec->lb_ips->ips_v4_reachable,
                             ip_address);
                    break;
                }
            }
        }
    }

    SSET_FOR_EACH (ip_address, lb_ips_v6) {
        struct ovn_port *op;
        struct in6_addr vip;
        if (ipv6_parse(ip_address, &vip)) {
            HMAP_FOR_EACH (op, dp_node, &lr_stateful_rec->od->ports) {
                if (lrouter_port_ipv6_reachable(op, &vip)) {
                    sset_add(&lr_stateful_rec->lb_ips->ips_v6_reachable,
                             ip_address);
                    break;
                }
            }
        }
    }
}

static void
remove_lrouter_lb_reachable_ips(struct lr_stateful_record *lr_stateful_rec,
                                enum lb_neighbor_responder_mode neigh_mode,
                                const struct sset *lb_ips_v4,
                                const struct sset *lb_ips_v6)
{
    if (neigh_mode == LB_NEIGH_RESPOND_NONE) {
        return;
    }

    const char *ip_address;
    SSET_FOR_EACH (ip_address, lb_ips_v4) {
        sset_find_and_delete(&lr_stateful_rec->lb_ips->ips_v4_reachable,
                             ip_address);
    }
    SSET_FOR_EACH (ip_address, lb_ips_v6) {
        sset_find_and_delete(&lr_stateful_rec->lb_ips->ips_v6_reachable,
                             ip_address);
    }
}

static void
lr_stateful_build_vip_nats(struct lr_stateful_record *lr_stateful_rec)
{
    sset_clear(&lr_stateful_rec->vip_nats);
    const char *external_ip;
    SSET_FOR_EACH (external_ip, &lr_stateful_rec->lrnat_rec->external_ips) {
        bool is_vip_nat = false;
        if (addr_is_ipv6(external_ip)) {
            is_vip_nat = sset_contains(&lr_stateful_rec->lb_ips->ips_v6,
                                       external_ip);
        } else {
            is_vip_nat = sset_contains(&lr_stateful_rec->lb_ips->ips_v4,
                                       external_ip);
        }

        if (is_vip_nat) {
            sset_add(&lr_stateful_rec->vip_nats, external_ip);
        }
    }
}
