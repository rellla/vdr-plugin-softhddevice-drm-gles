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

#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
}

class cSoftHdAudio;

/**
 * @addtogroup audiodecoder
 * @{
 */

/**
 * Bits used for the passthrough mask
 *
 * 0x01 and 0x02 are kept unused for compatibility with an existing setup.conf
 */
enum PassthroughMask {
	CODEC_AC3  = (1 << 2), ///< AC-3 bit mask
	CODEC_EAC3 = (1 << 3), ///< E-AC-3 bit mask
	CODEC_DTS  = (1 << 4), ///< DTS bit mask
};

/**
 * IEC Data type
 */
enum IEC61937Type {
	IEC61937_NULL   = 0x00, ///< no data
	IEC61937_AC3    = 0x01, ///< AC-3 data
	IEC61937_EAC3   = 0x15, ///< E-AC-3 data
	IEC61937_DTS1   = 0x0B, ///< DTS type I (512 samples)
	IEC61937_DTS2   = 0x0C, ///< DTS type II (1024 samples)
	IEC61937_DTS3   = 0x0D, ///< DTS type III (2048 samples)
	IEC61937_DTSHD  = 0x11, ///< DTS HD data (not used)
	IEC61937_TRUEHD = 0x16, ///< TrueHD data (not used)
};

/**
 * IEC Preambles
 */
enum IEC61937Preamble {
	IEC61937_PREAMBLE1  = 0xF872,
	IEC61937_PREAMBLE2  = 0x4E1F,
	DTS_PREAMBLE_16BE_1 = 0x7FFE,
	DTS_PREAMBLE_16BE_2 = 0x8001,
};

/**
 * Codec frame sizes for spdif
 */
enum CodecFrameSizes {
	DTS1_FRAME_SIZE   = 512,
	DTS2_FRAME_SIZE   = 1024,
	DTS3_FRAME_SIZE   = 2048,
	AC3_FRAME_SIZE    = 1536,
	EAC3_FRAME_SIZE   = 6144,
	MAX_FRAME_SIZE    = EAC3_FRAME_SIZE,

	TRUEHD_FRAME_SIZE = 15360, ///< (not used)
};

/**
 * Audio Decoder
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
	int m_currentPassthrough;                   ///< current passthrough mask
	int m_currentSampleRate;                    ///< current sample rate
	int m_currentNumChannels;                   ///< current number of channels
	int m_currentHwSampleRate;                  ///< current hw sample rate
	int m_currentHwNumChannels;                 ///< current number of hw channels
	uint16_t m_spdifOutput[(MAX_FRAME_SIZE * 4 + 16) / 2]; ///< SPDIF output buffer
	int m_spdifIndex;                           ///< index into SPDIF output buffer
	int m_spdifRepeatCount;                     ///< SPDIF repeat counter

	int DecodePassthrough(const AVPacket *, AVFrame *);
	int UpdateFormat(void);
	void ResetSpdif(void);
};

/** @} */

#endif
