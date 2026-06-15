// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file audio.cpp
 * Audio Interface
 *
 * cSoftHdAudio handles everything audio related except
 * the decoding itself (see cAudioDecoder) and ALSA output (see cAlsaDevice).
 *
 * @copyright 2009 - 2014 by Johns.  All Rights Reserved.
 * @copyright 2018 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <vdr/thread.h>

#include "alsadevice.h"
#include "audio.h"
#include "audioprocessor.h"
#include "codec_audio.h"
#include "config.h"
#include "event.h"
#include "filllevel.h"
#include "logger.h"
#include "misc.h"
#include "pidcontroller.h"
#include "ringbuffer.h"
#include "softhddevice.h"

/**
 * Create a new audio context
 */
cSoftHdAudio::cSoftHdAudio(cSoftHdDevice *device)
	: cThread("softhd audio"),
	  m_pDevice(device),
	  m_pConfig(m_pDevice->Config()),
	  m_alsa(m_pConfig),
	  m_pEventReceiver(device),
	  m_softVolume(m_pConfig->ConfigAudioSoftvol),
	  m_audioProcessor(BYTES_PER_SAMPLE),
	  m_pMixerChannel(m_pConfig->ConfigAudioMixerChannel)
{
	SetNormalize(m_pConfig->ConfigAudioNormalize, m_pConfig->ConfigAudioMaxNormalize);
	SetCompression(m_pConfig->ConfigAudioCompression, m_pConfig->ConfigAudioMaxCompression);
	SetStereoDescent(m_pConfig->ConfigAudioStereoDescent);
	SetEqualizer(m_pConfig->ConfigAudioEq, m_pConfig->ConfigAudioEqBand);
}

/******************************************************************************
 * Audio Filter
 *****************************************************************************/

/**
 * Put FFmpeg channel layout in a dynamic array of strings
 *
 * @param layout      current FFmpeg channel layout
 *
 * @return ffmpeg layout as an array of strings
 *
 * @ingroup audio
 */
static std::vector<std::string> GetFFmpegChannelLayoutAsArray(const AVChannelLayout &layout)
{
	std::vector<std::string> names;
	char buf[16];

	for (int i = 0; i < layout.nb_channels; i++) {
		enum AVChannel ch = av_channel_layout_channel_from_index(&layout, i);
		int ret = av_channel_name(buf, sizeof(buf), ch);
		if (ret < 0)
			continue;
		names.push_back(std::string(buf));
	}
	return names;
}

/**
 * Check, if FFmpeg and Alsa channel layout match
 *
 * @param ff      Array of FFmpeg channel layout
 * @param alsa    Array of Alsa channel layout
 *
 * @return true if the channel layouts match
 *
 * @ingroup audio
 */
static bool LayoutsMatch(const std::vector<std::string> &ff, const std::vector<std::string> &alsa)
{
	if (ff.size() != alsa.size())
		return false;

	for (size_t i = 0; i < ff.size(); i++) {
		if (ff[i] != alsa[i])
			return false;
	}

	return true;
}

/**
 * Check, if the channel layout has channels named "NA" (N/A, silent)
 *
 * @param channelLayout    channel layout
 *
 * @return true if the channel layout doesn't contain "NA" (N/A, silent)
 *
 * @ingroup audio
 */
static bool LayoutIsValid(const std::vector<std::string> &channelLayout)
{
	return std::find(channelLayout.begin(), channelLayout.end(), "NA") == channelLayout.end();
}

/**
 * Build the "|"-separated mappings list for the channelmap filter
 *
 * @param layout         current FFmpeg channel layout
 *
 * @return mapping string to feed into the channelmap filter
 *
 * @ingroup audio
 */
