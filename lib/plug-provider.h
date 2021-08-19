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

#ifndef PLUG_PROVIDER_H
#define PLUG_PROVIDER_H 1

/* Provider interface to pluggers.  A plugger implementation performs lookup
 * and/or initialization of ports, typically representor ports, using generic
 * non-blocking hardware interfaces.  This allows the ovn-controller to, upon
 * the CMS's request, create ports and interfaces in the chassis's Open vSwitch
 * instances (also known as vif plugging).
 */

#include <stdbool.h>

#include "plug.h"

#ifdef __cplusplus
extern "C" {
#endif

struct plug {
    const struct plug_class *plug_class;
};

struct plug_class {
    /* Type of plugger in this class. */
    const char *type;

    /* Interface options this plugger will maintain.  This set is used
     * to know which items to remove when maintaining the database record. */
    struct sset *maintained_iface_options;

    /* Called when the plug provider is registered, typically at program
     * startup.
     *
     * This function may be set to null if a plug class needs no
     * initialization at registration time. */
    int (*init)(void);

    /* Called when the plug provider is unregistered, typically at program
     * exit.
     *
     * This function may be set to null if a plug class needs no
     * de-initialization at unregister time.*/
    int (*destroy)(void);

    /* Creates a new plug class instance.
     *
     * If successful, stores a pointer to the plug instance in '*plugp' */
    int (*open)(const struct plug_class *class, struct plug **plugp);

    /* Closes plug class instance and frees associated memory. */
    int (*close)(struct plug *plug);

    /* Performs periodic work needed by plugger, if any is necessary.  Returns
     * true if something changed, false otherwise.
     *
     * Note that work performed by plugger in this function must under no
     * circumstances block. */
    bool (*run)(struct plug *plug);

    /* Pass plug_port_ctx_in to plug implementation to prepare for port
     * creation/update.
     *
     * The plug implemantation can perform lookup or any per port
     * initialization and should fill plug_port_ctx_out with data required for
     * port/interface creation.  The plug implementation should return true if
     * it wants the caller to create/update a port/interface, false otherwise.
     *
     * Data in the plug_port_ctx_out struct is owned by the plugging library,
     * and a call must be made to the plug_port_ctx_destroy callback to free
     * up any allocations when done with port creation/update.
     */
    bool (*plug_port_prepare)(const struct plug_port_ctx_in *,
                              struct plug_port_ctx_out *);

    /* Notify plugging library that port update is done. */
    void (*plug_port_finish)(const struct plug_port_ctx_in *,
                             struct plug_port_ctx_out *);

    /* Free any allocations made by the plug_port_prepare callback. */
    void (*plug_port_ctx_destroy)(const struct plug_port_ctx_in *,
                                  struct plug_port_ctx_out *);
};

extern const struct plug_class plug_dummy_class;
#ifdef HAVE_PLUG_PROVIDER
extern const struct plug_class *plug_provider_classes[];
#endif

#ifdef  __cplusplus
}
#endif

#endif /* plug-provider.h */
