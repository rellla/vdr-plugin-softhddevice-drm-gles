// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file videostream.cpp
 * Video Input Stream
 *
 * This file defines cVideoStream, which is repsonsible for
 * handling the video stream.
 *
 * @copyright 2011 - 2015 by Johns.  All Rights Reserved.
 * @copyright 2018 - 2019 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <functional>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
}

#include <vdr/thread.h>

#include "codec_video.h"
#include "config.h"
#include "h264parser.h"
#include "hardwaredevice.h"
#include "logger.h"
#include "misc.h"
#include "queue.h"
#include "videofilter.h"
#include "videostream.h"
#include "videorender.h"

/*****************************************************************************
 * cVideoStream class
 ****************************************************************************/

/**
 * Create a video stream
 */
cVideoStream::cVideoStream(cVideoRender *render, int hardwareQuirks, cQueue<cDrmBuffer> *drmBufferQueue, cSoftHdConfig *config, bool isPipStream, std::function<void(AVFrame *)> frameOutput)
	: cThread(isPipStream ? "shd PIP decode" : "shd main decode"),
	  m_pConfig(config),
	  m_pDecoder(nullptr),
	  m_pRender(render),
	  m_identifier(isPipStream ? "PIP" : "main"),
	  m_frameOutput(frameOutput),
	  m_pDrmBufferQueue(drmBufferQueue),
	  m_videoFilter(render, m_pDrmBufferQueue, isPipStream ? "shd PIP filter" : "shd main filter", frameOutput),
	  m_hardwareQuirks(hardwareQuirks),
	  m_userDisabledDeinterlacer(config->ConfigDisableDeint),
	  m_deinterlacerDeactivated(isPipStream ? true : false),
	  m_startDecodingWithIFrame(config->ConfigDecoderNeedsIFrame),
	  m_parseH264Dimensions(config->ConfigParseH264Dimensions)
{
	m_decoderFallbackToSwNumPkts = config->ConfigDecoderFallbackToSw ? config->ConfigDecoderFallbackToSwNumPkts : 0;

	LOGDEBUG("videostream %s: %s", __FUNCTION__, m_identifier);
	if (m_decoderFallbackToSwNumPkts)
		LOGDEBUG2(L_CODEC, "videostream %s: %s: fallback to sw decoder after %d packets sent", __FUNCTION__, m_identifier, m_decoderFallbackToSwNumPkts);
}

cVideoStream::~cVideoStream(void)
{
	LOGDEBUG("videostream %s:", __FUNCTION__);
}

/**
 * Flushes the video stream by finalizing any pending data.
 *
 * This function completes processing of any remaining PES fragments in the fragmentation
 * buffer, then pushes a nullptr packet to the queue to signal a flush operation to the decoder.
 */
void cVideoStream::Flush(void)
{
	m_packets.Push(nullptr);
}

/**
 * Pushes a pre-assembled AVPacket directly to the processing queue.
 *
 * This function bypasses the PES fragmentation/reassembly mechanism and directly
 * pushes an already-complete AVPacket to the m_packets queue for decoding. Used
 * when packets are received from sources that don't require fragmentation handling.
 *
 * @param avpkt    The AVPacket to push to the queue
 * @return         true if the packet was successfully pushed, false otherwise
 */
bool cVideoStream::PushAvPacket(AVPacket *avpkt)
{
	if (avpkt->pts != AV_NOPTS_VALUE)
		m_inputPts = avpkt->pts;

	return m_packets.Push(avpkt);
}

int64_t cVideoStream::GetInputPtsMs(void)
{
	return m_inputPts * 1000 * av_q2d(m_timebase);
}

/**
 * Exit video stream
 */
void cVideoStream::Exit(void)
{
	LOGDEBUG("videostream %s: %s:", m_identifier, __FUNCTION__);

	Stop();

	if (m_pDecoder) {
		m_pDecoder->Close();
		delete(m_pDecoder);
		m_pDecoder = nullptr;
	}

	ClearVdrCoreToDecoderQueue();
}

