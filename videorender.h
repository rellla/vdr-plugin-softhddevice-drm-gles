// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file videorender.h
 * Video Renderer (Display) Header File
 *
 * @copyright 2009 - 2015 by Johns.  All Rights Reserved.
 * @copyright 2018 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __VIDEORENDER_H
#define __VIDEORENDER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <xf86drmMode.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
}

#ifdef USE_GLES
#include <gbm.h>
#include <EGL/egl.h>
#endif

#include <vdr/osd.h>
#include <vdr/thread.h>

#include "drmbuffer.h"
#ifdef USE_GLES
#include "drmdevice.h"
#endif
#include "drmhdr.h"
#include "event.h"
#include "grab.h"
#include "misc.h"
#include "queue.h"

#ifndef USE_GLES
class cDrmDevice;
#endif
class cSoftHdDevice;
class cSoftHdConfig;
class cSoftHdAudio;

/**
 * @addtogroup render
 * @{
 */

#define AV_SYNC_THRESHOLD_AUDIO_BEHIND_VIDEO_MS 35 ///< threshold in ms, when to duplicate video frames to keep audio and video in sync
#define AV_SYNC_THRESHOLD_AUDIO_AHEAD_VIDEO_MS 5   ///< threshold in ms, when to drop video frames to keep audio and video in sync

enum drmColorSpace {
	COLORSPACE_BT709_YCC = 2,
	COLORSPACE_BT2020_RGB = 9
};

enum drmColorEncoding {
	COLORENCODING_BT709 = 1,
	COLORENCODING_BT2020 = 2
};

enum drmColorRange {
	COLORRANGE_LIMITED = 0,
	COLORRANGE_FULL = 1
};

/**
 * DRM Buffer Getting-Strategy
 */
class cBufferStrategy {
public:
	virtual ~cBufferStrategy() = default;
	virtual cDrmBuffer *GetBuffer(cDrmBufferPool *, AVDRMFrameDescriptor *) = 0;
};

/**
 * DRM Buffer: Get a Buffer to Use Once
 */
class cBufferStrategyUseOnce : public cBufferStrategy {
public:
	cDrmBuffer *GetBuffer(cDrmBufferPool *, AVDRMFrameDescriptor *) override;
};

/**
 * DRM Buffer: Get a Hardware Buffer to Reuse
 */
class cBufferStrategyReuseHardware : public cBufferStrategy {
public:
	cDrmBuffer *GetBuffer(cDrmBufferPool *, AVDRMFrameDescriptor *) override;
};

/**
 * DRM Buffer: Get a Software Buffer to Reuse
 */
class cBufferStrategyReuseSoftware : public cBufferStrategy {
public:
	cDrmBuffer *GetBuffer(cDrmBufferPool *, AVDRMFrameDescriptor *) override;
};

/**
 * Strategy to Prepare DRM Buffer for Decoding
 */
class cDecodingStrategy {
public:
	virtual ~cDecodingStrategy() = default;
	virtual AVFrame *PrepareDrmBuffer(cDrmBuffer *, int, AVFrame *) = 0;
};

/**
 * Prepare DRM Buffer for Software Decoding
 */
class cDecodingStrategySoftware : public cDecodingStrategy {
public:
	AVFrame *PrepareDrmBuffer(cDrmBuffer *, int, AVFrame *) override;
};

/**
 * Prepare DRM Buffer for Hardware Decoding
 */
class cDecodingStrategyHardware : public cDecodingStrategy {
public:
	AVFrame *PrepareDrmBuffer(cDrmBuffer *, int, AVFrame *) override;
};

/**
 * Video Renderer
 *
 * This part is responsible to put all the parts together and display them on the screen
 */
class cVideoRender : public cThread {
public:
	cVideoRender(cSoftHdDevice *);
	~cVideoRender(void);

	void Init(void);
	void Exit(void);
	void Stop(void);
	void Halt(void) { m_mutex.lock(); };
	void Resume(void) { m_mutex.unlock(); };

	void SetVideoOutputPosition(const cRect &);
	void SetScreenSize(int, int, double);
	int64_t GetVideoClock(void) { return m_pts; };
	void GetStats(int *, int *, int *);
	void ResetFrameCounter(void);
	void Reset();
	void SetPlaybackPaused(bool pause) { m_videoPlaybackPaused = pause; };
	void SetScheduleAudioResume(bool resume) { m_resumeAudioScheduled = resume; };
	void ScheduleVideoPlaybackPauseAt(int64_t ptsMs) { m_videoPlaybackPauseScheduledAt = ptsMs; };

