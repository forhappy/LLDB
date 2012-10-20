/*
 * =============================================================================
 *
 *       Filename:  lldbserver.c
 *
 *    Description:  lldbserver implementation.
 *
 *        Created:  10/16/2012 09:30:15 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#include "lldbserver.h"

/*
 * forward declarations
 */
static void drive_machine(connection_t *c);
static int new_socket(struct addrinfo *ai);
static int try_read_command(connection_t *c);

enum try_read_result {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,            /** an error occured (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR      /** failed to allocate more memory */
};

static enum try_read_result try_read_network(connection_t *c);
static enum try_read_result try_read_udp(connection_t *c);

static void conn_set_state(connection_t *c, enum conn_states state);

/* stats */
static void stats_init(void);
static void server_stats(ADD_STAT add_stats, connection_t *c);
static void process_stat_settings(ADD_STAT add_stats, void *c);


/* defaults */
static void settings_init(void);

/* event handling, network IO */
static void event_handler(const int fd, const short which, void *arg);
static void conn_close(connection_t *c);
static void conn_init(void);
static bool update_event(connection_t *c, const int new_flags);
static void complete_nread(connection_t *c);
static void process_command(connection_t *c, char *command);
static void write_and_free(connection_t *c, char *buf, int bytes);
static int ensure_iov_space(connection_t *c);
static int add_iov(connection_t *c, const void *buf, int len);
static int add_msghdr(connection_t *c);


static void conn_free(connection_t *c);

/** exported globals **/
struct stats stats;
struct settings settings;
time_t process_started;     /* when the process was started */

struct slab_rebalance slab_rebal;
volatile int slab_rebalance_signal;

/** file scope variables **/
static connection_t *listen_conn = NULL;
static struct event_base *main_base;

enum transmit_result {
    TRANSMIT_COMPLETE,   /** All done writing. */
    TRANSMIT_INCOMPLETE, /** More data remaining to write. */
    TRANSMIT_SOFT_ERROR, /** Can't write any more right now. */
    TRANSMIT_HARD_ERROR  /** Can't write (c->state is set to conn_closing) */
};

static enum transmit_result transmit(connection_t *c);

/* This reduces the latency without adding lots of extra wiring to be able to
 * notify the listener thread of when to listen again.
 * Also, the clock timer could be broken out into its own thread and we
 * can block the listener via a condition.
 */
static volatile bool allow_new_conns = true;
static struct event maxconnsevent;
static void maxconns_handler(const int fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 0, .tv_usec = 10000};

    if (fd == -42 || allow_new_conns == false) {
        /* reschedule in 10ms if we need to keep polling */
        evtimer_set(&maxconnsevent, maxconns_handler, 0);
        event_base_set(main_base, &maxconnsevent);
        evtimer_add(&maxconnsevent, &t);
    } else {
        evtimer_del(&maxconnsevent);
        accept_new_conns(true);
    }
}

#define REALTIME_MAXDELTA 60*60*24*30

/*
 * given time value that's either unix time or delta from current unix time, return
 * unix time. Use the fact that delta can't exceed one month (and real time value can't
 * be that low).
 */
static rel_time_t realtime(const time_t exptime) {
    /* no. of seconds in 30 days - largest possible delta exptime */

    if (exptime == 0) return 0; /* 0 means never expire */

    if (exptime > REALTIME_MAXDELTA) {
        /* if item expiration is at/before the server started, give it an
           expiration time of 1 second after the server started.
           (because 0 means don't expire).  without this, we'd
           underflow and wrap around to some large value way in the
           future, effectively making items expiring in the past
           really expiring never */
        if (exptime <= process_started)
            return (rel_time_t)1;
        return (rel_time_t)(exptime - process_started);
    } else {
        return (rel_time_t)(exptime + current_time);
    }
}

static void stats_init(void) {
    stats.curr_items = stats.total_items = stats.curr_conns = stats.total_conns = stats.conn_structs = 0;
    stats.get_cmds = stats.set_cmds = stats.get_hits = stats.get_misses = stats.evictions = stats.reclaimed = 0;
    stats.touch_cmds = stats.touch_misses = stats.touch_hits = stats.rejected_conns = 0;
    stats.curr_bytes = stats.listen_disabled_num = 0;
    stats.hash_power_level = stats.hash_bytes = stats.hash_is_expanding = 0;
    stats.expired_unfetched = stats.evicted_unfetched = 0;
    stats.slabs_moved = 0;
    stats.accepting_conns = true; /* assuming we start in this state. */
    stats.slab_reassign_running = false;

    /* make the time we started always be 2 seconds before we really
       did, so time(0) - time.started is never zero.  if so, things
       like 'settings.oldest_live' which act as booleans as well as
       values are now false in boolean context... */
    process_started = time(0) - 2;
    stats_prefix_init();
}

static void stats_reset(void) {
    STATS_LOCK();
    stats.total_items = stats.total_conns = 0;
    stats.rejected_conns = 0;
    stats.evictions = 0;
    stats.reclaimed = 0;
    stats.listen_disabled_num = 0;
    stats_prefix_clear();
    STATS_UNLOCK();
    threadlocal_stats_reset();
    item_stats_reset();
}

static void settings_init(void) {
    settings.use_cas = true;
    settings.access = 0700;
    settings.port = 11211;
    settings.udpport = 11211;
    /* By default this string should be NULL for getaddrinfo() */
    settings.inter = NULL;
    settings.maxbytes = 64 * 1024 * 1024; /* default is 64MB */
    settings.maxconns = 1024;         /* to limit connections-related memory to about 5MB */
    settings.verbose = 0;
    settings.oldest_live = 0;
    settings.evict_to_free = 1;       /* push old items out of cache when memory runs out */
    settings.socketpath = NULL;       /* by default, not using a unix socket */
    settings.factor = 1.25;
    settings.chunk_size = 48;         /* space for a modest key and value */
    settings.num_threads = 4;         /* N workers */
    settings.num_threads_per_udp = 0;
    settings.prefix_delimiter = ':';
    settings.detail_enabled = 0;
    settings.reqs_per_event = 20;
    settings.backlog = 1024;
    settings.binding_protocol = negotiating_prot;
    settings.item_size_max = 1024 * 1024; /* The famous 1MB upper limit. */
    settings.maxconns_fast = false;
    settings.hashpower_init = 0;
    settings.slab_reassign = false;
    settings.slab_automove = 0;
}

