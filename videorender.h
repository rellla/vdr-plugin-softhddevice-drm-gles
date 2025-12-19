/**
 * @file videorender.h
 * Rendering class header file
 *
 * @copyright (c) 2009 - 2015 by Johns.  All Rights Reserved.
 * @copyright (c) 2018 by zille.  All Rights Reserved.
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

#ifndef __VIDEORENDER_H
#define __VIDEORENDER_H

#include <atomic>
#include <vector>

#include <xf86drm.h>
#include <xf86drmMode.h>

extern "C" {
#include <libavutil/hwcontext_drm.h>
}

#ifdef USE_GLES
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include "glhelpers.h"

/* Hack:
 * xlib.h via eglplatform.h: #define Status int
 * X.h via eglplatform.h: #define CurrentTime 0L
 *
 * revert it, because it conflicts with vdr variables.
 */
#undef Status
#undef CurrentTime
#endif

#include <atomic>

#include "logger.h"
#include "iatomic.h"
#include "softhddevice.h"
#include "videostream.h"
#include "drmbuffer.h"
#include "drmdevice.h"
#include "threads.h"
#include "grab.h"
#include "drmplane.h"
#include "config.h"
#include "event.h"
#include "queue.h"

// Hardware quirks, that are set depending on the hardware used
#define QUIRK_NO_HW_DEINT               1 << 0     ///< set, if no hw deinterlacer available
#define QUIRK_CODEC_FLUSH_WORKAROUND    1 << 1     ///< set, if we have to close and reopen the codec instead of avcodec_flush_buffers (rpi)
#define QUIRK_CODEC_NEEDS_EXT_INIT      1 << 2     ///< set, if codec needs some infos for init (coded_width and coded_height)
#define QUIRK_CODEC_SKIP_FIRST_FRAMES   1 << 3     ///< set, if codec should skip first I-Frames
#define QUIRK_CODEC_SKIP_NUM_FRAMES     2          ///< skip QUIRK_CODEC_SKIP_NUM_FRAMES, in case QUIRK_CODEC_SKIP_FIRST_FRAMES is set
#define QUIRK_CODEC_DISABLE_MPEG_HW     1 << 4     ///< set, if disable mpeg hardware decoder
#define QUIRK_CODEC_DISABLE_H264_HW     1 << 5     ///< set, if disable h264 hardware decoder

#define AV_SYNC_THRESHOLD_AUDIO_BEHIND_VIDEO_MS 35 ///< threshold in ms, when to duplicate video frames to keep audio and video in sync
#define AV_SYNC_THRESHOLD_AUDIO_AHEAD_VIDEO_MS 5   ///< threshold in ms, when to drop video frames to keep audio and video in sync

class cDrmDevice;

class cBufferStrategy {
public:
	virtual ~cBufferStrategy() = default;
	virtual cDrmBuffer *GetBuffer(cDrmBufferPool *, AVDRMFrameDescriptor *) = 0;
};

class cBufferStrategyUseOnce : public cBufferStrategy {
public:
	cDrmBuffer *GetBuffer(cDrmBufferPool *, AVDRMFrameDescriptor *) override;
};

class cBufferStrategyReuseHardware : public cBufferStrategy {
public:
	cDrmBuffer *GetBuffer(cDrmBufferPool *, AVDRMFrameDescriptor *) override;
};

class cBufferStrategyReuseSoftware : public cBufferStrategy {
public:
	cDrmBuffer *GetBuffer(cDrmBufferPool *, AVDRMFrameDescriptor *) override;
};

class cDecodingStrategy {
public:
	virtual ~cDecodingStrategy() = default;
	virtual AVFrame *PrepareDrmBuffer(cDrmBuffer *, int, AVFrame *) = 0;
};

class cDecodingStrategySoftware : public cDecodingStrategy {
public:
	AVFrame *PrepareDrmBuffer(cDrmBuffer *, int, AVFrame *) override;
};

class cDecodingStrategyHardware : public cDecodingStrategy {
public:
	AVFrame *PrepareDrmBuffer(cDrmBuffer *, int, AVFrame *) override;
};

/**
 * cVideoRender - Video render class
 */
class cVideoRender
{
public:
	cVideoRender(cSoftHdDevice *);
	virtual ~cVideoRender(void);

	void Init(void);
	void Exit(void);

	void SetVideoOutputPosition(const cRect &);
	void SetScreenSize(int, int, uint32_t);
	int64_t GetVideoClock(void);
	void GetStats(int *, int *, int *);
	void ResetFrameCounter(void);
	void Reset();
	void SetPlaybackPaused(bool pause) { m_videoPlaybackPaused = pause; };
	void SetScheduleAudioResume(bool resume) { m_resumeAudioScheduled = resume; };

