///
///	@file codec_audio.cpp	@brief Audio decoder functions
///
///	Copyright (c) 2009 - 2015 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 by zille.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
//////////////////////////////////////////////////////////////////////////////

///
///	@defgroup Codec The codec module.
///
///		This module contains all decoder and codec functions.
///		It is uses ffmpeg (http://ffmpeg.org) as backend.
///

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

extern "C" {
#include <libavcodec/avcodec.h>

#include "codec_audio.h"
#include "misc.h"
#include "audio.h"
}

/*****************************************************************************
**	cAudioDecoder class
*****************************************************************************/

/**
**	cAudioDecoder constructor
**
**	@param mask	passthrough mask
*/
cAudioDecoder::cAudioDecoder(int mask)
{
    if (!(Frame = av_frame_alloc()))
	Fatal("cAudioDecoder::cAudioDecoder: can't allocate audio decoder frame buffer");
    AudioCtx = NULL;

    PassthroughMask = mask & (CodecPCM | CodecAC3 | CodecEAC3 | CodecDTS);
    Debug2(L_CODEC, "cAudioDecoder::SetPassthrough %d", PassthroughMask);
}

/**
**	cAudioDecoder destructor
*/
cAudioDecoder::~cAudioDecoder(void)
{
    av_frame_free(&Frame);
}

/**
**	Open audio decoder.
**
**	@param audio_decoder	private audio decoder
**	@param codec_id	audio	codec id
*/
void cAudioDecoder::Open(enum AVCodecID codec_id, AVCodecParameters *Par, AVRational * timebase)
{
	const AVCodec *codec;

	if (codec_id == AV_CODEC_ID_AC3) {
		if (!(codec = avcodec_find_decoder_by_name("ac3_fixed"))) {
			Fatal("cAudioDecoder::Open: codec ac3_fixed ID %#06x not found", codec_id);
		}
	} else if (codec_id == AV_CODEC_ID_AAC) {
		if (!(codec = avcodec_find_decoder_by_name("aac_fixed"))) {
			Fatal("cAudioDecoder::Open: codec aac_fixed ID %#06x not found", codec_id);
		}
	} else {
		if (!(codec = avcodec_find_decoder(codec_id))) {
			Fatal("cAudioDecoder::Open: codec %s ID %#06x not found",
				avcodec_get_name(codec_id), codec_id);
			// FIXME: errors aren't fatal
		}
	}

	if (!(AudioCtx = avcodec_alloc_context3(codec))) {
		Fatal("cAudioDecoder::Open: can't allocate audio codec context");
	}

	AudioCtx->pkt_timebase.num = timebase->num;
	AudioCtx->pkt_timebase.den = timebase->den;

	if (Par) {
		if ((avcodec_parameters_to_context(AudioCtx, Par)) < 0)
			Error("cAudioDecoder::Open: insert parameters to context failed!");
	}

	// open codec
	if (avcodec_open2(AudioCtx, AudioCtx->codec, NULL) < 0) {
		Fatal("cAudioDecoder::Open: can't open audio codec");
	}
	Debug2(L_CODEC, "cAudioDecoder::Open: Codec %s found PassthroughMask %d", AudioCtx->codec->long_name, PassthroughMask);

	SampleRate = 0;
	HwSampleRate = 0;
	Channels = 0;
	HwChannels = 0;
	Passthrough = 0;
}

/**
**	Close audio decoder
**
*/
void cAudioDecoder::Close(void)
{
	Debug2(L_CODEC, "cAudioDecoder::Close");
	if (AudioCtx)
		avcodec_free_context(&AudioCtx);
}

