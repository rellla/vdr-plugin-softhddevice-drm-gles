/**
 * @file codec_video.cpp
 * Video decoder class
 *
 * This file defines cVideoDecoder, which has all the functions
 * to decode video data. It's the video interface to ffmpeg.
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

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <cassert>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include "misc.h"

#include <vdr/thread.h>

#include "buf2rgb.h"
#include "codec_video.h"
#include "videorender.h"
#include "logger.h"

#define NUM_CAPTURE_BUFFERS 10
#define NUM_OUTPUT_BUFFERS 10

#define AV_LOGLEVEL AV_LOG_TRACE

/******************************************************************************
 * static functions
 *****************************************************************************/

/**
 * Logging callback, used for ffmpeg logging
 */
#ifdef FFMPEG_DEBUG
static void CodecLogCallback(__attribute__ ((unused)) void *ptr,
                             __attribute__ ((unused)) int level,
                             __attribute__ ((unused)) const char *fmt,
                             va_list vl)
{
	av_log_set_level(AV_LOG_INFO);

	if (level > AV_LOGLEVEL)
		return;

	char format[256];
	char prefix[20] = "";
	pid_t threadId = syscall(__NR_gettid);

	strcpy(prefix, "[FFMpeg]");
	snprintf(format, sizeof(format), "[%d] [softhddevice]%s %s", threadId, prefix, fmt);

	vsyslog(LOG_INFO, format, vl);
}
#else
static void CodecLogCallback(__attribute__ ((unused)) void *ptr,
                             __attribute__ ((unused)) int level,
                             __attribute__ ((unused)) const char *fmt,
                             __attribute__ ((unused)) va_list vl)
{
}
#endif

/**
 * Callback to negotiate the PixelFormat
 *
 * @param videoCtx      video codec context
 * @param fmt           the list of formats which are supported by
 *                      the codec, it is terminated by -1 as 0 is a
 *                      valid format, the formats are ordered by quality
 */
static enum AVPixelFormat GetFormat(AVCodecContext * videoCtx,
                                    const enum AVPixelFormat *fmt)
{
	while (*fmt != AV_PIX_FMT_NONE) {
		LOGDEBUG2(L_CODEC, "videocodec: %s: PixelFormat: %s videoCtx->pix_fmt: %s sw_pix_fmt: %s Codecname: %s",
			__FUNCTION__,
			av_get_pix_fmt_name(*fmt), av_get_pix_fmt_name(videoCtx->pix_fmt),
			av_get_pix_fmt_name(videoCtx->sw_pix_fmt), videoCtx->codec->name);
		if (*fmt == AV_PIX_FMT_DRM_PRIME) {
			return AV_PIX_FMT_DRM_PRIME;
		}

		if (*fmt == AV_PIX_FMT_YUV420P) {
			return AV_PIX_FMT_YUV420P;
		}
		fmt++;
	}
	LOGWARNING("videocodec: %s: No pixel format found! Set default format.", __FUNCTION__);

	return avcodec_default_get_format(videoCtx, fmt);
}

/**
 * Find a hardware based video decoder config
 *
 * @param codec    codec for which we should find a hw config
 *
 * @return         AVCodecHWConfig if found, NULL otherwise
 */
static const AVCodecHWConfig *FindHWConfig(const AVCodec *codec)
{
	const AVCodecHWConfig *config = NULL;
	for (int n = 0; (config = avcodec_get_hw_config(codec, n)); n++)
	{
		if (!(config->pix_fmt == AV_PIX_FMT_DRM_PRIME))
			continue;

		if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) ||
			(config->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL))
			return config;
	}

	LOGDEBUG2(L_CODEC, "videocodec: %s: no HW config found for %s", __FUNCTION__, codec->long_name ? codec->long_name : codec->name);
	return NULL;
}

/**
 * Find a suitable video codec
 *
 * @param codecId                 video codec id
 * @param forceSoftwareDecoder    force software decoding is set, otherwise prefer hardware decoder if available
 *
 * @return                        AVCodec if found, NULL otherwise
 */

