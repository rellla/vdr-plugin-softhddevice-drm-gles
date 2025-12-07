/**
 * @file videorender.cpp
 * Rendering class
 *
 * This file defines cVideoRender, which includes all methods to
 * bring the video and osd to display.
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

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <algorithm>
#include <atomic>
#include <functional>

#include <stdbool.h>
#include <unistd.h>

#include <inttypes.h>

#include <libintl.h>

#ifdef USE_GLES
#include <assert.h>
#endif
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <drm_fourcc.h>

#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}

#include "misc.h"
#include "buf2rgb.h"

#include "videorender.h"
#include "audio.h"
#include "drm.h"
#include "threads.h"
#include "grab.h"
#include "drmdevice.h"

/*****************************************************************************
 * cVideoRender class
 ****************************************************************************/

/**
 * cVideoRender constructor
 *
 * @param device         pointer to cSoftHdDevice
 */
cVideoRender::cVideoRender(cSoftHdDevice *device)
{
	m_pEventReceiver = device;
	m_pDevice = device;
	m_pConfig = m_pDevice->Config();
	m_pAudio = m_pDevice->Audio();
	m_pDrmDevice = new cDrmDevice(this, m_pConfig->ConfigDisplayResolution);

	m_startgrab = false;
	m_startCounter = 0;
	m_videoIsScaled = false;

	m_trickSpeed = 0;
	m_trickForward = true;

	m_numFramesToFilter = 0;

	m_pipFramesFilled = 0;
	m_pipFramesWrite = 0;
	m_pipFramesRead = 0;

	m_timebase = av_make_q(1, 90000);

	m_pBufOsd = nullptr;

	m_userDisabledDeinterlacer = m_pConfig->ConfigDisableDeint;
#ifdef USE_GLES
	m_disableOglOsd = m_pConfig->ConfigDisableOglOsd;
	m_bo = nullptr;
	m_pNextBo = nullptr;
	m_pOldBo = nullptr;
#endif
	SetPipSize(m_pConfig->ConfigPipUseAlt);
}

/**
 * cVideoRender destructor
 */
cVideoRender::~cVideoRender(void)
{
	LOGDEBUG2(L_DRM, "videorender: %s", __FUNCTION__);
	if (m_pFilterThread)
		delete m_pFilterThread;
	if (m_pPipFilterThread)
		delete m_pPipFilterThread;
	if (m_pDisplayThread)
		delete m_pDisplayThread;

	delete m_pDrmDevice;
}

/**
 * Clear (empty) the decoder to display queue
 */
void cVideoRender::ClearDecoderToDisplayQueue(void)
{
	std::lock_guard<std::mutex> lock(m_drmBufferPeekMutex);

	m_drmBufferQueue.Clear();
	m_drmBufferPool.DestroyAllExcept(m_pCurrentlyDisplayed);
}

/**
 * Clear (empty) the decoder to display queue
 */
void cVideoRender::ClearPipDecoderToDisplayQueue(void)
{
	PipFramesRbLock();
	while (atomic_read(&m_pipFramesFilled)) {
		AVFrame *frame = PipRbGetFrame();
		av_frame_free(&frame);
	}
	PipFramesRbUnlock();
}

/**
 * Destroy all frame buffers, except the currently displayed one.
 */
void cVideoRender::DestroyFrameBuffers(void)
{
	for (int i = 0; i < RENDERBUFFERS; ++i) {
		if (!m_buffer[i].IsDirty() || m_pCurrentlyDisplayed == &m_buffer[i])
			continue; // let the display thread destroy the currently displayed frame, once the following frame has been displayed

		m_buffer[i].Destroy();
	}

	m_numBuffers = 0;
	m_enqueueBufferIdx = 0;

	if (m_pCurrentlyDisplayed)
		m_pCurrentlyDisplayed->SetDestroyAfterUse(true); // destroy the currently displayed frame only after it has left the screen
}

/**
 * Destroy all frame buffers, except the currently displayed one.
 */
void cVideoRender::DestroyPipFrameBuffers(void)
{
	for (int i = 0; i < RENDERBUFFERS; ++i) {
		if (!m_pipBuffer[i].IsDirty())
			continue;

		m_pipBuffer[i].Destroy();
	}

	m_numPipBuffers = 0;
	m_enqueuePipBufferIdx = 0;
	m_pCurrentlyPipDisplayed = nullptr;
}

struct sRect {
	uint64_t x;
	uint64_t y;
	uint64_t w;
	uint64_t h;
};

/**
 * Fits the video frame into a given area
 *
 * @param frame        AVFrame with frame dimensions and aspect ratio information
 * @param dispX        x offset of video area
 * @param dispY        y offset of video area
 * @param dispWidth    width of video area
 * @param dispHeight   height of video area
 *
 * @returns            the new computed video area or the given area if no frame was given
 */
static sRect ComputeFittedRect(AVFrame *frame, uint64_t dispX, uint64_t dispY, uint64_t dispWidth, uint64_t dispHeight)
{
	if (!frame || dispWidth == 0 || dispHeight == 0)
		return { dispX, dispY, dispWidth, dispHeight };

	double frameWidth = frame->width > 0 ? frame->width : 1.0;
	double frameHeight = frame->height > 0 ? frame->height : 1.0;
	double frameSar = av_q2d(frame->sample_aspect_ratio) ? av_q2d(frame->sample_aspect_ratio) : 1.0;
	double dispAspect = static_cast<double>(dispWidth) / static_cast<double>(dispHeight);
	double frameAspect = frameWidth / frameHeight * frameSar;

	double picWidthD = dispWidth;
	double picHeightD = dispHeight;

	if (dispAspect > frameAspect) {
		// letterbox horizontally (frame narrower than display)
		picWidthD = dispHeight * frameAspect;
		if (picWidthD <= 0 || picWidthD > dispWidth)
			picWidthD = dispWidth;
	} else {
		// pillarbox vertically (frame wider than display)
		picHeightD = dispWidth / frameAspect;
		if (picHeightD <= 0 || picHeightD > dispHeight)
			picHeightD = dispHeight;
	}

	// round to the nearest pixel
	uint64_t picWidth = std::llround(std::max(0.0, picWidthD));
	uint64_t picHeight = std::llround(std::max(0.0, picHeightD));

	int64_t offsetX = static_cast<int64_t>(dispWidth) - static_cast<int64_t>(picWidth);
	int64_t offsetY = static_cast<int64_t>(dispHeight) - static_cast<int64_t>(picHeight);
	uint64_t posX = dispX + static_cast<uint64_t>(std::max<int64_t>(0, offsetX / 2));
	uint64_t posY = dispY + static_cast<uint64_t>(std::max<int64_t>(0, offsetY / 2));

	return { posX, posY, picWidth, picHeight };
}

/**
 * Destroy all frame buffers, except the currently displayed one.
 */
void cVideoRender::DestroyPipFrameBuffers(void)
{
	for (int i = 0; i < RENDERBUFFERS; ++i) {
		if (!m_pipBuffer[i].IsDirty())
			continue;

		m_pipBuffer[i].Destroy();
	}

	m_numPipBuffers = 0;
	m_enqueuePipBufferIdx = 0;
	m_pCurrentlyPipDisplayed = nullptr;
}

struct sRect {
	uint64_t x;
	uint64_t y;
	uint64_t w;
	uint64_t h;
};

/**
 * Fits the video frame into a given area
 *
 * @param frame        AVFrame with frame dimensions and aspect ratio information
 * @param dispX        x offset of video area
 * @param dispY        y offset of video area
 * @param dispWidth    width of video area
 * @param dispHeight   height of video area
 *
 * @returns            the new computed video area or the given area if no frame was given
 */