/*
 * Adds a message header to a connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int add_msghdr(connection_t *c)
{
    struct msghdr *msg;

    assert(c != NULL);

    if (c->msgsize == c->msgused) {
        msg = realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
        if (! msg)
            return -1;
        c->msglist = msg;
        c->msgsize *= 2;
    }

    msg = c->msglist + c->msgused;

    /* this wipes msg_iovlen, msg_control, msg_controllen, and
       msg_flags, the last 3 of which aren't defined on solaris: */
    memset(msg, 0, sizeof(struct msghdr));

    msg->msg_iov = &c->iov[c->iovused];

    if (c->request_addr_size > 0) {
        msg->msg_name = &c->request_addr;
        msg->msg_namelen = c->request_addr_size;
    }

    c->msgbytes = 0;
    c->msgused++;

    if (IS_UDP(c->transport)) {
        /* Leave room for the UDP header, which we'll fill in later. */
        return add_iov(c, NULL, UDP_HEADER_SIZE);
    }

    return 0;
}


/*
 * Free list management for connections.
 */

static connection_t **freeconns;
static int freetotal;
static int freecurr;
/* Lock for connection freelist */
static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;


static void conn_init(void) {
    freetotal = 200;
    freecurr = 0;
    if ((freeconns = calloc(freetotal, sizeof(connection_t *))) == NULL) {
        fprintf(stderr, "Failed to allocate connection structures\n");
    }
    return;
}

/*
 * Returns a connection from the freelist, if any.
 */
connection_t *conn_from_freelist() {
    connection_t *c;

    pthread_mutex_lock(&conn_lock);
    if (freecurr > 0) {
        c = freeconns[--freecurr];
    } else {
        c = NULL;
    }
    pthread_mutex_unlock(&conn_lock);

    return c;
}

/*
 * Adds a connection to the freelist. 0 = success.
 */
bool conn_add_to_freelist(connection_t *c) {
    bool ret = true;
    pthread_mutex_lock(&conn_lock);
    if (freecurr < freetotal) {
        freeconns[freecurr++] = c;
        ret = false;
    } else {
        /* try to enlarge free connections array */
        size_t newsize = freetotal * 2;
        connection_t **new_freeconns = realloc(freeconns, sizeof(connection_t *) * newsize);
        if (new_freeconns) {
            freetotal = newsize;
            freeconns = new_freeconns;
            freeconns[freecurr++] = c;
            ret = false;
        }
    }
    pthread_mutex_unlock(&conn_lock);
    return ret;
}

connection_t * lldb_create_connection(
		const int sfd,
		connection_state_t init_state,
		const int event_flags,
		const unsigned int read_buffer_size,
		struct event_base *base)
{
    connection_t *c = conn_from_freelist();

    if (NULL == c) {
        if (!(c = (connection_t *)calloc(1, sizeof(connection_t)))) {
            fprintf(stderr, "calloc()\n");
            return NULL;
        }

        c->rbuff = c->wbuff = NULL;
        c->iov = NULL;
        c->msglist = NULL;

        c->ralloc = read_buffer_size;
        c->walloc = DATA_BUFFER_SIZE;
        c->iov_size = IOV_LIST_INITIAL;
        c->msg_size = MSG_LIST_INITIAL;

        c->rbuff = (char *)malloc(c->ralloc);
        c->wbuff = (char *)malloc(c->walloc);
        c->iov = (struct iovec *)malloc(sizeof(struct iovec) * c->iov_size);
        c->msglist = (struct msghdr *)malloc(sizeof(struct msghdr) * c->msg_size);

        if (c->rbuff == NULL || c->wbuff == NULL
				|| c->iov == NULL || c->msglist == 0) {
            lldb_free_connection(c);
            return NULL;
        }
    }
    
    c->sfd = sfd;
    c->state = init_state;
    c->cmd = -1;
    c->rleft = c->wleft= 0;
    c->wcurr = c->wbuff;
    c->rcurr = c->rbuff;
    c->iov_used = 0;
    c->msglist_curr = 0;
    c->msglist_used = 0;

    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->event_flags = event_flags;

    if (event_add(&c->event, 0) == -1) {
        if (conn_add_to_freelist(c)) {
            lldb_free_connection(c);
        }
        return NULL;
    }

    return c;
}

static void
lldb_cleanup_connection(connection_t *c)
{
    assert(c != NULL);
}

static void
lldb_free_connection(connection_t *c)
{
    if (c != NULL) {
        if (c->msglist) free(c->msglist);
        if (c->rbuff) free(c->rbuff);
        if (c->wbuff) free(c->wbuf);
        if (c->iov) free(c->iov);
        free(c);
    }
}

static void
lldb_close_connection(connection_t *c)
{
    assert(c != NULL);

    /* delete the event, the socket and the connection_t */
    event_del(&c->event);
    close(c->sfd);
    lldb_cleanup_connection(c);

    if (c->ralloc > READ_BUFFER_HIGHWAT || conn_add_to_freelist(c)) {
        lldb_free_connection(c);
    }
}

static void
lldb_shrink_connection(connection_t *c)
{
    assert(c != NULL);

    if (c->ralloc > READ_BUFFER_HIGHWAT && c->rleft < DATA_BUFFER_SIZE) {
        char *newbuf;

        if (c->rcurr != c->rbuf)
            memmove(c->rbuff, c->rcurr, c->rleft);

        newbuf = (char *)realloc((void *)c->rbuff, DATA_BUFFER_SIZE);

        if (newbuf != NULL) {
            c->rbuff = newbuf;
            c->ralloc = DATA_BUFFER_SIZE;
        }
        c->rcurr = c->rbuff;
    }

    if (c->msglist_size > MSG_LIST_HIGHWAT) {
        struct msghdr *newbuf = (struct msghdr *) realloc((void *)c->msglist,
				MSG_LIST_INITIAL * sizeof(c->msglist[0]));
        if (newbuf != NULL) {
            c->msglist = newbuf;
            c->msgsize = MSG_LIST_INITIAL;
        }
    }

    if (c->iov_size > IOV_LIST_HIGHWAT) {
        struct iovec *newbuf = (struct iovec *) realloc((void *)c->iov,
				IOV_LIST_INITIAL * sizeof(c->iov[0]));
        if (newbuf != NULL) {
            c->iov = newbuf;
            c->iov_size = IOV_LIST_INITIAL;
        }
    }
}

static const char *
lldb_convert_state_readable(connection_state_t state)
{
    const char* const state_names[] = {
		"LISTENING: listening for new connections",
		"NEWCMD: preparing connection for next commands",
		"PARSECMD: trying to parse a command",
		"WAITING: waiting for a readable socket",
		"READ: reading in a command line",
		"NREAD: reading in a fixed number of bytes",
		"WRITE: writing out a simple response",
		"MWRITE: Writing out many bytes sequentially"
		"SWALLOW: swallowing unnecessary bytes",
		"CLOSING: closing this connection",
	};
    return state_names[state];
}

