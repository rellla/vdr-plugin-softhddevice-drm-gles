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

	m_interlaced = 0;
	m_trickpkts = 1;
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
void cVideoStream::StartDecoder(cVideoDecoder *decoder)
{
	LOGDEBUG2(L_CODEC, "videostream %s", __FUNCTION__);

	m_pDecoder = decoder;
	CreateDecodingThread();
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
 * Decodes a reassembled codec packet.
 */
void cVideoStream::DecodeInput(void)
{
	AVFrame *frame = nullptr;
	int ret = 0;

	if (m_codecId == AV_CODEC_ID_NONE || m_packets.Empty() || m_pRender->IsBufferFull())
		return;

	if (m_newStream) {
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

	// wait for m_trickpkts packets
	//
	// m_trickpkts is the number of packets we need to have in the buffer
	// while in interlaced trickspeed mode, needed to get a frame.
	// This guarantees, that we don't drain the decoder too early, but exactly after
	// m_trickpkts sent packets
	int minPkts = (m_pRender->GetTrickSpeed() && m_interlaced) ? m_trickpkts : 1;

	// send packet to decoder
	AVPacket *avpkt = m_packets.Peek();

	ret = m_pDecoder->SendPacket(avpkt);

	if (ret != AVERROR(EAGAIN)) {
		avpkt = m_packets.Pop();
		av_packet_free(&avpkt);
	}

	// in backward trickspeed force the decoder to decode the frame, if minPkts are sent
	if (ret == 0 && m_pRender->GetTrickSpeed() && !m_pRender->GetTrickForward()) {
		m_sentTrickPkts++;
		if (m_sentTrickPkts >= minPkts) {
			m_pDecoder->SendPacket(NULL);
			m_sentTrickPkts = 0;
		}
	}

	// receive frame from decoder
	if (!m_newStream) { // this is for mediaplayer?
		if (m_pDecoder->ReceiveFrame(&frame) == 0)
			m_pRender->RenderFrame(m_pDecoder->GetContext(), frame);
	}

	if (m_pRender->GetTrickSpeed() && ret == AVERROR_EOF) { // needs flush / reopen
		FlushDecoder();
		m_sentTrickPkts = 0;
	}
}

/**
 * Set the interlaced flag for the stream
 *
 * @param interlaced        true, if interlaced
 */
void cVideoStream::SetInterlaced(bool interlaced)
{
//	LOGDEBUG("videostream %s: %d", __FUNCTION__, m_interlaced);
	m_interlaced = interlaced;
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
 * Create and start the decoding thread
 */
void cVideoStream::CreateDecodingThread(void)
{
	m_pDecodingThread = new cDecodingThread(this);
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
