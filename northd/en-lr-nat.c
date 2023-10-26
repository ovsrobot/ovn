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
#include "openvswitch/util.h"
#include "openvswitch/vlog.h"
#include "stopwatch.h"

/* OVN includes */
#include "en-lr-nat.h"
#include "lib/inc-proc-eng.h"
#include "lib/lb.h"
#include "lib/ovn-nb-idl.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-util.h"
#include "lib/stopwatch-names.h"
#include "northd.h"

VLOG_DEFINE_THIS_MODULE(en_lr_nat);

/* Static function declarations. */
static void lr_nat_table_init(struct lr_nat_table *);
static void lr_nat_table_clear(struct lr_nat_table *);
static void lr_nat_table_destroy(struct lr_nat_table *);
static void lr_nat_table_build(struct lr_nat_table *,
                               const struct ovn_datapaths *lr_datapaths);
static struct lr_nat_record *lr_nat_table_find_(const struct lr_nat_table *,
                                         const struct nbrec_logical_router *);
static struct lr_nat_record *lr_nat_table_find_by_index_(
    const struct lr_nat_table *, size_t od_index);

static struct lr_nat_record *lr_nat_record_create(
    struct lr_nat_table *, const struct ovn_datapath *);
static void lr_nat_record_init(struct lr_nat_record *);
static void lr_nat_record_reinit(struct lr_nat_record *);
static void lr_nat_record_destroy(struct lr_nat_record *);

static void lr_nat_entries_init(struct lr_nat_record *);
static void lr_nat_entries_destroy(struct lr_nat_record *);
static void lr_nat_external_ips_init(struct lr_nat_record *);
static void lr_nat_external_ips_destroy(struct lr_nat_record *);
static bool get_force_snat_ip(struct lr_nat_record *, const char *key_type,
                              struct lport_addresses *);
static struct lr_nat_input lr_nat_get_input_data(struct engine_node *);
static bool is_lr_nats_changed(const struct nbrec_logical_router *);
static bool is_lr_nats_seqno_changed(const struct nbrec_logical_router *nbr);


const struct lr_nat_record *
lr_nat_table_find_by_index(const struct lr_nat_table *table,
                           size_t od_index)
{
    return lr_nat_table_find_by_index_(table, od_index);
}

/* 'lr_nat' engine node manages the NB logical router NAT data.
 */
void *
en_lr_nat_init(struct engine_node *node OVS_UNUSED,
               struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_lr_nat_data *data = xzalloc(sizeof *data);
    lr_nat_table_init(&data->lr_nats);
    hmapx_init(&data->tracked_data.crupdated);
    hmapx_init(&data->tracked_data.deleted);
    return data;
}

void
en_lr_nat_cleanup(void *data_)
{
    struct ed_type_lr_nat_data *data = (struct ed_type_lr_nat_data *) data_;
    lr_nat_table_destroy(&data->lr_nats);
    hmapx_destroy(&data->tracked_data.crupdated);
    hmapx_destroy(&data->tracked_data.deleted);
}

void
en_lr_nat_clear_tracked_data(void *data_)
{
    struct ed_type_lr_nat_data *data = (struct ed_type_lr_nat_data *) data_;

    struct hmapx_node *hmapx_node;
    HMAPX_FOR_EACH_SAFE (hmapx_node, &data->tracked_data.deleted) {
        lr_nat_record_destroy(hmapx_node->data);
        hmapx_delete(&data->tracked_data.deleted, hmapx_node);
    }

    hmapx_clear(&data->tracked_data.crupdated);
    data->tracked = false;
}

void
en_lr_nat_run(struct engine_node *node, void *data_)
{
    struct lr_nat_input input_data = lr_nat_get_input_data(node);
    struct ed_type_lr_nat_data *data = data_;

    stopwatch_start(LR_NAT_RUN_STOPWATCH_NAME, time_msec());
    data->tracked = false;
    lr_nat_table_clear(&data->lr_nats);
    lr_nat_table_build(&data->lr_nats, input_data.lr_datapaths);

    stopwatch_stop(LR_NAT_RUN_STOPWATCH_NAME, time_msec());
    engine_set_node_state(node, EN_UPDATED);
}

