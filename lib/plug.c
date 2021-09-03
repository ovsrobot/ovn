/*
 * Copyright (c) 2021 Canonical
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
#include "plug-provider.h"
#include "plug.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "openvswitch/vlog.h"
#include "openvswitch/shash.h"
#include "smap.h"
#include "sset.h"
#include "lib/inc-proc-eng.h"

VLOG_DEFINE_THIS_MODULE(plug);

#ifdef ENABLE_PLUG
static const struct plug_class *base_plug_classes[] = {
};
#endif

static struct shash plug_classes = SHASH_INITIALIZER(&plug_classes);

/* Protects the 'plug_classes' shash. */
static struct ovs_mutex plug_classes_mutex = OVS_MUTEX_INITIALIZER;

/* Initialize the the plug infrastructure by registering known plug classes */
void
plug_initialize(void)
{
    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;

    if (ovsthread_once_start(&once)) {
#ifdef ENABLE_PLUG
        /* Register built-in plug provider classes */
        for (int i = 0; i < ARRAY_SIZE(base_plug_classes); i++) {
            plug_register_provider(base_plug_classes[i]);
        }
#endif
#ifdef HAVE_PLUG_PROVIDER
        /* Register external plug provider classes.
         *
         * Note that we cannot use the ARRAY_SIZE macro here as
         * plug_provider_classes is defined in external code which is not
         * available at compile time.  The convention is to use a
         * NULL-terminated array instead. */
        for (const struct plug_class **pp = plug_provider_classes;
             pp && *pp;
             pp++)
        {
            plug_register_provider(*pp);
        }
#endif
        ovsthread_once_done(&once);
    }
}

static int
plug_register_provider__(const struct plug_class *new_class)
{
    struct plug_class *plug_class;
    int error;

    if (shash_find(&plug_classes, new_class->type)) {
        VLOG_WARN("attempted to register duplicate plug provider: %s",
                  new_class->type);
        return EEXIST;
    }

    error = new_class->init ? new_class->init() : 0;
    if (error) {
        VLOG_WARN("failed to initialize %s plug class: %s",
                  new_class->type, ovs_strerror(error));
        return error;
    }

    plug_class = xmalloc(sizeof *plug_class);
    memcpy(plug_class, new_class, sizeof *plug_class);

    shash_add(&plug_classes, new_class->type, plug_class);

    return 0;
}

/* Register the new plug provider referred to in 'new_class' and perform any
 * class level initialization as specified in its plug_class. */
int
plug_register_provider(const struct plug_class *new_class)
{
    int error;

    ovs_mutex_lock(&plug_classes_mutex);
    error = plug_register_provider__(new_class);
    ovs_mutex_unlock(&plug_classes_mutex);

    return error;
}

static int
plug_unregister_provider__(const char *type)
{
    int error;
    struct shash_node *node;
    struct plug_class *plug_class;

    node = shash_find(&plug_classes, type);
    if (!node) {
        return EINVAL;
    }

    plug_class = node->data;
    error = plug_class->destroy ? plug_class->destroy() : 0;
    if (error) {
        VLOG_WARN("failed to destroy %s plug class: %s",
                  plug_class->type, ovs_strerror(error));
        return error;
    }

    shash_delete(&plug_classes, node);
    free(plug_class);

    return 0;
}

/* Unregister the plug provider identified by 'type' and perform any class
 * level de-initialization as specified in its plug_class. */
int
plug_unregister_provider(const char *type)
{
    int error;

    ovs_mutex_lock(&plug_classes_mutex);
    error = plug_unregister_provider__(type);
    ovs_mutex_unlock(&plug_classes_mutex);

    return error;
}

const struct plug_class *
plug_get_provider(const char *type)
{
    struct plug_class *plug_class;

    ovs_mutex_lock(&plug_classes_mutex);
    plug_class = shash_find_data(&plug_classes, type);
    ovs_mutex_unlock(&plug_classes_mutex);

    return plug_class;
}

/* De-initialize and unregister the plug provider classes. */
void
plug_destroy_all(void)
{
    struct shash_node *node, *next;

    SHASH_FOR_EACH_SAFE (node, next, &plug_classes) {
        struct plug_class *plug_class = node->data;
        plug_unregister_provider(plug_class->type);
    }
}

/* Get the class level 'maintained_iface_options' set. */
const struct sset *
plug_get_maintained_iface_options(const struct plug_class *plug_class)
{
    return plug_class->plug_get_maintained_iface_options();
}

/* Prepare the logical port as identified by 'ctx_in' for port creation, update
 * or removal as specified by 'ctx_in->op_type'.
 *
 * When 'ctx_in->op_type' is PLUG_OP_CREATE the plug implementation must fill
 * 'ctx_out' with data to apply to the interface record maintained by OVN on
 * its behalf.
 *
 * When 'ctx_in_op_type' is PLUG_OP_REMOVE 'ctx_out' should be set to NULL and
 * the plug implementation must not attempt to use 'ctx_out'.
 *
 * The data in 'ctx_out' is owned by the plug implementation, and a call must
 * be made to plug_port_ctx_destroy when done with it. */
bool
plug_port_prepare(const struct plug_class *plug_class,
                  const struct plug_port_ctx_in *ctx_in,
                  struct plug_port_ctx_out *ctx_out)
{
    if (ctx_out) {
        memset(ctx_out, 0, sizeof(*ctx_out));
    }
    return plug_class->plug_port_prepare(ctx_in, ctx_out);
}

/* Notify the plug implementation that a port creation, update or removal has
 * been completed */
void
plug_port_finish(const struct plug_class *plug_class,
                 const struct plug_port_ctx_in *ctx_in,
                 struct plug_port_ctx_out *ctx_out)
{
    plug_class->plug_port_finish(ctx_in, ctx_out);
}

/* Free any data allocated to 'ctx_out' in a prevous call to
 * plug_port_prepare. */
void
plug_port_ctx_destroy(const struct plug_class *plug_class,
                      const struct plug_port_ctx_in *ctx_in,
                      struct plug_port_ctx_out *ctx_out)
{
    plug_class->plug_port_ctx_destroy(ctx_in, ctx_out);
}

/* Iterate over registered plug provider classes and call their 'run'
 * function if defined.
 *
 * If any of the classes report that something has changed we will trigger a
 * recompute. */
void
en_plug_provider_run(struct engine_node *inc_eng_node,
                     void *inc_eng_data OVS_UNUSED)
{
    struct shash_node *node, *next;
    bool handled = true;

    SHASH_FOR_EACH_SAFE (node, next, &plug_classes) {
        struct plug_class *plug_class = node->data;
        if (plug_class->run && !plug_class->run(plug_class)) {
            handled = false;
        }
    }

    if (!handled) {
        /* as we do not have a change handler registered in the incremental
         * processing engine this will trigger a recompute. */
        engine_set_node_state(inc_eng_node, EN_UPDATED);
    } else {
        engine_set_node_state(inc_eng_node, EN_UNCHANGED);
    }
}
