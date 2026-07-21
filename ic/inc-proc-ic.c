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

#include "lib/inc-proc-eng.h"
#include "lib/ovn-nb-idl.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-ic-nb-idl.h"
#include "lib/ovn-ic-sb-idl.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/vlog.h"
#include "inc-proc-ic.h"
#include "en-ic.h"
#include "en-az.h"
#include "en-dp-enum.h"
#include "en-gateway.h"
#include "en-ts.h"
#include "en-tr.h"
#include "en-port-binding.h"
#include "en-route.h"
#include "en-service-monitor.h"
#include "en-address-set.h"
#include "ovn-util.h"
#include "unixctl.h"
#include "util.h"

VLOG_DEFINE_THIS_MODULE(inc_proc_ic);

#define NB_NODES \
    NB_NODE(nb_global, "nb_global") \
    NB_NODE(logical_router_static_route, "logical_router_static_route") \
    NB_NODE(logical_router, "logical_router") \
    NB_NODE(logical_router_port, "logical_router_port") \
    NB_NODE(logical_switch, "logical_switch") \
    NB_NODE(logical_switch_port, "logical_switch_port") \
    NB_NODE(load_balancer, "load_balancer") \
    NB_NODE(load_balancer_group, "load_balancer_group") \
    NB_NODE(address_set, "address_set")

    enum nb_engine_node {
#define NB_NODE(NAME, NAME_STR) NB_##NAME,
    NB_NODES
#undef NB_NODE
    };

/* Define engine node functions for nodes that represent NB tables
 *
 * en_nb_<TABLE_NAME>_run()
 * en_nb_<TABLE_NAME>_init()
 * en_nb_<TABLE_NAME>_cleanup()
 */
#define NB_NODE(NAME, NAME_STR) ENGINE_FUNC_NB(NAME);
    NB_NODES
#undef NB_NODE

#define SB_NODES \
    SB_NODE(sb_global, "sb_global") \
    SB_NODE(chassis, "chassis") \
    SB_NODE(encap, "encap") \
    SB_NODE(datapath_binding, "datapath_binding") \
    SB_NODE(port_binding, "port_binding") \
    SB_NODE(service_monitor, "service_monitor") \
    SB_NODE(learned_route, "learned_route") \
    SB_NODE(address_set, "address_set")

    enum sb_engine_node {
#define SB_NODE(NAME, NAME_STR) SB_##NAME,
    SB_NODES
#undef SB_NODE
};

/* Define engine node functions for nodes that represent SB tables
 *
 * en_sb_<TABLE_NAME>_run()
 * en_sb_<TABLE_NAME>_init()
 * en_sb_<TABLE_NAME>_cleanup()
 */
#define SB_NODE(NAME, NAME_STR) ENGINE_FUNC_SB(NAME);
    SB_NODES
#undef SB_NODE

#define ICNB_NODES \
    ICNB_NODE(ic_nb_global, "ic_nb_global") \
    ICNB_NODE(transit_switch, "transit_switch") \
    ICNB_NODE(transit_router, "transit_router") \
    ICNB_NODE(transit_router_port, "transit_router_port")

    enum icnb_engine_node {
#define ICNB_NODE(NAME, NAME_STR) ICNB_##NAME,
    ICNB_NODES
#undef ICNB_NODE
    };

/* Define engine node functions for nodes that represent ICNB tables
 *
 * en_icnb_<TABLE_NAME>_run()
 * en_icnb_<TABLE_NAME>_init()
 * en_icnb_<TABLE_NAME>_cleanup()
 */
#define ICNB_NODE(NAME, NAME_STR) ENGINE_FUNC_ICNB(NAME);
    ICNB_NODES
#undef ICNB_NODE

/* Note: the ic_sb_global and availability_zone tables are intentionally not
 * modeled as engine input nodes.  ic_sb_global only carries IC-SB sequence
 * numbers, written by update_sequence_numbers() in the main loop (outside the
 * engine).  availability_zone is consumed by the en_az node, which reads it
 * directly every iteration; the subsystem nodes depend on en_az for the AZ
 * identity rather than on the (sequence-number-bumped) Availability_Zone
 * table. */
#define ICSB_NODES \
    ICSB_NODE(service_monitor, "service_monitor") \
    ICSB_NODE(route, "route") \
    ICSB_NODE(datapath_binding, "datapath_binding") \
    ICSB_NODE(encap, "encap") \
    ICSB_NODE(gateway, "gateway") \
    ICSB_NODE(port_binding, "port_binding") \
    ICSB_NODE(address_set, "address_set")

    enum icsb_engine_node {
#define ICSB_NODE(NAME, NAME_STR) ICSB_##NAME,
    ICSB_NODES
#undef ICSB_NODE
    };