static const AVCodec *FindDecoder(enum AVCodecID codecId, int forceSoftwareDecoder)
{
	const AVCodec *codec;
	void *i = 0;

	if (!forceSoftwareDecoder) {
		while ((codec = av_codec_iterate(&i))) {
			if (!av_codec_is_decoder(codec))
				continue;
			if (codec->id != codecId)
				continue;

			const AVCodecHWConfig *config = FindHWConfig(codec);
			if (config)
				return codec;
		}
	}

	codec = avcodec_find_decoder(codecId);
	if (codec)
		return codec;

	LOGWARNING("videocodec: %s: no decoder found", __FUNCTION__);
	return NULL;
}

/******************************************************************************
 * cVideoDecoder class
 *****************************************************************************/

/**
 * cVideoDecoder constructor
 *
 * @param hardwareQuirks     hardware specific quirks for decoder
 */
cVideoDecoder::cVideoDecoder(int hardwareQuirks)
{
	m_hardwareQuirks = hardwareQuirks;
	m_pVideoCtx = nullptr;

	av_log_set_callback(CodecLogCallback);

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,18,100)
	avcodec_register_all();		// register all formats and codecs
#endif
}

/**
 * cVideoDecoder destructor
 */
cVideoDecoder::~cVideoDecoder(void)
{
}

/**
 * Open the video decoder
 *
 * @param codecId                  video codec id
 * @param par                      codec parameters
 * @param timebase                 timebase
 * @param forceSoftwareDecoder     force software decoding
 * @param width                    force width (only for H264 and if par is not set)
 * @param height                   force height (only for H264 and if par is not set)
 *
 * @returns 0                      decoder successfully opend
 * @returns -1                     opening the decoder failed
 */