static void
lldb_set_connection_state(connection_t *c, connection_state_t state)
{
    assert(c != NULL);
    assert(state >= LISTENING && state < MAXSTATE);

    if (state != c->state) {
        c->state = state;
    }
}

/*
 * ensure that there is enough room for another struct iovec
 * in a connection's iov list.
 *
 * return 0 on success, -1 if out-of-memory.
 */
static int
lldb_ensure_iov_space(connection_t *c)
{
    assert(c != NULL);

    if (c->iov_used >= c->iov_size) {
        int i, iovnum;
        struct iovec *new_iov = (struct iovec *)realloc(c->iov,
                                (c->iov_size * 2) * sizeof(struct iovec));
        if (new_iov == NULL) return -1;
        c->iov = new_iov;
        c->iov_size *= 2;

        for (i = 0, iovnum = 0; i < c->msglist_used; i++) {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}


/*
 * adds data to the list of pending datagram
 * that will be written out to a connection.
 *
 * returns 0 on success, -1 on out-of-memory.
 */

static int
lldb_add_iov(connection_t *c, const void *buf, int len)
{
    struct msghdr *m;
    int leftover;
    bool limit_to_mtu;

    assert(c != NULL);

    do {
        m = &c->msglist[c->msgused - 1];

        /*
         * Limit UDP packets, and the first payloads of TCP replies, to
         * UDP_MAX_PAYLOAD_SIZE bytes.
         */
        limit_to_mtu = IS_UDP(c->transport) || (1 == c->msgused);

        /* We may need to start a new msghdr if this one is full. */
        if (m->msg_iovlen == IOV_MAX ||
            (limit_to_mtu && c->msgbytes >= UDP_MAX_PAYLOAD_SIZE)) {
            add_msghdr(c);
            m = &c->msglist[c->msgused - 1];
        }

        if (ensure_iov_space(c) != 0)
            return -1;

        /* If the fragment is too big to fit in the datagram, split it up */
        if (limit_to_mtu && len + c->msgbytes > UDP_MAX_PAYLOAD_SIZE) {
            leftover = len + c->msgbytes - UDP_MAX_PAYLOAD_SIZE;
            len -= leftover;
        } else {
            leftover = 0;
        }

        m = &c->msglist[c->msgused - 1];
        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;

        c->msgbytes += len;
        c->iovused++;
        m->msg_iovlen++;

        buf = ((char *)buf) + len;
        len = leftover;
    } while (leftover > 0);

    return 0;
}


static void out_string(connection_t *c, const char *str) {
    size_t len;

    assert(c != NULL);

    if (c->noreply) {
        if (settings.verbose > 1)
            fprintf(stderr, ">%d NOREPLY %s\n", c->sfd, str);
        c->noreply = false;
        conn_set_state(c, conn_new_cmd);
        return;
    }

    if (settings.verbose > 1)
        fprintf(stderr, ">%d %s\n", c->sfd, str);

    /* Nuke a partial output... */
    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    add_msghdr(c);

    len = strlen(str);
    if ((len + 2) > c->wsize) {
        /* ought to be always enough. just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    memcpy(c->wbuf, str, len);
    memcpy(c->wbuf + len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr = c->wbuf;

    conn_set_state(c, conn_write);
    c->write_and_go = conn_new_cmd;
    return;
}

static bool update_event(connection_t *c, const int new_flags) {
    assert(c != NULL);

    struct event_base *base = c->event.ev_base;
    if (c->ev_flags == new_flags)
        return true;
    if (event_del(&c->event) == -1) return false;
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1) return false;
    return true;
}

/*
 * Sets whether we are listening for new connections or not.
 */
void do_accept_new_conns(const bool do_accept) {
    connection_t *next;

    for (next = listen_conn; next; next = next->next) {
        if (do_accept) {
            update_event(next, EV_READ | EV_PERSIST);
            if (listen(next->sfd, settings.backlog) != 0) {
                perror("listen");
            }
        }
        else {
            update_event(next, 0);
            if (listen(next->sfd, 0) != 0) {
                perror("listen");
            }
        }
    }

    if (do_accept) {
        STATS_LOCK();
        stats.accepting_conns = true;
        STATS_UNLOCK();
    } else {
        STATS_LOCK();
        stats.accepting_conns = false;
        stats.listen_disabled_num++;
        STATS_UNLOCK();
        allow_new_conns = false;
        maxconns_handler(-42, 0, 0);
    }
}

/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static enum transmit_result transmit(connection_t *c) {
    assert(c != NULL);

    if (c->msgcurr < c->msgused &&
            c->msglist[c->msgcurr].msg_iovlen == 0) {
        /* Finished writing the current msg; advance to the next. */
        c->msgcurr++;
    }
    if (c->msgcurr < c->msgused) {
        ssize_t res;
        struct msghdr *m = &c->msglist[c->msgcurr];

        res = sendmsg(c->sfd, m, 0);
        if (res > 0) {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.bytes_written += res;
            pthread_mutex_unlock(&c->thread->stats.mutex);

            /* We've written some of the data. Remove the completed
               iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len) {
                res -= m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry;
               adjust it so the next write will do the rest. */
            if (res > 0) {
                m->msg_iov->iov_base = (caddr_t)m->msg_iov->iov_base + res;
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }
        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                return TRANSMIT_HARD_ERROR;
            }
            return TRANSMIT_SOFT_ERROR;
        }
        /* if res == 0 or res == -1 and error is not EAGAIN or EWOULDBLOCK,
           we have a real error, on which we close the connection */
        if (settings.verbose > 0)
            perror("Failed to write, and not due to blocking");

        if (IS_UDP(c->transport))
            conn_set_state(c, conn_read);
        else
            conn_set_state(c, conn_closing);
        return TRANSMIT_HARD_ERROR;
    } else {
        return TRANSMIT_COMPLETE;
    }
}

static void drive_machine(connection_t *c) {
    bool stop = false;
    int sfd, flags = 1;
    socklen_t addrlen;
    struct sockaddr_storage addr;
    int nreqs = settings.reqs_per_event;
    int res;
    const char *str;

    assert(c != NULL);

    while (!stop) {

        switch(c->state) {
        case conn_listening:
            addrlen = sizeof(addr);
            if ((sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen)) == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* these are transient, so don't log anything */
                    stop = true;
                } else if (errno == EMFILE) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Too many open connections\n");
                    accept_new_conns(false);
                    stop = true;
                } else {
                    perror("accept()");
                    stop = true;
                }
                break;
            }
            if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
                fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
                perror("setting O_NONBLOCK");
                close(sfd);
                break;
            }

            if (settings.maxconns_fast &&
                stats.curr_conns + stats.reserved_fds >= settings.maxconns - 1) {
                str = "ERROR Too many open connections\r\n";
                res = write(sfd, str, strlen(str));
                close(sfd);
                STATS_LOCK();
                stats.rejected_conns++;
                STATS_UNLOCK();
            } else {
                dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST,
                                     DATA_BUFFER_SIZE, tcp_transport);
            }

            stop = true;
            break;

        case conn_waiting:
            if (!update_event(c, EV_READ | EV_PERSIST)) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                break;
            }

            conn_set_state(c, conn_read);
            stop = true;
            break;

        case conn_read:
            res = IS_UDP(c->transport) ? try_read_udp(c) : try_read_network(c);

            switch (res) {
            case READ_NO_DATA_RECEIVED:
                conn_set_state(c, conn_waiting);
                break;
            case READ_DATA_RECEIVED:
                conn_set_state(c, conn_parse_cmd);
                break;
            case READ_ERROR:
                conn_set_state(c, conn_closing);
                break;
            case READ_MEMORY_ERROR: /* Failed to allocate more memory */
                /* State already set by try_read_network */
                break;
            }
            break;

        case conn_parse_cmd :
            if (try_read_command(c) == 0) {
                /* wee need more data! */
                conn_set_state(c, conn_waiting);
            }

            break;

        case conn_new_cmd:
            /* Only process nreqs at a time to avoid starving other
               connections */

            --nreqs;
            if (nreqs >= 0) {
                reset_cmd_handler(c);
            } else {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.conn_yields++;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                if (c->rbytes > 0) {
                    /* We have already read in data into the input buffer,
                       so libevent will most likely not signal read events
                       on the socket (unless more data is available. As a
                       hack we should just put in a request to write data,
                       because that should be possible ;-)
                    */
                    if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                        if (settings.verbose > 0)
                            fprintf(stderr, "Couldn't update event\n");
                        conn_set_state(c, conn_closing);
                    }
                }
                stop = true;
            }
            break;

        case conn_nread:
            if (c->rlbytes == 0) {
                complete_nread(c);
                break;
            }
            /* first check if we have leftovers in the conn_read buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
                if (c->ritem != c->rcurr) {
                    memmove(c->ritem, c->rcurr, tocopy);
                }
                c->ritem += tocopy;
                c->rlbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                if (c->rlbytes == 0) {
                    break;
                }
            }

            /*  now try reading from the socket */
            res = read(c->sfd, c->ritem, c->rlbytes);
            if (res > 0) {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.bytes_read += res;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                if (c->rcurr == c->ritem) {
                    c->rcurr += res;
                }
                c->ritem += res;
                c->rlbytes -= res;
                break;
            }
            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0) {
                fprintf(stderr, "Failed to read, and not due to blocking:\n"
                        "errno: %d %s \n"
                        "rcurr=%lx ritem=%lx rbuf=%lx rlbytes=%d rsize=%d\n",
                        errno, strerror(errno),
                        (long)c->rcurr, (long)c->ritem, (long)c->rbuf,
                        (int)c->rlbytes, (int)c->rsize);
            }
            conn_set_state(c, conn_closing);
            break;

        case conn_swallow:
            /* we are reading sbytes and throwing them away */
            if (c->sbytes == 0) {
                conn_set_state(c, conn_new_cmd);
                break;
            }

            /* first check if we have leftovers in the conn_read buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
                c->sbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                break;
            }

            /*  now try reading from the socket */
            res = read(c->sfd, c->rbuf, c->rsize > c->sbytes ? c->sbytes : c->rsize);
            if (res > 0) {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.bytes_read += res;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                c->sbytes -= res;
                break;
            }
            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0)
                fprintf(stderr, "Failed to read, and not due to blocking\n");
            conn_set_state(c, conn_closing);
            break;

        case conn_write:
            /*
             * We want to write out a simple response. If we haven't already,
             * assemble it into a msgbuf list (this will be a single-entry
             * list for TCP or a two-entry list for UDP).
             */
            if (c->iovused == 0 || (IS_UDP(c->transport) && c->iovused == 1)) {
                if (add_iov(c, c->wcurr, c->wbytes) != 0) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't build response\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
            }

            /* fall through... */

        case conn_mwrite:
          if (IS_UDP(c->transport) && c->msgcurr == 0 && build_udp_headers(c) != 0) {
            if (settings.verbose > 0)
              fprintf(stderr, "Failed to build UDP headers\n");
            conn_set_state(c, conn_closing);
            break;
          }
            switch (transmit(c)) {
            case TRANSMIT_COMPLETE:
                if (c->state == conn_mwrite) {
                    while (c->ileft > 0) {
                        item *it = *(c->icurr);
                        assert((it->it_flags & ITEM_SLABBED) == 0);
                        item_remove(it);
                        c->icurr++;
                        c->ileft--;
                    }
                    while (c->suffixleft > 0) {
                        char *suffix = *(c->suffixcurr);
                        cache_free(c->thread->suffix_cache, suffix);
                        c->suffixcurr++;
                        c->suffixleft--;
                    }
                    /* XXX:  I don't know why this wasn't the general case */
                    if(c->protocol == binary_prot) {
                        conn_set_state(c, c->write_and_go);
                    } else {
                        conn_set_state(c, conn_new_cmd);
                    }
                } else if (c->state == conn_write) {
                    if (c->write_and_free) {
                        free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    conn_set_state(c, c->write_and_go);
                } else {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Unexpected state %d\n", c->state);
                    conn_set_state(c, conn_closing);
                }
                break;

            case TRANSMIT_INCOMPLETE:
            case TRANSMIT_HARD_ERROR:
                break;                   /* Continue in state machine. */

            case TRANSMIT_SOFT_ERROR:
                stop = true;
                break;
            }
            break;

        case conn_closing:
            if (IS_UDP(c->transport))
                conn_cleanup(c);
            else
                conn_close(c);
            stop = true;
            break;

        case conn_max_state:
            assert(false);
            break;
        }
    }

    return;
}

