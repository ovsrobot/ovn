    /*
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

#include "coverage.h"
#include "en-global-config.h"
#include "en-northd.h"
#include "en-lb-data.h"
#include "lib/inc-proc-eng.h"
#include "lib/ovn-nb-idl.h"
#include "openvswitch/list.h" /* TODO This is needed for ovn-parallel-hmap.h.
                               * lib/ovn-parallel-hmap.h should be updated
                               * to include this dependency itself */
#include "lib/ovn-parallel-hmap.h"
#include "stopwatch.h"
#include "lib/stopwatch-names.h"
#include "northd.h"
#include "lib/util.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(en_northd);
COVERAGE_DEFINE(northd_run);

static void
northd_get_input_data(struct engine_node *node,
                      struct northd_input *input_data)
{
    input_data->sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_chassis", node),
            "sbrec_chassis_by_name");
    input_data->sbrec_chassis_by_hostname =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_chassis", node),
            "sbrec_chassis_by_hostname");
    input_data->sbrec_ha_chassis_grp_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_ha_chassis_group", node),
            "sbrec_ha_chassis_grp_by_name");
    input_data->sbrec_ip_mcast_by_dp =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_ip_multicast", node),
            "sbrec_ip_mcast_by_dp");
    input_data->sbrec_static_mac_binding_by_lport_ip =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_static_mac_binding", node),
            "sbrec_static_mac_binding_by_lport_ip");
    input_data->sbrec_fdb_by_dp_and_port =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_fdb", node),
            "sbrec_fdb_by_dp_and_port");

    input_data->nbrec_logical_switch_table =
        EN_OVSDB_GET(engine_get_input("NB_logical_switch", node));
    input_data->nbrec_logical_router_table =
        EN_OVSDB_GET(engine_get_input("NB_logical_router", node));
    input_data->nbrec_static_mac_binding_table =
        EN_OVSDB_GET(engine_get_input("NB_static_mac_binding", node));
    input_data->nbrec_chassis_template_var_table =
        EN_OVSDB_GET(engine_get_input("NB_chassis_template_var", node));
    input_data->nbrec_mirror_table =
        EN_OVSDB_GET(engine_get_input("NB_mirror", node));

    input_data->sbrec_datapath_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_datapath_binding", node));
    input_data->sbrec_port_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_port_binding", node));
    input_data->sbrec_mac_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_mac_binding", node));
    input_data->sbrec_ha_chassis_group_table =
        EN_OVSDB_GET(engine_get_input("SB_ha_chassis_group", node));
    input_data->sbrec_chassis_table =
        EN_OVSDB_GET(engine_get_input("SB_chassis", node));
    input_data->sbrec_fdb_table =
        EN_OVSDB_GET(engine_get_input("SB_fdb", node));
    input_data->sbrec_service_monitor_table =
        EN_OVSDB_GET(engine_get_input("SB_service_monitor", node));
    input_data->sbrec_dns_table =
        EN_OVSDB_GET(engine_get_input("SB_dns", node));
    input_data->sbrec_ip_multicast_table =
        EN_OVSDB_GET(engine_get_input("SB_ip_multicast", node));
    input_data->sbrec_static_mac_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_static_mac_binding", node));
    input_data->sbrec_chassis_template_var_table =
        EN_OVSDB_GET(engine_get_input("SB_chassis_template_var", node));
    input_data->sbrec_mirror_table =
        EN_OVSDB_GET(engine_get_input("SB_mirror", node));

    struct ed_type_lb_data *lb_data =
        engine_get_input_data("lb_data", node);
    input_data->lbs = &lb_data->lbs;
    input_data->lbgrps = &lb_data->lbgrps;

    struct ed_type_global_config *global_config =
        engine_get_input_data("global_config", node);
    input_data->nb_options = &global_config->nb_options;
    input_data->sb_options = &global_config->sb_options;
    input_data->svc_monitor_mac = global_config->svc_monitor_mac;
    input_data->svc_monitor_mac_ea = global_config->svc_monitor_mac_ea;
    input_data->features = &global_config->features;
}

void
en_northd_run(struct engine_node *node, void *data)
{
    const struct engine_context *eng_ctx = engine_get_context();

    struct northd_input input_data;

    northd_destroy(data);
    northd_init(data);

    northd_get_input_data(node, &input_data);

    COVERAGE_INC(northd_run);
    stopwatch_start(OVNNB_DB_RUN_STOPWATCH_NAME, time_msec());
    ovnnb_db_run(&input_data, data, eng_ctx->ovnnb_idl_txn,
                 eng_ctx->ovnsb_idl_txn);
    stopwatch_stop(OVNNB_DB_RUN_STOPWATCH_NAME, time_msec());
    engine_set_node_state(node, EN_UPDATED);
}