int cVideoDecoder::Open(enum AVCodecID codecId, AVCodecParameters * par,
                        AVRational * timebase, int forceSoftwareDecoder,
                        int width, int height)
{
	m_mutex.Lock();
	if (m_pVideoCtx != nullptr) {
		m_mutex.Unlock();
		return 0;
	}

	int swcodec = forceSoftwareDecoder;

	if ((m_hardwareQuirks & QUIRK_CODEC_DISABLE_MPEG_HW && codecId == AV_CODEC_ID_MPEG2VIDEO))
		swcodec = 1;
	if ((m_hardwareQuirks & QUIRK_CODEC_DISABLE_H264_HW && codecId == AV_CODEC_ID_H264))
		swcodec = 1;

	LOGDEBUG2(L_CODEC, "videocodec: %s: Try to open decoder for CodecID %s%s", __FUNCTION__,
		avcodec_get_name(codecId), swcodec ? " (sw decoding forced)" : "");

	const AVCodec *codec = FindDecoder(codecId, swcodec);
	if (!codec) {
		LOGERROR("videocodec: %s: Could not find any decoder for codec %s!", __FUNCTION__, avcodec_get_name(codecId));
		m_mutex.Unlock();
		return -1;
	}

	LOGDEBUG2(L_CODEC, "videocodec: %s: Codec %s for CodecID %s found%s", __FUNCTION__,
		codec->long_name ? codec->long_name : codec->name,
		avcodec_get_name(codecId), swcodec ? " (sw decoding forced)" : "");

	m_pVideoCtx = avcodec_alloc_context3(codec);
	if (!m_pVideoCtx) {
		LOGERROR("videocodec: %s: can't alloc codec context!", __FUNCTION__);
		m_mutex.Unlock();
		return -1;
	}

	const AVCodecHWConfig *config = !swcodec ? FindHWConfig(codec) : NULL;
	static AVBufferRef *hwDeviceCtx = NULL;

	if (config && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
		const char *type_name = av_hwdevice_get_type_name(config->device_type);
		if (av_hwdevice_ctx_create(&hwDeviceCtx, config->device_type, NULL, NULL, 0) < 0) {
			avcodec_free_context(&m_pVideoCtx);
			LOGERROR("videocodec: %s: Error creating HW context %s",__FUNCTION__,
				type_name ? type_name : "unknown");
			m_mutex.Unlock();
			return -1;
		}
		LOGINFO("videocodec: Using %s hardware video acceleration 🤩", type_name ? type_name : "unknown");
		m_pVideoCtx->hw_device_ctx = hwDeviceCtx;
		m_pVideoCtx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
	}

	if (par) {
		if ((avcodec_parameters_to_context(m_pVideoCtx, par)) < 0)
			LOGERROR("videocodec: %s: insert parameters to context failed!", __FUNCTION__);
	}

	m_pVideoCtx->codec_id = codecId;
	m_pVideoCtx->get_format = GetFormat;
	m_pVideoCtx->opaque = this;
	m_pVideoCtx->pkt_timebase.num = 1;
	m_pVideoCtx->pkt_timebase.den = 90000;

	if (timebase) {
		m_pVideoCtx->pkt_timebase.num = timebase->num;
		m_pVideoCtx->pkt_timebase.den = timebase->den;
	}

	// amlogic h264 decoder needs this
	if (codecId == AV_CODEC_ID_H264) {
		if (par) {
			m_pVideoCtx->coded_width = par->width;
			m_pVideoCtx->coded_height = par->height;
			m_pVideoCtx->width = par->width;
			m_pVideoCtx->height = par->height;
			LOGDEBUG2(L_CODEC, "videocodec: %s: Set width %d and height %d from par", __FUNCTION__, par->width, par->height);
		} else if (width && height) {
			m_pVideoCtx->coded_width = width;
			m_pVideoCtx->coded_height = height;
			m_pVideoCtx->width = width;
			m_pVideoCtx->height = height;
			LOGDEBUG2(L_CODEC, "videocodec: %s: Set width %d and height %d forced", __FUNCTION__, width, height);
		}
	}

	if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS ||
		AV_CODEC_CAP_SLICE_THREADS) {
		m_pVideoCtx->thread_count = swcodec ? 4 : 1;
	}

	if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
		m_pVideoCtx->thread_type = FF_THREAD_SLICE;

/*
	if (strstr(codec->name, "_v4l2")) {
		if (av_opt_set_int(m_pVideoCtx->priv_data, "num_capture_buffers", NUM_CAPTURE_BUFFERS, 0) < 0) {
			LOGERROR("videocodec: %s: can't set %d num_capture_buffers", __FUNCTION__, NUM_CAPTURE_BUFFERS);
		}
		LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: set num_capture_buffers %d", NUM_CAPTURE_BUFFERS);
		if (av_opt_set_int(m_pVideoCtx->priv_data, "num_output_buffers", NUM_OUTPUT_BUFFERS, 0) < 0) {
			LOGERROR("videocodec: %s: can't set %d num_output_buffers", __FUNCTION__, NUM_OUTPUT_BUFFERS);
		}
		LOGDEBUG2(L_CODEC, "videocodec: %s: set num_output_buffers %d", __FUNCTION__, NUM_OUTPUT_BUFFERS);
	}
*/
	int err = avcodec_open2(m_pVideoCtx, m_pVideoCtx->codec, NULL);
	if (err < 0) {
		avcodec_free_context(&m_pVideoCtx);
		if (forceSoftwareDecoder) {
			LOGERROR("videocodec: %s: Error opening the decoder: %s", __FUNCTION__, av_err2str(err));
			m_mutex.Unlock();
			return -1;
		}
		LOGDEBUG2(L_CODEC, "videocodec: %s: Could not open %s decoder, try opening software decoder",
			__FUNCTION__, codec->long_name ? codec->long_name : codec->name);

		m_mutex.Unlock();
		return Open(codecId, par, timebase, 1, 0, 0);
	}

	LOGDEBUG2(L_CODEC, "videocodec: %s: Codec %s for CodecID %s opened%s, using %d threads",
		__FUNCTION__,
		codec->long_name ? codec->long_name : codec->name,
		avcodec_get_name(codecId),
		swcodec ? " (sw decoding forced)" : "",
		m_pVideoCtx->thread_count);

	m_cntPacketsSent = m_cntFramesReceived = 0;
	m_cntStartKeyFrames = 1;
	m_mutex.Unlock();
	return 0;
}

