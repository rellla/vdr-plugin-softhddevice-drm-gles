/**
 * @file audio.cpp
 * Audio and alsa module class
 *
 * This file defines cSoftHdAudio , which holds all functions
 * we need to deal with audio, e.g.handling the audio stream
 * and sending it to hardware.
 *
 * @copyright (c) 2009 - 2014 by Johns.  All Rights Reserved.
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

 #include <algorithm>

#include <stdint.h>
#include <math.h>

#include <libintl.h>
#include <alsa/asoundlib.h>

#include <pthread.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include "misc.h"

#include "iatomic.h"

#include "ringbuffer.h"
#include "audio.h"
#include "videorender.h"
#include "codec_audio.h"
#include "videostream.h"

#include "logger.h"
#include "threads.h"

/******************************************************************************
 * cSoftHdAudio class
 *****************************************************************************/

/**
 * cSoftHdAudio constructor
 */
cSoftHdAudio::cSoftHdAudio(cSoftHdDevice *device)
{
	m_pDevice = device;
	m_pEventReceiver = device;
	m_pConfig = m_pDevice->Config();

	m_pPassthroughDevice = m_pConfig->ConfigAudioPassthroughDevice;
	m_pPCMDevice = m_pConfig->ConfigAudioPCMDevice;

	m_pFilterGraph = nullptr;

	m_pAlsaMixer = nullptr;
	m_pMixerDevice = nullptr;
	m_pMixerChannel = m_pConfig->ConfigAudioMixerChannel;
	m_pAlsaMixerElem = nullptr;

	m_compressionFactor = 0;
	m_hwSampleRate = 0;
	m_hwNumChannels = 0;

	m_downmix = m_pConfig->ConfigAudioDownmix;
	m_softVolume = m_pConfig->ConfigAudioSoftvol;
	SetNormalize(m_pConfig->ConfigAudioNormalize, m_pConfig->ConfigAudioMaxNormalize);
	SetCompression(m_pConfig->ConfigAudioCompression, m_pConfig->ConfigAudioMaxCompression);
	SetStereoDescent(m_pConfig->ConfigAudioStereoDescent);
	m_appendAES = m_pConfig->ConfigAudioAutoAES;
	SetEq(m_pConfig->ConfigAudioEqBand, m_pConfig->ConfigAudioEq);
	m_passthrough = 0;
	if (m_pConfig->ConfigAudioPassthroughState)
		m_passthrough = m_pConfig->ConfigAudioPassthroughMask;

	m_alsaCanPause = false;

	m_inputPts = AV_NOPTS_VALUE;
}

/**
 * cSoftHdAudio denstructor
 */
cSoftHdAudio::~cSoftHdAudio(void)
{
}

/******************************************************************************
 * Audio filter and manipulation
 *****************************************************************************/

 /**
 * Reorder audio frame
 *
 * ffmpeg L  R  C   Ls Rs           -> alsa L R  Ls Rs C
 * ffmpeg L  R  C   LFE Ls Rs       -> alsa L R  Ls Rs C  LFE
 * ffmpeg L  R  C   LFE Ls Rs Rl Rr -> alsa L R  Ls Rs C  LFE Rl Rr
 *
 * @param buf[IN,OUT]   sample buffer
 * @param size          size of sample buffer in bytes
 * @param channels      number of channels interleaved in sample buffer
 */
static void ReorderAudioFrame(uint16_t * buf, int size, int channels)
{
	int i;
	int c;
	int ls;
	int rs;
	int lfe;

	switch (channels) {
		case 5:
			size /= 2;
			for (i = 0; i < size; i += 5) {
				c = buf[i + 2];
				ls = buf[i + 3];
				rs = buf[i + 4];
				buf[i + 2] = ls;
				buf[i + 3] = rs;
				buf[i + 4] = c;
			}
			break;
		case 6:
			size /= 2;
			for (i = 0; i < size; i += 6) {
				c = buf[i + 2];
				lfe = buf[i + 3];
//				ls = buf[i + 4];	tested from jsffm
//				rs = buf[i + 5];
//				buf[i + 2] = ls;
//				buf[i + 3] = rs;
				buf[i + 2] = lfe;
				buf[i + 3] = c;
//				buf[i + 4] = c;
//				buf[i + 5] = lfe;
			}
			break;
		case 8:
			size /= 2;
			for (i = 0; i < size; i += 8) {
				c = buf[i + 2];
				lfe = buf[i + 3];
				ls = buf[i + 4];
				rs = buf[i + 5];
				buf[i + 2] = ls;
				buf[i + 3] = rs;
				buf[i + 4] = c;
				buf[i + 5] = lfe;
			}
			break;
	}
}

/**
 * Normalize audio
 *
 * @param samples   sample buffer
 * @param count     number of bytes in sample buffer
 */
void cSoftHdAudio::Normalize(uint16_t *samples, int count)
{
	int i;
	int l;
	int n;
	uint32_t avg;
	int factor;
	uint16_t *data;

	// average samples
	l = count / m_bytesPerSample;
	data = samples;
	do {
		n = l;
		if (m_normalizeCounter + n > m_normalizeSamples) {
			n = m_normalizeSamples - m_normalizeCounter;
		}
		avg = m_normalizeAverage[m_normalizeIndex];
		for (i = 0; i < n; ++i) {
			int t;

			t = data[i];
			avg += (t * t) / m_normalizeSamples;
		}
		m_normalizeAverage[m_normalizeIndex] = avg;
		m_normalizeCounter += n;
		if (m_normalizeCounter >= m_normalizeSamples) {
			if (m_normalizeReady < NORMALIZE_MAX_INDEX) {
				m_normalizeReady++;
			} else {
				avg = 0;
				for (i = 0; i < NORMALIZE_MAX_INDEX; ++i) {
					avg += m_normalizeAverage[i] / NORMALIZE_MAX_INDEX;
				}

			// calculate normalize factor
			if (avg > 0) {
				factor = ((INT16_MAX / 8) * 1000U) / (uint32_t) sqrt(avg);
				// smooth normalize
				m_normalizeFactor = (m_normalizeFactor * 500 + factor * 500) / 1000;
				if (m_normalizeFactor < m_normalizeMinFactor) {
					m_normalizeFactor = m_normalizeMinFactor;
				}
				if (m_normalizeFactor > m_normalizeMaxFactor) {
					m_normalizeFactor = m_normalizeMaxFactor;
				}
			} else {
				factor = 1000;
			}
			LOGDEBUG2(L_SOUND, "audio: %s: avg %8d, fac=%6.3f, norm=%6.3f", __FUNCTION__,
					  avg, factor / 1000.0, m_normalizeFactor / 1000.0);
			}

			m_normalizeIndex = (m_normalizeIndex + 1) % NORMALIZE_MAX_INDEX;
			m_normalizeCounter = 0;
			m_normalizeAverage[m_normalizeIndex] = 0U;
		}
		data += n;
		l -= n;
	} while (l > 0);

	// apply normalize factor
	for (i = 0; i < count / m_bytesPerSample; ++i) {
		int t;

		t = (samples[i] * m_normalizeFactor) / 1000;
		if (t < INT16_MIN) {
			t = INT16_MIN;
		} else if (t > INT16_MAX) {
			t = INT16_MAX;
		}
		samples[i] = t;
	}
}