	void ProcessEvents(void);
	void ResetBufferReuseStrategy() { delete m_bufferReuseStrategy; m_bufferReuseStrategy = nullptr; };
	void ResetDecodingStrategy() { delete m_decodingStrategy; m_decodingStrategy = nullptr; };
	void ResetPipBufferReuseStrategy() { delete m_pipBufferReuseStrategy; m_pipBufferReuseStrategy = nullptr; };
	void ResetPipDecodingStrategy() { delete m_pipDecodingStrategy; m_pipDecodingStrategy = nullptr; };

	// OSD
	void OsdClear(void);
	void OsdDrawARGB(int, int, int, int, int, const uint8_t *, int, int);

	// TrickSpeed/ Stillpicture
	void SetTrickSpeed(double, bool, bool);
	bool IsTrickSpeed(void) { return m_trickspeed; };
	bool IsForwardTrickspeed(void) { return m_forwardTrickspeed; };
	void SetStillpicture(bool active) { m_stillpicture = active; };
	bool IsStillpicture(void) { return m_stillpicture; };

	// Grab
	int TriggerGrab(void);
	void ClearGrabBuffers(void);
	cGrabBuffer *GetGrabbedVideoBuffer(void) { return &m_grabVideo; };
	cGrabBuffer *GetGrabbedOsdBuffer(void) { return &m_grabOsd; };
	cGrabBuffer *GetGrabbedPipBuffer(void) { return &m_grabPip; };

	// DRM
	int DrmHandleEvent(void);
	bool CanHandleHdr(void);
	void SetEnableHdr(bool enable) { m_enableHdr = enable; };

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
	void ScheduleResyncAtPtsMs(int64_t ptsMs) { m_scheduleResyncAtPtsMs = ptsMs; };
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
	void ClearPipDecoderToDisplayQueue(void);
	void SetPipSize(bool);

protected:
	virtual void Action(void);

private:
	cSoftHdDevice *m_pDevice;           ///< pointer to cSoftHdDevice
	cSoftHdAudio *m_pAudio;             ///< pointer to cSoftHdAudio
	cSoftHdConfig *m_pConfig;           ///< pointer to cSoftHdConfig
	std::mutex m_mutex;                 ///< mutex for thread control
	std::vector<Event> m_eventQueue;    ///< event queue for incoming events
	double m_refreshRateHz;             ///< screen refresh rate in Hz

	cQueue<cDrmBuffer> m_drmBufferQueue{VIDEO_SURFACES_MAX};     ///< queue for DRM buffers to be displayed (VIDEO_SURFACES_MAX is defined in thread.h)
	cQueue<cDrmBuffer> m_pipDrmBufferQueue{VIDEO_SURFACES_MAX};  ///< queue for PIP DRM buffers to be displayed (VIDEO_SURFACES_MAX is defined in thread.h)
	std::atomic<double> m_trickspeedFactor = 0;      ///< current trick speed
	std::atomic<bool> m_trickspeed = false;          ///< true, if trickspeed is active
	std::atomic<bool> m_forwardTrickspeed = true;    ///< true, if trickspeed plays forward
	std::atomic<bool> m_stillpicture = false;        ///< true, if stillpicture is active
	std::atomic<int> m_framePresentationCounter = 0; ///< number of times the current frame has to be shown (for slow-motion)
	int m_numWrongProgressive;          ///< counter for progressive frames sent in an interlaced stream
	                                    ///< (only used for logging)

	bool m_startgrab = false;           ///< internal flag to trigger grabbing
	cCondVar m_grabCond;                ///< condition gets signalled, if renederer finished to clone the grabbed buffers
	cGrabBuffer m_grabOsd;              ///< keeps the current grabbed osd
	cGrabBuffer m_grabVideo;            ///< keeps the current grabbed video
	cGrabBuffer m_grabPip;              ///< keeps the current grabbed pip video

	int m_startCounter = 0;             ///< counter for displayed frames, indicates a video start
	int m_framesDuped = 0;              ///< number of frames duplicated
	int m_framesDropped = 0;            ///< number of frames dropped
	bool m_lastFrameWasDropped = false; ///< true, if the last frame was dropped
	AVRational m_timebase;              ///< timebase used for pts, set by first RenderFrame()
	std::mutex m_timebaseMutex;         ///< mutex used around m_timebase
	std::atomic<int64_t> m_pts = AV_NOPTS_VALUE; ///< current video PTS

