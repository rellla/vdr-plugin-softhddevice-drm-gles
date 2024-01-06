///
///	@file codec.c	@brief Codec functions
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
///	$Id$
//////////////////////////////////////////////////////////////////////////////

///
///	@defgroup Codec The codec module.
///
///		This module contains all decoder and codec functions.
///		It is uses ffmpeg (http://ffmpeg.org) as backend.
///

    /// compile with pass-through support (stable, AC-3, E-AC-3 only)
#define USE_PASSTHROUGH

#include <unistd.h>
#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut
#include <pthread.h>

#include <libavcodec/avcodec.h>

#ifdef MAIN_H
#include MAIN_H
#endif
#include "iatomic.h"
#include "misc.h"
#include "video.h"
#include "audio.h"
#include "codec.h"
#include "softhddev.h"


//----------------------------------------------------------------------------
//	Global
//----------------------------------------------------------------------------

static pthread_mutex_t CodecLockMutex;

//----------------------------------------------------------------------------
//	Call-backs
//----------------------------------------------------------------------------

/**
**	Callback to negotiate the PixelFormat.
**
**	@param video_ctx	codec context
**	@param fmt		is the list of formats which are supported by
**				the codec, it is terminated by -1 as 0 is a
**				valid format, the formats are ordered by quality.
*/
static enum AVPixelFormat Codec_get_format(AVCodecContext * video_ctx,
		const enum AVPixelFormat *fmt)
{
	VideoDecoder *decoder;
	decoder = video_ctx->opaque;

	return Video_get_format(decoder->Render, video_ctx, fmt);
}

AVCodecContext *Codec_get_VideoContext(VideoDecoder * decoder)
{
	return decoder->VideoCtx;
}

//----------------------------------------------------------------------------
//	Test
//----------------------------------------------------------------------------

/**
**	Allocate a new video decoder context.
**
**	@param hw_decoder	video hardware decoder
**
**	@returns private decoder pointer for video decoder.
*/
VideoDecoder *CodecVideoNewDecoder(VideoRender * render)
{
    VideoDecoder *decoder;

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
		Fatal("codec: can't allocate vodeo decoder");
    }
    decoder->Render = render;

    return decoder;
}

/**
**	Deallocate a video decoder context.
**
**	@param decoder	private video decoder
*/
void CodecVideoDelDecoder(VideoDecoder * decoder)
{
    free(decoder);
}

/**
**	Open video decoder.
**
**	@param decoder	private video decoder
**	@param codec_id	video codec id
*/
void CodecVideoOpen(VideoDecoder * decoder, int codec_id, AVCodecParameters * Par,
		AVRational * timebase)
{
	AVCodec * codec;
	enum AVHWDeviceType type = 0;
	static AVBufferRef *hw_device_ctx = NULL;
	int err;

	Debug2(L_CODEC, "CodecVideoOpen: try open codec %s", avcodec_get_name(codec_id));
	if (VideoCodecMode(decoder->Render) & CODEC_V4L2M2M_H264 && codec_id == AV_CODEC_ID_H264) {
		if (!(codec = avcodec_find_decoder_by_name(VideoGetDecoderName(
			avcodec_get_name(codec_id)))))
			Error("CodecVideoOpen: The video codec %s is not present in libavcodec",
				VideoGetDecoderName(avcodec_get_name(codec_id)));
		else
			Debug2(L_CODEC, "CodecVideoOpen: avcodec_find_decoder_by_name found %s",
				VideoGetDecoderName(avcodec_get_name(codec_id)));
	} else {
		if (!(codec = avcodec_find_decoder(codec_id)))
			Error("CodecVideoOpen: The video codec %s is not present in libavcodec",
				avcodec_get_name(codec_id));
		else
			Debug2(L_CODEC, "CodecVideoOpen: avcodec_find_decoder found %s",
				VideoGetDecoderName(avcodec_get_name(codec_id)));

		if (!(VideoCodecMode(decoder->Render) & CODEC_NO_MPEG_HW && codec_id == AV_CODEC_ID_MPEG2VIDEO)) {
			for (int n = 0; ; n++) {
				const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, n);
				if (!cfg) {
					Warning("CodecVideoOpen: no HW config found");
					break;
				}
				if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
					cfg->device_type == AV_HWDEVICE_TYPE_DRM) {
					Debug2(L_CODEC, "CodecVideoOpen: HW codec %s found",
						av_hwdevice_get_type_name(cfg->device_type));
					type = cfg->device_type;
					break;
				}
			}
			Debug2(L_CODEC, "CodecVideoOpen: av_hwdevice_get_type_name HW codec %s not found",
				VideoGetDecoderName(avcodec_get_name(codec_id)));
		}
	}
	decoder->VideoCtx = avcodec_alloc_context3(codec);
	if (!decoder->VideoCtx) {
		Error("CodecVideoOpen: can't open video codec!");
	}

	decoder->VideoCtx->codec_id = codec_id;
	decoder->VideoCtx->get_format = Codec_get_format;
	decoder->VideoCtx->opaque = decoder;
