/**
 * @file threads.cpp
 * Thread classes
 *
 * This file defines all thread classes, which are
 *   - cDecodingThread
 *   - cDisplayThread
 *   - cAudioThread
 *   - cFilterThread
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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}

#include "logger.h"
#include "vdr/thread.h"
#include "threads.h"
#include "videorender.h"
#include "audio.h"

/*****************************************************************************
 * cDecodingThread class
 *
 * This thread decodes the video data
 ****************************************************************************/
cDecodingThread::cDecodingThread(cSoftHdDevice *device) : cThread("softhd decoding")
{
	m_pDevice = device;
	Start();
}

cDecodingThread::~cDecodingThread(void)
{
}

void cDecodingThread::Action(void)
{
	LOGDEBUG("threads: decoding thread started");
	while(Running()) {
		if (m_pDevice->VideoStream()->DecodeInput()) {
			usleep(10000);
		}
	}
	LOGDEBUG("threads: decoding thread stopped");
}

void cDecodingThread::Stop(void)
{
	if (!Active())
		return;

	LOGDEBUG("threads: stopping decoding thread");
	Cancel(2);
}

/*****************************************************************************
 * cDisplayThread class
 *
 * This thread is responsible for displaying the video and osd
 ****************************************************************************/
cDisplayThread::cDisplayThread(cVideoRender *render) : cThread("softhd display")
{
	m_pRender = render;
	Start();
}

cDisplayThread::~cDisplayThread(void)
{
}

void cDisplayThread::Action(void)
{
	LOGDEBUG("threads: display thread started");
	while(Running()) {
		int ret = m_pRender->DisplayFrame();

		if (ret)
			usleep(1000);
		else if (m_pRender->DrmHandleEvent() != 0)
			LOGERROR("threads: display thread: drmHandleEvent failed!");

		if (m_pRender->ShouldClose() || m_pRender->ShouldFlush())
			m_pRender->CleanUp();

	}
	LOGDEBUG("threads: display thread stopped");
}

void cDisplayThread::Stop(void)
{
	if (!Active())
		return;

	LOGDEBUG("threads: stopping display thread");
	Cancel(2);
}

/*****************************************************************************
 * cAudioThread class
 *
 * This thread is decodes the audio data and moves it to hardware
 ****************************************************************************/
cAudioThread::cAudioThread(cSoftHdAudio *audio) : cThread("softhd audio")
{
	m_pAudio = audio;
	Start();
}

cAudioThread::~cAudioThread(void)
{
}

void cAudioThread::Action(void)
{
	m_mutex.Lock();
	LOGDEBUG("threads: audio thread started");
	while (Running()) {
		if (!m_pAudio->IsPaused()) {
			m_pAudio->FlushAlsaBuffers();
			m_pAudio->ResetCompressor();
			m_pAudio->ResetNormalizer();
		}

		m_pAudio->SetRunning(0);
		m_pAudio->StartAlsaPlayer();

		// wait for sync start, if audio isn't running
		if (!m_pAudio->IsRunning()) {
			LOGDEBUG2(L_SOUND, "audio thread: wait on start condition");
			m_startWait.Wait(m_mutex);
		}

		LOGDEBUG("audio thread: start condition signalled");
		while(Running()) {
			if (!m_pAudio->AlsaPlayerRunning())
				break;

			if (m_pAudio->IsPaused()) {
				usleep(10000);
				continue;
			}

			if (m_pAudio->GetUsedBytes()) {
				// try to play some samples
				m_pAudio->PlayWithAlsa();
			} else {
//				LOGDEBUG2(L_SOUND, "audio thread: ring buffer is empty");
				usleep(5000);
			}
		}
	}
	LOGDEBUG("threads: audio thread stopped");
}

void cAudioThread::Stop(void)
{
	m_pAudio->SetRunning(1);
	m_pAudio->StopAlsaPlayer();
	m_startWait.Broadcast();

	if (!Active())
		return;

	LOGDEBUG("threads: stopping audio thread");
	Cancel(2);
}

void cAudioThread::SendStartSignal(void)
{
	m_startWait.Broadcast();
}

/*****************************************************************************
 * cFilterThread class
 *
 * This thread handles video filters like deinterlacer or scale filter
 ****************************************************************************/
cFilterThread::cFilterThread(cVideoRender *render) : cThread("softhd filter")
{
	m_pRender = render;
}

cFilterThread::~cFilterThread(void)
{
}

/**
 * Init the video filter
 *
 * @param videoCtx      codec context
 * @param frame         AVFrame to take init parameters from
 * @param disabled      true, if deinterlacer is disabled
 *
 * @returns 0           on success
 * @return 1            on failure, filter was not initiated
 */