/**
**	Audio pass-through decoder helper.
**
**	@param avpkt		undecoded audio packet
**	@param frame		decoded audio frame
**
**	@returns 0		no passthrough, nothing done
**	@returns -1		passthrough, but sth went wrong
**	@returns 1		passthrough, data enqueued
*/
int cAudioDecoder::DecodePassthrough(const AVPacket * avpkt, AVFrame *frame)
{
    // AC3 passthrough
    if (PassthroughMask & CodecAC3 && AudioCtx->codec_id == AV_CODEC_ID_AC3) {
	uint16_t *spdif;
	int spdif_sz;

	spdif = Spdif;
	spdif_sz = 6144;

	// build SPDIF header and append A52 audio to it
	// avpkt is the original data
	if (spdif_sz < avpkt->size + 8) {
	    Error("cAudioDecoder::DecodePassthrough: decoded data smaller than encoded");
	    return -1;
	}
	spdif[0] = htole16(0xF872);	// iec 61937 sync word
	spdif[1] = htole16(0x4E1F);
	spdif[2] = htole16(IEC61937_AC3 | (avpkt->data[5] & 0x07) << 8);
	spdif[3] = htole16(avpkt->size * 8);
	// copy original data for output
	// FIXME: not 100% sure, if endian is correct on not intel hardware
	swab(avpkt->data, spdif + 4, avpkt->size);
	// FIXME: don't need to clear always
	memset(spdif + 4 + avpkt->size / 2, 0, spdif_sz - 8 - avpkt->size);
	// don't play with the ac-3 samples
	AudioEnqueueSpdif(AudioCtx, spdif, spdif_sz, frame);
	return 1;
    }
// TrueHD passthrough
if (PassthroughMask & CodecTrueHD && AudioCtx->codec_id == AV_CODEC_ID_TRUEHD) {
	uint16_t *spdif;
	int spdif_sz;

	spdif = Spdif;
	spdif_sz = 61424;

	// build SPDIF header and append A52 audio to it
	// avpkt is the original data
	if (spdif_sz < avpkt->size + 8) {
	    Error("cAudioDecoder::DecodePassthrough: decoded data smaller than encoded");
	    return -1;
	}
	spdif[0] = htole16(0xF872);	// iec 61937 sync word
	spdif[1] = htole16(0x4E1F);
	spdif[2] = htole16(IEC61937_TRUEHD | (avpkt->data[5] & 0x07) << 8);
	spdif[3] = htole16(avpkt->size * 8);
	// copy original data for output
	// FIXME: not 100% sure, if endian is correct on not intel hardware
	swab(avpkt->data, spdif + 4, avpkt->size);
	// FIXME: don't need to clear always
	memset(spdif + 4 + avpkt->size / 2, 0, spdif_sz - 8 - avpkt->size);
	// don't play with the ac-3 samples
	AudioEnqueueSpdif(AudioCtx, spdif, spdif_sz, frame);
	return 1;
    }
	
    // EAC3 passthrough
    if (PassthroughMask & CodecEAC3 && AudioCtx->codec_id == AV_CODEC_ID_EAC3) {
	uint16_t *spdif;
	int spdif_sz;
	int repeat;

	// build SPDIF header and append A52 audio to it
	// avpkt is the original data
	spdif = Spdif;
	spdif_sz = 24576;		// 4 * 6144
	if (HwSampleRate == 48000) {
	    spdif_sz = 6144;
	}
	if (spdif_sz < SpdifIndex + avpkt->size + 8) {
	    Error("cAudioDecoder::DecodePassthrough: decoded data smaller than encoded");
	    return -1;
	}
	// check if we must pack multiple packets
	repeat = 1;
	if ((avpkt->data[4] & 0xc0) != 0xc0) {	// fscod
	    static const uint8_t eac3_repeat[4] = { 6, 3, 2, 1 };

	    // fscod2
	    repeat = eac3_repeat[(avpkt->data[4] & 0x30) >> 4];
	}
	// Debug2(L_CODEC, "cAudioDecoder::DecodePassthrough: repeat %d %d\n", repeat, avpkt->size);

	// copy original data for output
	// pack upto repeat EAC-3 pakets into one IEC 61937 burst
	// FIXME: not 100% sure, if endian is correct on not intel hardware
	swab(avpkt->data, spdif + 4 + SpdifIndex, avpkt->size);
	SpdifIndex += avpkt->size;
	if (++SpdifCount < repeat) {
	    return 1;
	}

	spdif[0] = htole16(0xF872);	// iec 61937 sync word
	spdif[1] = htole16(0x4E1F);
	spdif[2] = htole16(IEC61937_EAC3);
	spdif[3] = htole16(SpdifIndex * 8);
	memset(spdif + 4 + SpdifIndex / 2, 0,
	    spdif_sz - 8 - SpdifIndex);

	// don't play with the eac-3 samples
	AudioEnqueueSpdif(AudioCtx, spdif, spdif_sz, frame);
	SpdifIndex = 0;
	SpdifCount = 0;
	return 1;
    }

    // DTS passthrough
    if (PassthroughMask & CodecDTS && AudioCtx->codec_id == AV_CODEC_ID_DTS) {
	uint16_t *spdif;
	uint8_t nbs;
	int bsid;
	int burst_sz;

	nbs = (uint8_t)((avpkt->data[4]&0x01)<<6)|((avpkt->data[5]>>2)&0x3f);
	switch(nbs) {
	    case 0x07:
	        bsid = 0x0a;
	        burst_sz = 1024;
	        break;
	    case 0x0f:
	        bsid = IEC61937_DTS1;
	        burst_sz = 2048;
	        break;
	    case 0x1f:
	        bsid = IEC61937_DTS2;
	        burst_sz = 4096;
	        break;
	    case 0x3f:
	        bsid = IEC61937_DTS3;
	        burst_sz = 8192;
	        break;
	    default:
	        bsid = 0x00;
	        if (nbs < 5)
	            nbs = 127;
	        burst_sz = (nbs+1)*32*2+2;
	        break;
	}

	spdif = Spdif;

	// build SPDIF header and append DTS audio to it
	// avpkt is the original data
	if (burst_sz < avpkt->size + 8) {
	    Error("cAudioDecoder::DecodePassthrough: decoded data smaller than encoded");
	    return -1;
	}
	spdif[0] = htole16(0xF872);	// iec 61937 sync word
	spdif[1] = htole16(0x4E1F);
	spdif[2] = htole16(bsid);
	spdif[3] = htole16(avpkt->size * 8);
	spdif[4] = htole16(0x7FFE);
	spdif[5] = htole16(0x8001);
	// copy original data for output
	// FIXME: not 100% sure, if endian is correct on not intel hardware
	swab(avpkt->data, spdif + 4, avpkt->size);
	// FIXME: don't need to clear always
	memset(spdif + 4 + avpkt->size, 0, burst_sz - 8 - avpkt->size);
	// don't play with the dts samples
	AudioEnqueueSpdif(AudioCtx, spdif, burst_sz, frame);
	return 1;
    }
    return 0;
}