/* Handler functions. */
bool
lr_nat_northd_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    struct northd_data *northd_data = engine_get_input_data("northd", node);
    if (!northd_data->change_tracked) {
        return false;
    }

    return true;
}

bool
lr_nat_logical_router_handler(struct engine_node *node, void *data_)
{
    struct lr_nat_input input_data = lr_nat_get_input_data(node);
    struct ed_type_lr_nat_data *data = data_;
    const struct nbrec_logical_router *nbr;

    NBREC_LOGICAL_ROUTER_TABLE_FOR_EACH_TRACKED (
            nbr, input_data.nbrec_logical_router_table) {
        if (!is_lr_nats_changed(nbr)) {
            continue;
        }

        struct lr_nat_record *lrnat_rec = lr_nat_table_find_(&data->lr_nats,
                                                             nbr);

        if (nbrec_logical_router_is_deleted(nbr)) {
            if (lrnat_rec) {
                /* Remove the record from the entries. */
                hmap_remove(&data->lr_nats.entries, &lrnat_rec->key_node);

                /* Add the lrnet rec to the tracking data. */
                hmapx_add(&data->tracked_data.deleted, lrnat_rec);
            }
        } else {
            if (!lrnat_rec) {
                const struct ovn_datapath *od;
                od = ovn_datapath_find(&input_data.lr_datapaths->datapaths,
                                       &nbr->header_.uuid);
                ovs_assert(od);
                lrnat_rec = lr_nat_record_create(&data->lr_nats, od);
            } else {
                lr_nat_record_reinit(lrnat_rec);
            }

            /* Add the lrnet rec to the tracking data. */
            hmapx_add(&data->tracked_data.crupdated, lrnat_rec);
        }
    }

    if (!hmapx_is_empty(&data->tracked_data.deleted)
            || !hmapx_is_empty(&data->tracked_data.crupdated)) {
        data->tracked = true;
        engine_set_node_state(node, EN_UPDATED);
    }
    return true;
}

/* static functions. */
static void
lr_nat_table_init(struct lr_nat_table *table)
{
    *table = (struct lr_nat_table) {
        .entries = HMAP_INITIALIZER(&table->entries),
    };
}

static void
lr_nat_table_clear(struct lr_nat_table *table)
{
    struct lr_nat_record *lrnat_rec;
    HMAP_FOR_EACH_POP (lrnat_rec, key_node, &table->entries) {
        lr_nat_record_destroy(lrnat_rec);
    }

    free(table->array);
    table->array = NULL;
}

static void
lr_nat_table_build(struct lr_nat_table *table,
                   const struct ovn_datapaths *lr_datapaths)
{
    table->array = xrealloc(table->array,
                            ods_size(lr_datapaths) * sizeof *table->array);

    const struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, &lr_datapaths->datapaths) {
        lr_nat_record_create(table, od);
    }
}

static void
lr_nat_table_destroy(struct lr_nat_table *table)
{
    lr_nat_table_clear(table);
    hmap_destroy(&table->entries);
}

struct lr_nat_record *
lr_nat_table_find_(const struct lr_nat_table *table,
                  const struct nbrec_logical_router *nbr)
{
    struct lr_nat_record *lrnat_rec;

    HMAP_FOR_EACH_WITH_HASH (lrnat_rec, key_node,
                             uuid_hash(&nbr->header_.uuid), &table->entries) {
        if (nbr == lrnat_rec->od->nbr) {
            return lrnat_rec;
        }
    }
    return NULL;
}


struct lr_nat_record *
lr_nat_table_find_by_index_(const struct lr_nat_table *table,
                            size_t od_index)
{
    ovs_assert(od_index <= hmap_count(&table->entries));

    return table->array[od_index];
}

static struct lr_nat_record *
lr_nat_record_create(struct lr_nat_table *table,
                     const struct ovn_datapath *od)
{
    ovs_assert(od->nbr);

    struct lr_nat_record *lrnat_rec = xzalloc(sizeof *lrnat_rec);
    lrnat_rec->od = od;
    lr_nat_record_init(lrnat_rec);

    hmap_insert(&table->entries, &lrnat_rec->key_node,
                uuid_hash(&od->nbr->header_.uuid));
    table->array[od->index] = lrnat_rec;
    return lrnat_rec;
}

