///
///	@file codec.h	@brief Codec module headerfile
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
///	$Id$
//////////////////////////////////////////////////////////////////////////////

/// @addtogroup Codec
/// @{

#ifndef __CODEC_H
#define __CODEC_H

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define CodecPCM 0x01			///< PCM bit mask
#define CodecMPA 0x02			///< MPA bit mask (planned)
#define CodecAC3 0x04			///< AC-3 bit mask
#define CodecEAC3 0x08			///< E-AC-3 bit mask
#define CodecDTS 0x10			///< DTS bit mask (planned)

//----------------------------------------------------------------------------
//	Video
//----------------------------------------------------------------------------
///
///	Video decoder structure.
///
struct _video_decoder_
{
    VideoRender *Render;		///< video hardware decoder

    AVCodecContext *VideoCtx;		///< video codec context
};

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------

    /// Video decoder typedef.
typedef struct _video_decoder_ VideoDecoder;

    /// Audio decoder typedef.
typedef struct _audio_decoder_ AudioDecoder;

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Allocate a new video decoder context.
extern VideoDecoder *CodecVideoNewDecoder(VideoRender *);

    /// Deallocate a video decoder context.
extern void CodecVideoDelDecoder(VideoDecoder *);

	/// Get VideoContext
extern  AVCodecContext *Codec_get_VideoContext(VideoDecoder *);

    /// Open video codec.
extern void CodecVideoOpen(VideoDecoder *, int, AVCodecParameters *, AVRational *);

    /// Close video codec.
extern void CodecVideoClose(VideoDecoder *);

    /// Decode a video packet.
extern int CodecVideoSendPacket(VideoDecoder *, const AVPacket *);

extern int CodecVideoReceiveFrame(VideoDecoder *, int);

    /// Flush video buffers.
extern void CodecVideoFlushBuffers(VideoDecoder *);


    /// Allocate a new audio decoder context.
extern AudioDecoder *CodecAudioNewDecoder(void);

    /// Deallocate an audio decoder context.
extern void CodecAudioDelDecoder(AudioDecoder *);

    /// Open audio codec.
extern void CodecAudioOpen(AudioDecoder *, int, AVCodecParameters *, AVRational *);

    /// Close audio codec.
extern void CodecAudioClose(AudioDecoder *);

    /// Set audio pass-through.
extern void CodecSetAudioPassthrough(int);

    /// Decode an audio packet.
extern void CodecAudioDecode(AudioDecoder *, const AVPacket *);

    /// Flush audio buffers.
extern void CodecAudioFlushBuffers(AudioDecoder *);

    /// Setup and initialize codec module.
extern void CodecInit(void);

    /// Cleanup and exit codec module.
extern void CodecExit(void);

/// @}

#endif
