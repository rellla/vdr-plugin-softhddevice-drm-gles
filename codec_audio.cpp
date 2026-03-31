/**
 * @file codec_audio.cpp
 * Audio decoder class
 *
 * This file defines cAudioDecoder, which has all the functions
 * to decode audio data. It's the audio interface to ffmpeg.
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

#include <cstdint>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "audio.h"
#include "codec_audio.h"
#include "logger.h"
#include "misc.h"

/*****************************************************************************
 * cAudioDecoder class
 ****************************************************************************/

/**
 * Audio decoder class constructor
 *
 * @param audio           audio module
 */
cAudioDecoder::cAudioDecoder(cSoftHdAudio *audio)
	: m_pAudio(audio),
	  m_passthroughMask(m_pAudio->GetPassthrough() & (CODEC_AC3 | CODEC_EAC3 | CODEC_DTS))
{
	if (!(m_pFrame = av_frame_alloc()))
		LOGFATAL("audiocodec: %s: can't allocate audio decoder frame buffer", __FUNCTION__);

	LOGDEBUG2(L_CODEC, "audiocodec: %s: Set passthrough mask %d", __FUNCTION__, m_passthroughMask);
}

/**
 * Audio decoder class destructor
 */
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
 */
void cAudioDecoder::Open(AVCodecID codecId, AVCodecParameters *par, AVRational timebase)
{
	const AVCodec *codec;

	m_codecId = codecId;

	// FIXME: errors shouldn't be fatal, maybe just disable audio
	if (codecId == AV_CODEC_ID_AC3) {
		if (!(codec = avcodec_find_decoder_by_name("ac3_fixed"))) {
			LOGFATAL("audiocodec: %s: codec ac3_fixed ID %#06x not found", __FUNCTION__, codecId);
		}
	} else if (codecId == AV_CODEC_ID_AAC) {
		if (!(codec = avcodec_find_decoder_by_name("aac_fixed"))) {
			LOGFATAL("audiocodec: %s: codec aac_fixed ID %#06x not found", __FUNCTION__, codecId);
		}
	} else {
		if (!(codec = avcodec_find_decoder(codecId))) {
			LOGFATAL("audiocodec: %s: codec %s ID %#06x not found", __FUNCTION__,
			avcodec_get_name(codecId), codecId);
		}
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
	m_currentPassthrough = 0;
}

/**
 * Close the audio decoder
 */
void cAudioDecoder::Close(void)
{
	LOGDEBUG2(L_CODEC, "audiocodec: %s", __FUNCTION__);
	if (m_pAudioCtx)
		avcodec_free_context(&m_pAudioCtx);

	m_codecId = AV_CODEC_ID_NONE;
	m_lastPts = AV_NOPTS_VALUE;
}

/**
 * Passthrough audio data
 *
 * Build spdif headers depending on the codec and send the
 * data to the audio device.
 * Currently supported: AC3, EAC3, DTS
 *
 * @param avpkt         undecoded audio packet
 * @param frame         decoded audio frame
 *
 * @returns 0           codec is not supported for passthrough, use Filter to handle the data
 * @returns -1          sth went wrong, data will be discarded
 * @returns 1           data accepted
 *                      if finished, spdif header was created and data was sent to passthrough device
 */
int cAudioDecoder::DecodePassthrough(const AVPacket * avpkt, AVFrame *frame)
{
	m_pAudio->SetTimebase(&m_pAudioCtx->pkt_timebase);

	// AC3 passthrough
	if (m_passthroughMask & CODEC_AC3 && m_pAudioCtx->codec_id == AV_CODEC_ID_AC3) {
		uint16_t *spdif = m_spdifOutput;
		int spdifSize = AC3_FRAME_SIZE * 4; // frames * channels * (samplesize / 8)

		if (spdifSize < avpkt->size + 8) {
			LOGERROR("audiocodec: %s: too much data for spdif buffer!", __FUNCTION__);
			return -1;
		}

		if (avpkt->size < 6)
			return -1;

		// build SPDIF header and append AC3 audio data to it
		int bitstreamMode = avpkt->data[5] & 0x07;
		spdif[0] = htole16(IEC61937_PREAMBLE1);
		spdif[1] = htole16(IEC61937_PREAMBLE2);
		spdif[2] = htole16(IEC61937_AC3 | bitstreamMode << 8);
		spdif[3] = htole16(avpkt->size * 8);
		swab(avpkt->data, spdif + 4, avpkt->size);
		memset((uint8_t *)(spdif + 4) + avpkt->size, 0, spdifSize - 8 - avpkt->size);

		m_pAudio->EnqueueSpdif(spdif, spdifSize, frame);
		return 1;
	}

	// EAC3 passthrough
	if (m_passthroughMask & CODEC_EAC3 && m_pAudioCtx->codec_id == AV_CODEC_ID_EAC3) {
		uint16_t *spdif = m_spdifOutput;
		int spdifSize = EAC3_FRAME_SIZE * 4; // frames * channels * (samplesize / 8)
		int repeat = 1;

		// spdifSize is smaller, if we don't have 192000
		if (m_currentHwSampleRate == 48000) {
			spdifSize /= 4;
		}

		if (spdifSize < m_spdifIndex + avpkt->size + 8) {
			LOGERROR("audiocodec: %s: too much data for spdif buffer!", __FUNCTION__);
			ResetSpdif();
			return -1;
		}

		if (avpkt->size < 5) {
			LOGERROR("audiocodec: %s: avpkt size too small!", __FUNCTION__);
			ResetSpdif();
			return -1;
		}

		// check if we need to pack multiple packets
		int fscod = (avpkt->data[4] >> 6) & 0x3;
		int numblkscod = 6;
		if (fscod != 0x3) {
			numblkscod = (avpkt->data[4] >> 4) & 0x3;
			static const uint8_t eac3_repeat[4] = { 6, 3, 2, 1 };
			repeat = eac3_repeat[numblkscod];
		}

		if (repeat * avpkt->size > spdifSize - 8) {
			LOGERROR("audiocodec: %s: spdif size too small!", __FUNCTION__);
			ResetSpdif();
			return -1;
		}

		// pack upto repeat EAC-3 pakets into one IEC 61937 burst
		swab(avpkt->data, (uint8_t *)(spdif + 4) + m_spdifIndex, avpkt->size);
		m_spdifIndex += avpkt->size;

//		LOGDEBUG2(L_CODEC, "audiocodec: %s: E-AC3: set repeat to %d (fscod = %d, numblkscode = %d) avpkt->size %d (spdifSize %d) (repeatCount gets %d) (m_spdifIndex = %d)",
//			__FUNCTION__, repeat, fscod, numblkscod, avpkt->size, spdifSize, m_spdifRepeatCount + 1, m_spdifIndex);

		if (++m_spdifRepeatCount < repeat)
			return 1;

		if (m_spdifRepeatCount > 6) {
			ResetSpdif();
			return -1;
		}

		// build SPDIF header and append E-AC3 audio data to it
		spdif[0] = htole16(IEC61937_PREAMBLE1);
		spdif[1] = htole16(IEC61937_PREAMBLE2);
		spdif[2] = htole16(IEC61937_EAC3);
		spdif[3] = htole16(m_spdifIndex * 8);
		int pad = spdifSize - 8 - m_spdifIndex;
		if (pad > 0)
			memset((uint8_t *)(spdif + 4) + m_spdifIndex, 0, pad);

		m_pAudio->EnqueueSpdif(spdif, spdifSize, frame);
		ResetSpdif();
		return 1;
	}

	// DTS passthrough
	if (m_passthroughMask & CODEC_DTS && m_pAudioCtx->codec_id == AV_CODEC_ID_DTS) {
		uint16_t *spdif = m_spdifOutput;

		uint8_t nbs;
		int bsid;
		int burstSz;

		if (avpkt->size < 6) {
			LOGERROR("audiocodec: %s: avpkt size too small!", __FUNCTION__);
			return -1;
		}

		nbs = (uint8_t)((avpkt->data[4] & 0x01) << 6) |
		               ((avpkt->data[5] >> 2) & 0x3f);
		switch(nbs) {
		case 0x07:
			bsid = 0x0a;	// MPEG-2 layer 3 is used when?
			burstSz = 1024;
			break;
		case 0x0f:
			bsid = IEC61937_DTS1;
			burstSz = DTS1_FRAME_SIZE * 4; // frames * channels * (samplesize / 8)
			break;
		case 0x1f:
			bsid = IEC61937_DTS2;
			burstSz = DTS2_FRAME_SIZE * 4; // frames * channels * (samplesize / 8)
			break;
		case 0x3f:
			bsid = IEC61937_DTS3;
			burstSz = DTS3_FRAME_SIZE * 4; // frames * channels * (samplesize / 8)
			break;
		default:
			bsid = IEC61937_NULL;
			if (nbs < 5)
				nbs = 127;
			burstSz = (nbs + 1) * 32 * 2 + 2;
			break;
		}

		// build SPDIF header and append DTS audio data to it
		if (burstSz < avpkt->size + 8) {
			LOGERROR("audiocodec: %s: too much data for spdif buffer!", __FUNCTION__);
			return -1;
		}
		spdif[0] = htole16(IEC61937_PREAMBLE1);
		spdif[1] = htole16(IEC61937_PREAMBLE2);
		spdif[2] = htole16(bsid);
		spdif[3] = htole16(avpkt->size * 8);
		spdif[4] = htole16(DTS_PREAMBLE_16BE_1);
		spdif[5] = htole16(DTS_PREAMBLE_16BE_2);
		swab(avpkt->data, spdif + 6, avpkt->size);
		memset((uint8_t *)(spdif + 6) + avpkt->size, 0, burstSz - 12 - avpkt->size);

		m_pAudio->EnqueueSpdif(spdif, burstSz, frame);
		return 1;
	}

	return 0;
}

/**
 * Handle audio format changes
 *
 * Setup audio, if format changed
 *
 * @return     0 if new audio was correctly set up,
 *               otherwise return value of cSoftHdAudio::Setup()
 */
int cAudioDecoder::UpdateFormat(void)
{
	int isPassthrough = 0;
	int err = 0;

	m_currentSampleRate = m_pAudioCtx->sample_rate;
	m_currentHwSampleRate = m_pAudioCtx->sample_rate;
	m_currentNumChannels = m_pAudioCtx->ch_layout.nb_channels;
	m_currentHwNumChannels = m_pAudioCtx->ch_layout.nb_channels;
	m_currentPassthrough = m_passthroughMask;

	if ((m_currentPassthrough & CODEC_AC3  && m_pAudioCtx->codec_id == AV_CODEC_ID_AC3) ||
	    (m_currentPassthrough & CODEC_EAC3 && m_pAudioCtx->codec_id == AV_CODEC_ID_EAC3) ||
	    (m_currentPassthrough & CODEC_DTS  && m_pAudioCtx->codec_id == AV_CODEC_ID_DTS)) {

		// E-AC3 over HDMI: some receivers need HBR
		if (m_pAudioCtx->codec_id == AV_CODEC_ID_EAC3)
			m_currentHwSampleRate *= 4;

		m_currentHwNumChannels = 2;
		m_spdifIndex = 0;
		m_spdifRepeatCount = 0;
		isPassthrough = 1;
	}

	if ((err = m_pAudio->Setup(m_pAudioCtx, m_currentHwSampleRate, m_currentHwNumChannels, isPassthrough)) < 0) {
		// E-AC3 over HDMI: try without HBR
		m_currentHwSampleRate /= 4;

		if (m_pAudioCtx->codec_id != AV_CODEC_ID_EAC3 ||
		  ((err = m_pAudio->Setup(m_pAudioCtx, m_currentHwSampleRate, m_currentHwNumChannels, isPassthrough)) < 0)) {
			LOGERROR("audiocodec: %s: format change update error", __FUNCTION__);
			m_currentHwSampleRate = 0;
			m_currentHwNumChannels = 0;
			return err;
		}
	}

	if (!err) {
		LOGDEBUG2(L_SOUND, "audiocodec: %s: format change %s %dHz *%d channels%s%s%s%s%d", __FUNCTION__,
			av_get_sample_fmt_name(m_pAudioCtx->sample_fmt), m_pAudioCtx->sample_rate, m_pAudioCtx->ch_layout.nb_channels,
			m_passthroughMask & CODEC_AC3 ? " AC3" : "",
			m_passthroughMask & CODEC_EAC3 ? " EAC3" : "",
			m_passthroughMask & CODEC_DTS ? " DTS" : "",
			m_passthroughMask ? " passthrough mask " : "",
			m_passthroughMask ? m_passthroughMask : 0);
	}

	return err;
}

/**
 * Decode an audio packet
 *
 * @param avpkt        audio packet to decode
 */
void cAudioDecoder::Decode(const AVPacket * avpkt)
{
	int retSend, retRec;
	AVFrame *frame;

	// decoded frame is also needed for passthrough to set the PTS
	frame = m_pFrame;
	av_frame_unref(frame);

	do {
		retSend = avcodec_send_packet(m_pAudioCtx, avpkt);
		if (retSend < 0)
			LOGERROR("audiocodec: %s: avcodec_send_packet error: %s", __FUNCTION__, av_err2str(retSend));

		retRec = avcodec_receive_frame(m_pAudioCtx, frame);

		if (retRec < 0) {
			if (retRec != AVERROR(EAGAIN))
				LOGERROR("audiocodec: %s: avcodec_receive_frame error: %s", __FUNCTION__, av_err2str(retRec));
		} else {
			if (m_lastPts == AV_NOPTS_VALUE && avpkt->pts == AV_NOPTS_VALUE) {
				// the first AVPacket has no valid PTS, if its PES packet has been truncated while searching for the sync word
				av_frame_unref(frame);
				continue;
			}

			// update audio clock and remeber last PTS or guess the next PTS
			if (frame->pts != AV_NOPTS_VALUE) {
				m_lastPts = frame->pts;
			} else if (m_lastPts != AV_NOPTS_VALUE) {
				frame->pts = m_lastPts +
					av_rescale_q(frame->nb_samples, (AVRational){1, frame->sample_rate}, m_pAudioCtx->pkt_timebase);
				m_lastPts = frame->pts;
			}

			if (m_currentPassthrough != m_passthroughMask ||
				m_currentNumChannels != m_pAudioCtx->ch_layout.nb_channels ||
				m_currentSampleRate != m_pAudioCtx->sample_rate) {
				UpdateFormat();
			}

			if (!m_currentHwNumChannels || !m_currentHwSampleRate) {
				LOGERROR("audiocodec: %s: unsupported format!", __FUNCTION__);
				av_frame_unref(frame);
				return;
			}

			if (DecodePassthrough(avpkt, frame)) {
				av_frame_unref(frame);
				return;
			}

			m_pAudio->Filter(frame, m_pAudioCtx);
		}

	} while (retSend == AVERROR(EAGAIN));
}

/**
 * Flush the audio decoder
 */
void cAudioDecoder::FlushBuffers(void)
{
	LOGDEBUG2(L_CODEC, "audiocodec: %s", __FUNCTION__);
	if (m_pAudioCtx)
		avcodec_flush_buffers(m_pAudioCtx);

	m_lastPts = AV_NOPTS_VALUE;
	m_codecId = AV_CODEC_ID_NONE;

	ResetSpdif();
}

void cAudioDecoder::ResetSpdif(void)
{
	m_spdifIndex = 0;
	m_spdifRepeatCount = 0;
}

/**
 * Set audio pass-through mask
 *
 * @param mask         codec to enable (AC-3, E-AC-3, DTS)
 */
void cAudioDecoder::SetPassthrough(int mask)
{
	LOGDEBUG2(L_CODEC, "audiocodec: %s: %d", __FUNCTION__, mask);
	m_passthroughMask = mask & (CODEC_AC3 | CODEC_EAC3 | CODEC_DTS);
}