static void
lr_nat_record_init(struct lr_nat_record *lrnat_rec)
{
    lr_nat_entries_init(lrnat_rec);
    lr_nat_external_ips_init(lrnat_rec);
}

static void
lr_nat_record_reinit(struct lr_nat_record *lrnat_rec)
{
    lr_nat_entries_destroy(lrnat_rec);
    lr_nat_external_ips_destroy(lrnat_rec);
    lr_nat_record_init(lrnat_rec);
}

static void
lr_nat_record_destroy(struct lr_nat_record *lrnat_rec)
{
    lr_nat_entries_destroy(lrnat_rec);
    lr_nat_external_ips_destroy(lrnat_rec);
    free(lrnat_rec);
}

static void
lr_nat_external_ips_init(struct lr_nat_record *lrnat_rec)
{
    sset_init(&lrnat_rec->external_ips);
    for (size_t i = 0; i < lrnat_rec->od->nbr->n_nat; i++) {
        sset_add(&lrnat_rec->external_ips,
                 lrnat_rec->od->nbr->nat[i]->external_ip);
    }
}

static void
lr_nat_external_ips_destroy(struct lr_nat_record *lrnat_rec)
{
    sset_destroy(&lrnat_rec->external_ips);
}

static void
snat_ip_add(struct lr_nat_record *lrnat_rec, const char *ip,
            struct ovn_nat *nat_entry)
{
    struct ovn_snat_ip *snat_ip = shash_find_data(&lrnat_rec->snat_ips, ip);

    if (!snat_ip) {
        snat_ip = xzalloc(sizeof *snat_ip);
        ovs_list_init(&snat_ip->snat_entries);
        shash_add(&lrnat_rec->snat_ips, ip, snat_ip);
    }

    if (nat_entry) {
        ovs_list_push_back(&snat_ip->snat_entries,
                           &nat_entry->ext_addr_list_node);
    }
}

static void
lr_nat_entries_init(struct lr_nat_record *lrnat_rec)
{
    shash_init(&lrnat_rec->snat_ips);
    sset_init(&lrnat_rec->external_macs);
    lrnat_rec->has_distributed_nat = false;

    if (get_force_snat_ip(lrnat_rec, "dnat",
                          &lrnat_rec->dnat_force_snat_addrs)) {
        if (lrnat_rec->dnat_force_snat_addrs.n_ipv4_addrs) {
            snat_ip_add(lrnat_rec,
                        lrnat_rec->dnat_force_snat_addrs.ipv4_addrs[0].addr_s,
                        NULL);
        }
        if (lrnat_rec->dnat_force_snat_addrs.n_ipv6_addrs) {
            snat_ip_add(lrnat_rec,
                        lrnat_rec->dnat_force_snat_addrs.ipv6_addrs[0].addr_s,
                        NULL);
        }
    }

    /* Check if 'lb_force_snat_ip' is configured with 'router_ip'. */
    const char *lb_force_snat =
        smap_get(&lrnat_rec->od->nbr->options, "lb_force_snat_ip");
    if (lb_force_snat && !strcmp(lb_force_snat, "router_ip")
            && smap_get(&lrnat_rec->od->nbr->options, "chassis")) {

        /* Set it to true only if its gateway router and
         * options:lb_force_snat_ip=router_ip. */
        lrnat_rec->lb_force_snat_router_ip = true;
    } else {
        lrnat_rec->lb_force_snat_router_ip = false;

        /* Check if 'lb_force_snat_ip' is configured with a set of
         * IP address(es). */
        if (get_force_snat_ip(lrnat_rec, "lb",
                              &lrnat_rec->lb_force_snat_addrs)) {
            if (lrnat_rec->lb_force_snat_addrs.n_ipv4_addrs) {
                snat_ip_add(lrnat_rec,
                        lrnat_rec->lb_force_snat_addrs.ipv4_addrs[0].addr_s,
                        NULL);
            }
            if (lrnat_rec->lb_force_snat_addrs.n_ipv6_addrs) {
                snat_ip_add(lrnat_rec,
                        lrnat_rec->lb_force_snat_addrs.ipv6_addrs[0].addr_s,
                        NULL);
            }
        }
    }

    if (!lrnat_rec->od->nbr->n_nat) {
        return;
    }

    lrnat_rec->nat_entries =
        xmalloc(lrnat_rec->od->nbr->n_nat * sizeof *lrnat_rec->nat_entries);

    for (size_t i = 0; i < lrnat_rec->od->nbr->n_nat; i++) {
        const struct nbrec_nat *nat = lrnat_rec->od->nbr->nat[i];
        struct ovn_nat *nat_entry = &lrnat_rec->nat_entries[i];

        nat_entry->nb = nat;
        if (!extract_ip_addresses(nat->external_ip,
                                  &nat_entry->ext_addrs) ||
                !nat_entry_is_valid(nat_entry)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);

            VLOG_WARN_RL(&rl,
                         "Bad ip address %s in nat configuration "
                         "for router %s", nat->external_ip,
                         lrnat_rec->od->nbr->name);
            continue;
        }

        /* If this is a SNAT rule add the IP to the set of unique SNAT IPs. */
        if (!strcmp(nat->type, "snat")) {
            if (!nat_entry_is_v6(nat_entry)) {
                snat_ip_add(lrnat_rec,
                            nat_entry->ext_addrs.ipv4_addrs[0].addr_s,
                            nat_entry);
            } else {
                snat_ip_add(lrnat_rec,
                            nat_entry->ext_addrs.ipv6_addrs[0].addr_s,
                            nat_entry);
            }
        } else {
            if (!strcmp(nat->type, "dnat_and_snat")
                    && nat->logical_port && nat->external_mac) {
                lrnat_rec->has_distributed_nat = true;
            }

            if (nat->external_mac) {
                sset_add(&lrnat_rec->external_macs, nat->external_mac);
            }
        }
    }
    lrnat_rec->n_nat_entries = lrnat_rec->od->nbr->n_nat;
}

