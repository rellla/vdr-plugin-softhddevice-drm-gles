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

#include "misc.h"
}

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

	m_pPassthroughDevice = nullptr;
	m_pPCMDevice = nullptr;

	m_pAlsaMixer = nullptr;
	m_pMixerDevice = nullptr;
	m_pMixerChannel = nullptr;
	m_pAlsaMixerElem = nullptr;

	m_compressionFactor = 0;
	m_hwSampleRate = 0;
	m_hwNumChannels = 0;
	m_downmix = false;
	m_paused = false;
	m_muted = false;
	m_running = false;
	m_alsaPlayerRunning = false;
	m_alsaCanPause = false;

	m_pts = AV_NOPTS_VALUE;
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
 * Reset normalize settings
 */
void cSoftHdAudio::ResetNormalizer(void)
{
	int i;

	m_normalizeCounter = 0;
	m_normalizeReady = 0;
	for (i = 0; i < NORMALIZE_MAX_INDEX; ++i) {
		m_normalizeAverage[i] = 0U;
	}
	m_normalizeFactor = 1000;
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
 * Reset compressor
 */
void cSoftHdAudio::ResetCompressor(void)
{
	m_compressionFactor = 2000;
	if (m_compressionFactor > m_compressionMaxFactor) {
		m_compressionFactor = m_compressionMaxFactor;
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
	if (m_muted || !m_amplifier) {
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

	if (!(m_pFilterGraph = avfilter_graph_alloc()))
		LOGERROR("audio: %s: Unable to create filter graph.", __FUNCTION__);

	// input buffer
	if (!(abuffer = avfilter_get_by_name("abuffer")))
		LOGWARNING("audio: %s: Could not find the abuffer filter.", __FUNCTION__);
	if (!(m_pBuffersrcCtx = avfilter_graph_alloc_filter(m_pFilterGraph, abuffer, "src")))
		LOGWARNING("audio: %s: Could not allocate the m_pBuffersrcCtx instance.", __FUNCTION__);

	av_channel_layout_describe(&audioCtx->ch_layout, channelLayout, sizeof(channelLayout));

	LOGDEBUG2(L_SOUND, "audio: %s: IN channelLayout %s sample_fmt %s sample_rate %d channels %d", __FUNCTION__,
	          channelLayout, av_get_sample_fmt_name(audioCtx->sample_fmt), audioCtx->sample_rate, audioCtx->ch_layout.nb_channels);

	av_opt_set    (m_pBuffersrcCtx, "channel_layout", channelLayout,                                AV_OPT_SEARCH_CHILDREN);
	av_opt_set    (m_pBuffersrcCtx, "sample_fmt",     av_get_sample_fmt_name(audioCtx->sample_fmt), AV_OPT_SEARCH_CHILDREN);
	av_opt_set_q  (m_pBuffersrcCtx, "time_base",      (AVRational){ 1, audioCtx->sample_rate },     AV_OPT_SEARCH_CHILDREN);
	av_opt_set_int(m_pBuffersrcCtx, "sample_rate",    audioCtx->sample_rate,                        AV_OPT_SEARCH_CHILDREN);
//	av_opt_set_int(m_pBuffersrcCtx, "channel_counts", audioCtx->channels,                           AV_OPT_SEARCH_CHILDREN);

	// initialize the filter with NULL options, set all options above.
	if (avfilter_init_str(m_pBuffersrcCtx, NULL) < 0)
		LOGWARNING("audio: %s: Could not initialize the abuffer filter.", __FUNCTION__);

	// superequalizer
	if (m_useEqualizer) {
		if (!(eq = avfilter_get_by_name("superequalizer")))
			LOGWARNING("audio: %s: Could not find the superequalizer filter.", __FUNCTION__);
		if (!(pfilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, eq, "superequalizer")))
			LOGWARNING("audio: %s: Could not allocate the superequalizer instance.", __FUNCTION__);
		snprintf(optionsStr, sizeof(optionsStr),"1b=%.2f:2b=%.2f:3b=%.2f:4b=%.2f:5b=%.2f"
			":6b=%.2f:7b=%.2f:8b=%.2f:9b=%.2f:10b=%.2f:11b=%.2f:12b=%.2f:13b=%.2f:14b=%.2f:"
			"15b=%.2f:16b=%.2f:17b=%.2f:18b=%.2f ", m_equalizerBand[0], m_equalizerBand[1],
			m_equalizerBand[2], m_equalizerBand[3], m_equalizerBand[4], m_equalizerBand[5],
			m_equalizerBand[6], m_equalizerBand[7], m_equalizerBand[8], m_equalizerBand[9],
			m_equalizerBand[10], m_equalizerBand[11], m_equalizerBand[12], m_equalizerBand[13],
			m_equalizerBand[14], m_equalizerBand[15], m_equalizerBand[16], m_equalizerBand[17]);
		if (avfilter_init_str(pfilterCtx[numFilter], optionsStr) < 0)
			LOGWARNING("audio: %s: Could not initialize the superequalizer filter.", __FUNCTION__);
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
	if (!(aformat = avfilter_get_by_name("aformat")))
		LOGWARNING("audio: %s: Could not find the aformat filter.", __FUNCTION__);
	if (!(pfilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, aformat, "aformat")))
		LOGWARNING("audio: %s: Could not allocate the aformat instance.", __FUNCTION__);
	snprintf(optionsStr, sizeof(optionsStr),
		"sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
		av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), m_hwSampleRate, channelLayout);
	if (avfilter_init_str(pfilterCtx[numFilter], optionsStr) < 0)
		LOGWARNING("audio: %s: Could not initialize the aformat filter.", __FUNCTION__);
	numFilter++;

	// abuffersink
	if (!(abuffersink = avfilter_get_by_name("abuffersink")))
		LOGWARNING("audio: %s: Could not find the abuffersink filter.", __FUNCTION__);
	if (!(pfilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, abuffersink, "sink")))
		LOGWARNING("audio: %s: Could not allocate the abuffersink instance.", __FUNCTION__);
	if (avfilter_init_str(pfilterCtx[numFilter], NULL) < 0)
		LOGWARNING("audio: %s: Could not initialize the abuffersink instance.", __FUNCTION__);
	numFilter++;

	// Connect the filters
	for (i = 0; i < numFilter; i++) {
		if (i == 0) {
			err = avfilter_link(m_pBuffersrcCtx, 0, pfilterCtx[i], 0);
		} else {
			err = avfilter_link(pfilterCtx[i - 1], 0, pfilterCtx[i], 0);
		}
	}
	if (err < 0)
		LOGWARNING("audio: %s: Error connecting audio filters", __FUNCTION__);

	// Configure the graph.
	if (avfilter_graph_config(m_pFilterGraph, NULL) < 0)
		LOGWARNING("audio: %s: Error configuring the audio filter graph", __FUNCTION__);

	m_pBuffersinkCtx = pfilterCtx[numFilter - 1];
	m_filterChanged = 0;
	m_filterReady = 1;

	return 0;
}

