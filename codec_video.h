/**
 * @file codec_video.h
 * Video decoder header file
 *
 * @copyright (c) 2009 - 2013, 2015 by Johns.  All Rights Reserved.
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

#ifndef __CODEC_VIDEO_H
#define __CODEC_VIDEO_H

#include <pthread.h>
#include <vdr/thread.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

class cVideoStream;

/**
 * cVideoDecoder - VideoDecoder class
 */
class cVideoDecoder {
public:
	cVideoDecoder(cVideoStream *, int);
	virtual ~cVideoDecoder(void);
	int Open(enum AVCodecID, AVCodecParameters *, AVRational *, int, int, int);
	void Close(void);
	int SendPacket(const AVPacket *);
	int ReceiveFrame(AVFrame **);
	void FlushBuffers(void);
	int ReopenCodec(enum AVCodecID, AVCodecParameters *, AVRational *, int);
	AVCodecContext *GetContext(void) { return m_pVideoCtx; };

private:
	cVideoStream *m_pVideoStream;           ///< video stream, the decoder belongs to
	AVCodecContext *m_pVideoCtx = nullptr;  ///< video codec context
	cMutex m_mutex;                         ///< mutex to lock codec context (TODO: is this needed?)
	int m_cntPacketsSent;                   ///< number of packets sent to decoder
	int m_cntFramesReceived;                ///< number of decoded frames received from decoder
	int m_cntStartKeyFrames;                ///< number of keyframes arrived while starting the coded
	                                        ///< (needed for amlogic h264 decoder in order to drop some frames
	                                        ///< in ReceiveFrame() before sending them to the renderer)
	int m_lastCodedWidth;                   ///< save coded width while closing for a directly reopen
	int m_lastCodedHeight;                  ///< save coded height while closing for a directly reopen
	int m_hardwareQuirks;                   ///< hardware specific quirks needed for decoder

	int GetExtraData(const AVPacket *);
	bool IsKeyFrame(AVFrame *);
	bool IsInterlacedFrame(AVFrame *);
};

#endif
