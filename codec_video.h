// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file codec_video.h
 * Video Decoder Header File
 *
 * @copyright 2009 - 2013, 2015 by Johns.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __CODEC_VIDEO_H
#define __CODEC_VIDEO_H

#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
}

/**
 * FFmpeg Based Video Decoder Frontend
 * @defgroup videodecoder VideoDecoder
 */

/**
 * Video Decoder
 *
 * FFmpeg based Video Decoder Frontend
 *
 * Handles:
 * - Video packet decoding using FFmpeg
 *
 * @ingroup videodecoder
 */
class cVideoDecoder {
public:
	cVideoDecoder(int, const char *);
	int Open(enum AVCodecID, AVCodecParameters *, AVRational, bool, int, int);
	void Close(void);
	int SendPacket(const AVPacket *);
	int ReceiveFrame(AVFrame **);
	void FlushBuffers(void);
	int ReopenCodec(enum AVCodecID, AVCodecParameters *, AVRational, int);
	AVCodecContext *GetContext(void) { return m_pVideoCtx; };
	bool IsHardwareDecoder(void) { return m_isHardwareDecoder; };
	const char *Name(void) { return m_pCodecString; };
	int GetPacketsSent(void) { return m_cntPacketsSent; };
	int GetFramesReceived(void) { return m_cntFramesReceived; };

private:
	AVCodecContext *m_pVideoCtx = nullptr;  ///< video codec context
	const char *m_identifier;               ///< identifier for logging
	const char *m_pCodecString = "unknown"; ///< codec (long) name string
	std::mutex m_mutex;                     ///< mutex to lock codec context
	int m_cntPacketsSent;                   ///< number of packets sent to decoder
	int m_cntFramesReceived;                ///< number of decoded frames received from decoder
	int m_cntStartKeyFrames;                ///< number of keyframes arrived while starting the coded
	                                        ///< (needed for amlogic h264 decoder in order to drop some frames
	                                        ///< in ReceiveFrame() before sending them to the renderer)
	int m_lastCodedWidth;                   ///< save coded width while closing for a directly reopen
	int m_lastCodedHeight;                  ///< save coded height while closing for a directly reopen
	int m_hardwareQuirks;                   ///< hardware specific quirks needed for decoder
	bool m_isHardwareDecoder = false;       ///< true, if this is a hardware decoder

	int GetExtraData(const AVPacket *);
	bool IsKeyFrame(AVFrame *);
};

#endif