/******************************************************************************
 * Audio ringbuffer
 *****************************************************************************/

/**
 * Setup audio ringbuffer
 */
void cSoftHdAudio::InitRingbuffer(void)
{
	// ~2s 8ch 16bit
	m_pRingbuffer = new cSoftHdRingbuffer(m_ringBufferSize);
}

/**
 * Cleanup audio ringbuffer
 */
void cSoftHdAudio::ExitRingbuffer(void)
{
	if (m_pRingbuffer) {
		delete m_pRingbuffer;
		m_pRingbuffer = nullptr;
	}
	m_hwSampleRate = 0;	// checked for valid setup
}

/******************************************************************************
 * Audio stream handling
 *****************************************************************************/

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

	int count = frame->nb_samples * frame->ch_layout.nb_channels * m_bytesPerSample;
	buffer = (uint16_t *)frame->data[0];

	if (!m_alsaPlayerRunning) {
		av_frame_free(&frame);
//		LOGDEBUG2(L_SOUND, "audio: %s: Alsa player is not running!", __FUNCTION__);
		return;
	}

	if (m_compression) {		// in place operation
		Compress(buffer, count);
	}
	if (m_normalize) {			// in place operation
		Normalize(buffer, count);
	}
	ReorderAudioFrame(buffer, count, frame->ch_layout.nb_channels);

	Enqueue((uint16_t *)buffer, count, frame);
	if (!m_running && !m_paused)		// check, if we can start the thread
		StartAudioThread(frame);

	av_frame_free(&frame);
}

