/* Copyright (c) 2015, 2016 Nicira, Inc.
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

#include "lib/sset.h"
#include "lport.h"
#include "ha-chassis.h"
#include "hash.h"
#include "openvswitch/vlog.h"
#include "lib/ovn-sb-idl.h"
VLOG_DEFINE_THIS_MODULE(lport);

static const struct sbrec_port_binding *get_peer_lport(
    const struct sbrec_port_binding *pb,
    struct ovsdb_idl_index *sbrec_port_binding_by_name);

const struct sbrec_port_binding *
lport_lookup_by_name(struct ovsdb_idl_index *sbrec_port_binding_by_name,
                     const char *name)
{
    struct sbrec_port_binding *pb = sbrec_port_binding_index_init_row(
        sbrec_port_binding_by_name);
    sbrec_port_binding_index_set_logical_port(pb, name);

    const struct sbrec_port_binding *retval = sbrec_port_binding_index_find(
        sbrec_port_binding_by_name, pb);

    sbrec_port_binding_index_destroy_row(pb);

    return retval;
}

const struct sbrec_port_binding *
lport_lookup_by_key(struct ovsdb_idl_index *sbrec_datapath_binding_by_key,
                    struct ovsdb_idl_index *sbrec_port_binding_by_key,
                    uint64_t dp_key, uint64_t port_key)
{
    /* Lookup datapath corresponding to dp_key. */
    const struct sbrec_datapath_binding *db = datapath_lookup_by_key(
        sbrec_datapath_binding_by_key, dp_key);
    if (!db) {
        return NULL;
    }

    /* Build key for an indexed lookup. */
    struct sbrec_port_binding *pb = sbrec_port_binding_index_init_row(
        sbrec_port_binding_by_key);
    sbrec_port_binding_index_set_datapath(pb, db);
    sbrec_port_binding_index_set_tunnel_key(pb, port_key);

    const struct sbrec_port_binding *retval = sbrec_port_binding_index_find(
        sbrec_port_binding_by_key, pb);

    sbrec_port_binding_index_destroy_row(pb);

    return retval;
}

bool
lport_is_chassis_resident(struct ovsdb_idl_index *sbrec_port_binding_by_name,
                          const struct sbrec_chassis *chassis,
                          const struct sset *active_tunnels,
                          const char *port_name)
{
    const struct sbrec_port_binding *pb
        = lport_lookup_by_name(sbrec_port_binding_by_name, port_name);
    if (!pb || !pb->chassis) {
        return false;
    }
    if (strcmp(pb->type, "chassisredirect")) {
        return pb->chassis == chassis;
    } else {
        return ha_chassis_group_is_active(pb->ha_chassis_group,
                                          active_tunnels, chassis);
    }
}

const struct sbrec_port_binding *
lport_get_peer(const struct sbrec_port_binding *pb,
               struct ovsdb_idl_index *sbrec_port_binding_by_name)
{
    if (strcmp(pb->type, "patch")) {
        return NULL;
    }
    return get_peer_lport(pb, sbrec_port_binding_by_name);
}

const struct sbrec_port_binding *
lport_get_l3gw_peer(const struct sbrec_port_binding *pb,
                    struct ovsdb_idl_index *sbrec_port_binding_by_name)
{
    if (strcmp(pb->type, "l3gateway")) {
        return NULL;
    }
    return get_peer_lport(pb, sbrec_port_binding_by_name);
}

bool
lport_can_bind_on_this_chassis(const struct sbrec_chassis *chassis_rec,
                               const struct sbrec_port_binding *pb)
{
    /* We need to check for presence of the requested-chassis option in
     * addition to checking the pb->requested_chassis column because this
     * column will be set to NULL whenever the option points to a non-existent
     * chassis.  As the controller routinely clears its own chassis record this
     * might occur more often than one might think. */
    const char *requested_chassis_option = smap_get(&pb->options,
                                                    "requested-chassis");
    bool requested = false;
    if (requested_chassis_option && requested_chassis_option[0]
        && !pb->requested_chassis) {
        /* The requested-chassis option is set, but the requested_chassis
         * column is not filled.  This means that the chassis the option
         * points to is currently not running, or is in the process of starting
         * up.  In this case we must fall back to comparing the strings to
         * avoid release/claim thrashing. */
        requested = !strcmp(requested_chassis_option, chassis_rec->name)
               || !strcmp(requested_chassis_option, chassis_rec->hostname);
    } else {
        requested = !requested_chassis_option || !requested_chassis_option[0]
               || chassis_rec == pb->requested_chassis;
    }

    /* Alternatively, the upcoming migration destination chassis may also bind
     * the port. */
    if (!requested) {
        const char *migration_destination_option = smap_get(
            &pb->options, "migration-destination");
        if (migration_destination_option && migration_destination_option[0]) {
            requested = (
                !strcmp(migration_destination_option, chassis_rec->name) ||
                !strcmp(migration_destination_option, chassis_rec->hostname)
            );
        } else {
            requested = chassis_rec == pb->migration_destination;
        }
    }
    return requested;
}

const struct sbrec_datapath_binding *
datapath_lookup_by_key(struct ovsdb_idl_index *sbrec_datapath_binding_by_key,
                       uint64_t dp_key)
{
    struct sbrec_datapath_binding *db = sbrec_datapath_binding_index_init_row(
        sbrec_datapath_binding_by_key);
    sbrec_datapath_binding_index_set_tunnel_key(db, dp_key);

    const struct sbrec_datapath_binding *retval
        = sbrec_datapath_binding_index_find(sbrec_datapath_binding_by_key,
                                            db);

    sbrec_datapath_binding_index_destroy_row(db);

    return retval;
}

const struct sbrec_multicast_group *
mcgroup_lookup_by_dp_name(
    struct ovsdb_idl_index *sbrec_multicast_group_by_name_datapath,
    const struct sbrec_datapath_binding *db, const char *name)
{
    /* Build key for an indexed lookup. */
    struct sbrec_multicast_group *mc = sbrec_multicast_group_index_init_row(
        sbrec_multicast_group_by_name_datapath);
    sbrec_multicast_group_index_set_name(mc, name);
    sbrec_multicast_group_index_set_datapath(mc, db);

    const struct sbrec_multicast_group *retval
        = sbrec_multicast_group_index_find(
            sbrec_multicast_group_by_name_datapath, mc);

    sbrec_multicast_group_index_destroy_row(mc);

    return retval;
}

static const struct sbrec_port_binding *
get_peer_lport(const struct sbrec_port_binding *pb,
               struct ovsdb_idl_index *sbrec_port_binding_by_name)
{
    const char *peer_name = smap_get(&pb->options, "peer");

    if (!peer_name) {
        return NULL;
    }

    const struct sbrec_port_binding *peer;
    peer = lport_lookup_by_name(sbrec_port_binding_by_name,
                                peer_name);
    return (peer && peer->datapath) ? peer : NULL;
}
