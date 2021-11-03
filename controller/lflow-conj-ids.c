/*
 * Copyright (c) 2021, NVIDIA CORPORATION.  All rights reserved.
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
#include "coverage.h"
#include "lflow-conj-ids.h"
#include "util.h"

COVERAGE_DEFINE(lflow_conj_conflict);

/* Insert n_conjs conjuntion ids starting from start_conj_id into the conj_ids,
 * assuming the ids are confirmed to be available. */
static void
lflow_conj_ids_insert_(struct conj_ids *conj_ids,
                       const struct uuid *lflow_uuid,
                       uint32_t start_conj_id, uint32_t n_conjs)
{
    ovs_assert(n_conjs);
    uint32_t conj_id = start_conj_id;
    for (uint32_t i = 0; i < n_conjs; i++) {
        ovs_assert(conj_id);
        struct conj_id_node *node = xzalloc(sizeof *node);
        node->conj_id = conj_id;
        hmap_insert(&conj_ids->conj_id_allocations, &node->hmap_node, conj_id);
        conj_id++;
    }

    struct lflow_conj_node *lflow_conj = xzalloc(sizeof *lflow_conj);
    lflow_conj->lflow_uuid = *lflow_uuid;
    lflow_conj->start_conj_id = start_conj_id;
    lflow_conj->n_conjs = n_conjs;
    hmap_insert(&conj_ids->lflow_conj_ids, &lflow_conj->hmap_node,
                uuid_hash(lflow_uuid));
}

/* Allocate n_conjs continuous conjuction ids from the conj_ids for the given
 * lflow_uuid. (0 is never included in an allocated range)
 *
 * The first conjunction id is returned. If no conjunction ids available, or if
 * the input is invalid (n_conjs == 0), then 0 is returned.
 *
 * The algorithm tries to allocate the parts[0] of the input uuid as the first
 * conjunction id. If it is unavailable, or any of the subsequent n_conjs - 1
 * ids are unavailable, iterate until the next available n_conjs ids are found.
 * Given that n_conjs is very small (in most cases will be 1), the algorithm
 * should be efficient enough and in most cases just return the lflow_uuid's
 * part[0], which ensures conjunction ids are consistent for the same logical
 * flow in most cases.
 *
 * The performance will degrade if most of the uint32_t are allocated because
 * conflicts will happen a lot. In practice this is not expected to happen in
 * reasonalbe scales. Otherwise, if the amount of logical flows is close to
 * this (4G logical flows that need conjunction ids) there are other parts of
 * the system expected to be suffered even before reaching to a scale much
 * smaller than this. */
uint32_t
lflow_conj_ids_alloc(struct conj_ids *conj_ids, const struct uuid *lflow_uuid,
                     uint32_t n_conjs)
{
    if (!n_conjs) {
        return 0;
    }
    uint32_t start_conj_id = lflow_uuid->parts[0];
    uint32_t initial_id = start_conj_id;
    bool initial = true;
    while (true) {
        if (start_conj_id == 0) {
            start_conj_id++;
        }
        bool available = true;
        uint32_t conj_id = start_conj_id;
        for (uint32_t i = 0; i < n_conjs; i++) {
            if (conj_id == 0) {
                /* Overflow. Consider the current range as unavailable because
                 * we need a continuous range. Start over from 1 (0 is
                 * skipped). */
                available = false;
                break;
            }
            if (!initial && conj_id == initial_id) {
                /* It has checked all ids (extreme situation, not expected in
                 * real environment). */
                return 0;
            }
            initial = false;
            struct conj_id_node *conj_id_node;
            /* conj_id is both the key and the hash */
            HMAP_FOR_EACH_WITH_HASH (conj_id_node, hmap_node, conj_id,
                                     &conj_ids->conj_id_allocations) {
                if (conj_id_node->conj_id == conj_id) {
                    available = false;
                    COVERAGE_INC(lflow_conj_conflict);
                    break;
                }
            }
            if (!available) {
                break;
            }
            conj_id++;
        }
        if (available) {
            break;
        }
        start_conj_id = conj_id + 1;
    }
    lflow_conj_ids_insert_(conj_ids, lflow_uuid, start_conj_id, n_conjs);
    return start_conj_id;
}

