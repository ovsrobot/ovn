/*
 * Copyright (c) 2018 eBay Inc.
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

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "lib/util.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/hmap.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/vlog.h"
#include "inc-proc-eng.h"
#include "timeval.h"
#include "unixctl.h"

VLOG_DEFINE_THIS_MODULE(inc_proc_eng);

static struct ovs_list engines = OVS_LIST_INITIALIZER(&engines);

static const char *engine_node_state_name[EN_STATE_MAX] = {
    [EN_STALE]     = "Stale",
    [EN_UPDATED]   = "Updated",
    [EN_UNCHANGED] = "Unchanged",
    [EN_ABORTED]   = "Aborted",
};

static void
engine_recompute(struct engine_node *node, bool allowed,
                 const char *reason_fmt, ...) OVS_PRINTF_FORMAT(3, 4);

void
engine_set_force_recompute(struct engine *e, bool val)
{
    e->engine_force_recompute = val;
}

const struct engine_context *
engine_get_context(struct engine *e)
{
    return e->engine_context;
}

void
engine_set_context(struct engine *e, const struct engine_context *ctx)
{
    e->engine_context = ctx;
}

/* Builds the topologically sorted 'sorted_nodes' array starting from
 * 'node'.
 */
static struct engine_node **
engine_topo_sort(struct engine_node *node, struct engine_node **sorted_nodes,
                 size_t *n_count, size_t *n_size)
{
    /* It's not so efficient to walk the array of already sorted nodes but
     * we know that sorting is done only once at startup so it's ok for now.
     */
    for (size_t i = 0; i < *n_count; i++) {
        if (sorted_nodes[i] == node) {
            return sorted_nodes;
        }
    }

    for (size_t i = 0; i < node->n_inputs; i++) {
        sorted_nodes = engine_topo_sort(node->inputs[i].node, sorted_nodes,
                                        n_count, n_size);
    }
    if (*n_count == *n_size) {
        sorted_nodes = x2nrealloc(sorted_nodes, n_size, sizeof *sorted_nodes);
    }
    sorted_nodes[(*n_count)] = node;
    (*n_count)++;
    return sorted_nodes;
}

/* Return the array of topologically sorted nodes when starting from
 * 'node'. Stores the number of nodes in 'n_count'.
 */
static struct engine_node **
engine_get_nodes(struct engine_node *node, size_t *n_count)
{
    size_t n_size = 0;

    *n_count = 0;
    return engine_topo_sort(node, NULL, n_count, &n_size);
}

static void
engine_clear_stats(struct unixctl_conn *conn, int argc OVS_UNUSED,
                   const char *argv[] OVS_UNUSED, void *arg OVS_UNUSED)
{
    const char *target = argc == 2 ? argv[1] : NULL;
    struct ds reply = DS_EMPTY_INITIALIZER;
    struct engine *e;

    ds_put_format(&reply, "no %s engine found", target ? target : "");
    LIST_FOR_EACH (e, node, &engines) {
        for (size_t i = 0; i < e->engine_n_nodes; i++) {
            struct engine_node *node = e->engine_nodes[i];

            if (target && strcmp(target, e->name)) {
                continue;
            }
            memset(&node->stats, 0, sizeof node->stats);
            ds_clear(&reply);
        }
    }

    unixctl_command_reply(conn, ds_cstr(&reply));
    ds_destroy(&reply);
}

static void
engine_dump_stats(struct unixctl_conn *conn, int argc,
                  const char *argv[] OVS_UNUSED, void *arg OVS_UNUSED)
{
    const char *target = argc == 2 ? argv[1] : NULL;
    struct ds dump = DS_EMPTY_INITIALIZER;
    struct engine *e;

    LIST_FOR_EACH (e, node, &engines) {
        for (size_t i = 0; i < e->engine_n_nodes; i++) {
            struct engine_node *node = e->engine_nodes[i];

            if (target && strcmp(target, e->name)) {
                continue;
            }
            ds_put_format(&dump,
                          "Node: %s\n"
                          "- recompute: %12"PRIu64"\n"
                          "- compute:   %12"PRIu64"\n"
                          "- abort:     %12"PRIu64"\n",
                          node->name, node->stats.recompute,
                          node->stats.compute, node->stats.abort);
        }
    }
    if (!dump.length) {
        ds_put_format(&dump, "no %s engine found", target ? target : "");
    }
    unixctl_command_reply(conn, ds_cstr(&dump));

    ds_destroy(&dump);
}

static void
engine_trigger_recompute_cmd(struct unixctl_conn *conn, int argc OVS_UNUSED,
                             const char *argv[] OVS_UNUSED,
                             void *arg OVS_UNUSED)
{
    const char *target = argc == 2 ? argv[1] : NULL;
    struct ds reply = DS_EMPTY_INITIALIZER;
    struct engine *e;

    ds_put_format(&reply, "no %s engine found", target ? target : "");
    LIST_FOR_EACH (e, node, &engines) {
        if (target && strcmp(target, e->name)) {
            continue;
        }
        engine_trigger_recompute(e);
        ds_clear(&reply);
    }

    unixctl_command_reply(conn, ds_cstr(&reply));
    ds_destroy(&reply);
}

