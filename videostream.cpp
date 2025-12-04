/**
 * @file videostream.cpp
 * Videostream class
 *
 * This file defines cVideoStream, which is repsonsible for
 * handling the video stream.
 *
 * @copyright (c) 2011 - 2015 by Johns.  All Rights Reserved.
 * @copyright (c) 2018 - 2019 by zille.  All Rights Reserved.
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

#include <assert.h>
#include <unistd.h>

#include <libintl.h>

#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
}

#include "softhddevice-drm-gles.h"
#include "softhddevice.h"
#include "logger.h"
#include "h264parser.h"

#include "buf2rgb.h"

#include "iatomic.h"
#include "videostream.h"
#include "audio.h"
#include "videorender.h"
#include "codec_audio.h"
#include "codec_video.h"

#include "queue.h"
#include "misc.h"

/*****************************************************************************
 * cVideoStream class
 ****************************************************************************/

/**
 * cVideoStream constructor
 */
cVideoStream::cVideoStream(cSoftHdDevice *device)
{
	LOGDEBUG("videostream %s:", __FUNCTION__);

	m_pRender = device->Render();
	m_pDecoder = nullptr;

	m_codecId = AV_CODEC_ID_NONE;
	m_newStream = false;
	m_pPar = nullptr;

	m_videoWidth = 0;
	m_videoHeight = 0;
}

/**
 * cVideoStream destructor
 */
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
	return m_packets.Push(avpkt);
}

/**
 * Exit video stream
 */
void cVideoStream::Exit(void)
{
	LOGDEBUG("videostream %s:", __FUNCTION__);

	ExitDecodingThread();

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
	LOGDEBUG("videostream %s: packets %d", __FUNCTION__, m_packets.Size());

	while (!m_packets.Empty()) {
		AVPacket *avpkt = m_packets.Pop();
		av_packet_free(&avpkt);
	}
}

/**
 * Start the decoder
 */
void cVideoStream::StartDecoder(cVideoDecoder *decoder, const char *name)
{
	LOGDEBUG2(L_CODEC, "videostream %s", __FUNCTION__);

	m_pDecoder = decoder;
	CreateDecodingThread(name);
}

/**
 * Init the decoder
 */
void cVideoStream::TryInitDecoder(void)
{
	if (!m_newStream)
		return;

	int width = 0;
	int height = 0;

	// amlogic h264 decoder needs width an height for correct decoder open
	if ((m_codecId == AV_CODEC_ID_H264) && (m_pRender->HardwareQuirks() & QUIRK_CODEC_NEEDS_EXT_INIT)) {
		cH264Parser h264Parser(m_packets.Peek());
		h264Parser.GetDimensions(&width, &height);

		LOGDEBUG2(L_CODEC, "videostream %s: Parsed width %d height %d", __FUNCTION__, width, height);
	}

	if (m_pDecoder->Open(m_codecId, m_pPar, &m_timebase, 0, width, height))
		LOGFATAL("videostream %s: Could not open the decoder!", __FUNCTION__);

	m_newStream = false;
}

/**
 * Returns true if we are able to decode
 */
bool cVideoStream::CanDecodePacket(void)
{
	if (m_codecId == AV_CODEC_ID_NONE || m_packets.Empty())
		return false;

	return true;
}

/**
 * Decodes a reassembled codec packet.
 */
void cVideoStream::DecodeInput(void)
{
	if (!CanDecodePacket() || IsBufferFull())
		return;

	TryInitDecoder();

	// send packet to decoder
	AVPacket *avpkt = m_packets.Peek();

	int ret = m_pDecoder->SendPacket(avpkt);

	if (ret != AVERROR(EAGAIN)) {
		avpkt = m_packets.Pop();
		av_packet_free(&avpkt);
	}

	// handle trickspeed
	if (ret == 0)
		TrickspeedForceDecoderDrain();

	// receive frame from decoder
	AVFrame *frame = nullptr;
	int width = 0;
	int height = 0;
	if (m_pDecoder->ReceiveFrame(&frame, width, height) == 0) {
		SetVideoSize(width, height);
		RenderFrame(frame);
	}

	// flush if needed (in trickspeed)
	if (ret == AVERROR_EOF)
		TrickspeedFlushDecoderBuffers();
}

/**
 * Close the decoder
 */
void cVideoStream::CloseDecoder(void)
{
	LOGDEBUG2(L_CODEC, "videostream %s", __FUNCTION__);

	m_codecId = AV_CODEC_ID_NONE;
	m_pDecoder->Close();
	m_pPar = nullptr;
}

/**
 * Flush the decoder
 *
 * Some hardware (RPI) needs a reopen workaround (close/open) here, because
 * hardware doesn't do the hardware flush right.
 */