/**
 * Place samples in spdif audio output queue
 *
 * Places data as is in the ringbuffer. Caller has to ensure,
 * that necessary headers have been added to the buffer before (e.g. for passthrough).
 *
 * @param buffer     data buffer
 * @param count      number of bytes in data buffer
 * @param frame      decoded frame (used to get frame parameters), gets unreferenced
 */
void cSoftHdAudio::EnqueueRawData(uint16_t *buffer, int count, AVFrame *frame)
{
	if (!m_alsaPlayerRunning) {
//		LOGDEBUG2(L_SOUND, "audio: %s: Alsa player is not running!", __FUNCTION__);
		return;
	}

	Enqueue(buffer, count, frame);
	if (!m_running && !m_paused)		// check, if we can start the thread
		StartAudioThread(frame);
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
	size_t n;

	m_rbMutex.Lock();
	n = m_pRingbuffer->Write((const uint16_t *)buffer, count);
	if (n != (size_t) count)
		LOGERROR("audio: %s: can't place %d samples in ring buffer", __FUNCTION__, count);

	m_pts = frame->pts + (frame->nb_samples * m_pTimebase->den /
		m_pTimebase->num / frame->sample_rate);
	m_rbMutex.Unlock();
}

/**
 * Start audio thread if possible and skip
 */