/**
 * Close video decoder
 */
void cVideoDecoder::Close(void)
{
	m_mutex.Lock();
	if (m_pVideoCtx != nullptr) {
		LOGDEBUG2(L_CODEC, "videocodec: %s: m_pVideoCtx %p", __FUNCTION__, m_pVideoCtx);
		m_lastCodedWidth = m_pVideoCtx->coded_width;
		m_lastCodedHeight = m_pVideoCtx->coded_height;
		avcodec_free_context(&m_pVideoCtx);
		m_pVideoCtx = nullptr;
	}
	m_mutex.Unlock();
	m_cntPacketsSent = m_cntFramesReceived = 0;
}

/**
 * Get extradata from avpkt
 *
 * @param avpkt	video packet
 *
 * @returns 0      extradata set
 * @returns -1     something went wrong
 */
int cVideoDecoder::GetExtraData(const AVPacket * avpkt)
{
	AVBSFContext *bsfCtx;
	const AVBitStreamFilter *f;
	size_t extradataSize;
	uint8_t *extradata;
	int ret = 0;

	f = av_bsf_get_by_name("extract_extradata");
	if (!f) {
		LOGERROR("videocodec: %s: extradata av_bsf_get_by_name failed!", __FUNCTION__);
		return -1;
	}

	ret = av_bsf_alloc(f, &bsfCtx);
	if (ret < 0) {
		LOGERROR("videocodec: %s: extradata av_bsf_alloc failed!", __FUNCTION__);
		return ret;
	}

	bsfCtx->par_in->codec_id = m_pVideoCtx->codec_id;

	ret = av_bsf_init(bsfCtx);
	if (ret < 0) {
		LOGERROR("videocodec: %s: extradata av_bsf_init failed!", __FUNCTION__);
		av_bsf_free(&bsfCtx);
		return ret;
	}

	AVPacket *dstPkt = av_packet_alloc();
	AVPacket *pktRef = dstPkt;

	if (!dstPkt) {
		LOGERROR("videocodec: %s: extradata av_packet_alloc failed!", __FUNCTION__);
		av_bsf_free(&bsfCtx);
		return -1;
	}

	ret = av_packet_ref(pktRef, avpkt);
	if (ret < 0) {
		LOGERROR("videocodec: %s: extradata av_packet_ref failed!", __FUNCTION__);
		av_packet_free(&dstPkt);
		av_bsf_free(&bsfCtx);
		return ret;
	}

	ret = av_bsf_send_packet(bsfCtx, pktRef);
	if (ret < 0) {
		LOGERROR("videocodec: %s: extradata av_bsf_send_packet failed!", __FUNCTION__);
		av_packet_unref(pktRef);
		av_packet_free(&dstPkt);
		av_bsf_free(&bsfCtx);
		return ret;
	}

	ret = av_bsf_receive_packet(bsfCtx, pktRef);
	if (ret < 0) {
		LOGERROR("videocodec: %s: extradata av_bsf_send_packet failed!", __FUNCTION__);
		av_packet_unref(pktRef);
		av_packet_free(&dstPkt);
		av_bsf_free(&bsfCtx);
		return ret;
	}

	extradata = av_packet_get_side_data(pktRef, AV_PKT_DATA_NEW_EXTRADATA, &extradataSize);

	m_pVideoCtx->extradata = (uint8_t *)av_mallocz(extradataSize + AV_INPUT_BUFFER_PADDING_SIZE);
	memcpy(m_pVideoCtx->extradata, extradata, extradataSize);
	m_pVideoCtx->extradata_size = extradataSize;

	av_packet_unref(pktRef);
	av_packet_free(&dstPkt);
	av_bsf_free(&bsfCtx);
	return ret;
}

