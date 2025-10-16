/**
 * @file logger.cpp
 * Logger class
 *
 * This file defines cSoftHdLogger, which is a class to log things
 * into syslog. You can use one of the LOG* macros, which are
 * defined in the header file logger.h.
 *
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

#include "logger.h"

/*****************************************************************************
 * cSoftHdLogger class
 ****************************************************************************/

/**
 * cSoftHdLogger constructor
 */
cSoftHdLogger::cSoftHdLogger(void)
{
	logLevel = 0;
}

/**
 * Get an instance to the global logger
 *
 * @returns pointer to the logger instance
 */
std::shared_ptr<cSoftHdLogger> cSoftHdLogger::GetLogger()
{
	static std::shared_ptr<cSoftHdLogger> instance(new cSoftHdLogger());
	return instance;
}

/**
 * Set the loglevel
 *
 * @param level       new loglevel
 */
void cSoftHdLogger::SetLogLevel(int level)
{
	logLevel = level;
}

/**
 * Log to LOG_ERR and abort
 */
void cSoftHdLogger::LogFatal(const char *format, ...)
{
	if (SysLogLevel <= 0)
		abort();

	va_list ap;
	char fmt[256];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_ERR, fmt, ap);
	va_end(ap);

	abort();
}

/**
 * Log to LOG_ERR
 */
void cSoftHdLogger::LogError(const char *format, ...)
{
	if (SysLogLevel <= 0)
		return;

	va_list ap;
	char fmt[256];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_ERR, fmt, ap);
	va_end(ap);
}

/**
 * Log to LOG_WARNING
 */
void cSoftHdLogger::LogWarning(const char *format, ...)
{
	if (SysLogLevel <= 1)
		return;

	va_list ap;
	char fmt[256];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_WARNING, fmt, ap);
	va_end(ap);
}

/**
 * Log to LOG_INFO
 */
void cSoftHdLogger::LogInfo(const char *format, ...)
{
	if (SysLogLevel <= 2)
		return;

	va_list ap;
	char fmt[256];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_INFO, fmt, ap);
	va_end(ap);
}

/**
 * Log to LOG_DEBUG
 */
void cSoftHdLogger::LogDebug(const char *format, ...)
{
	if (!logLevel)
		return;

	va_list ap;
	char fmt[256];

	pid_t threadId = syscall(__NR_gettid);
	snprintf(fmt, sizeof(fmt), "[%d] [softhddevice] %s", threadId, format);

	va_start(ap, format);
	vsyslog(LOG_DEBUG, fmt, ap);
	va_end(ap);
}

/**
 * Log to LOG_DEBUG and add logging category to output
 */
void cSoftHdLogger::LogDebug2(const int cat, const char *format, ...)
{
	if (!format)
		return;

	va_list ap;
	char fmt[256];
	char prefix[20] = "";

	switch (logLevel & cat) {
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
