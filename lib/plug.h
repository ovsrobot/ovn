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

#ifndef PLUG_H
#define PLUG_H 1

/*
 * Plug, the plugging interface.  This module contains the infrastructure for
 * registering and instantiating plugging classes which may be hosted inside
 * or outside the core OVN repository.  The data structures and functions for
 * interacting with these plugging classes also live here.
 */

#include "smap.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct plug;
struct plug_class;
struct ovsdb_idl_txn;
struct ovsrec_bridge;

enum plug_op_type {
    PLUG_OP_CREATE = 1, /* Port is created or updated */
    PLUG_OP_REMOVE,     /* Port is removed from this chassis */
};

struct plug_port_ctx_in {
    /* Operation being performed */
    enum plug_op_type op_type;

    /* Whether the chassis uses DPDK */
    bool use_dpdk;

    /* Name of logical port, can be useful for plugging library to track any
     * per port resource initialization. */
    const char *lport_name;

    /* Logical port options, while OVN will forward the contents verbatim from
     * the Southbound database, the convention is for the plugging library to
     * only make decisions based on the plug-* options. */
    const struct smap *lport_options;

    /* When OVN knows about an existing interface record associated with this
     * lport, these will be filled in with information about it. */
    const char *iface_name;
    const char *iface_type;
    const struct smap *iface_options;
};

struct plug_port_ctx_out {
    /* The name to use for port and interface record. */
    char *name;

    /* Type of interface to create. */
    char *type;

    /* Options to set on the interface record. */
    struct smap *iface_options;
};


int plug_register_provider(const struct plug_class *);
int plug_unregister_provider(const char *type);
void plug_destroy_all(void);
int plug_open(const char *type, struct plug **);
void plug_close(struct plug *);
bool plug_run_instances(void);

struct sset * plug_class_get_maintained_iface_options(const struct plug *);

bool plug_port_prepare(const struct plug *,
                       const struct plug_port_ctx_in *,
                       struct plug_port_ctx_out *);
void plug_port_finish(const struct plug *,
                      const struct plug_port_ctx_in *,
                      struct plug_port_ctx_out *);
void plug_port_ctx_destroy(const struct plug *,
                           const struct plug_port_ctx_in *,
                           struct plug_port_ctx_out *);

#ifdef  __cplusplus
}
#endif

#endif /* plug.h */
