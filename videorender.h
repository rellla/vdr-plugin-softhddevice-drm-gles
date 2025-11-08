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

#include <xf86drm.h>
#include <xf86drmMode.h>

extern "C" {
#include <libavfilter/avfilter.h>
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

#include "logger.h"
#include "iatomic.h"
#include "softhddevice.h"
#include "videostream.h"
#include "drmbuffer.h"
#include "drmdevice.h"
#include "threads.h"
#include "grab.h"
#include "drmplane.h"
#include "drmdevice.h"

#define RENDERBUFFERS 36                 ///< number of render video buffers

// Hardware quirks, that are set depending on the hardware used
#define QUIRK_NO_HW_DEINT               1 << 0     ///< set, if no hw deinterlacer available
#define QUIRK_CODEC_FLUSH_WORKAROUND    1 << 1     ///< set, if we have to close and reopen the codec instead of avcodec_flush_buffers (rpi)
#define QUIRK_CODEC_NEEDS_EXT_INIT      1 << 2     ///< set, if codec needs some infos for init (coded_width and coded_height)
#define QUIRK_CODEC_SKIP_FIRST_FRAMES   1 << 3     ///< set, if codec should skip first I-Frames
#define QUIRK_CODEC_SKIP_NUM_FRAMES     2          ///< skip QUIRK_CODEC_SKIP_NUM_FRAMES, in case QUIRK_CODEC_SKIP_FIRST_FRAMES is set
#define QUIRK_CODEC_DISABLE_MPEG_HW     1 << 4     ///< set, if disable mpeg hardware decoder
#define QUIRK_CODEC_DISABLE_H264_HW     1 << 5     ///< set, if disable h264 hardware decoder

class cDrmDevice;

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
	int HardwareQuirks(void) { return m_hardwareQuirks; };
	void DisableDeint(bool disable) { m_userDisabledDeinterlacer = disable; };
	void DisableOglOsd(void) { m_disableOglOsd = true; };
	bool OglOsdDisabled(void) { return m_disableOglOsd; };

	void SetDisplayResolution(const char *);
	void SetVideoOutputPosition(const cRect &);
	void GetScreenSize(int *, int *, double *);
	int64_t GetVideoClock(void);
	void GetStats(int *, int *, int *);
	void ResetFrameCounter(void);
	void Reset();
	void SetPlaybackPaused(bool pause) { m_playbackPaused = pause; };
	bool IsPlaybackPaused(void) { return m_playbackPaused; };
	void SetDeinterlacerDeactivated(bool deactivate) { m_deinterlacerDeactivated = deactivate; };
	bool IsDeinterlacerDeactivated(void) { return m_deinterlacerDeactivated; };

	// OSD
	void OsdClear(void);
	void OsdDrawARGB(int, int, int, int, int, const uint8_t *, int, int);

	// TrickSpeed
	void SetTrickSpeed(int, int);
	int GetTrickSpeed(void);
	int GetTrickForward(void);
	bool ShallDisplayNextFrame(void);

	// Grab
	int TriggerGrab(void);
	void ConvertVideoBufToRgb(void);
	void ConvertOsdBufToRgb(void);
	void ClearGrab(void);
	cSoftHdGrab *GetGrab(int *, int *, int *, int *, int *, int);

	// Threads
	void Prepare(void);
	void CreateDecodingThread(void);
	bool DecodingThreadIsActive(void);
	void ExitDecodingThread(void);
	void ExitDisplayThread(void);

	void DecodingThreadHalt(void) { m_pDecodingThread->Halt(); };
	void DecodingThreadResume(void) { m_pDecodingThread->Resume(); };
	void DisplayThreadHalt(void) { m_pDisplayThread->Halt(); };
	void DisplayThreadResume(void) { m_pDisplayThread->Resume(); };

	void CancelFilterThread(void);

	// DRM
	int DrmHandleEvent(void);

	// Frame and buffer
	void RenderFrame(AVCodecContext *, AVFrame *);
	void DisplayFrame(AVFrame *);
	void EnqueueFB(AVFrame *);
	int GetFramesFilled(void) { return atomic_read(&m_framesFilled); };
	void RbPushFrame(AVFrame *);
	AVFrame *RbGetFrame(void);
	void FramesRbLock(void);
	void FramesRbUnlock(void);
	bool IsInterlacedFrame(AVFrame *);
	bool IsKeyFrame(AVFrame *);
	void ScheduleDisplayBlackFrame(void) { m_displayBlackFrame = true; };
	void DestroyFrameBuffers(void);
	void ClearDecoderToDisplayQueue(void);
	bool IsBufferFull(void);

	// Filter
	void ClearFramesToFilter(void) { m_numFramesToFilter = 0; };
	void IncFramesToFilter(void) { m_numFramesToFilter++; };
	void DecFramesToFilter(void) { m_numFramesToFilter--; };

#ifdef USE_GLES
	// GLES
	EGLSurface EglSurface(void) { return m_pDrmDevice->EglSurface(); };
	EGLDisplay EglDisplay(void) { return m_pDrmDevice->EglDisplay(); };
	EGLContext EglContext(void) { return m_pDrmDevice->EglContext(); };
	int GlInitiated(void) { return m_pDrmDevice->GlInitiated(); };
#endif

private:
	cSoftHdDevice *m_pDevice;           ///< pointer to cSoftHdDevice
	cSoftHdAudio *m_pAudio;	            ///< pointer to cSoftHdAudio
	cDecodingThread *m_pDecodingThread; ///< pointer to decoding thread
	cDisplayThread *m_pDisplayThread;   ///< pointer to display thread
	cFilterThread *m_pFilterThread;     ///< pointer to deinterlace filter thread
	cMutex m_waitCleanMutex;            ///< mutex used while display cleanup
	cMutex m_trickspeedMutex;           ///< mutex used while accessing trickspeed parameters
	cMutex m_playbackMutex;             ///< mutex used around m_videoIsPaused
	cMutex m_videoClockMutex;           ///< mutex used around m_pts
	cMutex m_displayQueue;              ///< mutex used while accessing the render ringbuffer

	int m_hardwareQuirks;               ///< hardware specific quirks

	AVFrame *m_framesRb[VIDEO_SURFACES_MAX];  ///< ringbuffer for frames to be displayed (VIDEO_SURFACES_MAX is defined in thread.h)
	int m_framesWrite;                  ///< m_framesRb write pointer
	int m_framesRead;                   ///< m_framesRb read pointer
	atomic_t m_framesFilled;            ///< how many of m_framesRb is used
	int m_trickSpeed;                   ///< current trick speed
	bool m_trickForward;                ///< true, if trickspeed plays forward
	int m_framePresentationCounter = 0; ///< number of times the current frame has to be shown (for slow motion)

	int m_numFramesToFilter;            ///< number of frames to be filtered
	bool m_userDisabledDeinterlacer = false; ///< set, if the user configured the deinterlace to be disabled
	                                    ///< That way, we don't change the current render pipeline (RenderFrame())
	int m_numWrongProgressive;          ///< counter for progressive frames sent in an interlaced stream
	                                    ///< (only used for logging)
	bool m_deinterlacerDeactivated = false; ///< set, if the deinterlacer shall be deactivated temporarily (used for trick speed and still picture)
	bool m_checkFilterThreadNeeded;     ///< set, if we have to check, if filter thread is needed at start of playback

	bool m_disableOglOsd;               ///< set, if ogl osd is disabled

	bool m_startgrab;                   ///< internal flag to trigger grabbing
	cCondVar m_grabCond;                ///< condition gets signalled, if renederer finished to clone the grabbed buffers
	cSoftHdGrab m_grabOsd;              ///< keeps the current grabbed osd
	cSoftHdGrab m_grabVideo;            ///< keeps the current grabbed video
	cRect m_lastVideoGrab;              ///< crtc rect of the last shown video frame

	int m_startCounter;                 ///< counter for displayed frames, indicates a video start
	int m_framesDuped = 0;              ///< number of frames duplicated
	int m_framesDropped = 0;            ///< number of frames dropped
	AVRational m_timebase;              ///< timebase used for pts, set by first RenderFrame()
	cMutex m_timebaseMutex;             ///< mutex used around m_timebase
	int64_t m_pts;                      ///< current video PTS

	cRect m_videoRect;                  ///< rect of the currently displayed video
	bool m_videoIsScaled;               ///< true, if the currently displayed video is scaled

	cDrmDevice *m_pDrmDevice;           ///< pointer cDrmDevice object
	cDrmBuffer m_buffer[RENDERBUFFERS]; ///< array of video drm buffer objects
	cDrmBuffer *m_pBufOsd;              ///< pointer to osd drm buffer object
	cDrmBuffer m_bufBlack;              ///< black drm buffer object
	cDrmBuffer *m_pCurrentlyDisplayed = nullptr; ///< pointer to currently displayed DRM buffer
	int m_numBuffers = 0;               ///< number of framebuffers currently set up
	int m_enqueueBufferIdx;             ///< index of the current (sw) framebuffer in the array
	bool m_osdShown;                    ///< set, if osd is shown currently
	bool m_displayBlackFrame = false;   ///< set, if a black frame shall be displayed
	bool m_destroyCurrentlyDisplayed = false; ///< set, if the currently displayed buffer shall be destroyed in the display thread
	bool m_playbackPaused = false;		///< set, if playback is frozen (used for pause)

#ifdef USE_GLES
	struct gbm_bo *m_bo;                ///< pointer to current gbm buffer object
	struct gbm_bo *m_pOldBo;            ///< pointer to old gbm buffer object (for later free)
	struct gbm_bo *m_pNextBo;           ///< pointer to next gbm buffer object (for later free)
#endif
	int GetFrameFlags(AVFrame *);
	void SetFrameFlags(AVFrame *, int);
	void SetVideoClock(int64_t);
	bool ShouldWaitForAudio(void);
	void WaitForAudioReady(int64_t, int64_t);
	void WaitForAudioClock(int64_t *);
	int HandleDropDup(int64_t, int64_t);
	void PageFlip(AVFrame *, cDrmBuffer *, int);
	void PageFlipBlack(void);
	void PageFlipOsd(void);
	void PageFlipVideo(AVFrame *, cDrmBuffer *);
	cDrmBuffer *GetBuffer(AVFrame *);
	int SetOsdBuffer(drmModeAtomicReqPtr);
	void SetVideoBuffer(cDrmBuffer *);
	int CommitBuffer(cDrmBuffer *, int);
	void Grab(cDrmBuffer *);
};

#endif