//	decoder->VideoCtx->debug = FF_DEBUG_PICT_INFO;
//	decoder->VideoCtx->workaround_bugs = FF_BUG_AUTODETECT;
//	decoder->VideoCtx->flags = AV_CODEC_FLAG_COPY_OPAQUE;

	if (strstr(codec->name, "_v4l2")) {
		Debug2(L_CODEC, "CodecVideoOpen: Codec %s found", codec->long_name);
		int width;
		int height;

		if (!Par) {
			ParseResolutionH264(&width, &height);
			Debug2(L_CODEC, "CodecVideoOpen: Parsed width %d height %d", width, height);
			decoder->VideoCtx->coded_width = width;
			decoder->VideoCtx->coded_height = height;
		} else {
			decoder->VideoCtx->coded_width = Par->width;
			decoder->VideoCtx->coded_height = Par->height;
			Debug2(L_CODEC, "CodecVideoOpen: from Par: width %d height %d", decoder->VideoCtx->coded_width, decoder->VideoCtx->coded_height);
		}
/*
	} else {
		Debug2(L_CODEC, "CodecVideoOpen: Codec %s found", codec->long_name);
		decoder->VideoCtx->coded_width = 720;
		decoder->VideoCtx->coded_height = 576;
		decoder->VideoCtx->bits_per_coded_sample = 12;
*/
	}

	if (codec->capabilities & AV_CODEC_CAP_DRAW_HORIZ_BAND)
		Debug2(L_CODEC, "CodecVideoOpen: AV_CODEC_CAP_DRAW_HORIZ_BAND");
	if (codec->capabilities & AV_CODEC_CAP_DR1)
		Debug2(L_CODEC, "CodecVideoOpen: AV_CODEC_CAP_DR1");
	if (codec->capabilities & AV_CODEC_CAP_HARDWARE)
		Debug2(L_CODEC, "CodecVideoOpen: AV_CODEC_CAP_HARDWARE");
	if (codec->capabilities & AV_CODEC_CAP_HYBRID)
		Debug2(L_CODEC, "CodecVideoOpen: AV_CODEC_CAP_HYBRID");

//	decoder->VideoCtx->flags |= AV_CODEC_FLAG_BITEXACT;
//	decoder->VideoCtx->flags |= AV_CODEC_FLAG_UNALIGNED;
//	decoder->VideoCtx->flags |= AV_CODEC_FLAG_COPY_QPAQUE;
//	decoder->VideoCtx->flags2 |= AV_CODEC_FLAG2_FAST;
//	decoder->VideoCtx->flags |= AV_CODEC_FLAG_TRUNCATED;
//	if (codec->capabilities & AV_CODEC_CAP_DR1)
//		Debug2(L_CODEC, "[CodecVideoOpen] AV_CODEC_CAP_DR1 => get_buffer()");
	Debug2(L_CODEC, "CodecVideoOpen: check AV_CODEC_CAP_FRAME_THREADS and AV_CODEC_CAP_SLICE_THREADS");
	if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS ||
		AV_CODEC_CAP_SLICE_THREADS) {
		decoder->VideoCtx->thread_count = 4;
		Debug2(L_CODEC, "CodecVideoOpen: decoder use %d threads",
			decoder->VideoCtx->thread_count);
	}
	Debug2(L_CODEC, "CodecVideoOpen: check AV_CODEC_CAP_SLICE_THREADS");
	if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS){
		decoder->VideoCtx->thread_type = FF_THREAD_SLICE;
		Debug2(L_CODEC, "CodecVideoOpen: decoder use THREAD_SLICE threads");
	}

	if (type) {
		Debug2(L_CODEC, "CodecVideoOpen: check type");
		if (av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0) < 0)
			Error("CodecVideoOpen: Error init the HW decoder");
		else {
			decoder->VideoCtx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
			Debug2(L_CODEC, "CodecVideoOpen: av_buffer_ref(hw_device_ctx)");
		}
	}

	if (Par) {
		Debug2(L_CODEC, "CodecVideoOpen: have Par");
		if ((avcodec_parameters_to_context(decoder->VideoCtx, Par)) < 0)
			Error("CodecVideoOpen: insert parameters to context failed!");
		else
			Debug2(L_CODEC, "CodecVideoOpen: avcodec_parameters_to_context success");
	}
	if (timebase) {
		Debug2(L_CODEC, "CodecVideoOpen: have timebase num %d den %d", timebase->num, timebase->den);
		decoder->VideoCtx->pkt_timebase.num = timebase->num;
		decoder->VideoCtx->pkt_timebase.den = timebase->den;
	}
	err = avcodec_open2(decoder->VideoCtx, decoder->VideoCtx->codec, NULL);
	if (err < 0) {
		Fatal("CodecVideoOpen: Error opening the decoder: %s",
			av_err2str(err));
	}
	Debug2(L_CODEC, "CodecVideoOpen: decoder opened");
}

