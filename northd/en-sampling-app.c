/*
 * Copyright (c) 2024, Red Hat, Inc.
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

#include "openvswitch/vlog.h"

#include "en-sampling-app.h"

VLOG_DEFINE_THIS_MODULE(en_sampling_app);

/* Static function declarations. */
static void sampling_app_table_add(struct sampling_app_table *,
                                   const struct nbrec_sampling_app *);
static uint8_t sampling_app_table_get_id(const struct sampling_app_table *,
                                         enum sampling_app_type);
static void sampling_app_table_reset(struct sampling_app_table *);
static enum sampling_app_type sampling_app_get_by_name(const char *app_name);

void *
en_sampling_app_init(struct engine_node *node OVS_UNUSED,
                     struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_sampling_app_data *data = xzalloc(sizeof *data);
    sampling_app_table_reset(&data->apps);
    return data;
}

void
en_sampling_app_cleanup(void *data OVS_UNUSED)
{
}

void
en_sampling_app_run(struct engine_node *node, void *data_)
{
    const struct nbrec_sampling_app_table *nb_sampling_app_table =
        EN_OVSDB_GET(engine_get_input("NB_sampling_app", node));
    struct ed_type_sampling_app_data *data = data_;

    sampling_app_table_reset(&data->apps);

    const struct nbrec_sampling_app *sa;
    NBREC_SAMPLING_APP_TABLE_FOR_EACH (sa, nb_sampling_app_table) {
        sampling_app_table_add(&data->apps, sa);
    }

    engine_set_node_state(node, EN_UPDATED);
}

uint8_t
sampling_app_get_id(const struct sampling_app_table *app_table,
                    enum sampling_app_type app_type)
{
    return sampling_app_table_get_id(app_table, app_type);
}

/* Static functions. */
static
void sampling_app_table_add(struct sampling_app_table *table,
                            const struct nbrec_sampling_app *sa)
{
    enum sampling_app_type app_type = sampling_app_get_by_name(sa->name);

    if (app_type == SAMPLING_APP_MAX) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "Unexpected Sampling_App name: %s", sa->name);
        return;
    }
    table->app_ids[app_type] = sa->id;
}

static
uint8_t sampling_app_table_get_id(const struct sampling_app_table *table,
                                  enum sampling_app_type app_type)
{
    ovs_assert(app_type < SAMPLING_APP_MAX);
    return table->app_ids[app_type];
}

static
void sampling_app_table_reset(struct sampling_app_table *table)
{
    for (size_t i = 0; i < SAMPLING_APP_MAX; i++) {
        table->app_ids[i] = SAMPLING_APP_ID_NONE;
    }
}

static const char *app_names[] = {
    [SAMPLING_APP_DROP_DEBUG] =
        "drop-sampling",
    [SAMPLING_APP_ACL_NEW_TRAFFIC] =
        "acl-new-traffic-sampling",
    [SAMPLING_APP_ACL_EST_TRAFFIC] =
        "acl-est-traffic-sampling",
};

static enum sampling_app_type
sampling_app_get_by_name(const char *app_name)
{
    for (size_t app_type = 0; app_type < ARRAY_SIZE(app_names); app_type++) {
        if (!strcmp(app_name, app_names[app_type])) {
            return app_type;
        }
    }
    return SAMPLING_APP_MAX;
}