void engine_init(struct engine **pe, struct engine_node *node,
                 struct engine_arg *arg, const char *name)
{
    struct engine *e = xzalloc(sizeof *e);

    e->engine_nodes = engine_get_nodes(node, &e->engine_n_nodes);
    e->name = name;

    for (size_t i = 0; i < e->engine_n_nodes; i++) {
        if (e->engine_nodes[i]->init) {
            e->engine_nodes[i]->data =
                e->engine_nodes[i]->init(e->engine_nodes[i], arg);
        } else {
            e->engine_nodes[i]->data = NULL;
        }
        e->engine_nodes[i]->e = e;
    }

    unixctl_command_register("inc-engine/show-stats", "", 0, 1,
                             engine_dump_stats, NULL);
    unixctl_command_register("inc-engine/clear-stats", "", 0, 1,
                             engine_clear_stats, NULL);
    unixctl_command_register("inc-engine/recompute", "", 0, 1,
                             engine_trigger_recompute_cmd, NULL);
    ovs_list_push_back(&engines, &e->node);
    *pe = e;
}

void
engine_cleanup(struct engine **pe)
{
    struct engine *e = *pe;
    for (size_t i = 0; i < e->engine_n_nodes; i++) {
        if (e->engine_nodes[i]->clear_tracked_data) {
            e->engine_nodes[i]->clear_tracked_data(
                    e->engine_nodes[i]->data);
        }

        if (e->engine_nodes[i]->cleanup) {
            e->engine_nodes[i]->cleanup(e->engine_nodes[i]->data);
        }
        free(e->engine_nodes[i]->data);
    }
    e->engine_n_nodes = 0;
    free(e->engine_nodes);
    *pe = NULL;
}

struct engine_node *
engine_get_input(const char *input_name, struct engine_node *node)
{
    size_t i;
    for (i = 0; i < node->n_inputs; i++) {
        if (!strcmp(node->inputs[i].node->name, input_name)) {
            return node->inputs[i].node;
        }
    }
    OVS_NOT_REACHED();
    return NULL;
}

void *
engine_get_input_data(const char *input_name, struct engine_node *node)
{
    struct engine_node *input_node = engine_get_input(input_name, node);
    return engine_get_data(input_node);
}

void
engine_add_input(struct engine_node *node, struct engine_node *input,
                 bool (*change_handler)(struct engine_node *, void *))
{
    ovs_assert(node->n_inputs < ENGINE_MAX_INPUT);
    node->inputs[node->n_inputs].node = input;
    node->inputs[node->n_inputs].change_handler = change_handler;
    node->n_inputs ++;
}

struct ovsdb_idl_index *
engine_ovsdb_node_get_index(struct engine_node *node, const char *name)
{
    struct ed_type_ovsdb_table *ed = node->data;
    for (size_t i = 0; i < ed->n_indexes; i++) {
        if (!strcmp(ed->indexes[i].name, name)) {
            return ed->indexes[i].index;
        }
    }
    OVS_NOT_REACHED();
    return NULL;
}

void
engine_ovsdb_node_add_index(struct engine_node *node, const char *name,
                            struct ovsdb_idl_index *index)
{
    struct ed_type_ovsdb_table *ed = node->data;
    ovs_assert(ed->n_indexes < ENGINE_MAX_OVSDB_INDEX);

    ed->indexes[ed->n_indexes].name = name;
    ed->indexes[ed->n_indexes].index = index;
    ed->n_indexes ++;
}

void
engine_set_node_state_at(struct engine_node *node,
                         enum engine_node_state state,
                         const char *where)
{
    if (node->state == state) {
        return;
    }

    VLOG_DBG("%s: node: %s, old_state %s, new_state %s",
             where, node->name,
             engine_node_state_name[node->state],
             engine_node_state_name[state]);

    node->state = state;
}

static bool
engine_node_valid(struct engine_node *node)
{
    if (node->state == EN_UPDATED || node->state == EN_UNCHANGED) {
        return true;
    }

    if (node->is_valid) {
        return node->is_valid(node);
    }
    return false;
}

bool
engine_node_changed(struct engine_node *node)
{
    return node->state == EN_UPDATED;
}

bool
engine_has_run(struct engine *e)
{
    for (size_t i = 0; i < e->engine_n_nodes; i++) {
        if (e->engine_nodes[i]->state != EN_STALE) {
            return true;
        }
    }
    return false;
}

bool
engine_aborted(struct engine *e)
{
    return e->engine_run_aborted;
}

void *
engine_get_data(struct engine_node *node)
{
    if (engine_node_valid(node)) {
        return node->data;
    }
    return NULL;
}

void *
engine_get_internal_data(struct engine_node *node)
{
    return node->data;
}