static sRect ComputeFittedRect(AVFrame *frame, uint64_t dispX, uint64_t dispY, uint64_t dispWidth, uint64_t dispHeight)
{
	if (!frame || dispWidth == 0 || dispHeight == 0)
		return { dispX, dispY, dispWidth, dispHeight };

	double frameWidth = frame->width > 0 ? frame->width : 1.0;
	double frameHeight = frame->height > 0 ? frame->height : 1.0;
	double frameSar = av_q2d(frame->sample_aspect_ratio) ? av_q2d(frame->sample_aspect_ratio) : 1.0;
	double dispAspect = static_cast<double>(dispWidth) / static_cast<double>(dispHeight);
	double frameAspect = frameWidth / frameHeight * frameSar;

	double picWidthD = dispWidth;
	double picHeightD = dispHeight;

	if (dispAspect > frameAspect) {
		// letterbox horizontally (frame narrower than display)
		picWidthD = dispHeight * frameAspect;
		if (picWidthD <= 0 || picWidthD > dispWidth)
			picWidthD = dispWidth;
	} else {
		// pillarbox vertically (frame wider than display)
		picHeightD = dispWidth / frameAspect;
		if (picHeightD <= 0 || picHeightD > dispHeight)
			picHeightD = dispHeight;
	}

	// round to the nearest pixel
	uint64_t picWidth = std::llround(std::max(0.0, picWidthD));
	uint64_t picHeight = std::llround(std::max(0.0, picHeightD));

	int64_t offsetX = static_cast<int64_t>(dispWidth) - static_cast<int64_t>(picWidth);
	int64_t offsetY = static_cast<int64_t>(dispHeight) - static_cast<int64_t>(picHeight);
	uint64_t posX = dispX + static_cast<uint64_t>(std::max<int64_t>(0, offsetX / 2));
	uint64_t posY = dispY + static_cast<uint64_t>(std::max<int64_t>(0, offsetY / 2));

	return { posX, posY, picWidth, picHeight };
}

/**
 * Modesetting for video
 *
 * @param[in] buf    drm video buffer to display
 */
void cVideoRender::SetVideoBuffer(cDrmBuffer *buf)
{
	AVFrame *frame = buf ? buf->Frame() : nullptr;

	// set display dimensions as default
	uint64_t dispWidth = m_pDrmDevice->DisplayWidth();
	uint64_t dispHeight = m_pDrmDevice->DisplayHeight();
	uint64_t dispX = 0;
	uint64_t dispY = 0;

	cDrmPlane *videoPlane = m_pDrmDevice->VideoPlane();

	// get video size and position
	if (m_videoIsScaled) {
		dispWidth = m_videoRect.Width();
		dispHeight = m_videoRect.Height();
		dispX = m_videoRect.X();
		dispY = m_videoRect.Y();
	}

	// fit frame into display
	sRect fittedRect = ComputeFittedRect(frame, dispX, dispY, dispWidth, dispHeight);

	// now set the plane parameters
	videoPlane->SetParams(m_pDrmDevice->CrtcId(), buf->Id(),
		fittedRect.x, fittedRect.y, fittedRect.w, fittedRect.h,
		0, 0, buf->Width(), buf->Height());

	// set dimensions for grab early, because we might skip this at the next frame
	m_lastVideoGrab.Set(fittedRect.x, fittedRect.y, fittedRect.w, fittedRect.h);
}

/**
 * Modesetting for osd
 *
 * @retval 1     osd is not dirty, do nothing
 * @retval 0     osd modesetting was done
 */
int cVideoRender::SetOsdBuffer(drmModeAtomicReqPtr modeReq)
{
	cDrmPlane *videoPlane = m_pDrmDevice->VideoPlane();
	cDrmPlane *osdPlane = m_pDrmDevice->OsdPlane();

	// We had draw activity on the osd buffer
	if (m_pBufOsd && m_pBufOsd->IsDirty()) {
		if (m_pDrmDevice->UseZpos()) {
			videoPlane->SetZpos(m_osdShown ? m_pDrmDevice->ZposPrimary() : m_pDrmDevice->ZposOverlay());
			osdPlane->SetZpos(m_osdShown ? m_pDrmDevice->ZposOverlay() : m_pDrmDevice->ZposPrimary());
			videoPlane->SetPlaneZpos(modeReq);
			osdPlane->SetPlaneZpos(modeReq);

			LOGDEBUG2(L_DRM, "videorender: %s: SetPlaneZpos: video->plane_id %d -> zpos %" PRIu64 ", osd->plane_id %d -> zpos %" PRIu64 "", __FUNCTION__,
				videoPlane->GetId(), videoPlane->GetZpos(),
				osdPlane->GetId(), osdPlane->GetZpos());
		}

		uint64_t crtcW = m_osdShown ? m_pBufOsd->Width() : 0;
		uint64_t crtcH = m_osdShown ? m_pBufOsd->Height() : 0;

		// now set the plane parameters
		osdPlane->SetParams(m_pDrmDevice->CrtcId(), m_pBufOsd->Id(),
			0, 0, crtcW, crtcH,
			0, 0, crtcW, crtcH);

		m_pBufOsd->MarkClean();
		return 0;
	}

	return 1;
}

/**
 * Modesetting for pip
 *
 * @param[in] buf    drm video buffer to display
 */
void cVideoRender::SetPipBuffer(cDrmBuffer *buf)
{
	AVFrame *frame = buf ? buf->Frame() : nullptr;

	// set display dimensions as default
	uint64_t dispWidth = m_pDrmDevice->DisplayWidth();
	uint64_t dispHeight = m_pDrmDevice->DisplayHeight();
	uint64_t dispX = 0;
	uint64_t dispY = 0;

	cDrmPlane *pipPlane = m_pDrmDevice->PipPlane();

	// Get video size and position
	if (m_videoIsScaled) {
		dispWidth = m_videoRect.Width();
		dispHeight = m_videoRect.Height();
		dispX = m_videoRect.X();
		dispY = m_videoRect.Y();
	}

	// fit frame into display
	sRect fittedRect = ComputeFittedRect(frame, dispX, dispY, dispWidth, dispHeight);

	// compute pip window with given scaling and positioning values from menu
	int64_t centerOffsetX = static_cast<int64_t>(dispWidth) - static_cast<int64_t>(fittedRect.w);
	int64_t centerOffsetY = static_cast<int64_t>(dispHeight) - static_cast<int64_t>(fittedRect.h);
	centerOffsetX = std::max<int64_t>(0, centerOffsetX / 2);
	centerOffsetY = std::max<int64_t>(0, centerOffsetY / 2);

	double crtcWD = fittedRect.w * m_pipScalePercent / 100.0;
	double crtcHD = fittedRect.h * m_pipScalePercent / 100.0;
	uint64_t crtcW = std::llround(crtcWD);
	uint64_t crtcH = std::llround(crtcHD);

	double spaceW = dispWidth - crtcW - centerOffsetX;
	double spaceH = dispHeight - crtcH - centerOffsetY;

	uint64_t crtcX = dispX + std::llround(spaceW * m_pipLeftPercent / 100.0 + centerOffsetX * m_pipScalePercent / 100.0);
	uint64_t crtcY = dispY + std::llround(spaceH * m_pipTopPercent / 100.0 + centerOffsetY * m_pipScalePercent / 100.0);

	// now set the plane parameters
	pipPlane->SetParams(m_pDrmDevice->CrtcId(), buf->Id(),
		crtcX, crtcY, crtcW, crtcH,
		0, 0, buf->Width(), buf->Height());

	// set dimensions for grab early, because we might skip this at the next frame
	m_lastPipGrab.Set(crtcX, crtcY, crtcW, crtcH);
}

/**
 * Grab video and osd
 *
 * @param buf       video drm buffer
 *                  if buf was (currently) set, it is used for the grab,
 *                  otherwise the last displayed buffer is used (pause mode)
 */
void cVideoRender::Grab(cDrmBuffer *buf, cDrmBuffer *pip)
{
	if (m_pBufOsd && m_osdShown) {
		LOGDEBUG2(L_GRAB, "videorender: %s: Trigger osd grab arrived", __FUNCTION__);
		cDrmBuffer *osdBuf = new cDrmBuffer(m_pBufOsd);
		// dimensions should be the size on screen
		m_grabOsd.SetRect(0, 0, m_pBufOsd->Width(), m_pBufOsd->Height());
		m_grabOsd.SetBuf(osdBuf);
	}

	cDrmBuffer *pbuf = buf ? buf : (m_pCurrentlyDisplayed ? m_pCurrentlyDisplayed : NULL);
	if (pbuf) {
		LOGDEBUG2(L_GRAB, "videorender: %s: Trigger video grab arrived", __FUNCTION__);
		cDrmBuffer *videoBuf = new cDrmBuffer(pbuf);
		// use dimensions which have been set earlier
		m_grabVideo.SetRect(m_lastVideoGrab.X(), m_lastVideoGrab.Y(), m_lastVideoGrab.Width(), m_lastVideoGrab.Height());
		m_grabVideo.SetBuf(videoBuf);
	}

	cDrmBuffer *pipBuf = pip ? pip : (m_pCurrentlyPipDisplayed ? m_pCurrentlyPipDisplayed : NULL);
	if (pipBuf) {
		LOGDEBUG2(L_GRAB, "videorender: %s: Trigger pip grab arrived", __FUNCTION__);
		cDrmBuffer *pipVideoBuf = new cDrmBuffer(pipBuf);
		// use dimensions which have been set earlier
		m_grabPip.SetRect(m_lastPipGrab.X(), m_lastPipGrab.Y(), m_lastPipGrab.Width(), m_lastPipGrab.Height());
		m_grabPip.SetBuf(pipVideoBuf);
	}

	m_grabCond.Broadcast();
}

