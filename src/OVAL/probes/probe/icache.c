/*
 * Copyright 2011 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Authors:
 *      Daniel Kopecek <dkopecek@redhat.com>
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pthread.h>
#include <stddef.h>
#include <sexp.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

#if defined(OS_FREEBSD)
#include <pthread_np.h>
#endif

#include "../SEAP/generic/rbt/rbt.h"
#include "probe-api.h"
#include "common/debug_priv.h"
#include "common/memusage.h"
#include "oscap_helpers.h"

#include "probe.h"
#include "icache.h"
#include "_sexp-ID.h"

static volatile uint32_t next_ID = 0;

#if !defined(HAVE_ATOMIC_FUNCTIONS)
pthread_mutex_t next_ID_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static void probe_icache_item_setID(SEXP_t *item, SEXP_ID_t item_ID)
{
        SEXP_t  *name_ref, *prev_id;
        SEXP_t   uniq_id;
        uint32_t local_id;

        /* ((foo_item :id "<int>") ... ) */

	if (item == NULL) {
		return;
	}
	if (!SEXP_listp(item)) {
		return;
	}

#if defined(HAVE_ATOMIC_FUNCTIONS)
        local_id = __sync_fetch_and_add(&next_ID, 1);
#else
        if (pthread_mutex_lock(&next_ID_mutex) != 0) {
                dE("Can't lock the next_ID_mutex: %u, %s", errno, strerror(errno));
                abort();
        }

        local_id = ++next_ID;

        if (pthread_mutex_unlock(&next_ID_mutex) != 0) {
                dE("Can't unlock the next_ID_mutex: %u, %s", errno, strerror(errno));
                abort();
        }
#endif
        SEXP_string_newf_r(&uniq_id, "1%05u%u", getpid(), local_id);

        name_ref = SEXP_listref_first(item);
        prev_id  = SEXP_list_replace(name_ref, 3, &uniq_id);

        SEXP_free(prev_id);
        SEXP_free_r(&uniq_id);
        SEXP_free(name_ref);

        return;
}

static int icache_lookup(rbt_t *tree, int64_t item_id, probe_iqpair_t *pair) {

	probe_citem_t *cached = NULL;

	if (rbt_i64_get(tree, item_id, (void**)&cached) != 0) {
		return -1;
	}

	/*
	* Maybe a cache HIT
	*/
	dD("cache HIT #1");

	register uint16_t i;
	for (i = 0; i < cached->count; ++i) {
		SEXP_t rest1;
		SEXP_t* rest_r1 = SEXP_list_rest_r(&rest1, pair->p.item);

		SEXP_t rest2;
		SEXP_t* rest_r2 = SEXP_list_rest_r(&rest2, cached->item[i]);

		if (SEXP_deepcmp(rest_r1, rest_r2)) {
			SEXP_free_r(&rest1);
			SEXP_free_r(&rest2);
			break;
		}

		SEXP_free_r(&rest1);
		SEXP_free_r(&rest2);
	}

	if (i == cached->count) {
		/*
		* Cache MISS
		*/
		dD("cache MISS");

		void *new_item = realloc(cached->item, sizeof(SEXP_t *) * (cached->count + 1));
		if (new_item == NULL) {
			dE("Unable to re-allocate memory for cache");
			return -1;
		}
		cached->count++;
		cached->item = new_item;
		cached->item[cached->count - 1] = pair->p.item;

		/* Assign an unique item ID */
		probe_icache_item_setID(pair->p.item, item_id);
	} else {
		/*
		* Cache HIT
		*/
		dD("cache HIT #2 -> real HIT");
		SEXP_free(pair->p.item);
		pair->p.item = cached->item[i];
	}
	return 0;
}

static void icache_add_to_tree(rbt_t *tree, int64_t item_id, probe_iqpair_t *pair) {

	probe_citem_t *cached = malloc(sizeof(probe_citem_t));
	cached->item = malloc(sizeof(SEXP_t *));
	cached->item[0] = pair->p.item;
	cached->count = 1;

	/* Assign an unique item ID */
	probe_icache_item_setID(pair->p.item, item_id);

	if (rbt_i64_add(tree, (int64_t)item_id, (void **)cached, NULL) != 0) {
		dE("Can't add item (k=%"PRIi64" to the cache (%p)", item_id, tree);

		free(cached->item);
		free(cached);

		/* now what? */
		abort();
	}
}

