/*
 * =============================================================================
 *
 *       Filename:  thread.c
 *
 *    Description:  thread utility.
 *
 *        Created:  10/18/2012 12:54:23 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "lldbserver.h"
#include "connection_queue.h"
#include "thread.h"

static cqi_pool_t *item_pool;

static MASTER_THREAD dispatcher_thread;
static WORKER_THREAD *worker_threads;

/** number of worker threads that have finished setting themselves up. */
static int init_count = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;

/** which thread we assigned a connection to most recently. */
static int last_thread = -1;

static void worker_thread_libevent_process(int fd, short which, void *arg);

static void
wait_for_worker_thread_registration(int nthreads)
{
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

static void
register_worker_thread_initialized(void)
{
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
}


/** creates a worker thread. */
static void create_worker_thread(void *(*func)(void *), void *arg)
{
    pthread_t       thread;
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&thread, &attr, func, arg)) != 0) {
        LOG_ERROR(("can't create thread: %s\n", strerror(ret)));
        exit(1);
    }
}

/** set up a thread's information. */
static void
setup_worker_thread_handler(WORKER_THREAD *me)
{
    me->base = event_init();
    if (me->base == NULL) {
        LOG_ERROR(("can't allocate event base."));
        exit(EXIT_FAILURE);
    }

    /* listen for notifications from other threads */
    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, worker_thread_libevent_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        LOG_ERROR(("can't monitor libevent notify pipe."));
        exit(EXIT_FAILURE);
    }

    me->conn_queue = (connection_queue_t *)malloc(sizeof(connection_queue_t));
    if (me->conn_queue == NULL) {
        LOG_ERROR(("failed to allocate memory for connection queue."));
        exit(EXIT_FAILURE);
    }

    connection_queue_init(me->conn_queue);
}

/** worker thread: main event loop. */
static void *worker_thread_callback(void *arg) {
    WORKER_THREAD *me = (WORKER_THREAD *)arg;

    register_worker_thread_initialized();
    event_base_loop(me->base, 0);
    return NULL;
}


/**
 * processes an incoming "handle a new connection" item,
 * this is called when input arrives on the libevent wakeup pipe.
 */
static void
worker_thread_libevent_process(int fd, short which, void *arg) 
{
    WORKER_THREAD *me = (WORKER_THREAD *)arg;
    connection_queue_item_t *item;
    char buf[1];

    if (read(fd, buf, 1) != 1) {
		LOG_ERROR(("can't read from libevent pipe."));
		exit(EXIT_FAILURE);
	}

    if (buf[0] == 'c') {
		item = connection_queue_pop(me->conn_queue);
		if (NULL != item) {
			conn *c = conn_new(item->sfd, item->init_state, item->event_flags,
					item->read_buffer_size, me->base);
			if (c == NULL) {
				LOG_ERROR(("can't listen for events on fd %d.", item->sfd));
				close(item->sfd);
			} else {
				c->thread = me;
			}
			cqi_pool_release_item(item);
		}
	}
}


/**
 * dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */
void
lldb_dispatch_new_connection(
		int sfd, connection_states_t init_state,
		int event_flags, int read_buffer_size)
{
    connection_queue_item_t *item = cqi_pool_get_item(item_pool);
    char buf[1];
    int tid = (last_thread + 1) % settings.nthreads;

    LIBEVENT_THREAD *thread = worker_threads + tid;

    last_thread = tid;

    item->sfd = sfd;
    item->init_state = init_state;
    item->event_flags = event_flags;
    item->read_buffer_size = read_buffer_size;

    connections_queue_push(thread->conn_queue, item);

    buf[0] = 'c';
    if (write(thread->notify_send_fd, buf, 1) != 1) {
        LOG_ERROR(("writing to thread notify pipe"));
		exit(EXIT_FAILURE);
    }
}

/** returns true if this is the thread that listens for new TCP connections. */
int
lldb_is_listening_thread()
{
    return pthread_self() == dispatcher_thread.thread;
}

void
lldb_initialize_worker_threads(int nthreads, struct event_base *main_base)
{
    int         i;

    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    worker_threads = (WORKER_THREAD *)calloc(nthreads, sizeof(WORKER_THREAD));
    if (worker_threads == NULL) {
		LOG_ERROR(("failed to allocate memory for worker_threads."));
        exit(EXIT_FAILURE);
    }

    dispatcher_thread.base = main_base;
    dispatcher_thread.thread = pthread_self();

	item_pool = cqi_pool_new();

    for (i = 0; i < nthreads; i++) {
        int fds[2];
        if (pipe(fds) == -1) {
			LOG_ERROR(("can't pipe."));
			exit(EXIT_FAILURE);
		}

        worker_threads[i].notify_receive_fd = fds[0];
        worker_threads[i].notify_send_fd = fds[1];

        setup_worker_thread_handler(&worker_threads[i]);
    }

    /* create threads after we've done all the libevent setup. */
    for (i = 0; i < nthreads; i++) {
        create_worker_thread(worker_thread_callback, &worker_threads[i]);
    }

    /** wait for all the threads to set themselves up before returning. */
    pthread_mutex_lock(&init_lock);
    wait_for_worker_thread_registration(nthreads);
    pthread_mutex_unlock(&init_lock);
}