/**
**	Handle audio format changes
**
*/
int cAudioDecoder::UpdateFormat(void)
{
	int passthrough;
	int err;

	Debug2(L_SOUND, "cAudioDecoder::UpdateFormat: format change %s %dHz *%d channels%s%s%s%s%s%s%d",
		av_get_sample_fmt_name(AudioCtx->sample_fmt), AudioCtx->sample_rate, AudioCtx->ch_layout.nb_channels,
		PassthroughMask & CodecPCM ? " PCM" : "",
		PassthroughMask & CodecMPA ? " MPA" : "",
		PassthroughMask & CodecAC3 ? " AC3" : "",
		PassthroughMask & CodecEAC3 ? " EAC3" : "",
		PassthroughMask & CodecDTS ? " DTS" : "",
		PassthroughMask ? " passthrough mask " : "",
		PassthroughMask ? PassthroughMask : 0);

	SampleRate = AudioCtx->sample_rate;
	HwSampleRate = AudioCtx->sample_rate;
	Channels = AudioCtx->ch_layout.nb_channels;
	HwChannels = AudioCtx->ch_layout.nb_channels;
	Passthrough = PassthroughMask;

	if ((Passthrough & CodecAC3 && AudioCtx->codec_id == AV_CODEC_ID_AC3) ||
	    (Passthrough & CodecEAC3 && AudioCtx->codec_id == AV_CODEC_ID_EAC3) ||
	    (Passthrough & CodecDTS && AudioCtx->codec_id == AV_CODEC_ID_DTS)) {
		HwChannels = 2;
		SpdifIndex = 0;
		SpdifCount = 0;
		passthrough = 1;
	}

	if ((err = AudioSetup(AudioCtx, HwSampleRate, HwChannels, passthrough))) {
		HwSampleRate /= 4;
		if (AudioCtx->codec_id != AV_CODEC_ID_EAC3 ||
		   (err = AudioSetup(AudioCtx, HwSampleRate, HwChannels, passthrough))) {
			HwSampleRate = 0;
			HwChannels = 0;
			Error("cAudioDecoder::UpdateFormat: format change update error");
			return err;
		}
	}
	return 0;
}