static void *probe_icache_worker(void *arg)
{
        probe_icache_t *cache = (probe_icache_t *)(arg);
        probe_iqpair_t *pair, pair_mem;
        SEXP_ID_t       item_ID;

	if (cache == NULL) {
		return NULL;
	}

#if defined(HAVE_PTHREAD_SETNAME_NP)
const char* thread_name = "icache_worker";
# if defined(OS_APPLE)
	pthread_setname_np(thread_name);
# else
	pthread_setname_np(pthread_self(), thread_name);
# endif
#endif

        if (pthread_mutex_lock(&cache->queue_mutex) != 0) {
                dE("An error ocured while locking the queue mutex: %u, %s",
                   errno, strerror(errno));
                return (NULL);
        }

	pair = &pair_mem;
        dD("icache worker ready");

        switch (errno = pthread_barrier_wait(cache->th_barrier))
        {
        case 0:
        case PTHREAD_BARRIER_SERIAL_THREAD:
	        break;
        default:
	        dE("pthread_barrier_wait: %d, %s.",
	           errno, strerror(errno));
	        pthread_mutex_unlock(&cache->queue_mutex);
	        return (NULL);
        }

        while(pthread_cond_wait(&cache->queue_notempty, &cache->queue_mutex) == 0) {
			if (cache->queue_cnt <= 0) {
				pthread_mutex_unlock(&cache->queue_mutex);
				return NULL;
			}
        do {
                dD("Extracting item from the cache queue: cnt=%"PRIu16", beg=%"PRIu16"", cache->queue_cnt, cache->queue_beg);
                /*
                 * Extract an item from the queue and update queue beg, end & cnt
                 */
                pair_mem = cache->queue[cache->queue_beg];
#ifndef NDEBUG
		memset(cache->queue + cache->queue_beg, 0, sizeof(probe_iqpair_t));
#endif
                --cache->queue_cnt;
		++cache->queue_beg;

		if (cache->queue_beg == cache->queue_max)
			cache->queue_beg = 0;

		if (cache->queue_cnt == 0 ?
			cache->queue_end != cache->queue_beg :
			cache->queue_end == cache->queue_beg) {
			pthread_mutex_unlock(&cache->queue_mutex);
			return NULL;
		}


                dD("Signaling `notfull'");

                if (pthread_cond_signal(&cache->queue_notfull) != 0) {
                        dE("An error ocured while signaling the `notfull' condition: %u, %s",
                           errno, strerror(errno));
                        abort();
                }
                /*
                 * Release the mutex
                 */
                if (pthread_mutex_unlock(&cache->queue_mutex) != 0) {
                        dE("An error ocured while unlocking the queue mutex: %u, %s",
                           errno, strerror(errno));
                        abort();
                }

                if (pair->cobj == NULL) {
                        /*
                         * Handle NOP case (synchronization)
                         */
					if (pair->p.cond == NULL) {
						return NULL;
					}

                        dD("Handling NOP");

                        if (pthread_cond_signal(pair->p.cond) != 0) {
                                dE("An error ocured while signaling NOP condition: %u, %s",
                                   errno, strerror(errno));
                                abort();
                        }
                } else {

                        dD("Handling cache request");

                        /*
                         * Compute item ID
                         */
			dD("pair address: %"PRIu64, (uint64_t) pair);
			dD("item address: %"PRIu64, (uint64_t) pair->p.item);
                        item_ID = SEXP_ID_v(pair->p.item);
                        dD("item ID=%"PRIu64"", item_ID);

                        if (icache_lookup(cache->tree, item_ID, pair) != 0) {
                                /*
                                 * Cache MISS
                                 */
                                dD("cache MISS");
                                icache_add_to_tree(cache->tree, item_ID, pair);
                        }

                        if (probe_cobj_add_item(pair->cobj, pair->p.item) != 0) {
                            dW("An error ocured while adding the item to the collected object");
                        }
                }

                if (pthread_mutex_lock(&cache->queue_mutex) != 0) {
                        dE("An error ocured while re-locking the queue mutex: %u, %s",
                           errno, strerror(errno));
                        abort();
                }

                } while (cache->queue_cnt > 0);
        }

        return (NULL);
}

probe_icache_t *probe_icache_new(pthread_barrier_t *th_barrier)
{
        probe_icache_t *cache = malloc(sizeof(probe_icache_t));
        cache->tree = rbt_i64_new();

        if (pthread_mutex_init(&cache->queue_mutex, NULL) != 0) {
                dE("Can't initialize icache mutex: %u, %s", errno, strerror(errno));
                goto fail;
        }

        cache->queue_beg = 0;
        cache->queue_end = 0;
        cache->queue_cnt = 0;
        cache->queue_max = PROBE_IQUEUE_CAPACITY;
        cache->th_barrier = th_barrier;

        if (pthread_cond_init(&cache->queue_notempty, NULL) != 0) {
                dE("Can't initialize icache queue condition variable (notempty): %u, %s",
                   errno, strerror(errno));
                goto fail;
        }

        if (pthread_cond_init(&cache->queue_notfull, NULL) != 0) {
                dE("Can't initialize icache queue condition variable (notfull): %u, %s",
                   errno, strerror(errno));
                goto fail;
        }

        if (pthread_create(&cache->thid, NULL,
                           probe_icache_worker, (void *)cache) != 0)
        {
                dE("Can't start the icache worker: %u, %s", errno, strerror(errno));
                goto fail;
        }

        return (cache);
fail:
        if (cache->tree != NULL)
                rbt_i64_free(cache->tree);

        pthread_mutex_destroy(&cache->queue_mutex);
        pthread_cond_destroy(&cache->queue_notempty);
        free(cache);

        return (NULL);
}

