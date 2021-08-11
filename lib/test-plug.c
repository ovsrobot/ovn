/* Copyright (c) 2021, Canonical
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

#include "plug.h"
#include "plug-dummy.h"
#include "plug-provider.h"
#include "smap.h"
#include "sset.h"
#include "tests/ovstest.h"

static void
test_plug(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct plug *plug;

    ovs_assert(plug_unregister_provider("dummy") == EINVAL);
    ovs_assert(plug_open("dummy", &plug) == EINVAL);

    ovs_assert(!plug_register_provider(&plug_dummy_class));
    ovs_assert(plug_register_provider(&plug_dummy_class) == EEXIST);
    ovs_assert(!plug_run_instances());

    ovs_assert(!plug_open("dummy", &plug));
    ovs_assert(plug_unregister_provider("dummy") == EBUSY);

    ovs_assert(sset_contains(
            plug_class_get_maintained_iface_options(plug),
            "plug-dummy-option"));
    ovs_assert(plug_run_instances());

    struct smap fake_lport_options = SMAP_INITIALIZER(&fake_lport_options);
    struct plug_port_ctx_in ctx_in = {
        .op_type = PLUG_OP_CREATE,
        .use_dpdk = false,
        .lport_name = "lsp1",
        .lport_options = &fake_lport_options,
    };
    struct plug_port_ctx_out ctx_out;
    plug_port_prepare(plug, &ctx_in, &ctx_out);
    ovs_assert(!strcmp(ctx_out.name, "lsp1"));
    ovs_assert(!strcmp(ctx_out.type, "internal"));
    ovs_assert(!strcmp(smap_get(
            ctx_out.iface_options, "plug-dummy-option"), "value"));

    plug_port_finish(plug, &ctx_in, &ctx_out);
    plug_port_ctx_destroy(plug, &ctx_in, &ctx_out);
    plug_close(plug);
    plug_destroy_all();
}

static void
test_plug_main(int argc, char *argv[])
{
    set_program_name(argv[0]);
    static const struct ovs_cmdl_command commands[] = {
        {"run", NULL, 0, 0, test_plug, OVS_RO},
        {NULL, NULL, 0, 0, NULL, OVS_RO},
    };
    struct ovs_cmdl_context ctx;
    ctx.argc = argc - 1;
    ctx.argv = argv + 1;
    ovs_cmdl_run_command(&ctx, commands);
}

OVSTEST_REGISTER("test-plug", test_plug_main);