void cSoftHdAudio::StartAudioThread(AVFrame *frame)
{
	int skip;
	size_t n;

	n = m_pRingbuffer->UsedBytes();
	skip = m_skip;
	// FIXME: round to packet size

	LOGDEBUG2(L_AV_SYNC, "audio: %s: start? in Rb %4zdms to skip %dms nb_samples %d",
		__FUNCTION__,
		n * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample,
		skip * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample,
		frame->nb_samples);
	if (skip) {
		if (n < (unsigned)skip) {
			skip = n;
		}
		m_skip -= skip;
		m_pRingbuffer->ReadAdvance(skip);
		n = m_pRingbuffer->UsedBytes();
	}
	// forced start or enough video + audio buffered
	// for some exotic channels * 4 too small
	if ((m_videoIsReady && m_startThreshold < n) ||	m_startThreshold * 4 < n) {
		// restart play-back
		// no lock needed, can wakeup next time
		LOGDEBUG2(L_AV_SYNC, "audio: %s: start play-back Threshold %ums RingBuffer %zums m_videoIsReady %d", __FUNCTION__,
			m_startThreshold * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample,
			n * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample,
			m_videoIsReady);
		m_running = true;
		LOGDEBUG2(L_SOUND, "audio: %s: start thread", __FUNCTION__);
		m_pAudioThread->SendStartSignal();
	}
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
 * Check, if we are able to sync audio and video
 *
 * Called before video sync, once we got a video frame.
 * It starts the audio thread, if there is more than m_startThreshold data in the ringbuffer
 * and skips audio frames if necessary.
 *
 * @param videoPts	real video presentation timestamp
 *
 * @retval false       we did not get a valid audio pts to sync, so video thread must wait
 * @retval true        audio was started or is already running
 */
bool cSoftHdAudio::VideoReady(int64_t videoPts)
{
	int64_t audioPts;
	int64_t used;
	int skip;

	if (m_running) {
		LOGDEBUG2(L_SOUND, "audio: %s: Audio is already running?", __FUNCTION__);
		return true;
	}

	// no valid audio known
	if (m_pts == AV_NOPTS_VALUE) {
		LOGDEBUG2(L_SOUND, "audio: %s: can't do a/v start, no valid PTS", __FUNCTION__);
		return false;
	}

	used = m_pRingbuffer->UsedBytes();
	audioPts = m_pts * 1000 * av_q2d(*m_pTimebase) -
	           used * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample;

	skip = videoPts - audioPts - m_pDevice->GetVideoAudioDelay();

	if (skip > 0) {
		skip = (int64_t)skip * m_hwSampleRate * m_hwNumChannels * m_bytesPerSample / 1000;

		//skip must be a multiple of m_hwNumChannels * m_bytesPerSample
		int frames = skip / m_hwNumChannels / m_bytesPerSample;
		skip = frames * m_hwNumChannels * m_bytesPerSample;

		if ((unsigned)skip > used) {
			m_skip = skip - used;
			skip = used;
		}
		LOGDEBUG2(L_AV_SYNC, "audio: %s: RB %" PRId64 "ms skip %dms to skip %dms", __FUNCTION__,
			used * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample,
			skip * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample,
			m_skip * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample);
		m_pRingbuffer->ReadAdvance(skip);

		used = m_pRingbuffer->UsedBytes();
	}

	// enough audio buffered
	if (m_startThreshold < used) {
		m_running = true;
		LOGDEBUG2(L_SOUND, "audio: %s: start thread", __FUNCTION__);
		m_pAudioThread->SendStartSignal();
	}
	m_videoIsReady = true;
	return true;
}

/**
 * Skip Audio if it's behind video
 *
 * @param videoPts   real video presentation timestamp
 * @param full       if true, skip all audio frames
 *                   if false, keep one byte left
 *                   this is a workaround to avoid empty audio ringbuffer
 */
int cSoftHdAudio::Skip(int64_t videoPts, int full)
{
	int64_t audioPts;
	int64_t used;
	int skip;

	// no valid audio pts known
	if (m_pts == AV_NOPTS_VALUE) {
		LOGDEBUG2(L_AV_SYNC, "audio: %s: can't do skip, no valid audio PTS", __FUNCTION__);
		return -1;
	}

	// no valid video pts
	if (videoPts == AV_NOPTS_VALUE) {
		LOGDEBUG2(L_AV_SYNC, "audio: %s: can't do skip, no valid video PTS", __FUNCTION__);
		return -1;
	}

	while (1) {
		used = m_pRingbuffer->UsedBytes(); // in bytes

		if (used * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample == 0)
			break;

		audioPts = m_pts * 1000 * av_q2d(*m_pTimebase) -
		           used * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample;

		skip = videoPts * 1000 * av_q2d(*m_pTimebase) - audioPts - m_pDevice->GetVideoAudioDelay(); // in ms

		if (skip <= 0) // audio >= video
			break;

		skip = (int64_t)skip * m_hwSampleRate * m_hwNumChannels * m_bytesPerSample / 1000;

		//skip must be a multiple of m_hwNumChannels * m_bytesPerSample
		int frames = skip / m_hwNumChannels / m_bytesPerSample;
		skip = frames * m_hwNumChannels * m_bytesPerSample;

		if ((unsigned)skip >= used)
			skip = used - (1 - full) * m_hwNumChannels * m_bytesPerSample;

		LOGDEBUG2(L_AV_SYNC, "audio: %s: RB %" PRId64 "ms skip %dms audio %s -> %s video %s", __FUNCTION__,
			used * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample,
			skip * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample,
			Timestamp2String(audioPts),
			Timestamp2String(audioPts + skip * 1000 / m_hwSampleRate / m_hwNumChannels / m_bytesPerSample),
			Timestamp2String(videoPts * 1000 * av_q2d(*m_pTimebase)));

		m_pRingbuffer->ReadAdvance(skip);
	}

	return 0;
}

/**
 * Flush audio buffers
 *
 * Stop alsa player if running,
 * otherwise flush the alsa buffers and force a filter init
 */
void cSoftHdAudio::FlushBuffers(void)
{
	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);

	if (m_running)
		m_alsaPlayerRunning = false;
	else if (m_pts != AV_NOPTS_VALUE)
		FlushAlsaBuffers();

	while(m_running) {
		usleep(5000);
	}

	m_filterChanged = 1;
}

/**
 * Get free bytes in audio ringbuffer
 */
int cSoftHdAudio::GetFreeBytes(void)
{
	return m_pRingbuffer ? m_pRingbuffer->FreeBytes() : INT32_MAX;
}

/**
 * Get used bytes in audio ringbuffer
 */
int cSoftHdAudio::GetUsedBytes(void)
{
	// FIXME: not correct, if multiple buffer are in use
	return m_pRingbuffer ? m_pRingbuffer->UsedBytes() : 0;
}

/**
 * Get current audio clock
 *
 * This is different from m_pts, which is the pts of the last
 * audio frame enqueued in the ringbuffer.
 * This function returns the pts of the audio frame, which should
 * go out first, e.g the lowest one.
 *
 * @returns the current audio clock in time stamps
 */