/**
 * Clears all video stream data, which is buffered to be decoded
 */
void cVideoStream::ClearVdrCoreToDecoderQueue(void)
{
	LOGDEBUG("videostream %s: %s: packets %d", m_identifier, __FUNCTION__, m_packets.Size());

	while (!m_packets.IsEmpty()) {
		AVPacket *avpkt = m_packets.Pop();
		av_packet_free(&avpkt);
	}

	m_inputPts = AV_NOPTS_VALUE;
}

/**
 * Start the decoder
 */
void cVideoStream::StartDecoder()
{
	LOGDEBUG2(L_CODEC, "videostream %s: %s", m_identifier, __FUNCTION__);

	m_pDecoder = new cVideoDecoder(m_identifier);
	if (m_hardwareQuirks & QUIRK_CODEC_SKIP_FIRST_FRAMES)
		m_pDecoder->SetSkipKeyFramesNum(QUIRK_CODEC_SKIP_NUM_FRAMES);

	Start();
}

/**
 * Close the decoder
 */
void cVideoStream::CloseDecoder(void)
{
	LOGDEBUG2(L_CODEC, "videostream %s: %s", m_identifier, __FUNCTION__);

	m_codecId = AV_CODEC_ID_NONE;
	m_pDecoder->Close();
	m_pPar = nullptr;
	m_numIFrames = 0;
	m_maxFrameNum = 1;
	m_naluTypesAtStart.clear();
	m_dpbFrames.clear();
	m_lastPts = AV_NOPTS_VALUE;
	m_framerate = 0.0;
}

/**
 * Flush the decoder
 *
 * Some hardware (RPI) needs a reopen workaround (close/open) here, because
 * hardware doesn't do the hardware flush right.
 */
void cVideoStream::FlushDecoder(void)
{
	LOGDEBUG2(L_CODEC, "videostream %s: %s", m_identifier, __FUNCTION__);

	if ((m_hardwareQuirks & QUIRK_CODEC_FLUSH_WORKAROUND) && m_pDecoder->IsHardwareDecoder()) {
		if (m_pDecoder->ReopenCodec(m_codecId, m_pPar, m_timebase, 0))
			LOGFATAL("videostream %s: %s: Could not reopen the decoder (flush)!", m_identifier, __FUNCTION__);
	} else {
		m_pDecoder->FlushBuffers();
	}
}

/**
 * Check, if we need to force the decoder to decode the frame (force a decoder drain)
 *
 * Get the number of packets we need to have in the buffer while in interlaced
 * trickspeed mode, in order to get a decoded frameout of the decoder.
 *
 * In a normal interlaced h264 stream we need to force decoding after sending 2 packets in
 * backwards trickspeed to get a decoded frame, in an mpeg2 stream 1 packet is enough.
 *
 * This minPkts magic guarantees, that we don't drain the decoder too early, but exactly after
 * the right amount of packets was sent in trickspeed mode.
 */
void cVideoStream::CheckForcingFrameDecode(void)
{
	int minPkts = m_interlaced ? m_trickpkts : 1;

	if (!m_pRender->IsForwardTrickspeed()) {
		m_sentTrickPkts++;
		if (m_sentTrickPkts >= minPkts) {
			m_pDecoder->SendPacket(NULL);
			m_sentTrickPkts = 0;
		}
	}
}

/**
 * Open the decoder including an H.264 parsing if needed
 */
