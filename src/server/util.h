/*
 * =============================================================================
 *
 *       Filename:  util.h
 *
 *    Description:  miscs utility.
 *
 *        Created:  10/21/2012 10:04:16 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#ifndef LLDB_UTIL_H
#define LLDB_UTIL_H

extern unsigned short util_refcount_incr(unsigned short *refcount);
extern unsigned short util_refcount_decr(unsigned short *refcount);

#endif // LLDB_UTIL_H
