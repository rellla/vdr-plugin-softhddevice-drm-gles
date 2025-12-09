/**
 * @file misc.h
 * Misc function header file
 *
 * @copyright (c) 2009 - 2012 by Lutz Sammer.  All Rights Reserved.
 * @copyright (c) 2025 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.}
 */

#ifndef __MISC_H
#define __MISC_H

#include <syslog.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>	// clock_gettime
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>

#ifndef AV_NOPTS_VALUE
#define AV_NOPTS_VALUE INT64_C(0x8000000000000000)
#endif

#define VIDEO_SURFACES_MAX 3

/**
 * Check, if this is an interlaced frame
 *
 * @param frame    AVFrame
 *
 * @return         true, if this frame is an interlaced frame
 */
static inline bool isInterlacedFrame(AVFrame *frame)
{
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
	return frame->interlaced_frame;
#else
	return frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
}

/**
 * Workaround for av_err2str() not working with C++
 */
#ifdef av_err2str
#undef av_err2str
static inline const char* av_err2string(int errnum)
{
	static char str[3][AV_ERROR_MAX_STRING_SIZE];
	char buf[AV_ERROR_MAX_STRING_SIZE];
	static int idx = 0;

	idx = (idx + 1) % 3;
	av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, errnum);
	snprintf(str[idx], sizeof(str[idx]), "%s", buf);

	return str[idx];
}
#define av_err2str(err) av_err2string(err)
#endif

/**
 * Nice time-stamp string.
 *
 * @param ts       time stamp
 */
static inline const char *Timestamp2String(int64_t ts, uint8_t divisor)
{
	static char buf[3][20];
	static int idx = 0;

	if (ts == (int64_t) AV_NOPTS_VALUE) {
		return "--:--:--.---";
	}

	ts /= divisor;

	idx = (idx + 1) % 3;
	snprintf(buf[idx], sizeof(buf[idx]), "%2d:%02d:%02d.%03d",
		(int)(ts / (3600000)), (int)((ts / (60000)) % 60),
		(int)((ts / (1000)) % 60), (int)(ts % 1000));

	return buf[idx];
}

#endif