void cVideoStream::OpenDecoder(void)
{
	int width = 0;
	int height = 0;

	bool needsParsing = m_startDecodingWithIFrame ||
	                    m_parseH264Dimensions ||
	                   (m_hardwareQuirks & QUIRK_CODEC_NEEDS_DIMENSION_PARSE);

	if (needsParsing && m_codecId == AV_CODEC_ID_H264) {
		cH264Parser h264Packet(m_packets.Peek(),
		                       m_log2MaxFrameNumMinus4,
		                       m_ppsNumRefIdxL0DefaultActiveMinus1,
		                       m_ppsNumRefIdxL1DefaultActiveMinus1);
		if (!h264Packet.HasSPS()) {
			LOGDEBUG2(L_CODEC, "videostream %s: %s: No SPS in the packet!", m_identifier, __FUNCTION__);
			h264Packet.PrintStreamData();
		}

		// start decoding with an I-Frame only
		if (!h264Packet.IsISlice() && m_startDecodingWithIFrame) {
			h264Packet.PrintNalUnits();
			LOGDEBUG2(L_CODEC, "videostream %s: %s: Skip h264 packet, no I-Frame!", m_identifier, __FUNCTION__);
			AVPacket *avpkt = m_packets.Pop();
			av_packet_free(&avpkt);
			return;
		}

		// amlogic h264 decoder needs width an height for correct decoder open
		if ((m_hardwareQuirks & QUIRK_CODEC_NEEDS_DIMENSION_PARSE) || m_parseH264Dimensions) {
			width = h264Packet.GetWidth();
			height = h264Packet.GetHeight();
			LOGDEBUG2(L_CODEC, "videostream %s: %s: Parsed width %d height %d", m_identifier, __FUNCTION__, width, height);
		}
	}

	if (m_pDecoder->Open(m_codecId, m_pPar, m_timebase, false, width, height))
		LOGFATAL("videostream %s: %s: Could not open the decoder!", m_identifier, __FUNCTION__);

	m_pConfig->CurrentDecoderType = m_pDecoder->IsHardwareDecoder() ? "hardware" : "software";
	m_pConfig->CurrentDecoderName = m_pDecoder->Name();
	m_newStream = false;
	m_logPackets = m_pConfig->ConfigParseH264StreamStart ? m_pConfig->ConfigParseH264StreamStart + 1 : 0;
	m_dropInvalidPackets = m_pConfig->ConfigDropInvalidH264PFrames ? m_pConfig->ConfigDropInvalidH264PFrames + 1 : 0;
}

/**
 * Parse an H.264 packet
 */
bool cVideoStream::ParseH264Packet(AVPacket *avpkt)
{
	cH264Parser h264Packet(avpkt,
                               m_log2MaxFrameNumMinus4,
                               m_ppsNumRefIdxL0DefaultActiveMinus1,
                               m_ppsNumRefIdxL1DefaultActiveMinus1);

	if (h264Packet.HasSPS()) {
		m_maxFrameNum = 1 << (h264Packet.GetLog2MaxFrameNumMinus4() + 4);
		m_log2MaxFrameNumMinus4 = h264Packet.GetLog2MaxFrameNumMinus4();
	}

	if (h264Packet.HasPPS()) {
		m_ppsNumRefIdxL0DefaultActiveMinus1 = h264Packet.GetPpsNumRefIdxL0DefaultActiveMinus1();
		m_ppsNumRefIdxL1DefaultActiveMinus1 = h264Packet.GetPpsNumRefIdxL1DefaultActiveMinus1();
	}

	int frameNumber = h264Packet.GetFrameNum();

	if (h264Packet.IsReference()) {
		h264Packet.AddFrameNumber(frameNumber);
		m_dpbFrames.insert(frameNumber);
	} else {
		h264Packet.AddFrameNumber(-1);
	}

	bool dropPacket = false;
	if (h264Packet.IsPSlice() || h264Packet.IsBSlice()) {
		int numRefL0 = h264Packet.GetNumRefIdxL0Active();
		int numRefL1 = h264Packet.GetNumRefIdxL1Active();

		for (auto& mod : h264Packet.GetRefMods()) {
			if (mod.idc != 0 && mod.idc != 1)
				continue;

			int activeRefs = (mod.list == 0) ? numRefL0 : numRefL1;
			if (activeRefs <= 0)
				continue;

			// Compute the short-term reference frame number
			int modRef = -1;
			int diff = mod.abs_diff_pic_num_minus1 + 1;

			if (mod.idc == 0) { // subtraction
				modRef = (frameNumber - diff + m_maxFrameNum) % m_maxFrameNum;
			} else if (mod.idc == 1) { // addition
				modRef = (frameNumber + diff) % m_maxFrameNum;
			}

			// Check if this reference exists in the DPB
			if (m_dpbFrames.find(modRef) == m_dpbFrames.end()) {
				h264Packet.AddInvalidReference(modRef, frameNumber);
			} else {
				h264Packet.AddValidReference(modRef);
			}
		}

		// only print invalid references for better readability
		h264Packet.BuildInvalidReferenceString(frameNumber);
		// h264Packet.BuildValidReferenceString();

		if (h264Packet.HasInvalidBackwardReferences() && h264Packet.IsPSlice() &&  m_numIFrames < m_dropInvalidPackets) {
			LOGDEBUG2(L_CODEC, "videostream %s: %s: invalid backward reference, drop P-Frame %d", m_identifier, __FUNCTION__, frameNumber);
			dropPacket = true;
		}
	}

	if (h264Packet.IsISlice())
		m_numIFrames++;

	m_naluTypesAtStart.push_back(h264Packet.GetNalUnitString());

	return dropPacket;
}

