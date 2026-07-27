// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file codec_video.cpp
 * Video Decoder
 *
 * This file defines cVideoDecoder, which has all the functions
 * to decode video data. It's the video interface to ffmpeg.
 *
 * @copyright 2009 - 2015 by Johns.  All Rights Reserved.
 * @copyright 2018 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
//#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include "codec_video.h"
#include "logger.h"
#include "misc.h"

//#define NUM_CAPTURE_BUFFERS 10
//#define NUM_OUTPUT_BUFFERS 10

/********************************************************************************
 * Static functions
 *******************************************************************************/

/**
 * Callback to negotiate the PixelFormat
 *
 * @param videoCtx      video codec context
 * @param fmt           the list of formats which are supported by
 *                      the codec, it is terminated by -1 as 0 is a
 *                      valid format, the formats are ordered by quality
 *
 * @return              the negotiated pixel format
 *
 * @ingroup videodecoder
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
 *
 * @ingroup videodecoder
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

	return NULL;
}

/**
 * Find a suitable video codec (hardware decoding)
 *
 * @param codecId                 video codec id
 *
 * @return                        AVCodec if found, NULL otherwise
 *
 * @ingroup videodecoder
 */
static const AVCodec *FindHWDecoder(enum AVCodecID codecId)
{
	const AVCodec *codec;
	void *i = 0;

	while ((codec = av_codec_iterate(&i))) {
		if (!av_codec_is_decoder(codec))
			continue;
		if (codec->id != codecId)
			continue;

		const AVCodecHWConfig *config = FindHWConfig(codec);
		if (config)
			return codec;
	}

	return NULL;
}

/**
 * Find a suitable video codec (software decoding)
 *
 * @param codecId                 video codec id
 *
 * @return                        AVCodec if found, NULL otherwise
 *
 * @ingroup videodecoder
 */
static const AVCodec *FindSWDecoder(enum AVCodecID codecId)
{
	return avcodec_find_decoder(codecId);
}

/**
 * Create a new video decoder
 *
 * @param identifier         string to identify decoder for video or pip stream
 *                           (used within logging only)
 */
cVideoDecoder::cVideoDecoder(const char *identifier)
	: m_identifier(identifier)
{
	av_log_set_callback(cSoftHdLogger::LogFFmpegCallback);

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,18,100)
	avcodec_register_all();		// register all formats and codecs
#endif
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
 * @retval 0                       decoder successfully opend
 * @retval -1                      opening the decoder failed
 */