int cFilterThread::Init(const AVCodecContext *videoCtx, AVFrame *frame, int disabled)
{
	int ret;
	char args[512];
	const char *filterDescr = NULL;
	m_pFilterGraph = avfilter_graph_alloc();
	if (!m_pFilterGraph) {
		LOGERROR("filter thread: %s: Cannot alloc filter graph", __FUNCTION__);
		return -1;
	}

	m_pRender->ClearFramesToFilter();
	m_filterBug = false;
	m_filterTrick = false;
	m_filterStill = false;
	m_isInterlaceFilter = false;

	const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	const AVFilter *buffersink = avfilter_get_by_name("buffersink");

	bool interlaced = m_pRender->IsInterlacedFrame(frame);
	if (videoCtx->framerate.num > 0) {
		if (videoCtx->framerate.num / videoCtx->framerate.den > 30)
			interlaced = false;
		else
			interlaced = true;
	}

	if (videoCtx->codec_id == AV_CODEC_ID_HEVC)
		interlaced = false;

	if (disabled) {
		if (interlaced)
			LOGDEBUG2(L_CODEC, "filter thread: %s: Deinterlacer wanted, but disabled in setup!", __FUNCTION__);
		interlaced = false;
	}

	// interlaced and non-trickspeed AV_PIX_FMT_DRM_PRIME (hardware decoded) -> hardware deinterlacer
	// interlaced and non-trickspeed AV_PIX_FMT_YUV420P (software decoded) -> software deinterlacer
	// progressive and trickspeed AV_PIX_FMT_YUV420P (software decoded) -> scale filter (for NV12 output)
	// progressive and trickspeed AV_PIX_FMT_DRM_PRIME (hardware decoded) doesn't get to the FilterHandlerThread
	if (interlaced && !(m_pRender->IsTrickspeedFrame(frame) || m_pRender->IsStillpictureFrame(frame))) {
		if (frame->format == AV_PIX_FMT_DRM_PRIME) {
			filterDescr = "deinterlace_v4l2m2m";
			m_isInterlaceFilter = true;
		} else if (frame->format == AV_PIX_FMT_YUV420P) {
			filterDescr = "bwdif=1:-1:0";
			m_filterBug = true;
			m_isInterlaceFilter = true;
		}
	} else if (frame->format == AV_PIX_FMT_YUV420P) {
		filterDescr = "scale";
		if (m_pRender->IsTrickspeedFrame(frame))
			m_filterTrick = true;
		if (m_pRender->IsStillpictureFrame(frame))
			m_filterStill = true;
	}
#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(7,16,100)
	avfilter_register_all();
#endif

	// if we have a 576i stream without a valid sample_aspect_ratio (0/1) force it to be 64/45
	// wich "stretches" a 576i stream to 1920/1080 size
	int sarNum = videoCtx->sample_aspect_ratio.num != 0 ? videoCtx->sample_aspect_ratio.num : (videoCtx->height == 576 ? 64 : 1);
	int sarDen = videoCtx->sample_aspect_ratio.num != 0 ? videoCtx->sample_aspect_ratio.den : (videoCtx->height == 576 ? 45 : 1);

	snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		videoCtx->width, videoCtx->height, frame->format,
		videoCtx->pkt_timebase.num ? videoCtx->pkt_timebase.num : 1,
		videoCtx->pkt_timebase.num ? videoCtx->pkt_timebase.den : 1,
		sarNum,
		sarDen);

	LOGDEBUG2(L_CODEC, "filter thread: %s: filter=\"%s\" args=\"%s\"", __FUNCTION__, filterDescr, args);

	ret = avfilter_graph_create_filter(&m_pBuffersrcCtx, buffersrc, "in", args, NULL, m_pFilterGraph);
	if (ret < 0) {
		LOGERROR("filter thread: %s: Cannot create buffer source (%d)", __FUNCTION__, ret);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}

	AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
	memset(par, 0, sizeof(*par));
	par->format = AV_PIX_FMT_NONE;
	par->hw_frames_ctx = frame->hw_frames_ctx;
	ret = av_buffersrc_parameters_set(m_pBuffersrcCtx, par);
	if (ret < 0) {
		LOGERROR("filter thread: %s: Cannot av_buffersrc_parameters_set (%d)", __FUNCTION__, ret);
		av_free(par);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}

	av_free(par);

	ret = avfilter_graph_create_filter(&m_pBuffersinkCtx, buffersink, "out", NULL, NULL, m_pFilterGraph);
	if (ret < 0) {
		LOGERROR("filter thread: %s: Cannot create buffer sink (%d)", __FUNCTION__, ret);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}

	if (frame->format != AV_PIX_FMT_DRM_PRIME) {
		enum AVPixelFormat pixFmts[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };
		ret = av_opt_set_int_list(m_pBuffersinkCtx, "pix_fmts", pixFmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
		if (ret < 0) {
			LOGERROR("filter thread: %s: Cannot set output pixel format (%d)", __FUNCTION__, ret);
			avfilter_graph_free(&m_pFilterGraph);
			return -1;
		}
	}

	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();

	outputs->name       = av_strdup("in");
	outputs->filter_ctx = m_pBuffersrcCtx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	inputs->name       = av_strdup("out");
	inputs->filter_ctx = m_pBuffersinkCtx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	ret = avfilter_graph_parse_ptr(m_pFilterGraph, filterDescr, &inputs, &outputs, NULL);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	if (ret < 0) {
		LOGERROR("filter thread: %s: avfilter_graph_parse_ptr failed (%d)", __FUNCTION__, ret);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}

	ret = avfilter_graph_config(m_pFilterGraph, NULL);
	if (ret < 0) {
		LOGERROR("filter thread: %s: avfilter_graph_config failed (%d)", __FUNCTION__, ret);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}

	return 0;
}

