// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file codec_audio.cpp
 * Audio Decoder
 *
 * This file defines cAudioDecoder, which has all the functions
 * to decode audio data. It's the audio interface to ffmpeg.
 *
 * @copyright 2009 - 2015 by Johns.  All Rights Reserved.
 * @copyright 2018 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <cstdint>
#include <mutex>
#include <vector>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "audio.h"
#include "codec_audio.h"
#include "logger.h"
#include "misc.h"

/**
 * Create a new audio decoder for the given audio context
 *
 * @param audio    audio context
 */
cAudioDecoder::cAudioDecoder(cSoftHdAudio *audio)
	: m_pAudio(audio),
	  m_passthroughMask(m_pAudio->GetPassthroughMask() & (CODEC_AC3 | CODEC_EAC3 | CODEC_DTS))
{
	if (!(m_pFrame = av_frame_alloc()))
		LOGFATAL("audiocodec: %s: can't allocate audio decoder frame buffer", __FUNCTION__);

	LOGDEBUG2(L_CODEC, "audiocodec: %s: Set passthrough mask %d", __FUNCTION__, m_passthroughMask);
}

cAudioDecoder::~cAudioDecoder(void)
{
	Close();

	av_frame_free(&m_pFrame);
}

/**
 * Open and initiate the audio decoder
 *
 * @param codecId       audio codec id
 * @param par           audio codec parameters
 * @param timebase      timebase
 *
 * @todo FIXME: errors shouldn't be fatal, maybe just disable audio
 */
void cAudioDecoder::Open(AVCodecID codecId, AVCodecParameters *par, AVRational timebase)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	const AVCodec *codec;

	m_codecId = codecId;

	switch (codecId) {
	case AV_CODEC_ID_AC3:
		if (!(codec = avcodec_find_decoder_by_name("ac3_fixed")))
			LOGFATAL("audiocodec: %s: codec ac3_fixed ID %#06x not found", __FUNCTION__, codecId);
		break;
	case AV_CODEC_ID_AAC:
		if (!(codec = avcodec_find_decoder_by_name("aac_fixed")))
			LOGFATAL("audiocodec: %s: codec aac_fixed ID %#06x not found", __FUNCTION__, codecId);
		break;
	default:
		if (!(codec = avcodec_find_decoder(codecId))) {
			LOGFATAL("audiocodec: %s: codec %s ID %#06x not found", __FUNCTION__,
			avcodec_get_name(codecId), codecId);
		}
		break;
	}

	if (!(m_pAudioCtx = avcodec_alloc_context3(codec)))
		LOGFATAL("audiocodec: %s: can't allocate audio codec context", __FUNCTION__);

	m_pAudioCtx->pkt_timebase = timebase;

	if (par && ((avcodec_parameters_to_context(m_pAudioCtx, par)) < 0))
		LOGERROR("audiocodec: %s: insert parameters to context failed!", __FUNCTION__);

	if (avcodec_open2(m_pAudioCtx, m_pAudioCtx->codec, NULL) < 0)
		LOGFATAL("audiocodec: %s: can't open audio codec", __FUNCTION__);

	LOGDEBUG2(L_CODEC, "audiocodec: %s: Codec %s found, passthrough mask %d", __FUNCTION__, m_pAudioCtx->codec->long_name, m_passthroughMask);

	m_currentSampleRate = 0;
	m_currentHwSampleRate = 0;
	m_currentNumChannels = 0;
	m_currentHwNumChannels = 0;
	m_currentPassthroughMask = 0;
}

/**
 * Close the audio decoder
 */
void cAudioDecoder::Close(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_pAudioCtx)
		return;

	CloseSpdifMuxer();

	LOGDEBUG2(L_CODEC, "audiocodec: %s", __FUNCTION__);

	avcodec_free_context(&m_pAudioCtx);

	m_codecId = AV_CODEC_ID_NONE;
	m_lastPts = AV_NOPTS_VALUE;
}

/**
 * Callback for the spdif muxer
 *
 * This is called, whenever FFmpeg flushes data from the muxer to the AVIOContext.
 * In the current implementation, a flush is forced after every muxer input write.
 */
