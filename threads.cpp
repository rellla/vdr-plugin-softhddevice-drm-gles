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
#include "videostream.h"
#include "misc.h"

/*****************************************************************************
 * cDecodingThread class
 *
 * This thread decodes the video data
 ****************************************************************************/
cDecodingThread::cDecodingThread(cVideoStream *stream, const char *name) : cThread(name)
{
	m_pStream = stream;
	Start();
}

cDecodingThread::~cDecodingThread(void)
{
}

void cDecodingThread::Action(void)
{
	LOGDEBUG("threads: decoding thread started");
	while(Running()) {
		m_mutex.lock();

		m_pStream->DecodeInput();

		m_mutex.unlock();

		usleep(1000);
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
		m_mutex.lock();

		bool scheduleImmediately = m_pRender->DisplayFrame();

		m_mutex.unlock();

		m_pRender->ProcessEvents();

		if (scheduleImmediately)
			usleep(100); // yield thread. give control also to threads with lower priority.
		else
			usleep(1000);
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
	LOGDEBUG("threads: audio thread started");
	while (Running()) {
		m_pAudio->CyclicCall();
		m_pAudio->ProcessEvents();

		usleep(10000);
	}
	LOGDEBUG("threads: audio thread stopped");
}

void cAudioThread::Stop(void)
{
	if (!Active())
		return;

	LOGDEBUG("threads: stopping audio thread");
	Cancel(2);
}

/*****************************************************************************
 * cFilterThread class
 *
 * This thread handles video filters like deinterlacer or scale filter
 ****************************************************************************/
cFilterThread::cFilterThread(cVideoRender *videoRender, cQueue<cDrmBuffer> *drmBufferQueue, const char *name, std::function<void(AVFrame *)> frameOutput) : cThread(name)
{
	m_pRender = videoRender;
	m_pDrmBufferQueue = drmBufferQueue;
	m_frameOutput = frameOutput;
}

cFilterThread::~cFilterThread(void)
{
}

/**
 * Init and start the video filter thread
 *
 * @param videoCtx               codec context
 * @param frame                  AVFrame to take init parameters from
 * @param enableDeinterlacer     true, if the deinterlacer should be used
 */
void cFilterThread::InitAndStart(const AVCodecContext *videoCtx, AVFrame *frame, bool enableDeinterlacer)
{
	int ret;
	char args[512];
	const char *filterDescr = NULL;
	m_pFilterGraph = avfilter_graph_alloc();
	if (!m_pFilterGraph)
		LOGFATAL("filter thread: %s: Cannot alloc filter graph", __FUNCTION__);

	m_numFramesToFilter = 0;
	m_filterBug = false;

	const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	const AVFilter *buffersink = avfilter_get_by_name("buffersink");

	// interlaced and non-trickspeed AV_PIX_FMT_DRM_PRIME (hardware decoded) -> hardware deinterlacer
	// interlaced and non-trickspeed AV_PIX_FMT_YUV420P (software decoded) -> software deinterlacer
	// progressive and trickspeed AV_PIX_FMT_YUV420P (software decoded) -> scale filter (for NV12 output)
	// progressive and trickspeed AV_PIX_FMT_DRM_PRIME (hardware decoded) doesn't get to the FilterHandlerThread
	if (enableDeinterlacer) {
		if (frame->format == AV_PIX_FMT_DRM_PRIME) {
			filterDescr = "deinterlace_v4l2m2m";
		} else if (frame->format == AV_PIX_FMT_YUV420P) {
			filterDescr = "bwdif=1:-1:0";
			m_filterBug = true;
		}
	} else if (frame->format == AV_PIX_FMT_YUV420P) {
		filterDescr = "scale";
	} else
		LOGFATAL("filter thread: %s: Unexpected pixel format: %d", __FUNCTION__, frame->format);
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
	if (ret < 0)
		LOGFATAL("filter thread: %s: Cannot create buffer source (%d)", __FUNCTION__, ret);

	AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
	memset(par, 0, sizeof(*par));
	par->format = AV_PIX_FMT_NONE;
	par->hw_frames_ctx = frame->hw_frames_ctx;
	ret = av_buffersrc_parameters_set(m_pBuffersrcCtx, par);
	if (ret < 0)
		LOGFATAL("filter thread: %s: Cannot av_buffersrc_parameters_set (%d)", __FUNCTION__, ret);

	av_free(par);

	m_pBuffersinkCtx = avfilter_graph_alloc_filter(m_pFilterGraph, buffersink, "out");
	if (!m_pBuffersinkCtx)
		LOGFATAL("filter thread: %s: Cannot create buffer sink", __FUNCTION__);

	if (frame->format != AV_PIX_FMT_DRM_PRIME) {
		enum AVPixelFormat pixFmts[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };

		ret = av_opt_set_int_list(m_pBuffersinkCtx, "pix_fmts", pixFmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
		if (ret < 0)
			LOGFATAL("filter thread: %s: Cannot set output pixel format (%d)", __FUNCTION__, ret);

		ret = avfilter_init_dict(m_pBuffersinkCtx, NULL);
		if (ret < 0)
			LOGFATAL("filter thread: %s: Cannot initialize buffer sink (%d)", __FUNCTION__, ret);
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
	if (ret < 0) {
		LOGFATAL("filter thread: %s: avfilter_graph_parse_ptr failed (%d)", __FUNCTION__, ret);
	}

	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	ret = avfilter_graph_config(m_pFilterGraph, NULL);
	if (ret < 0)
		LOGFATAL("filter thread: %s: avfilter_graph_config failed (%d)", __FUNCTION__, ret);

	Start();
}

void cFilterThread::Action(void)
{
	LOGDEBUG("threads: video filter thread started");

	while (Running()) {
		if (m_frames.IsEmpty()) {
			usleep(1000);
			continue;
		}

		AVFrame *frame = m_frames.Pop();

		m_numFramesToFilter++;
		if (isInterlacedFrame(frame))
			m_numFramesToFilter++;

		// add frame to filter
		int ret;
		if ((ret = av_buffersrc_add_frame_flags(m_pBuffersrcCtx, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0)
			LOGWARNING("filter thread: %s: can't add_frame: %s", __FUNCTION__, av_err2str(ret));

		av_frame_free(&frame);

		// get filtered frames
		while (Running()) {
			AVFrame *filtFrame = av_frame_alloc();
			if (!filtFrame)
				LOGFATAL("filter thread: %s: can't allocate frame", __FUNCTION__);

			ret = av_buffersink_get_frame(m_pBuffersinkCtx, filtFrame);

			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				av_frame_free(&filtFrame);
				break;
			} else if (ret < 0) {
				LOGERROR("filter thread: %s: can't get filtered frame: %s", __FUNCTION__, av_err2str(ret));
				av_frame_free(&filtFrame);
				break;
			}

			while (Running() && m_pDrmBufferQueue->IsFull())
				usleep(1000);

			if (Running()) {
				if (filtFrame->format == AV_PIX_FMT_NV12 && m_filterBug) // scale filter or sw deinterlacer, no prime data, always returns NV12
					filtFrame->pts /= 2; // ffmpeg bug

				m_frameOutput(filtFrame);
			} else
				av_frame_free(&filtFrame);
		}
	}
	LOGDEBUG("threads: filter thread stopped");
}

/**
 * Put a frame in the buffer to be filtered
 */
void cFilterThread::PushFrame(AVFrame *frame)
{
	m_frames.Push(frame);
}

void cFilterThread::Stop(void)
{
	if (!Active())
		return;

	LOGDEBUG("threads: stopping filter thread");
	Cancel(2);
	m_filterBug = false;
	m_numFramesToFilter = 0;

	while (!m_frames.IsEmpty()) {
		AVFrame *frame = m_frames.Pop();
		av_frame_free(&frame);
	}

	avfilter_graph_free(&m_pFilterGraph);
}