/**
**	Close video decoder.
**
**	@param decoder	private video decoder
*/
void CodecVideoClose(VideoDecoder * decoder)
{
	Debug2(L_CODEC, "CodecVideoClose: VideoCtx %p", decoder->VideoCtx);
	pthread_mutex_lock(&CodecLockMutex);
	if (decoder->VideoCtx) {
		avcodec_free_context(&decoder->VideoCtx);
	}
	pthread_mutex_unlock(&CodecLockMutex);
}

/**
**	Decode a video packet.
**
**	@param decoder	video decoder data
**	@param avpkt	video packet
**
**	@returns 1 if packet must send again.
*/
int CodecVideoSendPacket(VideoDecoder * decoder, const AVPacket * avpkt)
{
	int ret = AVERROR_DECODER_NOT_FOUND;

#if 0
	if (!decoder->VideoCtx->extradata_size) {
		AVBSFContext *bsf_ctx;
		const AVBitStreamFilter *f;
		int extradata_size;
		uint8_t *extradata;

		f = av_bsf_get_by_name("extract_extradata");
		if (!f)
			Error("extradata av_bsf_get_by_name failed!");

		if (av_bsf_alloc(f, &bsf_ctx) < 0)
			Error("extradata av_bsf_alloc failed!");

		bsf_ctx->par_in->codec_id = decoder->VideoCtx->codec_id;

		if (av_bsf_init(bsf_ctx) < 0)
			Error("extradata av_bsf_init failed!");

		if (av_bsf_send_packet(bsf_ctx, avpkt) < 0)
			Error("extradata av_bsf_send_packet failed!");

		if (av_bsf_receive_packet(bsf_ctx, avpkt) < 0)
			Error("extradata av_bsf_send_packet failed!");

		extradata = av_packet_get_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA,
			&extradata_size);

		decoder->VideoCtx->extradata = av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(decoder->VideoCtx->extradata, extradata, extradata_size);
		decoder->VideoCtx->extradata_size = extradata_size;

		av_bsf_free(&bsf_ctx);

		Debug2(L_CODEC, "extradata %p %d", decoder->VideoCtx->extradata, decoder->VideoCtx->extradata_size);
	}
#endif

	if (!avpkt->size) {
		return 0;
	}

	pthread_mutex_lock(&CodecLockMutex);
	if (decoder->VideoCtx) {
		ret = avcodec_send_packet(decoder->VideoCtx, avpkt);
	}
	pthread_mutex_unlock(&CodecLockMutex);
	if (ret == AVERROR(EAGAIN))
		return 1;
	if (ret < 0)
		Debug2(L_CODEC, "CodecVideoSendPacket: send_packet ret: %s",
			av_err2str(ret));
	return 0;
}

/**
**	Get a decoded a video frame.
**
**	@param decoder		video decoder data
**	@param no_deint		set interlaced_frame to 0
**
**	@returns 1	get no frame.
*/
int CodecVideoReceiveFrame(VideoDecoder * decoder, int no_deint)
{
	int ret;

	if (!(decoder->Frame = av_frame_alloc())) {
		Fatal("CodecVideoReceiveFrame: can't allocate decoder frame");
	}

	pthread_mutex_lock(&CodecLockMutex);
	if (decoder->VideoCtx) {
		ret = avcodec_receive_frame(decoder->VideoCtx, decoder->Frame);
	} else {
		av_frame_free(&decoder->Frame);
		pthread_mutex_unlock(&CodecLockMutex);
		return 1;
	}
	pthread_mutex_unlock(&CodecLockMutex);

	if (decoder->Frame->flags == AV_FRAME_FLAG_CORRUPT)
		Debug2(L_CODEC, "CodecVideoReceiveFrame: AV_FRAME_FLAG_CORRUPT");

	if (!ret) {
		if (no_deint) {
			decoder->Frame->interlaced_frame = 0;
			Debug2(L_CODEC, "CodecVideoReceiveFrame: interlaced_frame = 0");
		}
		VideoRenderFrame(decoder->Render, decoder->VideoCtx, decoder->Frame);
	} else {
		av_frame_free(&decoder->Frame);
		if (ret != AVERROR(EAGAIN))
			Debug2(L_CODEC, "CodecVideoReceiveFrame: receive_frame ret: %s", av_err2str(ret));
	}

	if (ret == AVERROR(EAGAIN))
		return 1;
	return 0;
}

