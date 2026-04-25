// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file videostream.h
 * Video Input Stream Header File
 *
 * @copyright 2011 - 2015 by Johns.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __VIDEOSTREAM_H
#define __VIDEOSTREAM_H

#include <atomic>
#include <functional>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <vdr/thread.h>

#include "queue.h"
#include "videofilter.h"

class cDrmBuffer;
class cSoftHdConfig;
class cVideoDecoder;
class cVideoRender;

/**
 * Video Input Stream
 *
 * @defgroup video Video Input Stream
 */

/**
 * Video Input Stream
 *
 * @ingroup video
 */
class cVideoStream : public cThread {
public:
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

	// decoding thread
	void Stop(void);
	void Halt(void) { m_mutex.lock(); };
	void Resume(void) { m_mutex.unlock(); };

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
	void ResetInputPts(void) { m_inputPts = AV_NOPTS_VALUE; };
	void GetVideoSize(int *, int *, double *);
	int GetVideoPacketMax(void) { return VIDEO_PACKET_MAX; };

	// Filter
	void CancelFilterThread(void);
	void ResetFilterThreadNeededCheck() { m_checkFilterThreadNeeded = true; m_useDeinterlacer = false; };

	virtual void SetDeinterlacerDeactivated(bool deactivate) { m_deinterlacerDeactivated = deactivate; };
	bool IsDeinterlacerDeactivated(void) { return m_deinterlacerDeactivated; };
	void DisableDeint(bool disable) { m_userDisabledDeinterlacer = disable; };
	void SetStartDecodingWithIFrame(bool enable) { m_startDecodingWithIFrame = enable; };
	void SetParseH264Dimensions(bool enable) { m_parseH264Dimensions = enable; };
	void SetDecoderFallbackToSwNumPkts(int numPackets) { m_decoderFallbackToSwNumPkts = numPackets; };

protected:
	cVideoStream(cVideoRender *, int, cQueue<cDrmBuffer> *, cSoftHdConfig *, bool, std::function<void(AVFrame *)>);
	virtual void Action(void);

private:
	cSoftHdConfig *m_pConfig;           ///< plugin config
	cVideoDecoder *m_pDecoder;          ///< video decoder
	cVideoRender *m_pRender;            ///< video renderer
	const char *m_identifier;           ///< identifier string for logging
	std::function<void(AVFrame *)> m_frameOutput;   ///< function to output the frame
	cQueue<cDrmBuffer> *m_pDrmBufferQueue;          ///< pointer to renderer's DRM buffer queue
	cVideoFilter m_videoFilter;         ///< pointer to deinterlace/scaling video filter thread
	std::mutex m_mutex;                 ///< mutex for decoding thread control

	bool m_checkFilterThreadNeeded;                 ///< set, if we have to check, if filter thread is needed at start of playback
	int m_hardwareQuirks;                           ///< hardware specific quirks
	bool m_userDisabledDeinterlacer = false;        ///< set, if the user configured the deinterlace to be disabled
	bool m_deinterlacerDeactivated;                 ///< set, if the deinterlacer should be disabled temporarily (trickspeed, stillpicture, pip)
	bool m_useDeinterlacer = false;                 ///< set, if the deinterlacer is used
	bool m_startDecodingWithIFrame = false;         ///< wait for an I-Frame to start h264 decoding
	bool m_parseH264Dimensions = false;             ///< parse width and height when starting an h264 stream
	int m_decoderFallbackToSwNumPkts = 22;          ///< fallback to sw decoder if hw decoder fails after the given number of packets sent

	constexpr static int VIDEO_PACKET_MAX = 192;    ///< max number of video packets held in the buffer
	cQueue<AVPacket> m_packets{VIDEO_PACKET_MAX};   ///< AVPackets queue

	enum AVCodecID m_codecId = AV_CODEC_ID_NONE;    ///< current codec id
	AVCodecParameters *m_pPar = nullptr;            ///< current codec parameters
	std::atomic<struct AVRational> m_timebase;      ///< current codec timebase
	int m_trickpkts;                                ///< how many avpkt does the decoder need in trickspeed mode?
	int m_sentTrickPkts = 0;                        ///< how many avpkt have been sent to the decoder in trickspeed mode?
	volatile bool m_newStream = false;              ///< flag for new stream
	bool m_interlaced;                              ///< flag for interlaced stream
	double m_framerate = 0.0;                       ///< current stream framerate

	int64_t m_inputPts = AV_NOPTS_VALUE;            ///< PTS of the first packet in the input buffer
	int64_t m_lastPts = AV_NOPTS_VALUE;             ///< helper PTS to calculate a framerate at stream start

	// h264 parsing
	std::vector<std::string> m_naluTypesAtStart;    ///< array of strings to log the H.264 frames at stream start
	int m_numIFrames = 0;                           ///< counter for the arriving I-Frames at H.264 stream start
	int m_logPackets = 0;                           ///< parse and log all frames until the number of given I-Frames arrived
	int m_dropInvalidPackets = 0;                   ///< drop P-Frames with invalid references until the given number of I-Frames arrived
	std::set<int> m_dpbFrames;                      ///< private set of reference frames (internal short-time decoded picture buffer)
	int m_maxFrameNum = 1;                          ///< = 1 << Log2MaxFrameNumMinus4 + 4
	int m_log2MaxFrameNumMinus4 = -4;               ///< cache Log2MaxFrameNumMinus4 from a previous SPS parsing
	int m_ppsNumRefIdxL0DefaultActiveMinus1 = -1;   ///< cache NumRefIdxL0DefaultActiveMinuns1 from a previous PPS parsing
	int m_ppsNumRefIdxL1DefaultActiveMinus1 = -1;   ///< cache NumRefIdxL1DefaultActiveMinuns1 from a previous PPS parsing
	bool m_isResend = false;                        ///< track, if we already tried to send the AVPacket to the decoder
	                                                ///< if so, skip the parsing

	void RenderFrame(AVFrame *);
	void CheckForcingFrameDecode(void);
	void OpenDecoder(void);
	bool ParseH264Packet(AVPacket *);
};

/**
 * Main Video Stream
 *
 * @ingroup video
 */
class cMainVideoStream : public cVideoStream {
public:
	cMainVideoStream(cVideoRender *render, int hardwareQuirks, cQueue<cDrmBuffer> *buf, cSoftHdConfig *config, std::function<void(AVFrame *)> fn)
		: cVideoStream(render, hardwareQuirks, buf, config, false, fn) {};
};

/**
 * PiP Video Stream
 *
 * @ingroup video
 */
class cPipVideoStream : public cVideoStream {
public:
	cPipVideoStream(cVideoRender *render, int hardwareQuirks, cQueue<cDrmBuffer> *buf, cSoftHdConfig *config, std::function<void(AVFrame *)> fn)
		: cVideoStream(render, hardwareQuirks, buf, config, true, fn) {};
	void SetDeinterlacerDeactivated(bool) override {}; // deinterlacing is permanently disabled
};

#endif