void event_handler(const int fd, const short which, void *arg) {
    connection_t *c;

    c = (connection_t *)arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd) {
        if (settings.verbose > 0)
            fprintf(stderr, "Catastrophic: event fd doesn't match connection_t fd!\n");
        conn_close(c);
        return;
    }

    drive_machine(c);

    /* wait for next event */
    return;
}

static int new_socket(struct addrinfo *ai) {
    int sfd;
    int flags;

    if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}


/*
 * Sets a socket's send buffer size to the maximum allowed by the system.
 */
static void maximize_sndbuf(const int sfd) {
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;

    /* Start with the default size. */
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &old_size, &intsize) != 0) {
        if (settings.verbose > 0)
            perror("getsockopt(SO_SNDBUF)");
        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size;
    max = MAX_SENDBUF_SIZE;

    while (min <= max) {
        avg = ((unsigned int)(min + max)) / 2;
        if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0) {
            last_good = avg;
            min = avg + 1;
        } else {
            max = avg - 1;
        }
    }

    if (settings.verbose > 1)
        fprintf(stderr, "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
}

/**
 * Create a socket and bind it to a specific port number
 * @param interface the interface to bind to
 * @param port the port number to bind to
 * @param transport the transport protocol (TCP / UDP)
 * @param portnumber_file A filepointer to write the port numbers to
 *        when they are successfully added to the list of ports we
 *        listen on.
 */