/**
**	Flush the video decoder.
**
**	@param decoder	video decoder data
*/
void CodecVideoFlushBuffers(VideoDecoder * decoder)
{
	Debug2(L_CODEC, "CodecVideoFlushBuffers: VideoCtx %p", decoder->VideoCtx);
	pthread_mutex_lock(&CodecLockMutex);
	if (decoder->VideoCtx) {
		avcodec_flush_buffers(decoder->VideoCtx);
	}
	pthread_mutex_unlock(&CodecLockMutex);
}

//----------------------------------------------------------------------------
//	Audio
//----------------------------------------------------------------------------

///
///	Audio decoder structure.
///
struct _audio_decoder_
{
    AVCodecContext *AudioCtx;		///< audio codec context

    AVFrame *Frame;			///< decoded audio frame buffer
    int64_t last_pts;			///< last PTS
};

///
///	IEC Data type enumeration.
///
enum IEC61937
{
    IEC61937_AC3 = 0x01,		///< AC-3 data
    // FIXME: more data types
    IEC61937_EAC3 = 0x15,		///< E-AC-3 data
};

#ifdef USE_PASSTHROUGH
    ///
    /// Pass-through flags: CodecPCM, CodecAC3, CodecEAC3, ...
    ///
static char CodecPassthrough;
#else
static const int CodecPassthrough = 0;
#endif


/**
**	Allocate a new audio decoder context.
**
**	@returns private decoder pointer for audio decoder.
*/
AudioDecoder *CodecAudioNewDecoder(void)
{
    AudioDecoder *audio_decoder;

    if (!(audio_decoder = calloc(1, sizeof(*audio_decoder)))) {
		Fatal("codec: can't allocate audio decoder");
    }
    if (!(audio_decoder->Frame = av_frame_alloc())) {
		Fatal("codec: can't allocate audio decoder frame buffer");
    }
	audio_decoder->AudioCtx = NULL;

    return audio_decoder;
}

/**
**	Deallocate an audio decoder context.
**
**	@param decoder	private audio decoder
*/
void CodecAudioDelDecoder(AudioDecoder * decoder)
{
    av_frame_free(&decoder->Frame);	// callee does checks
    free(decoder);
}

/**
**	Open audio decoder.
**
**	@param audio_decoder	private audio decoder
**	@param codec_id	audio	codec id
*/
void CodecAudioOpen(AudioDecoder * audio_decoder, int codec_id,
		AVCodecParameters *Par, AVRational * timebase)
{
	AVCodec *codec;

	if (codec_id == AV_CODEC_ID_AC3) {
		if (!(codec = avcodec_find_decoder_by_name("ac3_fixed"))) {
			Fatal("codec: codec ac3_fixed ID %#06x not found", codec_id);
		}
	} else if (codec_id == AV_CODEC_ID_AAC) {
		if (!(codec = avcodec_find_decoder_by_name("aac_fixed"))) {
			Fatal("codec: codec aac_fixed ID %#06x not found", codec_id);
		}
	} else {
		if (!(codec = avcodec_find_decoder(codec_id))) {
			Fatal("codec: codec %s ID %#06x not found",
				avcodec_get_name(codec_id), codec_id);
			// FIXME: errors aren't fatal
		}
	}

	if (!(audio_decoder->AudioCtx = avcodec_alloc_context3(codec))) {
		Fatal("codec: can't allocate audio codec context");
	}

	audio_decoder->AudioCtx->pkt_timebase.num = timebase->num;
	audio_decoder->AudioCtx->pkt_timebase.den = timebase->den;

	if (Par) {
		if ((avcodec_parameters_to_context(audio_decoder->AudioCtx, Par)) < 0)
			Error("CodecAudioOpen: insert parameters to context failed!");
	}

	// open codec
	if (avcodec_open2(audio_decoder->AudioCtx, audio_decoder->AudioCtx->codec, NULL) < 0) {
		Fatal("codec: can't open audio codec");
	}
	Debug2(L_CODEC, "CodecAudioOpen: Codec %s found", audio_decoder->AudioCtx->codec->long_name);
}

