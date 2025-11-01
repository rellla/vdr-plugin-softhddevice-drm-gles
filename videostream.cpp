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
		if (m_pDecoder->ReceiveFrame(&frame) == 0) {
			if (m_pRender->GetTrickSpeed()) {
				m_pRender->MarkAsProgressiveFrame(frame);
				m_pRender->MarkAsTrickspeedFrame(frame);
			}

			m_pRender->RenderFrame(m_pDecoder->GetContext(), frame);
		}
	}

	if (m_pRender->GetTrickSpeed() && ret == AVERROR_EOF) { // needs flush / reopen
		FlushDecoder();
		m_sentTrickPkts = 0;
	}
}


/**
 * Display the given I-frame as a still picture
 *
 * @param pesPacket      cPesVideo packet
 */
void cVideoStream::StillPicture(cPesVideo *pesPacket)
{
	AVPacket *avpkt = CreateAvPacket(pesPacket->GetPayload(), pesPacket->GetPayloadSize(), pesPacket->GetPts());

	// close the decoder if open and another codec id arrives
	if (Decoder()->GetContext()) {
		if ((int)(Decoder()->GetContext()->codec_id) != pesPacket->GetCodec()) {
			Decoder()->Close();
		}
	}
	// open the decoder if we have none (context flag is set)
	bool context = false;
	if (!Decoder()->GetContext()) {
		if (Decoder()->Open(pesPacket->GetCodec(), NULL, NULL, 0, 0, 0))
			LOGFATAL("videostream: %s: Could not open the decoder!", __FUNCTION__);
		context = true;
	}

	int ret = 0;
	ret = Decoder()->SendPacket(avpkt);
	if (ret)
		LOGDEBUG2(L_STILL, "videostream: %s: SendPacket(avpkt) returned %d", __FUNCTION__, ret);
	else
		LOGDEBUG2(L_STILL, "videostream: %s: avpkt sent", __FUNCTION__);

	av_packet_free(&avpkt);

	// force decoder to enter draining because we only want 1 avpkt to be decoded
	Decoder()->SendPacket(NULL);

	AVFrame *frame;
	ret = Decoder()->ReceiveFrame(&frame);

	// we got a frame, so try to render it and try another one (should end up with AVERROR_EOF)
	while (!ret) {
		// always treat a stillpicture frame as a progressive frame
		LOGDEBUG2(L_STILL, "videostream: %s: frame received", __FUNCTION__);
		m_pRender->MarkAsProgressiveFrame(frame);
		m_pRender->MarkAsStillpictureFrame(frame);
		while (m_pRender->RenderFrame(Decoder()->GetContext(), frame)) {}
		// try to get another frame
		ret = Decoder()->ReceiveFrame(&frame);
	}

	// no more frames available or error
	if (ret == AVERROR_EOF) {
		// AVERROR_EOF, flush needed
		FlushDecoder();
	} else {
		// sth went wrong or AVERROR(EAGAIN)
		LOGDEBUG2(L_STILL, "videostream: %s: ReceiveFrame returned %d, should not happen!", __FUNCTION__, ret);
	}

	// close the decoder, if it was opened by StillPicture
	if (context) {
		Decoder()->Close();
		SetCodecId(AV_CODEC_ID_NONE);
	}

	FlushDecoder();
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
