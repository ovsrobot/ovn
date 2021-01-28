/*
 * Copyright (c) 2015, 2016 Nicira, Inc.
 * Copyright (c) 2021, Red Hat, Inc.
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

#if HAVE_DECL_MALLOC_TRIM
#include <malloc.h>
#endif

#include "coverage.h"
#include "lib/ovn-sb-idl.h"
#include "lflow-cache.h"
#include "ovn/expr.h"

COVERAGE_DEFINE(lflow_cache_flush);
COVERAGE_DEFINE(lflow_cache_add_conj_id);
COVERAGE_DEFINE(lflow_cache_add_expr);
COVERAGE_DEFINE(lflow_cache_add_matches);
COVERAGE_DEFINE(lflow_cache_free_conj_id);
COVERAGE_DEFINE(lflow_cache_free_expr);
COVERAGE_DEFINE(lflow_cache_free_matches);
COVERAGE_DEFINE(lflow_cache_add);
COVERAGE_DEFINE(lflow_cache_hit);
COVERAGE_DEFINE(lflow_cache_miss);
COVERAGE_DEFINE(lflow_cache_delete);
COVERAGE_DEFINE(lflow_cache_full);

struct lflow_cache {
    struct hmap entries;
    uint32_t capacity;
    bool enabled;
};

struct lflow_cache_entry {
    struct hmap_node node;
    struct uuid lflow_uuid; /* key */

    struct lflow_cache_value value;
};

static struct lflow_cache_value *lflow_cache_add__(
    struct lflow_cache *lc,
    const struct sbrec_logical_flow *lflow,
    enum lflow_cache_type type);
static void lflow_cache_delete__(struct lflow_cache *lc,
                                 struct lflow_cache_entry *lce);

struct lflow_cache *
lflow_cache_create(void)
{
    struct lflow_cache *lc = xmalloc(sizeof *lc);

    hmap_init(&lc->entries);
    lc->enabled = true;
    return lc;
}

void
lflow_cache_flush(struct lflow_cache *lc)
{
    struct lflow_cache_entry *lce;
    struct lflow_cache_entry *lce_next;

    COVERAGE_INC(lflow_cache_flush);

    HMAP_FOR_EACH_SAFE (lce, lce_next, node, &lc->entries) {
        lflow_cache_delete__(lc, lce);
    }

    hmap_shrink(&lc->entries);

#if HAVE_DECL_MALLOC_TRIM
    malloc_trim(0);
#endif
}

void
lflow_cache_destroy(struct lflow_cache *lc)
{
    if (!lc) {
        return;
    }

    lflow_cache_flush(lc);
    hmap_destroy(&lc->entries);
    free(lc);
}

void
lflow_cache_enable(struct lflow_cache *lc, bool enabled, uint32_t capacity)
{
    if ((lc->enabled && !enabled) || capacity < hmap_count(&lc->entries)) {
        lflow_cache_flush(lc);
    }

    lc->enabled = enabled;
    lc->capacity = capacity;
}

bool
lflow_cache_is_enabled(struct lflow_cache *lc)
{
    return lc->enabled;
}

void
lflow_cache_add_conj_id(struct lflow_cache *lc,
                        const struct sbrec_logical_flow *lflow,
                        uint32_t conj_id_ofs)
{
    struct lflow_cache_value *lcv =
        lflow_cache_add__(lc, lflow, LCACHE_T_CONJ_ID);

    if (!lcv) {
        return;
    }

    COVERAGE_INC(lflow_cache_add_conj_id);
    lcv->conj_id_ofs = conj_id_ofs;
}

void
lflow_cache_add_expr(struct lflow_cache *lc,
                     const struct sbrec_logical_flow *lflow,
                     uint32_t conj_id_ofs,
                     struct expr *expr)
{
    struct lflow_cache_value *lcv =
        lflow_cache_add__(lc, lflow, LCACHE_T_EXPR);

    if (!lcv) {
        return;
    }

    COVERAGE_INC(lflow_cache_add_expr);
    lcv->conj_id_ofs = conj_id_ofs;
    lcv->expr = expr;
}

void
lflow_cache_add_matches(struct lflow_cache *lc,
                        const struct sbrec_logical_flow *lflow,
                        struct hmap *matches)
{
    struct lflow_cache_value *lcv =
        lflow_cache_add__(lc, lflow, LCACHE_T_MATCHES);

    if (!lcv) {
        return;
    }

    COVERAGE_INC(lflow_cache_add_matches);
    lcv->expr_matches = matches;
}

struct lflow_cache_value *
lflow_cache_get(struct lflow_cache *lc, const struct sbrec_logical_flow *lflow)
{
    if (!lflow_cache_is_enabled(lc)) {
        return NULL;
    }

    size_t hash = uuid_hash(&lflow->header_.uuid);
    struct lflow_cache_entry *lce;

    HMAP_FOR_EACH_WITH_HASH (lce, node, hash, &lc->entries) {
        if (uuid_equals(&lce->lflow_uuid, &lflow->header_.uuid)) {
            COVERAGE_INC(lflow_cache_hit);
            return &lce->value;
        }
    }
    COVERAGE_INC(lflow_cache_miss);
    return NULL;
}

void
lflow_cache_delete(struct lflow_cache *lc,
                   const struct sbrec_logical_flow *lflow)
{
    if (!lc) {
        return;
    }

    struct lflow_cache_value *lcv = lflow_cache_get(lc, lflow);
    if (lcv) {
        COVERAGE_INC(lflow_cache_delete);
        lflow_cache_delete__(lc, CONTAINER_OF(lcv, struct lflow_cache_entry,
                                              value));
    }
}

static struct lflow_cache_value *
lflow_cache_add__(struct lflow_cache *lc,
                  const struct sbrec_logical_flow *lflow,
                  enum lflow_cache_type type)
{
    if (hmap_count(&lc->entries) == lc->capacity) {
        COVERAGE_INC(lflow_cache_full);
        return NULL;
    }

    struct lflow_cache_entry *lce = xzalloc(sizeof *lce);

    COVERAGE_INC(lflow_cache_add);
    lce->lflow_uuid = lflow->header_.uuid;
    lce->value.type = type;
    hmap_insert(&lc->entries, &lce->node, uuid_hash(&lflow->header_.uuid));
    return &lce->value;
}

static void
lflow_cache_delete__(struct lflow_cache *lc, struct lflow_cache_entry *lce)
{
    if (!lce) {
        return;
    }

    hmap_remove(&lc->entries, &lce->node);
    switch (lce->value.type) {
    case LCACHE_T_NONE:
        OVS_NOT_REACHED();
        break;
    case LCACHE_T_CONJ_ID:
        COVERAGE_INC(lflow_cache_free_conj_id);
        break;
    case LCACHE_T_EXPR:
        COVERAGE_INC(lflow_cache_free_expr);
        expr_destroy(lce->value.expr);
        break;
    case LCACHE_T_MATCHES:
        COVERAGE_INC(lflow_cache_free_matches);
        expr_matches_destroy(lce->value.expr_matches);
        free(lce->value.expr_matches);
        break;
    }
    free(lce);
}
