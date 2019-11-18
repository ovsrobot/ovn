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
engine_init(struct engine_node *node)
{
    for (size_t i = 0; i < node->n_inputs; i++) {
        engine_init(node->inputs[i].node);
    }
    if (node->init) {
        node->init(node);
    }
}

void
engine_cleanup(struct engine_node *node)
{
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

bool
engine_has_run(struct engine_node *node, uint64_t run_id)
{
    return node->run_id == run_id;
}

/* Do a full recompute (or at least try). If we're not allowed then
 * mark the node as "aborted".
 */
static bool
engine_recompute(struct engine_node *node, bool forced, bool allowed)
{
    VLOG_DBG("node: %s, recompute (%s)", node->name,
             forced ? "forced" : "triggered");

    if (!allowed) {
        VLOG_DBG("node: %s, recompute aborted", node->name);
        return false;
    }

    node->run(node);
    VLOG_DBG("node: %s, changed: %d", node->name, node->changed);
    return true;
}

/* Return true if the node could be computed without triggerring a full
 * recompute.
 */
static bool
engine_compute(struct engine_node *node, bool recompute_allowed)
{
    for (size_t i = 0; i < node->n_inputs; i++) {
        /* If the input node data changed call its change handler. */
        if (node->inputs[i].node->changed) {
            VLOG_DBG("node: %s, handle change for input %s",
                     node->name, node->inputs[i].node->name);

            /* If the input change can't be handled incrementally, run
             * the node handler.
             */
            if (!node->inputs[i].change_handler(node)) {
                VLOG_DBG("node: %s, can't handle change for input %s, "
                         "fall back to recompute",
                         node->name, node->inputs[i].node->name);
                if (!engine_recompute(node, false, recompute_allowed)) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool engine_run(struct engine_node *node, uint64_t run_id)
{
    if (node->run_id == run_id) {
        /* The node was already updated in this run (could be input for
         * multiple other nodes). Stop processing.
         */
        return true;
    }

    /* Initialize the node for this run. */
    node->run_id = run_id;
    node->changed = false;

    if (!node->n_inputs) {
        node->run(node);
        VLOG_DBG("node: %s, changed: %d", node->name, node->changed);
        return true;
    }

    for (size_t i = 0; i < node->n_inputs; i++) {
        if (!engine_run(node->inputs[i].node, run_id)) {
            return false;
        }
    }

    bool need_compute = false;

    if (engine_force_recompute) {
        return engine_recompute(node, true, !engine_abort_recompute);
    }

    /* If any of the inputs updated data but there is no change_handler, then
     * recompute the current node too.
     */
    for (size_t i = 0; i < node->n_inputs; i++) {
        if (node->inputs[i].node->changed) {
            need_compute = true;

            /* Trigger a recompute if we don't have a change handler. */
            if (!node->inputs[i].change_handler) {
                return engine_recompute(node, false, !engine_abort_recompute);
            }
        }
    }

    if (need_compute) {
        /* If we couldn't compute the node we either aborted or triggered
         * a full recompute. In any case, stop processing.
         */
        return engine_compute(node, !engine_abort_recompute);
    }

    VLOG_DBG("node: %s, changed: %d", node->name, node->changed);
    return true;
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
        VLOG_DBG("input node: %s, changed: %d", node->name, node->changed);
        return node->changed;
    }

    for (i = 0; i < node->n_inputs; i++) {
        if (engine_need_run(node->inputs[i].node, run_id)) {
            return true;
        }
    }

    return false;
}
