/*
 * =============================================================================
 *
 *       Filename:  log.h
 *
 *    Description:  log utility.
 *
 *        Created:  10/20/2012 10:06:52 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#ifndef LLDB_LOG_H
#define LLDB_LOG_H

#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _log_level_e log_level_t;

enum _log_level_e {
	// LLDB_LOG_LEVEL_QUIET = 0,
	LLDB_LOG_LEVEL_ERROR = 1,
	LLDB_LOG_LEVEL_WARN  = 2,
	LLDB_LOG_LEVEL_INFO  = 3,
	LLDB_LOG_LEVEL_DEBUG = 4
};

extern log_level_t log_level;

#define LOGSTREAM log_get_stream()

#define LOG_ERROR(x) if (log_level >= LLDB_LOG_LEVEL_ERROR) \
    log_message(LLDB_LOG_LEVEL_ERROR, __LINE__, __func__, log_format_message x)
#define LOG_WARN(x) if(log_level >= LLDB_LOG_LEVEL_WARN) \
    log_message(LLDB_LOG_LEVEL_WARN, __LINE__, __func__, log_format_message x)
#define LOG_INFO(x) if(log_level >= LLDB_LOG_LEVEL_INFO) \
    log_message(LLDB_LOG_LEVEL_INFO, __LINE__, __func__, log_format_message x)
#define LOG_DEBUG(x) if(log_level == LLDB_LOG_LEVEL_DEBUG) \
    log_message(LLDB_LOG_LEVEL_DEBUG, __LINE__, __func__, log_format_message x)

extern void log_message(
		log_level_t level, int line,
		const char* funcname,
		const char* message);
extern const char * log_format_message(const char* format, ...);
extern FILE * log_get_stream(void);
extern void log_set_stream(FILE *stream);
extern void log_set_debug_level(log_level_t level);

#ifdef __cplusplus
}
#endif

#endif /*LLDB_LOG_H*/