#if LIBAVFORMAT_VERSION_MAJOR >= 61
int cAudioDecoder::SpdifWriteCallback(void *opaque, const uint8_t *buffer, int bufferSize)
#else
int cAudioDecoder::SpdifWriteCallback(void *opaque, uint8_t *buffer, int bufferSize)
#endif
{
	auto &output = *static_cast<std::vector<uint8_t> *>(opaque);
	output.insert(output.end(), buffer, buffer + bufferSize);

	return bufferSize;
}

/**
 * Open the spdif muxer
 *
 * @param codecId         Codec ID of the stream
 * @param sampleRate      Samplerate of the stream
 *
 * @return                true, if the spdif muxer could be opened
 *                        false on any error
 */
bool cAudioDecoder::OpenSpdifMuxer(AVCodecID codecId, int sampleRate)
{
	CloseSpdifMuxer();

	if (avformat_alloc_output_context2(&m_spdifFmtCtx, nullptr, "spdif", nullptr) < 0 || !m_spdifFmtCtx) {
		LOGERROR("audiocodec: %s: failed to allocate spdif muxer", __FUNCTION__);
		return false;
	}

	m_spdifFmtCtx->pb = avio_alloc_context(m_spdifIoBuffer.data(), m_spdifIoBuffer.size(), 1, &m_spdifOutputBuf, nullptr, &cAudioDecoder::SpdifWriteCallback, nullptr);
	if (!m_spdifFmtCtx->pb) {
		avformat_free_context(m_spdifFmtCtx);
		m_spdifFmtCtx = nullptr;
		return false;
	}

	AVStream *stream = avformat_new_stream(m_spdifFmtCtx, nullptr);
	if (!stream) {
		CloseSpdifMuxer();
		return false;
	}

	stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
	stream->codecpar->codec_id = codecId;
	stream->codecpar->sample_rate = sampleRate;
	av_channel_layout_default(&stream->codecpar->ch_layout, 2);
	stream->time_base = AVRational{1, sampleRate};

	if (avformat_write_header(m_spdifFmtCtx, nullptr) < 0) {
		LOGERROR("audiocodec: %s: failed to write avformat header", __FUNCTION__);
		CloseSpdifMuxer();
		return false;
	}

	LOGDEBUG2(L_CODEC, "audiocodec: %s: opened spdif muxer for %s @ %dHz", __FUNCTION__, avcodec_get_name(codecId), sampleRate);

	return true;
}

/**
 * Close the spdif muxer and free the resources
 */
void cAudioDecoder::CloseSpdifMuxer(void)
{
	if (m_spdifFmtCtx) {
		LOGDEBUG2(L_CODEC, "audiocodec: %s: close spdif muxer", __FUNCTION__);
		if (m_spdifFmtCtx->pb) {
			m_spdifFmtCtx->pb->buffer = nullptr;
			avio_context_free(&m_spdifFmtCtx->pb);
		}
		avformat_free_context(m_spdifFmtCtx);
		m_spdifFmtCtx = nullptr;
	}

	m_spdifOutputBuf.clear();
}

/**
 * Prepend an IEC61937 header to the raw audio data by sending the avpkt to the spdif muxer
 *
 * @param avpkt        input packet
 *
 * @return             output data from the spdif muxer (maybe nullptr, if no output is available)
 */
const std::vector<uint8_t> &cAudioDecoder::BuildIEC61937(const AVPacket *avpkt)
{
	if (!m_spdifFmtCtx || !avpkt || !avpkt->data || avpkt->size <= 0) {
		m_spdifOutputBuf.clear();
		return m_spdifOutputBuf;
	}

	m_spdifOutputBuf.clear();

	AVPacket pkt = {};
	int ret = av_packet_ref(&pkt, avpkt);
	if (ret < 0) {
		LOGERROR("audiocodec: %s: av_packet_ref failed: %s", __FUNCTION__, av_err2str(ret));
		return m_spdifOutputBuf;
	}

	pkt.stream_index = 0;

	ret = av_write_frame(m_spdifFmtCtx, &pkt);
	av_packet_unref(&pkt);
	if (ret < 0) {
		LOGERROR("audiocodec: %s: av_write_frame failed: %s", __FUNCTION__, av_err2str(ret));
		m_spdifOutputBuf.clear();
		return m_spdifOutputBuf;
	}

	// always flush to see, if new output is ready
	avio_flush(m_spdifFmtCtx->pb);

	return m_spdifOutputBuf;
}

