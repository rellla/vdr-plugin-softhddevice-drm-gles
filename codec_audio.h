// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file codec_audio.h
 * Audio Decoder Header File
 *
 * @copyright 2009 - 2013, 2015 by Johns.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __CODEC_AUDIO_H
#define __CODEC_AUDIO_H

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class cSoftHdAudio;

/**
 * @defgroup audiodecoder Audio Decoder
 * FFmpeg Based Audio Decoder Frontend
 */

/**
 * Bits used for the passthrough mask
 *
 * 0x01 and 0x02 are kept unused for compatibility with an existing setup.conf
 *
 * @ingroup audiodecoder
 */
enum PassthroughMask {
	CODEC_AC3  = (1 << 2), ///< AC-3 bit mask
	CODEC_EAC3 = (1 << 3), ///< E-AC-3 bit mask
	CODEC_DTS  = (1 << 4), ///< DTS bit mask
};

/**
 * Audio Decoder
 *
 * FFmpeg Based Audio Decoder Frontend
 *
 * Handles:
 * - Audio packet decoding using FFmpeg
 * - SPDIF passthrough
 * - Format changes
 * - Audio frame delivery to cSoftHdAudio
 *
 * @ingroup audiodecoder
 */
class cAudioDecoder {
public:
	cAudioDecoder(cSoftHdAudio *);
	~cAudioDecoder(void);
	void Open(AVCodecID, AVCodecParameters * = nullptr, AVRational = { .num = 1, .den = 90000 });
	void Close(void);
	void Decode(const AVPacket *);
	void FlushBuffers(void);
	void SetPassthroughMask(int);
	AVCodecID GetCodecId() const { return m_codecId; };

private:
	cSoftHdAudio *m_pAudio;                     ///< audio module
	AVCodecContext *m_pAudioCtx = nullptr;      ///< ffmpeg audio codec context
	AVCodecID m_codecId = AV_CODEC_ID_NONE;     ///< current codec id
	AVFrame *m_pFrame;                          ///< decoded ffmpeg audio frame
	int64_t m_lastPts = AV_NOPTS_VALUE;         ///< last seen PTS
	int m_passthroughMask;                      ///< passthrough mask to be set
	int m_currentPassthroughMask;               ///< current passthrough mask
	int m_currentSampleRate;                    ///< current sample rate
	int m_currentNumChannels;                   ///< current number of channels
	int m_currentHwSampleRate;                  ///< current hw sample rate
	int m_currentHwNumChannels;                 ///< current number of hw channels
	std::mutex m_mutex;                         ///< decoder mutex

	std::array<uint8_t, 32768> m_spdifIoBuffer; ///< spdif I/O buffer
	AVFormatContext *m_spdifFmtCtx = nullptr;   ///< spdif muxer context
	std::vector<uint8_t> m_spdifOutputBuf;      ///< spdif muxer output

	constexpr static int AUDIO_PASSTHROUGH_NUM_CHANNELS = 2; ///< fixed passthrough channel number
	constexpr static int AUDIO_PASSTHROUGH_RATE_HZ = 48000;  ///< fixed passthrough sample rate

	bool OpenSpdifMuxer(AVCodecID, int);
	void CloseSpdifMuxer(void);
	const std::vector<uint8_t> &BuildIEC61937(const AVPacket *);
#if LIBAVFORMAT_VERSION_MAJOR >= 61
	static int SpdifWriteCallback(void *, const uint8_t *, int);
#else
	static int SpdifWriteCallback(void *, uint8_t *, int);
#endif

	bool ShouldTryPassthrough(void);
	int Passthrough(const AVPacket *);
	void DecodePCM(const AVPacket *);
	int CheckUpdateFormat(bool);
};

#endif
