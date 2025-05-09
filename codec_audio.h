///
///	@file codec_audio.h	@brief Audio decoder module headerfile
///
///	Copyright (c) 2009 - 2013, 2015 by Johns.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
//////////////////////////////////////////////////////////////////////////////

/// @addtogroup Codec
/// @{

#ifndef __CODEC_AUDIO_H
#define __CODEC_AUDIO_H

#include <libavcodec/avcodec.h>

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define CodecPCM  	0x01			///< PCM bit mask
#define CodecMPA  	0x02			///< MPA bit mask (planned)
#define CodecAC3  	0x04			///< AC-3 bit mask
#define CodecEAC3 	0x08			///< E-AC-3 bit mask
#define CodecDTS  	0x10			///< DTS bit mask (planned)
#define CodecTrueHD	0x16			///< TrueHD bit mask	

//----------------------------------------------------------------------------
//	Variables and enums
//----------------------------------------------------------------------------

///
///	IEC Data type enumeration.
///
enum IEC61937
{
    IEC61937_AC3 = 0x01,		///< AC-3 data
    IEC61937_EAC3 = 0x15,		///< E-AC-3 data
    IEC61937_DTS1 = 0x0B,		///< DTS type I (512 samples)
    IEC61937_DTS2 = 0x0C,		///< DTS type II (1024 samples)
    IEC61937_DTS3 = 0x0D,		///< DTS type III (2048 samples)
    IEC61937_DTSHD = 0x11,		///< DTS HD data
    IEC61937_TRUEHD = 0x16,		///< TrueHD data
};

//----------------------------------------------------------------------------
//	Audio
//----------------------------------------------------------------------------

/**
**	cAudioDecoder - AudioDecoder class
*/

class cAudioDecoder {
private:
    AVCodecContext *AudioCtx;		///< audio codec context
    AVFrame *Frame;			///< decoded audio frame buffer
    int64_t last_pts;			///< last PTS
    int64_t initial_pts;		///< initial PTS
    int PassthroughMask;		///< set audio passthrough mask

    int Passthrough;			///< current passthrough mask
    int SampleRate;			///< current sample rate
    int Channels;			///< current channels

    int HwSampleRate;			///< hw sample rate
    int HwChannels;			///< hw channels

    uint16_t Spdif[24576 / 2];		///< SPDIF output buffer
    int SpdifIndex;			///< index into SPDIF output buffer
    int SpdifCount;			///< SPDIF repeat counter

    int DecodePassthrough(const AVPacket *, AVFrame *);	///< try to decode passthrough
    int UpdateFormat(void);		///< update format

public:
    cAudioDecoder(int);
    virtual ~cAudioDecoder(void);
    void Open(enum AVCodecID, AVCodecParameters *, AVRational *);
	///< Open audio decoder
    void Close(void);
	///< Close audio decoder
    void Decode(const AVPacket *);
	///< Decode an audio packet
    void FlushBuffers(void);
	///< Flush audio buffers
    void SetPassthrough(int);
	/// set pass-through mask
};
#endif
/// @}