std::string cSoftHdAudio::BuildChannelMapFilter(const AVChannelLayout &layout)
{
	auto ff = GetFFmpegChannelLayoutAsArray(layout);
	auto alsa = m_alsa.GetChannelLayoutAsArray();

	if (ff.size() != alsa.size()) {
		LOGWARNING("audio: %s: Skip channelmap filter, FFmpeg and Alsa channel count differs: FFmpeg %zu ALSA %zu", __FUNCTION__, ff.size(), alsa.size());
		return "";
	}

	std::string ffString;
	for (size_t i = 0; i < ff.size(); i++) {
		ffString += ff[i];
		if (i < ff.size() - 1)
			ffString += " ";
	}

	std::string alsaString;
	for (size_t i = 0; i < alsa.size(); i++) {
		alsaString += alsa[i];
		if (i < alsa.size() - 1)
			alsaString += " ";
	}

	if (!LayoutIsValid(alsa)) {
		LOGDEBUG2(L_SOUND, "audio: %s: Skip channelmap filter, alsa channel layout isn't valid: %s", __FUNCTION__, alsaString.c_str());
		return "";
	}

	if (LayoutsMatch(ff, alsa)) {
		LOGDEBUG2(L_SOUND, "audio: %s: Skip channelmap filter, FFmpeg and Alsa channel layouts match: %s", __FUNCTION__, ffString.c_str());
		return "";
	}

	std::stringstream ss;
	for (size_t i = 0; i < ff.size(); i++) {
		if (i != 0)
			ss << "|";
		ss << ff[i] << "-" << alsa[i];
	}

	LOGDEBUG2(L_SOUND, "audio: %s: FFmpeg Channel Layout: %s", __FUNCTION__, ffString.c_str());
	LOGDEBUG2(L_SOUND, "audio: %s: Alsa Channel Layout  : %s", __FUNCTION__, alsaString.c_str());
	LOGDEBUG2(L_SOUND, "audio: %s: Layouts don't match, map FFmpeg to Alsa: %s", __FUNCTION__, ss.str().c_str());

	return ss.str();
}

/**
 * Init audio filters
 *
 * The following alsa filters are set:
 *   - abuffer
 *   - channelmap
 *   - superequalizer
 *   - aformat
 *   - abuffersink
 *
 * @retval 0    everything ok
 * @retval 1    didn't support channels, downmix set -> scrap this frame, test next
 * @retval -1   something gone wrong
 */
