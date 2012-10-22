/*
 * =============================================================================
 *
 *       Filename:  cq_handler.c
 *
 *    Description:  per-thread connection queue handler.
 *
 *        Created:  10/20/2012 03:16:45 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#include "connection_queue.h"

/** initializes a connection queue. */
void
connection_queue_init(connection_queue_t *queue)
{
    pthread_mutex_init(&(queue->lock), NULL);
    pthread_cond_init(&(queue->cond), NULL);
    queue->head = NULL;
    queue->tail = NULL;
}

/**
 * looks for an item on a connection queue,
 * but doesn't block if there isn't one.
 * returns the item, or NULL if no item is available
 */
connection_queue_item_t *
connection_queue_pop(connection_queue_t *queue)
{
    connection_queue_item_t *item;

    pthread_mutex_lock(&(queue->lock));
    item = queue->head;
    if (NULL != item) {
        queue->head = item->next;
        if (NULL == queue->head)
            queue->tail = NULL;
    }
    pthread_mutex_unlock(&queue->lock);

    return item;
}

/** adds an item to a connection queue. */
void
connection_queue_push(connection_queue_t *queue, connection_queue_item_t *item)
{
    item->next = NULL;

    pthread_mutex_lock(&queue->lock);
    if (NULL == cq->tail)
        cq->head = item;
    else
        cq->tail->next = item;
    cq->tail = item;
    pthread_cond_signal(&(queue->cond));
    pthread_mutex_unlock(&(queue->lock));
}

