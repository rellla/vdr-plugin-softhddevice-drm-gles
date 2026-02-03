// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file misc.h
 * Misc Functions
 *
 * @copyright 2009 - 2012 by Lutz Sammer.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __MISC_H
#define __MISC_H

#include <cstdio>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
}

#ifdef USE_GLES
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "logger.h"

/**
 * @addtogroup misc
 * @{
 */

/****************************************************************************************
 * GL Helpers
 ***************************************************************************************/
/**
 * Check for OpenGL errors and log them
 */
static inline void glCheckError(const char *stmt, const char *fname, int line) {
	GLint err = glGetError();
	if (err != GL_NO_ERROR)
		LOGERROR("GL Error (0x%08x): %s failed at %s:%i", err, stmt, fname, line);
}

/**
 * Check for EGL errors and log them
 */
static inline void eglCheckError(const char *stmt, const char *fname, int line) {
	EGLint err = eglGetError();
	if (err != EGL_SUCCESS)
		LOGERROR("EGL ERROR (0x%08x): %s failed at %s:%i", err, stmt, fname, line);
}

/** glCheckError macro */
#define GL_CHECK(stmt) do { \
		stmt; \
		glCheckError(#stmt, __FILE__, __LINE__); \
	} while (0)

/** eglCheckError macro */
#define EGL_CHECK(stmt) do { \
		stmt; \
		eglCheckError(#stmt, __FILE__, __LINE__); \
	} while (0)

#endif // USE_GLES


/****************************************************************************************
 * FFmpeg Helpers
 ***************************************************************************************/
#ifndef AV_NOPTS_VALUE
#define AV_NOPTS_VALUE INT64_C(0x8000000000000000)
#endif

#define VIDEO_SURFACES_MAX 3 ///< maximum video surfaces kept by the filter and render queues

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

#ifdef av_err2str
#undef av_err2str
/**
 * Workaround for av_err2str() not working with C++
 */
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

/****************************************************************************************
 * Misc Helpers
 ***************************************************************************************/

/**
 * Nice time-stamp string.
 *
 * @param ts       time stamp
 * @param divisor  optional divisor for the given timestamp int (must be > 0)
 *
 * @return nice formatted timestamp string
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

/**
 * Return _count_ amount of bytes from _data_
 */
static inline uint32_t ReadBytes(const uint8_t *data, int count)
{
	uint32_t value = 0;

	for (int i = 0; i < count; i++) {
		value <<= 8;
		value |= data[i];
	}

	return value;
}

/** @} */

#endif
