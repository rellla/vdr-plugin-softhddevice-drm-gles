///
///	@file misc.h	@brief Misc function header file
///
///	Copyright (c) 2009 - 2012 by Lutz Sammer.  All Rights Reserved.
///
///	Contributor(s):
///		Copied from uwm.
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

/// @addtogroup misc
/// @{
extern int SysLogLevel;			///< how much information wanted

#ifdef __cplusplus
extern "C"
{
#endif

#include <syslog.h>
#include <stdarg.h>
#include <time.h>			// clock_gettime
#include <sys/syscall.h>

//////////////////////////////////////////////////////////////////////////////
//	Defines
//////////////////////////////////////////////////////////////////////////////

#define L_DEBUG            (1 << 0)
#define L_AV_SYNC          (1 << 1)
#define L_SOUND            (1 << 2)
#define L_OSD              (1 << 3)
#define L_DRM              (1 << 4)
#define L_CODEC            (1 << 5)
#define L_DEINT            (1 << 6)
#define L_STILL            (1 << 7)
#define L_MEDIA            (1 << 8)
#define L_OPENGL           (1 << 9)
#define L_OPENGL_TIME      (1 << 10)
#define L_OPENGL_TIME_ALL  (1 << 11)

typedef unsigned char uchar;

/**
**	Show error.
*/
#define Error(fmt...) (void)( (SysLogLevel > 0) ? Syslog(LOG_ERR, 0, fmt) : (void)0 )

/**
**	Show fatal error.
*/
#define Fatal(fmt...) do { Error(fmt); abort(); } while (0)

/**
**	Show warning.
*/
#define Warning(fmt...) (void)( (SysLogLevel > 0) ? Syslog(LOG_WARNING, 0, fmt) : (void)0 )

/**
**	Show info.
*/
#define Info(fmt...) (void)( (SysLogLevel > 0) ? Syslog(LOG_INFO, 0, fmt) : (void)0 )

/**
**	Show debug.
*/
#define Debug(fmt...) Syslog(LOG_DEBUG, L_DEBUG, fmt)

/**
**	Show debug with logging category.
*/
#define Debug2(cat, fmt...) Syslog(LOG_DEBUG, cat, fmt)

#ifndef AV_NOPTS_VALUE
#define AV_NOPTS_VALUE INT64_C(0x8000000000000000)
#endif

//////////////////////////////////////////////////////////////////////////////
//	Prototypes
//////////////////////////////////////////////////////////////////////////////

static inline void Syslog(const int, const int, const char *format, ...)
    __attribute__ ((format(printf, 3, 4)));

//////////////////////////////////////////////////////////////////////////////
//	Inlines
//////////////////////////////////////////////////////////////////////////////

/**
**	Syslog output function.
*/
static inline void Syslog(const int level, const int cat, const char *format, ...)
{
	int DebugLogLevel = 0;
#ifdef DEBUG
	DebugLogLevel |= L_DEBUG;
#endif
#ifdef AV_SYNC_DEBUG
	DebugLogLevel |= L_AV_SYNC;
#endif
#ifdef SOUND_DEBUG
	DebugLogLevel |= L_SOUND;
#endif
#ifdef OSD_DEBUG
	DebugLogLevel |= L_OSD;
#endif
#ifdef DRM_DEBUG
	DebugLogLevel |= L_DRM;
#endif
#ifdef CODEC_DEBUG
	DebugLogLevel |= L_CODEC;
#endif
#ifdef DEINT_DEBUG
	DebugLogLevel |= L_DEINT;
#endif
#ifdef STILL_DEBUG
	DebugLogLevel |= L_STILL;
#endif
#ifdef MEDIA_DEBUG
	DebugLogLevel |= L_MEDIA;
#endif
#ifdef GL_DEBUG
	DebugLogLevel |= L_OPENGL;
#endif
#ifdef GL_DEBUG_TIME
	DebugLogLevel |= L_OPENGL_TIME;
#endif
#ifdef GL_DEBUG_TIME_ALL
	DebugLogLevel |= L_OPENGL_TIME_ALL;
#endif
	va_list ap;
	char fmt[256];
	char prefix[20] = "";

	if (level == LOG_DEBUG) {
		switch (DebugLogLevel & cat) {
		case L_DEBUG:
			break;
		case L_AV_SYNC:
			strcpy(prefix, "[AV_Sync]");
			break;
		case L_SOUND:
			strcpy(prefix, "[Sound]");
			break;
		case L_OSD:
			strcpy(prefix, "[Osd]");
			break;
		case L_DRM:
			strcpy(prefix, "[Drm]");
			break;
		case L_CODEC:
			strcpy(prefix, "[Codec]");
			break;
		case L_DEINT:
			strcpy(prefix, "[Deint]");
			break;
		case L_STILL:
			strcpy(prefix, "[Still]");
			break;
		case L_MEDIA:
			strcpy(prefix, "[Media]");
			break;
		case L_OPENGL:
		case L_OPENGL_TIME:
		case L_OPENGL_TIME_ALL:
			strcpy(prefix, "[OpenGL]");
			break;
		default:
			return;
		}
	}
	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice]%s %s", threadId, prefix, format);
	va_start(ap, format);
	vsyslog(level, fmt, ap);
	va_end(ap);
}

/**
**	Nice time-stamp string.
**
**	@param ts	time stamp
*/
static inline const char *Timestamp2String(int64_t ts)
{
	static char buf[3][16];
	static int idx = 0;

	if (ts == (int64_t) AV_NOPTS_VALUE) {
		return "--:--:--.---";
	}
	idx = (idx + 1) % 3;
	snprintf(buf[idx], sizeof(buf[idx]), "%2d:%02d:%02d.%03d",
		(int)(ts / (3600000)), (int)((ts / (60000)) % 60),
		(int)((ts / (1000)) % 60), (int)(ts % 1000));

	return buf[idx];
}

/**
**	Get ticks in ms.
**
**	@returns ticks in ms,
*/
static inline uint32_t GetMsTicks(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec tspec;

    clock_gettime(CLOCK_MONOTONIC, &tspec);
    return (tspec.tv_sec * 1000) + (tspec.tv_nsec / (1000 * 1000));
#else
    struct timeval tval;

    if (gettimeofday(&tval, NULL) < 0) {
	return 0;
    }
    return (tval.tv_sec * 1000) + (tval.tv_usec / 1000);
#endif
}

/**
**	Read the PES header length from PES header.
**
**	@returns length
*/
static inline int PesHeadLength(const uint8_t *p)
{
  return 9 + p[8];
}

#ifdef __cplusplus
}
#endif

/// @}
