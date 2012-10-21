/*
 * =============================================================================
 *
 *       Filename: cqi_pool.c 
 *
 *    Description: connection queue item pool. 
 *
 *        Created:  10/18/2012 07:56:53 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "cqi_pool.h"

/** returns a fresh connection queue item. */
connection_queue_item_t *
cqi_pool_get_item(cqi_pool_t *pool)
{
    int i = 0, ret = 0;
	connection_queue_item_t *newpool = NULL;
    connection_queue_item_t *item = NULL;
	
    ret = pthread_mutex_lock(&(pool->lock));
	if (ret != 0) {
		LOG_ERROR(("pthread_mutex_lock() failed."));
		return NULL;
	}

	if (pool->curr != NULL) {
		item = pool->curr;
		pool->curr = item->next;
	}

    if (NULL == item) {
		if (pool->allocs == ITEMS_MAX_ALLOC){
			ret = pthread_mutex_unlock(&(pool->lock));
			if (ret != 0) {
				LOG_ERROR(("pthread_mutex_unlock() failed."));
				return NULL;
			}
			LOG_ERROR(("connection item pool allocated too many times."));
			return NULL;
		}
        /* allocate a bunch of items at once to reduce fragmentation */
        newpool = (connection_queue_item_t *) malloc(
				sizeof(connection_queue_item_t) * ITEMS_PER_ALLOC);
        if (NULL == newpool) {
			LOG_ERROR(("cqi_pool_get_item() failed due to out of memory."));
            return NULL;
		} else {
			memset(newpool, 0, sizeof(connection_queue_item_t) *
					(ITEMS_PER_ALLOC));
			for (i = 1; i < ITEMS_PER_ALLOC; i++)
				newpool[i - 1].next = &newpool[i];
			newpool[ITEMS_PER_ALLOC - 1].next = NULL;
			pool->memo[pool->allocs++] = newpool;
			pool->curr = newpool;
			item = pool->curr;
			pool->curr = item->next;
			pool->size += ITEMS_PER_ALLOC;
		}
	}


	ret = pthread_mutex_unlock(&(pool->lock));
	if (ret != 0) {
		LOG_ERROR(("pthread_mutex_unlock() failed."));
		return NULL;
	}

    return item;
}

/** return a connection queue item pool. */
cqi_pool_t *
cqi_pool_new(void)
{
    int i = 0, ret = 0;
    cqi_pool_t *pool = NULL;

	pool = (cqi_pool_t *) malloc(sizeof(cqi_pool_t));
	if (pool == NULL) {
		exit(-1);
	}

	ret = pthread_mutex_init(&(pool->lock), NULL);
	if (ret != 0) {
		LOG_ERROR(("pthread_mutex_init() failed."));
		return NULL;
	}

	pool->pool = (connection_queue_item_t *)
		malloc(sizeof(connection_queue_item_t) * INITIAL_CQI_POOL_SIZE);
	if (pool->pool == NULL) {
		LOG_ERROR(("cqi_pool_new() failed due to out of memory."));
		return NULL;
	} else {
		pool->curr = pool->pool;
		pool->size = INITIAL_CQI_POOL_SIZE;
		pool->allocs = 0;
		memset(pool->memo, 0, sizeof(connection_queue_item_t *)
				* ITEMS_MAX_ALLOC);
        for (i = 1; i < INITIAL_CQI_POOL_SIZE; i++)
            (pool->pool)[i - 1].next= &(pool->pool)[i];
		(pool->pool)[INITIAL_CQI_POOL_SIZE - 1].next = NULL;
	}

	return pool;
}

/*
 * release a connection queue item back to the pool.
 */
void
cqi_pool_release_item(cqi_pool_t *pool,
		connection_queue_item_t *item) {
	int index = -1, ret = 0;

    ret = pthread_mutex_lock(&(pool->lock));
	if (ret != 0) LOG_ERROR(("pthread_mutex_lock() failed."));
	item->next = pool->curr;
	pool->curr = item;
    ret = pthread_mutex_unlock(&(pool->lock));
	if (ret != 0) LOG_ERROR(("pthread_mutex_unlock() failed."));
}

/** free a connection queue item pool. */
void 
cqi_pool_free(cqi_pool_t *pool)
{
	int i = 0;
	if (pool != NULL) {
		pthread_mutex_destroy(&(pool->lock));
		if (pool->pool != NULL) {
			free(pool->pool);
			pool->pool = NULL;
		}
		for (i = 0; i < pool->allocs; i++) {
			free(pool->memo[i]);
		}
		free(pool);
	}
}

#ifdef LLDB_CQI_POOL_TEST
#define WORKER_THREAD_SIZE 64 

void * worker_thread(void *arg)
{
	cqi_pool_t *pool = (cqi_pool_t *)arg;
	connection_queue_item_t *item = cqi_pool_get_item(pool);
	if (item != NULL) {
		item->sfd = pthread_self();
		item->active_flag = pthread_self();
		sleep(1);
		cqi_pool_release_item(pool, item);
	}
}


cqi_pool_t *pool = NULL;

int main()
{
	pool = cqi_pool_new();
	connection_queue_item_t *curr = NULL;

	pthread_t thead[WORKER_THREAD_SIZE];
	log_set_debug_level(LLDB_LOG_LEVEL_DEBUG);

	for (int i = 0; i < WORKER_THREAD_SIZE; i++) {
		pthread_create(&thead[i], NULL, worker_thread, pool);
	}
	
	for (int i = 0; i < WORKER_THREAD_SIZE; i++) {
		pthread_join(thead[i], NULL);
	}
	printf("pool size: %d\n", pool->size);
	cqi_pool_free(pool);
}

#endif // LLDB_CQI_POOL_TEST