/**
 * Send a video packet to be decoded
 *
 * @param avpkt                  video packet
 *
 * @returns 0                    packet was sent
 * @returns AVERROR(EAGAIN)      packet was not accepted, first receive frame and send packet again
 * @returns AVERROR(EINVAL)      invalid input or missing m_pVideoCtx
 * @returns ret                  other ffmpeg error
 */
int cVideoDecoder::SendPacket(const AVPacket *avpkt)
{
	int ret = 0;

	m_mutex.Lock();
	if (m_pVideoCtx == nullptr) {
		m_mutex.Unlock();
		return AVERROR(EINVAL);
	}

	// force a flush, if avpkt is NULL, this initiates a decoder drain
	if (!avpkt) {
		LOGDEBUG2(L_CODEC, "videocodec: %s: send NULL packet, flush reqeusted", __FUNCTION__);
		avcodec_send_packet(m_pVideoCtx, NULL);
		m_mutex.Unlock();
		return 0;
	}

	if (!avpkt->size) {
		m_mutex.Unlock();
		return AVERROR(EINVAL);
	}

	// get extradata, if not yet done
	if (!m_pVideoCtx->extradata_size) {
		if (!GetExtraData(avpkt))
			LOGDEBUG2(L_CODEC, "videocodec: %s: set extradata %p %d", __FUNCTION__, m_pVideoCtx->extradata, m_pVideoCtx->extradata_size);
	}

	ret = avcodec_send_packet(m_pVideoCtx, avpkt);
	if (ret) {
		if (ret != AVERROR(EAGAIN))
			LOGDEBUG2(L_CODEC, "videocodec: %s: send_packet ret: %s", __FUNCTION__, av_err2str(ret));
		m_mutex.Unlock();
		return ret;
	}

	m_cntPacketsSent++;
	LOGDEBUG2(L_PACKET, "videocodec: %s:   %6d PTS %s <<---", __FUNCTION__, m_cntPacketsSent, Timestamp2String(avpkt->pts, 90));
	m_mutex.Unlock();
	return 0;
}

/**
 * Receive a decoded a video frame
 *
 * @param[out] frame            decoded AVFrame
 * @param[out] width            coded_width
 * @param[out] height           coded_height
 *
 * @returns 0                   received frame
 * @returns AVERROR(EAGAIN)     get no frame, send avpkt again
 * @returns AVERROR_EOF         EOF, needs flushing
 * @returns AVERROR(EINVAL)     get no frame, something went wrong
 * @returns ret                 return other ffmpeg error
 */