void cFilterThread::Action(void)
{
	AVFrame *frame = 0;
	int ret = 0;
	int enqueued = 0;

	LOGDEBUG("threads: video filter thread started");

	while (Running()) {
		if (m_frames.Empty()) {
			m_waitIdleCondition.Broadcast();
			usleep(10000);
			continue;
		}

		frame = m_frames.Pop();

		if (m_pRender->IsInterlacedFrame(frame)) {
			m_pRender->IncFramesToFilter();
			m_pRender->IncFramesToFilter();
		} else {
			m_pRender->IncFramesToFilter();
		}

		// add frame to filter
		if (av_buffersrc_add_frame_flags(m_pBuffersrcCtx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
			LOGWARNING("filter thread: %s: can't add_frame.", __FUNCTION__);
		else
			av_frame_free(&frame);

		// get filtered frames
		while (Running()) {
			AVFrame *filtFrame = av_frame_alloc();
			ret = av_buffersink_get_frame(m_pBuffersinkCtx, filtFrame);

			if (ret == AVERROR(EAGAIN)) {
				av_frame_free(&filtFrame);
				break;
			} else if (ret == AVERROR_EOF) {
				av_frame_free(&filtFrame);
				break;
			} else if (ret < 0) {
				LOGERROR("filter thread: %s: can't get filtered frame: %s", __FUNCTION__,
				av_err2str(ret));
				av_frame_free(&filtFrame);
				break;
			}

			// set flag of the filtered frame (scale filter and AV_PIX_FMT_YUV420P)
			if (m_filterTrick)
				m_pRender->MarkAsTrickspeedFrame(filtFrame);
			if (m_filterStill)
				m_pRender->MarkAsStillpictureFrame(filtFrame);

			// put frame into display queue
			enqueued = 0;
			while (Running()) {
				m_pRender->FramesRbLock();
				if (m_pRender->GetFramesFilled() < VIDEO_SURFACES_MAX) {
					if (filtFrame->format == AV_PIX_FMT_NV12) {
						// scale filter or sw deinterlacer, no prime data, always returns NV12
						// -> go through EnqueueFB
						if (m_filterBug)
							filtFrame->pts /= 2; // ffmpeg bug
						m_pRender->DecFramesToFilter();
						m_pRender->FramesRbUnlock();
						m_pRender->EnqueueFB(filtFrame);
					} else {
						// hw deinterlacers, we received prime data
						// -> put the frame directly into render Rb
						m_pRender->RbPushFrame(filtFrame);
						m_pRender->DecFramesToFilter();
						m_pRender->FramesRbUnlock();
					}
					enqueued = 1;
					break;
				// render ringbuffer is full, wait until some frames were displayed
				} else {
					m_pRender->FramesRbUnlock();
					usleep(1000);
					continue;
				}
			}
			if (!enqueued)
				av_frame_free(&filtFrame);
		}
	}
	LOGDEBUG("threads: filter thread stopped");
}

/**
 * Get the number of frames in the ringbuffer to be filtered
 */
int cFilterThread::GetBufferFrameCount(void)
{
	return m_frames.Size();
}

/**
 * Put a frame in the ringbuffer to be filtered
 */
bool cFilterThread::PushFrame(AVFrame *frame)
{
	return m_frames.Push(frame);
}

void cFilterThread::Stop(void)
{
	if (!Active())
		return;

	LOGDEBUG("threads: stopping filter thread");
	Cancel(2);
	m_filterBug = false;
	m_filterTrick = false;
	m_filterStill = false;
	m_pRender->ClearFramesToFilter();

	while (!m_frames.Empty()) {
		AVFrame *frame = m_frames.Pop();
		av_frame_free(&frame);
	}

	avfilter_graph_free(&m_pFilterGraph);
}

void cFilterThread::WaitForIdle(void)
{
	if (!Active())
		return;

	cMutex mutex;
	int timeoutInMs = 2000;
	mutex.Lock();
	if (!m_waitIdleCondition.TimedWait(mutex, timeoutInMs))
		LOGERROR("filter thread: %s: timeout (%dms) while waiting for empty filter ringbuffer", __FUNCTION__, timeoutInMs);
}