int cVideoDecoder::Open(enum AVCodecID codecId, AVCodecParameters * par,
                        AVRational timebase, bool forceSoftwareDecoder,
                        int width, int height)
{
	std::unique_lock<std::mutex> lock(m_mutex);

	if (m_pVideoCtx != nullptr)
		return 0;

	const AVCodec *codec = nullptr;
	m_isHardwareDecoder = false;

	LOGDEBUG2(L_CODEC, "videocodec: %s: %s: Try to open decoder for codec \"%s\"%s", m_identifier, __FUNCTION__,
		avcodec_get_name(codecId), forceSoftwareDecoder ? " (sw decoding forced)" : "");

	if (!forceSoftwareDecoder)
		codec = FindHWDecoder(codecId);

	if (codec) {
		m_isHardwareDecoder = true;
	} else {
		if (!forceSoftwareDecoder)
			LOGDEBUG2(L_CODEC, "videocodec: %s: no HW decoder found for codec \"%s\", try software decoder%s", __FUNCTION__, avcodec_get_name(codecId), forceSoftwareDecoder ? " (forced)" : "");
		codec = FindSWDecoder(codecId);
	}

	if (!codec) {
		LOGERROR("videocodec: %s: %s: Could not find any decoder for codec \"%s\"!", m_identifier, __FUNCTION__, avcodec_get_name(codecId));
		return -1;
	}

	m_pVideoCtx = avcodec_alloc_context3(codec);
	if (!m_pVideoCtx) {
		LOGERROR("videocodec: %s: %s: can't alloc codec context!", m_identifier, __FUNCTION__);
		return -1;
	}

	const AVCodecHWConfig *config = m_isHardwareDecoder ? FindHWConfig(codec) : NULL;
	static AVBufferRef *hwDeviceCtx = NULL;

	if (config && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
		const char *type_name = av_hwdevice_get_type_name(config->device_type);
		if (av_hwdevice_ctx_create(&hwDeviceCtx, config->device_type, NULL, NULL, 0) < 0) {
			avcodec_free_context(&m_pVideoCtx);
			LOGERROR("videocodec: %s: %s: Error creating HW context %s", m_identifier, __FUNCTION__,
				type_name ? type_name : "unknown");
			return -1;
		}
		m_pVideoCtx->hw_device_ctx = hwDeviceCtx;
		m_pVideoCtx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
	}

	if (par && avcodec_parameters_to_context(m_pVideoCtx, par) < 0)
		LOGERROR("videocodec: %s: %s: insert parameters to context failed!", m_identifier, __FUNCTION__);

	m_pVideoCtx->codec_id = codecId;
	m_pVideoCtx->get_format = GetFormat;
	m_pVideoCtx->opaque = this;
	m_pVideoCtx->pkt_timebase.num = 1;
	m_pVideoCtx->pkt_timebase.den = 90000;

	if (av_q2d(timebase) > 0)
		m_pVideoCtx->pkt_timebase = timebase;

	if (codecId == AV_CODEC_ID_H264) {
		if (par) {
			m_pVideoCtx->coded_width = par->width;
			m_pVideoCtx->coded_height = par->height;
			m_pVideoCtx->width = par->width;
			m_pVideoCtx->height = par->height;
			LOGDEBUG2(L_CODEC, "videocodec: %s: %s: Set width %d and height %d from par", m_identifier, __FUNCTION__, par->width, par->height);
		} else if (width && height) {
			m_pVideoCtx->coded_width = width;
			m_pVideoCtx->coded_height = height;
			m_pVideoCtx->width = width;
			m_pVideoCtx->height = height;
			LOGDEBUG2(L_CODEC, "videocodec: %s: %s: Set width %d and height %d forced", m_identifier, __FUNCTION__, width, height);
		}
	}

	if (codec->capabilities & (AV_CODEC_CAP_FRAME_THREADS | AV_CODEC_CAP_SLICE_THREADS))
		m_pVideoCtx->thread_count = !m_isHardwareDecoder ? 4 : 1;

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
		if (!m_isHardwareDecoder) {
			LOGERROR("videocodec: %s: %s: Error opening the decoder: %s", m_identifier, __FUNCTION__, av_err2str(err));
			return -1;
		}
		LOGDEBUG2(L_CODEC, "videocodec: %s: %s: Could not open hw decoder \"%s\", force using software decoder",
			m_identifier, __FUNCTION__, codec->long_name ? codec->long_name : codec->name);

		// unlock here, otherwise we run into a deadlock
		lock.unlock();
		return Open(codecId, par, timebase, true, 0, 0);
	}

	LOGINFO("videocodec: %s: %s (%s) for codec \"%s\" opened%s, using %s decoding with %d threads%s",
		m_identifier,
		codec->long_name ? codec->long_name : codec->name,
		codec->name,
		avcodec_get_name(codecId),
		forceSoftwareDecoder ? " (sw decoding forced)" : "",
		m_isHardwareDecoder ? "hardware" : "software",
		m_pVideoCtx->thread_count,
		m_isHardwareDecoder ? " 🤩" : "");

	m_pCodecString = codec->long_name ? codec->long_name : codec->name;
	m_cntPacketsSent = m_cntFramesReceived = 0;
	m_cntStartKeyFrames = 1;

	return 0;
}

/**
 * Close video decoder
 */
void cVideoDecoder::Close(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_pVideoCtx != nullptr) {
		LOGDEBUG2(L_CODEC, "videocodec: %s: %s: m_pVideoCtx %p", m_identifier, __FUNCTION__, m_pVideoCtx);
		m_lastCodedWidth = m_pVideoCtx->coded_width;
		m_lastCodedHeight = m_pVideoCtx->coded_height;
		avcodec_free_context(&m_pVideoCtx);
		m_pVideoCtx = nullptr;
	}
	m_cntPacketsSent = m_cntFramesReceived = 0;
}

