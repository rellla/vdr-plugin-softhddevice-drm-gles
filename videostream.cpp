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
	m_paused = false;
	m_pPar = nullptr;

	m_interlaced = 0;
	m_trickpkts = 1;

	Start();
}

/**
 * cVideoStream destructor
 */
cVideoStream::~cVideoStream(void)
{
	LOGDEBUG("videostream %s:", __FUNCTION__);
}

/**
 * Pushes a PES packet to the processing queue. The packet may have a fragmented payload.
 *
 * @param pesPacket    PES packet to push
 */
void cVideoStream::PushPesPacket(cPes *pesPacket)
{
	if (pesPacket->HasPts()) {
		// Received the first fragment of the upcoming codec packet.
		if (!m_currentCodecPacket.empty()) {
			// Push the buffered PES fragments as one reassembled codec packet to the next stage.
			m_packets.Push(CreateAvPacket(m_currentCodecPacket.data(), m_currentCodecPacket.size(), m_currentPacketPts));
			m_currentCodecPacket.clear();
		}

		m_currentPacketPts = pesPacket->GetPts();
	}

	// buffer the payload of the just received PES packet in the fragmentation buffer
	m_currentCodecPacket.insert(m_currentCodecPacket.end(), pesPacket->GetPayload(), pesPacket->GetPayload() + pesPacket->GetPayloadSize());
}

void cVideoStream::ResetFragmentationBuffer() {
	m_currentCodecPacket.clear();
}

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

	if (m_pDecoder) {
		m_pDecoder->Close();
		delete(m_pDecoder);
		m_pDecoder = nullptr;
	}

	ClearPacketQueue();
}

/**
 * Clears all video stream data, which is buffered to be decoded
 */
void cVideoStream::ClearPacketQueue(void)
{
	LOGDEBUG("videostream %s: packets %d", __FUNCTION__, m_packets.Size());

	while (!m_packets.Empty()) {
		AVPacket *avpkt = m_packets.Pop();
		av_packet_free(&avpkt);
	}

	ResetFragmentationBuffer();
}

/**
 * Start the decoder
 */
