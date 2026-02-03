// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file logger.h
 * Logger Header File
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __LOGGER_H
#define __LOGGER_H

#include <atomic>
#include <cstdarg>
#include <memory>

/**
 * @addtogroup misc
 * @{
 */

/** FFmpeg log level */
#define AV_LOGLEVEL AV_LOG_INFO

/********************************************************************************
 * Logger macros
 *******************************************************************************/

/** log to LOG_ERR and abort */
#define LOGFATAL cSoftHdLogger::GetLogger()->LogFatal
/** log to LOG_ERR */
#define LOGERROR cSoftHdLogger::GetLogger()->LogError
/** log to LOG_WARN */
#define LOGWARNING cSoftHdLogger::GetLogger()->LogWarning
/** log to LOG_INFO */
#define LOGINFO cSoftHdLogger::GetLogger()->LogInfo
/** log to LOG_DEBUG */
#define LOGDEBUG cSoftHdLogger::GetLogger()->LogDebug
/** log to LOG_DEBUG and add a prefix */
#define LOGDEBUG2 cSoftHdLogger::GetLogger()->LogDebug2

/**
 * Logger Flags
 *
 * depending on the flag used in the macro, logging is enabled and gets
 * a nice prefix in the syslog.
 */
enum LogFlags {
	L_DEBUG           = (1 << 0),  ///< common debug logs
	L_AV_SYNC         = (1 << 1),  ///< audio/video sync logs
	L_SOUND           = (1 << 2),  ///< sound logs
	L_OSD             = (1 << 3),  ///< osd logs
	L_DRM             = (1 << 4),  ///< drm logs
	L_CODEC           = (1 << 5),  ///< codec logs
	L_STILL           = (1 << 6),  ///< stillpicture logs
	L_TRICK           = (1 << 7),  ///< trickspeed logs
	L_MEDIA           = (1 << 8),  ///< mediaplayer logs
	L_OPENGL          = (1 << 9),  ///< opengl osd logs
	L_OPENGL_TIME     = (1 << 10), ///< opengl osd flush time measurement
	L_OPENGL_TIME_ALL = (1 << 11), ///< opengl osd all commands time measurement
	L_PACKET          = (1 << 12), ///< decoder packet/frame tracking logs
	L_GRAB            = (1 << 13), ///< grabbing logs
	L_FFMPEG          = (1 << 14), ///< ffmpeg logs
};

/**
 * Logger
 *
 * Plugin specific logging implementation which does not depend on VDR loglevels
 */
class cSoftHdLogger {
public:
	static std::shared_ptr<cSoftHdLogger> GetLogger();
	static void LogFFmpegCallback(void *, int, const char *, va_list);
	void LogFatal(const char *format, ...);
	void LogError(const char *format, ...);
	void LogWarning(const char *format, ...);
	void LogInfo(const char *format, ...);
	void LogDebug(const char *format, ...);
	void LogDebug2(const int cat, const char *format, ...);
	void LogFFmpeg(const char *, va_list);
	void SetLogLevel(int level);

private:
	cSoftHdLogger(void) = default;
	cSoftHdLogger(const cSoftHdLogger &) = delete;
	cSoftHdLogger& operator=(const cSoftHdLogger &) = delete;

	static constexpr int MAX_LOGMESSAGE_SIZE = 512; ///< max size of the log message

	std::atomic<int> m_logLevel = 0; ///< loglevel mask (see enum LogFlags)
};

/** @} */

#endif