static int server_socket(const char *interface,
                         int port,
                         enum network_transport transport,
                         FILE *portnumber_file) {
    int sfd;
    struct linger ling = {0, 0};
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints = { .ai_flags = AI_PASSIVE,
                              .ai_family = AF_UNSPEC };
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;
    int flags =1;

    hints.ai_socktype = IS_UDP(transport) ? SOCK_DGRAM : SOCK_STREAM;

    if (port == -1) {
        port = 0;
    }
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    error= getaddrinfo(interface, port_buf, &hints, &ai);
    if (error != 0) {
        if (error != EAI_SYSTEM)
          fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
        else
          perror("getaddrinfo()");
        return 1;
    }

    for (next= ai; next; next= next->ai_next) {
        connection_t *listen_conn_add;
        if ((sfd = new_socket(next)) == -1) {
            /* getaddrinfo can return "junk" addresses,
             * we make sure at least one works before erroring.
             */
            if (errno == EMFILE) {
                /* ...unless we're out of fds */
                perror("server_socket");
                exit(EX_OSERR);
            }
            continue;
        }

#ifdef IPV6_V6ONLY
        if (next->ai_family == AF_INET6) {
            error = setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &flags, sizeof(flags));
            if (error != 0) {
                perror("setsockopt");
                close(sfd);
                continue;
            }
        }
#endif

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
        if (IS_UDP(transport)) {
            maximize_sndbuf(sfd);
        } else {
            error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
            if (error != 0)
                perror("setsockopt");

            error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
            if (error != 0)
                perror("setsockopt");

            error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
            if (error != 0)
                perror("setsockopt");
        }

        if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1) {
            if (errno != EADDRINUSE) {
                perror("bind()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            close(sfd);
            continue;
        } else {
            success++;
            if (!IS_UDP(transport) && listen(sfd, settings.backlog) == -1) {
                perror("listen()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            if (portnumber_file != NULL &&
                (next->ai_addr->sa_family == AF_INET ||
                 next->ai_addr->sa_family == AF_INET6)) {
                union {
                    struct sockaddr_in in;
                    struct sockaddr_in6 in6;
                } my_sockaddr;
                socklen_t len = sizeof(my_sockaddr);
                if (getsockname(sfd, (struct sockaddr*)&my_sockaddr, &len)==0) {
                    if (next->ai_addr->sa_family == AF_INET) {
                        fprintf(portnumber_file, "%s INET: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in.sin_port));
                    } else {
                        fprintf(portnumber_file, "%s INET6: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in6.sin6_port));
                    }
                }
            }
        }

        if (IS_UDP(transport)) {
            int c;

            for (c = 0; c < settings.num_threads_per_udp; c++) {
                /* this is guaranteed to hit all threads because we round-robin */
                dispatch_conn_new(sfd, conn_read, EV_READ | EV_PERSIST,
                                  UDP_READ_BUFFER_SIZE, transport);
            }
        } else {
            if (!(listen_conn_add = conn_new(sfd, conn_listening,
                                             EV_READ | EV_PERSIST, 1,
                                             transport, main_base))) {
                fprintf(stderr, "failed to create listening connection\n");
                exit(EXIT_FAILURE);
            }
            listen_conn_add->next = listen_conn;
            listen_conn = listen_conn_add;
        }
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

static int server_sockets(int port, enum network_transport transport,
                          FILE *portnumber_file) {
    if (settings.inter == NULL) {
        return server_socket(settings.inter, port, transport, portnumber_file);
    } else {
        // tokenize them and bind to each one of them..
        char *b;
        int ret = 0;
        char *list = strdup(settings.inter);

        if (list == NULL) {
            fprintf(stderr, "Failed to allocate memory for parsing server interface string\n");
            return 1;
        }
        for (char *p = strtok_r(list, ";,", &b);
             p != NULL;
             p = strtok_r(NULL, ";,", &b)) {
            int the_port = port;
            char *s = strchr(p, ':');
            if (s != NULL) {
                *s = '\0';
                ++s;
                if (!safe_strtol(s, &the_port)) {
                    fprintf(stderr, "Invalid port number: \"%s\"", s);
                    return 1;
                }
            }
            if (strcmp(p, "*") == 0) {
                p = NULL;
            }
            ret |= server_socket(p, the_port, transport, portnumber_file);
        }
        free(list);
        return ret;
    }
}


/*
 * We keep the current time of day in a global variable that's updated by a
 * timer event. This saves us a bunch of time() system calls (we really only
 * need to get the time once a second, whereas there can be tens of thousands
 * of requests a second) and allows us to use server-start-relative timestamps
 * rather than absolute UNIX timestamps, a space savings on systems where
 * sizeof(time_t) > sizeof(unsigned int).
 */
volatile rel_time_t current_time;
static struct event clockevent;

/* libevent uses a monotonic clock when available for event scheduling. Aside
 * from jitter, simply ticking our internal timer here is accurate enough.
 * Note that users who are setting explicit dates for expiration times *must*
 * ensure their clocks are correct before starting memcached. */
static void clock_handler(const int fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 1, .tv_usec = 0};
    static bool initialized = false;
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
    static bool monotonic = false;
    static time_t monotonic_start;
#endif

    if (initialized) {
        /* only delete the event if it's actually there. */
        evtimer_del(&clockevent);
    } else {
        initialized = true;
        /* process_started is initialized to time() - 2. We initialize to 1 so
         * flush_all won't underflow during tests. */
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            monotonic = true;
            monotonic_start = ts.tv_sec - 2;
        }
#endif
    }

    evtimer_set(&clockevent, clock_handler, 0);
    event_base_set(main_base, &clockevent);
    evtimer_add(&clockevent, &t);

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
    if (monotonic) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
            return;
        current_time = (rel_time_t) (ts.tv_sec - monotonic_start);
        return;
    }
#endif
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        current_time = (rel_time_t) (tv.tv_sec - process_started);
    }
}

static void save_pid(const char *pid_file) {
    FILE *fp;
    if (access(pid_file, F_OK) == 0) {
        if ((fp = fopen(pid_file, "r")) != NULL) {
            char buffer[1024];
            if (fgets(buffer, sizeof(buffer), fp) != NULL) {
                unsigned int pid;
                if (safe_strtoul(buffer, &pid) && kill((pid_t)pid, 0) == 0) {
                    fprintf(stderr, "WARNING: The pid file contained the following (running) pid: %u\n", pid);
                }
            }
            fclose(fp);
        }
    }

    if ((fp = fopen(pid_file, "w")) == NULL) {
        vperror("Could not open the pid file %s for writing", pid_file);
        return;
    }

    fprintf(fp,"%ld\n", (long)getpid());
    if (fclose(fp) == -1) {
        vperror("Could not close the pid file %s", pid_file);
    }
}

static void remove_pidfile(const char *pid_file) {
  if (pid_file == NULL)
      return;

  if (unlink(pid_file) != 0) {
      vperror("Could not remove the pid file %s", pid_file);
  }

}

static void sig_handler(const int sig) {
    printf("SIGINT handled.\n");
    exit(EXIT_SUCCESS);
}

#ifndef HAVE_SIGIGNORE
static int sigignore(int sig) {
    struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = 0 };

    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(sig, &sa, 0) == -1) {
        return -1;
    }
    return 0;
}
#endif


