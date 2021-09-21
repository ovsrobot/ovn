/*
 * Copyright (c) 2020 Red Hat, Inc.
 * Copyright (c) 2008, 2009, 2010, 2012, 2013, 2015, 2019 Nicira, Inc.
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
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>
#include "fatal-signal.h"
#include "util.h"
#include "openvswitch/vlog.h"
#include "openvswitch/hmap.h"
#include "openvswitch/thread.h"
#include "ovn-parallel-hmap.h"
#include "ovs-atomic.h"
#include "ovs-thread.h"
#include "ovs-numa.h"
#include "random.h"

VLOG_DEFINE_THIS_MODULE(ovn_parallel_hmap);

#ifndef OVS_HAS_PARALLEL_HMAP

#define WORKER_SEM_NAME "%x-%p-%x"
#define MAIN_SEM_NAME "%x-%p-main"

/* These are accessed under mutex inside add_worker_pool().
 * They do not need to be atomic.
 */
static atomic_bool initial_pool_setup = ATOMIC_VAR_INIT(false);
static bool can_parallelize = false;

/* This is set only in the process of exit and the set is
 * accompanied by a fence. It does not need to be atomic or be
 * accessed under a lock.
 */

static struct ovs_list worker_pools = OVS_LIST_INITIALIZER(&worker_pools);

static struct ovs_mutex init_mutex = OVS_MUTEX_INITIALIZER;

static int pool_size;

static int sembase;

static void worker_pool_hook(void *aux OVS_UNUSED);
static void setup_worker_pools(bool force);
static void merge_list_results(struct worker_pool *pool OVS_UNUSED,
                               void *fin_result, void *result_frags,
                               int index);
static void merge_hash_results(struct worker_pool *pool OVS_UNUSED,
                               void *fin_result, void *result_frags,
                               int index);


static bool init_control(struct worker_control *control, int id,
                         struct worker_pool *pool);

static void cleanup_control(struct worker_pool *pool, int id);

static void free_controls(struct worker_pool *pool);

static struct worker_control *alloc_controls(int size);

static void *standard_helper_thread(void *arg);

struct worker_pool *ovn_add_standard_pool(int size)
{
    return add_worker_pool(standard_helper_thread, size);
}

bool
ovn_stop_parallel_processing(struct worker_pool *pool)
{
    return pool->workers_must_exit;
}

bool
ovn_can_parallelize_hashes(bool force_parallel)
{
    bool test = false;

    if (atomic_compare_exchange_strong(
            &initial_pool_setup,
            &test,
            true)) {
        ovs_mutex_lock(&init_mutex);
        setup_worker_pools(force_parallel);
        ovs_mutex_unlock(&init_mutex);
    }
    return can_parallelize;
}


void
destroy_pool(struct worker_pool *pool) {
    char sem_name[256];

    free_controls(pool);
    sem_close(pool->done);
    sprintf(sem_name, MAIN_SEM_NAME, sembase, pool);
    sem_unlink(sem_name);
    free(pool);
}

bool
ovn_resize_pool(struct worker_pool *pool, int size)
{
    int i;

    ovs_assert(pool != NULL);

    if (!size) {
        size = pool_size;
    }

    ovs_mutex_lock(&init_mutex);

    if (can_parallelize) {
        free_controls(pool);
        pool->size = size;

        /* Allocate new control structures. */

        pool->controls = alloc_controls(size);
        pool->workers_must_exit = false;

        for (i = 0; i < pool->size; i++) {
            if (! init_control(&pool->controls[i], i, pool)) {
                goto cleanup;
            }
        }
    }
    ovs_mutex_unlock(&init_mutex);
    return true;
cleanup:

    /* Something went wrong when opening semaphores. In this case
     * it is better to shut off parallel procesing altogether
     */

    VLOG_INFO("Failed to initialize parallel processing, error %d", errno);
    can_parallelize = false;
    free_controls(pool);

    ovs_mutex_unlock(&init_mutex);
    return false;
}