int64_t cSoftHdAudio::GetClock(void)
{
	if (!m_running || !m_hwSampleRate ||
		!m_pAlsaPCMHandle || m_pts == AV_NOPTS_VALUE) {

		return AV_NOPTS_VALUE;
	}
	snd_pcm_sframes_t delay;
	int64_t pts;
	int64_t ret;

	m_rbMutex.Lock();
	// delay in frames in alsa + kernel buffers
	if (snd_pcm_delay(m_pAlsaPCMHandle, &delay) < 0) {
		if (!m_paused)
			LOGDEBUG2(L_SOUND, "audio: %s: no hw delay", __FUNCTION__);
		delay = 0L;
	}

	if (delay < 0) {
		LOGDEBUG2(L_SOUND, "audio: %s: delay < 0", __FUNCTION__);
		delay = 0L;
	}

	pts = (int64_t)delay * 1000 / m_hwSampleRate;

	pts += (int64_t)m_pRingbuffer->UsedBytes() * 1000 /
	       m_hwSampleRate / m_hwNumChannels / m_bytesPerSample;

	ret = m_pts * 1000 * av_q2d(*m_pTimebase) - pts;
	m_rbMutex.Unlock();

	return ret;
}

/**
 * Set mixer volume (0-1000)
 *
 * @param volume    volume (0 .. 1000)
 */
void cSoftHdAudio::SetVolume(int volume)
{
	m_volume = volume;
	m_muted = volume == 0;
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
 * Mute audio
 */
void cSoftHdAudio::Mute(void)
{
	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);
	m_muted = true;
}

/**
 * Unmute audio
 */
void cSoftHdAudio::Unmute(void)
{
	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);
	m_muted = false;
}

/**
 * Resume audio
 *
 * Start audio playback after pause
 */
void cSoftHdAudio::Resume(void)
{
	int err;

	if (!m_paused && !m_alsaCanPause) {
		LOGDEBUG2(L_SOUND, "audio: %s: not paused, check the code", __FUNCTION__);
		return;
	}
	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);
	if (m_alsaCanPause) {
		snd_pcm_state_t state;
		state = snd_pcm_state(m_pAlsaPCMHandle);
		if (state == SND_PCM_STATE_PAUSED) {
			LOGDEBUG2(L_SOUND, "audio: %s: state paused, try snd_pcm_pause(0)!", __FUNCTION__);
			if ((err = snd_pcm_pause(m_pAlsaPCMHandle, 0))) {
				LOGERROR("audio: %s: snd_pcm_pause(): %s", __FUNCTION__, snd_strerror(err));
			}
		}
	} else {
		m_paused = false;
		if (m_startThreshold < m_pRingbuffer->UsedBytes()) {
			LOGDEBUG2(L_SOUND, "audio: %s: start thread", __FUNCTION__);
			m_pAudioThread->SendStartSignal();
		}
	}
}

/**
 * Pause audio
 */
void cSoftHdAudio::Pause(void)
{
	int err;

	if (m_paused) {
		LOGDEBUG2(L_SOUND, "audio: %s: already paused, check the code", __FUNCTION__);
		return;
	}

	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);
	if (m_alsaCanPause) {
		snd_pcm_state_t state;
		state = snd_pcm_state(m_pAlsaPCMHandle);
		if (state == SND_PCM_STATE_RUNNING) {
			LOGDEBUG2(L_SOUND, "audio: %s: state running, try snd_pcm_pause(1)!", __FUNCTION__);
			if ((err = snd_pcm_pause(m_pAlsaPCMHandle, 1))) {
				LOGERROR("audio: %s: snd_pcm_pause(): %s", __FUNCTION__, snd_strerror(err));
			}
		}
	} else {
		m_paused = true;
	}
}

/**
 * Set audio buffer time.
 *
 * PES audio packets have a max distance of 300 ms.
 * TS audio packet have a max distance of 100 ms.
 * The period size of the audio buffer is 24 ms.
 * With streamdev sometimes extra +100ms are needed.
 */
