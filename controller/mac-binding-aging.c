
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

#include "byte-order.h"
#include "dirs.h"
#include "mac-binding-aging.h"
#include "openvswitch/hmap.h"
#include "openvswitch/ofp-flow.h"
#include "openvswitch/vconn.h"
#include "openvswitch/vlog.h"
#include "ovn-sb-idl.h"
#include "timeval.h"
#include "util.h"
#include "uuid.h"

VLOG_DEFINE_THIS_MODULE(mac_binding_aging);

/* Contains "struct mac_binding_aging"s. */
static struct hmap mb_aging_hmap;

struct mac_binding_aging {
    struct hmap_node hmap_node;

    /* Idle time from last statistic check in ms. */
    long long idle_age;
    /* Time when the statistics were updated in ms. */
    long long last_check;

    const struct sbrec_mac_binding *mb;
};

void
mac_binding_aging_init(void)
{
    hmap_init(&mb_aging_hmap);
}

void
mac_binding_aging_destroy(void)
{
    struct mac_binding_aging *mb_aging;

    HMAP_FOR_EACH_POP(mb_aging, hmap_node, &mb_aging_hmap) {
        free(mb_aging);
    }
    hmap_destroy(&mb_aging_hmap);
}

static struct mac_binding_aging *
mac_binding_aging_find(const struct uuid *uuid)
{
    struct mac_binding_aging *mb_aging;

    size_t hash = uuid_hash(uuid);

    HMAP_FOR_EACH_WITH_HASH(mb_aging, hmap_node, hash, &mb_aging_hmap) {
        if (uuid_equals(&mb_aging->mb->header_.uuid, uuid)) {
            return mb_aging;
        }
    }

    return NULL;
}

static void
mac_binding_aging_add(const struct sbrec_mac_binding *mb)
{
    struct mac_binding_aging *mb_aging;
    size_t hash = uuid_hash(&mb->header_.uuid);

    mb_aging = xmalloc(sizeof *mb_aging);
    mb_aging->mb = mb;
    mb_aging->last_check = time_msec();
    mb_aging->idle_age = 0;
    hmap_insert(&mb_aging_hmap, &mb_aging->hmap_node, hash);
}

static bool
mac_binding_aging_needs_update(struct mac_binding_aging *mb_aging,
                               long long now, unsigned long long threshold)
{
    return (now - mb_aging->last_check + mb_aging->idle_age) >= threshold;
}

static void
mac_binding_aging_update_statistics(struct vconn *vconn,
                                    struct mac_binding_aging *mb_aging,
                                    long long now)
{
    uint32_t cookie = mb_aging->mb->header_.uuid.parts[0];

    struct ofputil_flow_stats_request fsr = {
        .cookie = htonll(cookie),
        .cookie_mask = OVS_BE64_MAX,
        .out_port = OFPP_ANY,
        .out_group = OFPG_ANY,
        .table_id = OFPTT_ALL,
    };

    struct ofputil_flow_stats *fses;
    size_t n_fses;

    int error =
        vconn_dump_flows(vconn, &fsr, OFPUTIL_P_OF15_OXM, &fses, &n_fses);
    if (error) {
        VLOG_WARN("%s: error obtaining flow stats (%s)",
                  vconn_get_name(vconn), ovs_strerror(error));
        goto free;
    }

    if (n_fses != 2) {
        VLOG_DBG("Unexpected statistics count (%" PRIuSIZE "), "
                 "the flows might not be installed yet or they "
                 "are already removed.", n_fses);
        goto free;
    }

    mb_aging->idle_age = MIN(fses[0].idle_age, fses[1].idle_age) * 1000;
    mb_aging->last_check = now;

free:
    for (size_t i = 0; i < n_fses; i++) {
        free(CONST_CAST(struct ofpact *, fses[i].ofpacts));
    }
    free(fses);
}

static void
mac_binding_aging_update_monitored(struct ovsdb_idl_index *mb_by_chassis_index,
                                   const struct sbrec_mac_binding_table
                                   *mac_binding_table,
                                   const struct sbrec_chassis *chassis)
{
    const struct sbrec_mac_binding *mb;
    SBREC_MAC_BINDING_TABLE_FOR_EACH_TRACKED(mb, mac_binding_table) {
        if (sbrec_mac_binding_is_deleted(mb) ||
            (sbrec_mac_binding_is_updated(mb, SBREC_MAC_BINDING_COL_CHASSIS) &&
             mb->chassis != chassis)) {
            struct mac_binding_aging *mb_aging =
                mac_binding_aging_find(&mb->header_.uuid);
            if (mb_aging) {
                hmap_remove(&mb_aging_hmap, &mb_aging->hmap_node);
                free(mb_aging);
            }
        }
    }

    struct sbrec_mac_binding *mb_index_row =
        sbrec_mac_binding_index_init_row(mb_by_chassis_index);
    sbrec_mac_binding_index_set_chassis(mb_index_row, chassis);
    SBREC_MAC_BINDING_FOR_EACH_EQUAL(mb, mb_index_row, mb_by_chassis_index) {
        if (!mac_binding_aging_find(&mb->header_.uuid)) {
            mac_binding_aging_add(mb);
        }
    }
    sbrec_mac_binding_index_destroy_row(mb_index_row);
}

static struct vconn *
create_ovs_connection(const char *br_int_name)
{
    struct vconn *vconn;
    char *target = xasprintf("unix:%s/%s.mgmt", ovs_rundir(), br_int_name);
    int retval = vconn_open_block(target, 1 << OFP15_VERSION, 0, -1, &vconn);

    if (retval) {
        VLOG_WARN("%s: connection failed (%s)", target, ovs_strerror(retval));
    }
    free(target);

    return vconn;
}

void
mac_binding_aging_run(struct ovsdb_idl_txn *ovnsb_idl_txn,
                      const char *br_int_name,
                      const struct sbrec_chassis *chassis,
                      const struct sbrec_mac_binding_table *mac_binding_table,
                      struct ovsdb_idl_index *mb_by_chassis_index,
                      unsigned long long threshold)
{
    if (!ovnsb_idl_txn) {
        return;
    }

    struct vconn *vconn = create_ovs_connection(br_int_name);

    if (!vconn) {
        return;
    }

    mac_binding_aging_update_monitored(mb_by_chassis_index, mac_binding_table,
                                       chassis);

    long long now = time_msec();

    struct mac_binding_aging *mb_aging;

    HMAP_FOR_EACH_SAFE(mb_aging, hmap_node, &mb_aging_hmap) {
        if (mac_binding_aging_needs_update(mb_aging, now, threshold)) {
            mac_binding_aging_update_statistics(vconn, mb_aging, now);
        }
        if (mb_aging->idle_age >= threshold) {
            VLOG_DBG("MAC binding exceeded threshold uuid=" UUID_FMT ", "
                     "idle_age=%llu ms",
                     UUID_ARGS(&mb_aging->mb->header_.uuid),
                     mb_aging->idle_age);
            sbrec_mac_binding_delete(mb_aging->mb);
            hmap_remove(&mb_aging_hmap, &mb_aging->hmap_node);
            free(mb_aging);
        }
    }

    vconn_close(vconn);
}