/**
 * Commit the frame to the hardware
 *
 * @param buf        video drm buffer
 * @param osdOnly    commit only osd
 *
 * @retval 0         modesetting and commit was done, need to process outstanding DRM events
 * @retval -1        no modesetting and commit was done
 */
int cVideoRender::CommitBuffer(cDrmBuffer *buf, cDrmBuffer *pip, int osdOnly)
{
	enum modeSetLevel {
		MODESET_OSD   = (1 << 0),
		MODESET_VIDEO = (1 << 1),
		MODESET_PIP   = (1 << 2)
	};

	int modeSet = 0;
	int fdDrm = m_pDrmDevice->Fd();
	cDrmPlane *videoPlane = m_pDrmDevice->VideoPlane();
	cDrmPlane *osdPlane = m_pDrmDevice->OsdPlane();
	cDrmPlane *pipPlane = m_pDrmDevice->PipPlane();
	drmModeAtomicReqPtr modeReq;
	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;

	if (!(modeReq = drmModeAtomicAlloc())) {
		LOGERROR("videorender: %s: cannot allocate atomic request (%d): %m", __FUNCTION__, errno);
		return -1;
	}

	// handle the video plane
	if (!osdOnly) {
		SetVideoBuffer(buf);
		videoPlane->SetPlane(modeReq);
		modeSet |= MODESET_VIDEO;
//		LOGDEBUG2(L_DRM, "videorender: %s: SetPlane Video (fb = %" PRIu64 ")", __FUNCTION__, videoPlane->GetFbId());
	} else if (m_pCurrentlyDisplayed) {
		// If no new video is available, set the old buffer again, if available.
		// This is necessary to recognize a size-change in SetVideoBuffer().
		// Though this is not expensive, maybe we should only call that, if size really changed.
		SetVideoBuffer(m_pCurrentlyDisplayed);
		videoPlane->SetPlane(modeReq);
		modeSet |= MODESET_VIDEO;
	}

	// handle the pip plane
	if (IsPipActive() && pip) {
		SetPipBuffer(pip);
		pipPlane->SetPlane(modeReq);
		modeSet |= MODESET_PIP;
	} else if (IsPipActive() && m_pCurrentlyPipDisplayed) {
		SetPipBuffer(m_pCurrentlyPipDisplayed);
		pipPlane->SetPlane(modeReq);
		modeSet |= MODESET_PIP;
	} else {
		pipPlane->ClearPlane(modeReq);
		modeSet |= MODESET_PIP;
	}

	// handle the osd plane
	if (!SetOsdBuffer(modeReq)) {
		osdPlane->SetPlane(modeReq);
		modeSet |= MODESET_OSD;
		LOGDEBUG2(L_DRM, "videorender: %s: SetPlane OSD %d (fb = %" PRIu64 ")", __FUNCTION__, m_osdShown, osdPlane->GetFbId());
	}

	// grab, if requested
	if (m_startgrab)
		Grab(buf, pip);

	// return without an atomic commit (no video frame and osd activity)
	if (!modeSet) {
		drmModeAtomicFree(modeReq);
		return -1;
	}

	// do the atomic commit
	if (drmModeAtomicCommit(fdDrm, modeReq, flags, NULL) != 0) {
		if (modeSet & MODESET_OSD)
			osdPlane->DumpParameters();
		if (modeSet & MODESET_VIDEO)
			videoPlane->DumpParameters();
		if (modeSet & MODESET_PIP)
			pipPlane->DumpParameters();

		drmModeAtomicFree(modeReq);
		LOGERROR("videorender: %s: page flip failed (%d): %m", __FUNCTION__, errno);
		return -1;
	}

	drmModeAtomicFree(modeReq);

	return 0;
}

/**
 * Log A/V sync debug message
 *
 * @param audioPtsMs               audio pts
 * @param videoPtsMs               video pts
 * @param audioBehindVideoByMs     audio is behind video by this many ms
 */
void cVideoRender::LogDroppedDuped(int64_t audioPtsMs, int64_t videoPtsMs, int audioBehindVideoByMs)
{
	LOGERROR("Frame %s (drop %d, dup %d) Pkts %d Frames %d UsedBytes %d audio %s video %s Delay %dms diff %dms",
		audioBehindVideoByMs > 0 ? "duped" : "dropped",
		m_framesDropped,
		m_framesDuped,
		m_pDevice->VideoStream()->GetAvPacketsFilled(),
		m_drmBufferQueue.Size(),
		m_pAudio->GetUsedBytes(),
		Timestamp2String(audioPtsMs, 1),
		Timestamp2String(videoPtsMs, 1),
		m_pDevice->GetVideoAudioDelayMs(),
		audioBehindVideoByMs);

	if (audioBehindVideoByMs > 0)
		m_framesDuped++;
	else
		m_framesDropped++;

}

/**
 * Get frame flags
 *
 * @param frame	      AVFrame
 *
 * @returns           FRAME_FLAG_TRICKSPEED or FRAME_FLAG_STILLPICTURE
 */
int cVideoRender::GetFrameFlags(AVFrame *frame)
{
	if (!frame || !frame->opaque_ref)
		return 0;

	int *frameFlags = (int *)frame->opaque_ref->data;
	return *frameFlags;
}

/**
 * Set frame flags
 *
 * @param frame     AVFrame
 * @param flags     FRAME_FLAG_TRICKSPEED and/or FRAME_FLAG_STILLPICTURE
 */
void cVideoRender::SetFrameFlags(AVFrame *frame, int flags)
{
	int *frameFlags;
	if (!frame->opaque_ref) {
		frame->opaque_ref = av_buffer_allocz(sizeof(*frameFlags));
		if (!frame->opaque_ref) {
			LOGFATAL("videorender: %s: cannot allocate private frame data", __FUNCTION__);
		}
	}

	frameFlags = (int *)frame->opaque_ref->data;
	*frameFlags = flags;
}

/**
 * Check, if this is an interlaced frame
 *
 * @param frame    AVFrame
 *
 * @return         true, if this frame is an interlaced frame
 */
bool cVideoRender::IsInterlacedFrame(AVFrame *frame)
{
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
	return frame->interlaced_frame;
#else
	return frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
}

/**
 * Do the pageflip
 *
 * @param frame     AVFrame
 * @param buf       drm buffer
 */
void cVideoRender::PageFlip(AVFrame *frame, cDrmBuffer *buf, AVFrame *pipFrame, cDrmBuffer *pipBuf, int osdOnly)
{
	if (CommitBuffer(buf, pipBuf, osdOnly) < 0) {
		// no modesetting was done
		if (frame)
			av_frame_free(&frame);
		if (pipFrame)
			av_frame_free(&pipFrame);
	} else {
		if (m_pDrmDevice->HandleEvent() != 0)
			LOGERROR("threads: display thread: drmHandleEvent failed!");

		// now, that we had a successful commit, set the STC if we have a frame. Skip if only the OSD was updated.
		if (frame && !osdOnly) {
			if (frame->pts != AV_NOPTS_VALUE)
				SetVideoClock(frame->pts);

			LOGDEBUG2(L_PACKET, "videorender: %s: ID %d:                 PTS %s", __FUNCTION__, buf->Id(), Timestamp2String(frame->pts, 90));
		}
	}
}

/**
 * Do the pageflip and set a black buffer for the video
 *
 */
void cVideoRender::PageFlipBlack(cDrmBuffer *pipBuf, AVFrame *pipFrame)
{
	PageFlip(NULL, &m_bufBlack, pipFrame, pipBuf, 0);
}

/**
 * Do the pageflip for osd and skip the video
 */
void cVideoRender::PageFlipOsd(cDrmBuffer *pipBuf, AVFrame *pipFrame)
{
	PageFlip(NULL, NULL, pipFrame, pipBuf, 1);
}

/**
 * Do the pageflip for osd and/or video
 *
 * @param frame     AVFrame
 * @param buf       drm buffer
 */