/* Similar to lflow_conj_ids_alloc, except that it takes an extra parameter
 * start_conj_id, which specifies the desired conjunction ids to be allocated,
 * and if they are unavailable, return false directly without trying to find
 * the next available ones. It returns true if the specified range is
 * allocated successfully. */
bool
lflow_conj_ids_alloc_specified(struct conj_ids *conj_ids,
                               const struct uuid *lflow_uuid,
                               uint32_t start_conj_id, uint32_t n_conjs)
{
    if (!n_conjs) {
        return false;
    }
    uint32_t conj_id = start_conj_id;
    for (uint32_t i = 0; i < n_conjs; i++) {
        ovs_assert(conj_id);
        struct conj_id_node *conj_id_node;
        HMAP_FOR_EACH_WITH_HASH (conj_id_node, hmap_node, conj_id,
                                 &conj_ids->conj_id_allocations) {
            if (conj_id_node->conj_id == conj_id) {
                return false;
            }
        }
        conj_id++;
    }
    lflow_conj_ids_insert_(conj_ids, lflow_uuid, start_conj_id, n_conjs);
    return true;
}

/* Frees the conjunction IDs used by lflow_uuid. */
void
lflow_conj_ids_free(struct conj_ids *conj_ids, const struct uuid *lflow_uuid)
{
    struct lflow_conj_node *lflow_conj;
    bool found = false;
    HMAP_FOR_EACH_WITH_HASH (lflow_conj, hmap_node, uuid_hash(lflow_uuid),
                             &conj_ids->lflow_conj_ids) {
        if (uuid_equals(&lflow_conj->lflow_uuid, lflow_uuid)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    ovs_assert(lflow_conj->n_conjs);
    uint32_t conj_id = lflow_conj->start_conj_id;
    for (uint32_t i = 0; i < lflow_conj->n_conjs; i++) {
        ovs_assert(conj_id);
        struct conj_id_node *conj_id_node;
        HMAP_FOR_EACH_WITH_HASH (conj_id_node, hmap_node, conj_id,
                                 &conj_ids->conj_id_allocations) {
            if (conj_id_node->conj_id == conj_id) {
                hmap_remove(&conj_ids->conj_id_allocations,
                            &conj_id_node->hmap_node);
                free(conj_id_node);
                break;
            }
        }
        conj_id++;
    }

    hmap_remove(&conj_ids->lflow_conj_ids, &lflow_conj->hmap_node);
    free(lflow_conj);
}

void
lflow_conj_ids_init(struct conj_ids *conj_ids)
{
    hmap_init(&conj_ids->conj_id_allocations);
    hmap_init(&conj_ids->lflow_conj_ids);
}

void
lflow_conj_ids_destroy(struct conj_ids *conj_ids) {
    struct conj_id_node *conj_id_node, *next;
    HMAP_FOR_EACH_SAFE (conj_id_node, next, hmap_node,
                        &conj_ids->conj_id_allocations) {
        hmap_remove(&conj_ids->conj_id_allocations, &conj_id_node->hmap_node);
        free(conj_id_node);
    }
    hmap_destroy(&conj_ids->conj_id_allocations);

    struct lflow_conj_node *lflow_conj, *l_c_next;
    HMAP_FOR_EACH_SAFE (lflow_conj, l_c_next, hmap_node,
                        &conj_ids->lflow_conj_ids) {
        hmap_remove(&conj_ids->lflow_conj_ids, &lflow_conj->hmap_node);
        free(lflow_conj);
    }
    hmap_destroy(&conj_ids->lflow_conj_ids);
}

void lflow_conj_ids_clear(struct conj_ids *conj_ids) {
    lflow_conj_ids_destroy(conj_ids);
    lflow_conj_ids_init(conj_ids);
}