	void ProcessEvents(void);
	void ResetBufferReuseStrategy() { delete m_bufferReuseStrategy; m_bufferReuseStrategy = nullptr; };
	void ResetDecodingStrategy() { delete m_decodingStrategy; m_decodingStrategy = nullptr; };

	// OSD
	void OsdClear(void);
	void OsdDrawARGB(int, int, int, int, int, const uint8_t *, int, int);

	// TrickSpeed
	void SetTrickSpeed(int, int);
	int GetTrickSpeed(void);
	int GetTrickForward(void);

	// Grab
	int TriggerGrab(void);
	void ConvertVideoBufToRgb(void);
	void ConvertOsdBufToRgb(void);
	void ConvertPipBufToRgb(void);
	void ClearGrab(void);
	cSoftHdGrab *GetGrab(int *, int *, int *, int *, int *, int);

	// Threads
	void ExitDisplayThread(void);
	void DisplayThreadHalt(void) { m_pDisplayThread->Halt(); };
	void DisplayThreadResume(void) { m_pDisplayThread->Resume(); };

	// DRM
	int DrmHandleEvent(void);

	// Frame and buffer
	bool DisplayFrame();
	int GetFramesFilled(void) { return m_drmBufferQueue.Size(); };
	void PushMainFrame(AVFrame *);
	void PushPipFrame(AVFrame *);
	int64_t GetOutputPtsMs(void);
	void DisplayBlackFrame(void);
	void ClearDecoderToDisplayQueue(void);
	bool IsOutputBufferFull(void);
	void SetDisplayOneFrameThenPause(bool pause) { m_displayOneFrameThenPause = pause; };
	void SchedulePlaybackStartAtPtsMs(int64_t ptsMs) { m_schedulePlaybackStartAtPtsMs = ptsMs; };
	cQueue<cDrmBuffer> *GetMainOutputBuffer(void) { return &m_drmBufferQueue; };
	cQueue<cDrmBuffer> *GetPipOutputBuffer(void) { return &m_pipDrmBufferQueue; };

#ifdef USE_GLES
	// GLES
	void DisableOglOsd(void) { m_disableOglOsd = true; };
	void EnableOglOsd(void) { m_disableOglOsd = false; };
	bool OglOsdDisabled(void) { return m_disableOglOsd; };
	EGLSurface EglSurface(void) { return m_pDrmDevice->EglSurface(); };
	EGLDisplay EglDisplay(void) { return m_pDrmDevice->EglDisplay(); };
	EGLContext EglContext(void) { return m_pDrmDevice->EglContext(); };
	int GlInitiated(void) { return m_pDrmDevice->GlInitiated(); };
#endif

	// PIP
	void SetPipActive(bool on) { m_pipActive = on; };
	bool IsPipActive(void) { return m_pipActive; };
	void ClearPipDecoderToDisplayQueue(void);
	void SetPipSize(bool);

private:
	cSoftHdDevice *m_pDevice;           ///< pointer to cSoftHdDevice
	cSoftHdAudio *m_pAudio;             ///< pointer to cSoftHdAudio
	cSoftHdConfig *m_pConfig;           ///< pointer to cSoftHdConfig
	cDisplayThread *m_pDisplayThread;   ///< pointer to display thread
	cMutex m_trickspeedMutex;           ///< mutex used while accessing trickspeed parameters
	cMutex m_videoClockMutex;           ///< mutex used around m_pts
	std::vector<Event> m_eventQueue;    ///< event queue for incoming events

	cQueue<cDrmBuffer> m_drmBufferQueue{VIDEO_SURFACES_MAX};     ///< queue for DRM buffers to be displayed (VIDEO_SURFACES_MAX is defined in thread.h)
	cQueue<cDrmBuffer> m_pipDrmBufferQueue{VIDEO_SURFACES_MAX};  ///< queue for PIP DRM buffers to be displayed (VIDEO_SURFACES_MAX is defined in thread.h)
	int m_trickSpeed;                   ///< current trick speed
	bool m_trickForward;                ///< true, if trickspeed plays forward
	int m_framePresentationCounter = 0; ///< number of times the current frame has to be shown (for slow motion)
	int m_numWrongProgressive;          ///< counter for progressive frames sent in an interlaced stream
	                                    ///< (only used for logging)

