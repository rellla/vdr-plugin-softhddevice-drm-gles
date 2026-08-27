// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file softhddevice.h
 * Output Device Header File
 *
 * @copyright 2011 - 2015 by Johns.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __SOFTHDDEVICE_H
#define __SOFTHDDEVICE_H

#include <atomic>
#include <mutex>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include <vdr/device.h>
#include <vdr/osd.h>
#include <vdr/status.h>
#include <vdr/thread.h>

#include "config.h"
#include "hardwaredevice.h"
#include "jittertracker.h"
#include "pes.h"
#include "statemachine.h"

class cAudioDecoder;
class cDvbSpuDecoder;
class cPipHandler;
class cPipReceiver;
class cSpuDecoder;
class cSoftHdAudio;
class cSoftHdDevice;
class cSoftHdGrab;
class cSoftOsdProvider;
class cStateMachine;
class cVideoRender;
class cVideoStream;

/**
 * Output Device Implementation
 * @defgroup device Device
 */

/**
 * @addtogroup device
 * @{
 */

enum PlaybackMode {
	NONE,
	AUDIO_AND_VIDEO,
	AUDIO_ONLY,
	VIDEO_ONLY
};

/** @} */

/**
 * Output Device Implementation
 *
 * @ingroup device
 */
class cSoftHdDevice : public cDevice, public cStatus {
public:
	cSoftHdDevice(cSoftHdConfig *);
	virtual ~cSoftHdDevice(void);

	//
	// VDR cPlugin interface (wrapped by cPluginSoftHdDevice)
	//
	bool Initialize(void);
	int Start(void);
	void Stop(void);

	//
	// VDR cDevice interface
	//
protected:
	virtual void MakePrimaryDevice(bool);
public:
	virtual cString DeviceName(void) const { return "softhddevice-drm-gles"; }
	virtual bool HasDecoder(void) const;

	// SPU facilities
	virtual cSpuDecoder * GetSpuDecoder(void);

	// Image grab facilities
	virtual uchar *GrabImage(int &, bool, int, int, int);

	// Video format facilities
	virtual void SetVideoDisplayFormat(eVideoDisplayFormat);
	virtual void SetVideoFormat(bool);
	virtual void GetVideoSize(int &, int &, double &);
	virtual void GetOsdSize(int &, int &, double &);

	// Audio facilities
protected:
	virtual void SetVolumeDevice(int);

	// Player facilities
	virtual bool CanReplay(void) const;
	virtual bool SetPlayMode(ePlayMode);
	virtual int PlayVideo(const uchar *, int);
	virtual int PlayAudio(const uchar *, int, uchar);
public:
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
#if APIVERSNUM >= 30014
	virtual bool Drain(void);
#endif

	//
	// VDR cStatus interface
	//
protected:
	virtual void ChannelSwitch(const cDevice *, int, bool);

	//
	// cSoftHdDevice public API
	//
public:
	cSoftHdConfig *Config(void) { return m_pConfig; };
	cVideoStream *VideoStream(void) { return m_pVideoStream; };
	cVideoRender *Render(void) { return m_pRender; };
	cSoftHdAudio *Audio(void) { return m_pAudio; };

	// Playback, display and decoder
	void SetDisableDeint(void);
	void SetDecoderNeedsIFrame(void);
	void SetParseH264Dimensions(void);
	void SetDecoderFallbackToSw(bool);
	void SetEnableHdr(bool);
	void SetChannelSwitchMode(ChannelSwitchMode mode) { m_channelSwitchMode = mode; };
	void SetDisplayMode(int);
	bool CheckPlaybackStartConditions(void);
	bool CheckAudioPlaybackStartConditions(void);
	bool IsVideoOnlyPlayback(void) { return m_playbackMode == VIDEO_ONLY; };

	// Osd
#ifdef USE_GLES
	int MaxSizeGPUImageCache(void);
	int OglOsdIsDisabled(void);
	void SetDisableOglOsd(void);
	void SetEnableOglOsd(void);
#endif
	void OsdClose(void);
	void OsdDrawARGB(int, int, int, int, int, const uint8_t *, int, int);
	void SetOsdSize(int, int);
	void SetScreenSize(int, int);

	// Audio
	int GetVideoAudioDelayMs(void) { return m_pConfig->ConfigVideoAudioDelayMs; };
	int GetMinBufferFillLevelThresholdMs(void) { return MIN_BUFFER_FILL_LEVEL_THRESHOLD_MS; };
	void SetPassthroughMask(int);
	void ResetChannelId(void);

	// Logging, statistics
	void GetStats(int *, int *, int *);

	// Mediaplayer
	void SetAudioCodec(enum AVCodecID, AVCodecParameters *, AVRational);
	void SetVideoCodec(enum AVCodecID, AVCodecParameters *, AVRational);
	int PlayAudioPkts(AVPacket *);
	int PlayVideoPkts(AVPacket *);

	// Detach/ attach
	void Detach(void);
	void Attach(void);
	bool IsDetached(void) const;
	void ResetOsdProvider(void) { m_pOsdProvider = nullptr; }
	bool IsOsdProviderSet(void) const { return m_pOsdProvider != nullptr; }
	void SetStartDetached(void) { m_forceDetached = true; };
	bool IsDraining(void) { return m_draining; };