void cSoftHdAudio::SetBufferTimeInMs(int delayInMs)
{
	m_bufferTimeInMs = MIN_AUDIO_BUFFER + delayInMs;
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
 * Set pcm audio device.
 *
 * @param device    name of pcm device
 */
void cSoftHdAudio::SetDevice(const char *device)
{
	m_pPCMDevice = device;
}

/**
 * Set pass-through audio device
 *
 * @param device    name of pass-through device
 */
void cSoftHdAudio::SetPassthroughDevice(const char *device)
{
	m_pPassthroughDevice = device;
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
 * Set pcm audio mixer channel
 *
 * @param channel    name of the mixer channel (e.g. PCM or Master)
 */
void cSoftHdAudio::SetChannel(const char *channel)
{
	m_pMixerChannel = channel;
}

/**
 * Initialize audio output module
 *
 */
void cSoftHdAudio::LazyInit()
{
	if (!m_initialized) {
		InitRingbuffer();
		AlsaInit();
		m_pAudioThread = new cAudioThread(this);

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

		AlsaExit();
		ExitRingbuffer();
		m_running = false;
		m_paused = false;
	}
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

	m_pRingbuffer->Reset();
	m_skip = 0;
	m_pts = AV_NOPTS_VALUE;
	m_videoIsReady = false;
}

/******************************************************************************
 * Thread playback
 *****************************************************************************/

/**
 * Play some samples and return
 *
 * PlayWithAlsa is called in the cAudioThread
 *
 * @retval -1        error
 * @retval 1         running
 */
int cSoftHdAudio::PlayWithAlsa(void)
{
	for (;;) {
		int avail;
		int n;
		int err;
		int frames;
		const void *p;

		if (m_paused || !m_alsaPlayerRunning || !m_pAlsaPCMHandle)
			return 1;

		// wait for space in kernel buffers
		if ((err = snd_pcm_wait(m_pAlsaPCMHandle, 150)) < 0) {
//			LOGERROR("audio: %s: snd_pcm_wait error? '%s'", __FUNCTION__, snd_strerror(err));
			err = snd_pcm_recover(m_pAlsaPCMHandle, err, 0);
//			LOGERROR("audio: %s: snd_pcm_wait error: snd_pcm_recover %s", __FUNCTION__, snd_strerror(err));
		}

		if (m_paused || !m_alsaPlayerRunning)
			return 1;

		// how many bytes can be written?
//		n = snd_pcm_avail_update(m_pAlsaPCMHandle);
		n = snd_pcm_avail(m_pAlsaPCMHandle);
		if (n < 0) {
			if (n == -EAGAIN) {
				continue;
			}
			err = snd_pcm_recover(m_pAlsaPCMHandle, n, 0);
			if (err >= 0) {
				continue;
			}
			LOGERROR("audio: %s: snd_pcm_avail_update(): %s", __FUNCTION__, snd_strerror(n));
			return -1;
		}
		avail = snd_pcm_frames_to_bytes(m_pAlsaPCMHandle, n);
		if (avail < 256) {		// too much overhead
			LOGDEBUG2(L_SOUND, "audio: %s: break state '%s' avail %d", __FUNCTION__,
				snd_pcm_state_name(snd_pcm_state(m_pAlsaPCMHandle)), avail);
			break;
		}

		n = m_pRingbuffer->GetReadPointer(&p);
		if (!n) { // ring buffer empty
			LOGWARNING("audio: %s: ring buffer empty Videopkts: %d", __FUNCTION__,
			           m_pDevice->VideoStream()->GetAvPacketsFilled());
		}
		if (n < avail) { // not enough bytes in ring buffer
			avail = n;
		}
		if (!avail) { // full or buffer empty
			break;
		}
		// muting pass-through AC-3, can produce disturbance
		if (m_muted || (m_softVolume && !m_passthrough)) {
			// FIXME: quick&dirty cast
			SoftAmplify((int16_t *) p, avail);
			// FIXME: if not all are written, we double amplify them
		}

		frames = snd_pcm_bytes_to_frames(m_pAlsaPCMHandle, avail);

		m_rbMutex.Lock();
		if (m_alsaUseMmap) {
			err = snd_pcm_mmap_writei(m_pAlsaPCMHandle, p, frames);
		} else {
			err = snd_pcm_writei(m_pAlsaPCMHandle, p, frames);
		}
		m_pRingbuffer->ReadAdvance(avail);
		m_rbMutex.Unlock();
		if (err != frames) {
			if (err < 0) {
				if (err == -EAGAIN) {
					continue;
				}
				LOGWARNING("audio: %s: writei underrun error? '%s'", __FUNCTION__, snd_strerror(err));
				err = snd_pcm_recover(m_pAlsaPCMHandle, err, 0);
				if (err >= 0) {
					continue;
				}
				LOGERROR("audio: %s: snd_pcm_writei failed: %s", __FUNCTION__, snd_strerror(err));
				return -1;
			}
			// this could happen, if underrun happened
			LOGWARNING("audio: %s: not all frames written", __FUNCTION__);
			avail = snd_pcm_frames_to_bytes(m_pAlsaPCMHandle, err);
			break;
		}
	}
	return 0;
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
 * @returns	an opened alsa device if successful, NULL otherwise
 */
char *cSoftHdAudio::FindAlsaDevice(const char *devname, const char *hint, int passthrough)
{
	char **hints;
	int err;
	char **n;
	char *name;
	char *device = NULL;

	err = snd_device_name_hint(-1, devname, (void ***)&hints);
	if (err != 0) {
		LOGWARNING("audio: %s: Cannot get device names for %s!", __FUNCTION__, hint);
		return NULL;
	}

	n = hints;
	while (*n != NULL) {
		name = snd_device_name_get_hint(*n, "NAME");

		if (strstr(name, hint)) {
			if ((device = OpenAlsaDevice(name, passthrough))) {
				device = (char *)malloc(sizeof(char) * (strlen(name) + 1));
				strcpy(device, name);
				free(name);
				snd_device_name_free_hint((void **)hints);
				return (char *)device;
			}
		}

		if (name && strcmp("null", name)) free(name);
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
	}

	// walkthrough default: devices
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try default: devices...", __FUNCTION__);
		device = FindAlsaDevice("pcm", "default:", m_passthrough);
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
	int delay;
	unsigned bufferTime = 100000;	// 100ms

	m_downmix = 0;

	if (m_running) {
		LOGDEBUG2(L_SOUND, "audio: %s: Audio is Running -> FlushBuffers", __FUNCTION__);
		FlushBuffers();
	}

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

	if ((err = snd_pcm_hw_params_set_buffer_time_near(m_pAlsaPCMHandle, hwparams, &bufferTime, NULL)) < 0) {
		LOGWARNING("audio: %s: bufferTime %d not supported! %s", __FUNCTION__, bufferTime, snd_strerror(err));
	}

	m_alsaCanPause = snd_pcm_hw_params_can_pause(hwparams);

/*	err = snd_pcm_hw_params_test_format(m_pAlsaPCMHandle, hwparams, SND_PCM_FORMAT_S16);
	if (err < 0)	// err == 0 if is supported
		LOGERROR("audio: %s: SND_PCM_FORMAT_S16 not supported! %s", __FUNCTION__,
			snd_strerror(err));
*/
	if ((err = snd_pcm_set_params(m_pAlsaPCMHandle, SND_PCM_FORMAT_S16,
		m_alsaUseMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED :
		SND_PCM_ACCESS_RW_INTERLEAVED, m_hwNumChannels, m_hwSampleRate, 1, bufferTime))) {

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
			bufferTime, snd_pcm_state_name(state));
		return -1;
	}

	// update buffer
	m_startThreshold = (bufferTime / 1000) * (m_hwSampleRate / 1000) * m_hwNumChannels * m_bytesPerSample;

	// buffer time/delay in ms
	delay = m_bufferTimeInMs;
	if (m_pDevice->GetVideoAudioDelay() > 0) {
		delay += m_pDevice->GetVideoAudioDelay();
	}
	if (m_startThreshold < (m_hwSampleRate * m_hwNumChannels * m_bytesPerSample * delay) / 1000U) {
		m_startThreshold = (m_hwSampleRate * m_hwNumChannels * m_bytesPerSample * delay) / 1000U;
	}
	// no bigger, than 1/3 the buffer
	if (m_startThreshold > m_ringBufferSize / 3) {
		m_startThreshold = m_ringBufferSize / 3;
	}

	LOGINFO("audio: alsa set up:\n"
		"           Channels %d SampleRate %d%s\n"
		"           HWChannels %d HWSampleRate %d SampleFormat %s\n"
		"           Supports pause: %s mmap: %s\n"
		"           AlsaBufferTime %dms m_bufferTimeInMs %dms Threshold %ums",
		channels, sample_rate, passthrough ? " -> passthrough" : "",
		m_hwNumChannels, m_hwSampleRate,
		snd_pcm_format_name(SND_PCM_FORMAT_S16),
		m_alsaCanPause ? "yes" : "no", m_alsaUseMmap ? "yes" : "no",
		bufferTime / 1000, m_bufferTimeInMs, (m_startThreshold * 1000) /
		(m_hwSampleRate * m_hwNumChannels * m_bytesPerSample));
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

	m_bufferTimeInMs = MIN_AUDIO_BUFFER;
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
