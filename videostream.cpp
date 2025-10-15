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
	m_packetsFilled = 0;
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
 * Initialize video packet ringbuffer
 */
void cVideoStream::InitPacketRb(void)
{
	for (int i = 0; i < VIDEO_PACKET_MAX; ++i) {
		AVPacket *avpkt;

		avpkt = &m_packetRb[i];
		if (av_new_packet(avpkt, VIDEO_BUFFER_SIZE)) {
			LOGFATAL("videostream %s: out of memory", __FUNCTION__);
		}
		avpkt->size = 0;
	}

	atomic_set(&m_packetsFilled, 0);
	m_packetRead = 0;
	m_packetWrite = 0;
}

/**
 * Cleanup video packet ringbuffer
 */
void cVideoStream::CleanupPacketRb(void)
{
	atomic_set(&m_packetsFilled, 0);

	for (int i = 0; i < VIDEO_PACKET_MAX; ++i) {
		av_packet_unref(&m_packetRb[i]);
	}
}

/**
 * Place video data in packet ringbuffer
 *
 * @param pts       presentation timestamp of pes packet
 * @param data      data of pes packet
 * @param size      size of pes packet
 */
void cVideoStream::EnqueueInRb(int64_t pts, const void *data, int size)
{
	AVPacket *avpkt = &m_packetRb[m_packetWrite];

	if (pts != AV_NOPTS_VALUE) {
		if (avpkt->size) {
			m_packetWrite = (m_packetWrite + 1) % VIDEO_PACKET_MAX;
			atomic_inc(&m_packetsFilled);
		}
		avpkt = &m_packetRb[m_packetWrite];
		avpkt->size = 0;
		avpkt->pts = pts;
		avpkt->dts = AV_NOPTS_VALUE;
	}

	if ((size_t)(avpkt->size + size) >= avpkt->buf->size) {
		int pktSize = avpkt->size;
		LOGWARNING("videostream %s: packet buffer too small for %d", __FUNCTION__, avpkt->size + size);
		av_grow_packet(avpkt, size);
		avpkt->size = pktSize;
	}

	memcpy(avpkt->data + avpkt->size, data, size);
	avpkt->size += size;
	memset(avpkt->data + avpkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
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

	CleanupPacketRb();
}

/**
 * Clears all video stream data, which is buffered to be decoded
 */
void cVideoStream::Clear(void)
{
	LOGDEBUG("videostream %s: packets %d", __FUNCTION__, atomic_read(&m_packetsFilled));

	AVPacket *avpkt;
	m_pktsMutex.Lock();
	atomic_set(&m_packetsFilled, 0);
	m_packetRead = m_packetWrite = 0;

	avpkt = &m_packetRb[m_packetWrite];
	avpkt->size = 0;
	avpkt->pts = AV_NOPTS_VALUE;

	m_pktsMutex.Unlock();
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
 * Decode from PES packet ringbuffer.
 *
 * @param stream	video stream
 *
 * @retval 0        packet was decoded or more data is needed
 * @retval 1        stream is paused
 * @retval -1       stream is empty or closed
 */
int cVideoStream::DecodeInput(void)
{
	AVPacket *avpkt;
	AVFrame *frame;
	int ret = 0;
	static int sent = 0;

	if (IsClosing()) {
		m_closeCondition.Signal();
		return -1;
	}

	if (IsPaused()) {
//		LOGDEBUG2(L_CODEC, "videostream %s: stream is paused", __FUNCTION__);
		m_pauseCondition.Broadcast();
		return 1;
	}

	// early skip, if there are no packets to decode
	m_pktsMutex.Lock();
	if (!atomic_read(&m_packetsFilled)) {
		m_pktsMutex.Unlock();
		return -1;
	}
	m_pktsMutex.Unlock();

	if (m_newStream && m_codecId != AV_CODEC_ID_NONE) {
		int width = 0;
		int height = 0;

		// amlogic h264 decoder needs this
		if ((m_codecId == AV_CODEC_ID_H264) && (m_pRender->HardwareQuirks() & QUIRK_CODEC_NEEDS_EXT_INIT)) {
			m_pktsMutex.Lock();
			if (!atomic_read(&m_packetsFilled)) {
				m_pktsMutex.Unlock();
				return -1;
			}

			cH264Parser h264Parser(&m_packetRb[m_packetRead]);
			h264Parser.GetDimensions(&width, &height);
			m_pktsMutex.Unlock();

			LOGDEBUG2(L_CODEC, "videostream %s: Parsed width %d height %d", __FUNCTION__, width, height);
		}

		if (m_pDecoder->Open(m_codecId, m_pPar, &m_timebase, 0, width, height))
			LOGFATAL("videostream %s: Could not open the decoder!", __FUNCTION__);
		m_newStream = false;
	}

	if (m_codecId != AV_CODEC_ID_NONE) {
		m_pktsMutex.Lock();
		// wait for m_trickpkts packets
		//
		// m_trickpkts is the number of packets we need to have in the ringbuffer
		// while in interlaced trickspeed mode, needed to get a frame.
		// This guarantees, that we don't drain the decoder too early, but exactly after
		// m_trickpkts sent packets
		int minPkts = (m_pRender->GetTrickSpeed() && m_interlaced) ? m_trickpkts : 1;
		if (atomic_read(&m_packetsFilled) < minPkts) {
			m_pktsMutex.Unlock();
			return -1;
		}
		avpkt = &m_packetRb[m_packetRead];

		// send packet to decoder
		ret = m_pDecoder->SendPacket(avpkt);
		if (ret != AVERROR(EAGAIN)) { // something went wrong or packet was sent, advance packet
			m_packetRead = (m_packetRead + 1) % VIDEO_PACKET_MAX;
			atomic_dec(&m_packetsFilled);
			// in backward trickspeed force the decoder to decode the frame, if minPkts are sent
			if (ret == 0 && m_pRender->GetTrickSpeed() && !m_pRender->GetTrickForward()) {
				sent++;
				if (sent >= minPkts) {
					m_pDecoder->SendPacket(NULL);
					sent = 0;
				}
			}
		}
		m_pktsMutex.Unlock();

		// receive frame from decoder
		if (!m_pRender->GetTrickSpeed()) {
			// this is normal Playback
			if (!m_newStream) { // this is for mediaplayer ?
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
			// ths is TrickSpeed
			ret = m_pDecoder->ReceiveFrame(1, &frame);
			while (ret == 0) {
				while (m_pRender->GetTrickSpeed() && m_pRender->GetTrickCounter() > 0) {
					AVFrame *trickframe = av_frame_clone(frame);
					if (!trickframe) {
						LOGERROR("videostream %s: could not clone frame", __FUNCTION__);
						break;
					}
					LOGDEBUG2(L_TRICK, "videostream %s: Trickspeed, send another cloned trick frame %d %p", __FUNCTION__, m_pRender->GetTrickCounter(), trickframe);
					m_pRender->MarkAsTrickspeedFrame(trickframe);
					while (m_pRender->RenderFrame(m_pDecoder->GetContext(), trickframe)) {
						if (IsClosing()) {
							av_frame_free(&trickframe);
							av_frame_free(&frame);
							sent = 0;
							return -1;
						}
					}
					m_pRender->DecTrickCounter();
					if (IsClosing()) {
						av_frame_free(&frame);
						sent = 0;
						return -1;
					}
				}
				av_frame_free(&frame);
				sent = 0;

				int trickSpeed = m_pRender->GetTrickSpeed();
				m_pRender->SetTrickCounter(trickSpeed);

				// try receiving another frame from decoder, should end up with AVERROR_EOF
				ret = m_pDecoder->ReceiveFrame(1, &frame);
			}

			if (ret == AVERROR_EOF) { // needs flush / reopen
				FlushDecoder();
				sent = 0;
			}
		} // end receive frame
		return 0;
	}

	return -1;
}

/**
 * Get pointer to avpkt in ringbuffer, where we can write to
 *
 * @return     avpkt to write data in
 */
AVPacket *cVideoStream::GetPacketToWrite(void)
{
	AVPacket *avpkt = &m_packetRb[m_packetWrite];

	return avpkt;
}

/**
 * Advance the write pointer to avpkt in ringbuffer
 */
void cVideoStream::AdvancePacketToWrite(void)
{
	m_packetWrite = (m_packetWrite + 1) % VIDEO_PACKET_MAX;
}

/**
 * Increase filled packets counter
 */
void cVideoStream::IncreasePacketsFilled(void)
{
	atomic_inc(&m_packetsFilled);
}

/**
 * Get number of video buffers.
 *
 * @param stream            video stream
 */
int cVideoStream::GetPacketsFilled(void)
{
	return atomic_read(&m_packetsFilled);
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