/* Define engine node functions for nodes that represent ICSB tables
 *
 * en_icsb_<TABLE_NAME>_run()
 * en_icsb_<TABLE_NAME>_init()
 * en_icsb_<TABLE_NAME>_cleanup()
 */
#define ICSB_NODE(NAME, NAME_STR) ENGINE_FUNC_ICSB(NAME);
    ICSB_NODES
#undef ICSB_NODE

/* Define engine nodes for NB, SB, ICNB and ICSB tables
 *
 * struct engine_node en_nb_<TABLE_NAME>
 * struct engine_node en_sb_<TABLE_NAME>
 * struct engine_node en_icnb_<TABLE_NAME>
 * struct engine_node en_icsb_<TABLE_NAME>
 *
 * Define nodes as static to avoid sparse errors.
 */
#define NB_NODE(NAME, NAME_STR) static ENGINE_NODE_NB(NAME);
    NB_NODES
#undef NB_NODE

#define SB_NODE(NAME, NAME_STR) static ENGINE_NODE_SB(NAME);
    SB_NODES
#undef SB_NODE

#define ICNB_NODE(NAME, NAME_STR) static ENGINE_NODE_ICNB(NAME);
    ICNB_NODES
#undef ICNB_NODE

#define ICSB_NODE(NAME, NAME_STR) static ENGINE_NODE_ICSB(NAME);
    ICSB_NODES
#undef ICSB_NODE

/* Define engine nodes for other nodes. They should be defined as static to
 * avoid sparse errors. */
static ENGINE_NODE(az);
static ENGINE_NODE(dp_enum);
static ENGINE_NODE(gateway);
static ENGINE_NODE(ts);
static ENGINE_NODE(tr);
static ENGINE_NODE(port_binding, CLEAR_TRACKED_DATA);
static ENGINE_NODE(route, CLEAR_TRACKED_DATA);
static ENGINE_NODE(service_monitor);
static ENGINE_NODE(address_set);
static ENGINE_NODE(ic);

void inc_proc_ic_init(struct ovsdb_idl_loop *nb,
                      struct ovsdb_idl_loop *sb,
                      struct ovsdb_idl_loop *icnb,
                      struct ovsdb_idl_loop *icsb)
{
    /* Define relationships between nodes where the first argument is dependent
     * on the second argument.
     *
     * Each subsystem node below depends on the table input nodes it reads, so
     * the engine only re-runs a subsystem when one of its inputs changed.  No
     * change handlers are wired yet: every dependency uses a NULL handler, so
     * any tracked change to an input triggers a full recompute of just that
     * subsystem (run() method).  This preserves the previous behavior while
     * splitting the monolithic ovn_db_run() into independently-gated nodes.
     * Change handlers are added incrementally in a later step. */

    /* en_dp_enum: enumerate IC-SB datapath bindings (tunnel-key allocator and
     * transit switch/router datapath maps shared by en_ts and en_tr).
     *
     * The node's state (dp_tnlids, isb_ts_dps, isb_tr_dps) is derived purely
     * from the IC-SB Datapath_Binding rows and is maintained incrementally by
     * the en_icsb_datapath_binding handler (insert/delete in place; a tunnel
     * key/transit-switch/nb_ic_uuid modify falls back to a recompute).
     *
     * The IC-NB transit switch/router inputs are ordering dependencies only:
     * en_ts and en_tr (downstream of en_dp_enum) allocate datapath tunnel keys
     * from the shared 'dp_tnlids' set during their own scoped handlers.  That
     * allocation needs no en_dp_enum rebuild - ovn_allocate_tnlid() reserves
     * the key in the live 'dp_tnlids' immediately (so concurrent allocations
     * in the same iteration cannot collide), ovn_add_tnlid() is idempotent
     * (so the committed binding re-arriving as an insert is a no-op), and the
     * binding's insert/delete is reconciled by the en_icsb_datapath_binding
     * handler on a later iteration. A transit switch/router add or delete
     * therefore does not require re-enumerating every datapath binding, so
     * these edges use a no-op handler instead of forcing a full recompute
     * - which matters at scale (tens of thousands of transit switches). The
     * only IC-NB Global change that affects the allocator is vxlan_mode (a
     * global tunnel-key refresh), which still forces a recompute via
     * en_ic_nb_global_handler. */
    engine_add_input(&en_dp_enum, &en_icsb_datapath_binding,
                     en_dp_enum_icsb_datapath_binding_handler);
    engine_add_input(&en_dp_enum, &en_icnb_transit_switch,
                     engine_noop_handler);
    engine_add_input(&en_dp_enum, &en_icnb_transit_router,
                     engine_noop_handler);
    engine_add_input(&en_dp_enum, &en_icnb_ic_nb_global,
                     en_ic_nb_global_handler);