void cVideoStream::StartDecoder(cVideoDecoder *decoder)
{
	LOGDEBUG2(L_CODEC, "videostream %s", __FUNCTION__);

	m_pDecoder = decoder;
	m_pRender->CreateDecodingThread();
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
 * Render a decoded frame as often as trickspeed mode wants it
 *
 * This functions returns if the requested count of frames (GetTrickCounter) was renderer,
 * if TrickSpeed mode ended or if a stream closing was reqeusted.
 *
 * @retval -1     stream closing was requested or sth went wrong
 * @retval 0      frames have been rendered as expected or no need to render anything
 */
int cVideoStream::RenderTrickspeedFrames(AVFrame *frame)
{
	if (!frame)
		return -1;

	while (m_pRender->GetTrickSpeed() && m_pRender->GetTrickCounter() > 0) {
		AVFrame *trickframe = av_frame_clone(frame);
		if (!trickframe) {
			LOGERROR("videostream %s: could not clone frame", __FUNCTION__);
			return -1;
		}
		LOGDEBUG2(L_TRICK, "videostream %s: Trickspeed, send another cloned trick frame %d %p", __FUNCTION__, m_pRender->GetTrickCounter(), trickframe);
		m_pRender->MarkAsTrickspeedFrame(trickframe);
		while (m_pRender->RenderFrame(m_pDecoder->GetContext(), trickframe)) {
			if (IsClosing()) {
				av_frame_free(&trickframe);
				return -1;
			}
		}
		m_pRender->DecTrickCounter();

		if (IsClosing())
			return -1;
	}

	return 0;
}

/**
 * Decodes a reassembled codec packet.
 *
 * @retval 0        packet was decoded or more data is needed
 * @retval 1        stream is paused
 * @retval -1       stream is empty or closed
 */
int cVideoStream::DecodeInput(void)
{
	AVFrame *frame = nullptr;
	int ret = 0;

	if (IsClosing()) {
		m_closeCondition.Signal();
		return -1;
	}

	if (IsPaused()) {
//		LOGDEBUG2(L_CODEC, "videostream %s: stream is paused", __FUNCTION__);
		m_pauseCondition.Broadcast();
		return 1;
	}

	if (m_codecId == AV_CODEC_ID_NONE)
		return -1;

	if (m_newStream) {
		int width = 0;
		int height = 0;

		// amlogic h264 decoder needs width an height for correct decoder open
		if ((m_codecId == AV_CODEC_ID_H264) && (m_pRender->HardwareQuirks() & QUIRK_CODEC_NEEDS_EXT_INIT)) {
			AVPacket *packet = m_packets.Peek();
			if (packet == nullptr)
				return -1;

			cH264Parser h264Parser(packet);
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
	if ((int)m_packets.Size() < minPkts) {
		return -1;
	}

	// send packet to decoder
	ret = m_pDecoder->SendPacket(m_packets.Peek());
	if (ret != AVERROR(EAGAIN)) { // something went wrong or packet was sent, advance packet
		AVPacket *avpkt = m_packets.Pop();
		av_packet_free(&avpkt);

		// in backward trickspeed force the decoder to decode the frame, if minPkts are sent
		if (ret == 0 && m_pRender->GetTrickSpeed() && !m_pRender->GetTrickForward()) {
			m_sentTrickPkts++;
			if (m_sentTrickPkts >= minPkts) {
				m_pDecoder->SendPacket(NULL);
				m_sentTrickPkts = 0;
			}
		}
	}

	// receive frame from decoder
	if (!m_pRender->GetTrickSpeed()) {
		// this is normal Playback
		 if (!m_newStream) { // this is for mediaplayer?
			ret = m_pDecoder->ReceiveFrame(0, &frame);
			if (ret == 0) {
				while (m_pRender->RenderFrame(m_pDecoder->GetContext(), frame)) {
					if (IsClosing()) {
						av_frame_free(&frame);
						return -1;
					}
				}
			}
		}
	} else {
		// this is TrickSpeed
		ret = m_pDecoder->ReceiveFrame(1, &frame);
		while (ret == 0) {
			if (RenderTrickspeedFrames(frame)) { // returns -1 only if stream should be closed
				av_frame_free(&frame);
				m_sentTrickPkts = 0;
				return -1;
			}

			av_frame_free(&frame);
			m_sentTrickPkts = 0;

			m_pRender->SetTrickCounter(m_pRender->GetTrickSpeed());
			ret = m_pDecoder->ReceiveFrame(1, &frame);
		} // try receiving another frame from decoder, should end up with AVERROR_EOF

		if (ret == AVERROR_EOF) { // needs flush / reopen
			FlushDecoder();
			m_sentTrickPkts = 0;
		}
	}

	return 0;
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
 * Set the timebase for the stream
 *
 * @param num       timbase numerator
 * @param den       timebase denumerator
 */
void cVideoStream::SetTimebase(int num, int den)
{
	m_timebase.num = num;
	m_timebase.den = den;
}

/**
 * Stop the stream
 *
 * Skips the decoding of the stream until m_closing gets false again (with Start())
 */
void cVideoStream::Stop(void)
{
	int timeoutInMs = 1000;
	m_closing = true;

	if (!m_closeCondition.Wait(timeoutInMs))
		LOGERROR("videostream %s: Timeout while closing stream (%d ms)!", __FUNCTION__, timeoutInMs);

	LOGDEBUG2(L_CODEC, "videostream %s: stream is closing", __FUNCTION__);
}

/**
 * Pause the stream
 *
 * Prevent the stream from decoding new frames and sending them to filter or renderer
 * cCondVar is necessary to finish a decoding loop
 */
void cVideoStream::Pause(void)
{
	int timeoutInMs = 2000;

	m_paused = true;
	cMutex mutex;
	mutex.Lock();
	if (!m_pauseCondition.TimedWait(mutex, timeoutInMs))
		LOGERROR("videostream %s: Timeout while pausing stream (%d ms)!", __FUNCTION__, timeoutInMs);
}
