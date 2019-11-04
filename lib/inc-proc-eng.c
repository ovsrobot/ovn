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
    [EN_NEW]       = "New",
    [EN_STALE]     = "Stale",
    [EN_UPDATED]   = "Updated",
    [EN_VALID]     = "Valid",
    [EN_ABORTED]   = "Aborted",
    [EN_DESTROYED] = "Destroyed",
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

void
engine_init(struct engine_node *node, void *arg)
{
    if (!engine_node_new(node)) {
        /* The node was already initialized (could be input for multiple
         * nodes). Nothing to do then.
         */
        return;
    }

    engine_set_node_state(node, EN_STALE);
    for (size_t i = 0; i < node->n_inputs; i++) {
        engine_init(node->inputs[i].node, arg);
    }
    if (node->init) {
        node->init(node, arg);
    }
}

void
engine_cleanup(struct engine_node *node)
{
    /* The neode was already destroyed (could be input for multiple nodes).
     * Nothing to do then.
     */
    if (engine_node_destroyed(node)) {
        return;
    }

    engine_set_node_state(node, EN_DESTROYED);
    for (size_t i = 0; i < node->n_inputs; i++) {
        engine_cleanup(node->inputs[i].node);
    }
    if (node->cleanup) {
        node->cleanup(node);
    }
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
    struct ed_type_ovsdb_table *ed = (struct ed_type_ovsdb_table *)node->data;
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
    struct ed_type_ovsdb_table *ed = (struct ed_type_ovsdb_table *)node->data;
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

    VLOG_DBG("%s: node: %s (run-id %lu), old_state %s, new_state %s",
             where, node->name, node->run_id,
             engine_node_state_name[node->state],
             engine_node_state_name[state]);

    node->state = state;
}

bool
engine_node_new(struct engine_node *node)
{
    return node->state == EN_NEW;
}

bool
engine_node_destroyed(struct engine_node *node)
{
    return node->state == EN_DESTROYED;
}

bool
engine_node_valid(struct engine_node *node, uint64_t run_id)
{
    return node->run_id == run_id &&
        (node->state == EN_UPDATED || node->state == EN_VALID);
}

bool
engine_node_changed(struct engine_node *node, uint64_t run_id)
{
    return node->run_id == run_id && node->state == EN_UPDATED;
}

bool
engine_has_run(struct engine_node *node, uint64_t run_id)
{
    return node->run_id == run_id;
}

bool
engine_aborted(struct engine_node *node)
{
    return node->state == EN_ABORTED;
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
                return false;
            }
        }
    }

    return true;
}

void
engine_run(struct engine_node *node, uint64_t run_id)
{
    if (node->run_id == run_id) {
        /* The node was already updated in this run (could be input for
         * multiple other nodes). Stop processing.
         */
        return;
    }

    /* Initialize the node for this run. */
    node->run_id = run_id;
    engine_set_node_state(node, EN_STALE);

    if (!node->n_inputs) {
        /* Run the node handler which might change state. */
        node->run(node);
        return;
    }

    for (size_t i = 0; i < node->n_inputs; i++) {
        engine_run(node->inputs[i].node, run_id);
        if (!engine_node_valid(node->inputs[i].node, run_id)) {
            /* If the input node aborted computation, move to EN_ABORTED to
             * propagate the result, otherwise stay in EN_STALE.
             */
            if (engine_aborted(node->inputs[i].node)) {
                engine_set_node_state(node, EN_ABORTED);
            }
            return;
        }
    }

    bool need_compute = false;

    if (engine_force_recompute) {
        engine_recompute(node, true, !engine_abort_recompute);
        return;
    }

    /* If one of the inputs updated data then we need to recompute the
     * current node too.
     */
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
        /* If we coudln't compute the node we either aborted or triggered
         * a full recompute. In any case, stop processing.
         */
        if (!engine_compute(node, !engine_abort_recompute)) {
            return;
        }
    }

    /* If we reached this point, either the node was updated or its state is
     * still valid.
     */
    if (!engine_node_changed(node, run_id)) {
        engine_set_node_state(node, EN_VALID);
    }
}

bool
engine_need_run(struct engine_node *node, uint64_t run_id)
{
    size_t i;

    if (node->run_id == run_id) {
        return false;
    }

    if (!node->n_inputs) {
        node->run(node);
        VLOG_DBG("input node: %s, state: %s", node->name,
                 engine_node_state_name[node->state]);
        return node->state == EN_UPDATED;
    }

    for (i = 0; i < node->n_inputs; i++) {
        if (engine_need_run(node->inputs[i].node, run_id)) {
            return true;
        }
    }

    return false;
}
