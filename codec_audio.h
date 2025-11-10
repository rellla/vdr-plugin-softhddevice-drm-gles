/**
 * @file codec_audio.h
 * Audio decoder header file
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

#ifndef __CODEC_AUDIO_H
#define __CODEC_AUDIO_H

extern "C" {
#include <libavcodec/avcodec.h>
}

/**
 * bits used for the passthrough mask
 */
#define CODEC_PCM  0x01      ///< PCM bit mask
#define CODEC_MPA  0x02      ///< MPA bit mask (not used)
#define CODEC_AC3  0x04      ///< AC-3 bit mask
#define CODEC_EAC3 0x08      ///< E-AC-3 bit mask
#define CODEC_DTS  0x10      ///< DTS bit mask

/**
 * IEC Data type enumeration
 */
enum IEC61937
{
	IEC61937_NULL = 0x00,      ///< no data
	IEC61937_AC3 = 0x01,       ///< AC-3 data
	IEC61937_EAC3 = 0x15,      ///< E-AC-3 data
	IEC61937_DTS1 = 0x0B,      ///< DTS type I (512 samples)
	IEC61937_DTS2 = 0x0C,      ///< DTS type II (1024 samples)
	IEC61937_DTS3 = 0x0D,      ///< DTS type III (2048 samples)
	IEC61937_DTSHD = 0x11,     ///< DTS HD data (not used)
	IEC61937_TRUEHD = 0x16,	   ///< TrueHD data (not used)
};

#define IEC61937_PREAMBLE1      0xF872
#define IEC61937_PREAMBLE2      0x4E1F
#define DTS_PREAMBLE_16BE_1     0x7FFE
#define DTS_PREAMBLE_16BE_2     0x8001

/**
 * Codec frame sizes
 */
#define DTS1_FRAME_SIZE          512
#define DTS2_FRAME_SIZE         1024
#define DTS3_FRAME_SIZE         2048
#define AC3_FRAME_SIZE          1536
#define EAC3_FRAME_SIZE         6144
#define TRUEHD_FRAME_SIZE      15360    ///< (not used)

#define MAX_FRAME_SIZE EAC3_FRAME_SIZE

class cSoftHdAudio;

/**
 * cAudioDecoder - Audio decoder class
 */
class cAudioDecoder {
public:
	cAudioDecoder(cSoftHdAudio *);
	virtual ~cAudioDecoder(void);
	void Open(AVCodecID, AVCodecParameters * = nullptr, AVRational = { .num = 1, .den = 90000 });
	void Close(void);
	void Decode(const AVPacket *);
	void FlushBuffers(void);
	void SetPassthrough(int);
	AVCodecID GetCodecId() const { return m_codecId; };

private:
	cSoftHdAudio *m_pAudio;                     ///< audio module
	AVCodecContext *m_pAudioCtx;                ///< ffmpeg audio codec context
	AVCodecID m_codecId = AV_CODEC_ID_NONE;     ///< current codec id
	AVFrame *m_pFrame;                          ///< decoded ffmpeg audio frame
	int64_t m_lastPts;                          ///< last seen PTS
	int64_t m_initialAvpktPts;                  ///< PTS of the first valid avpkt
	int m_passthroughMask;                      ///< passthrough mask to be set
	int m_currentPassthrough;                   ///< current passthrough mask
	int m_currentSampleRate;                    ///< current sample rate
	int m_currentNumChannels;                   ///< current number of channels
	int m_currentHwSampleRate;                  ///< current hw sample rate
	int m_currentHwNumChannels;                 ///< current number of hw channels
	uint16_t m_spdifOutput[MAX_FRAME_SIZE * 2]; ///< SPDIF output buffer
	int m_spdifIndex;                           ///< index into SPDIF output buffer
	int m_spdifRepeatCount;                     ///< SPDIF repeat counter

	int DecodePassthrough(const AVPacket *, AVFrame *);
	int UpdateFormat(void);
};

#endif