void cVideoRender::PageFlipVideo(cDrmBuffer *buf, cDrmBuffer *pipBuf, AVFrame *pipFrame)
{
	PageFlip(buf, pipFrame , pipBuf, 0);
}

/**
 * Display the frame (video and/or osd)
 */
void cVideoRender::DisplayFrame()
{
	if (m_pDevice->IsBufferingThresholdReached())
		m_eventQueue.push_back(BufferingThresholdReachedEvent{});

	if (m_drmBufferQueue.IsEmpty() && !m_videoPlaybackPaused && m_schedulePlaybackStartAtPtsMs == AV_NOPTS_VALUE && !GetTrickSpeed())
		m_eventQueue.push_back(BufferUnderrunEvent{VIDEO});

	cDrmBuffer *pipBuf = nullptr;
	if (pipFrame) {
		pipBuf = GetPipBuffer(pipFrame);
		if (!pipBuf) {
			av_frame_free(&pipFrame);
			LOGERROR("videorender: %s: getting buffer for pip frame failed", __FUNCTION__);
		} else {
			pipBuf->SetFrame(pipFrame);
		}
	}

	if (m_displayBlackFrame) {
		LOGDEBUG2(L_DRM, "videorender: %s: closing, set a black FB", __FUNCTION__);

		PageFlipBlack(pipBuf, pipFrame);

		if (m_pCurrentlyDisplayed) {
			m_pCurrentlyDisplayed->Destroy();

			AVFrame *frame = m_pCurrentlyDisplayed->Frame();
			av_frame_free(&frame);

			m_pCurrentlyDisplayed = nullptr;
			m_destroyCurrentlyDisplayed = false;
		}

		m_displayBlackFrame = false;
	}

	if (m_framePresentationCounter == 0)
		m_framePresentationCounter = std::max(1, GetTrickSpeed());

		{
			std::lock_guard<std::mutex> lock(m_drmBufferPeekMutex);
			if (!m_videoPlaybackPaused)
				drmBuffer = m_drmBufferQueue.Pop();
		}
	}

	if (drmBuffer) {
		if (m_schedulePlaybackStartAtPtsMs != AV_NOPTS_VALUE) {
			// check if playback shall start
			if (PtsToMs(drmBuffer->frame->pts) < m_schedulePlaybackStartAtPtsMs) {
				drmBuffer->PresentationFinished();
				return;
			} else
				m_schedulePlaybackStartAtPtsMs = AV_NOPTS_VALUE;
		} else if (!m_displayOneFrameThenPause) {
			// A/V sync
			int64_t audioPtsMs = m_pAudio->GetHardwareOutputPtsMs();
			int64_t videoPtsMs = PtsToMs(drmBuffer->frame->pts);

			if (audioPtsMs != AV_NOPTS_VALUE) {
				int audioBehindVideoByMs = videoPtsMs - audioPtsMs - m_pDevice->GetVideoAudioDelayMs();

				if (m_resumeAudioScheduled && audioBehindVideoByMs >= 0) { // resume audio from pause
					LOGDEBUG2(L_AV_SYNC, "videorender: resuming audio playback: video %s, audio %s", Timestamp2String(videoPtsMs, 1), Timestamp2String(audioPtsMs, 1));
					m_pAudio->SetPaused(false);
					m_resumeAudioScheduled = false;
				} else if (!m_pAudio->IsPaused() && audioBehindVideoByMs > AV_SYNC_THRESHOLD_AUDIO_BEHIND_VIDEO_MS) { // duplicate frame
					LogDroppedDuped(audioPtsMs, videoPtsMs, audioBehindVideoByMs);

					m_framePresentationCounter++; // display the current video frame one period longer
				} else if (!m_pAudio->IsPaused() && !m_lastFrameWasDropped && audioBehindVideoByMs < -AV_SYNC_THRESHOLD_AUDIO_AHEAD_VIDEO_MS) { // drop frame
					// Drop max every second frame. Otherwise, the buffer gets drained immediately, if multiple frames in a row are dropped.
					LogDroppedDuped(audioPtsMs, videoPtsMs, audioBehindVideoByMs);

					if (pipFrame)
						av_frame_free(&pipFrame);

					drmBuffer->PresentationFinished();
					m_framePresentationCounter--; // skip this pageflip
					m_lastFrameWasDropped = true;

					return;
				}

				m_startCounter++;
			}

			if (m_videoPlaybackPaused || GetTrickSpeed())
				m_pAudio->DropSamplesOlderThanPtsMs(drmBuffer->frame->pts * 1000 * av_q2d(m_timebase));
		}

		if (m_displayOneFrameThenPause) {
			m_videoPlaybackPaused = true;
			m_displayOneFrameThenPause = false;
		}

		PageFlipVideo(drmBuffer, pipBuffer, pipFrame);

		if (m_pCurrentlyDisplayed) {
			m_pCurrentlyDisplayed->PresentationFinished();
			m_pCurrentlyDisplayed = nullptr;
		}

		m_lastFrameWasDropped = false;
		m_pCurrentlyDisplayed = drmBuffer;
	} else if ((m_pBufOsd && m_pBufOsd->IsDirty())) {
		PageFlipOsd(pipBuf, pipFrame);
	} else if (!m_drmBufferQueue.IsEmpty() && !m_videoPlaybackPaused && m_pCurrentlyDisplayed) {
		// display the current frame again in trick speed mode or for A/V syncing.
		PageFlipVideo(m_pCurrentlyDisplayed, pipBuf, pipFrame);
	} else if (pipBuf || m_startgrab) {
		PageFlipBlack(pipBuf, pipFrame);
	}

	if (pipFrame) {
		if (m_pCurrentlyPipDisplayed && m_pCurrentlyPipDisplayed != pipBuf) { // do not destroy the frame, if it is displayed again
			AVFrame *frame = m_pCurrentlyPipDisplayed->Frame();
			av_frame_free(&frame);
		}

		m_pCurrentlyPipDisplayed = pipBuf;
	}

	m_framePresentationCounter--;
}

void cVideoRender::DisplayBlackFrame(void)
{
	LOGDEBUG2(L_DRM, "videorender: %s: closing, set a black FB", __FUNCTION__);

	PageFlipBlack();

	if (m_pCurrentlyDisplayed) {
		av_frame_free(&m_pCurrentlyDisplayed->frame);
		m_pCurrentlyDisplayed->Destroy();
		m_pCurrentlyDisplayed = nullptr;
	}
}

int64_t cVideoRender::PtsToMs(int64_t pts)
{
	m_timebaseMutex.Lock();
	int64_t videoPtsMs = pts * 1000 * av_q2d(m_timebase);
	m_timebaseMutex.Unlock();

	return videoPtsMs;
}

/**
 * Wrapper for drmHandleEvent()
 */
int cVideoRender::DrmHandleEvent(void)
{
	return m_pDrmDevice->HandleEvent();
}

/*****************************************************************************
 * OSD
 ****************************************************************************/

/**
 * Clear the OSD (draw an empty/ transparent OSD)
 */
