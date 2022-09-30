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

#include <config.h>

#include "template-var.h"

struct template_var_table *
template_var_table_create(void)
{
    struct template_var_table *table = xmalloc(sizeof *table);

    hmap_init(&table->vars);
    return table;
}

void
template_var_table_destroy(struct template_var_table *table)
{
    struct template_var *tv;

    HMAP_FOR_EACH_POP (tv, hmap_node, &table->vars) {
        template_var_destroy(tv);
    }
    hmap_destroy(&table->vars);
}

void
template_var_insert(struct template_var_table *table,
                    const struct nbrec_template_var *nbrec_tv)
{
    struct template_var *tv = xmalloc(sizeof *tv);
    tv->nb = nbrec_tv;
    hmap_insert(&table->vars, &tv->hmap_node,
                template_var_hash(nbrec_tv->name, nbrec_tv->chassis_name));
}

struct template_var *
template_var_find(struct template_var_table *table,
                  const char *name, const char *chassis_name)
{
    struct template_var *tv;

    HMAP_FOR_EACH_WITH_HASH (tv, hmap_node,
                             template_var_hash(name, chassis_name),
                             &table->vars) {
        if (!strcmp(name, tv->nb->name) &&
                !strcmp(chassis_name, tv->nb->chassis_name)) {
            return tv;
        }
    }
    return NULL;
}

void
template_var_remove(struct template_var_table *table, struct template_var *tv)
{
    hmap_remove(&table->vars, &tv->hmap_node);
}

void
template_var_destroy(struct template_var *tv)
{
    free(tv);
}
