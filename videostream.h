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

	void Open(AVCodecID, AVCodecParameters * = nullptr, AVRational = { .num = 1, .den = 90000 });
	void Exit(void);
	void ClearVdrCoreToDecoderQueue(void);
	void FlushDecoder(void);
	void CloseDecoder(void);
	void DecodeInput(void);
	bool PushAvPacket(AVPacket *avpkt);
	void Flush(void);

	// getters and setters
	cVideoDecoder *Decoder(void) { return m_pDecoder; };
	void StartDecoder(cVideoDecoder *decoder);
	void SetInterlaced(bool interlaced);
	bool IsInterlaced(void) { return m_interlaced; };
	size_t GetAvPacketsFilled(void) { return m_packets.Size(); };
	enum AVCodecID GetCodecId(void) { return m_codecId; };
	void ResetTrickSpeedFramesSentCounter(void) { m_sentTrickPkts = 0; };

	// decoding thread
	void CreateDecodingThread(void);
	void ExitDecodingThread(void);
	void DecodingThreadHalt(void) { m_pDecodingThread->Halt(); };
	void DecodingThreadResume(void) { m_pDecodingThread->Resume(); };

private:
	cVideoDecoder *m_pDecoder;             ///< video decoder
	cVideoRender *m_pRender;               ///< video renderer
	cDecodingThread *m_pDecodingThread;    ///< pointer to decoding thread

	cQueue<AVPacket> m_packets{VIDEO_PACKET_MAX}; ///< AVPackets queue
	std::vector<uint8_t> m_currentCodecPacket;    ///< fragmentation buffer
	int64_t m_currentPacketPts = AV_NOPTS_VALUE;  ///< PTS of the currently receiving codec packet

	enum AVCodecID m_codecId;              ///< current codec id
	AVCodecParameters *m_pPar = nullptr;   ///< current codec parameters
	struct AVRational m_timebase;          ///< current codec timebase
	int m_trickpkts;                       ///< how many avpkt does the decoder need in trickspeed mode?
	int m_sentTrickPkts = 0;               ///< how many avpkt have been sent to the decoder in trickspeed mode?

	volatile bool m_newStream;             ///< flag for new stream
	bool m_interlaced;                     ///< flag for interlaced stream
};

#endif
