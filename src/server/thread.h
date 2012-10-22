/*
 * =============================================================================
 *
 *       Filename:  thread.h
 *
 *    Description:  thread utility.
 *
 *        Created:  10/18/2012 02:34:22 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#ifndef LLDB_THREAD_H
#define LLDB_THREAD_H

#include "lldbserver.h"

void lldb_dispatch_new_connection(
		int sfd, connection_states_t init_state,
		int event_flags, int read_buffer_size);

int lldb_is_listening_thread();

void lldb_initialize_worker_threads(int nthreads, struct event_base *main_base);

#endif // LLDB_THREAD_H
