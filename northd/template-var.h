/* Copyright (c) 2022, Red Hat, Inc.
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

#ifndef OVN_NORTHD_TEMPLATE_VAR_H
#define OVN_NORTHD_TEMPLATE_VAR_H 1

#include "openvswitch/hmap.h"
#include "lib/ovn-nb-idl.h"

struct template_var {
    struct hmap_node hmap_node;

    const struct nbrec_template_var *nb;
};

struct template_var_table {
    struct hmap vars;
};

#define TEMPLATE_VAR_TABLE_INITIALIZER(TBL) \
    HMAP_INITIALIZER(&(TBL)->vars)

#define TEMPLATE_VAR_TABLE_FOR_EACH(NODE, TBL) \
    HMAP_FOR_EACH (NODE, hmap_node, &(TBL)->vars)

struct template_var_table *template_var_table_create(void);
void template_var_table_destroy(struct template_var_table *table);

static inline uint32_t
template_var_hash(const char *tv_name, const char *tv_chassis)
{
    return hash_string(tv_name, hash_string(tv_chassis, 0));
}

void template_var_insert(struct template_var_table *table,
                         const struct nbrec_template_var *nbrec_tv);

struct template_var *
template_var_find(struct template_var_table *table,
                  const char *name, const char *chassis_name);

void template_var_remove(struct template_var_table *table,
                         struct template_var *tv);

void template_var_destroy(struct template_var *tv);

#endif /* OVN_NORTHD_TEMPLATE_VAR_H 1 */