int cSoftHdAudio::InitFilter(AVCodecContext *audioCtx)
{
	const AVFilter  *abuffer;
	AVFilterContext *pFilterCtx[4];
	const AVFilter *channelmap;
	const AVFilter *eq;
	const AVFilter *aformat;
	const AVFilter *abuffersink;
	char channelLayout[64];
	char optionsStr[1024];
	int err, i, numFilter = 0;

	// Before filter init setup HW parameter
	err = Setup(audioCtx->pkt_timebase, audioCtx->sample_rate, audioCtx->ch_layout.nb_channels, false);
	if (err < 0) {
		LOGERROR("audio: %s: failed!", __FUNCTION__);
		return err;
	}

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

	// channelmap
	//
	// Map FFmpeg channel layout to Alsa channel layout.
	// Depending on the hardware, e.g. FC and LFE have to be swapped.
	// This is the case for HDMI on RPI4, so we need to do the following:
	//    FL-FL|FR-FR|FC-LFE|LFE-FC|BL-BL|BR-BR
	//
	// The channel mapping is skipped, if
	//   - a stereo downmix is forced (downmix will be done later in aformat filter)
	//   - channel count differs, aformat will handle downmix later
	if (!(m_alsa.GetDownmix() && m_alsa.GetHwNumChannels() == 2)) {
		std::string channelMapString;
		channelMapString = BuildChannelMapFilter(audioCtx->ch_layout);

		if (!channelMapString.empty()) {
			if (!(channelmap = avfilter_get_by_name("channelmap"))) {
				LOGWARNING("audio: %s: Could not find the channelmap filter.", __FUNCTION__);
				return -1;
			}
			if (!(pFilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, channelmap, "channelmap"))) {
				LOGWARNING("audio: %s: Could not allocate the channelmap instance.", __FUNCTION__);
				return -1;
			}
			snprintf(optionsStr, sizeof(optionsStr),"map=%s", channelMapString.c_str());
			if (avfilter_init_str(pFilterCtx[numFilter], optionsStr) < 0) {
				LOGWARNING("audio: %s: Could not initialize the channelmap filter \"%s\"", __FUNCTION__, optionsStr);
				avfilter_graph_free(&m_pFilterGraph);
				return -1;
			}
			numFilter++;
		}
	}

	// superequalizer
	if (m_useEqualizer) {
		if (!(eq = avfilter_get_by_name("superequalizer"))) {
			LOGWARNING("audio: %s: Could not find the superequalizer filter.", __FUNCTION__);
			avfilter_graph_free(&m_pFilterGraph);
			return -1;
		}
		if (!(pFilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, eq, "superequalizer"))) {
			LOGWARNING("audio: %s: Could not allocate the superequalizer instance.", __FUNCTION__);
			avfilter_graph_free(&m_pFilterGraph);
			return -1;
		}

		std::string equalizerOptions = m_audioProcessor.GetEqualizerOptions();
		snprintf(optionsStr, sizeof(optionsStr), "%s", equalizerOptions.c_str());

		if (avfilter_init_str(pFilterCtx[numFilter], optionsStr) < 0) {
			LOGWARNING("audio: %s: Could not initialize the superequalizer filter.", __FUNCTION__);
			avfilter_graph_free(&m_pFilterGraph);
			return -1;
		}
		numFilter++;
	}

	// aformat
	AVChannelLayout channel_layout;
	if (m_alsa.GetDownmix() && m_alsa.GetHwNumChannels() == 2) {
		// explicit stereo downmix
		av_channel_layout_default(&channel_layout, 2);
	} else {
		if (av_channel_layout_copy(&channel_layout, &audioCtx->ch_layout) < 0) {
			LOGWARNING("audio: %s: Could not copy channel layout", __FUNCTION__);
			return -1;
		}

		// clamp channels if the hardware doesn't support them
		if (channel_layout.nb_channels > (int)m_alsa.GetHwNumChannels()) {
			LOGDEBUG2(L_SOUND, "audio: %s: clamp channels from %d -> %d", __FUNCTION__, channel_layout.nb_channels, m_alsa.GetHwNumChannels());
			av_channel_layout_uninit(&channel_layout);
			av_channel_layout_default(&channel_layout, m_alsa.GetHwNumChannels());
		}
	}
	av_channel_layout_describe(&channel_layout, channelLayout, sizeof(channelLayout));
	av_channel_layout_uninit(&channel_layout);

	LOGDEBUG2(L_SOUND, "audio: %s: OUT downmix %d hwNumChannels %d hwSampleRate %d channelLayout %s bytes_per_sample %d",
			  __FUNCTION__, m_alsa.GetDownmix(), m_alsa.GetHwNumChannels(), m_alsa.GetHwSampleRate(), channelLayout, av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));

	if (!(aformat = avfilter_get_by_name("aformat"))) {
		LOGWARNING("audio: %s: Could not find the aformat filter.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	if (!(pFilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, aformat, "aformat"))) {
		LOGWARNING("audio: %s: Could not allocate the aformat instance.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	snprintf(optionsStr, sizeof(optionsStr),
		"sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
		av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), m_alsa.GetHwSampleRate(), channelLayout);
	if (avfilter_init_str(pFilterCtx[numFilter], optionsStr) < 0) {
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
	if (!(pFilterCtx[numFilter] = avfilter_graph_alloc_filter(m_pFilterGraph, abuffersink, "sink"))) {
		LOGWARNING("audio: %s: Could not allocate the abuffersink instance.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	if (avfilter_init_str(pFilterCtx[numFilter], NULL) < 0) {
		LOGWARNING("audio: %s: Could not initialize the abuffersink instance.", __FUNCTION__);
		avfilter_graph_free(&m_pFilterGraph);
		return -1;
	}
	numFilter++;

	// Connect the filters
	for (i = 0; i < numFilter; i++) {
		if (i == 0) {
			err = avfilter_link(m_pBuffersrcCtx, 0, pFilterCtx[i], 0);
		} else {
			err = avfilter_link(pFilterCtx[i - 1], 0, pFilterCtx[i], 0);
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

	m_pBuffersinkCtx = pFilterCtx[numFilter - 1];
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

	if (!HasInputPts())
		return;

	int64_t dropMs = std::max((int64_t)0, ptsMs - GetOutputPtsMsInternal());
	int dropFrames = m_alsa.MsToFrames(dropMs);
	int dropBytes = m_alsa.FramesToBytes(dropFrames);

	dropBytes = std::min(dropBytes, (int)m_pRingbuffer.UsedBytes());

	if (dropBytes > 0) {
		LOGDEBUG2(L_AV_SYNC, "audio: %s: dropping %dms audio samples to start in sync with the video (output PTS %s -> %s)",
			__FUNCTION__,
			dropMs,
			Timestamp2String(GetOutputPtsMsInternal(), 1),
			Timestamp2String(ptsMs, 1));

		m_pidController.Reset();
		m_fillLevel.Reset();
		m_fillLevel.WroteFrames(dropFrames);
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

	int byteCount = frame->nb_samples * frame->ch_layout.nb_channels * BYTES_PER_SAMPLE;
	buffer = (uint16_t *)frame->data[0];

	if (m_useCompressor)       // in place operation
		m_audioProcessor.Compress(buffer, byteCount);

	if (m_useNormalizer)       // in place operation
		m_audioProcessor.Normalize(buffer, byteCount);

	Enqueue((uint16_t *)buffer, byteCount, frame->pts);

	av_frame_free(&frame);
}

/**
 * Build a pause spdif burst with the size of the last recognized normal spdif audio
 */
void cSoftHdAudio::BuildPauseBurst(void)
{
	uint16_t *spdif = m_pauseBurst.data();

	constexpr int IEC61937_PREAMBLE1 = 0xF872;
	constexpr int IEC61937_PREAMBLE2 = 0x4E1F;
	constexpr int IEC61937_NULL = 0x00;

	spdif[0] = htole16(IEC61937_PREAMBLE1);
	spdif[1] = htole16(IEC61937_PREAMBLE2);
	spdif[2] = htole16(IEC61937_NULL);
	spdif[3] = 0;

	memset(m_pauseBurst.data() + 4, 0, m_spdifBurstSize - 8);
}

/**
 * Enqueue prepared spdif bursts in audio output queue
 *
 * Wrapper for Enqueue(), but builds a new pause burst if necessary
 *
 * @param buffer     data buffer
 * @param count      number of bytes in data buffer
 * @param pts        pts of the buffer
 */
void cSoftHdAudio::EnqueueSpdif(const uint16_t *buffer, int count, int64_t pts)
{
	std::lock_guard<std::mutex> lock(m_pauseMutex);

	if (count != m_spdifBurstSize) {
		LOGDEBUG2(L_SOUND, "audio: %s: spdif burst size changed %d -> %d, rebuild pause burst", __FUNCTION__, m_spdifBurstSize, count);
		m_spdifBurstSize = count;
		m_pauseBurst.resize(m_spdifBurstSize / 2);

		BuildPauseBurst();
	}

	Enqueue(buffer, count, pts);
}

/**
 * Send audio data to ringbuffer
 *
 * @param buffer     data buffer
 * @param count      number of bytes in data buffer
 * @param pts        pts to set
 */
void cSoftHdAudio::Enqueue(const uint16_t *buffer, int count, int64_t pts)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	// pitch adjustment
	if (!m_alsa.IsPassthroughActive() && m_pitchAdjustFrameCounter == 0 && std::abs(m_pitchPpm) > 1) { // only adjust if pitch has a significant value to prevent overly large values/division by zero
		int oneFrameBytes = m_alsa.FramesToBytes(1);

		if (m_pitchPpm < 0 && m_pRingbuffer.Write((const uint16_t *)buffer, oneFrameBytes)) // insert additional frame
			m_fillLevel.ReceivedFrames(1);
		else if (m_pitchPpm > 0) // drop frame
			count = std::max(0, count - oneFrameBytes);

		m_pitchAdjustFrameCounter = std::round(1'000'000.0 / std::abs(m_pitchPpm));
	}

	m_pitchAdjustFrameCounter = std::max(0, m_pitchAdjustFrameCounter - m_alsa.BytesToFrames(count));

	// write to ringbuffer
	int bytesWritten = m_pRingbuffer.Write((const uint16_t *)buffer, count);
	if (bytesWritten != count)
		LOGERROR("audio: %s: can't place %d samples in ring buffer", __FUNCTION__, count);

	m_fillLevel.ReceivedFrames(m_alsa.BytesToFrames(bytesWritten));

	if (pts != AV_NOPTS_VALUE) {
		// discontinuity check, force a resync if the new pts differs more than AV_SYNC_BORDER_MS to the last
		if (m_inputPts != AV_NOPTS_VALUE && std::abs(m_alsa.PtsToMs(m_inputPts, av_q2d(m_timebase)) - m_alsa.PtsToMs(pts, av_q2d(m_timebase))) > AV_SYNC_BORDER_MS) {
			LOGDEBUG2(L_AV_SYNC, "audio: %s: discontinuity detected in audio PTS %s -> %s%s", __FUNCTION__,
				Timestamp2String(m_alsa.PtsToMs(m_inputPts, av_q2d(m_timebase)), 1), Timestamp2String(m_alsa.PtsToMs(pts, av_q2d(m_timebase)), 1),
				m_alsa.PtsToMs(m_inputPts, av_q2d(m_timebase)) > m_alsa.PtsToMs(pts, av_q2d(m_timebase)) ? " (PTS wrapped)" : "");
			m_eventQueue.push_back(ScheduleResyncAtPtsMsEvent{m_alsa.PtsToMs(pts, av_q2d(m_timebase))});
		}

		m_inputPts = pts;
	} else if (m_inputPts != AV_NOPTS_VALUE) {
		m_inputPts += m_alsa.FramesToPts(m_alsa.BytesToFrames(count), av_q2d(m_timebase));
	}
}

/**
 * Alsa setup wrapper
 *
 * only used for passthrough atm, setting up PCM goes via Filter()
 *
 * @param timebase          codec timebase
 * @param samplerate        stream samplerate
 * @param channels          stream nb of channels
 * @param passthrough       passthrough enabled
 *
 * @retval 0                everything ok
 * @retval -1               something gone wrong in AlsaSetup
 * @retval 1                no parameter change, no setup needed
 */
int cSoftHdAudio::Setup(AVRational timebase, int samplerate, int channels, bool passthrough)
{
	int err = 0;

	m_timebase = timebase;

	// skip setup, nothing changed
	if (samplerate == (int)m_alsa.GetHwSampleRate() &&
	    passthrough == m_alsa.IsPassthroughActive() &&
	   (channels == (int)m_alsa.GetHwNumChannels() || (m_alsa.GetDownmix() && m_alsa.GetHwNumChannels() == 2)))
		return 1;

	if (Active()) {
		Stop();
		DropAlsaBuffers();
	}

	err = m_alsa.Setup(channels, samplerate, passthrough, m_pConfig->ConfigAudioDownmix);
	if (err)
		LOGERROR("audio: %s: failed!", __FUNCTION__);
	else
		Start();

	return err;
}

/**
 * Get frame from filter sink
 *
 * @return       pointer to AVFrame if success, NULL otherwise
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
 * Flush the alsa buffers and reset audio: pts, ringbuffer, pidController, fillLevel
 */
void cSoftHdAudio::FlushBuffers(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);

	if (!m_initialized)
		return;

	if (m_inputPts != AV_NOPTS_VALUE)
		FlushAlsaBuffers();

	m_fillLevel.Reset();
	m_fillLevel.ResetFramesCounters();
	m_pidController.Reset();
	m_pRingbuffer.Reset();
	m_inputPts = AV_NOPTS_VALUE;
	m_filterChanged = 1;
}

/**
 * Get used bytes in audio ringbuffer
 */
int cSoftHdAudio::GetUsedRingbufferBytes(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	return m_pRingbuffer.UsedBytes();
}

/**
 * Get used ms in audio ringbuffer
 */
int cSoftHdAudio::GetUsedRingbufferMs(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	return m_alsa.FramesToMs(m_alsa.BytesToFrames(m_pRingbuffer.UsedBytes()));
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
 * @return     PTS in milliseconds
 */
int64_t cSoftHdAudio::GetOutputPtsMs(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	return GetOutputPtsMsInternal();
}

int64_t cSoftHdAudio::GetOutputPtsMsInternal(void)
{
	return m_alsa.PtsToMs(m_inputPts, av_q2d(m_timebase)) - m_alsa.FramesToMs(m_alsa.BytesToFrames(m_pRingbuffer.UsedBytes()));
}

/**
 * Get the hardware output PTS in milliseconds
 *
 * Calculates the presentation timestamp of audio currently being output by the
 * hardware by accounting for ALSA/kernel buffer delays. This represents the PTS
 * of the audio that is actually being played right now.
 *
 * @return     PTS in milliseconds, or AV_NOPTS_VALUE if not available
 */
int64_t cSoftHdAudio::GetHardwareOutputPtsMs(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_alsa.IsRunning() || m_inputPts == AV_NOPTS_VALUE)
		return AV_NOPTS_VALUE;

	int delayFrames = m_alsa.GetHwDelayFrames();

	// subtract baseline to ignore pause bursts already in the buffer
	delayFrames -= m_hwBaseline;

	return GetOutputPtsMsInternal() - m_alsa.FramesToMs(delayFrames);
}

/**
 * Get the hardware delay in milliseconds
 *
 * @return delay in milliseconds, or AV_NOPTS_VALUE if not available
 */
int64_t cSoftHdAudio::GetHardwareOutputDelayMs(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_alsa.IsRunning() || m_inputPts == AV_NOPTS_VALUE)
		return AV_NOPTS_VALUE;

	int delayFrames = m_alsa.GetHwDelayFrames();

	return m_alsa.FramesToMs(delayFrames);
}

/**
 * Get the hardware output PTS in timebase units
 *
 * @return      presentation timestamp in timebase units
 */
int64_t cSoftHdAudio::GetHardwareOutputPtsTimebaseUnits(void)
{
	int64_t ptsMs = GetHardwareOutputPtsMs();
	if (ptsMs == AV_NOPTS_VALUE)
		return AV_NOPTS_VALUE;

	return m_alsa.MsToPts(ptsMs, av_q2d(m_timebase));
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
	if (m_stereoDescent && m_alsa.GetHwNumChannels() == 2 && !m_alsa.IsPassthroughActive()) {
		volume -= m_stereoDescent;
		if (volume < 0)
			volume = 0;
		else if (volume > 1000)
			volume = 1000;
	}

	m_audioProcessor.SetAmplifier(volume);
	if (!m_softVolume)
		m_alsa.SetVolume(volume);
}

/**
 * Set audio playback pause state
 *
 * @param pause     true to pause, false to resume
 */
void cSoftHdAudio::SetPaused(bool pause)
{
	std::lock_guard<std::mutex> lock(m_pauseMutex);
	LOGDEBUG2(L_SOUND, "audio: %s: %d", __FUNCTION__, pause);

	m_paused = pause;
}

/**
 * Set normalize volume parameters
 *
 * @param enable         true, turn on normalize
 * @param maxfac         max. factor of normalize / 1000
 */
void cSoftHdAudio::SetNormalize(bool enable, int maxfac)
{
	m_useNormalizer = enable;
	m_audioProcessor.SetNormalizer(maxfac);
}

/**
 * Set volume compression parameters
 *
 * @param enable        true, turn on compression
 * @param maxfac        max. factor of compression / 1000
 */
void cSoftHdAudio::SetCompression(bool enable, int maxfac)
{
	m_useCompressor = enable;
	m_audioProcessor.SetCompressor(maxfac);
}

/**
 * Set equalizer bands
 *
 * @param enable    set using equalizer
 * @param band      setting frequenz bands
 */
void cSoftHdAudio::SetEqualizer(bool enable, int band[18])
{
	m_filterChanged = 1;
	m_useEqualizer = enable;
	m_audioProcessor.SetEqualizer(band);
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
 * Initialize audio output module (alsa)
 *
 * The init is done lazily as soon as there is a STOP->PLAY state change
 * or the mediaplayer wants to play video or audio.
 *
 * This function can safely be called anytime, because it does nothing,
 * if the init has already be done.
 */
void cSoftHdAudio::LazyInit()
{
	if (!m_initialized) {
		if (!m_alsa.Init())
			LOGFATAL("audio: could not initialize alsa, abort!");
		m_initialized = true;
	}
}

/**
 * Cleanup audio output module (alsa)
 *
 * This currently also stops the audio thread.
 *
 * @todo Move stopping the thread to AlsaExit()
 */
void cSoftHdAudio::Exit(void)
{
	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);

	Stop();

	if (!m_initialized)
		return;

	avfilter_graph_free(&m_pFilterGraph);
	m_alsa.Exit();
	m_initialized = false;
}

/**
 * Flush alsa buffers
 */
void cSoftHdAudio::FlushAlsaBuffers(void)
{
	m_alsa.FlushBuffers(false);

	m_audioProcessor.ResetCompressor();
	m_audioProcessor.ResetNormalizer();
}

/**
 * Drop alsa buffers
 */
void cSoftHdAudio::DropAlsaBuffers(void)
{
	m_alsa.FlushBuffers(true);

	m_audioProcessor.ResetCompressor();
	m_audioProcessor.ResetNormalizer();
}

/******************************************************************************
 * Thread playback
 *****************************************************************************/

/**
 * Audio thread loop, started with Start().
 * Tries to periodically send frames to the hardware and checks for events (underruns)
 */
void cSoftHdAudio::Action(void)
{
	LOGDEBUG("audio: thread started");
	while (Running()) {
		bool scheduleImmediately = CyclicCall();
		ProcessEvents();

		if (scheduleImmediately)
			usleep(1000);
		else
			usleep(10000);
	}
	LOGDEBUG("audio: thread stopped");
}

/**
 * Stop the thread
 */
void cSoftHdAudio::Stop(void)
{
	if (!Active())
		return;

	LOGDEBUG("audio: stopping thread");
	Cancel(2);
}

/**
 * Cyclic audio playback call
 *
 * Handles audio output to ALSA, writing samples from the ring buffer
 * to the hardware when space is available.
 *
 * If passthrough is enabled, the thread continues sending data (pause bursts) even if audio playback
 * is paused. This prevents, that the AV-Receiver looses the lock and may switch to PCM instead.
 *
 * @return      true if data was written or the next write should be scheduled immediately
 */
bool cSoftHdAudio::CyclicCall(void)
{
	std::lock_guard<std::mutex> lock1(m_pauseMutex);

	// do nothing in paused PCM mode
	if (m_paused && !m_alsa.IsPassthroughActive())
		return false;

	int err = m_alsa.WaitUntilReady();
	if (err < 0) {
		if (m_alsa.HandleError(err))
			m_eventQueue.push_back(BufferUnderrunEvent{AUDIO});
		return false;
	} else if (err == 0) {
		return true;
	}

	std::lock_guard<std::mutex> lock2(m_mutex);

	int freeAlsaBufferFrames = m_alsa.GetAvailableBufferFrames(false);
	if (freeAlsaBufferFrames == -EAGAIN)
		return true; // ?? is this correct?
	else if (freeAlsaBufferFrames < 0) {
		if (m_alsa.HandleError(freeAlsaBufferFrames))
			m_eventQueue.push_back(BufferUnderrunEvent{AUDIO});
		return false;
	}

	if (m_alsa.IsPassthroughActive() && m_paused) {
		// only write, if there is space for a full pause burst
		size_t freeAlsaBufferBytes = m_alsa.FramesToBytes(freeAlsaBufferFrames);
		if ((int)freeAlsaBufferBytes < m_spdifBurstSize)
			return false;

		// send a pause burst to keep the audio stream locked
		return SendPause();
	}

	return SendAudio(freeAlsaBufferFrames);
}


/**
 * Write regular audio data from the ringbuffer to the hardware
 *
 * @param freeAlsaBufferFrames     number of frames that can be written to the hardware
 *
 * @retval true      if data was written or the write should be scheduled again immediately
 * @retval false     if no data was written
 */
bool cSoftHdAudio::SendAudio(int freeAlsaBufferFrames)
{
	int bytesToWrite;
	int freeAlsaBufferBytes = m_alsa.FramesToBytes(freeAlsaBufferFrames);

	// query ringbuffer fill level
	const void *data;
	ssize_t ringBufferFillLevelBytes = m_pRingbuffer.GetReadPointer(&data);

	bytesToWrite = std::min(freeAlsaBufferBytes, (int)ringBufferFillLevelBytes);

	if (bytesToWrite == 0)
		return false;

	// muting pass-through AC-3, can produce disturbance
	if (m_volume == 0 || (m_softVolume && !m_alsa.IsPassthroughActive())) {
		// FIXME: quick&dirty cast
		m_audioProcessor.Amplify((int16_t *) data, bytesToWrite, m_volume);
		// FIXME: if not all are written, we double amplify them
	}

	int framesToWrite = m_alsa.BytesToFrames(bytesToWrite);
	int framesWritten = m_alsa.Write(data, framesToWrite);
	m_fillLevel.WroteFrames(framesWritten);
	m_pRingbuffer.ReadAdvance(m_alsa.FramesToBytes(framesWritten));

	return m_alsa.CheckWrittenFrames(framesWritten, framesToWrite);
}

/**
 * Write pause to passthrough device
 *
 * @return true if a complete burst was written, false otherwise
 */
bool cSoftHdAudio::SendPause(void)
{
	int framesToWrite = m_alsa.BytesToFrames(m_spdifBurstSize);
	int framesWritten = m_alsa.Write(m_pauseBurst.data(), framesToWrite);

	return m_alsa.CheckWrittenFrames(framesWritten, framesToWrite);
}


/**
 * Set the hw delay baseline
 */
void cSoftHdAudio::SetHwDelayBaseline(void)
{
	if (!m_firstRealAudioReceived) {
		m_hwBaseline = 0;

		if (!m_alsa.IsPassthroughActive())
			return;

		m_hwBaseline = m_alsa.GetHwDelayFrames();

		LOGDEBUG2(L_SOUND, "audio: %s: first real audio was sent, hwBaseline %ld frames (%dms)", __FUNCTION__, m_hwBaseline, m_alsa.FramesToMs(m_hwBaseline));
		m_firstRealAudioReceived = true;
	}
}

/**
 * Reset the hw delay baseline
 */
void cSoftHdAudio::ResetHwDelayBaseline(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	LOGDEBUG2(L_SOUND, "audio: %s: reset hw delay baseline to 0", __FUNCTION__);
	m_hwBaseline = 0;
	m_firstRealAudioReceived = false;
}

/**
 * Process queued events and forward them to event receiver
 */
void cSoftHdAudio::ProcessEvents(void)
{
	for (Event event : m_eventQueue)
		m_pEventReceiver->OnEventReceived(event);

	m_eventQueue.clear();
}

/**
 * Calculate clock drift compensation
 *
 * Uses a PID controller to adjust the playback pitch based on the
 * audio buffer fill level. This keeps the buffer level constant
 * and compensates for clock drift between the sender and the audio hardware.
 *
 * Also updates the low-pass filter for the buffer fill level.
 */
void cSoftHdAudio::ClockDriftCompensation(void)
{
	if (m_alsa.IsPassthroughActive())
		return;

	double bufferFillLevelMs = m_alsa.FramesToMsDouble(m_fillLevel.GetBufferFillLevelFramesAvg());
	if (m_fillLevel.IsSettled()) {
		auto now = std::chrono::steady_clock::now();
		std::chrono::duration<double> elapsedSec = now - m_lastPidInvocation;
		m_lastPidInvocation = now;

		m_pitchPpm = m_pidController.Update(bufferFillLevelMs, elapsedSec.count()) * -1;
	} else
		m_pidController.SetTargetValue(bufferFillLevelMs);

	if (m_packetCounter++ % 1000 == 0) {
		LOGDEBUG2(L_SOUND, "audio: %s: buffer fill level: %.1fms (target: %.1fms), clock drift compensating pitch: %.1fppm, PID controller: P=%.2fppm I=%.2fppm D=%.2fppm",
			__FUNCTION__,
			bufferFillLevelMs,
			m_pidController.GetTargetValue(),
			m_pitchPpm.load(),
			m_pidController.GetPTerm(),
			m_pidController.GetITerm(),
			m_pidController.GetDTerm());
	}

	// buffer fill level low pass filter
	int availableFrames = m_alsa.GetAvailableBufferFrames(true);

	if (availableFrames >= 0)
		m_fillLevel.UpdateAvgBufferFillLevel(m_alsa.GetBufferSizeFrames() - availableFrames);
}
