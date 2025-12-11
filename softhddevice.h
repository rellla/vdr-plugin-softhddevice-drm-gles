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

#if __cplusplus < 201703L
#error "C++17 or higher is required"
#endif

#include <mutex>
#include <atomic>

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
#include "event.h"

// State machine definitions
// Implementing C++17 visitor pattern

template<class... Ts>
struct overload : Ts... { using Ts::operator()...; };
template<class... Ts> overload(Ts...) -> overload<Ts...>;

enum State {
	STOP,
	BUFFERING,
	PLAY,
	TRICK_SPEED,
	STILL_PICTURE,
	DETACHED
};

inline const char* EventToString(const Event& e) {
    return std::visit(overload{
        [](const PlayEvent&) -> const char* { return "PlayEvent"; },
        [](const PauseEvent&) -> const char* { return "PauseEvent"; },
        [](const StopEvent&) -> const char* { return "StopEvent"; },
        [](const TrickSpeedEvent&) -> const char* { return "TrickSpeedEvent"; },
        [](const StillPictureEvent&) -> const char* { return "StillPictureEvent"; },
        [](const DetachEvent&) -> const char* { return "DetachEvent"; },
        [](const AttachEvent&) -> const char* { return "AttachEvent"; },
		[](const BufferUnderrunEvent& e) -> const char* { return e.type == AUDIO ? "BufferUnderrunEvent: Audio" : "BufferUnderrunEvent: Video"; },
		[](const BufferingThresholdReachedEvent&) -> const char* { return "BufferingThresholdReachedEvent"; },
        [](const PipEvent&) -> const char* { return "PipEvent"; },
    }, e);
}

inline const char* StateToString(State s) {
    switch(s) {
        case State::STOP: return "STOP";
		case State::BUFFERING: return "BUFFERING";
        case State::PLAY: return "PLAY";
        case State::TRICK_SPEED: return "TRICK_SPEED";
        case State::STILL_PICTURE: return "STILL_PICTURE";
        case State::DETACHED: return "DETACHED";
    }
    return "Unknown";
}

enum PlaybackMode {
	NONE,
	AUDIO_AND_VIDEO,
	AUDIO_ONLY,
	VIDEO_ONLY
};

class cAudioDecoder;


/*****************************************************************************
 * cSoftHdDevice - cDevice class
 ****************************************************************************/

class cAudioDecoder;
class cVideoRender;
class cSoftHdAudio;
class cSoftHdConfig;
class cPipReceiver;

class cSoftHdDevice : public cDevice, public IEventReceiver
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
	const char *CommandLineHelp(void);  // wrapped by cPluginSoftHdDevice::CommandLineHelp()
	int ProcessArgs(int, char *[]);     // wrapped by cPluginSoftHdDevice::ProcessArgs()
	int Start(void);
	void Stop(void);

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
	int GetVideoAudioDelayMs(void) { return m_pConfig->ConfigVideoAudioDelayMs; };
	void SetPassthrough(int);
	void ResetChannelId(void);

	// Logging, statistics
	void GetStats(int *, int *, int *);

	// Mediaplayer
	void SetAudioCodec(enum AVCodecID, AVCodecParameters *, AVRational);
	void SetVideoCodec(enum AVCodecID, AVCodecParameters *, AVRational);
	int PlayAudioPkts(AVPacket *);
	int PlayVideoPkts(AVPacket *);

	// detach/ attach
	void Detach(void);
	void Attach(void);
	bool IsDetached(void) const;
	void ResetOsdProvider(void) { m_pOsdProvider = nullptr; }
	bool IsOsdProviderSet(void) const { return m_pOsdProvider != nullptr; }

	bool IsBufferingThresholdReached(void);

	// pip
	void PipEnable(void);
	void PipDisable(void);
	void PipToggle(void);
	void PipChannelChange(int);
	void PipChannelSwap(void);
	bool PipIsEnabled(void);
	int PlayPipVideo(const uchar *, int);
	void PipSetSize(void);
	void PipSwapPosition(void);

private:
	static constexpr int MIN_BUFFER_FILL_LEVEL_THRESHOLD_MS = 450; ///< min buffering threshold in ms

	std::atomic<State> m_state = DETACHED; ///< current plugin state, normal plugin start sets detached state
	std::mutex m_eventMutex;         ///< mutex to protect event queue
	bool m_needsMakePrimary = false;
	cDvbSpuDecoder *m_pSpuDecoder;   ///< pointer to spu decoder
	cSoftHdConfig *m_pConfig;        ///< pointer to cSoftHdConfig object
	cVideoRender *m_pRender;         ///< pointer to cVideoRender object
	cVideoStream *m_pVideoStream;    ///< pointer to main video stream
	cSoftHdAudio *m_pAudio;          ///< pointer to cSoftHdAudio object
	cAudioDecoder *m_pAudioDecoder;  ///< pointer to cAudioDecoder object
	cSoftOsdProvider *m_pOsdProvider; ///< pointer to cSoftOsdProvider object
	cReassemblyBufferVideo m_videoReassemblyBuffer; ///< video pes reassembly buffer
	cReassemblyBufferAudio m_audioReassemblyBuffer; ///< audio pes reassembly buffer

	std::atomic<PlaybackMode> m_playbackMode = NONE; ///< current playback mode
	int m_audioChannelID;            ///< current audio channel ID
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
	std::atomic<bool> m_receivedAudio = false; ///< flag if audio packets have been received
	std::atomic<bool> m_receivedVideo = false; ///< flag if video packets have been received
	bool m_pipUseAlt;                ///< use alternative pip position

	int m_screenWidth;
	int m_screenHeight;
	uint32_t m_screenRefreshRate;

	int PlayVideoInternal(cVideoStream *, cReassemblyBufferVideo *, const uchar *, int);
	void ClearAudio(void);
	void OnEventReceived(const Event&);
	void HandleStillPicture(const uchar *data, int size);
	int64_t GetFirstAudioPtsMsToPlay();
	int64_t GetFirstVideoPtsMsToPlay();

	int GetBufferFillLevelThresholdMs();

	// State machine
	void SetState(State);
	void OnEnteringState(State);
	void OnLeavingState(State);

	// PIP
	void SetEnablePip(bool);
	void TogglePip(void);
	void ChangePipChannel(int);
	void ResetPipChannel(void);
	void DelPip(void);
	void NewPip(int);
	void HandlePip(enum PipState);
	void SetPipSize(void);
	void SwapPipPosition(void);
};

#endif
