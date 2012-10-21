/*
 * =============================================================================
 *
 *       Filename:  cqi_pool.h
 *
 *    Description:  connection queue item pool.
 *
 *        Created:  10/20/2012 02:52:51 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#ifndef LLDB_CQI_POOL_H
#define LLDB_CQI_POOL_H

#include "lldbserver.h"
#include "log.h"

/** returns a fresh connection queue item. */
extern connection_queue_item_t * cqi_pool_get_item(cqi_pool_t *pool);

/** return a connection queue item pool. */
extern cqi_pool_t * cqi_pool_new(void);

/** release a connection queue item back to the pool. */
extern void cqi_pool_release_item(cqi_pool_t *pool, connection_queue_item_t *item);

/** free a connection queue item pool. */
extern void cqi_pool_free(cqi_pool_t *pool);

#endif // LLDB_CQI_POOL_H