/**
**	Close audio decoder.
**
**	@param audio_decoder	private audio decoder
*/
void CodecAudioClose(AudioDecoder * audio_decoder)
{
	Debug2(L_CODEC, "CodecAudioClose");
	if (audio_decoder->AudioCtx) {
		avcodec_free_context(&audio_decoder->AudioCtx);
	}
}

/**
**	Set audio pass-through.
**
**	@param mask	enable mask (PCM, AC-3, E-AC-3)
*/
void CodecSetAudioPassthrough(int mask)
{
#ifdef USE_PASSTHROUGH
    CodecPassthrough = mask & (CodecPCM | CodecAC3 | CodecEAC3);
#endif
    (void)mask;
}

/**
**	Decode an audio packet.
**
**	@param audio_decoder	audio decoder data
**	@param avpkt		audio packet
*/
void CodecAudioDecode(AudioDecoder * audio_decoder, const AVPacket * avpkt)
{
	AVFrame *frame;
	int ret_send, ret_rec;

	// FIXME: don't need to decode pass-through codecs
	frame = audio_decoder->Frame;
	av_frame_unref(frame);

send:
	ret_send = avcodec_send_packet(audio_decoder->AudioCtx, avpkt);
	if (ret_send < 0)
		Error("CodecAudioDecode: avcodec_send_packet error: %s",
			av_err2str(ret_send));

	ret_rec = avcodec_receive_frame(audio_decoder->AudioCtx, frame);
	if (ret_rec < 0) {
		Error("CodecAudioDecode: avcodec_receive_frame error: %s",
			av_err2str(ret_rec));
	} else {
		// Control PTS is valid
		if (audio_decoder->last_pts == (int64_t) AV_NOPTS_VALUE &&
			frame->pts == (int64_t) AV_NOPTS_VALUE) {
			Warning("CodecAudioDecode: NO VALID PTS");
		}
		// update audio clock
		if (frame->pts != (int64_t) AV_NOPTS_VALUE) {
			audio_decoder->last_pts = frame->pts;
		} else if (audio_decoder->last_pts != (int64_t) AV_NOPTS_VALUE) {
			frame->pts = audio_decoder->last_pts + 
				(int64_t)(frame->nb_samples /
				av_q2d(audio_decoder->AudioCtx->pkt_timebase) /
				frame->sample_rate);
			audio_decoder->last_pts = frame->pts;
		}
		AudioFilter(frame, audio_decoder->AudioCtx);
	}

	if (ret_send == AVERROR(EAGAIN))
		goto send;
}


/**
**	Flush the audio decoder.
**
**	@param decoder	audio decoder data
*/
void CodecAudioFlushBuffers(AudioDecoder * decoder)
{
	Debug2(L_CODEC, "CodecAudioFlushBuffers");
	if (decoder->AudioCtx) {
		avcodec_flush_buffers(decoder->AudioCtx);
	}
	decoder->last_pts = AV_NOPTS_VALUE;
}

//----------------------------------------------------------------------------
//	Codec
//----------------------------------------------------------------------------

/**
**	Empty log callback
*/
static void CodecNoopCallback( __attribute__ ((unused))
    void *ptr, __attribute__ ((unused))
    int level, __attribute__ ((unused))
    const char *fmt, __attribute__ ((unused)) va_list vl)
{
}

/**
**	log callback
*/
static void CodecLogCallback( __attribute__ ((unused))
    void *ptr, __attribute__ ((unused))
    int level, __attribute__ ((unused))
    const char *fmt, va_list vl)
{
	char format[256];
	char prefix[20] = "";
	pid_t threadId = syscall(__NR_gettid);

	strcpy(prefix, "[FFMpeg]");
	snprintf(format, sizeof(format), "[%d] [softhddevice]%s %s", threadId, prefix, fmt);

//	va_start(vl, fmt);
	vsyslog(level, format, vl);
//	va_end(vl);
}

/**
**	Codec init
*/
void CodecInit(void)
{
#ifdef FFMPEG_DEBUG
	av_log_set_level(AV_LOG_DEBUG);
//	av_log_set_level(AV_LOG_ERROR );
	av_log_set_callback(CodecLogCallback);
#else
	av_log_set_callback(CodecNoopCallback);
#endif

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,18,100)
	avcodec_register_all();		// register all formats and codecs
#endif
	pthread_mutex_init(&CodecLockMutex, NULL);
}

/**
**	Codec exit.
*/
void CodecExit(void)
{
	pthread_mutex_destroy(&CodecLockMutex);
}
