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

#include <atomic>
#include <functional>
#include <vector>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include "codec_video.h"
#include "videorender.h"
#include "pes.h"
#include "threads.h"
#include "drmbuffer.h"
#include "queue.h"

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
	cVideoStream(cVideoRender *, cQueue<cDrmBuffer> *, cSoftHdConfig *, const char *, std::function<void(AVFrame *)>);
	virtual ~cVideoStream(void);

	void DecodeInput(void);
	bool IsInterlaced(void) { return false; };

	void Open(AVCodecID, AVCodecParameters * = nullptr, AVRational = { .num = 1, .den = 90000 });
	void Exit(void);
	void ClearVdrCoreToDecoderQueue(void);
	void FlushDecoder(void);
	void CloseDecoder(void);
	bool PushAvPacket(AVPacket *avpkt);
	void Flush(void);

	// getters and setters
	cVideoDecoder *Decoder(void) { return m_pDecoder; };
	void StartDecoder();
	size_t GetAvPacketsFilled(void) { return m_packets.Size(); };
	bool IsInputBufferFull(void) { return m_packets.Size() >= VIDEO_PACKET_MAX; };
	enum AVCodecID GetCodecId(void) { return m_codecId; };
	void ResetTrickSpeedFramesSentCounter(void) { m_sentTrickPkts = 0; };
	bool HasInputPts(void) { return m_inputPts != AV_NOPTS_VALUE; }
	int64_t GetInputPtsMs(void);
	int64_t GetInputPts(void) { return m_inputPts; };
	void GetVideoSize(int *, int *, double *);

	// decoding thread
	void ExitDecodingThread(void);
	void DecodingThreadHalt(void) { m_pDecodingThread->Halt(); };
	void DecodingThreadResume(void) { m_pDecodingThread->Resume(); };

	// Filter
	void CancelFilterThread(void);
	void ResetFilterThreadNeededCheck() { m_checkFilterThreadNeeded = true; };

	void SetDeinterlacerDeactivated(bool deactivate) { m_deinterlacerDeactivated = deactivate; };
	bool IsDeinterlacerDeactivated(void) { return m_deinterlacerDeactivated; };
	int HardwareQuirks(void) { return m_hardwareQuirks; };
	void DisableDeint(bool disable) { m_userDisabledDeinterlacer = disable; };

private:
	cVideoDecoder *m_pDecoder;          ///< video decoder
	cVideoRender *m_pRender;            ///< video renderer
	cFilterThread *m_pFilterThread;     ///< pointer to deinterlace filter thread
	const char *m_identifier;           ///< identifier string for logging
	std::string m_filterThreadName;     ///< filter thread name string (persists for object lifetime)
	std::string m_decodingThreadName;   ///< decoding thread name string (persists for object lifetime)
	std::function<void(AVFrame *)> m_frameOutput;   ///< function to output the frame
	cQueue<cDrmBuffer> *m_pDrmBufferQueue;          ///< pointer to renderer's DRM buffer queue

	bool m_checkFilterThreadNeeded;     ///< set, if we have to check, if filter thread is needed at start of playback
	int m_hardwareQuirks;               ///< hardware specific quirks
	bool m_userDisabledDeinterlacer = false; ///< set, if the user configured the deinterlace to be disabled
	bool m_deinterlacerDeactivated = false; ///< set, if the deinterlacer shall be deactivated temporarily (used for trick speed and still picture)

	cQueue<AVPacket> m_packets{VIDEO_PACKET_MAX}; ///< AVPackets queue

	enum AVCodecID m_codecId = AV_CODEC_ID_NONE;  ///< current codec id
	AVCodecParameters *m_pPar = nullptr;   ///< current codec parameters
	std::atomic<struct AVRational> m_timebase;          ///< current codec timebase
	int m_trickpkts;                       ///< how many avpkt does the decoder need in trickspeed mode?
	int m_sentTrickPkts = 0;               ///< how many avpkt have been sent to the decoder in trickspeed mode?
	volatile bool m_newStream;             ///< flag for new stream
	bool m_interlaced;                     ///< flag for interlaced stream

	cDecodingThread *m_pDecodingThread;    ///< pointer to decoding thread
	int64_t m_inputPts = AV_NOPTS_VALUE;   ///< PTS of the first packet in the input buffer
	std::mutex m_mutex;  // TODO                  ///< mutex to lock video size (which is accessed by different threads)

	void RenderFrame(AVFrame *);
};

#endif