struct worker_pool *
ovn_add_worker_pool(void *(*start)(void *), int size)
{
    struct worker_pool *new_pool = NULL;
    bool test = false;
    int i;
    char sem_name[256];

    /* Belt and braces - initialize the pool system just in case if
     * if it is not yet initialized.
     */
    if (atomic_compare_exchange_strong(
            &initial_pool_setup,
            &test,
            true)) {
        ovs_mutex_lock(&init_mutex);
        setup_worker_pools(false);
        ovs_mutex_unlock(&init_mutex);
    }

    if (!size) {
        size = pool_size;
    }

    ovs_mutex_lock(&init_mutex);
    if (can_parallelize) {
        new_pool = xmalloc(sizeof(struct worker_pool));
        new_pool->size = size;
        new_pool->start = start;
        sprintf(sem_name, MAIN_SEM_NAME, sembase, new_pool);
        new_pool->done = sem_open(sem_name, O_CREAT, S_IRWXU, 0);
        if (new_pool->done == SEM_FAILED) {
            goto cleanup;
        }

        new_pool->controls = alloc_controls(size);
        new_pool->workers_must_exit = false;

        for (i = 0; i < new_pool->size; i++) {
            if (!init_control(&new_pool->controls[i], i, new_pool)) {
                goto cleanup;
            }
        }
        ovs_list_push_back(&worker_pools, &new_pool->list_node);
    }
    ovs_mutex_unlock(&init_mutex);
    return new_pool;
cleanup:

    /* Something went wrong when opening semaphores. In this case
     * it is better to shut off parallel procesing altogether
     */

    VLOG_INFO("Failed to initialize parallel processing, error %d", errno);
    can_parallelize = false;
    free_controls(new_pool);
    if (new_pool->done != SEM_FAILED) {
        sem_close(new_pool->done);
        sprintf(sem_name, MAIN_SEM_NAME, sembase, new_pool);
        sem_unlink(sem_name);
    }
    ovs_mutex_unlock(&init_mutex);
    return NULL;
}

/* Initializes 'hmap' as an empty hash table with mask N. */
void
ovn_fast_hmap_init(struct hmap *hmap, ssize_t mask)
{
    size_t i;

    hmap->buckets = xmalloc(sizeof (struct hmap_node *) * (mask + 1));
    hmap->one = NULL;
    hmap->mask = mask;
    hmap->n = 0;
    for (i = 0; i <= hmap->mask; i++) {
        hmap->buckets[i] = NULL;
    }
}

/* Initializes 'hmap' as an empty hash table of size X.
 * Intended for use in parallel processing so that all
 * fragments used to store results in a parallel job
 * are the same size.
 */
void
ovn_fast_hmap_size_for(struct hmap *hmap, int size)
{
    size_t mask;
    mask = size / 2;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
#if SIZE_MAX > UINT32_MAX
    mask |= mask >> 32;
#endif

    /* If we need to dynamically allocate buckets we might as well allocate at
     * least 4 of them. */
    mask |= (mask & 1) << 1;

    fast_hmap_init(hmap, mask);
}

/* Run a thread pool which uses a callback function to process results
 */
void
ovn_start_pool(struct worker_pool *pool)
{
    int index;

    /* Ensure that all worker threads see the same data as the
     * main thread.
     */
    atomic_thread_fence(memory_order_release);

    /* Start workers */
    for (index = 0; index < pool->size; index++) {
        sem_post(pool->controls[index].fire);
    }
}

/* Complete a thread pool which uses a callback function to process results
 */
void
ovn_complete_pool_callback(struct worker_pool *pool,
                           void *fin_result, void *result_frags,
                           void (*helper_func)(struct worker_pool *pool,
                                               void *fin_result,
                                               void *result_frags,
                                               int index))
{
    int index;
    int completed = 0;

    do {
        bool test;
        /* Note - we do not loop on semaphore until it reaches
         * zero, but on pool size/remaining workers.
         * This is by design. If the inner loop can handle
         * completion for more than one worker within an iteration
         * it will do so to ensure no additional iterations and
         * waits once all of them are done.
         *
         * This may result in us having an initial positive value
         * of the semaphore when the pool is invoked the next time.
         * This is harmless - the loop will spin up a couple of times
         * doing nothing while the workers are processing their data
         * slices.
         */
        wait_for_work_completion(pool);
        for (index = 0; index < pool->size; index++) {
            test = true;
            /* If the worker has marked its data chunk as complete,
             * invoke the helper function to combine the results of
             * this worker into the main result.
             *
             * The worker must invoke an appropriate memory fence
             * (most likely acq_rel) to ensure that the main thread
             * sees all of the results produced by the worker.
             */
            if (atomic_compare_exchange_weak(
                    &pool->controls[index].finished,
                    &test,
                    false)) {
                atomic_thread_fence(memory_order_acquire);
                if (helper_func) {
                    (helper_func)(pool, fin_result, result_frags, index);
                }
                completed++;
                pool->controls[index].data = NULL;
            }
        }
    } while (completed < pool->size);
}