static bool
get_force_snat_ip(struct lr_nat_record *lrnat_rec, const char *key_type,
                  struct lport_addresses *laddrs)
{
    char *key = xasprintf("%s_force_snat_ip", key_type);
    const char *addresses = smap_get(&lrnat_rec->od->nbr->options, key);
    free(key);

    if (!addresses) {
        return false;
    }

    if (!extract_ip_address(addresses, laddrs)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "bad ip %s in options of router "UUID_FMT"",
                     addresses, UUID_ARGS(&lrnat_rec->od->nbr->header_.uuid));
        return false;
    }

    return true;
}

static void
lr_nat_entries_destroy(struct lr_nat_record *lrnat_rec)
{
    shash_destroy_free_data(&lrnat_rec->snat_ips);
    destroy_lport_addresses(&lrnat_rec->dnat_force_snat_addrs);
    destroy_lport_addresses(&lrnat_rec->lb_force_snat_addrs);

    for (size_t i = 0; i < lrnat_rec->n_nat_entries; i++) {
        destroy_lport_addresses(&lrnat_rec->nat_entries[i].ext_addrs);
    }

    free(lrnat_rec->nat_entries);
    lrnat_rec->nat_entries = NULL;
    lrnat_rec->n_nat_entries = 0;
    sset_destroy(&lrnat_rec->external_macs);
}

static struct lr_nat_input
lr_nat_get_input_data(struct engine_node *node)
{
    struct northd_data *northd_data = engine_get_input_data("northd", node);
    return (struct lr_nat_input) {
        .nbrec_logical_router_table =
            EN_OVSDB_GET(engine_get_input("NB_logical_router", node)),
        .lr_datapaths = &northd_data->lr_datapaths,
    };
}

static bool
is_lr_nats_seqno_changed(const struct nbrec_logical_router *nbr)
{
    for (size_t i = 0; i < nbr->n_nat; i++) {
        if (nbrec_nat_row_get_seqno(nbr->nat[i],
                                    OVSDB_IDL_CHANGE_MODIFY) > 0) {
            return true;
        }
    }

    return false;
}

static bool
is_lr_nats_changed(const struct nbrec_logical_router *nbr) {
    return (nbrec_logical_router_is_new(nbr)
            || nbrec_logical_router_is_deleted(nbr)
            || nbrec_logical_router_is_updated(nbr,
                                               NBREC_LOGICAL_ROUTER_COL_NAT)
            || nbrec_logical_router_is_updated(
                nbr, NBREC_LOGICAL_ROUTER_COL_OPTIONS)
            || is_lr_nats_seqno_changed(nbr));
}
