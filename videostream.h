/**
 * @file videostream.h
 * Videostream class header file
 *
 * @copyright (c) 2011 - 2015 by Johns.  All Rights Reserved.
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

#ifndef __VIDEOSTREAM_H
#define __VIDEOSTREAM_H

#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "codec_video.h"
#include "videorender.h"
#include "pes.h"

#define VIDEO_BUFFER_SIZE (512 * 1024)  ///< video PES buffer default size
#define VIDEO_PACKET_MAX 192            ///< max number of video packets held in the buffer

class cVideoDecoder;
class cVideoRender;

/**
 * cVideoStream - Video stream class
 */
class cVideoStream
{
public:
	cVideoStream(cSoftHdDevice *);
	virtual ~cVideoStream(void);

	void Open(void) { m_newStream = true; };
	void Exit(void);
	void ClearPacketQueue(void);
	void FlushDecoder(void);
	void CloseDecoder(void);
	int DecodeInput(void);
	void Start(void) { m_closing = false; };
	void Stop(void);
	bool IsClosing(void) { return m_closing; };
	void Resume(void) { m_paused = false; };
	void Pause(void);
	bool IsPaused(void) { return m_paused; };
	void PushPesPacket(cPes *pesPacket);
	bool PushAvPacket(AVPacket *avpkt);
	void ResetFragmentationBuffer(void);

	// getters and setters
	cVideoDecoder *Decoder(void) { return m_pDecoder; };
	void StartDecoder(cVideoDecoder *decoder);
	void SetCodecId(enum AVCodecID id) { m_codecId = id; };
	void SetParameters(AVCodecParameters *par) { m_pPar = par; };
	void SetTimebase(int, int);
	void SetTrickpkts(int pkts) { m_trickpkts = pkts; };
	void SetInterlaced(bool interlaced);
	size_t GetAvPacketsFilled(void) { return m_packets.Size(); };
	enum AVCodecID GetCodecId(void) { return m_codecId; };

private:
	cVideoDecoder *m_pDecoder;             ///< video decoder
	cVideoRender *m_pRender;               ///< video renderer

	cQueue<AVPacket> m_packets{VIDEO_PACKET_MAX}; ///< AVPackets queue
	std::vector<uint8_t> m_currentCodecPacket;    ///< fragmentation buffer
	int64_t m_currentPacketPts = AV_NOPTS_VALUE;  ///< PTS of the currently receiving codec packet

	enum AVCodecID m_codecId;              ///< current codec id
	AVCodecParameters *m_pPar = nullptr;   ///< current codec parameters
	struct AVRational m_timebase;          ///< current codec timebase
	int m_trickpkts;                       ///< how many avpkt does the decoder need in trickspeed mode?
	int m_sentTrickPkts;                   ///< how many avpkt have been sent to the decoder in trickspeed mode?

	volatile bool m_newStream;             ///< flag for new stream
	volatile bool m_closing;               ///< flag for closing request
	volatile bool m_paused;                ///< flag for paused stream
	bool m_interlaced;                     ///< flag for interlaced stream
	cCondWait m_closeCondition;            ///< condition object to wait for finishing jobs while closing
	cCondVar m_pauseCondition;             ///< condition object to wait for pausing the stream

	int RenderTrickspeedFrames(AVFrame *);
};

#endif