void cVideoRender::OsdClear(void)
{
#ifdef USE_GLES
	if (m_disableOglOsd) {
		memset((void *)m_pBufOsd->Plane(0), 0,
			(size_t)(m_pBufOsd->Pitch(0) * m_pBufOsd->Height()));
	} else {
		cDrmBuffer *buf;

		EGL_CHECK(eglSwapBuffers(m_pDrmDevice->EglDisplay(), m_pDrmDevice->EglSurface()));
		m_pNextBo = gbm_surface_lock_front_buffer(m_pDrmDevice->GbmSurface());
		assert(m_pNextBo);

		buf = m_pDrmDevice->GetBufFromBo(m_pNextBo);
		if (!buf) {
			LOGERROR("videorender: %s: Failed to get GL buffer", __FUNCTION__);
			return;
		}

		m_pBufOsd = buf;

		// release old buffer for writing again
		if (m_bo)
			gbm_surface_release_buffer(m_pDrmDevice->GbmSurface(), m_bo);

		// rotate bos and create and keep bo as m_pOldBo to make it free'able
		m_pOldBo = m_bo;
		m_bo = m_pNextBo;

		LOGDEBUG2(L_OPENGL, "videorender: %s: eglSwapBuffers m_eglDisplay %p eglSurface %p (%i x %i, %i)", __FUNCTION__, m_pDrmDevice->EglDisplay(), m_pDrmDevice->EglSurface(), buf->Width(), buf->Height(), buf->Pitch(0));
	}
#else
	memset((void *)m_pBufOsd->Plane(0), 0,
		(size_t)(m_pBufOsd->Pitch(0) * m_pBufOsd->Height()));
#endif

	m_pBufOsd->MarkDirty();
	m_osdShown = false;
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/**
 * Draw an OSD ARGB image.
 *
 * @param xi         x-coordinate in argb image
 * @param yi         y-coordinate in argb image
 * @param height     height in pixel in argb image
 * @param width      width in pixel in argb image
 * @param pitch      pitch of argb image
 * @param argb       32bit ARGB image data
 * @param x          x-coordinate on screen of argb image
 * @param y          y-coordinate on screen of argb image
 */
void cVideoRender::OsdDrawARGB(int xi, int yi,
                               int width, int height, int pitch,
                               const uint8_t * argb, int x, int y)
{
#ifdef USE_GLES
	if (m_disableOglOsd) {
		LOGDEBUG2(L_OSD, "videorender: %s: width %d height %d pitch %d argb %p x %d y %d pitch buf %d xi %d yi %d", __FUNCTION__,
			width, height, pitch, argb, x, y, m_pBufOsd->Pitch(0), xi, yi);
		for (int i = 0; i < height; ++i) {
			memcpy(m_pBufOsd->Plane(0) + x * 4 + (i + y) * m_pBufOsd->Pitch(0),
				argb + i * pitch, MIN((size_t)pitch, m_pBufOsd->Pitch(0)));
		}
	} else {
		cDrmBuffer *buf;

		EGL_CHECK(eglSwapBuffers(m_pDrmDevice->EglDisplay(), m_pDrmDevice->EglSurface()));
		m_pNextBo = gbm_surface_lock_front_buffer(m_pDrmDevice->GbmSurface());
		assert(m_pNextBo);

		buf = m_pDrmDevice->GetBufFromBo(m_pNextBo);
		if (!buf) {
			LOGERROR("videorender: %s: Failed to get GL buffer", __FUNCTION__);
			return;
		}

		m_pBufOsd = buf;

		// release old buffer for writing again
		if (m_bo)
			gbm_surface_release_buffer(m_pDrmDevice->GbmSurface(), m_bo);

		// rotate bos and create and keep bo as m_pOldBo to make it free'able
		m_pOldBo = m_bo;
		m_bo = m_pNextBo;

		LOGDEBUG2(L_OPENGL, "videorender: %s: eglSwapBuffers eglDisplay %p eglSurface %p (%i x %i, %i)", __FUNCTION__, m_pDrmDevice->EglDisplay(), m_pDrmDevice->EglSurface(), buf->Width(), buf->Height(), buf->Pitch(0));
	}
#else
	// suppress unused variable warnings ...
	(void) xi;
	(void) yi;
	(void) width;

	for (int i = 0; i < height; ++i) {
		memcpy(m_pBufOsd->Plane(0) + x * 4 + (i + y) * m_pBufOsd->Pitch(0),
			argb + i * pitch, (size_t)pitch);
	}
#endif
	m_pBufOsd->MarkDirty();
	m_osdShown = true;
}

/*****************************************************************************
 * Thread
 ****************************************************************************/

/**
 * Stop display thread
 */
void cVideoRender::ExitDisplayThread(void)
{
	LOGDEBUG("videorender: %s", __FUNCTION__);

	Reset();
	if (m_pDisplayThread->Active())
		m_pDisplayThread->Stop();
}

/**
 * Stop filter thread
 */
void cVideoRender::CancelFilterThread(void) {
	if (m_pFilterThread->Active())
		m_pFilterThread->Stop();

	m_checkFilterThreadNeeded = true;
}

/**
 * Stop pip filter thread
 */
void cVideoRender::CancelPipFilterThread(void) {
	if (m_pPipFilterThread->Active())
		m_pPipFilterThread->Stop();

	m_checkPipFilterThreadNeeded = true;
}

/**
 * Callback free primedata if av_buffer is unreferenced
 */
static void ReleaseFrame( __attribute__ ((unused)) void *opaque, uint8_t *data)
{
	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)data;

	av_free(primedata);
}

/**
 * Render a frame
 *
 * Frames either go through the filter thread or directly into the render buffer.
 *
 * @param videoCtx      ffmpeg video codec context
 * @param frame         frame to render
 */
void cVideoRender::RenderFrame(AVCodecContext * videoCtx, AVFrame * frame)
{
	if (frame->decode_error_flags || frame->flags & AV_FRAME_FLAG_CORRUPT)
		LOGWARNING("videorender: %s: error_flag or FRAME_FLAG_CORRUPT", __FUNCTION__);

	// Filter thread will only be started, if the lambda function returns true
	if (m_checkFilterThreadNeeded) {
		m_timebaseMutex.Lock();
		m_timebase = videoCtx->pkt_timebase;
		m_timebaseMutex.Unlock();

		// Enable the deinterlacer only if:
		// - The user did not disable the deinterlacer
		// - The deinterlacer is not temporarily deactivated (trickspeed and still picture)
		// - A hardware quirk does not forbid using the deinterlacer
		// - It is an interlaced stream, determined by:
		//   - The codec is different from HEVC (always progressive)
		//   - The framerate is lower or equal to 30fps
		//   - Or, if the frame's interlaced flag is set
		// We cannot solely rely on the frame's interlaced flag, because the deinterlacer shall also be enabled with mixed progressive/interlaced streams (e.g. TV station "ProSieben").

		bool interlacedStream =
			(videoCtx->codec_id != AV_CODEC_ID_HEVC &&
			videoCtx->framerate.num > 0 &&
			av_q2d(videoCtx->framerate) < 30.1) || IsInterlacedFrame(frame); // account for rounding errors when comparing double

		bool useDeinterlacer =
			!m_userDisabledDeinterlacer &&
			!m_deinterlacerDeactivated &&
			!(m_hardwareQuirks & QUIRK_NO_HW_DEINT) &&
			interlacedStream;

		m_pDevice->VideoStream()->SetInterlaced(interlacedStream);

		if (m_userDisabledDeinterlacer)
			LOGDEBUG("videorender: %s: deinterlacer disabled by user configuration", __FUNCTION__);

		// Use the filter thread if:
		// - AV_PIX_FMT_YUV420P, interlaced -> software deinterlacer (bwdif filter)
		// - AV_PIX_FMT_YUV420P, progressive -> scale filter to get NV12 frames
		// - AV_PIX_FMT_DRM_PRIME, interlaced, hw deinterlacer available -> hw deinterlacer
		return frame->format == AV_PIX_FMT_YUV420P ||
		      (frame->format == AV_PIX_FMT_DRM_PRIME && useDeinterlacer);
	});

	if (m_pFilterThread->Active())
		m_pFilterThread->PushFrame(frame);
	else {
		// AV_PIX_FMT_DRM_PRIME, interlaced, hw deinterlacer not available
		// AV_PIX_FMT_DRM_PRIME, progressive
		// -> put the frame directly into render buffer
		if (!m_numFramesToFilter)
			PushFrame(frame);
	}
}

/**
 * Check, if the render output buffer is full.
 *
 * @retval true     render output buffer is full
  */
bool cVideoRender::IsOutputBufferFull(void)
{
	return m_drmBufferQueue.IsFull();
}

/*****************************************************************************
 * Buffer reuse strategy: use-once
 ****************************************************************************/
cDrmBuffer *cBufferStrategyUseOnce::GetBuffer(cDrmBufferPool *pool, AVDRMFrameDescriptor *)
{
	cDrmBuffer *buf = pool->FindUninitilized();

	if (buf)
		buf->SetDestroyAfterUse(true);

	return buf;
}

/*****************************************************************************
 * Buffer reuse strategy: reuse
 ****************************************************************************/
cDrmBuffer *cBufferStrategyReuseHardware::GetBuffer(cDrmBufferPool *pool, AVDRMFrameDescriptor *primedata)
{
	cDrmBuffer *buf = pool->FindByDmaBufHandle(primedata->objects[0].fd);

	if (buf)
		return buf;
	else
		return pool->FindUninitilized();
}

cDrmBuffer *cBufferStrategyReuseSoftware::GetBuffer(cDrmBufferPool *pool, AVDRMFrameDescriptor *)
{
	cDrmBuffer *buf = pool->FindNoPresentationPending();

	if (buf)
		return buf;
	else
		return pool->FindUninitilized();
}

/*****************************************************************************
 * Decoding strategy: software
 ****************************************************************************/
