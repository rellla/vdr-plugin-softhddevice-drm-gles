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

#include <mutex>
#include <variant>

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
#include "softhdosd.h"
#include "pipreceiver.h"

#if __cplusplus < 201703L
#error "C++17 or higher is required"
#endif

// State machine definitions
// Implementing C++17 visitor pattern

enum State {
	STOP,
	PLAY,
	TRICK_SPEED,
	STILL_PICTURE,
	DETACHED
};

struct PlayEvent {};
struct PauseEvent {};
struct StopEvent {};
struct TrickSpeedEvent {
	int speed;
	bool forward;
};
struct StillPictureEvent {
	const uchar *data;
	int size;
};
struct DetachEvent {};
struct AttachEvent {};
struct PipStartEvent {};
struct PipStopEvent {};

using Event = std::variant<PlayEvent, PauseEvent, StopEvent, TrickSpeedEvent, StillPictureEvent, DetachEvent, AttachEvent, PipStartEvent, PipStopEvent>;

template<class... Ts>
struct overload : Ts... { using Ts::operator()...; };
template<class... Ts> overload(Ts...) -> overload<Ts...>;

inline const char* EventToString(const Event& e) {
    return std::visit(overload{
        [](const PlayEvent&) -> const char* { return "PlayEvent"; },
        [](const PauseEvent&) -> const char* { return "PauseEvent"; },
        [](const StopEvent&) -> const char* { return "StopEvent"; },
        [](const TrickSpeedEvent&) -> const char* { return "TrickSpeedEvent"; },
        [](const StillPictureEvent&) -> const char* { return "StillPictureEvent"; },
        [](const DetachEvent&) -> const char* { return "DetachEvent"; },
        [](const AttachEvent&) -> const char* { return "AttachEvent"; },
        [](const PipStartEvent&) -> const char* { return "PipStartEvent"; },
        [](const PipStopEvent&) -> const char* { return "PipStopEvent"; },
    }, e);
}

inline const char* StateToString(State s) {
    switch(s) {
        case State::STOP: return "STOP";
        case State::PLAY: return "PLAY";
        case State::TRICK_SPEED: return "TRICK_SPEED";
        case State::STILL_PICTURE: return "STILL_PICTURE";
        case State::DETACHED: return "DETACHED";
    }
    return "Unknown";
}

/*****************************************************************************
 * cSoftHdDevice - cDevice class
 ****************************************************************************/

class cAudioDecoder;
class cVideoStream;
class cVideoRender;
class cSoftHdAudio;
class cSoftHdConfig;
class cPipReceiver;

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
	void Init(void);                    // wrapped by cPluginSoftHdDevice::Initialize()
	void Start(void);                   // wrapped by cPluginSoftHdDevice::Start()
	void Stop(void);                    // wrapped by cPluginSoftHdDevice::Stop()
	const char *CommandLineHelp(void);  // wrapped by cPluginSoftHdDevice::CommandLineHelp()
	int ProcessArgs(int, char *[]);     // wrapped by cPluginSoftHdDevice::ProcessArgs()

	//
	// cSoftHdDevice public methods
	//
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
	void SetEnableOglOsd(void);
#endif
	void SetDisableDeint(void);
	void OsdClose(void);
	void OsdDrawARGB(int, int, int, int, int, const uint8_t *, int, int);
	void SetScreenSize(int, int, uint32_t);

	// audio
	void SetVideoAudioDelay(int delay) { m_videoAudioDelay = delay; };
	int GetVideoAudioDelay(void) { return m_videoAudioDelay; };
	void SetPassthrough(int);
	void ResetChannelId(void);

	// Logging, statistics
	void GetStats(int *, int *, int *);

	// Mediaplayer
	void SetAudioCodec(enum AVCodecID, AVCodecParameters *, AVRational);
	void SetVideoCodec(enum AVCodecID, AVCodecParameters *, AVRational);
	int PlayAudioPkts(AVPacket *);
	int PlayVideoPkts(AVPacket *);

	// State machine
	void SetState(enum State);
	void OnEnteringState(enum State);
	void OnLeavingState(enum State);

	// detach/ attach
	void Detach(void);
	void Attach(void);
	bool IsDetached(void) const;

	// pip
	void PipEnable(void);
	void PipDisable(void);
	bool PipIsEnabled(void);
	int PlayPipVideo(const uchar *, int);

private:
	enum State m_state = DETACHED;   ///< current plugin state, normal plugin start sets detached state
	cDvbSpuDecoder *m_pSpuDecoder;   ///< pointer to spu decoder
	cSoftHdConfig *m_pConfig;        ///< pointer to cSoftHdConfig object
	cVideoRender *m_pRender;         ///< pointer to cVideoRender object
	cVideoStream *m_pVideoStream;    ///< pointer to main video stream
	cSoftHdAudio *m_pAudio;          ///< pointer to cSoftHdAudio object
	cAudioDecoder *m_pAudioDecoder;  ///< pointer to cAudioDecoder object
	cSoftOsdProvider *m_pOsdProvider; ///< pointer to cSoftOsdProvider object
	cReassemblyBufferVideo m_videoReassemblyBuffer; ///< video pes reassembly buffer
	cReassemblyBufferAudio m_audioReassemblyBuffer; ///< audio pes reassembly buffer

	int m_audioChannelID;            ///< current audio channel ID
	int m_videoAudioDelay;           ///< audio/video delay set via setup menu
	bool m_grabActive;               ///< simple lock variable
	                                 ///< skips a new grab request if the last one is still active

	bool m_pipActive;                ///< true, if pip is active
	int m_pipChannelNum;             ///< current pip channel number
	const cChannel *m_pPipChannel;   ///< current pip channel
	cPipReceiver *m_pPipReceiver;    ///< cReceiver for pip stream
	cVideoStream *m_pPipStream;      ///< pointer to pip video stream
	cReassemblyBufferVideo m_pipReassemblyBuffer; ///< pip pes reassembly buffer
	mutable std::mutex m_mutex;      ///< mutex to lock the state machine
	std::mutex m_sizeMutex;          ///< mutex to lock screen size (which is accessed by different threads)

	int m_screenWidth;
	int m_screenHeight;
	uint32_t m_screenRefreshRate;

	int PlayVideoInternal(cVideoStream *, cReassemblyBufferVideo *, const uchar *, int);
	void ClearAudio(void);
	void Exit(void);
	void OnEventReceived(const Event&);
	void HandlePause(void);
	void HandleStillPicture(const uchar *data, int size);

	void TogglePip(bool);
	void DelPip(void);
	void NewPip(int);
};

#endif