void
engine_init_run(struct engine *e)
{
    VLOG_DBG("Initializing new run");
    for (size_t i = 0; i < e->engine_n_nodes; i++) {
        engine_set_node_state(e->engine_nodes[i], EN_STALE);

        if (e->engine_nodes[i]->clear_tracked_data) {
            e->engine_nodes[i]->clear_tracked_data(
                    e->engine_nodes[i]->data);
        }
    }
}

/* Do a full recompute (or at least try). If we're not allowed then
 * mark the node as "aborted".
 */
static void
engine_recompute(struct engine_node *node, bool allowed,
                 const char *reason_fmt, ...)
{
    char *reason = NULL;

    if (VLOG_IS_DBG_ENABLED()) {
        va_list reason_args;

        va_start(reason_args, reason_fmt);
        reason = xvasprintf(reason_fmt, reason_args);
        va_end(reason_args);
    }

    if (!allowed) {
        VLOG_DBG("node: %s, recompute (%s) aborted", node->name, reason);
        engine_set_node_state(node, EN_ABORTED);
        goto done;
    }

    /* Clear tracked data before calling run() so that partially tracked data
     * from some of the change handler executions are cleared. */
    if (node->clear_tracked_data) {
        node->clear_tracked_data(node->data);
    }

    /* Run the node handler which might change state. */
    long long int now = time_msec();
    node->run(node, node->data);
    node->stats.recompute++;
    VLOG_DBG("node: %s, recompute (%s) took %lldms", node->name, reason,
             time_msec() - now);
done:
    free(reason);
}

/* Return true if the node could be computed, false otherwise. */
static bool
engine_compute(struct engine_node *node, bool recompute_allowed)
{
    for (size_t i = 0; i < node->n_inputs; i++) {
        /* If the input node data changed call its change handler. */
        if (node->inputs[i].node->state == EN_UPDATED) {
            /* If the input change can't be handled incrementally, run
             * the node handler.
             */
            long long int now = time_msec();
            bool handled = node->inputs[i].change_handler(node, node->data);

            VLOG_DBG("node: %s, handler for input %s took %lldms",
                     node->name, node->inputs[i].node->name,
                     time_msec() - now);
            if (!handled) {
                engine_recompute(node, recompute_allowed,
                                 "failed handler for input %s",
                                 node->inputs[i].node->name);
                return (node->state != EN_ABORTED);
            }
        }
    }
    node->stats.compute++;

    return true;
}

static void
engine_run_node(struct engine *e, struct engine_node *node,
                bool recompute_allowed)
{
    if (!node->n_inputs) {
        /* Run the node handler which might change state. */
        node->run(node, node->data);
        node->stats.recompute++;
        return;
    }

    if (e->engine_force_recompute) {
        engine_recompute(node, recompute_allowed, "forced");
        return;
    }

    /* If any of the inputs updated data but there is no change_handler, then
     * recompute the current node too.
     */
    bool need_compute = false;
    for (size_t i = 0; i < node->n_inputs; i++) {
        if (node->inputs[i].node->state == EN_UPDATED) {
            need_compute = true;

            /* Trigger a recompute if we don't have a change handler. */
            if (!node->inputs[i].change_handler) {
                engine_recompute(node, recompute_allowed,
                                 "missing handler for input %s",
                                 node->inputs[i].node->name);
                return;
            }
        }
    }

    if (need_compute) {
        /* If we couldn't compute the node we either aborted or triggered
         * a full recompute. In any case, stop processing.
         */
        if (!engine_compute(node, recompute_allowed)) {
            return;
        }
    }

    /* If we reached this point, either the node was updated or its state is
     * still valid.
     */
    if (!engine_node_changed(node)) {
        engine_set_node_state(node, EN_UNCHANGED);
    }
}

void
engine_run(struct engine *e, bool recompute_allowed)
{
    /* If the last run was aborted skip the incremental run because a
     * recompute is needed first.
     */
    if (!recompute_allowed && e->engine_run_aborted) {
        return;
    }

    e->engine_run_aborted = false;
    for (size_t i = 0; i < e->engine_n_nodes; i++) {
        engine_run_node(e, e->engine_nodes[i], recompute_allowed);

        if (e->engine_nodes[i]->state == EN_ABORTED) {
            e->engine_nodes[i]->stats.abort++;
            e->engine_run_aborted = true;
            return;
        }
    }
}

bool
engine_need_run(struct engine *e)
{
    for (size_t i = 0; i < e->engine_n_nodes; i++) {
        /* Check only leaf nodes for updates. */
        if (e->engine_nodes[i]->n_inputs) {
            continue;
        }

        e->engine_nodes[i]->run(e->engine_nodes[i], e->engine_nodes[i]->data);
        e->engine_nodes[i]->stats.recompute++;
        VLOG_DBG("input node: %s, state: %s", e->engine_nodes[i]->name,
                 engine_node_state_name[e->engine_nodes[i]->state]);
        if (e->engine_nodes[i]->state == EN_UPDATED) {
            return true;
        }
    }
    return false;
}

void
engine_trigger_recompute(struct engine *e)
{
    VLOG_INFO("User triggered force recompute.");
    engine_set_force_recompute(e, true);
    poll_immediate_wake();
}
