// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file config.h
 * Plugin Configuration Header File
 *
 * @copyright 2011, 2015 by Johns.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __SOFTHDCONFIG_H
#define __SOFTHDCONFIG_H

#include <atomic>
#include <mutex>

/**
 * Plugin Configuration
 * @defgroup config Plugin Configuration
 */

/**
 * Plugin Configuration
 *
 * @ingroup config
 */
class cSoftHdConfig {
public:
	//
	// setup.conf parameters
	//

	// General
	bool ConfigHideMainMenuEntry = false;       ///< config hide main menu entry

	// Video
	int ConfigVideoEnableHDR = 0;               ///< enable HDR
	int ConfigVideoDisplayMode = 0;             ///< display mode (default, auto adjust)

	// Audio
	bool ConfigAudioSoftvol = false;            ///< config use software volume
	bool ConfigAudioDownmix = false;            ///< config ffmpeg audio downmix
	int ConfigAudioPassthroughMask = 0;         ///< config audio pass-through mask
	bool ConfigAudioPassthroughState = false;   ///< flag audio-passthrough on/off
	int ConfigAudioAutoAES = 0;                 ///< config automatic AES handling
	int ConfigVideoAudioDelayMs = 0;            ///< config audio delay
	bool ConfigAudioNormalize = false;          ///< config use normalize volume
	int ConfigAudioMaxNormalize = 0;            ///< config max normalize factor
	bool ConfigAudioCompression = false;        ///< config use volume compression
	int ConfigAudioMaxCompression = 0;          ///< config max volume compression
	int ConfigAudioStereoDescent = 0;           ///< config reduce stereo loudness

	// Audio Equalizer
	int ConfigAudioEq = 0;                      ///< config equalizer filter
	int ConfigAudioEqBand[18] =
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, 0 };      ///< config equalizer filter bands

	// PiP
	// default position at right top, 25% scaled
	int ConfigPipScalePercent = 25;             ///< scale factor of pip video
	int ConfigPipLeftPercent = 100;             ///< 0 = aligned to left, 100 = aligned to right
	int ConfigPipTopPercent = 0;                ///< 0 = aligned to top, 100 = aligned to bottom
	int ConfigPipUseAlt = false;
	// alternative position at left top, 25% scaled
	int ConfigPipAltScalePercent = 25;          ///< alternative scale factor of pip video
	int ConfigPipAltLeftPercent = 0;            ///< 0 = aligned to left, 100 = aligned to right
	int ConfigPipAltTopPercent = 0;             ///< 0 = aligned to top, 100 = aligned to bottom

	// Logging
	bool ConfigLogState = true;                 ///< flag logging on/off
	int ConfigLogLevels = 0;                    ///< loglevel config

	// Expert Settings
	int ConfigAdditionalBufferLengthMs = 0;     ///< config size ms of a/v buffer
	bool ConfigDisableDeint = false;            ///< disable deinterlacer
	bool ConfigDecoderFallbackToSw = false;     ///< fallback to software decoder if the hardware decoder fails
	int ConfigDecoderFallbackToSwNumPkts = 22;  ///< maximum number of packets sent before fallback to sw decoder
	bool ConfigDecoderNeedsIFrame = false;      ///< start h264 decoder only when an I-Frame arrives
	bool ConfigParseH264Dimensions = false;     ///< parse h264 stream for width and height for decoder init
	int ConfigParseH264StreamStart = 0;         ///< log frames at stream start up to the given number of I-Frames
	int ConfigDropInvalidH264PFrames = 0;       ///< drop P-Frames with invalid references on stream start up to the given number of I-Frames
#ifdef USE_GLES
	int ConfigMaxSizeGPUImageCache = 128;       ///< config max gpu image cache size
#endif

	//
	// command line parameters
	//
	const char *ConfigAudioPCMDevice = nullptr;         ///< audio PCM device
	const char *ConfigAudioMixerChannel = nullptr;      ///< audio mixer channel name
	const char *ConfigDrmConnector = nullptr;           ///< user requested drm connector (e.g. "HDMI-A-1")
	const char *ConfigDrmDevice = nullptr;              ///< user requested drm device (e.g. "/dev/dri/card0")
	const char *ConfigDisplayResolution = nullptr;      ///< display resolution (syntax: "1920x1080@50")
#ifdef USE_GLES
	int ConfigDisableOglOsd = 0;                ///< config disable ogl osd
#endif

	//
	// runtime stats
	//
	const char *CurrentDecoderName = "unknown";         ///< current decoder name
	const char *CurrentDecoderType = "unknown";         ///< current decoder type: "hardware" or "software"
	std::atomic<int> StatMaxShortTermAudioJitterMs = 0; ///< logged max audio jitter of the last 1000 packets
	std::atomic<int> StatMaxLongTermAudioJitterMs = 0;  ///< logged max overall audio jitter since stream start
	std::atomic<int> StatMaxShortTermVideoJitterMs = 0; ///< logged max video jitter of the last 1000 packets
	std::atomic<int> StatMaxLongTermVideoJitterMs = 0;  ///< logged max overall video jitter since stream start

	cSoftHdConfig(void) = default;
	bool SetupParse(const char *, const char *);
	void PrintLogLevel(int);
	void SetDecoderNeedsMaxPackets(int);
	int GetDecoderNeedsMaxPackets(void);

private:
	int m_decoderNeedsMaxPackets = 0;
	std::mutex m_mutex;
};

#endif
