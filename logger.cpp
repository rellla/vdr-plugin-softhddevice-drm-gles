// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file logger.cpp
 * Logger
 *
 * This file defines cSoftHdLogger, which is a class to log things
 * into syslog. You can use one of the LOG* macros, which are
 * defined in the header file logger.h.
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <syslog.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {
#include <libavutil/log.h>
}

#include <vdr/tools.h>

#include "logger.h"

/*****************************************************************************
 * cSoftHdLogger class
 ****************************************************************************/

/**
 * Get an instance to the global logger
 *
 * @return    pointer to the logger instance
 */
std::shared_ptr<cSoftHdLogger> cSoftHdLogger::GetLogger()
{
	static std::shared_ptr<cSoftHdLogger> instance(new cSoftHdLogger());
	return instance;
}

/**
 * Log to syslog LOG_ERR and abort
 */
void cSoftHdLogger::LogFatal(const char *format, ...)
{
	if (SysLogLevel <= 0)
		abort();

	va_list ap;
	char fmt[MAX_LOGMESSAGE_SIZE];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_ERR, fmt, ap);
	va_end(ap);

	abort();
}

/**
 * Log to syslog LOG_ERR
 */
void cSoftHdLogger::LogError(const char *format, ...)
{
	if (SysLogLevel <= 0)
		return;

	va_list ap;
	char fmt[MAX_LOGMESSAGE_SIZE];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_ERR, fmt, ap);
	va_end(ap);
}

/**
 * Log to syslog LOG_WARNING
 */
void cSoftHdLogger::LogWarning(const char *format, ...)
{
	if (SysLogLevel <= 1)
		return;

	va_list ap;
	char fmt[MAX_LOGMESSAGE_SIZE];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_WARNING, fmt, ap);
	va_end(ap);
}

/**
 * Log to syslog LOG_INFO
 */
void cSoftHdLogger::LogInfo(const char *format, ...)
{
	if (SysLogLevel <= 2)
		return;

	va_list ap;
	char fmt[MAX_LOGMESSAGE_SIZE];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_INFO, fmt, ap);
	va_end(ap);
}

/**
 * Log to syslog LOG_DEBUG
 */
void cSoftHdLogger::LogDebug(const char *format, ...)
{
	if (!m_logLevel)
		return;

	va_list ap;
	char fmt[MAX_LOGMESSAGE_SIZE];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_DEBUG, fmt, ap);
	va_end(ap);
}

/**
 * Log to syslog LOG_DEBUG and add logging category to output
 */
void cSoftHdLogger::LogDebug2(const int cat, const char *format, ...)
{
	if (!format)
		return;

	va_list ap;
	char fmt[MAX_LOGMESSAGE_SIZE];
	char prefix[20] = "";

	switch (m_logLevel & cat) {
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
	case L_STILL:
		strcpy(prefix, "[Still]");
		break;
	case L_TRICK:
		strcpy(prefix, "[Trick]");
		break;
	case L_MEDIA:
		strcpy(prefix, "[Media]");
		break;
	case L_OPENGL:
	case L_OPENGL_TIME:
	case L_OPENGL_TIME_ALL:
		strcpy(prefix, "[OpenGL]");
		break;
	case L_PACKET:
		strcpy(prefix, "[Packet]");
		break;
	case L_GRAB:
		strcpy(prefix, "[Grab]");
		break;
	case L_DEBUG:
	default:
		return;
	}
	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice]%s %s", threadId, prefix, format);

	va_start(ap, format);
	vsyslog(LOG_DEBUG, fmt, ap);
	va_end(ap);
}

/**
 * Log to syslog LOG_DEBUG and add prefix [FFMpeg] to output
 */
void cSoftHdLogger::LogFFmpeg(const char *fmt, va_list vl)
{
	if (!(m_logLevel & L_FFMPEG))
		return;

	av_log_set_level(AV_LOGLEVEL);

	char format[MAX_LOGMESSAGE_SIZE];
	char prefix[20] = "";
	pid_t threadId = syscall(__NR_gettid);

	strcpy(prefix, "[FFMpeg]");
	snprintf(format, sizeof(format), "[%d] [softhddevice]%s %s", threadId, prefix, fmt);

	vsyslog(LOG_DEBUG, format, vl);
}

/**
 * Callback for ffmpeg logs
 *
 * Log to LOG_DEBUG and add prefix to output
 */
void cSoftHdLogger::LogFFmpegCallback([[maybe_unused]] void *ptr, [[maybe_unused]] int level, const char *fmt, va_list vl)
{
	if (auto logger = GetLogger())
		logger->LogFFmpeg(fmt, vl);
}
