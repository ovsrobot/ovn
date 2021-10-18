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
#ifndef NORTHD_H
#define NORTHD_H 1

#include "ovsdb-idl.h"

#include "openvswitch/hmap.h"

struct northd_idl_context {
    struct ovsdb_idl *ovnnb_idl;
    struct ovsdb_idl *ovnsb_idl;
    struct ovsdb_idl_loop *ovnnb_idl_loop;
    struct ovsdb_idl_loop *ovnsb_idl_loop;
    struct ovsdb_idl_txn *ovnnb_txn;
    struct ovsdb_idl_txn *ovnsb_txn;
};

struct northd_data {
    struct hmap datapaths;
    struct hmap ports;
    struct hmap port_groups;
    struct hmap mcast_groups;
    struct hmap igmp_groups;
    struct shash meter_groups;
    struct hmap lbs;
    struct hmap bfd_connections;
    struct ovs_list lr_list;
    bool ovn_internal_version_changed;
    bool use_parallel_build;

    struct ovsdb_idl_index *sbrec_chassis_by_name;
    struct ovsdb_idl_index *sbrec_ha_chassis_grp_by_name;
    struct ovsdb_idl_index *sbrec_mcast_group_by_name_dp;
    struct ovsdb_idl_index *sbrec_ip_mcast_by_dp;
};

void northd_run(struct northd_idl_context *ctx, struct northd_data *data);
void northd_destroy(struct northd_data *data);
void northd_init(struct northd_data *data);
void northd_indices_create(struct northd_data *data,
                           struct ovsdb_idl *ovnsb_idl);
void build_lflows(struct northd_idl_context *ctx, struct northd_data *data);

#endif /* NORTHD_H */