static int __probe_icache_add_nolock(probe_icache_t *cache, SEXP_t *cobj, SEXP_t *item, pthread_cond_t *cond)
{
	if (!((cond == NULL) ^ (item == NULL))) {
		return -1;
	}
retry:
        if (cache->queue_cnt < cache->queue_max) {
                cache->queue[cache->queue_end].cobj = cobj;

                if (item != NULL) {
					if (cobj == NULL) {
						return -1;
					}
                        cache->queue[cache->queue_end].p.item = item;
                } else {
					if (item != NULL || cobj != NULL) {
						return -1;
					}
                        cache->queue[cache->queue_end].p.cond = cond;
		}

                ++cache->queue_cnt;
		++cache->queue_end;

                if (cache->queue_end == cache->queue_max)
                        cache->queue_end = 0;
        } else {
                /*
                 * The queue is full, we have to wait
                 */
                if (pthread_cond_wait(&cache->queue_notfull, &cache->queue_mutex) == 0)
                        goto retry;
                else {
                        dE("An error ocured while waiting for the `notfull' queue condition: %u, %s",
                           errno, strerror(errno));
                        return (-1);
                }
        }

        return (0);
}

int probe_icache_add(probe_icache_t *cache, SEXP_t *cobj, SEXP_t *item)
{
        int ret;

        if (cache == NULL || cobj == NULL || item == NULL)
                return (-1); /* XXX: EFAULT */

        if (pthread_mutex_lock(&cache->queue_mutex) != 0) {
                dE("An error ocured while locking the queue mutex: %u, %s",
                   errno, strerror(errno));
                return (-1);
        }

        ret = __probe_icache_add_nolock(cache, cobj, item, NULL);

        if (pthread_cond_signal(&cache->queue_notempty) != 0) {
                dE("An error ocured while signaling the `notempty' condition: %u, %s",
                   errno, strerror(errno));
                pthread_mutex_unlock(&cache->queue_mutex);
                return (-1);
        }

        if (pthread_mutex_unlock(&cache->queue_mutex) != 0) {
                dE("An error ocured while unlocking the queue mutex: %u, %s",
                   errno, strerror(errno));
                abort();
        }

        if (ret != 0)
                return (-1);

        return (0);
}

int probe_icache_nop(probe_icache_t *cache)
{
        pthread_cond_t cond;

        dD("NOP");

        if (pthread_mutex_lock(&cache->queue_mutex) != 0) {
                dE("An error ocured while locking the queue mutex: %u, %s",
                   errno, strerror(errno));
                return (-1);
        }

        if (pthread_cond_init(&cond, NULL) != 0) {
                dE("Can't initialize icache queue condition variable (NOP): %u, %s",
                   errno, strerror(errno));
                pthread_mutex_unlock(&cache->queue_mutex);
                return (-1);
        }

        if (__probe_icache_add_nolock(cache, NULL, NULL, &cond) != 0) {
                if (pthread_mutex_unlock(&cache->queue_mutex) != 0) {
                        dE("An error ocured while unlocking the queue mutex: %u, %s",
                           errno, strerror(errno));
                        abort();
                }

                pthread_cond_destroy(&cond);
                return (-1);
        }

        dD("Signaling `notempty'");

        if (pthread_cond_signal(&cache->queue_notempty) != 0) {
                dE("An error ocured while signaling the `notempty' condition: %u, %s",
                   errno, strerror(errno));

                pthread_cond_destroy(&cond);
                pthread_mutex_unlock(&cache->queue_mutex);
                return (-1);
        }

        dD("Waiting for icache worker to handle the NOP");

        if (pthread_cond_wait(&cond, &cache->queue_mutex) != 0) {
                dE("An error ocured while waiting for the `NOP' queue condition: %u, %s",
                   errno, strerror(errno));
                return (-1);
        }

        dD("Sync");

        if (pthread_mutex_unlock(&cache->queue_mutex) != 0) {
                dE("An error ocured while unlocking the queue mutex: %u, %s",
                   errno, strerror(errno));
                abort();
        }

        pthread_cond_destroy(&cond);

        return (0);
}

#define PROBE_RESULT_MEMCHECK_CTRESHOLD  1000  /* item count */

/**
 * Returns 0 if the memory constraints are not reached. Otherwise, 1 is returned.
 * In case of an error, -1 is returned.
 */