/**
 * Compress audio
 *
 * @param samples   sample buffer
 * @param count     number of bytes in sample buffer
 */
void cSoftHdAudio::Compress(uint16_t *samples, int count)
{
	int maxSample;
	int i;
	int factor;

	// find loudest sample
	maxSample = 0;
	for (i = 0; i < count / m_bytesPerSample; ++i) {
		int t;

		t = abs(samples[i]);
		if (t > maxSample) {
			maxSample = t;
		}
	}

	// calculate compression factor
	if (maxSample > 0) {
		factor = (INT16_MAX * 1000) / maxSample;
		// smooth compression (FIXME: make configurable?)
		m_compressionFactor = (m_compressionFactor * 950 + factor * 50) / 1000;
		if (m_compressionFactor > factor) {
			m_compressionFactor = factor;	// no clipping
		}
		if (m_compressionFactor > m_compressionMaxFactor) {
			m_compressionFactor = m_compressionMaxFactor;
		}
	} else {
		return;				// silent nothing todo
	}

	LOGDEBUG2(L_SOUND, "audio: %s: max %5d, fac=%6.3f, com=%6.3f", __FUNCTION__, maxSample,
	          factor / 1000.0, m_compressionFactor / 1000.0);

	// apply compression factor
	for (i = 0; i < count / m_bytesPerSample; ++i) {
		int t;

		t = (samples[i] * m_compressionFactor) / 1000;
		if (t < INT16_MIN) {
			t = INT16_MIN;
		} else if (t > INT16_MAX) {
			t = INT16_MAX;
		}
		samples[i] = t;
	}
}

/**
 * Software amplifier
 *
 * @param samples   sample buffer
 * @param count     number of bytes in sample buffer
 *
 * @todo FIXME: this does hard clipping
 */
void cSoftHdAudio::SoftAmplify(int16_t *samples, int count)
{
	int i;

	// silence
	if (m_volume == 0 || !m_amplifier) {
		memset(samples, 0, count);
		return;
	}

	for (i = 0; i < count / m_bytesPerSample; ++i) {
		int t;

		t = (samples[i] * m_amplifier) / 1000;
		if (t < INT16_MIN) {
			t = INT16_MIN;
		} else if (t > INT16_MAX) {
			t = INT16_MAX;
		}
		samples[i] = t;
	}
}

/**
 * Set equalizer bands
 *
 * @param band      setting frequenz bands
 * @param onoff     set using equalizer
 */
void cSoftHdAudio::SetEq(int band[18], int onoff)
{
	int i;
/*
	LOGDEBUG2(L_SOUND, "audio: %s: %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i onoff %d", __FUNCTION__,
	          band[0], band[1], band[2], band[3], band[4], band[5], band[6], band[7],
	          band[8], band[9], band[10], band[11], band[12], band[13], band[14],
	          band[15], band[16], band[17], onoff);
*/
	for (i = 0; i < 18; i++) {
		switch (band[i]) {
			case 1:
				m_equalizerBand[i] = 1.5;
				break;
			case 0:
				m_equalizerBand[i] = 1;
				break;
			case -1:
				m_equalizerBand[i] = 0.95;
				break;
			case -2:
				m_equalizerBand[i] = 0.9;
				break;
			case -3:
				m_equalizerBand[i] = 0.85;
				break;
			case -4:
				m_equalizerBand[i] = 0.8;
				break;
			case -5:
				m_equalizerBand[i] = 0.75;
				break;
			case -6:
				m_equalizerBand[i] = 0.7;
				break;
			case -7:
				m_equalizerBand[i] = 0.65;
				break;
			case -8:
				m_equalizerBand[i] = 0.6;
				break;
			case -9:
				m_equalizerBand[i] = 0.55;
				break;
			case -10:
				m_equalizerBand[i] = 0.5;
				break;
			case -11:
				m_equalizerBand[i] = 0.45;
				break;
			case -12:
				m_equalizerBand[i] = 0.4;
				break;
			case -13:
				m_equalizerBand[i] = 0.35;
				break;
			case -14:
				m_equalizerBand[i] = 0.3;
				break;
			case -15:
				m_equalizerBand[i] = 0.25;
				break;
		}
	}

	m_filterChanged = 1;
	m_useEqualizer = onoff;
}

/**
 * Init filter
 *
 * @retval 0    everything ok
 * @retval 1    didn't support channels, downmix set -> scrap this frame, test next
 * @retval -1   something gone wrong
 */
