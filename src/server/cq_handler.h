/*
 * =============================================================================
 *
 *       Filename:  cq_hanlder.h 
 *
 *    Description:  lldbserver per-thread connection queue data structure.
 *
 *        Created:  10/20/2012 02:59:24 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#ifndef LLDB_CONNECTION_QUEUE_H
#define LLDB_CONNECTION_QUEUE_H

#include "lldbserver.h"

/** initializes a connection queue. */
extern void connection_queue_init(connection_queue_t *queue);

/**
 * looks for an item on a connection queue,
 * but doesn't block if there isn't one.
 * returns the item, or NULL if no item is available
 */
extern connection_queue_item_t *
connection_queue_pop(connection_queue_t *queue);


/** adds an item to a connection queue. */
extern void connection_queue_push(connection_queue_t *queue,
		connection_queue_item_t *item);

#endif // LLDB_CONNECTION_QUEUE_H_
