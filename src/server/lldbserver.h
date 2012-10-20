/*
 * =============================================================================
 *
 *       Filename:  lldbserver.h
 *
 *    Description:  lldbserver definitions.
 *
 *        Created:  10/16/2012 08:31:21 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#ifndef LLDBSERVER_H
#define LLDBSERVER_H

#include <assert.h>
#include <event.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#include <leveldb/c.h>

#include "cqi_pool.h"
#include "connection.h"
#include "database.h"
#include "thread.h"

#define BITMASK(b) (1 << ((b) % CHAR_BIT))
#define BITSLOT(b) ((b) / CHAR_BIT)
#define BITSET(a, b) ((a)[BITSLOT(b)] |= BITMASK(b))
#define BITCLEAR(a, b) ((a)[BITSLOT(b)] &= ~BITMASK(b))
#define BITTEST(a, b) ((a)[BITSLOT(b)] & BITMASK(b))
#define BITNSLOTS(nb) ((nb + CHAR_BIT - 1) / CHAR_BIT)

/** how many items in the connection queue item pool
 * when it's initialized. */
#define INITIAL_CQI_POOL_SIZE 512

/** items allocated each time when
 * there are no more items available in the pool. */
#define ITEMS_PER_ALLOC 256

/** Total item allocated times that are allowed. */
#define ITEMS_MAX_ALLOC 256

typedef enum _connection_state_e connection_state_t;

typedef struct _MASTER_THREAD_S MASTER_THREAD;
typedef struct _SLAVE_THREAD_S SLAVE_THREAD;

typedef struct _connection_queue_item_s connection_queue_item_t;
typedef struct _connection_queue_s connection_queue_t;
typedef struct _connection_s connection_t;
typedef struct _cqi_pool_s cqi_pool_t;
typedef struct _database_s database_t;

enum _connection_state_e {
	/**< the socket which listens for connections. */
	LISTENING,
	/**< prepare connection for next command. */
    NEWCMD,
	/**< try to parse a command from the input buffer. */
    PARSECMD,
	/**< waiting for a readable socket. */
    WAITING,
	/**< reading in a command line. */
    READ,
	/**< reading in a fixed number of bytes. */
    NREAD,
	/**< writing out a simple response. */
    WRITE,
    /**< writing out many items sequentially. */
    MWRITE,
    /**< swallowing unnecessary bytes w/o storing. */
    SWALLOW,
    /**< closing this connection. */
    CLOSING,
	/** used for checking. */
	MAXSTATE
};

struct _MASTER_THREAD_S {
	pthread_t thread;
	struct event_base *base;
};

struct _SLAVE_THREAD_S {
	/** thread id. */
	pthread_t thread;
	/* per-thread event_base. */
	struct event_base *base;
	/** listened event for notify pipe. */
	struct event notify_event;
	/** receiving endpoint of notify pipe. */
	int notify_recv_fd;
	/** send endpoint of notify pipe. */
	int notify_send_fd;
	/** queue of new connections. */
	connection_queue_t *conn_queue;
};


struct _connection_queue_item_s {
    int sfd;
    connection_state_t init_state;
    int event_flags;
	int active_flag;
    int read_buffer_size;
    connection_queue_item_t *next;
};

struct _connection_queue_s {
    connection_queue_item_t *head;
    connection_queue_item_t *tail;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
};


struct _connection_s {
	int sfd;
	connection_state_t state;
	struct event event;
	short event_flags;
	short which;

	/** read buffer to read command into. */
	char *rbuff;
	/** current read position. */
	char *rcurr;
	/** total allocated size of read buffer. */
	size_t ralloc;
	/** how much data left starting from @rcurr are left unread. */
	size_t rleft; 

	/** write buffer. */
	char *wbuff;
	/** current write position. */
	char *wcurr;
	/** total allocated size of write buffer. */
	size_t walloc;
	/** how much data left starting from &wcurr are left unprocessed. */
	size_t wleft;

    struct iovec *iov;
	/** number of elements allocated in iov[] */
    size_t iov_size;
	/** number of elements used in iov[] */
    size_t iov_used;   

    struct msghdr *msglist;
	/* number of elements allocated in msglist[] */
    size_t msglist_size;
	/* number of elements used in msglist[] */
    size_t msglist_used;
	/* element in msglist[] being transmitted now */
    size_t msglist_curr;
	/* number of bytes in current msg */
    size_t msgbytes;

	/** current command being processed. */
	short cmd;

	/** database name, which is used to find the proper database instance. */
	char *dbname;
	/** database name length. */
	unsigned int dbname_len;

	/** per-connection database instance. */
	database_t * db;

	/** used for listening multiple ports. */
	connection_t *next;

	/** thread object serving this connection. */
	SLAVE_THREAD *thread;
};

/** connection queue item pool. */
struct _cqi_pool_s {
	/** connection queue items pool start address. */
	connection_queue_item_t *pool;
	connection_queue_item_t *curr;
	/* keep memory address of each allocated items. */
	connection_queue_item_t *memo[ITEMS_MAX_ALLOC];
	/** how many items do we have. */
	unsigned int size;
	/** how many allocates we have done. */
	unsigned int allocs;
	/** lock to protect this pool. */
	pthread_mutex_t lock;
};

struct _database_s {
	/** database specified variable. */
	leveldb_t *db;
	leveldb_cache_t *cache;
	leveldb_comparator_t *comparator;
	leveldb_env_t *ent;
	leveldb_filterpolicy_t *filterpolicy;
	leveldb_iterator_t *iterator;
	leveldb_logger_t *logger;
	leveldb_options_t *options;
	leveldb_readoptions_t *roptions;
	leveldb_snapshot_t *snapshot;
	leveldb_writebatch_t *writebatch;
	leveldb_writeoptions_t *woptions;

	/** database name. */
	char *dbname;
	/** what error has occured on this database when we do operations. */
	char *err;

	/** reference count that how many clients opened this database. */
	int refcnt;

	database_t *next;
};

#endif // LLDBSERVER_H