AVFrame *cDecodingStrategySoftware::PrepareDrmBuffer(cDrmBuffer *buf, int drmDeviceFd, AVFrame *inframe)
{
	if (!buf->IsDirty()) {
		buf->Setup(drmDeviceFd, inframe->width, inframe->height, DRM_FORMAT_NV12, nullptr);

		int dmaBufHandle;
		if (drmPrimeHandleToFD(drmDeviceFd, buf->PrimeHandle(0), DRM_CLOEXEC | DRM_RDWR, &dmaBufHandle))
			LOGFATAL("videorender: %s: Failed to retrieve the Prime FD (%d): %m", __FUNCTION__, errno);

		buf->SetDmaBufHandle(dmaBufHandle);
	}

	for (int i = 0; i < inframe->height; ++i)
		memcpy(buf->Plane(0) + i * buf->Pitch(0), inframe->data[0] + i * inframe->linesize[0], inframe->linesize[0]);

	for (int i = 0; i < inframe->height / 2; ++i)
		memcpy(buf->Plane(1) + i * buf->Pitch(1), inframe->data[1] + i * inframe->linesize[1], inframe->linesize[1]);

	AVFrame *frame = av_frame_alloc();
	frame->pts = inframe->pts;
	frame->width = inframe->width;
	frame->height = inframe->height;
	frame->format = AV_PIX_FMT_DRM_PRIME;
	frame->sample_aspect_ratio = inframe->sample_aspect_ratio;

	frame->format = AV_PIX_FMT_DRM_PRIME;
	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)av_mallocz(sizeof(AVDRMFrameDescriptor));
	primedata->objects[0].fd = buf->DmaBufHandle();
	frame->data[0] = (uint8_t *)primedata;
	frame->buf[0] = av_buffer_create((uint8_t *)primedata, sizeof(*primedata), ReleaseFrame, NULL, AV_BUFFER_FLAG_READONLY);

	av_frame_free(&inframe);

	return frame;
}

/*****************************************************************************
 * Decoding strategy: hardware
 ****************************************************************************/
AVFrame *cDecodingStrategyHardware::PrepareDrmBuffer(cDrmBuffer *buf, int drmDeviceFd, AVFrame *frame)
{
	if (!buf->IsDirty()) {
		AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)frame->data[0];
		buf->Setup(drmDeviceFd, frame->width, frame->height, 0, primedata);
	}

	return frame;
}

/**
 * Push the frame into the render ringbuffer
 */
void cVideoRender::PushFrame(AVFrame *frame)
{
	if (m_bufferReuseStrategy == nullptr) {
		if (GetTrickSpeed() && !GetTrickForward())
			m_bufferReuseStrategy = new cBufferStrategyUseOnce();
		else if (frame->format == AV_PIX_FMT_DRM_PRIME)
			m_bufferReuseStrategy = new cBufferStrategyReuseHardware();
		else
			m_bufferReuseStrategy = new cBufferStrategyReuseSoftware();
	}

	if (m_decodingStrategy == nullptr) {
		if (frame->format == AV_PIX_FMT_DRM_PRIME)
			m_decodingStrategy = new cDecodingStrategyHardware();
		else
			m_decodingStrategy = new cDecodingStrategySoftware();
	}

	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)frame->data[0];
	cDrmBuffer *buf = m_bufferReuseStrategy.load()->GetBuffer(&m_drmBufferPool, primedata);

	if (!buf)
		LOGFATAL("videorender: %s: no free DRM buffer found. This is a bug.", __FUNCTION__);

	frame = m_decodingStrategy.load()->PrepareDrmBuffer(buf, m_pDrmDevice->Fd(), frame);

	buf->frame = frame;
	buf->SetPresentationPending(true);

	m_drmBufferQueue.Push(buf);
}

/**
 * Check if output has a valid PTS
 *
 * @return true if the next frame to display has a valid PTS
 */
bool cVideoRender::HasOutputPts(void)
{
	std::lock_guard<std::mutex> lock(m_drmBufferPeekMutex);

	return !m_drmBufferQueue.IsEmpty() && m_drmBufferQueue.Peek()->frame->pts != AV_NOPTS_VALUE;
}

/**
 * Get the output PTS in milliseconds
 *
 * Returns the presentation timestamp of the next frame to be displayed.
 *
 * @return PTS in milliseconds
 */
int64_t cVideoRender::GetOutputPtsMs(void)
{
	std::lock_guard<std::mutex> lock(m_drmBufferPeekMutex);

	m_timebaseMutex.Lock();
	int64_t pts = m_drmBufferQueue.Peek()->frame->pts * 1000 * av_q2d(m_timebase);
	m_timebaseMutex.Unlock();

	return pts;
}

/**
 * Wrapper to lock the render ringbuffer
 */
void cVideoRender::PipFramesRbLock(void) {
	m_pipDisplayQueue.Lock();
}

/**
 * Wrapper to unlock the render ringbuffer
 */
void cVideoRender::PipFramesRbUnlock(void) {
	m_pipDisplayQueue.Unlock();
}

/**
 * Push the frame into the render ringbuffer
 *
 * @param frame     AVFrame which should go to the ringbuffer
 */
void cVideoRender::PipRbPushFrame(AVFrame *frame) {
	m_pipFramesRb[m_pipFramesWrite] = frame;
	m_pipFramesWrite = (m_pipFramesWrite + 1) % VIDEO_SURFACES_MAX;
	atomic_inc(&m_pipFramesFilled);
}

/**
 * Get a frame from the render ringbuffer
 *
 * @returns        next AVFrame from the ringbuffer
 */
AVFrame *cVideoRender::PipRbGetFrame(void) {
	AVFrame *frame = m_pipFramesRb[m_pipFramesRead];
	m_pipFramesRead = (m_pipFramesRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&m_pipFramesFilled);

	return frame;
}

/**
 * Wrapper to set the video clock (m_pts)
 *
 * @param pts      the pts to be set
 */
void cVideoRender::SetVideoClock(int64_t pts)
{
	m_videoClockMutex.Lock();
	m_pts = pts;
	m_videoClockMutex.Unlock();
}

/**
 * Wrapper to get the video clock (m_pts)
 *
 * @returns the current pts
 */
int64_t cVideoRender::GetVideoClock(void)
{
	int64_t pts;
	m_videoClockMutex.Lock();
	pts = m_pts;
	m_videoClockMutex.Unlock();
	return pts;
}

/**
 * Send start condition to video thread
 */
void cVideoRender::ResetFrameCounter(void)
{
	m_startCounter = 0;
	m_checkFilterThreadNeeded = true;
	LOGDEBUG("videorender: %s: reset m_startCounter %d TrickSpeed %d", __FUNCTION__, m_startCounter, GetTrickSpeed());
}

void cVideoRender::Reset()
{
	m_startCounter = 0;
	m_framesDuped = 0;
	m_framesDropped = 0;
	m_numWrongProgressive = 0;

	delete m_decodingStrategy;
	m_decodingStrategy = nullptr;
}

/**
 * Set the trickspeed parameters
 *
 * @param speed         trick speed value from VDR (0 = normal)
 * @param forward       1 if forward trick speed, 0 if backward
 */
void cVideoRender::SetTrickSpeed(int speed, int forward)
{
	LOGDEBUG2(L_TRICK, "videorender: %s: set trick speed %d %s", __FUNCTION__, speed, forward ? "forward" : "backward");
	m_trickspeedMutex.Lock();
	m_framePresentationCounter = std::max(1, speed); // speed is 0 in normal playback. Set it to 1 to display the frames exactly once.
	m_trickSpeed = speed;
	m_trickForward = forward;
	m_trickspeedMutex.Unlock();
}

/**
 * Get the current trickspeed
 *
 * @returns current trick speed value set with SetTrickSpeed()
 */
int cVideoRender::GetTrickSpeed(void)
{
	int speed;
	m_trickspeedMutex.Lock();
	speed = m_trickSpeed * (m_pDevice->VideoStream()->IsInterlaced() ? 2 : 1);
	m_trickspeedMutex.Unlock();
	return speed;
}

/**
 * Get the current trickspeed direction
 *
 * @retval 1       if forward trickspeed
 * @retval 0       if backward trickspeed
 */
int cVideoRender::GetTrickForward(void)
{
	int dir;
	m_trickspeedMutex.Lock();
	dir = m_trickForward;
	m_trickspeedMutex.Unlock();
	return dir;
}