static int probe_cobj_memcheck(size_t item_cnt, double max_ratio)
{
	if (item_cnt > PROBE_RESULT_MEMCHECK_CTRESHOLD) {
		struct proc_memusage mu_proc;
		struct sys_memusage  mu_sys;
		double c_ratio;

		memset(&mu_proc, 0, sizeof(mu_proc));
		memset(&mu_sys, 0, sizeof(mu_sys));

		if (oscap_proc_memusage (&mu_proc) != 0)
			return (-1);

		if (oscap_sys_memusage (&mu_sys) != 0)
			return (-1);

		c_ratio = (double)mu_proc.mu_rss/(double)(mu_sys.mu_total);

		if (c_ratio > max_ratio) {
			dW("Memory usage ratio limit reached! limit=%f, current=%f, used=%ld MB, free=%ld MB, total=%ld MB, count of items=%ld",
			max_ratio, c_ratio, mu_proc.mu_rss / 1024, mu_sys.mu_realfree / 1024, mu_sys.mu_total / 1024, item_cnt);
			errno = ENOMEM;
			return (1);
		}

	}

	return (0);
}

static int _mark_collected_object_as_incomplete(struct probe_ctx *ctx, const char *message)
{
	/*
	 * Don't set the message again if the collected object is
	 * already flagged as incomplete.
	 */
	if (probe_cobj_get_flag(ctx->probe_out) == SYSCHAR_FLAG_INCOMPLETE) {
		return 0;
	}
	/*
	 * Sync with the icache thread before modifying the
	 * collected object.
	 */
	if (probe_icache_nop(ctx->icache) != 0) {
		return -1;
	}

	SEXP_t *sexp_msg = probe_msg_creat(OVAL_MESSAGE_LEVEL_WARNING, (char *) message);
	probe_cobj_add_msg(ctx->probe_out, sexp_msg);
	probe_cobj_set_flag(ctx->probe_out, SYSCHAR_FLAG_INCOMPLETE);
	SEXP_free(sexp_msg);
	return 0;
}

/**
 * Collect an item
 * This function adds an item the collected object assosiated
 * with the given probe context.
 *
 * Returns:
 * 0 ... the item was succesfully added to the collected object
 * 1 ... the item was filtered out
 * 2 ... the item was not added because of memory constraints
 *       and the collected object was flagged as incomplete
 *-1 ... unexpected/internal error
 *
 * The caller must not free the item, it's freed automatically
 * by this function or by the icache worker thread.
 */
int probe_item_collect(struct probe_ctx *ctx, SEXP_t *item)
{
	if (ctx == NULL || ctx->probe_out == NULL || item == NULL) {
		return -1;
	}

	if (ctx->max_collected_items != OSCAP_PROBE_COLLECT_UNLIMITED && ctx->collected_items >= ctx->max_collected_items) {
		char *message = oscap_sprintf("Object is incomplete because the object matches more than %ld items.", ctx->max_collected_items);
		if (_mark_collected_object_as_incomplete(ctx, message) != 0) {
			free(message);
			return -1;
		}
		free(message);
		return 2;
	}

	int memcheck_ret = probe_cobj_memcheck(ctx->collected_items, ctx->max_mem_ratio);
	if (memcheck_ret == -1) {
		dE("Failed to check available memory");
		SEXP_free(item);
		return -1;
	}
	if (memcheck_ret == 1) {
		SEXP_free(item);
		const char *message = "Object is incomplete due to memory constraints.";
		if (_mark_collected_object_as_incomplete(ctx, message) != 0) {
			return -1;
		}

		return 2;
	}

        if (ctx->filters != NULL && probe_item_filtered(item, ctx->filters)) {
                SEXP_free(item);
		return (1);
        }

        if (probe_icache_add(ctx->icache, ctx->probe_out, item) != 0) {
                dE("Can't add item (%p) to the item cache (%p)", item, ctx->icache);
                SEXP_free(item);
                return (-1);
        }

        ctx->collected_items++;
        return (0);
}

static void probe_icache_free_node(struct rbt_i64_node *n)
{
        probe_citem_t *ci = (probe_citem_t *)n->data;

	for ( ; ci->count > 0 ; --ci->count ) {
		SEXP_free(ci->item[ci->count - 1]);
	}

        free(ci->item);
        free(ci);
        return;
}

void probe_icache_free(probe_icache_t *cache)
{
        void *ret = NULL;

        pthread_cancel(cache->thid);
        pthread_join(cache->thid, &ret);
        pthread_mutex_destroy(&cache->queue_mutex);
        pthread_cond_destroy(&cache->queue_notempty);
        pthread_cond_destroy(&cache->queue_notfull);

        rbt_i64_free_cb(cache->tree, &probe_icache_free_node);
        free(cache);
        return;
}