    /* en_gateway: sync gateways/chassis between SB and IC-SB.
     *
     * The availability zone is provided by en_az (which reports EN_UPDATED
     * only when the AZ identity changes).  en_gateway does not read the
     * Availability_Zone table itself - only gateway rows' availability_zone
     * reference and ctx->runned_az - so it deliberately does not depend on
     * en_icsb_availability_zone, whose rows also carry the frequently-bumped
     * nb_ic_cfg sequence number. */
    engine_add_input(&en_gateway, &en_az, NULL);
    engine_add_input(&en_gateway, &en_icsb_gateway,
                     en_gateway_icsb_gateway_handler);
    engine_add_input(&en_gateway, &en_icsb_encap, NULL);
    engine_add_input(&en_gateway, &en_sb_chassis,
                     en_gateway_sb_chassis_handler);
    engine_add_input(&en_gateway, &en_sb_encap, NULL);

    /* en_ts: sync transit switches to NB and IC-SB datapath bindings.
     *
     * en_dp_enum is an ordering dependency only: it owns the shared tunnel-key
     * allocator (dp_tnlids) and transit-switch datapath map, both maintained
     * incrementally and read live by en_ts.  Because it recomputes en_ts's
     * datapath map in place, the en_dp_enum edge itself must not force a full
     * en_ts recompute, so it uses a no-op handler; the resulting IC-SB
     * Datapath_Binding change is instead reacted to directly via
     * en_icsb_datapath_binding below, which re-syncs only the affected transit
     * switches. This matters at scale (tens of thousands of transit switches):
     * the alternative NULL edge would recompute every transit switch on any
     * datapath-binding change.
     *
     * en_icsb_datapath_binding drives the follow-up NB requested-tnl-key sync
     * after a tunnel-key (re)assignment - notably the global refresh from an
     * IC-NB vxlan_mode change (see en_ts_icsb_datapath_binding_handler).  It
     * is ordered after en_dp_enum (which depends on the same table), so en_ts
     * sees the freshly folded key. */
    engine_add_input(&en_ts, &en_az, NULL);
    engine_add_input(&en_ts, &en_dp_enum, engine_noop_handler);
    engine_add_input(&en_ts, &en_icsb_datapath_binding,
                     en_ts_icsb_datapath_binding_handler);
    engine_add_input(&en_ts, &en_icnb_ic_nb_global, en_ic_nb_global_handler);
    engine_add_input(&en_ts, &en_icnb_transit_switch,
                     en_ts_icnb_transit_switch_handler);
    engine_add_input(&en_ts, &en_nb_logical_switch,
                     en_ts_nb_logical_switch_handler);
    engine_add_input(&en_ts, &en_icsb_encap, NULL);

    /* en_tr: sync transit routers to NB and IC-SB datapath bindings.
     *
     * Like en_ts, en_dp_enum is an ordering dependency only: en_tr reads its
     * shared tunnel-key allocator (dp_tnlids) and transit-router datapath map
     * (isb_tr_dps) live in en_tr_run(), so a datapath-binding change it
     * reports - including transit-switch churn that does not concern en_tr,
     * and the binding en_tr itself just created - must not force a full
     * recompute; the edge therefore uses a no-op handler.  The real re-run
     * trigger is the en_icsb_datapath_binding handler, which recomputes only
     * for transit-router datapath bindings (en_dp_enum, being upstream, has
     * already refreshed isb_tr_dps by then). */
    engine_add_input(&en_tr, &en_az, NULL);
    engine_add_input(&en_tr, &en_dp_enum, engine_noop_handler);
    engine_add_input(&en_tr, &en_icsb_datapath_binding,
                     en_tr_icsb_datapath_binding_handler);
    engine_add_input(&en_tr, &en_icnb_transit_router, NULL);
    engine_add_input(&en_tr, &en_nb_logical_router,
                     en_tr_nb_logical_router_handler);

