/*
 * =============================================================================
 *
 *       Filename:  util.c
 *
 *    Description:  miscs utility.
 *
 *        Created:  10/21/2012 10:04:32 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "util.h"

#if !defined(HAVE_GCC_ATOMICS)
pthread_mutex_t atomics_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

unsigned short
util_refcount_incr(unsigned short *refcount)
{
#ifdef HAVE_GCC_ATOMICS
    return __sync_add_and_fetch(refcount, 1);
#else
    unsigned short res;
    pthread_mutex_lock(&atomics_mutex);
    (*refcount)++;
    res = *refcount;
    pthread_mutex_unlock(&atomics_mutex);
    return res;
#endif
}

unsigned short
util_refcount_decr(unsigned short *refcount)
{
#ifdef HAVE_GCC_ATOMICS
    return __sync_sub_and_fetch(refcount, 1);
#else
    unsigned short res;
    pthread_mutex_lock(&atomics_mutex);
    (*refcount)--;
    res = *refcount;
    pthread_mutex_unlock(&atomics_mutex);
    return res;
#endif
}