/**
**	Decode an audio packet.
**
**	@param avpkt		audio packet
*/
void cAudioDecoder::Decode(const AVPacket * avpkt)
{
	AVFrame *frame;
	int ret_send, ret_rec;

	// FIXME: don't need to decode pass-through codecs
	frame = Frame;
	av_frame_unref(frame);

send:
	ret_send = avcodec_send_packet(AudioCtx, avpkt);
	if (ret_send < 0)
		Error("cAudioDecoder::Decode: avcodec_send_packet error: %s",
			av_err2str(ret_send));

	ret_rec = avcodec_receive_frame(AudioCtx, frame);
	if (ret_rec < 0) {
		if (ret_rec != AVERROR(EAGAIN)) {
			Error("cAudioDecoder::Decode: avcodec_receive_frame error: %s",
				av_err2str(ret_rec));
		} else if (last_pts == (int64_t)AV_NOPTS_VALUE && avpkt->pts != (int64_t)AV_NOPTS_VALUE) {
			// if multiple avpkt are needed for the (first!) frame (last_pts == AV_NOPTS_VALUE),
			// remember the avpkt->pts if we have one and use it for the frame->pts
			// if we don't get one after decode. this way, last_pts also gets set
			Debug2(L_CODEC, "cAudioDecoder::Decode: New audio stream, set initial pts to avpkt->pts %s",
				Timestamp2String(avpkt->pts * 1000 * av_q2d(AudioCtx->pkt_timebase)));
			initial_pts = avpkt->pts;
		}
	} else {
		// Control PTS is valid
		if (last_pts == (int64_t) AV_NOPTS_VALUE &&
			frame->pts == (int64_t) AV_NOPTS_VALUE) {
			Warning("cAudioDecoder::Decode: NO VALID PTS, set frame->pts to last known avpkt->pts %s",
				Timestamp2String(initial_pts * 1000 * av_q2d(AudioCtx->pkt_timebase)));
			frame->pts = initial_pts;
			initial_pts = AV_NOPTS_VALUE;
		}
		// update audio clock
		if (frame->pts != (int64_t) AV_NOPTS_VALUE) {
			last_pts = frame->pts;
		} else if (last_pts != (int64_t) AV_NOPTS_VALUE) {
			frame->pts = last_pts +
				(int64_t)(frame->nb_samples /
				av_q2d(AudioCtx->pkt_timebase) /
				frame->sample_rate);
			last_pts = frame->pts;
		}

		if (Passthrough != PassthroughMask ||
		    Channels != AudioCtx->ch_layout.nb_channels ||
		    SampleRate != AudioCtx->sample_rate) {
			UpdateFormat();
		}

		if (!HwChannels || !HwSampleRate) {
			Error("cAudioDecoder::Decode: unsupported format!");
			av_frame_unref(frame);
			return;
		}

		if (DecodePassthrough(avpkt, frame))
			return;

		AudioFilter(frame, AudioCtx);
	}

	if (ret_send == AVERROR(EAGAIN))
		goto send;
}

/**
**	Flush the audio decoder.
*/
void cAudioDecoder::FlushBuffers(void)
{
	Debug2(L_CODEC, "cAudioDecoder::FlushBuffers");
	if (AudioCtx)
		avcodec_flush_buffers(AudioCtx);

	last_pts = AV_NOPTS_VALUE;
}

/**
**	Set audio pass-through mask
**
**	@param mask	enable mask (PCM, AC-3, E-AC-3, DTS)
*/
void cAudioDecoder::SetPassthrough(int mask)
{
	Debug2(L_CODEC, "cAudioDecoder::SetPassthrough %d", mask);
	PassthroughMask = mask & (CodecPCM | CodecAC3 | CodecEAC3 | CodecDTS);
}