/*
 * On systems that supports multiple page sizes we may reduce the
 * number of TLB-misses by using the biggest available page size
 */
static int enable_large_pages(void) {
#if defined(HAVE_GETPAGESIZES) && defined(HAVE_MEMCNTL)
    int ret = -1;
    size_t sizes[32];
    int avail = getpagesizes(sizes, 32);
    if (avail != -1) {
        size_t max = sizes[0];
        struct memcntl_mha arg = {0};
        int ii;

        for (ii = 1; ii < avail; ++ii) {
            if (max < sizes[ii]) {
                max = sizes[ii];
            }
        }

        arg.mha_flags   = 0;
        arg.mha_pagesize = max;
        arg.mha_cmd = MHA_MAPSIZE_BSSBRK;

        if (memcntl(0, 0, MC_HAT_ADVISE, (caddr_t)&arg, 0, 0) == -1) {
            fprintf(stderr, "Failed to set large pages: %s\n",
                    strerror(errno));
            fprintf(stderr, "Will use default page size\n");
        } else {
            ret = 0;
        }
    } else {
        fprintf(stderr, "Failed to get supported pagesizes: %s\n",
                strerror(errno));
        fprintf(stderr, "Will use default page size\n");
    }

    return ret;
#else
    return -1;
#endif
}

/**
 * Do basic sanity check of the runtime environment
 * @return true if no errors found, false if we can't use this env
 */
static bool sanitycheck(void) {
    /* One of our biggest problems is old and bogus libevents */
    const char *ever = event_get_version();
    if (ever != NULL) {
        if (strncmp(ever, "1.", 2) == 0) {
            /* Require at least 1.3 (that's still a couple of years old) */
            if ((ever[2] == '1' || ever[2] == '2') && !isdigit(ever[3])) {
                fprintf(stderr, "You are using libevent %s.\nPlease upgrade to"
                        " a more recent version (1.3 or newer)\n",
                        event_get_version());
                return false;
            }
        }
    }

    return true;
}