bool
northd_nb_logical_switch_handler(struct engine_node *node,
                                 void *data)
{
    const struct engine_context *eng_ctx = engine_get_context();
    struct northd_data *nd = data;

    struct northd_input input_data;

    northd_get_input_data(node, &input_data);

    if (!northd_handle_ls_changes(eng_ctx->ovnsb_idl_txn, &input_data, nd)) {
        return false;
    }

    if (northd_has_tracked_data(&nd->trk_data)) {
        engine_set_node_state(node, EN_UPDATED);
    }

    return true;
}

bool
northd_sb_port_binding_handler(struct engine_node *node,
                               void *data)
{
    struct northd_data *nd = data;

    struct northd_input input_data;

    northd_get_input_data(node, &input_data);

    if (!northd_handle_sb_port_binding_changes(
        input_data.sbrec_port_binding_table, &nd->ls_ports, &nd->lr_ports)) {
        return false;
    }

    return true;
}

bool
northd_nb_logical_router_handler(struct engine_node *node,
                                 void *data)
{
    struct northd_data *nd = data;
    struct northd_input input_data;

    northd_get_input_data(node, &input_data);

    if (!northd_handle_lr_changes(&input_data, nd)) {
        return false;
    }

    if (northd_has_lr_nats_in_tracked_data(&nd->trk_data)) {
        engine_set_node_state(node, EN_UPDATED);
    }

    return true;
}

bool
northd_lb_data_handler(struct engine_node *node, void *data)
{
    struct ed_type_lb_data *lb_data = engine_get_input_data("lb_data", node);

    if (!lb_data->tracked) {
        return false;
    }

    struct northd_data *nd = data;
    if (!northd_handle_lb_data_changes(&lb_data->tracked_lb_data,
                                       &nd->ls_datapaths,
                                       &nd->lr_datapaths,
                                       &nd->lb_datapaths_map,
                                       &nd->lb_group_datapaths_map,
                                       &nd->trk_data)) {
        return false;
    }

    if (northd_has_lbs_in_tracked_data(&nd->trk_data)) {
        engine_set_node_state(node, EN_UPDATED);
    }

    return true;
}

bool
northd_global_config_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    struct ed_type_global_config *global_config =
        engine_get_input_data("global_config", node);

    if (!global_config->tracked
        || global_config->tracked_data.nb_options_changed) {
        return false;
    }

    return true;
}

void
*en_northd_init(struct engine_node *node OVS_UNUSED,
                struct engine_arg *arg OVS_UNUSED)
{
    struct northd_data *data = xzalloc(sizeof *data);

    northd_init(data);

    return data;
}

void
en_northd_cleanup(void *data)
{
    northd_destroy(data);
}

void
en_northd_clear_tracked_data(void *data_)
{
    struct northd_data *data = data_;
    destroy_northd_data_tracked_changes(data);
}

bool
sb_fdb_change_handler(struct engine_node *node, void *data)
{
    struct northd_data *nd = data;
    const struct sbrec_fdb_table *sbrec_fdb_table =
        EN_OVSDB_GET(engine_get_input("SB_fdb", node));

    /* check if changed rows are stale and delete them */
    const struct sbrec_fdb *fdb_e, *fdb_prev_del = NULL;
    SBREC_FDB_TABLE_FOR_EACH_TRACKED(fdb_e, sbrec_fdb_table) {
        if (sbrec_fdb_is_deleted(fdb_e)) {
            continue;
        }

        if (fdb_prev_del) {
            sbrec_fdb_delete(fdb_prev_del);
        }

        fdb_prev_del = fdb_e;
        struct ovn_datapath *od
            = ovn_datapath_find_by_key(&nd->ls_datapaths.datapaths,
                                          fdb_e->dp_key);
        if (od) {
            if (ovn_tnlid_present(&od->port_tnlids, fdb_e->port_key)) {
                fdb_prev_del = NULL;
            }
        }
    }

    if (fdb_prev_del) {
        sbrec_fdb_delete(fdb_prev_del);
    }

    engine_set_node_state(node, EN_UPDATED);
    return true;
}