/* Complete a thread pool which uses a callback function to process results
 */
void
ovn_run_pool_callback(struct worker_pool *pool,
                      void *fin_result, void *result_frags,
                      void (*helper_func)(struct worker_pool *pool,
                                          void *fin_result,
                                          void *result_frags, int index))
{
    ovn_start_pool(pool);
    ovn_complete_pool_callback(pool, fin_result, result_frags, helper_func);
}

/* Run a thread pool - basic, does not do results processing.
 */
void
ovn_run_pool(struct worker_pool *pool)
{
    run_pool_callback(pool, NULL, NULL, NULL);
}

/* Brute force merge of a hashmap into another hashmap.
 * Intended for use in parallel processing. The destination
 * hashmap MUST be the same size as the one being merged.
 *
 * This can be achieved by pre-allocating them to correct size
 * and using hmap_insert_fast() instead of hmap_insert()
 */

void
ovn_fast_hmap_merge(struct hmap *dest, struct hmap *inc)
{
    size_t i;

    ovs_assert(inc->mask == dest->mask);

    if (!inc->n) {
        /* Request to merge an empty frag, nothing to do */
        return;
    }

    for (i = 0; i <= dest->mask; i++) {
        struct hmap_node **dest_bucket = &dest->buckets[i];
        struct hmap_node **inc_bucket = &inc->buckets[i];
        if (*inc_bucket != NULL) {
            struct hmap_node *last_node = *inc_bucket;
            while (last_node->next != NULL) {
                last_node = last_node->next;
            }
            last_node->next = *dest_bucket;
            *dest_bucket = *inc_bucket;
            *inc_bucket = NULL;
        }
    }
    dest->n += inc->n;
    inc->n = 0;
}

/* Run a thread pool which gathers results in an array
 * of hashes. Merge results.
 */
void
ovn_run_pool_hash(struct worker_pool *pool,
                  struct hmap *result,
                  struct hmap *result_frags)
{
    run_pool_callback(pool, result, result_frags, merge_hash_results);
}

/* Run a thread pool which gathers results in an array of lists.
 * Merge results.
 */
void
ovn_run_pool_list(struct worker_pool *pool,
                  struct ovs_list *result,
                  struct ovs_list *result_frags)
{
    run_pool_callback(pool, result, result_frags, merge_list_results);
}

void
ovn_update_hashrow_locks(struct hmap *target, struct hashrow_locks *hrl)
{
    int i;
    if (hrl->mask != target->mask) {
        if (hrl->row_locks) {
            free(hrl->row_locks);
        }
        hrl->row_locks = xcalloc(sizeof(struct ovs_mutex), target->mask + 1);
        hrl->mask = target->mask;
        for (i = 0; i <= target->mask; i++) {
            ovs_mutex_init(&hrl->row_locks[i]);
        }
    }
}

static bool
init_control(struct worker_control *control, int id,
             struct worker_pool *pool)
{
    char sem_name[256];
    control->id = id;
    control->done = pool->done;
    control->data = NULL;
    ovs_mutex_init(&control->mutex);
    control->finished = ATOMIC_VAR_INIT(false);
    sprintf(sem_name, WORKER_SEM_NAME, sembase, pool, id);
    control->fire = sem_open(sem_name, O_CREAT, S_IRWXU, 0);
    control->pool = pool;
    control->worker = 0;
    if (control->fire == SEM_FAILED) {
        return false;
    }
    control->worker =
        ovs_thread_create("worker pool helper", pool->start, control);
    return true;
}

static void
cleanup_control(struct worker_pool *pool, int id)
{
    char sem_name[256];
    struct worker_control *control = &pool->controls[id];

    if (control->fire != SEM_FAILED) {
        sem_close(control->fire);
        sprintf(sem_name, WORKER_SEM_NAME, sembase, pool, id);
        sem_unlink(sem_name);
    }
}