void cVideoStream::FlushDecoder(void)
{
	LOGDEBUG2(L_CODEC, "videostream %s", __FUNCTION__);

	if (m_pRender->HardwareQuirks() & QUIRK_CODEC_FLUSH_WORKAROUND) {
		if (m_pDecoder->ReopenCodec(m_codecId, m_pPar, &m_timebase, 0))
			LOGFATAL("videostream %s: Could not reopen the decoder (flush)!", __FUNCTION__);
	} else {
		m_pDecoder->FlushBuffers();
	}
}

/**
 * Set video size and aspect
 *
 * @param width            video stream width
 * @param height           video stream height
 */
void cVideoStream::SetVideoSize(int width, int height)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_videoWidth = width;
	m_videoHeight = height;
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
	std::lock_guard<std::mutex> lock(m_mutex);
	*width = 0;
	*height = 0;
	*aspect_ratio = 1.0f;

	if (m_videoWidth && m_videoHeight) {
		*width = m_videoWidth;
		*height = m_videoHeight;
		*aspect_ratio = (double)(m_videoWidth) / (double)(m_videoHeight);
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
	m_timebase = timebase;
	m_codecId = codecId;
	m_pPar = par;
}

/*****************************************************************************
 * Thread
 ****************************************************************************/

/**
 * Create and start the decoding thread
 */
void cVideoStream::CreateDecodingThread(const char *name)
{
	m_pDecodingThread = new cDecodingThread(this, name);
}

/**
 * Stop decoding thread
 */
void cVideoStream::ExitDecodingThread(void)
{
	LOGDEBUG("videostream: %s", __FUNCTION__);

	if (m_pDecodingThread->Active())
		m_pDecodingThread->Stop();

	if (m_pDecodingThread)
		delete m_pDecodingThread;
}

/*****************************************************************************
 * cMainVideoStream class
 ****************************************************************************/

/**
 * cMainVideoStream constructor
 */
cMainVideoStream::cMainVideoStream(cSoftHdDevice *device) : cVideoStream(device)
{
	m_trickpkts = 1;
	m_interlaced = 0;
}

/**
 * Set the interlaced flag for the stream
 *
 * @param interlaced        true, if interlaced
 */
void cMainVideoStream::SetInterlaced(bool interlaced)
{
//	LOGDEBUG("videostream %s: %d", __FUNCTION__, m_interlaced);
	m_interlaced = interlaced;
	// H264 and HEVC need 2 interlaced packets to be sent to the decoder
	// in trickspeed mode in order to get a decoded frame out
	m_trickpkts = m_interlaced ? (m_codecId != AV_CODEC_ID_MPEG2VIDEO ? 2 : 1) : 1;
}

/**
 * Returns true, if the render buffer for main video frames is full
 */
bool cMainVideoStream::IsBufferFull(void) const
{
	return m_pRender->IsBufferFull();
}

/**
 * Send AVFrame to the renderer
 */
void cMainVideoStream::RenderFrame(AVFrame *frame)
{
	m_pRender->RenderFrame(m_pDecoder->GetContext(), frame);
}

/**
 * If in trickspeed, drain decoder if needed
 *
 * In backward trickspeed force the decoder to decode the frame, if m_trickpkts are sent
 * m_trickpkts is the number of packets we need to have in the buffer
 * while in interlaced trickspeed mode, needed to get a frame.
 * This guarantees, that we don't drain the decoder too early, but exactly after
 * m_trickpkts sent packets
 *
 * This needs a check for AVERROR_EOF after receiving the frame (TrickspeedFlushDecoderBuffers())
 */
void cMainVideoStream::TrickspeedForceDecoderDrain(void)
{
	if (m_pRender->GetTrickSpeed() && !m_pRender->GetTrickForward()) {
		m_sentTrickPkts++;
		if (m_sentTrickPkts >= m_trickpkts) {
			m_pDecoder->SendPacket(NULL);
			m_sentTrickPkts = 0;
		}
	}
}

/**
 * Flush the decoder if needed
 */
void cMainVideoStream::TrickspeedFlushDecoderBuffers(void)
{
	if (m_pRender->GetTrickSpeed()) { // needs flush / reopen
		FlushDecoder();
		m_sentTrickPkts = 0;
	}
}

/*****************************************************************************
 * cPipVideoStream class
 ****************************************************************************/

/**
 * cPipVideoStream constructor
 */
cPipVideoStream::cPipVideoStream(cSoftHdDevice *device) : cVideoStream(device)
{
}

/**
 * Returns true, if the render buffer for pip frames is full
 */
bool cPipVideoStream::IsBufferFull(void) const
{
	return m_pRender->IsPipBufferFull();
}

/**
 * Send AVFrame to the renderer
 */
void cPipVideoStream::RenderFrame(AVFrame *frame)
{
	m_pRender->RenderPipFrame(m_pDecoder->GetContext(), frame);
}