/**
 * Decodes a reassembled codec packet
 */
void cVideoStream::DecodeInput(void)
{
	AVFrame *frame = nullptr;
	int ret = 0;

	if (m_codecId == AV_CODEC_ID_NONE || m_packets.IsEmpty() || m_pDrmBufferQueue->IsFull() || m_videoFilter.IsInputBufferFull())
		return;

	if (m_newStream)
		OpenDecoder();

	AVPacket *avpkt = m_packets.Peek();

	// log H.264 frames up to the given number of I-Frames
	bool dropPacket = false;
	if (avpkt && m_codecId == AV_CODEC_ID_H264 && (m_logPackets || m_dropInvalidPackets) && !m_isResend) {
		if (m_numIFrames < m_logPackets || m_numIFrames < m_dropInvalidPackets) {
			dropPacket = ParseH264Packet(avpkt);
		} else if (m_logPackets) {
			LOGDEBUG("videostream %s: %s: parsed H.264 stream:", m_identifier, __FUNCTION__);
			for (std::size_t i = 0; i < m_naluTypesAtStart.size(); i++) {
				LOGDEBUG("[H264] (%02d) %s", i, m_naluTypesAtStart[i].c_str());
			}
			m_logPackets = 0;
		}
	}

	// send packet to decoder
	if (!dropPacket) {
		ret = m_pDecoder->SendPacket(avpkt);

		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
			avpkt = m_packets.Pop();
			av_packet_free(&avpkt);
			m_isResend = false;
		} else {
			m_isResend = true;
		}

		if (!ret && m_pRender->IsTrickSpeed())
			CheckForcingFrameDecode();
	} else {
		avpkt = m_packets.Pop();
		av_packet_free(&avpkt);
		m_isResend = false;
	}

	// receive frame from decoder
	ret = m_pDecoder->ReceiveFrame(&frame);
	if (ret == 0) {
		RenderFrame(frame);
	} else if (ret == AVERROR_EOF) {
		FlushDecoder();
		m_sentTrickPkts = 0;
	}

	if (m_pDecoder->IsHardwareDecoder() && !m_pDecoder->GetFramesReceived()) {
		// log maximum number of packets needed for the hw decoder to deliver a frame
		if (m_pConfig->GetDecoderNeedsMaxPackets() < m_pDecoder->GetPacketsSent())
			m_pConfig->SetDecoderNeedsMaxPackets(m_pDecoder->GetPacketsSent());

		// fallback to software decoder if configured and hw decoder fails
		if (m_decoderFallbackToSwNumPkts && m_pDecoder->GetPacketsSent() > m_decoderFallbackToSwNumPkts) {
			LOGWARNING("videostream %s: %s: Could not decode frame after %d packets sent, fallback to software decoder!", m_identifier, __FUNCTION__, m_decoderFallbackToSwNumPkts);
			if (m_pDecoder->ReopenCodec(m_codecId, m_pPar, m_timebase, m_decoderFallbackToSwNumPkts))
				LOGFATAL("videostream %s: %s: Could not reopen the decoder (sw fallback)!", m_identifier, __FUNCTION__);
		}
	}
}

