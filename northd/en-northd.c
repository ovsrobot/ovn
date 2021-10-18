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

#include "en-northd.h"
#include "lib/inc-proc-eng.h"
#include "openvswitch/list.h" /* TODO This is needed for ovn-parallel-hmap.h.
                               * lib/ovn-parallel-hmap.h should be updated
                               * to include this dependency itself */
#include "lib/ovn-parallel-hmap.h"
#include "northd.h"
#include "lib/util.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(en_northd);

void en_northd_run(struct engine_node *node, void *data)
{
    const struct engine_context *eng_ctx = engine_get_context();
    struct northd_idl_context *ctx = eng_ctx->client_ctx;
    struct northd_data *northd_data = ((struct ed_type_northd *)data)->data;

    northd_destroy(northd_data);
    northd_init(northd_data);

    northd_run(ctx, northd_data);
    engine_set_node_state(node, EN_UPDATED);

}
void *en_northd_init(struct engine_node *node OVS_UNUSED,
                     struct engine_arg *arg)
{
    struct ed_type_northd *data = xmalloc(sizeof *data);
    data->data = xmalloc(sizeof *data->data);

    data->data->use_parallel_build = can_parallelize_hashes(false);
    northd_indices_create(data->data, arg->sb_idl);
    northd_init(data->data);

    return data;
}

void en_northd_cleanup(void *data OVS_UNUSED)
{
    struct northd_data *northd_data = ((struct ed_type_northd *)data)->data;

    northd_destroy(northd_data);
    free(northd_data);
}
