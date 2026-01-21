/**
 * @file logger.h
 * Logger class header file
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

#ifndef __LOGGER_H
#define __LOGGER_H

#include <memory>

/**
 * Logger macros
 *
 *   - LOGFATAL     logs to LOG_ERR and aborts
 *   - LOGERROR     logs to LOG_ERR
 *   - LOGWARNING   logs to LOG_WARN
 *   - LOGINFO      logs to LOG_INFO
 *   - LOGDEBUG     logs to LOG_DEBUG
 *   - LOGDEBUG2    logs to LOG_DEBUG and prints a category info
 */
#define LOGFATAL cSoftHdLogger::GetLogger()->LogFatal
#define LOGERROR cSoftHdLogger::GetLogger()->LogError
#define LOGWARNING cSoftHdLogger::GetLogger()->LogWarning
#define LOGINFO cSoftHdLogger::GetLogger()->LogInfo
#define LOGDEBUG cSoftHdLogger::GetLogger()->LogDebug
#define LOGDEBUG2 cSoftHdLogger::GetLogger()->LogDebug2

/**
 * Logger flags
 *
 * depending on the flag used in the macro, logging is enabled and gets
 * a nice prefix in the syslog.
 */
#define L_DEBUG            (1 << 0)
#define L_AV_SYNC          (1 << 1)
#define L_SOUND            (1 << 2)
#define L_OSD              (1 << 3)
#define L_DRM              (1 << 4)
#define L_CODEC            (1 << 5)
#define L_STILL            (1 << 6)
#define L_TRICK            (1 << 7)
#define L_MEDIA            (1 << 8)
#define L_OPENGL           (1 << 9)
#define L_OPENGL_TIME      (1 << 10)
#define L_OPENGL_TIME_ALL  (1 << 11)
#define L_PACKET           (1 << 12)
#define L_GRAB             (1 << 13)

/**
 * cSoftHdLogger - Logger class
 */
class cSoftHdLogger {
public:
	static std::shared_ptr<cSoftHdLogger> GetLogger();
	void LogFatal(const char *format, ...);
	void LogError(const char *format, ...);
	void LogWarning(const char *format, ...);
	void LogInfo(const char *format, ...);
	void LogDebug(const char *format, ...);
	void LogDebug2(const int cat, const char *format, ...);
	void SetLogLevel(int level);

private:
	cSoftHdLogger(void) = default;
	cSoftHdLogger(const cSoftHdLogger &) = delete;
	cSoftHdLogger& operator=(const cSoftHdLogger &) = delete;

	int logLevel = 0; ///< loglevel (see Logger flags above)
};

#endif
