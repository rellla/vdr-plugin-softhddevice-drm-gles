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

	virtual void DecodeInput(void) = 0;
	virtual bool IsInterlaced(void) { return false; };

	void Open(AVCodecID, AVCodecParameters * = nullptr, AVRational = { .num = 1, .den = 90000 });
	void Exit(void);
	void ClearVdrCoreToDecoderQueue(void);
	void FlushDecoder(void);
	void CloseDecoder(void);
	bool PushAvPacket(AVPacket *avpkt);
	void Flush(void);

	// getters and setters
	cVideoDecoder *Decoder(void) { return m_pDecoder; };
	void StartDecoder(cVideoDecoder *decoder);
	size_t GetAvPacketsFilled(void) { return m_packets.Size(); };
	enum AVCodecID GetCodecId(void) { return m_codecId; };
	void SetVideoSize(int, int);
	void GetVideoSize(int *, int *, double *);

	// decoding thread
	void CreateDecodingThread(void);
	void ExitDecodingThread(void);
	void DecodingThreadHalt(void) { m_pDecodingThread->Halt(); };
	void DecodingThreadResume(void) { m_pDecodingThread->Resume(); };

protected:
	cVideoDecoder *m_pDecoder;             ///< video decoder
	cVideoRender *m_pRender;               ///< video renderer

	cQueue<AVPacket> m_packets{VIDEO_PACKET_MAX}; ///< AVPackets queue

	enum AVCodecID m_codecId;              ///< current codec id
	AVCodecParameters *m_pPar = nullptr;   ///< current codec parameters
	struct AVRational m_timebase;          ///< current codec timebase
	volatile bool m_newStream;             ///< flag for new stream
	bool m_isPipStream = false;            ///< true if this is the pip stream

	void TryInitDecoder(void);             ///< init/ open the decoder if necessary
	bool CanDecodePacket(void);            ///< is the codec open and we have packets?

private:
	cDecodingThread *m_pDecodingThread;    ///< pointer to decoding thread
	int m_videoWidth;                      ///< current video width (set by decoder)
	int m_videoHeight;                     ///< current video height (set by decoder)
	std::mutex m_mutex;                    ///< mutex to lock video size (which is accessed by different threads)
};

/**
 * cMainVideoStream - Main video stream class
 */
class cMainVideoStream : public cVideoStream
{
public:
	cMainVideoStream(cSoftHdDevice *);
	virtual ~cMainVideoStream(void);

	void DecodeInput(void);
	void SetInterlaced(bool);
	bool IsInterlaced(void) { return m_interlaced; };
	void ResetTrickSpeedFramesSentCounter(void) { m_sentTrickPkts = 0; };

private:
	int m_sentTrickPkts = 0;               ///< how many avpkt have been sent to the decoder in trickspeed mode?
	int m_trickpkts = 1;                   ///< how many avpkt does the decoder need in trickspeed mode?
	bool m_interlaced = 0;                 ///< flag for interlaced stream
};

/**
 * cPipVideoStream - Pip video stream class
 */
class cPipVideoStream : public cVideoStream
{
public:
	cPipVideoStream(cSoftHdDevice *);
	virtual ~cPipVideoStream(void);

	void DecodeInput(void);
};

#endif
