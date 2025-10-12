/**
 * @file softhddevice.h
 * Device class header file
 *
 * @copyright (c) 2011 - 2015 by Johns.  All Rights Reserved.
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

#ifndef __SOFTHDDEVICE_H
#define __SOFTHDDEVICE_H

#include <vdr/dvbspu.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include "config.h"
#include "codec_audio.h"
#include "videostream.h"
#include "audio.h"
#include "videorender.h"

class cAudioDecoder;


/*****************************************************************************
 * cSoftHdDevice - cDevice class
 ****************************************************************************/

class cVideoStream;
class cVideoRender;
class cSoftHdAudio;
class cSoftHdConfig;

class cSoftHdDevice:public cDevice
{
public:
	cSoftHdDevice(cSoftHdConfig *);
	virtual ~cSoftHdDevice(void);

	//
	// virtual cDevice
	//
protected:
	virtual void MakePrimaryDevice(bool);

public:
	virtual cString DeviceName(void) const { return "softhddevice-drm-gles"; }
	virtual bool HasDecoder(void) const;

	// SPU facilities
	virtual cSpuDecoder * GetSpuDecoder(void);

	// player facilities
	virtual bool CanReplay(void) const;
	virtual bool SetPlayMode(ePlayMode);
	virtual int PlayVideo(const uchar *, int);
	virtual int PlayAudio(const uchar *, int, uchar);
	virtual int64_t GetSTC(void);
	virtual cRect CanScaleVideo(const cRect &, int taCenter);
	virtual void ScaleVideo(const cRect & = cRect::Null);
	virtual void TrickSpeed(int, bool);
	virtual void Clear(void);
	virtual void Play(void);
	virtual void Freeze(void);
	virtual void Mute(void);
	virtual void StillPicture(const uchar *, int);
	virtual bool Poll(cPoller &, int = 0);
	virtual bool Flush(int = 0);

	// Image Grab facilities
	virtual uchar *GrabImage(int &, bool, int, int, int);

	// video format facilities
	virtual void SetVideoDisplayFormat(eVideoDisplayFormat);
	virtual void SetVideoFormat(bool);
	virtual void GetVideoSize(int &, int &, double &);
	virtual void GetOsdSize(int &, int &, double &);

	// track facilities
	virtual void SetAudioTrackDevice(eTrackType);

	// audio facilities
	virtual int GetAudioChannelDevice(void);
	virtual void SetAudioChannelDevice(int);
	virtual void SetVolumeDevice(int);
	virtual void SetDigitalAudioDevice(bool);

	//
	// wrapped by cPluginSoftHdDevice
	//
	void Start(void);                   // wrapped by cPluginSoftHdDevice::Start()
	void Stop(void);                    // wrapped by cPluginSoftHdDevice::Stop()
	const char *CommandLineHelp(void);  // wrapped by cPluginSoftHdDevice::CommandLineHelp()
	int ProcessArgs(int, char *[]);     // wrapped by cPluginSoftHdDevice::ProcessArgs()

	//
	// cSoftHdDevice public methods
	//
	void Init(void);
	cSoftHdConfig *Config(void) { return m_pConfig; };
	cVideoStream *VideoStream(void) { return m_pVideoStream; };
	cVideoRender *Render(void) { return m_pRender; };
	cSoftHdAudio *Audio(void) { return m_pAudio; };

	// osd
#ifdef USE_GLES
#ifdef WRITE_PNG
	char WritePngs(void);
#endif
	int MaxSizeGPUImageCache(void);
	int OglOsdIsDisabled(void);
	void SetDisableOglOsd(void);
#endif
	void SetDisableDeint(void);
	void OsdClose(void);
	void OsdDrawARGB(int, int, int, int, int, const uint8_t *, int, int);

	// audio
	void SetVideoAudioDelay(int delay) { m_videoAudioDelay = delay; };
	int GetVideoAudioDelay(void) { return m_videoAudioDelay; };
	void SetPassthrough(int);
	void ResetChannelId(void);

	// Logging, statistics
	void GetStats(int *, int *, int *);

	// Mediaplayer
	void SetAudioCodec(enum AVCodecID, AVCodecParameters *, AVRational *);
	void SetVideoCodec(enum AVCodecID, AVCodecParameters *, AVRational *);
	int PlayAudioPkts(AVPacket *);
	int PlayVideoPkts(AVPacket *);

private:
	cDvbSpuDecoder *m_pSpuDecoder;   ///< pointer to spu decoder
	cSoftHdConfig *m_pConfig;        ///< pointer to cSoftHdConfig object
	cVideoRender *m_pRender;         ///< pointer to cVideoRender object
	cVideoStream *m_pVideoStream;    ///< pointer to cVideoStream object
	cSoftHdAudio *m_pAudio;          ///< pointer to cSoftHdAudio object
	cAudioDecoder *m_pAudioDecoder;  ///< pointer to cAudioDecoder object

	AVPacket *m_pAudioAvPkt;         ///< pointer to current audio AVPacket

	enum AVCodecID m_audioCodecID;   ///< pointer to current audio AVPacket
	int m_audioChannelID;            ///< current audio channel ID
	volatile bool m_newAudioStream;  ///< set, if we a new audio stream arrived
	volatile bool m_skipAudio;       ///< set, if audio should be skipped (mute)
	int m_videoAudioDelay;           ///< audio/video delay set via setup menu
	bool m_grabActive;                ///< simple lock variable
	                                 ///< skips a new grab request if the last one is still active

	void ClearAudio(void);
	void Exit(void);
	int PesHeadLength(const uint8_t *);
};

#endif