/**
 * Get video size and aspect ratio
 *
 * @param[out] width            video stream width
 * @param[out] height           video stream height
 * @param[out] aspect_ratio     video stream aspect ratio (is currently width/ height)
 */
void cVideoStream::GetVideoSize(int *width, int *height, double *aspect_ratio)
{
	AVCodecContext *videoCtx = m_pDecoder->GetContext();

	if (m_pDecoder && videoCtx) {
		*width = videoCtx->coded_width;
		*height = videoCtx->coded_height;
		*aspect_ratio = *width / (double)*height;
	} else {
		*width = 0;
		*height = 0;
		*aspect_ratio = 1.0;
	}
}

/**
 * Open a video codec
 *
 * @param codecId       video codec id
 * @param par           video codec parameters
 * @param timebase      timebase
 */
void cVideoStream::Open(AVCodecID codecId, AVCodecParameters *par, AVRational timebase) {
	m_newStream = true;
	m_trickpkts = codecId == AV_CODEC_ID_MPEG2VIDEO ? 1 : 2;
	m_timebase = timebase;
	m_codecId = codecId;
	m_pPar = par;
}

/*****************************************************************************
 * Thread
 ****************************************************************************/

/**
 * Decoding thread loop, which periodically tries to decode input
 */
void cVideoStream::Action(void)
{
	LOGDEBUG("videostream: decoding thread started");
	while(Running()) {
		m_mutex.lock();

		DecodeInput();

		m_mutex.unlock();

		usleep(1000);
	}
	LOGDEBUG("videostream: decoding thread stopped");
}

/**
 * Stop the decoding thread
 */
void cVideoStream::Stop(void)
{
	if (!Active())
		return;

	LOGDEBUG("videostream: stopping decoding thread");
	Cancel(2);
}

/**
 * Stop filter thread
 */
void cVideoStream::CancelFilterThread(void) {
	if (m_videoFilter.Active())
		m_videoFilter.Stop();

	m_checkFilterThreadNeeded = true;
	SetDeinterlacerDeactivated(false);
}

/**
 * Render a frame
 *
 * Frames either go through the filter thread or directly into the render buffer.
 *
 * @param videoCtx      ffmpeg video codec context
 * @param frame         frame to render
 */