	cRect m_videoRect;                  ///< rect of the currently displayed video
	bool m_videoIsScaled = false;       ///< true, if the currently displayed video is scaled
	int m_pipScalePercent;              ///< scale factor for pip
	int m_pipLeftPercent;               ///< left margin for pip
	int m_pipTopPercent;                ///< top margin for pip

	cDrmDevice *m_pDrmDevice;           ///< pointer cDrmDevice object
	cDrmBuffer *m_pBufOsd = nullptr;    ///< pointer to osd drm buffer object
	cDrmBuffer m_bufBlack;              ///< black drm buffer object
	cDrmBuffer *m_pCurrentlyDisplayed = nullptr;    ///< pointer to currently displayed DRM buffer
	cDrmBuffer *m_pCurrentlyPipDisplayed = nullptr; ///< pointer to currently displayed DRM buffer
	bool m_osdShown = false;            ///< set, if osd is shown currently
	std::atomic<bool> m_videoPlaybackPaused = true;                       ///< set, if playback is frozen (used for pause)
	std::atomic<bool> m_resumeAudioScheduled = false;                     ///< set, if audio resume is scheduled after a pause
	std::atomic<int64_t> m_videoPlaybackPauseScheduledAt = AV_NOPTS_VALUE; ///< if set, video will be paused at the given pts
	std::atomic<bool> m_displayOneFrameThenPause = false;                 ///< set, if only one frame shall be displayed and then pause playback
	std::atomic<int64_t> m_schedulePlaybackStartAtPtsMs = AV_NOPTS_VALUE; ///< if set, frames with PTS older than this will be dropped
	std::atomic<int64_t> m_scheduleResyncAtPtsMs = AV_NOPTS_VALUE;        ///< if set, a resync (enter state BUFFERING) will be forced at the given pts

	IEventReceiver *m_pEventReceiver;                                     ///< pointer to event receiver
	cDrmBufferPool m_drmBufferPool;                                       ///< pool of drm buffers
	cDrmBufferPool m_pipDrmBufferPool;                                    ///< PIP pool of drm buffers
	std::atomic<cBufferStrategy *> m_bufferReuseStrategy = nullptr;       ///< strategy to select drm buffers
	std::atomic<cBufferStrategy *> m_pipBufferReuseStrategy = nullptr;    ///< strategy to select drm buffers
	std::atomic<cDecodingStrategy *> m_decodingStrategy = nullptr;        ///< strategy for decoding setup
	std::atomic<cDecodingStrategy *> m_pipDecodingStrategy = nullptr;     ///< strategy for decoding setup

	cHdrMetadata m_pHdrMetadata;                             ///< hdr metadata object
	bool m_hasDoneHdrModeset = false;                        ///< true, if we ever created an hdr blob and did a modesetting
	std::atomic<bool> m_enableHdr = false;                   ///< hdr is enabled
	drmColorRange m_originalColorRange = COLORRANGE_LIMITED; ///< initial color range
	bool m_colorRangeStored = false;                         ///< true, if the original color range was stored

#ifdef USE_GLES
	bool m_disableOglOsd;                      ///< set, if ogl osd is disabled
	struct gbm_bo *m_bo;                       ///< pointer to current gbm buffer object
	struct gbm_bo *m_pOldBo;                   ///< pointer to old gbm buffer object (for later free)
	struct gbm_bo *m_pNextBo;                  ///< pointer to next gbm buffer object (for later free)
#endif

	std::atomic<bool> m_pipActive = false;     ///< true, if pip should be displayed

	int GetFrameFlags(AVFrame *);
	void SetFrameFlags(AVFrame *, int);
	void SetVideoClock(int64_t pts) { m_pts = pts; };
	bool PageFlip(cDrmBuffer *, cDrmBuffer *);
	int SetVideoBuffer(cDrmBuffer *);
	int SetOsdBuffer(drmModeAtomicReqPtr);
	int SetPipBuffer(cDrmBuffer *);
	int CommitBuffer(cDrmBuffer *, cDrmBuffer *);
	void CreateGrabBuffers(bool);
	void LogDroppedDuped(int64_t, int64_t, int);
	int64_t PtsToMs(int64_t);
	void PushFrame(AVFrame *, bool, std::atomic<cBufferStrategy*> &, std::atomic<cDecodingStrategy*> &, cQueue<cDrmBuffer> *, cDrmBufferPool *);
	int GetFramePresentationCount(int64_t);
	void SetHdrBlob(struct hdr_output_metadata);
	void SetColorSpace(drmColorRange);
	void RestoreColorSpace();
};

/** @} */

#endif