/*****************************************************************************
 * Grabbing
 ****************************************************************************/

/**
 * Trigger a screen grab
 *
 * @retval 0     on success, grab was triggered
 * @retval 1     on timeout, grab was not triggered
 */
int cVideoRender::TriggerGrab(void)
{
	int timeout = 50;
	cMutex mutex;
	mutex.Lock();
	m_startgrab = true;
	int err = 0;

	if (!m_grabCond.TimedWait(mutex, timeout)) {
		LOGWARNING("videorender: %s: timed out after %dms", __FUNCTION__, timeout);
		err = 1;
	}

	m_startgrab = false;
	return err;
}

/**
 * Convert the video drm buffer to an rgb image
*/
void cVideoRender::ConvertVideoBufToRgb(void)
{
	int size = 0;
	cSoftHdGrab *grab = &m_grabVideo;
	cDrmBuffer *buf = grab->GetBuf();

	// early return if buf = NULL
	if (!buf) {
		grab->SetData(NULL);
		grab->SetSize(0);
		return;
	}

	for (int plane = 0; plane < buf->NumPlanes(); plane++) {
		LOGDEBUG2(L_GRAB, "videorender: %s: VIDEO plane %d address %p pitch %d offset %d handle %d size %d", __FUNCTION__,
			   plane, buf->Plane(plane), buf->Pitch(plane), buf->Offset(plane), buf->PrimeHandle(plane), buf->Size(plane));
	}
	// result's width and height are original dimensions how buffer is presented on the screen
	uint8_t * result = BufToRgb(buf, &size, grab->GetWidth(), grab->GetHeight(), AV_PIX_FMT_RGB24);
	grab->SetData(result);
	grab->SetSize(size);
	grab->FreeBuf();

	return;
}

/**
 * Convert the pip drm buffer to an rgb image
*/
void cVideoRender::ConvertPipBufToRgb(void)
{
	int size = 0;
	cSoftHdGrab *grab = &m_grabPip;
	cDrmBuffer *buf = grab->GetBuf();

	// early return if buf = NULL
	if (!buf) {
		grab->SetData(NULL);
		grab->SetSize(0);
		return;
	}

	for (int plane = 0; plane < buf->NumPlanes(); plane++) {
		LOGDEBUG2(L_GRAB, "videorender: %s: PIP plane %d address %p pitch %d offset %d handle %d size %d", __FUNCTION__,
			   plane, buf->Plane(plane), buf->Pitch(plane), buf->Offset(plane), buf->Handle(plane), buf->Size(plane));
	}
	// result's width and height are original dimensions how buffer is presented on the screen
	uint8_t * result = BufToRgb(buf, &size, grab->GetWidth(), grab->GetHeight(), AV_PIX_FMT_RGB24);
	grab->SetData(result);
	grab->SetSize(size);
	grab->FreeBuf();

	return;
}


/**
 * Convert the osd drm buffer to an rgb image
*/
void cVideoRender::ConvertOsdBufToRgb(void)
{
	int size;
	cSoftHdGrab *grab = &m_grabOsd;
	cDrmBuffer *buf = grab->GetBuf();

	// early return if buf = NULL
	if (!buf) {
		grab->SetData(NULL);
		grab->SetSize(0);
		return;
	}

	for (int plane = 0; plane < buf->NumPlanes(); plane++) {
		LOGDEBUG2(L_GRAB, "videorender: %s: OSD plane %d address %p pitch %d offset %d handle %d size %d", __FUNCTION__,
			   plane, buf->Plane(plane), buf->Pitch(plane), buf->Offset(plane), buf->PrimeHandle(plane), buf->Size(plane));
	}
	// result's width and height are original dimensions how buffer is presented on the screen
	uint8_t * result = BufToRgb(buf, &size, grab->GetWidth(), grab->GetHeight(), AV_PIX_FMT_BGRA);
	grab->SetData(result);
	grab->SetSize(size);
	grab->FreeBuf();

	return;
}

/**
 * Clear the grab drm buffers
*/
void cVideoRender::ClearGrab(void)
{
	if (m_grabOsd.GetBuf())
		m_grabOsd.FreeBuf();
	if (m_grabVideo.GetBuf())
		m_grabVideo.FreeBuf();
	if (m_grabPip.GetBuf())
		m_grabPip.FreeBuf();
}

/**
 * Get the grabbed image
 *
 * @param[out] size       returns output size (memory)
 * @param[out] width      returns output width
 * @param[out] height     returns output height
 * @param[in] type        0: video, 1: osd, 2: pip
 *
 * @returns the pointer to the cSoftHdGrab object
 */
cSoftHdGrab *cVideoRender::GetGrab(int *size, int *width, int *height, int *x, int *y, int type)
{
	cSoftHdGrab *grab;
	switch (type) {
	case 0:
		grab = &m_grabVideo;
		break;
	case 1:
		grab = &m_grabOsd;
		break;
	case 2:
		grab = &m_grabPip;
		break;
	default:
		LOGERROR("videorender: %s: unknown grab requested", __FUNCTION__);
		return NULL;
	}

	LOGDEBUG2(L_GRAB, "videorender: %s: %s size %d %dx%d at %d|%d %p", __FUNCTION__, type == 0 ? "VIDEO" : (type == 1 ? "OSD" : "PIP"),
		grab->GetSize(), grab->GetWidth(), grab->GetHeight(), grab->GetX(), grab->GetY(), grab->GetData());

	if (size)
		*size = grab->GetSize();
	if (width)
		*width = grab->GetWidth();
	if (height)
		*height = grab->GetHeight();
	if (x)
		*x = grab->GetX();
	if (y)
		*y = grab->GetY();

	return grab;
}

/**
 * Get some rendering statistics
 *
 * @param[out] duped      number of duplicated frames
 * @param[out] dropped    number of dropped frames
 * @param[out] counter    number of decoded frames
 */
void cVideoRender::GetStats(int *duped, int *dropped, int *counter)
{
	*duped = m_framesDuped;
	*dropped = m_framesDropped;
	*counter = m_startCounter;
}

/*****************************************************************************
 * Setup and initialization
 ****************************************************************************/

/**
 * Wrapper to set the screen size in the device
 *
 * @param width           screen width
 * @param height          screen height
 * @param refreshRate     screen refresh rate
 */
void cVideoRender::SetScreenSize(int width, int height, uint32_t refreshRate)
{
	m_pDevice->SetScreenSize(width, height, refreshRate);
}

/**
 * Helper function to read a line from a given file
 *
 * @param[out] buf           pointer to the data
 * @param[out] size          size of the data at buf
 * @param[in] file           the filepointer to be read on
 *
 * @returns the number of characters read
 */
static size_t ReadLineFromFile(char *buf, size_t size, const char * file)
{
	FILE *fd = NULL;
	size_t character;

	fd = fopen(file, "r");
	if (fd == NULL) {
		LOGERROR("videorender: %s: Can't open %s", __FUNCTION__, file);
		return 0;
	}

	character = getline(&buf, &size, fd);

	fclose(fd);

	return character;
}

/**
 * Helper function to find out which platform we are on
 *
 * @returns the hardware quirks of the device
 */