void cVideoStream::RenderFrame(AVFrame * frame)
{
	if (frame->decode_error_flags || frame->flags & AV_FRAME_FLAG_CORRUPT)
		LOGWARNING("videostream: %s: %s: error_flag or FRAME_FLAG_CORRUPT", m_identifier, __FUNCTION__);

	if (!m_framerate) {
		if (av_q2d(m_pDecoder->GetContext()->framerate) > 0.0) {
			// the decoder has a valid framerate, use it
			m_framerate = av_q2d(m_pDecoder->GetContext()->framerate);
		} else if (frame->pts != AV_NOPTS_VALUE) {
			// The decoder has no valid framerate, calculate it from pts and timebase
			// @todo We are only using the first two frames here.
			// If there are issues in the streamm this might not be correct.
			if (m_lastPts != AV_NOPTS_VALUE) {
				int64_t delta = frame->pts - m_lastPts;
				m_framerate = 1.0 / (delta * av_q2d(m_timebase));
			}
			m_lastPts = frame->pts;
		}
	}

	// Normalize 720x576(i) to 1280x720(p)
	//
	// We don't want really use a 720x576 display mode...
	// Deinterlace it here and choose 1280x720 (720p) mode
	int normalizedWidth = m_pDecoder->GetContext()->coded_width;
	int normalizedHeight = m_pDecoder->GetContext()->coded_height;
	bool forceDeinterlacing = false;

	if (normalizedWidth == 720 && normalizedHeight == 576) {
		normalizedWidth = 1280;
		normalizedHeight = 720;
		forceDeinterlacing = true;
	}

	bool displayModeFollowsVideo = m_pConfig->ConfigVideoDisplayMode == 1;

	// Filter thread will only be started, if the lambda function returns true
	if (m_checkFilterThreadNeeded) {
		m_timebase = m_pDecoder->GetContext()->pkt_timebase;

		// Enable the deinterlacer only if:
		// - The user did not disable the deinterlacer
		// - The deinterlacer is not temporarily deactivated (trickspeed and still picture)
		// - A hardware quirk does not forbid using the deinterlacer
		// - It is an interlaced stream, determined by:
		//   - The codec is different from HEVC (always progressive)
		//   - The framerate is lower or equal to 30fps
		//   - Or, if the frame's interlaced flag is set
		// We cannot solely rely on the frame's interlaced flag, because the deinterlacer shall also be enabled with mixed progressive/interlaced streams (e.g. TV station "ProSieben").

		m_interlaced =
			(m_pDecoder->GetContext()->codec_id != AV_CODEC_ID_HEVC &&
			m_pDecoder->GetContext()->framerate.num > 0 &&
			av_q2d(m_pDecoder->GetContext()->framerate) < 30.1) || isInterlacedFrame(frame); // account for rounding errors when comparing double

		bool displayCanHandleMode = false;
		sDrmMode mode = { normalizedWidth,
		                  normalizedHeight,
		                  forceDeinterlacing ? m_framerate * 2 : m_framerate,
		                  forceDeinterlacing ? false : m_interlaced };
		if (m_pRender->CanHandleMode(&mode))
			displayCanHandleMode = true;

		bool useDeinterlacer =
			!m_userDisabledDeinterlacer &&
			!m_deinterlacerDeactivated &&
			!(m_hardwareQuirks & QUIRK_NO_HW_DEINT) &&
			m_interlaced &&
			(!displayModeFollowsVideo || (!displayCanHandleMode || forceDeinterlacing));

		if (m_userDisabledDeinterlacer)
			LOGDEBUG("videostream: %s: %s: deinterlacer disabled by user configuration", m_identifier, __FUNCTION__);

		// Use the filter thread if:
		// - AV_PIX_FMT_YUV420P, interlaced -> software deinterlacer (bwdif filter)
		// - AV_PIX_FMT_YUV420P, progressive -> scale filter to get NV12 frames
		// - AV_PIX_FMT_DRM_PRIME, interlaced, hw deinterlacer available -> hw deinterlacer
		if (frame->format == AV_PIX_FMT_YUV420P || (frame->format == AV_PIX_FMT_DRM_PRIME && useDeinterlacer))
			m_videoFilter.InitAndStart(m_pDecoder->GetContext(), frame, useDeinterlacer);

		m_checkFilterThreadNeeded = false;
	}

	// reset display mode if needed
	if (m_framerate &&
	   (m_pConfig->CurrentVideoDrmMode.width         != normalizedWidth ||
	    m_pConfig->CurrentVideoDrmMode.height        != normalizedHeight ||
	    m_pConfig->CurrentVideoDrmMode.refreshRateHz != (forceDeinterlacing ? m_framerate * 2 : m_framerate) ||
	    m_pConfig->CurrentVideoDrmMode.interlaced    != (forceDeinterlacing ? false : m_interlaced))) {

		m_pConfig->CurrentVideoDrmMode = { normalizedWidth,
		                                   normalizedHeight,
		                                   (forceDeinterlacing ? m_framerate * 2 : m_framerate),
		                                   (forceDeinterlacing ? false : m_interlaced) };

		if (displayModeFollowsVideo && forceDeinterlacing)
			LOGDEBUG2(L_DRM, "videostream: %s: force 576i -> 720p", __FUNCTION__);

		m_pRender->SetDisplayMode(m_pConfig->ConfigVideoDisplayMode);
	}

	if (m_videoFilter.Active())
		m_videoFilter.PushFrame(frame);
	else {
		// AV_PIX_FMT_DRM_PRIME, interlaced, hw deinterlacer not available
		// AV_PIX_FMT_DRM_PRIME, progressive
		// -> put the frame directly into render buffer
		if (!m_videoFilter.GetNumFramesToFilter())
			m_frameOutput(frame);
	}
}
