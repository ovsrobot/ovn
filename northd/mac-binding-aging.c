/* Copyright (c) 2022, Red Hat, Inc.
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

#include "lib/inc-proc-eng.h"
#include "lib/ovn-nb-idl.h"
#include "lib/ovn-sb-idl.h"
#include "lib/timeval.h"
#include "northd/mac-binding-aging.h"
#include "northd/northd.h"
#include "openvswitch/hmap.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/util.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(mac_binding_aging);

#define MAC_BINDING_BULK_REMOVAL_DELAY_MSEC 10

struct mac_binding_waker {
    bool should_schedule;
    long long next_wake_msec;
};

static struct mac_binding_waker waker;

static void
mac_binding_aging_run_for_datapath(const struct sbrec_datapath_binding *dp,
                                   const struct nbrec_logical_router *nbr,
                                   struct ovsdb_idl_index *mb_by_datapath,
                                   int64_t now, int64_t *wake_delay,
                                   uint32_t removal_limit, uint32_t *removed_n)
{
    uint64_t threshold = smap_get_uint(&nbr->options,
                                         "mac_binding_age_threshold",
                                         0) * 1000;
    if (!threshold) {
        return;
    }

    struct sbrec_mac_binding *mb_index_row =
        sbrec_mac_binding_index_init_row(mb_by_datapath);
    sbrec_mac_binding_index_set_datapath(mb_index_row, dp);

    const struct sbrec_mac_binding *mb;
    SBREC_MAC_BINDING_FOR_EACH_EQUAL (mb, mb_index_row, mb_by_datapath) {
        int64_t elapsed = now - mb->timestamp;

        if (elapsed < 0) {
            continue;
        } else if (elapsed >= threshold) {
            sbrec_mac_binding_delete(mb);
            (*removed_n)++;
            if (removal_limit && *removed_n == removal_limit) {
                break;
            }
        } else {
            *wake_delay = MIN(*wake_delay, threshold - elapsed);
        }
    }
    sbrec_mac_binding_index_destroy_row(mb_index_row);
}

static uint32_t
get_removal_limit(struct engine_node *node)
{
    const struct nbrec_nb_global_table *nb_global_table =
        EN_OVSDB_GET(engine_get_input("NB_nb_global", node));
    const struct nbrec_nb_global *nb =
        nbrec_nb_global_table_first(nb_global_table);
    if (!nb) {
       return 0;
    }

    return smap_get_uint(&nb->options, "mac_binding_removal_limit", 0);
}

void
en_mac_binding_aging_run(struct engine_node *node, void *data OVS_UNUSED)
{
    const struct engine_context *eng_ctx = engine_get_context();

    if (!eng_ctx->ovnsb_idl_txn) {
        return;
    }

    int64_t next_expire_msec = INT64_MAX;
    int64_t now = time_wall_msec();
    uint32_t removal_limit = get_removal_limit(node);
    uint32_t removed_n = 0;
    struct northd_data *northd_data = engine_get_input_data("northd", node);
    struct ovsdb_idl_index *sbrec_mac_binding_by_datapath =
        engine_ovsdb_node_get_index(engine_get_input("SB_mac_binding", node),
                                    "sbrec_mac_binding_by_datapath");

    struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, &northd_data->datapaths) {
        if (od->sb && od->nbr) {
            mac_binding_aging_run_for_datapath(od->sb, od->nbr,
                                               sbrec_mac_binding_by_datapath,
                                               now, &next_expire_msec,
                                               removal_limit, &removed_n);
            if (removal_limit && removed_n == removal_limit) {
                /* Schedule the next run after specified delay. */
                next_expire_msec = MAC_BINDING_BULK_REMOVAL_DELAY_MSEC;
                break;
            }
        }
    }

    if (next_expire_msec < INT64_MAX) {
        waker.should_schedule = true;
        waker.next_wake_msec = time_msec() + next_expire_msec;
        /* Run the engine right after so the waker can reflect on the new wake
         * time. */
        poll_immediate_wake();
    } else {
        waker.should_schedule = false;
    }

    /* This node is part of lflow, but lflow does not depend on it. Setting
     * state as unchanged does not trigger lflow node when it is not needed. */
    engine_set_node_state(node, EN_UNCHANGED);
}

void *
en_mac_binding_aging_init(struct engine_node *node OVS_UNUSED,
                          struct engine_arg *arg OVS_UNUSED)
{
    return NULL;
}

void
en_mac_binding_aging_cleanup(void *data OVS_UNUSED)
{
}

void
en_mac_binding_aging_waker_run(struct engine_node *node, void *data OVS_UNUSED)
{
    if (!waker.should_schedule) {
        return;
    }

    if (time_msec() >= waker.next_wake_msec) {
        waker.should_schedule = false;
        engine_set_node_state(node, EN_UPDATED);
        return;
    }

    engine_set_node_state(node, EN_UNCHANGED);
    poll_timer_wait_until(waker.next_wake_msec);
}

void *
en_mac_binding_aging_waker_init(struct engine_node *node OVS_UNUSED,
                                struct engine_arg *arg OVS_UNUSED)
{
    waker.should_schedule = false;
    waker.next_wake_msec = 0;
    return NULL;
}

void
en_mac_binding_aging_waker_cleanup(void *data OVS_UNUSED)
{
}
