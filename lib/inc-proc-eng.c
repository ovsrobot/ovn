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
#include "openvswitch/vlog.h"
#include "inc-proc-eng.h"

VLOG_DEFINE_THIS_MODULE(inc_proc_eng);

static bool engine_force_recompute = false;
static bool engine_abort_recompute = false;
static const struct engine_context *engine_context;

static const char *engine_node_state_name[EN_STATE_MAX] = {
    [EN_STALE]   = "Stale",
    [EN_UPDATED] = "Updated",
    [EN_VALID]   = "Valid",
    [EN_ABORTED] = "Aborted",
};

void
engine_set_force_recompute(bool val)
{
    engine_force_recompute = val;
}

void
engine_set_abort_recompute(bool val)
{
    engine_abort_recompute = val;
}

const struct engine_context *
engine_get_context(void)
{
    return engine_context;
}

void
engine_set_context(const struct engine_context *ctx)
{
    engine_context = ctx;
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

struct engine_node **
engine_get_nodes(struct engine_node *root_node, size_t *n_count)
{
    size_t n_size = 0;

    *n_count = 0;
    return engine_topo_sort(root_node, NULL, n_count, &n_size);
}

void
engine_init(struct engine_node **nodes, size_t n_count)
{
    for (size_t i = 0; i < n_count; i++) {
        if (nodes[i]->init) {
            nodes[i]->init(nodes[i]);
        }
    }
}

void
engine_cleanup(struct engine_node **nodes, size_t n_count)
{
    for (size_t i = 0; i < n_count; i++) {
        if (nodes[i]->cleanup) {
            nodes[i]->cleanup(nodes[i]);
        }
    }
    free(nodes);
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

void
engine_add_input(struct engine_node *node, struct engine_node *input,
                 bool (*change_handler)(struct engine_node *))
{
    ovs_assert(node->n_inputs < ENGINE_MAX_INPUT);
    node->inputs[node->n_inputs].node = input;
    node->inputs[node->n_inputs].change_handler = change_handler;
    node->n_inputs ++;
}

struct ovsdb_idl_index *
engine_ovsdb_node_get_index(struct engine_node *node, const char *name)
{
    struct ed_type_ovsdb_table *ed = node->internal_data;
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
    struct ed_type_ovsdb_table *ed = node->internal_data;
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
    if (node->is_valid) {
        return node->is_valid(node);
    }
    return (node->state == EN_UPDATED || node->state == EN_VALID);
}

bool
engine_node_changed(struct engine_node *node)
{
    return node->state == EN_UPDATED;
}

bool
engine_has_run(struct engine_node *root_node)
{
    return root_node->state != EN_STALE;
}

bool
engine_aborted(struct engine_node *node)
{
    return node->state == EN_ABORTED;
}

static void *
engine_get_data(struct engine_node *node)
{
    if (engine_node_valid(node)) {
        return node->internal_data;
    }
    return NULL;
}

void
engine_init_run(struct engine_node **nodes, size_t n_count,
                struct engine_node *root_node)
{
    /* No need to reinitialize if last run didn't happen. */
    if (!engine_has_run(root_node)) {
        return;
    }

    VLOG_DBG("Initializing new run");
    for (size_t i = 0; i < n_count; i++) {
        engine_set_node_state(nodes[i], EN_STALE);

        /* Make sure we reset the data pointer for outside users.
         * For nodes that always store valid data the value will be non-NULL.
         */
        nodes[i]->data = engine_get_data(nodes[i]);
    }
}

/* Do a full recompute (or at least try). If we're not allowed then
 * mark the node as "aborted".
 */
static void
engine_recompute(struct engine_node *node, bool forced, bool allowed)
{
    VLOG_DBG("node: %s, recompute (%s)", node->name,
             forced ? "forced" : "triggered");

    if (!allowed) {
        VLOG_DBG("node: %s, recompute aborted", node->name);
        engine_set_node_state(node, EN_ABORTED);
        return;
    }

    /* Run the node handler which might change state. */
    node->run(node);
}

/* Return true if the node could be computed without triggerring a full
 * recompute.
 */
static bool
engine_compute(struct engine_node *node, bool recompute_allowed)
{
    for (size_t i = 0; i < node->n_inputs; i++) {
        /* If the input node data changed call its change handler. */
        if (node->inputs[i].node->state == EN_UPDATED) {
            VLOG_DBG("node: %s, handle change for input %s",
                     node->name, node->inputs[i].node->name);

            /* If the input change can't be handled incrementally, run
             * the node handler.
             */
            if (!node->inputs[i].change_handler(node)) {
                VLOG_DBG("node: %s, can't handle change for input %s, "
                         "fall back to recompute",
                         node->name, node->inputs[i].node->name);
                engine_recompute(node, false, recompute_allowed);
                if (engine_aborted(node)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void
engine_run_node(struct engine_node *node)
{
    if (!node->n_inputs) {
        /* Run the node handler which might change state. */
        node->run(node);
        return;
    }

    bool input_stale = false;
    for (size_t i = 0; i < node->n_inputs; i++) {
        if (!engine_node_valid(node->inputs[i].node)) {
            /* If the input node aborted computation, move to EN_ABORTED.
             * This will be propagated to following nodes.
             */
            if (engine_aborted(node->inputs[i].node)) {
                engine_set_node_state(node, EN_ABORTED);
            }

            input_stale = true;
        }
    }

    /* If at least one input is stale, don't change state. */
    if (input_stale) {
        return;
    }

    if (engine_force_recompute) {
        engine_recompute(node, true, !engine_abort_recompute);
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
                engine_recompute(node, false, !engine_abort_recompute);
                return;
            }
        }
    }

    if (need_compute) {
        /* If we couldn't compute the node we either aborted or triggered
         * a full recompute. In any case, stop processing.
         */
        if (!engine_compute(node, !engine_abort_recompute)) {
            return;
        }
    }

    /* If we reached this point, either the node was updated or its state is
     * still valid.
     */
    if (!engine_node_changed(node)) {
        engine_set_node_state(node, EN_VALID);
    }
}

void
engine_run(struct engine_node **nodes, size_t n_count)
{
    for (size_t i = 0; i < n_count; i++) {
        engine_run_node(nodes[i]);
        /* Make sure we reset the data pointer for outside users as the
         * node's state might have changed.
         */
        nodes[i]->data = engine_get_data(nodes[i]);
    }
}

bool
engine_need_run(struct engine_node **nodes, size_t n_count)
{
    for (size_t i = 0; i < n_count; i++) {
        /* Check only leaf nodes. */
        if (nodes[i]->n_inputs) {
            continue;
        }

        nodes[i]->run(nodes[i]);
        VLOG_DBG("input node: %s, state: %s", nodes[i]->name,
                 engine_node_state_name[nodes[i]->state]);
        if (nodes[i]->state == EN_UPDATED) {
            return true;
        }
    }
    return false;
}