/**
 * Test, if passthrough audio should be tried
 *
 * To enable passthrough, the current codec must be enabled in the setup
 *
 * Currently supported: AC3, E-AC-3, DTS
 *
 * @return             true, if the data should be passed through
 */
bool cAudioDecoder::ShouldTryPassthrough(void)
{
	return (m_passthroughMask & CODEC_AC3  && m_pAudioCtx->codec_id == AV_CODEC_ID_AC3) ||
	       (m_passthroughMask & CODEC_EAC3 && m_pAudioCtx->codec_id == AV_CODEC_ID_EAC3) ||
	       (m_passthroughMask & CODEC_DTS  && m_pAudioCtx->codec_id == AV_CODEC_ID_DTS);
}

/**
 * Handle audio format changes and setup audio, if format changed
 *
 * @retval 0     if new audio was correctly set up,
 *               otherwise return value of cSoftHdAudio::Setup()
 */
int cAudioDecoder::CheckUpdateFormat(bool passthrough)
{
	if (m_currentPassthroughMask == m_passthroughMask &&
	    m_currentNumChannels     == m_currentHwNumChannels &&
	    m_currentSampleRate      == m_currentHwSampleRate)
		return 0;

	m_currentHwSampleRate = m_pAudioCtx->sample_rate;
	m_currentHwNumChannels = m_pAudioCtx->ch_layout.nb_channels;
	m_currentPassthroughMask = m_passthroughMask;

	if (passthrough) {
		CloseSpdifMuxer();
		m_currentHwSampleRate = AUDIO_PASSTHROUGH_RATE_HZ;
		m_currentHwNumChannels = AUDIO_PASSTHROUGH_NUM_CHANNELS;

		// E-AC3 over HDMI: some receivers need HBR
		if (m_pAudioCtx->codec_id == AV_CODEC_ID_EAC3)
			m_currentHwSampleRate *= 4;
	}

	int err = 0;
	if ((err = m_pAudio->Setup(m_pAudioCtx->pkt_timebase, m_currentHwSampleRate, m_currentHwNumChannels, passthrough)) < 0) {
		// E-AC3 over HDMI: try without HBR
		m_currentHwSampleRate /= 4;

		if (m_pAudioCtx->codec_id != AV_CODEC_ID_EAC3 ||
		  ((err = m_pAudio->Setup(m_pAudioCtx->pkt_timebase, m_currentHwSampleRate, m_currentHwNumChannels, passthrough)) < 0)) {
			LOGERROR("audiocodec: %s: format change update error", __FUNCTION__);
			m_currentHwSampleRate = 0;
			m_currentHwNumChannels = 0;
			return err;
		}
	}

	// remember for next update check
	m_currentSampleRate = m_currentHwSampleRate;
	m_currentNumChannels = m_currentHwNumChannels;

	LOGDEBUG2(L_SOUND, "audiocodec: %s: format change %s %dHz *%d channels%s%s%s%s%d", __FUNCTION__,
		av_get_sample_fmt_name(m_pAudioCtx->sample_fmt), m_currentHwSampleRate, m_currentHwNumChannels,
		m_passthroughMask & CODEC_AC3 ? " AC3" : "",
		m_passthroughMask & CODEC_EAC3 ? " EAC3" : "",
		m_passthroughMask & CODEC_DTS ? " DTS" : "",
		m_passthroughMask ? " passthrough mask " : "",
		m_passthroughMask ? m_passthroughMask : 0);

	return 0;
}

/**
 * Forward an audio packet either to the decoder or passthrough
 *
 * @param avpkt        audio packet to decode
 */
void cAudioDecoder::Decode(const AVPacket *avpkt)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (ShouldTryPassthrough())
		Passthrough(avpkt);
	else
		DecodePCM(avpkt);
}

