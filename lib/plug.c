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

VLOG_DEFINE_THIS_MODULE(plug);

struct registered_plug_class {
    const struct plug_class *plug_class;
    int refcount;
};
static struct shash plug_classes = SHASH_INITIALIZER(&plug_classes);
static struct shash plug_instances = SHASH_INITIALIZER(&plug_instances);

/* Protects 'plug_classes', including the refcount. */
static struct ovs_mutex plug_classes_mutex = OVS_MUTEX_INITIALIZER;
/* Protects 'plug_instances' */
static struct ovs_mutex plug_instances_mutex = OVS_MUTEX_INITIALIZER;

/* Initialize the the plug infrastructure by registering known plug classes */
static void
plug_initialize(void)
{
    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;

    if (ovsthread_once_start(&once)) {
#ifdef HAVE_PLUG_PROVIDER
        for (int i = 0; i < ARRAY_SIZE(plug_provider_classes); i++) {
            plug_register_provider(plug_provider_classes[i]);
        }
#endif
        ovsthread_once_done(&once);
    }
}

static int
plug_register_provider__(const struct plug_class *new_class)
{
    struct registered_plug_class *rc;
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

    rc = xmalloc(sizeof *rc);
    rc->plug_class = new_class;
    rc->refcount = 0;

    shash_add(&plug_classes, new_class->type, rc);

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
    struct registered_plug_class *rc;

    node = shash_find(&plug_classes, type);
    if (!node) {
        return EINVAL;
    }

    rc = node->data;
    if (rc->refcount) {
        VLOG_WARN("attempted to unregister in use plug provider: %s", type);
        return EBUSY;
    }

    error = rc->plug_class->destroy ? rc->plug_class->destroy() : 0;
    if (error) {
        VLOG_WARN("failed to destroy %s plug class: %s",
                  rc->plug_class->type, ovs_strerror(error));
        return error;
    }

    shash_delete(&plug_classes, node);
    free(rc);

    return 0;
}

/* Unregister the plug provider identified by 'type' and perform any class
 * level de-initialization as specified in its plug_class. */
int
plug_unregister_provider(const char *type)
{
    int error;

    plug_initialize();

    ovs_mutex_lock(&plug_classes_mutex);
    error = plug_unregister_provider__(type);
    ovs_mutex_unlock(&plug_classes_mutex);

    return error;
}

static void
plug_class_unref(struct registered_plug_class *rc)
{
    ovs_mutex_lock(&plug_classes_mutex);
    ovs_assert(rc->refcount);
    rc->refcount--;
    ovs_mutex_unlock(&plug_classes_mutex);
}

static struct registered_plug_class *
plug_class_lookup(const char *type)
{
    struct registered_plug_class *rc;

    ovs_mutex_lock(&plug_classes_mutex);
    rc = shash_find_data(&plug_classes, type);
    if (rc) {
        rc->refcount++;
    }
    ovs_mutex_unlock(&plug_classes_mutex);

    return rc;
}

static int
plug_open__(const char *type, struct plug **plugp)
{
    struct plug *plug = NULL;
    int error;
    struct registered_plug_class *rc;

    plug_initialize();
    rc = plug_class_lookup(type);
    if (!rc) {
        VLOG_WARN("unable to open plug provider of unknown type: %s", type);
        error = EINVAL;
        goto out;
    }

    error = rc->plug_class->open(rc->plug_class, &plug);
    if (error) {
        plug_class_unref(rc);
    }

out:
    *plugp = error ? NULL: plug;
    return error;
}

/* Create, or retrieve the already created instance of plug class from a
 * previous call to plug_open, identified by 'type' and store a reference to it
 * in '*plugp'.
 *
 * The plug implementation will perform any initialization and allocations it
 * needs, and the plug infrastructure will store a reference to it.  Subsequent
 * calls to this function with the same 'type' parameter will return the same
 * object, until the instance is removed with a call to plug_close. */
int
plug_open(const char *type, struct plug **plugp)
{
    struct plug *instance = shash_find_data(&plug_instances, type);
    int error;

    if (instance) {
        *plugp = instance;
        return 0;
    }

    error = plug_open__(type, plugp);
    if (error) {
        return error;
    }

    ovs_mutex_lock(&plug_instances_mutex);
    shash_add(&plug_instances, type, *plugp);
    ovs_mutex_unlock(&plug_instances_mutex);

    return 0;
}

/* Close the plug class instance previously created by a call to 'plug_open'.
 *
 * The plug implementation will perform any destruction of its data and the
 * plug infrastructure will remove its references to it. */
void
plug_close(struct plug *plug)
{
    if (plug) {
        ovs_mutex_lock(&plug_instances_mutex);
        shash_find_and_delete(&plug_instances, plug->plug_class->type);
        ovs_mutex_unlock(&plug_instances_mutex);

        struct registered_plug_class *rc;
        rc = shash_find_data(&plug_classes, plug->plug_class->type);
        rc->plug_class->close(plug);
        plug_class_unref(rc);
    }
}

/* Close any previously instantiated plug classes and unregister the plug
 * providers. */
void
plug_destroy_all(void)
{
    struct shash_node *node, *next;

    SHASH_FOR_EACH_SAFE(node, next, &plug_instances) {
        struct plug *plug = node->data;
        plug_close(plug);
    }

    SHASH_FOR_EACH_SAFE(node, next, &plug_classes) {
        struct registered_plug_class *rc = node->data;
        plug_unregister_provider(rc->plug_class->type);
    }
}

/* Iterate over previously instantiated plug classes and call their 'run'
 * function if defined.
 *
 * If any of the instances report they have changed something this function
 * will return 'true', otherwise it will return 'false'. */
bool
plug_run_instances(void)
{
    struct shash_node *node;
    bool something_changed = false;

    ovs_mutex_lock(&plug_instances_mutex);

    SHASH_FOR_EACH(node, &plug_instances) {
        struct plug *instance = node->data;
        if (instance->plug_class->run(instance)) {
            something_changed = true;
        }
    }

    ovs_mutex_unlock(&plug_instances_mutex);

    return something_changed;
}

/* Get the class level 'maintained_iface_options' set. */
struct sset *
plug_class_get_maintained_iface_options(const struct plug *plug)
{
    return plug->plug_class->maintained_iface_options;
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
plug_port_prepare(const struct plug *plug,
                  const struct plug_port_ctx_in *ctx_in,
                  struct plug_port_ctx_out *ctx_out)
{
    if (ctx_out) {
        memset(ctx_out, 0, sizeof(*ctx_out));
    }
    return plug->plug_class->plug_port_prepare(ctx_in, ctx_out);
}

/* Notify the plug implementation that a port creation, update or removal has
 * been completed */
void
plug_port_finish(const struct plug *plug,
                 const struct plug_port_ctx_in *ctx_in,
                 struct plug_port_ctx_out *ctx_out)
{
    plug->plug_class->plug_port_finish(ctx_in, ctx_out);
}

/* Free any data allocated to 'ctx_out' in a prevous call to
 * plug_port_prepare. */
void
plug_port_ctx_destroy(const struct plug *plug,
                      const struct plug_port_ctx_in *ctx_in,
                      struct plug_port_ctx_out *ctx_out)
{
    plug->plug_class->plug_port_ctx_destroy(ctx_in, ctx_out);
}