	bool m_startgrab;                   ///< internal flag to trigger grabbing
	cCondVar m_grabCond;                ///< condition gets signalled, if renederer finished to clone the grabbed buffers
	cSoftHdGrab m_grabOsd;              ///< keeps the current grabbed osd
	cSoftHdGrab m_grabVideo;            ///< keeps the current grabbed video
	cSoftHdGrab m_grabPip;              ///< keeps the current grabbed pip video
	cRect m_lastVideoGrab;              ///< crtc rect of the last shown video frame
	cRect m_lastPipGrab;                ///< crtc rect of the last shown pip frame

	int m_startCounter;                 ///< counter for displayed frames, indicates a video start
	int m_framesDuped = 0;              ///< number of frames duplicated
	int m_framesDropped = 0;            ///< number of frames dropped
	bool m_lastFrameWasDropped = false; ///< true, if the last frame was dropped
	AVRational m_timebase;              ///< timebase used for pts, set by first RenderFrame()
	cMutex m_timebaseMutex;             ///< mutex used around m_timebase
	int64_t m_pts = AV_NOPTS_VALUE;     ///< current video PTS

	cRect m_videoRect;                  ///< rect of the currently displayed video
	bool m_videoIsScaled;               ///< true, if the currently displayed video is scaled
	int m_pipScalePercent;              ///< scale factor for pip
	int m_pipLeftPercent;               ///< left margin for pip
	int m_pipTopPercent;                ///< top margin for pip

	cDrmDevice *m_pDrmDevice;           ///< pointer cDrmDevice object
	cDrmBuffer *m_pBufOsd;              ///< pointer to osd drm buffer object
	cDrmBuffer m_bufBlack;              ///< black drm buffer object
	cDrmBuffer *m_pCurrentlyDisplayed = nullptr;    ///< pointer to currently displayed DRM buffer
	cDrmBuffer *m_pCurrentlyPipDisplayed = nullptr; ///< pointer to currently displayed DRM buffer
	bool m_osdShown;                    ///< set, if osd is shown currently
	std::atomic<bool> m_videoPlaybackPaused = true;		                  ///< set, if playback is frozen (used for pause)
	std::atomic<bool> m_resumeAudioScheduled = false;                     ///< set, if audio resume is scheduled after a pause
	std::atomic<bool> m_displayOneFrameThenPause = false;                 ///< set, if only one frame shall be displayed and then pause playback
	std::atomic<int64_t> m_schedulePlaybackStartAtPtsMs = AV_NOPTS_VALUE; ///< if set, frames with PTS older than this will be dropped

	IEventReceiver *m_pEventReceiver;                                     ///< pointer to event receiver
	cDrmBufferPool m_drmBufferPool;                                       ///< pool of drm buffers
	cDrmBufferPool m_pipDrmBufferPool;                                    ///< PIP pool of drm buffers
	std::atomic<cBufferStrategy *> m_bufferReuseStrategy = nullptr;       ///< strategy to select drm buffers
	std::atomic<cBufferStrategy *> m_pipBufferReuseStrategy = nullptr;    ///< strategy to select drm buffers
	std::atomic<cDecodingStrategy *> m_decodingStrategy = nullptr;        ///< strategy for decoding setup
	std::atomic<cDecodingStrategy *> m_pipDecodingStrategy = nullptr;     ///< strategy for decoding setup

#ifdef USE_GLES
	bool m_disableOglOsd;                      ///< set, if ogl osd is disabled
	struct gbm_bo *m_bo;                       ///< pointer to current gbm buffer object
	struct gbm_bo *m_pOldBo;                   ///< pointer to old gbm buffer object (for later free)
	struct gbm_bo *m_pNextBo;                  ///< pointer to next gbm buffer object (for later free)
#endif

	std::atomic<bool> m_pipActive = false;     ///< true, if pip should be displayed

	int GetFrameFlags(AVFrame *);
	void SetFrameFlags(AVFrame *, int);
	void SetVideoClock(int64_t);
	bool PageFlip(cDrmBuffer *, cDrmBuffer *);
	void SetVideoBuffer(cDrmBuffer *);
	int SetOsdBuffer(drmModeAtomicReqPtr);
	void SetPipBuffer(cDrmBuffer *);
	int CommitBuffer(cDrmBuffer *, cDrmBuffer *);
	void Grab(cDrmBuffer *, cDrmBuffer *);
	void LogDroppedDuped(int64_t, int64_t, int);
	int64_t PtsToMs(int64_t);
	void PushFrame(AVFrame *, bool, std::atomic<cBufferStrategy*> &, std::atomic<cDecodingStrategy*> &, cQueue<cDrmBuffer> *, cDrmBufferPool *);
};

#endif
