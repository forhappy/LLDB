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

#include <event.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

typedef struct _connection_s connection_t;
typedef struct _connection_queue_s connection_queue_t;
typedef struct _MASTER_THREAD_S MASTER_THREAD;
typedef struct _SLAVE_THREAD_S SLAVE_THREAD;

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

struct _connection_s {
	int sfd;
	struct event event;
	short event_flags;
	short which;

	/** read buffer to read command into. */
	char *rbuff;
	/** current read position. */
	char *rcurr;
	/** total allocated size of read buffer. */
	unsigned int ralloc;
	/** how much data left starting from @rcurr are left unread. */
	unsigned int rleft; 

	/** write buffer. */
	char *wbuff;
	/** current write position. */
	char *wcurr;
	/** total allocated size of write buffer. */
	unsigned int walloc;
	/** how much data left starting from &wcurr are left unprocessed. */
	unsigned int wleft;

	/** current command being processed. */
	short cmd;

	lldb_connection_t *next;

	/** thread object serving this connection. */
	SLAVE_THREAD *thread;
}

#endif // LLDBSERVER_H