/**
 * Get extradata from avpkt
 *
 * @param avpkt    video packet
 *
 * @retval 0       extradata set
 * @retval -1      something went wrong
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
		LOGERROR("videocodec: %s: %s: extradata av_bsf_get_by_name failed!", m_identifier, __FUNCTION__);
		return -1;
	}

	ret = av_bsf_alloc(f, &bsfCtx);
	if (ret < 0) {
		LOGERROR("videocodec: %s: %s: extradata av_bsf_alloc failed!", m_identifier, __FUNCTION__);
		return ret;
	}

	bsfCtx->par_in->codec_id = m_pVideoCtx->codec_id;

	ret = av_bsf_init(bsfCtx);
	if (ret < 0) {
		LOGERROR("videocodec: %s: %s: extradata av_bsf_init failed!", m_identifier, __FUNCTION__);
		av_bsf_free(&bsfCtx);
		return ret;
	}

	AVPacket *dstPkt = av_packet_alloc();
	AVPacket *pktRef = dstPkt;

	if (!dstPkt) {
		LOGERROR("videocodec: %s: %s: extradata av_packet_alloc failed!", m_identifier, __FUNCTION__);
		av_bsf_free(&bsfCtx);
		return -1;
	}

	ret = av_packet_ref(pktRef, avpkt);
	if (ret < 0) {
		LOGERROR("videocodec: %s: %s: extradata av_packet_ref failed!", m_identifier, __FUNCTION__);
		av_packet_free(&dstPkt);
		av_bsf_free(&bsfCtx);
		return ret;
	}

	ret = av_bsf_send_packet(bsfCtx, pktRef);
	if (ret < 0) {
		LOGERROR("videocodec: %s: %s: extradata av_bsf_send_packet failed!", m_identifier, __FUNCTION__);
		av_packet_unref(pktRef);
		av_packet_free(&dstPkt);
		av_bsf_free(&bsfCtx);
		return ret;
	}

	ret = av_bsf_receive_packet(bsfCtx, pktRef);
	if (ret < 0) {
		LOGERROR("videocodec: %s: %s: extradata av_bsf_receive_packet failed!", m_identifier, __FUNCTION__);
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
 * @retval 0                     packet was sent
 * @retval AVERROR(EAGAIN)       packet was not accepted, first receive frame and send packet again
 * @retval AVERROR(EINVAL)       invalid input or missing m_pVideoCtx
 * @retval ret                   other ffmpeg error
 */
int cVideoDecoder::SendPacket(const AVPacket *avpkt)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	int ret = 0;

	if (m_pVideoCtx == nullptr)
		return AVERROR(EINVAL);

	// force a flush, if avpkt is NULL, this initiates a decoder drain
	if (!avpkt) {
		LOGDEBUG2(L_CODEC, "videocodec: %s: %s: send NULL packet, flush requested", m_identifier, __FUNCTION__);
		avcodec_send_packet(m_pVideoCtx, NULL);
		return 0;
	}

	if (!avpkt->size)
		return AVERROR(EINVAL);

	// get extradata, if not yet done
	if (!m_pVideoCtx->extradata_size) {
		if (!GetExtraData(avpkt))
			LOGDEBUG2(L_CODEC, "videocodec: %s: %s: set extradata %p %d", m_identifier, __FUNCTION__, m_pVideoCtx->extradata, m_pVideoCtx->extradata_size);
	}

	ret = avcodec_send_packet(m_pVideoCtx, avpkt);
	if (ret) {
		if (ret != AVERROR(EAGAIN))
			LOGDEBUG2(L_CODEC, "videocodec: %s: %s: send_packet ret: %s", m_identifier, __FUNCTION__, av_err2str(ret));
		return ret;
	}

	m_cntPacketsSent++;
	LOGDEBUG2(L_PACKET, "videocodec: %s: %s:   %6d PTS %s <<---", m_identifier, __FUNCTION__, m_cntPacketsSent, Timestamp2String(avpkt->pts, 90));

	return 0;
}

/**
 * Receive a decoded a video frame
 *
 * @param[out] frame            decoded AVFrame
 * @param instance              instance name for logging
 *
 * @retval 0                    received frame
 * @retval AVERROR(EAGAIN)      get no frame, send avpkt again
 * @retval AVERROR_EOF          EOF, needs flushing
 * @retval AVERROR(EINVAL)      get no frame, something went wrong
 * @retval ret                  return other ffmpeg error
 */