    /* en_port_binding: sync cross-AZ port bindings.
     *
     * Like en_gateway, this node uses only the AZ identity (ctx->runned_az,
     * provided by en_az, plus the by-AZ port-binding index) and does not read
     * the Availability_Zone table itself, so it does not depend on
     * en_icsb_availability_zone and is not churned by the nb_ic_cfg sequence
     * number bumped there on every change. */
    engine_add_input(&en_port_binding, &en_az, NULL);
    engine_add_input(&en_port_binding, &en_icsb_port_binding,
                     port_binding_icsb_port_binding_handler);
    engine_add_input(&en_port_binding, &en_icnb_transit_switch,
                     port_binding_icnb_transit_switch_handler);
    engine_add_input(&en_port_binding, &en_icnb_transit_router,
                     port_binding_icnb_transit_router_handler);
    engine_add_input(&en_port_binding, &en_icnb_transit_router_port,
                     port_binding_icnb_transit_router_port_handler);
    engine_add_input(&en_port_binding, &en_nb_logical_switch,
                     port_binding_nb_logical_switch_handler);
    engine_add_input(&en_port_binding, &en_nb_logical_switch_port,
                     port_binding_nb_logical_switch_port_handler);
    engine_add_input(&en_port_binding, &en_nb_logical_router,
                     port_binding_nb_logical_router_handler);
    engine_add_input(&en_port_binding, &en_nb_logical_router_port,
                     port_binding_nb_logical_router_port_handler);
    engine_add_input(&en_port_binding, &en_sb_port_binding,
                     port_binding_sb_port_binding_handler);
    /* SB chassis affects gateways and trp_is_remote across many ports.  The
     * reverse mapping chassis -> affected ports is impractical, but the sync
     * only reads a chassis' existence and its other_config (is-remote); so the
     * handler recomputes only on chassis insert/delete or an other_config
     * change and ignores the frequent heartbeat-style updates (Fix D). */
    engine_add_input(&en_port_binding, &en_sb_chassis,
                     port_binding_sb_chassis_handler);

    /* en_route: advertise/learn cross-AZ routes.
     *
     * Like en_gateway and en_port_binding, this node uses only the AZ identity
     * (ctx->runned_az from en_az and the by-AZ route/port-binding indexes) and
     * does not read the Availability_Zone table, so it does not depend on
     * en_icsb_availability_zone and is not churned by its nb_ic_cfg sequence
     * number. */
    engine_add_input(&en_route, &en_az, NULL);
    /* en_port_binding is an ordering dependency only: en_route reads IC-SB
     * port bindings synced by en_port_binding, so it must run after it.  The
     * real port-binding data arrives via en_icsb_port_binding, so a no-op
     * handler avoids forcing a recompute. */
    engine_add_input(&en_route, &en_port_binding, engine_noop_handler);
    engine_add_input(&en_route, &en_icsb_port_binding,
                     route_icsb_port_binding_handler);
    engine_add_input(&en_route, &en_icsb_route, route_icsb_route_handler);
    engine_add_input(&en_route, &en_icnb_transit_switch,
                     route_icnb_transit_switch_handler);
    engine_add_input(&en_route, &en_nb_nb_global, route_nb_nb_global_handler);
    engine_add_input(&en_route, &en_nb_logical_switch,
                     route_nb_logical_switch_handler);
    engine_add_input(&en_route, &en_nb_logical_router,
                     route_nb_logical_router_handler);
    engine_add_input(&en_route, &en_nb_logical_router_port,
                     route_nb_logical_router_port_handler);
    engine_add_input(&en_route, &en_nb_logical_router_static_route,
                     route_nb_logical_router_static_route_handler);
    engine_add_input(&en_route, &en_nb_logical_switch_port,
                     route_nb_logical_switch_port_handler);
    engine_add_input(&en_route, &en_nb_load_balancer,
                     route_nb_load_balancer_handler);
    engine_add_input(&en_route, &en_nb_load_balancer_group,
                     route_nb_load_balancer_group_handler);
    engine_add_input(&en_route, &en_sb_datapath_binding,
                     route_sb_datapath_binding_handler);
    engine_add_input(&en_route, &en_sb_learned_route,
                     route_sb_learned_route_handler);