static int ReadHWPlatform(void)
{
	char *txt_buf;
	char *read_ptr;
	size_t bufsize = 128;
	size_t read_size;

	txt_buf = (char *) calloc(bufsize, sizeof(char));
	int hardwareQuirks = 0;

	read_size = ReadLineFromFile(txt_buf, bufsize, "/sys/firmware/devicetree/base/compatible");
	if (!read_size) {
		free((void *)txt_buf);
		return 0;
	}

	read_ptr = txt_buf;
	// be aware: device tree string can contain \x0 bytes, so every C-string function
	// thinks, we already reached the string's terminating null bytes
	// so copy the string into a temporary string without the "\0"
	char *_txt_buf = (char *) calloc(bufsize, sizeof(char));
	char *_read_ptr = _txt_buf;
	for (size_t i = 0; i < bufsize; i++) {
		if (memcmp(read_ptr, "\0", sizeof(char))) {
			memcpy(_read_ptr, read_ptr, sizeof(char));
			_read_ptr++;
		}
		read_ptr++;
	}

	read_ptr = txt_buf;
	LOGDEBUG2(L_DRM, "videorender: %s: found \"%s\", set hardware quirks", __FUNCTION__, _txt_buf);

	while(read_size) {
		if (strstr(read_ptr, "bcm2836")) {
			LOGDEBUG2(L_DRM, "videorender: %s: bcm2836 (Raspberry Pi 2 Model B) found", __FUNCTION__);
			hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "bcm2837")) {
			LOGDEBUG2(L_DRM, "videorender: %s: bcm2837 (Raspberry Pi 2 Model B v1.2/ 3 Model B, Raspberry Pi 3 Compute Module 3) found", __FUNCTION__);
			hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "bcm2711")) {
			LOGDEBUG2(L_DRM, "videorender: %s: bcm2711 (Raspberry Pi 4 Model B, Compute Module 4, Pi 400) found", __FUNCTION__);
			hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "bcm2712")) {
			LOGDEBUG2(L_DRM, "videorender: %s: bcm2712 (Raspberry Pi 5, Compute Module 5, Pi 500) found", __FUNCTION__);
			hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "amlogic")) {
			LOGDEBUG2(L_DRM, "videorender: %s: amlogic found, disable HW deinterlacer", __FUNCTION__);
			hardwareQuirks |= QUIRK_CODEC_NEEDS_EXT_INIT
					   |  QUIRK_CODEC_SKIP_FIRST_FRAMES
					   |  QUIRK_NO_HW_DEINT;
			break;
		}

		read_size -= (strlen(read_ptr) + 1);
		read_ptr = (char *)&read_ptr[(strlen(read_ptr) + 1)];
	}
	free((void *)_txt_buf);
	free((void *)txt_buf);

	return hardwareQuirks;
}

/**
 * Initialize the renderer
 */
void cVideoRender::Init(void)
{
	m_pDisplayThread = new cDisplayThread(this);
	m_pFilterThread = new cFilterThread(this);

	atomic_set(&m_framesFilled, 0);
	m_enqueueBufferIdx = 0;

	if (m_pDrmDevice->Init())
		LOGFATAL("videorender: %s: failed", __FUNCTION__);

	cDrmPlane *videoPlane = m_pDrmDevice->VideoPlane();
	cDrmPlane *osdPlane = m_pDrmDevice->OsdPlane();

	m_hardwareQuirks = ReadHWPlatform();

	// osd FB
#ifndef USE_GLES
	if (!m_pBufOsd)
		m_pBufOsd = new cDrmBuffer();

	if (m_pBufOsd->Setup(m_pDrmDevice->Fd(), m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(), DRM_FORMAT_ARGB8888, NULL)) {
		LOGFATAL("videorender: %s: SetupFB FB OSD failed!", __FUNCTION__);
	}
#else
	if (m_disableOglOsd) {
		if (!m_pBufOsd)
			m_pBufOsd = new cDrmBuffer();

		m_pBufOsd->Setup(m_pDrmDevice->Fd(), m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(), DRM_FORMAT_ARGB8888, NULL);
	}
#endif

	// black fb
	LOGDEBUG2(L_DRM, "videorender: %s: Try to create a black FB", __FUNCTION__);
	m_bufBlack.Setup(m_pDrmDevice->Fd(), m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(), DRM_FORMAT_NV12, NULL);
	m_bufBlack.FillBlack();

	// save actual modesetting
	m_pDrmDevice->SaveCrtc();

	drmModeAtomicReqPtr modeReq;
	const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	uint32_t modeID = 0;

	if (m_pDrmDevice->CreatePropertyBlob(&modeID) != 0)
		LOGFATAL("videorender: %s: Failed to create mode property blob.", __FUNCTION__);
	if (!(modeReq = drmModeAtomicAlloc()))
		LOGFATAL("videorender: %s: cannot allocate atomic request (%d): %m", __FUNCTION__, errno);

	m_pDrmDevice->SetPropertyRequest(modeReq, m_pDrmDevice->CrtcId(),
						DRM_MODE_OBJECT_CRTC, "MODE_ID", modeID);
	m_pDrmDevice->SetPropertyRequest(modeReq, m_pDrmDevice->ConnectorId(),
						DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", m_pDrmDevice->CrtcId());
	m_pDrmDevice->SetPropertyRequest(modeReq, m_pDrmDevice->CrtcId(),
						DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);

	// Osd plane
	// We don't have the m_pBufOsd for OpenGL yet, so we can't set anything. Set src and FbId later when osd was drawn,
	// but initially move the OSD behind the VIDEO
#ifndef USE_GLES
	osdPlane->SetParams(m_pDrmDevice->CrtcId(), m_pBufOsd->Id(),
		0, 0, m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(),
		0, 0, m_pBufOsd->Width(), m_pBufOsd->Height());

	osdPlane->SetPlane(modeReq);
#else
	if (m_disableOglOsd) {
		osdPlane->SetParams(m_pDrmDevice->CrtcId(), m_pBufOsd->Id(),
			0, 0, m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(),
			0, 0, m_pBufOsd->Width(), m_pBufOsd->Height());

		osdPlane->SetPlane(modeReq);
	}
#endif
	if (m_pDrmDevice->UseZpos()) {
		videoPlane->SetZpos(m_pDrmDevice->ZposOverlay());
		videoPlane->SetPlaneZpos(modeReq);
#ifdef USE_GLES
		osdPlane->SetZpos(m_pDrmDevice->ZposPrimary());
		osdPlane->SetPlaneZpos(modeReq);
#endif
	}

	// Black buffer for video plane
	videoPlane->SetParams(m_pDrmDevice->CrtcId(), m_bufBlack.Id(),
		0, 0, m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(),
		0, 0, m_bufBlack.Width(), m_bufBlack.Height());

	videoPlane->SetPlane(modeReq);

	if (drmModeAtomicCommit(m_pDrmDevice->Fd(), modeReq, flags, NULL) != 0) {
#ifndef USE_GLES
		osdPlane->DumpParameters();
#endif
		videoPlane->DumpParameters();

		drmModeAtomicFree(modeReq);
		LOGFATAL("videorender: %s: cannot set atomic mode (%d): %m", __FUNCTION__, errno);
	}

	drmModeAtomicFree(modeReq);

	m_osdShown = false;

	// init variables page flip
	m_pDrmDevice->InitEvent();
}

/**
 * Exit and cleanup the renderer
 */
void cVideoRender::Exit(void)
{
	cDrmPlane *videoPlane = m_pDrmDevice->VideoPlane();
	cDrmPlane *osdPlane = m_pDrmDevice->OsdPlane();

	ExitDisplayThread();

	// restore saved CRTC configuration
	m_pDrmDevice->RestoreCrtc();

	videoPlane->FreeProperties();
	osdPlane->FreeProperties();

	m_bufBlack.Destroy();
#ifdef USE_GLES
	if (m_disableOglOsd) {
		if (m_pBufOsd) {
			m_pBufOsd->Destroy();
			delete m_pBufOsd;
		}
	} else {
		if (m_pNextBo)
			gbm_bo_destroy(m_pNextBo);
		if (m_pOldBo)
			gbm_bo_destroy(m_pOldBo);
	}
#else
	if (m_pBufOsd) {
		m_pBufOsd->Destroy();
		delete m_pBufOsd;
	}
#endif

	m_pDrmDevice->Close();
}

/**
 * Set size and position of the video on the screen
 *
 * @param rect         a cRect, where the video should be rendered in
 */
void cVideoRender::SetVideoOutputPosition(const cRect &rect)
{
	m_videoRect.Set(rect.Point(), rect.Size());

	if (m_videoRect.IsEmpty())
		m_videoIsScaled = false;
	else
		m_videoIsScaled = true;

	LOGDEBUG("videorender: %s: %d %d %d %d%s", __FUNCTION__, rect.X(), rect.Y(), rect.Width(), rect.Height(), m_videoIsScaled ? ", video is scaled" : "");
}

/**
 * Process queued events and forward to event receiver
 */
void cVideoRender::ProcessEvents()
{
	for (Event event : m_eventQueue)
		m_pEventReceiver->OnEventReceived(event);

	m_eventQueue.clear();
}

void cVideoRender::SetPipSize(bool useAlt)
{
	if (useAlt) {
		m_pipScalePercent = m_pConfig->ConfigPipAltScalePercent;
		m_pipLeftPercent = m_pConfig->ConfigPipAltLeftPercent;
		m_pipTopPercent = m_pConfig->ConfigPipAltTopPercent;
	} else {
		m_pipScalePercent = m_pConfig->ConfigPipScalePercent;
		m_pipLeftPercent = m_pConfig->ConfigPipLeftPercent;
		m_pipTopPercent = m_pConfig->ConfigPipTopPercent;
	}
}