	// Pip
	int PlayPipVideo(const uchar *, int);
	void SetDrmCanDisplayPip(bool canDisplay) { m_drmCanDisplayPip = canDisplay; };
	bool UsePip(void) { return m_drmCanDisplayPip && !m_disablePip && m_pPipHandler; };
	void ResetPipStream(void);
	void ToggleRenderPipPosition(void) { m_pipUseAlt = !m_pipUseAlt; };
	// wrapper functions
	void SetDisablePip(void) { m_disablePip = true; };
	bool PipIsEnabled(void);
	void PipEnable(void);
	void PipDisable(void);
	void PipToggle(void);
	void PipChannelChange(int);
	void PipChannelSwap(bool);
	void PipSwapPosition(void);
	void PipSetSize(void);
	void SetRenderPipSize(void);
	void SetRenderPipActive(bool);

	// state transitioning functions
	void TriggerEvent(const Event&);

	void LeaveState(State);
	void EnterState(State);
	void HandleStillPicture(const uchar *data, int size);
	void HandleDisplayModeChange(const sDrmMode &);
	void HaltVideoThreads(void);
	void ResumeVideoThreads(void);
#ifdef USE_GLES
	bool HaltOpenGlThread(void);
	void ResumeOpenGlThread(void);
#endif
	bool IsDetachForced(void);
	void SetDetachForced(void) { m_forceDetached = true; };
	int InitAudio(bool);
	void ResetVideoFilter(void);
	void SetTrickSpeed(double, bool, bool);
	bool SchedulePlaybackStart(void);
	void ScheduleResyncAtPtsMs(int64_t);
	void ResumeFromPause(void);
	void PausePlayback(bool);
	void ResumePlayback(void);

private:
	static constexpr int MIN_BUFFER_FILL_LEVEL_THRESHOLD_MS = 450; ///< min buffering threshold in ms

	std::unique_ptr<cStateMachine> m_pStateMachine;

	bool m_initialized = false;                     ///< true, if the plugin had a successful Initialize()
	std::atomic<bool> m_draining = false;           ///< true, if the device is in draining mode (waiting for empty buffers)
	std::mutex m_eventMutex;                        ///< mutex to protect event queue
	bool m_needsMakePrimary = false;                ///< true, if device should be made a primary device after attach
	cDvbSpuDecoder *m_pSpuDecoder;                  ///< pointer to spu decoder
	cSoftHdConfig *m_pConfig;                       ///< pointer to cSoftHdConfig object
	cVideoRender *m_pRender;                        ///< pointer to cVideoRender object
	cVideoStream *m_pVideoStream;                   ///< pointer to main video stream
	cSoftHdAudio *m_pAudio;                         ///< pointer to cSoftHdAudio object
	cAudioDecoder *m_pAudioDecoder = nullptr;       ///< pointer to cAudioDecoder object
	cSoftOsdProvider *m_pOsdProvider = nullptr;     ///< pointer to cSoftOsdProvider object
	cHardwareDevice *m_pHardwareDevice;             ///< pointer to hardware device description
	cEventHandler *m_pEventHandler;                 ///< event handler thread
	cReassemblyBufferVideo m_videoReassemblyBuffer; ///< video pes reassembly buffer
	cReassemblyBufferAudio m_audioReassemblyBuffer; ///< audio pes reassembly buffer
	cJitterTracker m_audioJitterTracker{"audio"};   ///< audio jitter tracker
	cJitterTracker m_videoJitterTracker{"video"};   ///< video jitter tracker
	std::atomic<ChannelSwitchMode> m_channelSwitchMode = CHANNEL_SWITCH_AVSYNC; ///< current channel switch mode
	bool m_logPlaybackStart = true;

	std::atomic<PlaybackMode> m_playbackMode = NONE; ///< current playback mode
	int m_audioChannelID = -1;       ///< current audio channel ID
	cSoftHdGrab *m_pGrab;            ///< pointer to grabber object

	cVideoStream *m_pPipStream;      ///< pointer to pip video stream
	cReassemblyBufferVideo m_pipReassemblyBuffer; ///< pip pes reassembly buffer
	cPipHandler *m_pPipHandler = nullptr; ///< pointer to pip handler
	mutable std::mutex m_mutex;      ///< mutex to lock the state machine
	std::mutex m_sizeMutex;          ///< mutex to lock screen size (which is accessed by different threads)
	std::atomic<bool> m_receivedAudio = false; ///< flag if audio packets have been received
	std::atomic<bool> m_receivedVideo = false; ///< flag if video packets have been received
	std::atomic<bool> m_receivedValidAudio = false; ///< flag if valid audio packets have been received
	std::atomic<bool> m_receivedValidVideo = false; ///< flag if valid video packets have been received
	bool m_pipUseAlt;                ///< use alternative pip position
	bool m_drmCanDisplayPip = true;  ///< true, if the drm device is able to display a pip video
	bool m_disablePip = false;       ///< true, if pip was disabled by the user
	int m_volume = 0;                ///< track the volume in the device (for attach)

	int m_osdWidth;
	int m_osdHeight;
	int m_screenWidth;
	int m_screenHeight;

	bool m_forceDetached = false; ///< start the plugin in detached state
	bool m_externalPlayerActive = false; ///< true, if we detached for an external player

	int PlayVideoInternal(cVideoStream *, cReassemblyBufferVideo *, const uchar *, int, bool, bool);
	void FlushAudio(void);
	int64_t GetFirstAudioPtsMsToPlay();
	int64_t GetFirstVideoPtsMsToPlay();
	int GetBufferFillLevelThresholdMs();
};

#endif