/**
 * Passthrough audio data
 *
 * Build spdif headers depending on the codec and send the
 * data to the audio device.
 *
 * @param avpkt         undecoded audio packet
 *
 * @retval 0            codec is not supported for passthrough, sth. went wrong or we need more data to finish a spdif burst packet
 * @retval 1            spdif burst was enqueued
 */
int cAudioDecoder::Passthrough(const AVPacket *avpkt)
{
	if (CheckUpdateFormat(true)) {
		LOGERROR("audiocodec: %s: unsupported format!", __FUNCTION__);
		return 0;
	}

	if (!m_spdifFmtCtx && !OpenSpdifMuxer(m_pAudioCtx->codec_id, m_currentHwSampleRate)) {
		LOGERROR("audiocodec: %s: failed to open spdif muxer for codec %s @ %dHz!", __FUNCTION__, avcodec_get_name(m_pAudioCtx->codec_id), m_currentHwSampleRate);
		return 0;
	}

	m_pAudio->SetTimebase(m_pAudioCtx->pkt_timebase);

	const auto &burst = BuildIEC61937(avpkt);

	if (burst.empty())
		return 0;

	m_pAudio->EnqueueSpdif(reinterpret_cast<const uint16_t *>(burst.data()), burst.size(), avpkt->pts);
	return 1;
}

/**
 * Decode an audio packet
 *
 * @param avpkt        audio packet to decode
 */
void cAudioDecoder::DecodePCM(const AVPacket *avpkt)
{
	int retSend, retRec;
	AVFrame *frame;

	frame = m_pFrame;
	av_frame_unref(frame);

	do {
		retSend = avcodec_send_packet(m_pAudioCtx, avpkt);
		if (retSend < 0 && retSend != AVERROR(EAGAIN))
			LOGERROR("audiocodec: %s: avcodec_send_packet error: %s", __FUNCTION__, av_err2str(retSend));

		do {
			retRec = avcodec_receive_frame(m_pAudioCtx, frame);

			if (retRec < 0) {
				if (retRec != AVERROR(EAGAIN))
					LOGERROR("audiocodec: %s: avcodec_receive_frame error: %s", __FUNCTION__, av_err2str(retRec));
				continue;
			}

			if (m_lastPts == AV_NOPTS_VALUE && avpkt->pts == AV_NOPTS_VALUE) {
				// the first AVPacket has no valid PTS, if its PES packet has been truncated while searching for the sync word
				av_frame_unref(frame);
				continue;
			}

			// update audio clock and remember last PTS or guess the next PTS
			if (frame->pts != AV_NOPTS_VALUE) {
				m_lastPts = frame->pts;
			} else if (m_lastPts != AV_NOPTS_VALUE) {
				frame->pts = m_lastPts +
					av_rescale_q(frame->nb_samples, (AVRational){1, frame->sample_rate}, m_pAudioCtx->pkt_timebase);
				m_lastPts = frame->pts;
			}

			if (CheckUpdateFormat(false)) {
				LOGERROR("audiocodec: %s: unsupported format!", __FUNCTION__);
				av_frame_unref(frame);
				return;
			}

			m_pAudio->Filter(frame, m_pAudioCtx);

		} while (retRec == 0);
	} while (retSend == AVERROR(EAGAIN));
}

/**
 * Flush the audio decoder buffers
 *
 * Also resets the last PTS and Codec ID
 */
void cAudioDecoder::FlushBuffers(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	LOGDEBUG2(L_CODEC, "audiocodec: %s", __FUNCTION__);
	if (m_pAudioCtx)
		avcodec_flush_buffers(m_pAudioCtx);

	m_lastPts = AV_NOPTS_VALUE;
	m_codecId = AV_CODEC_ID_NONE;
}

/**
 * Set audio pass-through mask
 *
 * @param mask         codec mask to enable (AC-3, E-AC-3, DTS)
 */
void cAudioDecoder::SetPassthroughMask(int mask)
{
	LOGDEBUG2(L_CODEC, "audiocodec: %s: %d", __FUNCTION__, mask);
	m_passthroughMask = mask & (CODEC_AC3 | CODEC_EAC3 | CODEC_DTS);
}
