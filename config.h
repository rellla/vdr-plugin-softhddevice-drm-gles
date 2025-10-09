/**
 * @file config.h
 * SoftHdDevice config header file
 *
 * @copyright (c) 2011, 2015 by Johns.  All Rights Reserved.
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
#ifndef __SOFTHDCONFIG_H
#define __SOFTHDCONFIG_H

#include "softhddevice.h"
#include "audio.h"

/*****************************************************************************
 * Config
 ****************************************************************************/
class cSoftHdConfig
{
public:
	cSoftHdConfig(void) = default;
	virtual ~cSoftHdConfig(void);

	bool SetupParse(const char *, const char *, cSoftHdDevice *, cSoftHdAudio *);
#ifdef USE_GLES
#ifdef WRITE_PNG
	bool ConfigWritePngs = false;               ///< config write pngs from OSD
#endif
	int ConfigMaxSizeGPUImageCache = 128;       ///< config max gpu image cache size
	int ConfigDisableOglOsd = 0;                ///< config disable ogl osd
#endif
	int ConfigVideoAudioDelay = 0;              ///< config audio delay
	int ConfigAudioPassthrough = 0;             ///< config audio pass-through mask
	bool AudioPassthroughState = false;         ///< flag audio-passthrough on/off
	bool ConfigAudioDownmix = false;            ///< config ffmpeg audio downmix
	bool ConfigAudioSoftvol = false;            ///< config use software volume
	bool ConfigAudioNormalize = false;          ///< config use normalize volume
	int ConfigAudioMaxNormalize = 0;            ///< config max normalize factor
	bool ConfigAudioCompression = false;        ///< config use volume compression
	int ConfigAudioMaxCompression = 0;          ///< config max volume compression
	int ConfigAudioStereoDescent = 0;           ///< config reduce stereo loudness
	int ConfigAudioBufferTime = 0;              ///< config size ms of audio buffer
	int ConfigAudioAutoAES = 0;                 ///< config automatic AES handling
	int ConfigAudioEq = 0;                      ///< config equalizer filter
	int SetupAudioEqBand[18] =
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, 0 };      ///< config equalizer filter bands

	bool ConfigMakePrimary = false;             ///< config primary wanted
	bool ConfigHideMainMenuEntry = false;       ///< config hide main menu entry
	bool LogState = true;                       ///< flag logging on/off
	int ConfigLog = 0;                          ///< loglevel config

	bool ConfigDisableDeint = false;            ///< disable deinterlacer
	void PrintLogLevel(int);
private:
	void SetLogState(void);
	void SetPassthrough(cSoftHdDevice *);
};

#endif