    /* en_service_monitor: sync load-balancer health checks across AZs.
     *
     * Like the other AZ-scoped nodes it uses only the AZ identity
     * (ctx->runned_az->name from en_az and the by-source/target-AZ indexes)
     * and does not read the Availability_Zone table, so it does not depend on
     * en_icsb_availability_zone and is not churned by its nb_ic_cfg sequence
     * number. */
    engine_add_input(&en_service_monitor, &en_az, NULL);
    engine_add_input(&en_service_monitor, &en_icsb_service_monitor, NULL);
    engine_add_input(&en_service_monitor, &en_sb_sb_global, NULL);
    engine_add_input(&en_service_monitor, &en_sb_service_monitor, NULL);
    /* SB port bindings are the busiest table in the AZ, but the sync reads
     * only the 'up'/'chassis' of ports backing a service monitor targeting
     * this AZ, so a dedicated handler scopes that churn out instead of forcing
     * a full recompute on every port-binding change. */
    engine_add_input(&en_service_monitor, &en_sb_port_binding,
                     en_service_monitor_sb_port_binding_handler);

    /* en_address_set: advertise/learn address sets across AZs. */
    engine_add_input(&en_address_set, &en_nb_nb_global, NULL);
    engine_add_input(&en_address_set, &en_nb_address_set, NULL);
    engine_add_input(&en_address_set, &en_sb_address_set, NULL);
    engine_add_input(&en_address_set, &en_icsb_address_set, NULL);

    /* en_ic: output node aggregating all subsystems.  Order matches the
     * previous ovn_db_run() call order; in particular en_ts is added before
     * en_tr so they allocate datapath tunnel keys from the shared en_dp_enum
     * allocator in the same order as before. */
    engine_add_input(&en_ic, &en_gateway, NULL);
    engine_add_input(&en_ic, &en_ts, NULL);
    engine_add_input(&en_ic, &en_tr, NULL);
    engine_add_input(&en_ic, &en_port_binding, NULL);
    engine_add_input(&en_ic, &en_route, NULL);
    engine_add_input(&en_ic, &en_service_monitor, NULL);
    engine_add_input(&en_ic, &en_address_set, NULL);

    struct engine_arg engine_arg = {
        .nb_idl = nb->idl,
        .sb_idl = sb->idl,
        .icnb_idl = icnb->idl,
        .icsb_idl = icsb->idl,
    };

    engine_init(&en_ic, &engine_arg);
}

/* Returns true if the incremental processing ended up updating nodes. */
bool
inc_proc_ic_run(struct ic_context *ctx,
                struct ic_engine_context *ic_eng_ctx)
{
    ovs_assert(ctx->ovnnb_txn && ctx->ovnsb_txn &&
               ctx->ovninb_txn && ctx->ovnisb_unlocked_txn);

    int64_t start = time_msec();
    engine_init_run();

    struct engine_context eng_ctx = {
        .client_ctx = ctx,
    };

    engine_set_context(&eng_ctx);
    engine_run(true);

    if (!engine_has_run()) {
        if (engine_need_run()) {
            VLOG_DBG("engine did not run, force recompute next time.");
            engine_set_force_recompute_immediate();
        } else {
            VLOG_DBG("engine did not run, and it was not needed");
        }
    } else if (engine_canceled()) {
        VLOG_DBG("engine was canceled, force recompute next time.");
        engine_set_force_recompute_immediate();
    } else {
        engine_clear_force_recompute();
    }

    int64_t now = time_msec();
    /* Postpone the next run by length of current run with maximum capped
     * by "northd-backoff-interval-ms" interval. */
    ic_eng_ctx->next_run_ms = now + MIN(now - start, ic_eng_ctx->backoff_ms);

    return engine_has_updated();
}

void
inc_proc_ic_cleanup(void)
{
    engine_cleanup();
    engine_set_context(NULL);
}

bool
inc_proc_ic_can_run(struct ic_engine_context *ctx)
{
    if (engine_get_force_recompute() || time_msec() >= ctx->next_run_ms ||
        ctx->nb_idl_duration_ms >= IDL_LOOP_MAX_DURATION_MS ||
        ctx->sb_idl_duration_ms >= IDL_LOOP_MAX_DURATION_MS ||
        ctx->inb_idl_duration_ms >= IDL_LOOP_MAX_DURATION_MS ||
        ctx->isb_idl_duration_ms >= IDL_LOOP_MAX_DURATION_MS ||
        ctx->isb_unlock_idl_duration_ms >= IDL_LOOP_MAX_DURATION_MS) {
        return true;
    }

    poll_timer_wait_until(ctx->next_run_ms);
    return false;
}