int main (int argc, char **argv) {
    int c;
    bool lock_memory = false;
    bool do_daemonize = false;
    bool preallocate = false;
    int maxcore = 0;
    char *username = NULL;
    char *pid_file = NULL;
    struct passwd *pw;
    struct rlimit rlim;
    char unit = '\0';
    int size_max = 0;
    int retval = EXIT_SUCCESS;
    /* listening sockets */
    static int *l_socket = NULL;

    /* udp socket */
    static int *u_socket = NULL;
    bool protocol_specified = false;
    bool tcp_specified = false;
    bool udp_specified = false;

    char *subopts;
    char *subopts_value;
    enum {
        MAXCONNS_FAST = 0,
        HASHPOWER_INIT,
        SLAB_REASSIGN,
        SLAB_AUTOMOVE
    };
    char *const subopts_tokens[] = {
        [MAXCONNS_FAST] = "maxconns_fast",
        [HASHPOWER_INIT] = "hashpower",
        [SLAB_REASSIGN] = "slab_reassign",
        [SLAB_AUTOMOVE] = "slab_automove",
        NULL
    };

    if (!sanitycheck()) {
        return EX_OSERR;
    }

    /* handle SIGINT */
    signal(SIGINT, sig_handler);

    /* init settings */
    settings_init();

    /* set stderr non-buffering (for running under, say, daemontools) */
    setbuf(stderr, NULL);

    /* process arguments */
    while (-1 != (c = getopt(argc, argv,
          "a:"  /* access mask for unix socket */
          "p:"  /* TCP port number to listen on */
          "s:"  /* unix socket path to listen on */
          "U:"  /* UDP port number to listen on */
          "m:"  /* max memory to use for items in megabytes */
          "M"   /* return error on memory exhausted */
          "c:"  /* max simultaneous connections */
          "k"   /* lock down all paged memory */
          "hi"  /* help, licence info */
          "r"   /* maximize core file limit */
          "v"   /* verbose */
          "d"   /* daemon mode */
          "l:"  /* interface to listen on */
          "u:"  /* user identity to run as */
          "P:"  /* save PID in file */
          "f:"  /* factor? */
          "n:"  /* minimum space allocated for key+value+flags */
          "t:"  /* threads */
          "D:"  /* prefix delimiter? */
          "L"   /* Large memory pages */
          "R:"  /* max requests per event */
          "C"   /* Disable use of CAS */
          "b:"  /* backlog queue limit */
          "B:"  /* Binding protocol */
          "I:"  /* Max item size */
          "S"   /* Sasl ON */
          "o:"  /* Extended generic options */
        ))) {
        switch (c) {
        case 'a':
            /* access for unix domain socket, as octal mask (like chmod)*/
            settings.access= strtol(optarg,NULL,8);
            break;

        case 'U':
            settings.udpport = atoi(optarg);
            udp_specified = true;
            break;
        case 'p':
            settings.port = atoi(optarg);
            tcp_specified = true;
            break;
        case 's':
            settings.socketpath = optarg;
            break;
        case 'm':
            settings.maxbytes = ((size_t)atoi(optarg)) * 1024 * 1024;
            break;
        case 'M':
            settings.evict_to_free = 0;
            break;
        case 'c':
            settings.maxconns = atoi(optarg);
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 'i':
            usage_license();
            exit(EXIT_SUCCESS);
        case 'k':
            lock_memory = true;
            break;
        case 'v':
            settings.verbose++;
            break;
        case 'l':
            if (settings.inter != NULL) {
                size_t len = strlen(settings.inter) + strlen(optarg) + 2;
                char *p = malloc(len);
                if (p == NULL) {
                    fprintf(stderr, "Failed to allocate memory\n");
                    return 1;
                }
                snprintf(p, len, "%s,%s", settings.inter, optarg);
                free(settings.inter);
                settings.inter = p;
            } else {
                settings.inter= strdup(optarg);
            }
            break;
        case 'd':
            do_daemonize = true;
            break;
        case 'r':
            maxcore = 1;
            break;
        case 'R':
            settings.reqs_per_event = atoi(optarg);
            if (settings.reqs_per_event == 0) {
                fprintf(stderr, "Number of requests per event must be greater than 0\n");
                return 1;
            }
            break;
        case 'u':
            username = optarg;
            break;
        case 'P':
            pid_file = optarg;
            break;
        case 'f':
            settings.factor = atof(optarg);
            if (settings.factor <= 1.0) {
                fprintf(stderr, "Factor must be greater than 1\n");
                return 1;
            }
            break;
        case 'n':
            settings.chunk_size = atoi(optarg);
            if (settings.chunk_size == 0) {
                fprintf(stderr, "Chunk size must be greater than 0\n");
                return 1;
            }
            break;
        case 't':
            settings.num_threads = atoi(optarg);
            if (settings.num_threads <= 0) {
                fprintf(stderr, "Number of threads must be greater than 0\n");
                return 1;
            }
            /* There're other problems when you get above 64 threads.
             * In the future we should portably detect # of cores for the
             * default.
             */
            if (settings.num_threads > 64) {
                fprintf(stderr, "WARNING: Setting a high number of worker"
                                "threads is not recommended.\n"
                                " Set this value to the number of cores in"
                                " your machine or less.\n");
            }
            break;
        case 'D':
            if (! optarg || ! optarg[0]) {
                fprintf(stderr, "No delimiter specified\n");
                return 1;
            }
            settings.prefix_delimiter = optarg[0];
            settings.detail_enabled = 1;
            break;
        case 'L' :
            if (enable_large_pages() == 0) {
                preallocate = true;
            } else {
                fprintf(stderr, "Cannot enable large pages on this system\n"
                    "(There is no Linux support as of this version)\n");
                return 1;
            }
            break;
        case 'C' :
            settings.use_cas = false;
            break;
        case 'b' :
            settings.backlog = atoi(optarg);
            break;
        case 'B':
            protocol_specified = true;
            if (strcmp(optarg, "auto") == 0) {
                settings.binding_protocol = negotiating_prot;
            } else if (strcmp(optarg, "binary") == 0) {
                settings.binding_protocol = binary_prot;
            } else if (strcmp(optarg, "ascii") == 0) {
                settings.binding_protocol = ascii_prot;
            } else {
                fprintf(stderr, "Invalid value for binding protocol: %s\n"
                        " -- should be one of auto, binary, or ascii\n", optarg);
                exit(EX_USAGE);
            }
            break;
        case 'I':
            unit = optarg[strlen(optarg)-1];
            if (unit == 'k' || unit == 'm' ||
                unit == 'K' || unit == 'M') {
                optarg[strlen(optarg)-1] = '\0';
                size_max = atoi(optarg);
                if (unit == 'k' || unit == 'K')
                    size_max *= 1024;
                if (unit == 'm' || unit == 'M')
                    size_max *= 1024 * 1024;
                settings.item_size_max = size_max;
            } else {
                settings.item_size_max = atoi(optarg);
            }
            if (settings.item_size_max < 1024) {
                fprintf(stderr, "Item max size cannot be less than 1024 bytes.\n");
                return 1;
            }
            if (settings.item_size_max > 1024 * 1024 * 128) {
                fprintf(stderr, "Cannot set item size limit higher than 128 mb.\n");
                return 1;
            }
            if (settings.item_size_max > 1024 * 1024) {
                fprintf(stderr, "WARNING: Setting item max size above 1MB is not"
                    " recommended!\n"
                    " Raising this limit increases the minimum memory requirements\n"
                    " and will decrease your memory efficiency.\n"
                );
            }
            break;
        case 'S': /* set Sasl authentication to true. Default is false */
#ifndef ENABLE_SASL
            fprintf(stderr, "This server is not built with SASL support.\n");
            exit(EX_USAGE);
#endif
            settings.sasl = true;
            break;
        case 'o': /* It's sub-opts time! */
            subopts = optarg;

            while (*subopts != '\0') {

            switch (getsubopt(&subopts, subopts_tokens, &subopts_value)) {
            case MAXCONNS_FAST:
                settings.maxconns_fast = true;
                break;
            case HASHPOWER_INIT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing numeric argument for hashpower\n");
                    return 1;
                }
                settings.hashpower_init = atoi(subopts_value);
                if (settings.hashpower_init < 12) {
                    fprintf(stderr, "Initial hashtable multiplier of %d is too low\n",
                        settings.hashpower_init);
                    return 1;
                } else if (settings.hashpower_init > 64) {
                    fprintf(stderr, "Initial hashtable multiplier of %d is too high\n"
                        "Choose a value based on \"STAT hash_power_level\" from a running instance\n",
                        settings.hashpower_init);
                    return 1;
                }
                break;
            case SLAB_REASSIGN:
                settings.slab_reassign = true;
                break;
            case SLAB_AUTOMOVE:
                if (subopts_value == NULL) {
                    settings.slab_automove = 1;
                    break;
                }
                settings.slab_automove = atoi(subopts_value);
                if (settings.slab_automove < 0 || settings.slab_automove > 2) {
                    fprintf(stderr, "slab_automove must be between 0 and 2\n");
                    return 1;
                }
                break;
            default:
                printf("Illegal suboption \"%s\"\n", subopts_value);
                return 1;
            }

            }
            break;
        default:
            fprintf(stderr, "Illegal argument \"%c\"\n", c);
            return 1;
        }
    }

    /*
     * Use one workerthread to serve each UDP port if the user specified
     * multiple ports
     */
    if (settings.inter != NULL && strchr(settings.inter, ',')) {
        settings.num_threads_per_udp = 1;
    } else {
        settings.num_threads_per_udp = settings.num_threads;
    }

    if (settings.sasl) {
        if (!protocol_specified) {
            settings.binding_protocol = binary_prot;
        } else {
            if (settings.binding_protocol != binary_prot) {
                fprintf(stderr, "ERROR: You cannot allow the ASCII protocol while using SASL.\n");
                exit(EX_USAGE);
            }
        }
    }

    if (tcp_specified && !udp_specified) {
        settings.udpport = settings.port;
    } else if (udp_specified && !tcp_specified) {
        settings.port = settings.udpport;
    }

    if (maxcore != 0) {
        struct rlimit rlim_new;
        /*
         * First try raising to infinity; if that fails, try bringing
         * the soft limit to the hard.
         */
        if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
            rlim_new.rlim_cur = rlim_new.rlim_max = RLIM_INFINITY;
            if (setrlimit(RLIMIT_CORE, &rlim_new)!= 0) {
                /* failed. try raising just to the old max */
                rlim_new.rlim_cur = rlim_new.rlim_max = rlim.rlim_max;
                (void)setrlimit(RLIMIT_CORE, &rlim_new);
            }
        }
        /*
         * getrlimit again to see what we ended up with. Only fail if
         * the soft limit ends up 0, because then no core files will be
         * created at all.
         */

        if ((getrlimit(RLIMIT_CORE, &rlim) != 0) || rlim.rlim_cur == 0) {
            fprintf(stderr, "failed to ensure corefile creation\n");
            exit(EX_OSERR);
        }
    }

    /*
     * If needed, increase rlimits to allow as many connections
     * as needed.
     */

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fprintf(stderr, "failed to getrlimit number of files\n");
        exit(EX_OSERR);
    } else {
        rlim.rlim_cur = settings.maxconns;
        rlim.rlim_max = settings.maxconns;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            fprintf(stderr, "failed to set rlimit for open files. Try starting as root or requesting smaller maxconns value.\n");
            exit(EX_OSERR);
        }
    }

    /* lose root privileges if we have them */
    if (getuid() == 0 || geteuid() == 0) {
        if (username == 0 || *username == '\0') {
            fprintf(stderr, "can't run as root without the -u switch\n");
            exit(EX_USAGE);
        }
        if ((pw = getpwnam(username)) == 0) {
            fprintf(stderr, "can't find the user %s to switch to\n", username);
            exit(EX_NOUSER);
        }
        if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            fprintf(stderr, "failed to assume identity of user %s\n", username);
            exit(EX_OSERR);
        }
    }

    /* Initialize Sasl if -S was specified */
    if (settings.sasl) {
        init_sasl();
    }

    /* daemonize if requested */
    /* if we want to ensure our ability to dump core, don't chdir to / */
    if (do_daemonize) {
        if (sigignore(SIGHUP) == -1) {
            perror("Failed to ignore SIGHUP");
        }
        if (daemonize(maxcore, settings.verbose) == -1) {
            fprintf(stderr, "failed to daemon() in order to daemonize\n");
            exit(EXIT_FAILURE);
        }
    }

    /* lock paged memory if needed */
    if (lock_memory) {
#ifdef HAVE_MLOCKALL
        int res = mlockall(MCL_CURRENT | MCL_FUTURE);
        if (res != 0) {
            fprintf(stderr, "warning: -k invalid, mlockall() failed: %s\n",
                    strerror(errno));
        }
#else
        fprintf(stderr, "warning: -k invalid, mlockall() not supported on this platform.  proceeding without.\n");
#endif
    }

    /* initialize main thread libevent instance */
    main_base = event_init();

    /* initialize other stuff */
    stats_init();
    assoc_init(settings.hashpower_init);
    conn_init();
    slabs_init(settings.maxbytes, settings.factor, preallocate);

    /*
     * ignore SIGPIPE signals; we can use errno == EPIPE if we
     * need that information
     */
    if (sigignore(SIGPIPE) == -1) {
        perror("failed to ignore SIGPIPE; sigaction");
        exit(EX_OSERR);
    }
    /* start up worker threads if MT mode */
    thread_init(settings.num_threads, main_base);

    if (start_assoc_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    if (settings.slab_reassign &&
        start_slab_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    /* initialise clock event */
    clock_handler(0, 0, 0);

    /* create unix mode sockets after dropping privileges */
    if (settings.socketpath != NULL) {
        errno = 0;
        if (server_socket_unix(settings.socketpath,settings.access)) {
            vperror("failed to listen on UNIX socket: %s", settings.socketpath);
            exit(EX_OSERR);
        }
    }

    /* create the listening socket, bind it, and init */
    if (settings.socketpath == NULL) {
        const char *portnumber_filename = getenv("MEMCACHED_PORT_FILENAME");
        char temp_portnumber_filename[PATH_MAX];
        FILE *portnumber_file = NULL;

        if (portnumber_filename != NULL) {
            snprintf(temp_portnumber_filename,
                     sizeof(temp_portnumber_filename),
                     "%s.lck", portnumber_filename);

            portnumber_file = fopen(temp_portnumber_filename, "a");
            if (portnumber_file == NULL) {
                fprintf(stderr, "Failed to open \"%s\": %s\n",
                        temp_portnumber_filename, strerror(errno));
            }
        }

        errno = 0;
        if (settings.port && server_sockets(settings.port, tcp_transport,
                                           portnumber_file)) {
            vperror("failed to listen on TCP port %d", settings.port);
            exit(EX_OSERR);
        }

        /*
         * initialization order: first create the listening sockets
         * (may need root on low ports), then drop root if needed,
         * then daemonise if needed, then init libevent (in some cases
         * descriptors created by libevent wouldn't survive forking).
         */

        /* create the UDP listening socket and bind it */
        errno = 0;
        if (settings.udpport && server_sockets(settings.udpport, udp_transport,
                                              portnumber_file)) {
            vperror("failed to listen on UDP port %d", settings.udpport);
            exit(EX_OSERR);
        }

        if (portnumber_file) {
            fclose(portnumber_file);
            rename(temp_portnumber_filename, portnumber_filename);
        }
    }

    /* Give the sockets a moment to open. I know this is dumb, but the error
     * is only an advisory.
     */
    usleep(1000);
    if (stats.curr_conns + stats.reserved_fds >= settings.maxconns - 1) {
        fprintf(stderr, "Maxconns setting is too low, use -c to increase.\n");
        exit(EXIT_FAILURE);
    }

    if (pid_file != NULL) {
        save_pid(pid_file);
    }

    /* Drop privileges no longer needed */
    drop_privileges();

    /* enter the event loop */
    if (event_base_loop(main_base, 0) != 0) {
        retval = EXIT_FAILURE;
    }

    stop_assoc_maintenance_thread();

    /* remove the PID file if we're a daemon */
    if (do_daemonize)
        remove_pidfile(pid_file);
    /* Clean up strdup() call for bind() address */
    if (settings.inter)
      free(settings.inter);
    if (l_socket)
      free(l_socket);
    if (u_socket)
      free(u_socket);

    return retval;
}
