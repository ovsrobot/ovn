/* Copyright (c) 2021, Red Hat, Inc.
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

#include "ovn/expr.h"
#include "ovn-sb-idl.h"
#include "tests/ovstest.h"
#include "tests/test-utils.h"
#include "util.h"

#include "lflow-cache.h"

static void
test_lflow_cache_add__(struct lflow_cache *lc, const char *op_type,
                       const struct sbrec_logical_flow *lflow,
                       unsigned int conj_id_ofs,
                       struct expr *e)
{
    printf("ADD %s:\n", op_type);
    printf("  conj-id-ofs: %u\n", conj_id_ofs);

    if (!strcmp(op_type, "conj-id")) {
        lflow_cache_add_conj_id(lc, lflow, conj_id_ofs);
    } else if (!strcmp(op_type, "expr")) {
        lflow_cache_add_expr(lc, lflow, conj_id_ofs, expr_clone(e));
    } else if (!strcmp(op_type, "matches")) {
        struct hmap *matches = xmalloc(sizeof *matches);
        ovs_assert(expr_to_matches(e, NULL, NULL, matches) == 0);
        ovs_assert(hmap_count(matches) == 1);
        lflow_cache_add_matches(lc, lflow, matches);
    } else {
        OVS_NOT_REACHED();
    }
}

static void
test_lflow_cache_lookup__(struct lflow_cache *lc,
                          const struct sbrec_logical_flow *lflow)
{
    struct lflow_cache_value *lcv = lflow_cache_get(lc, lflow);

    printf("LOOKUP:\n");
    if (!lcv) {
        printf("  not found\n");
        return;
    }

    printf("  conj_id_ofs: %"PRIu32"\n", lcv->conj_id_ofs);
    switch (lcv->type) {
    case LCACHE_T_CONJ_ID:
        printf("  type: conj-id\n");
        break;
    case LCACHE_T_EXPR:
        printf("  type: expr\n");
        break;
    case LCACHE_T_MATCHES:
        printf("  type: matches\n");
        break;
    case LCACHE_T_NONE:
        OVS_NOT_REACHED();
        break;
    }
}

static void
test_lflow_cache_delete__(struct lflow_cache *lc,
                          const struct sbrec_logical_flow *lflow)
{
    printf("DELETE\n");
    lflow_cache_delete(lc, lflow);
}

static void
test_lflow_cache_stats__(struct lflow_cache *lc)
{
    struct lflow_cache_stats *lcs = lflow_cache_get_stats(lc);

    if (!lcs) {
        return;
    }
    printf("Enabled: %s\n", lflow_cache_is_enabled(lc) ? "true" : "false");
    for (size_t i = 0; i < LCACHE_T_MAX; i++) {
        printf("  %s: %"PRIuSIZE"\n", lflow_cache_type_names[i],
               lcs->n_entries[i]);
    }
    free(lcs);
}

static void
test_lflow_cache_operations(struct ovs_cmdl_context *ctx)
{
    struct lflow_cache *lc = lflow_cache_create();
    struct expr *e = expr_create_boolean(true);
    bool enabled = !strcmp(ctx->argv[1], "true");
    unsigned int shift = 2;
    unsigned int n_ops;

    lflow_cache_enable(lc, enabled, UINT32_MAX);
    test_lflow_cache_stats__(lc);

    if (!test_read_uint_value(ctx, shift++, "n_ops", &n_ops)) {
        goto done;
    }

    for (unsigned int i = 0; i < n_ops; i++) {
        const char *op = test_read_value(ctx, shift++, "op");

        if (!op) {
            goto done;
        }

        struct sbrec_logical_flow lflow;
        uuid_generate(&lflow.header_.uuid);

        if (!strcmp(op, "add")) {
            const char *op_type = test_read_value(ctx, shift++, "op_type");
            if (!op_type) {
                goto done;
            }

            unsigned int conj_id_ofs;
            if (!test_read_uint_value(ctx, shift++, "conj-id-ofs",
                                      &conj_id_ofs)) {
                goto done;
            }

            test_lflow_cache_add__(lc, op_type, &lflow, conj_id_ofs, e);
            test_lflow_cache_lookup__(lc, &lflow);
        } else if (!strcmp(op, "add-del")) {
            const char *op_type = test_read_value(ctx, shift++, "op_type");
            if (!op_type) {
                goto done;
            }

            unsigned int conj_id_ofs;
            if (!test_read_uint_value(ctx, shift++, "conj-id-ofs",
                                      &conj_id_ofs)) {
                goto done;
            }

            test_lflow_cache_add__(lc, op_type, &lflow, conj_id_ofs, e);
            test_lflow_cache_lookup__(lc, &lflow);
            test_lflow_cache_delete__(lc, &lflow);
            test_lflow_cache_lookup__(lc, &lflow);
        } else if (!strcmp(op, "enable")) {
            unsigned int limit;
            if (!test_read_uint_value(ctx, shift++, "limit", &limit)) {
                goto done;
            }
            printf("ENABLE\n");
            lflow_cache_enable(lc, true, limit);
        } else if (!strcmp(op, "disable")) {
            printf("DISABLE\n");
            lflow_cache_enable(lc, false, UINT32_MAX);
        } else if (!strcmp(op, "flush")) {
            printf("FLUSH\n");
            lflow_cache_flush(lc);
        } else {
            OVS_NOT_REACHED();
        }
        test_lflow_cache_stats__(lc);
    }
done:
    lflow_cache_destroy(lc);
    expr_destroy(e);
}

static void
test_lflow_cache_negative(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    lflow_cache_flush(NULL);
    lflow_cache_destroy(NULL);
    lflow_cache_enable(NULL, true, UINT32_MAX);
    ovs_assert(!lflow_cache_is_enabled(NULL));
    ovs_assert(!lflow_cache_get_stats(NULL));

    struct lflow_cache *lcs[] = {
        NULL,
        lflow_cache_create(),
    };

    for (size_t i = 0; i < ARRAY_SIZE(lcs); i++) {
        struct expr *e = expr_create_boolean(true);
        struct hmap *matches = xmalloc(sizeof *matches);

        ovs_assert(expr_to_matches(e, NULL, NULL, matches) == 0);
        ovs_assert(hmap_count(matches) == 1);

        lflow_cache_add_conj_id(lcs[i], NULL, 0);
        lflow_cache_add_expr(lcs[i], NULL, 0, NULL);
        lflow_cache_add_expr(lcs[i], NULL, 0, e);
        lflow_cache_add_matches(lcs[i], NULL, NULL);
        lflow_cache_add_matches(lcs[i], NULL, matches);
        lflow_cache_destroy(lcs[i]);
    }
}

static void
test_lflow_cache_main(int argc, char *argv[])
{
    set_program_name(argv[0]);
    static const struct ovs_cmdl_command commands[] = {
        {"lflow_cache_operations", NULL, 3, INT_MAX,
         test_lflow_cache_operations, OVS_RO},
        {"lflow_cache_negative", NULL, 0, 0,
         test_lflow_cache_negative, OVS_RO},
        {NULL, NULL, 0, 0, NULL, OVS_RO},
    };
    struct ovs_cmdl_context ctx;
    ctx.argc = argc - 1;
    ctx.argv = argv + 1;
    ovs_cmdl_run_command(&ctx, commands);
}

OVSTEST_REGISTER("test-lflow-cache", test_lflow_cache_main);