static void
free_controls(struct worker_pool *pool)
{
    int i;
    if (pool->controls) {
        pool->workers_must_exit = true;
        for (i = 0; i < pool->size ; i++) {
            if (pool->controls[i].fire != SEM_FAILED) {
                sem_post(pool->controls[i].fire);
            }
        }
        for (i = 0; i < pool->size ; i++) {
            if (pool->controls[i].worker) {
                pthread_join(pool->controls[i].worker, NULL);
                pool->controls[i].worker = 0;
            }
        }
        for (i = 0; i < pool->size; i++) {
                cleanup_control(pool, i);
            }
        free(pool->controls);
        pool->controls = NULL;
        pool->workers_must_exit = false;
    }
}

static struct worker_control *alloc_controls(int size)
{
    int i;
    struct worker_control *controls =
        xcalloc(sizeof(struct worker_control), size);

    for (i = 0; i < size ; i++) {
        controls[i].fire = SEM_FAILED;
    }
    return controls;
}

static void
worker_pool_hook(void *aux OVS_UNUSED) {
    static struct worker_pool *pool;
    char sem_name[256];

    /* All workers must honour the must_exit flag and check for it regularly.
     * We can make it atomic and check it via atomics in workers, but that
     * is not really necessary as it is set just once - when the program
     * terminates. So we use a fence which is invoked before exiting instead.
     */
    atomic_thread_fence(memory_order_acq_rel);

    /* Wake up the workers after the must_exit flag has been set */

    LIST_FOR_EACH (pool, list_node, &worker_pools) {
        free_controls(pool);
        sem_close(pool->done);
        sprintf(sem_name, MAIN_SEM_NAME, sembase, pool);
        sem_unlink(sem_name);
    }
}

static void
setup_worker_pools(bool force) {
    int cores, nodes;

    nodes = ovs_numa_get_n_numas();
    if (nodes == OVS_NUMA_UNSPEC || nodes <= 0) {
        nodes = 1;
    }
    cores = ovs_numa_get_n_cores();

    /* If there is no NUMA config, use 4 cores.
     * If there is NUMA config use half the cores on
     * one node so that the OS does not start pushing
     * threads to other nodes.
     */
    if (cores == OVS_CORE_UNSPEC || cores <= 0) {
        /* If there is no NUMA we can try the ovs-threads routine.
         * It falls back to sysconf and/or affinity mask.
         */
        cores = count_cpu_cores();
        pool_size = cores;
    } else {
        pool_size = cores / nodes;
    }
    if ((pool_size < 4) && force) {
        pool_size = 4;
    }
    can_parallelize = (pool_size >= 3);
    fatal_signal_add_hook(worker_pool_hook, NULL, NULL, true);
    sembase = random_uint32();
}

static void
merge_list_results(struct worker_pool *pool OVS_UNUSED,
                   void *fin_result, void *result_frags,
                   int index)
{
    struct ovs_list *result = (struct ovs_list *)fin_result;
    struct ovs_list *res_frags = (struct ovs_list *)result_frags;

    if (!ovs_list_is_empty(&res_frags[index])) {
        ovs_list_splice(result->next,
                ovs_list_front(&res_frags[index]), &res_frags[index]);
    }
}

static void
merge_hash_results(struct worker_pool *pool OVS_UNUSED,
                   void *fin_result, void *result_frags,
                   int index)
{
    struct hmap *result = (struct hmap *)fin_result;
    struct hmap *res_frags = (struct hmap *)result_frags;

    fast_hmap_merge(result, &res_frags[index]);
    hmap_destroy(&res_frags[index]);
}

static void *
standard_helper_thread(void *arg)
{
    struct worker_control *control = (struct worker_control *) arg;
    struct helper_data *hd;
    int bnum;
    struct hmap_node *element, *next;

    while (!stop_parallel_processing(control->pool)) {
        wait_for_work(control);
        hd = (struct helper_data *) control->data;
        if (stop_parallel_processing(control->pool)) {
            return NULL;
        }
        if (hd) {
            for (bnum = control->id; bnum <= hd->target->mask;
                 bnum += control->pool->size)
            {
                element = hmap_first_in_bucket_num(hd->target, bnum);
                while (element) {
                    if (stop_parallel_processing(control->pool)) {
                        return NULL;
                    }
                    next = element->next;
                    hd->element_func(element, hd->target, hd->data);
                    element = next;
                }
            }

        }
        post_completed_work(control);
    }
    return NULL;
}


#endif