int cVideoDecoder::ReceiveFrame(AVFrame **frame, int &width, int &height)
{
	int ret;
	AVFrame *pFrame;

	m_mutex.Lock();
	if (m_pVideoCtx == nullptr) {
		m_mutex.Unlock();
		return AVERROR(EINVAL);
	}

	if (!(pFrame = av_frame_alloc())) {
		m_mutex.Unlock();
		LOGFATAL("videocodec: %s: can't allocate decoder frame", __FUNCTION__);
	}

	ret = avcodec_receive_frame(m_pVideoCtx, pFrame);

	if (ret) {
		if (ret == AVERROR_EOF)
			LOGDEBUG2(L_CODEC, "videocodec: %s: receive_frame ret: AVERROR_EOF", __FUNCTION__);
		else if (ret != AVERROR(EAGAIN))
			LOGDEBUG2(L_CODEC, "videocodec: %s: receive_frame ret: %s", __FUNCTION__, av_err2str(ret));
		av_frame_free(&pFrame);
		m_mutex.Unlock();
		return ret;
	}

	if (pFrame->flags == AV_FRAME_FLAG_CORRUPT)
		LOGDEBUG2(L_CODEC, "videocodec: %s: AV_FRAME_FLAG_CORRUPT", __FUNCTION__);

	// codec artifacts workaround for amlogic H264:
	// skip QUIRK_CODEC_SKIP_NUM_FRAMES key frames
	if (m_pVideoCtx->codec_id == AV_CODEC_ID_H264 &&
	   (m_hardwareQuirks & QUIRK_CODEC_SKIP_FIRST_FRAMES) && m_cntStartKeyFrames) {
		if (IsKeyFrame(pFrame)) {
			LOGDEBUG2(L_CODEC, "videocodec: %s: artifact workaround - skip %s I-frame nr %d", __FUNCTION__,
				IsInterlacedFrame(pFrame) ? "interlaced" : "progressive", m_cntStartKeyFrames);

			if (m_cntStartKeyFrames++ > QUIRK_CODEC_SKIP_NUM_FRAMES - 1)
				m_cntStartKeyFrames = 0;
		}

		av_frame_free(&pFrame);
		m_mutex.Unlock();
		return AVERROR(EAGAIN);
	}

	width = m_pVideoCtx->coded_width;
	height = m_pVideoCtx->coded_height;

	*frame = pFrame;

	m_cntFramesReceived++;
	LOGDEBUG2(L_PACKET, "videocodec: %s: %6d PTS %s --->> (%2d)%s", __FUNCTION__,
		m_cntFramesReceived, Timestamp2String(pFrame->pts, 90), m_cntPacketsSent - m_cntFramesReceived,
		IsInterlacedFrame(pFrame) ? " I" : "");
	m_mutex.Unlock();
	return 0;
}

/**
 * Reopen the video decoder
 *
 * @param codecId                 video codec id
 * @param par                     codec parameters
 * @param timebase                timebase
 * @param forceSoftwareDecoding   force software decoding
 *
 * @returns 0                     success
 * @returns -1                    reopen decoder failed
 *
 * @todo:
 * This is just a temporary implementation
 * RPi's ffmpeg decoder is broken. In order to get the same result if
 * we want to flush the decoder, we need to close and reopen it.
 * This function is only needed, if some decoder can't flush correctly.
 * Once this is fixed in ffmpeg, we can drop this function.
 * remove, once ffmpeg is fixed
 */
int cVideoDecoder::ReopenCodec(enum AVCodecID codecId, AVCodecParameters *par,
                               AVRational *timebase, int forceSoftwareDecoding)
{
	LOGDEBUG2(L_CODEC, "videocodec: %s: m_pVideoCtx %p", __FUNCTION__, m_pVideoCtx);
	Close();
	if (Open(codecId, par, timebase, forceSoftwareDecoding, m_lastCodedWidth, m_lastCodedHeight))
		return -1;
	m_cntStartKeyFrames = 0; // currently unused, because we have no hardware which needs both quirks
	m_cntPacketsSent = m_cntFramesReceived = 0;

	return 0;
}

/**
 * Flush the video decoder
 */
void cVideoDecoder::FlushBuffers(void)
{
	LOGDEBUG2(L_CODEC, "videocodec: %s: m_pVideoCtx %p", __FUNCTION__, m_pVideoCtx);
	m_mutex.Lock();
	if (m_pVideoCtx)
		avcodec_flush_buffers(m_pVideoCtx);
	m_cntPacketsSent = m_cntFramesReceived = 0;
	m_mutex.Unlock();
}

/**
 * Check, if this is an interlaced frame
 *
 * @param frame    AVFrame
 *
 * @return         true, if this frame is an interlaced frame
 */
bool cVideoDecoder::IsInterlacedFrame(AVFrame *frame)
{
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
	return frame->interlaced_frame;
#else
	return frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
}

/**
 * Check, if this is a key frame
 *
 * @param frame    AVFrame
 *
 * @return         true, if this frame is a key frame
 */
bool cVideoDecoder::IsKeyFrame(AVFrame *frame)
{
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
	return frame->key_frame;
#else
	return frame->flags & AV_FRAME_FLAG_KEY;
#endif
}