int cSoftHdAudio::InitFilter(AVCodecContext *audioCtx)
{
	const AVFilter  *abuffer;
	AVFilterContext *pfilterCtx[3];
	const AVFilter *eq;
	const AVFilter *aformat;
	const AVFilter *abuffersink;
	char channelLayout[64];
	char optionsStr[1024];
	int err, i, numFilter = 0;

	// Before filter init set HW parameter.
	if (audioCtx->sample_rate != (int)m_hwSampleRate ||
		(audioCtx->ch_layout.nb_channels != (int)m_hwNumChannels &&
		!(m_downmix && m_hwNumChannels == 2))) {

		err = AlsaSetup(audioCtx->ch_layout.nb_channels, audioCtx->sample_rate, 0);
		if (err)
			return err;
	}

	m_pTimebase = &audioCtx->pkt_timebase;

#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(7,16,100)
	avfilter_register_all();
#endif

	if (!(m_pFilterGraph = avfilter_graph_alloc())) {
		LOGERROR("audio: %s: Unable to create filter graph.", __FUNCTION__);
		return -1;
	}

	// input buffer
	if (!(abuffer = avfilter_get_by_name("abuffer"))) {
		LOGWARNING("audio: %s: Could not find the abuffer filter.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	if (!(m_pBuffersrcCtx = avfilter_graph_alloc_filter(m_pFilterGraph, abuffer, "src"))) {
		LOGWARNING("audio: %s: Could not allocate the m_pBuffersrcCtx instance.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}

	av_channel_layout_describe(&audioCtx->ch_layout, channelLayout, sizeof(channelLayout));

	LOGDEBUG2(L_SOUND, "audio: %s: IN channelLayout %s sample_fmt %s sample_rate %d channels %d", __FUNCTION__,
	          channelLayout, av_get_sample_fmt_name(audioCtx->sample_fmt), audioCtx->sample_rate, audioCtx->ch_layout.nb_channels);

	av_opt_set    (m_pBuffersrcCtx, "channel_layout", channelLayout,                                AV_OPT_SEARCH_CHILDREN);
	av_opt_set    (m_pBuffersrcCtx, "sample_fmt",     av_get_sample_fmt_name(audioCtx->sample_fmt), AV_OPT_SEARCH_CHILDREN);
	av_opt_set_q  (m_pBuffersrcCtx, "time_base",      (AVRational){ 1, audioCtx->sample_rate },     AV_OPT_SEARCH_CHILDREN);
	av_opt_set_int(m_pBuffersrcCtx, "sample_rate",    audioCtx->sample_rate,                        AV_OPT_SEARCH_CHILDREN);
//	av_opt_set_int(m_pBuffersrcCtx, "channel_counts", audioCtx->channels,                           AV_OPT_SEARCH_CHILDREN);

	// initialize the filter with NULL options, set all options above.
	if (avfilter_init_str(m_pBuffersrcCtx, NULL) < 0) {
		LOGWARNING("audio: %s: Could not initialize the abuffer filter.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}

	// superequalizer
	if (m_useEqualizer) {
		if (!(eq = avfilter_get_by_name("superequalizer"))) {
			LOGWARNING("audio: %s: Could not find the superequalizer filter.", __FUNCTION__);
			avfilter_graph_free(&m_pFilterGraph);
			return -1;
		}
		if (!(pfilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, eq, "superequalizer"))) {
			LOGWARNING("audio: %s: Could not allocate the superequalizer instance.", __FUNCTION__);
			avfilter_graph_free(&m_pFilterGraph);
			return -1;
		}
		snprintf(optionsStr, sizeof(optionsStr),"1b=%.2f:2b=%.2f:3b=%.2f:4b=%.2f:5b=%.2f"
			":6b=%.2f:7b=%.2f:8b=%.2f:9b=%.2f:10b=%.2f:11b=%.2f:12b=%.2f:13b=%.2f:14b=%.2f:"
			"15b=%.2f:16b=%.2f:17b=%.2f:18b=%.2f ", m_equalizerBand[0], m_equalizerBand[1],
			m_equalizerBand[2], m_equalizerBand[3], m_equalizerBand[4], m_equalizerBand[5],
			m_equalizerBand[6], m_equalizerBand[7], m_equalizerBand[8], m_equalizerBand[9],
			m_equalizerBand[10], m_equalizerBand[11], m_equalizerBand[12], m_equalizerBand[13],
			m_equalizerBand[14], m_equalizerBand[15], m_equalizerBand[16], m_equalizerBand[17]);
		if (avfilter_init_str(pfilterCtx[numFilter], optionsStr) < 0) {
			LOGWARNING("audio: %s: Could not initialize the superequalizer filter.", __FUNCTION__);
			avfilter_graph_free(&m_pFilterGraph);
			return -1;
		}
		numFilter++;
	}

	// aformat
	AVChannelLayout channel_layout;
	av_channel_layout_default(&channel_layout, m_hwNumChannels);
	av_channel_layout_describe(&channel_layout, channelLayout, sizeof(channelLayout));
	av_channel_layout_uninit(&channel_layout);
	// should use IN layout if more then 2 ch!?
	LOGDEBUG2(L_SOUND, "audio: %s: OUT m_downmix %d m_hwNumChannels %d m_hwSampleRate %d channelLayout %s bytes_per_sample %d",
			  __FUNCTION__, m_downmix, m_hwNumChannels, m_hwSampleRate, channelLayout, av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));
	if (!(aformat = avfilter_get_by_name("aformat"))) {
		LOGWARNING("audio: %s: Could not find the aformat filter.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	if (!(pfilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, aformat, "aformat"))) {
		LOGWARNING("audio: %s: Could not allocate the aformat instance.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	snprintf(optionsStr, sizeof(optionsStr),
		"sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
		av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), m_hwSampleRate, channelLayout);
	if (avfilter_init_str(pfilterCtx[numFilter], optionsStr) < 0) {
		LOGWARNING("audio: %s: Could not initialize the aformat filter.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	numFilter++;

	// abuffersink
	if (!(abuffersink = avfilter_get_by_name("abuffersink"))) {
		LOGWARNING("audio: %s: Could not find the abuffersink filter.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	if (!(pfilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, abuffersink, "sink"))) {
		LOGWARNING("audio: %s: Could not allocate the abuffersink instance.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	if (avfilter_init_str(pfilterCtx[numFilter], NULL) < 0) {
		LOGWARNING("audio: %s: Could not initialize the abuffersink instance.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	numFilter++;

	// Connect the filters
	for (i = 0; i < numFilter; i++) {
		if (i == 0) {
			err = avfilter_link(m_pBuffersrcCtx, 0, pfilterCtx[i], 0);
		} else {
			err = avfilter_link(pfilterCtx[i - 1], 0, pfilterCtx[i], 0);
		}
	}
	if (err < 0) {
		LOGWARNING("audio: %s: Error connecting audio filters", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}

	// Configure the graph.
	if (avfilter_graph_config(m_pFilterGraph, NULL) < 0) {
		LOGWARNING("audio: %s: Error configuring the audio filter graph", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}

	m_pBuffersinkCtx = pfilterCtx[numFilter - 1];
	m_filterChanged = 0;
	m_filterReady = 1;

	return 0;
}

/******************************************************************************
 * Audio stream handling
 *****************************************************************************/

/**
 * Drop samples older than the given PTS
 *
 * Removes audio samples from the ringbuffer that have a presentation timestamp
 * older than the specified ptsMs.
 *
 * @param ptsMs     presentation timestamp in milliseconds - samples older than this will be dropped
 */
void cSoftHdAudio::DropSamplesOlderThanPtsMs(int64_t ptsMs)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!HasPts())
		return;

	int64_t dropMs = std::max((int64_t)0, ptsMs - GetOutputPtsMsInternal());
	int dropBytes = snd_pcm_frames_to_bytes(m_pAlsaPCMHandle, MsToFrames(dropMs));

	dropBytes = std::min(dropBytes, (int)m_pRingbuffer.UsedBytes());

	if (dropBytes > 0) {
		LOGDEBUG2(L_AV_SYNC, "audio: %s: dropping %dms audio samples to start in sync with the video (output PTS %s -> %s)",
			__FUNCTION__,
			dropMs,
			Timestamp2String(GetOutputPtsMsInternal(), 1),
			Timestamp2String(ptsMs, 1));

		m_pRingbuffer.ReadAdvance(dropBytes);
	}
}

/**
 * Place samples in audio output queue
 *
 * @param frame		audio frame
 */
void cSoftHdAudio::EnqueueFrame(AVFrame *frame)
{
	if (!frame)
		return;

	uint16_t *buffer;

	int byteCount = frame->nb_samples * frame->ch_layout.nb_channels * m_bytesPerSample;
	buffer = (uint16_t *)frame->data[0];

	if (m_compression) {		// in place operation
		Compress(buffer, byteCount);
	}
	if (m_normalize) {			// in place operation
		Normalize(buffer, byteCount);
	}
	ReorderAudioFrame(buffer, byteCount, frame->ch_layout.nb_channels);

	Enqueue((uint16_t *)buffer, byteCount, frame);

	av_frame_free(&frame);
}

/**
 * Send audio data to ringbuffer
 *
 * @param buffer     data buffer
 * @param count      number of bytes in data buffer
 * @param frame      decoded frame (used to get frame parameters)
 */
void cSoftHdAudio::Enqueue(uint16_t *buffer, int count, AVFrame *frame)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	size_t n = m_pRingbuffer.Write((const uint16_t *)buffer, count);
	if (n != (size_t) count)
		LOGERROR("audio: %s: can't place %d samples in ring buffer", __FUNCTION__, count);

	if (frame->pts != AV_NOPTS_VALUE)
		m_inputPts = frame->pts;
}

/**
 * Setup alsa
 *
 * only used for passthrough atm, setting up PCM goes via Filter()
 *
 * @param AudioCtx          AVCodec audio decoding context
 * @param samplerate        stream samplerate
 * @param channels          stream nb of channels
 * @param passthrough       passthrough enabled
 *
 * @retval 0                everything ok
 * @retval err              something gone wrong
 */
int cSoftHdAudio::Setup(AVCodecContext *ctx, int samplerate, int channels, int passthrough)
{
	int err = 0;

	if (samplerate != (int)m_hwSampleRate ||
	   (channels != (int)m_hwNumChannels && !(m_downmix && m_hwNumChannels == 2))) {

		err = AlsaSetup(channels, samplerate, passthrough);
		if (err) {
			LOGERROR("audio: %s: failed!", __FUNCTION__);
			return err;
		}
	}
	m_pTimebase = &ctx->pkt_timebase;

	return 0;
}

/**
 * Get frame from filter sink
 *
 * @returns       pointer to AVFrame if success, NULL otherwise
 */
AVFrame *cSoftHdAudio::FilterGetFrame(void)
{
	AVFrame *outframe = nullptr;
	outframe = av_frame_alloc();
	if (!outframe) {
		LOGERROR("audio: %s: Error allocating frame", __FUNCTION__);
		return NULL;
	}

	int err = av_buffersink_get_frame(m_pBuffersinkCtx, outframe);

	if (err == AVERROR(EAGAIN)) {
//		LOGERROR("audio: %s: Error filtering AVERROR(EAGAIN)", __FUNCTION__);
		av_frame_free(&outframe);
	} else if (err == AVERROR_EOF) {
		LOGERROR("audio: %s: Error filtering AVERROR_EOF", __FUNCTION__);
		av_frame_free(&outframe);
	} else if (err < 0) {
		LOGERROR("audio: %s: Error filtering the data", __FUNCTION__);
		av_frame_free(&outframe);
	}

	return outframe;
}

/**
 * Check if the filter has changed and is ready, init the filter if needed
 *
 * @param ctx       AVCodec audio decoding context
 *
 * @retval 1        error, init failed
 * @retval 0        filter initiated
 */
int cSoftHdAudio::CheckForFilterReady(AVCodecContext *ctx)
{
	if (m_filterReady && m_filterChanged) {
//		LOGDEBUG2(L_SOUND, "audio: %s: m_filterReady %d sink_links_count %d channels %d nb_filters %d nb_outputs %d channels %d m_filterChanged %d",
//			__FUNCTION__, m_filterReady,
//			m_pFilterGraph->sink_links_count, m_pFilterGraph->sink_links[0]->channels,
//			m_pFilterGraph->filters[m_pFilterGraph->nb_filters - 1]->nb_outputs,
//			m_pFilterGraph->nb_filters, m_pFilterGraph->filters[m_pFilterGraph->nb_filters - 1]->outputs[m_pFilterGraph->filters[m_pFilterGraph->nb_filters - 1]->nb_outputs - 1]->channels,
//			m_filterChanged);
		avfilter_graph_free(&m_pFilterGraph);
		m_filterReady = 0;
		LOGDEBUG2(L_SOUND, "audio: %s: Free the filter graph.", __FUNCTION__);
	}

	if (!m_filterReady) {
		if (InitFilter(ctx)) {
			LOGDEBUG2(L_SOUND, "audio: %s: AudioFilterReady failed!", __FUNCTION__);
			return 1;
		}
	}

	return 0;
}

/**
 * Send audio frame to filter and enqueue it
 *
 * @param inframe   incoming audio frame to be filtered
 * @param ctx       AVCodec audio decoding context
 *
 * @retval 1        error, send again
 * @retval 0        running
 */
void cSoftHdAudio::Filter(AVFrame *inframe, AVCodecContext *ctx)
{
	AVFrame *outframe = NULL;
	int err = -1;
	int err_count = 0;

	if (inframe) {
		while (err < 0) {
			if (CheckForFilterReady(ctx)) {
				av_frame_unref(inframe);
				return;
			}

			err = av_buffersrc_add_frame(m_pBuffersrcCtx, inframe);
			if (err < 0) {
				if (err_count) {
					char errbuf[128];
					av_strerror(err, errbuf, sizeof(errbuf));
					LOGERROR("audio: %s: Error submitting the frame to the filter fmt %s channels %d %s", __FUNCTION__,
						av_get_sample_fmt_name(ctx->sample_fmt), ctx->ch_layout.nb_channels, errbuf);
					av_frame_unref(inframe);
					return;
				} else {
					m_filterChanged = 1;
					err_count++;
					LOGDEBUG2(L_SOUND, "audio: %s: m_filterChanged %d  err_count %d", __FUNCTION__, m_filterChanged, err_count);
				}
			}
		}
	}

//	if (!inframe)
//		LOGDEBUG2(L_SOUND, "audio: %s: NO inframe!", __FUNCTION__);

	outframe = FilterGetFrame();
	EnqueueFrame(outframe);
}

/**
 * Flush audio buffers
 *
 * Stop alsa player if running,
 * otherwise flush the alsa buffers and force a filter init
 */
void cSoftHdAudio::FlushBuffers(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);

	if (!m_initialized)
		return;

	if (m_inputPts != AV_NOPTS_VALUE)
		FlushAlsaBuffers();

	m_pRingbuffer.Reset();
	m_inputPts = AV_NOPTS_VALUE;
	m_filterChanged = 1;
}

/**
 * Get free bytes in audio ringbuffer
 */
int cSoftHdAudio::GetFreeBytes(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	return m_pRingbuffer.FreeBytes();
}

/**
 * Get used bytes in audio ringbuffer
 */
int cSoftHdAudio::GetUsedBytes(void)
{
	// FIXME: not correct, if multiple buffer are in use
	return m_pRingbuffer.UsedBytes();
}

/**
 * Get the output PTS of the ringbuffer
 *
 * Calculates the presentation timestamp of the next audio sample that will be
 * output from the ringbuffer. This is the input PTS minus the duration of audio
 * currently buffered in the ringbuffer.
 *
 * Note: This does not account for ALSA/kernel buffer delays. For the actual
 * hardware output PTS, use GetHardwareOutputPtsMs() instead.
 *
 * @return PTS in milliseconds
 */
int64_t cSoftHdAudio::GetOutputPtsMs(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	return GetOutputPtsMsInternal();
}

int64_t cSoftHdAudio::GetOutputPtsMsInternal(void)
{
	return PtsToMs(m_inputPts) - FramesToMs(snd_pcm_bytes_to_frames(m_pAlsaPCMHandle, m_pRingbuffer.UsedBytes()));
}

/**
 * Get the hardware output PTS in milliseconds
 *
 * Calculates the presentation timestamp of audio currently being output by the
 * hardware by accounting for ALSA/kernel buffer delays. This represents the PTS
 * of the audio that is actually being played right now.
 *
 * @return PTS in milliseconds, or AV_NOPTS_VALUE if not available
 */
int64_t cSoftHdAudio::GetHardwareOutputPtsMs(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_hwSampleRate || !m_pAlsaPCMHandle || m_inputPts == AV_NOPTS_VALUE)
		return AV_NOPTS_VALUE;

	snd_pcm_sframes_t delayFrames;
	if (snd_pcm_delay(m_pAlsaPCMHandle, &delayFrames) < 0)
		delayFrames = 0L;

	if (delayFrames < 0) {
		LOGDEBUG2(L_SOUND, "audio: %s: delay < 0", __FUNCTION__);
		delayFrames = 0L;
	}

	return GetOutputPtsMsInternal() - FramesToMs(delayFrames);
}

/**
 * Get the hardware output PTS in timebase units
 *
 * @return presentation timestamp in timebase units
 */
int64_t cSoftHdAudio::GetHardwareOutputPtsTimebaseUnits(void) {
	int64_t ptsMs = GetHardwareOutputPtsMs();
	if (ptsMs == AV_NOPTS_VALUE)
		return AV_NOPTS_VALUE;

	return MsToPts(ptsMs);
}

/**
 * Set mixer volume (0-1000)
 *
 * @param volume    volume (0 .. 1000)
 */
void cSoftHdAudio::SetVolume(int volume)
{
	m_volume = volume;
	// reduce loudness for stereo output
	if (m_stereoDescent && m_hwNumChannels == 2 && !m_passthrough) {
		volume -= m_stereoDescent;
		if (volume < 0) {
			volume = 0;
		} else if (volume > 1000) {
			volume = 1000;
		}
	}
	m_amplifier = volume;
	if (!m_softVolume) {
		AlsaSetVolume(volume);
	}
}

/**
 * Set audio playback paused state
 *
 * @param pause     true to pause, false to resume
 */
void cSoftHdAudio::SetPaused(bool pause)
{
	LOGDEBUG2(L_SOUND, "audio: %s: %d", __FUNCTION__, pause);

	std::lock_guard<std::mutex> lock(m_mutex);

	m_paused = pause;

	if (m_alsaCanPause) {
		snd_pcm_state_t expectedState;
		if (pause)
			expectedState = SND_PCM_STATE_RUNNING;
		else
			expectedState = SND_PCM_STATE_PAUSED;

		if (snd_pcm_state(m_pAlsaPCMHandle) == expectedState) {
			LOGDEBUG2(L_SOUND, "audio: %s: state running, try snd_pcm_pause(1)!", __FUNCTION__);

			int ret = snd_pcm_pause(m_pAlsaPCMHandle, pause);
			if (ret)
				LOGERROR("audio: %s: snd_pcm_pause(): %s", __FUNCTION__, snd_strerror(ret));
		}
	}
}

/**
 * Set normalize volume parameters
 *
 * @param enable         true, turn on normalize
 * @param maxfac         max. factor of normalize / 1000
 */
void cSoftHdAudio::SetNormalize(bool enable, int maxfac)
{
	m_normalize = enable;
	m_normalizeMaxFactor = maxfac;
}

/**
 * Set volume compression parameters
 *
 * @param enable        true, turn on compression
 * @param maxfac        max. factor of compression / 1000
 */
void cSoftHdAudio::SetCompression(bool enable, int maxfac)
{
	m_compression = enable;

	m_compressionMaxFactor = maxfac;
	if (!m_compressionFactor) {
		m_compressionFactor = 1000;
	}
	if (m_compressionFactor > m_compressionMaxFactor) {
		m_compressionFactor = m_compressionMaxFactor;
	}
}

/**
 * Set stereo loudness descent
 *
 * @param delta     value (/1000) to reduce stereo volume
 */
void cSoftHdAudio::SetStereoDescent(int delta)
{
	m_stereoDescent = delta;
	SetVolume(m_volume);	// update channel delta
}

/**
 * Set audio passthrough mask
 *
 * @param mask    passthrough mask (as a bitmask)
 */
void cSoftHdAudio::SetPassthrough(int mask)
{
	m_passthrough = mask;
}

/**
 * Initialize audio output module
 *
 */
void cSoftHdAudio::LazyInit()
{
	if (!m_initialized) {
		AlsaInit();
		m_initialized = true;
	}
}

/**
 * Cleanup audio output module
 */
void cSoftHdAudio::Exit(void)
{
	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);

	if (m_initialized) {
		if (m_pAudioThread->Active())
			m_pAudioThread->Stop();
		delete m_pAudioThread;

		avfilter_graph_free(&m_pFilterGraph);

		AlsaExit();
	}
	m_initialized = false;
}

/******************************************************************************
 * A L S A
 *****************************************************************************/

/**
 * xrun recovery
 */
void cSoftHdAudio::XrunRecovery(void)
{
	int err;
	snd_pcm_state_t state;

	err = snd_pcm_prepare(m_pAlsaPCMHandle);
	if (err < 0) {
		state = snd_pcm_state(m_pAlsaPCMHandle);
		LOGERROR("audio: %s: Can't recovery from xrun: %s pcm state: %s", __FUNCTION__,
			snd_strerror(err), snd_pcm_state_name(state));
	}
}

/**
 * Flush alsa buffers
 */
void cSoftHdAudio::FlushAlsaBuffers(void)
{
	int err;
	snd_pcm_state_t state;

	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);

	state = snd_pcm_state(m_pAlsaPCMHandle);
	if (state != SND_PCM_STATE_OPEN) {
		if ((err = snd_pcm_drop(m_pAlsaPCMHandle)) < 0)
			LOGERROR("audio: %s: snd_pcm_drop(): %s", __FUNCTION__, snd_strerror(err));
		// alsa crash, when in open state here ?
		if ((err = snd_pcm_prepare(m_pAlsaPCMHandle)) < 0)
			LOGERROR("audio: %s: snd_pcm_prepare(): %s", __FUNCTION__, snd_strerror(err));
		state = snd_pcm_state(m_pAlsaPCMHandle);
		LOGDEBUG2(L_SOUND, "audio: %s: pcm state %s", __FUNCTION__, snd_pcm_state_name(state));
	}

	m_compressionFactor = 2000;
	if (m_compressionFactor > m_compressionMaxFactor)
		m_compressionFactor = m_compressionMaxFactor;

	m_normalizeCounter = 0;
	m_normalizeReady = 0;

	for (int i = 0; i < NORMALIZE_MAX_INDEX; ++i)
		m_normalizeAverage[i] = 0U;

	m_normalizeFactor = 1000;
}

/******************************************************************************
 * Thread playback
 *****************************************************************************/

/**
 * Cyclic audio playback call
 *
 * Handles audio output to ALSA, writing samples from the ring buffer
 * to the hardware when space is available.
 *
 * @return true if data was written, false otherwise
 */
bool cSoftHdAudio::CyclicCall()
{
	if (m_paused)
		return false;

	// wait for space in kernel buffers
	int ret = snd_pcm_wait(m_pAlsaPCMHandle, 150);
	if (ret < 0) {
		ret = snd_pcm_recover(m_pAlsaPCMHandle, ret, 0);
		return false;
	} else if (ret == 0) {
		LOGERROR("audio: %s: snd_pcm_wait() timeout", __FUNCTION__);
		return false;
	}

	std::lock_guard<std::mutex> lock(m_mutex);

	// check if paused while waiting for kernel buffer space
	if (m_paused)
		return false;

	// query available space in alsa buffer
	int freeAlsaBufferFrameCount = snd_pcm_avail(m_pAlsaPCMHandle);
	if (freeAlsaBufferFrameCount < 0) {
		if (freeAlsaBufferFrameCount == -EAGAIN)
			return false;

		if (snd_pcm_recover(m_pAlsaPCMHandle, freeAlsaBufferFrameCount, 0) < 0)
			LOGERROR("audio: %s: failed to recover from snd_pcm_avail: %s", __FUNCTION__, snd_strerror(freeAlsaBufferFrameCount));

		LOGERROR("audio: %s: snd_pcm_avail(): %s", __FUNCTION__, snd_strerror(freeAlsaBufferFrameCount));
		return false;
	}

	// calculcate amount of data to write
	const void *data;
	ssize_t inputBufferFillLevelBytes = m_pRingbuffer.GetReadPointer(&data);

	if (inputBufferFillLevelBytes == 0)
		m_eventQueue.push_back(BufferUnderrunEvent{AUDIO});

	int bytesToWrite = std::min(snd_pcm_frames_to_bytes(m_pAlsaPCMHandle, freeAlsaBufferFrameCount), inputBufferFillLevelBytes);

	if (bytesToWrite == 0)
		return false;

	// muting pass-through AC-3, can produce disturbance
	if (m_volume == 0 || (m_softVolume && !m_passthrough)) {
		// FIXME: quick&dirty cast
		SoftAmplify((int16_t *) data, bytesToWrite);
		// FIXME: if not all are written, we double amplify them
	}

	int framesToWrite = snd_pcm_bytes_to_frames(m_pAlsaPCMHandle, bytesToWrite);

	int framesWritten;
	if (m_alsaUseMmap)
		framesWritten = snd_pcm_mmap_writei(m_pAlsaPCMHandle, data, framesToWrite);
	else
		framesWritten = snd_pcm_writei(m_pAlsaPCMHandle, data, framesToWrite);

	m_pRingbuffer.ReadAdvance(snd_pcm_frames_to_bytes(m_pAlsaPCMHandle, framesWritten));

	if (framesWritten != framesToWrite) {
		if (framesWritten < 0) {
			if (framesWritten == -EAGAIN)
				return false;

			LOGWARNING("audio: %s: writei failed: %s", __FUNCTION__, snd_strerror(framesWritten));

			if (snd_pcm_recover(m_pAlsaPCMHandle, framesWritten, 0) < 0)
				LOGERROR("audio: %s: failed to recover from writei: %s", __FUNCTION__, snd_strerror(framesWritten));

			return false;
		} else {
			LOGWARNING("audio: %s: not all frames written", __FUNCTION__);
			return false;
		}
	}

	return true;
}

/**
 * Open alsa device
 *
 * @param device             alsa device
 * @param passthrough        set, if this is a passthrough device
 *
 * @returns	the alsa device if successful, NULL otherwise
 */
char *cSoftHdAudio::OpenAlsaDevice(const char *device, int passthrough)
{
	int err;
	char tmp[80];

	if (!device)
		return NULL;

	LOGDEBUG2(L_SOUND, "audio: %s: try opening %sdevice '%s'", __FUNCTION__, passthrough ? "pass-through " : "", device);

	if (passthrough && m_appendAES) {
		if (!(strchr(device, ':'))) {
			sprintf(tmp, "%s:AES0=%d,AES1=%d,AES2=0,AES3=%d",
				device,
				IEC958_AES0_NONAUDIO | IEC958_AES0_PRO_EMPHASIS_NONE,
				IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
				IEC958_AES3_CON_FS_48000);
		} else {
			sprintf(tmp, "%s,AES0=%d,AES1=%d,AES2=0,AES3=%d",
				device,
				IEC958_AES0_NONAUDIO | IEC958_AES0_PRO_EMPHASIS_NONE,
				IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
				IEC958_AES3_CON_FS_48000);
		}
		LOGDEBUG2(L_SOUND, "audio: %s: auto append AES: %s -> %s", __FUNCTION__, device, tmp);
	} else {
		sprintf(tmp, "%s", device);
	}

	// open none blocking; if device is already used, we don't want wait
	if ((err = snd_pcm_open(&m_pAlsaPCMHandle, tmp, SND_PCM_STREAM_PLAYBACK,
		SND_PCM_NONBLOCK)) < 0) {

		LOGWARNING("audio: %s: could not open device '%s' error: %s", __FUNCTION__, device, snd_strerror(err));
		return NULL;
	}

	LOGDEBUG2(L_SOUND, "audio: %s: opened %sdevice '%s'", __FUNCTION__, passthrough ? "pass-through " : "", device);

	return (char *)device;
}

/**
 * Find alsa device giving some search hints
 *
 * @param devname          interface identification (e.g. "pcm")
 * @param hint             string to compare with device name hints
 * @param passthrough      set, if we want a passthrough device
 *
 * @returns	an opened alsa device name if successful, NULL otherwise
 *              NOTE: Returned string is allocated and must be freed by caller
 */
char *cSoftHdAudio::FindAlsaDevice(const char *devname, const char *hint, int passthrough)
{
	char **hints;
	int err;
	char **n;
	char *name;

	err = snd_device_name_hint(-1, devname, (void ***)&hints);
	if (err != 0) {
		LOGWARNING("audio: %s: Cannot get device names for %s!", __FUNCTION__, hint);
		return NULL;
	}

	n = hints;
	while (*n != NULL) {
		name = snd_device_name_get_hint(*n, "NAME");

		if (name && strstr(name, hint)) {
			if (OpenAlsaDevice(name, passthrough)) {
				snd_device_name_free_hint((void **)hints);
				return name;
			}
		}

		if (name)
			free(name);
		n++;
	}

	snd_device_name_free_hint((void **)hints);
	return NULL;
}

/**
 * Search for an alsa pcm device and open it
 */
void cSoftHdAudio::AlsaInitPCMDevice(void)
{
	char *device = NULL;
	bool freeDevice = false;  // track if device needs to be freed
	int err;
	LOGDEBUG2(L_SOUND, "audio: %s: passthrough %d", __FUNCTION__, m_passthrough);

	// try user set device
	if (m_passthrough)
		device = OpenAlsaDevice(m_pPassthroughDevice, m_passthrough);

	if (!device && m_passthrough)
		device = OpenAlsaDevice(getenv("ALSA_PASSTHROUGH_DEVICE"), m_passthrough);

	if (!device)
		device = OpenAlsaDevice(m_pPCMDevice, m_passthrough);

	if (!device)
		device = OpenAlsaDevice(getenv("ALSA_DEVICE"), m_passthrough);

	// walkthrough hdmi: devices
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try hdmi: devices...", __FUNCTION__);
		device = FindAlsaDevice("pcm", "hdmi:", m_passthrough);
		freeDevice = (device != NULL);  // FindAlsaDevice allocates memory
	}

	// walkthrough default: devices
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try default: devices...", __FUNCTION__);
		device = FindAlsaDevice("pcm", "default:", m_passthrough);
		freeDevice = (device != NULL);  // FindAlsaDevice allocates memory
	}

	// try default device
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try default device...", __FUNCTION__);
		device = OpenAlsaDevice("default", m_passthrough);
	}

	// use null device
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try null device...", __FUNCTION__);
		device = OpenAlsaDevice("null", m_passthrough);
	}

	if (!device)
		LOGFATAL("audio: %s: could not open any device, abort!", __FUNCTION__);

	if (!strcmp(device, "null"))
		LOGWARNING("audio: %s: using %sdevice '%s'", __FUNCTION__,
			m_passthrough ? "pass-through " : "", device);
	else
		LOGINFO("audio: using %sdevice '%s'",
			m_passthrough ? "pass-through " : "", device);

	// Free device string if it was allocated by FindAlsaDevice
	if (freeDevice)
		free(device);

	if ((err = snd_pcm_nonblock(m_pAlsaPCMHandle, 0)) < 0) {
		LOGERROR("audio: %s: can't set block mode: %s", __FUNCTION__, snd_strerror(err));
	}
}

/******************************************************************************
 * Alsa Mixer
 *****************************************************************************/

/**
 * Initialize alsa mixer
 */
void cSoftHdAudio::AlsaInitMixer(void)
{
	const char *device;
	const char *channel;
	snd_mixer_t *alsaMixer;
	snd_mixer_elem_t *alsaMixerElem;
	long alsaMixerElemMin;
	long alsaMixerElemMax;

	if (!(device = m_pMixerDevice)) {
		if (!(device = getenv("ALSA_MIXER"))) {
			device = "default";
		}
	}
	if (!(channel = m_pMixerChannel)) {
		if (!(channel = getenv("ALSA_MIXER_CHANNEL"))) {
			channel = "PCM";
		}
	}
	LOGDEBUG2(L_SOUND, "audio: %s: mixer %s - %s open", __FUNCTION__, device, channel);
	snd_mixer_open(&alsaMixer, 0);
	if (alsaMixer && snd_mixer_attach(alsaMixer, device) >= 0
		&& snd_mixer_selem_register(alsaMixer, NULL, NULL) >= 0
		&& snd_mixer_load(alsaMixer) >= 0) {

		const char *const alsaMixerElem_name = channel;

		alsaMixerElem = snd_mixer_first_elem(alsaMixer);
		while (alsaMixerElem) {
			const char *name;

			name = snd_mixer_selem_get_name(alsaMixerElem);
			if (!strcasecmp(name, alsaMixerElem_name)) {
				snd_mixer_selem_get_playback_volume_range(alsaMixerElem, &alsaMixerElemMin, &alsaMixerElemMax);
				m_alsaRatio = 1000 * (alsaMixerElemMax - alsaMixerElemMin);
				LOGDEBUG2(L_SOUND, "audio: %s: %s mixer found %ld - %ld ratio %d", __FUNCTION__, channel, alsaMixerElemMin, alsaMixerElemMax, m_alsaRatio);
				break;
			}

			alsaMixerElem = snd_mixer_elem_next(alsaMixerElem);
		}

		m_pAlsaMixer = alsaMixer;
		m_pAlsaMixerElem = alsaMixerElem;
	} else {
		LOGERROR("audio: %s: can't open mixer '%s'", __FUNCTION__, device);
	}
}

/**
 * Set alsa mixer volume (0-1000)
 *
 * @param volume      volume (0 .. 1000)
 */
void cSoftHdAudio::AlsaSetVolume(int volume)
{
	int v;
	if (m_pAlsaMixer && m_pAlsaMixerElem) {
		v = (volume * m_alsaRatio) / (1000 * 1000);
		snd_mixer_selem_set_playback_volume(m_pAlsaMixerElem, SND_MIXER_SCHN_FRONT_LEFT, v);
		snd_mixer_selem_set_playback_volume(m_pAlsaMixerElem, SND_MIXER_SCHN_FRONT_RIGHT, v);
	}
}

/**
 * Setup alsa audio for requested format
 *
 * @param channels      Channels requested
 * @param sample_rate   SampleRate requested
 * @param passthrough   use pass-through (AC-3, ...) device
 *
 * @retval 0            everything ok
 * @retval 1            didn't support hw channels, downmix set -> retest
 * @retval -1           something gone wrong
 *
 * @todo FIXME: remove pointer for freq + channels
 */
int cSoftHdAudio::AlsaSetup(int channels, int sample_rate, int passthrough)
{
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_state_t state;
	int err;
	unsigned bufferTimeUs = 100'000;

	if (m_pAudioThread) {
		m_pAudioThread->Stop();
		delete m_pAudioThread;

		FlushAlsaBuffers();
	}

	m_downmix = 0;

	state = snd_pcm_state(m_pAlsaPCMHandle);
	if (state == SND_PCM_STATE_XRUN) {
		LOGERROR("audio: %s: recover from xrun pcm state: %s", __FUNCTION__, snd_pcm_state_name(state));
		XrunRecovery();
	}

	snd_pcm_hw_params_alloca(&hwparams);
	if ((err = snd_pcm_hw_params_any(m_pAlsaPCMHandle, hwparams)) < 0) {
		LOGERROR("audio: %s: Read HW config failed! %s", __FUNCTION__, snd_strerror(err));
		return -1;
	}

	if (!snd_pcm_hw_params_test_access(m_pAlsaPCMHandle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED)) {
		m_alsaUseMmap = true;
	}

	m_hwSampleRate = sample_rate;
	if ((err = snd_pcm_hw_params_set_rate_near(m_pAlsaPCMHandle, hwparams, &m_hwSampleRate, 0) < 0)) {
		LOGERROR("audio: %s: SampleRate %d not supported! %s", __FUNCTION__, sample_rate, snd_strerror(err));
		return -1;
	}
	if ((int)m_hwSampleRate != sample_rate) {
		LOGDEBUG2(L_SOUND, "audio: %s: sample_rate %d m_hwSampleRate %d", __FUNCTION__, sample_rate, m_hwSampleRate);
	}

	m_hwNumChannels = channels;
	if ((err = snd_pcm_hw_params_set_channels_near(m_pAlsaPCMHandle, hwparams, &m_hwNumChannels)) < 0) {
		LOGWARNING("audio: %s: %d channels not supported! %s", __FUNCTION__, m_hwNumChannels, snd_strerror(err));
	}
	if ((int)m_hwNumChannels != channels && !passthrough) {
		m_downmix = 1;
	}

	if ((err = snd_pcm_hw_params_set_buffer_time_near(m_pAlsaPCMHandle, hwparams, &bufferTimeUs, NULL)) < 0) {
		LOGWARNING("audio: %s: bufferTime %d not supported! %s", __FUNCTION__, bufferTimeUs, snd_strerror(err));
	}

	m_alsaCanPause = snd_pcm_hw_params_can_pause(hwparams);

/*	err = snd_pcm_hw_params_test_format(m_pAlsaPCMHandle, hwparams, SND_PCM_FORMAT_S16);
	if (err < 0)	// err == 0 if is supported
		LOGERROR("audio: %s: SND_PCM_FORMAT_S16 not supported! %s", __FUNCTION__,
			snd_strerror(err));
*/
	if ((err = snd_pcm_set_params(m_pAlsaPCMHandle, SND_PCM_FORMAT_S16,
		m_alsaUseMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED :
		SND_PCM_ACCESS_RW_INTERLEAVED, m_hwNumChannels, m_hwSampleRate, 1, bufferTimeUs))) {

		state = snd_pcm_state(m_pAlsaPCMHandle);
		LOGERROR("audio: %s: set params error: %s\n"
			"           Channels %d SampleRate %d\n"
			"           HWChannels %d HWSampleRate %d SampleFormat %s\n"
			"           Supports pause: %s mmap: %s\n"
			"           AlsaBufferTime %dms pcm state: %s",
			__FUNCTION__,
			snd_strerror(err), channels, sample_rate, m_hwNumChannels,
			m_hwSampleRate, snd_pcm_format_name(SND_PCM_FORMAT_S16),
			m_alsaCanPause ? "yes" : "no", m_alsaUseMmap ? "yes" : "no",
			bufferTimeUs / 1000, snd_pcm_state_name(state));
		return -1;
	}

	LOGINFO("audio: alsa set up:\n"
		"           Channels %d SampleRate %d%s\n"
		"           HWChannels %d HWSampleRate %d SampleFormat %s\n"
		"           Supports pause: %s mmap: %s\n"
		"           AlsaBufferTime %dms",
		channels, sample_rate, passthrough ? " -> passthrough" : "",
		m_hwNumChannels, m_hwSampleRate,
		snd_pcm_format_name(SND_PCM_FORMAT_S16),
		m_alsaCanPause ? "yes" : "no", m_alsaUseMmap ? "yes" : "no",
		bufferTimeUs / 1000);

	m_pAudioThread = new cAudioThread(this);

	return 0;
}

/**
 * Empty log callback
 */
static void AlsaNoopCallback( __attribute__ ((unused))
	const char *file, __attribute__ ((unused))
	int line, __attribute__ ((unused))
	const char *function, __attribute__ ((unused))
	int err, __attribute__ ((unused))
	const char *fmt, ...)
{
}

/**
 * @brief	Initialize alsa audio output module.
 */
void cSoftHdAudio::AlsaInit(void)
{
#ifdef ALSA_DEBUG
	(void)AlsaNoopCallback;
#else
	// disable display of alsa error messages
	snd_lib_error_set_handler(AlsaNoopCallback);
#endif

	AlsaInitPCMDevice();
	AlsaInitMixer();
}

/**
 * Cleanup alsa audio output module.
 */
void cSoftHdAudio::AlsaExit(void)
{
	if (m_pAlsaPCMHandle) {
		snd_pcm_close(m_pAlsaPCMHandle);
		m_pAlsaPCMHandle = NULL;
	}
	if (m_pAlsaMixer) {
		snd_mixer_close(m_pAlsaMixer);
		m_pAlsaMixer = NULL;
		m_pAlsaMixerElem = NULL;
	}
}

/**
 * Process queued events and forward to event receiver
 */
void cSoftHdAudio::ProcessEvents()
{
	for (Event event : m_eventQueue)
		m_pEventReceiver->OnEventReceived(event);

	m_eventQueue.clear();
}