int cVideoDecoder::ReceiveFrame(AVFrame **frame)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	int ret;
	AVFrame *pFrame;

	if (m_pVideoCtx == nullptr)
		return AVERROR(EINVAL);

	if (!(pFrame = av_frame_alloc()))
		LOGFATAL("videocodec: %s: %s: can't allocate decoder frame", m_identifier, __FUNCTION__);

	ret = avcodec_receive_frame(m_pVideoCtx, pFrame);

	if (ret) {
		if (ret == AVERROR_EOF)
			LOGDEBUG2(L_CODEC, "videocodec: %s: %s: receive_frame ret: AVERROR_EOF", m_identifier, __FUNCTION__);
		else if (ret != AVERROR(EAGAIN))
			LOGDEBUG2(L_CODEC, "videocodec: %s: %s: receive_frame ret: %s", m_identifier, __FUNCTION__, av_err2str(ret));
		av_frame_free(&pFrame);
		return ret;
	}

	if (pFrame->flags == AV_FRAME_FLAG_CORRUPT)
		LOGDEBUG2(L_CODEC, "videocodec: %s: %s: AV_FRAME_FLAG_CORRUPT", m_identifier, __FUNCTION__);

	// Codec artifacts workaround for amlogic H264:
	// Skip m_skipKeyFramesNum Key-Frames at stream start.
	// m_skipKeyFramesNum can be set with SetSkipKeyFramesNum()
	if (m_pVideoCtx->codec_id == AV_CODEC_ID_H264 && m_skipKeyFramesNum && m_cntStartKeyFrames) {
		if (IsKeyFrame(pFrame)) {
			LOGDEBUG2(L_CODEC, "videocodec: %s: %s: artifact workaround - skip %s Keyframe nr %d", m_identifier, __FUNCTION__,
				isInterlacedFrame(pFrame) ? "interlaced" : "progressive", m_cntStartKeyFrames);

			if (m_cntStartKeyFrames++ > m_skipKeyFramesNum - 1)
				m_cntStartKeyFrames = 0;
		}

		av_frame_free(&pFrame);
		return AVERROR(EAGAIN);
	}

	*frame = pFrame;

	m_cntFramesReceived++;
	LOGDEBUG2(L_PACKET, "videocodec: %s: %s: %6d PTS %s --->> (%2d)%s", m_identifier, __FUNCTION__,
		m_cntFramesReceived, Timestamp2String(pFrame->pts, 90), m_cntPacketsSent - m_cntFramesReceived,
		isInterlacedFrame(pFrame) ? " I" : "");

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
 * @retval 0                      success
 * @retval -1                     reopen decoder failed
 *
 * @todo
 * This is just a temporary implementation
 * RPi's ffmpeg decoder is broken. In order to get the same result if
 * we want to flush the decoder, we need to close and reopen it.
 * This function is only needed, if some decoder can't flush correctly.
 * Once this is fixed in ffmpeg, we can drop this function.
 * remove, once ffmpeg is fixed
 */
int cVideoDecoder::ReopenCodec(enum AVCodecID codecId, AVCodecParameters *par,
                               AVRational timebase, int forceSoftwareDecoding)
{
	LOGDEBUG2(L_CODEC, "videocodec: %s: %s: m_pVideoCtx %p", m_identifier, __FUNCTION__, m_pVideoCtx);
	Close();
	if (Open(codecId, par, timebase, forceSoftwareDecoding, m_lastCodedWidth, m_lastCodedHeight))
		return -1;
	m_cntStartKeyFrames = 0; // currently unused, because we have no hardware which needs both quirks
	m_cntPacketsSent = m_cntFramesReceived = 0;

	return 0;
}

/**
 * Flush the video decoder buffers
 *
 * Also reset packet sent/ frame received counter
 */
void cVideoDecoder::FlushBuffers(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	LOGDEBUG2(L_CODEC, "videocodec: %s: %s: m_pVideoCtx %p", m_identifier, __FUNCTION__, m_pVideoCtx);

	if (m_pVideoCtx)
		avcodec_flush_buffers(m_pVideoCtx);

	m_cntPacketsSent = m_cntFramesReceived = 0;
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
