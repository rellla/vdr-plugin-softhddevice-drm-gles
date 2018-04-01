///
///	@file video.h	@brief Video module header file
///
///	Copyright (c) 2009 - 2015 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 by zille.  All Rights Reserved.
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

/// @addtogroup Video
/// @{

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------

    /// Video hardware decoder typedef
typedef struct _Drm_decoder_ VideoHwDecoder;

    /// Video output stream typedef
typedef struct __video_stream__ VideoStream;

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

extern signed char VideoHardwareDecoder;	///< flag use hardware decoder
extern int VideoAudioDelay;		///< audio/video delay

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Allocate new video hardware decoder.
extern VideoHwDecoder *VideoNewHwDecoder(VideoStream *);

    /// Deallocate video hardware decoder.
extern void VideoDelHwDecoder(VideoHwDecoder *);

    /// Callback to negotiate the PixelFormat.
extern enum AVPixelFormat Video_get_format(VideoHwDecoder *, AVCodecContext *,
    const enum AVPixelFormat *);

    /// Render a ffmpeg frame.
extern void VideoRenderFrame(VideoHwDecoder *, const AVCodecContext *,
    AVFrame *);

    /// Wakeup display handler.
extern void VideoDisplayWakeup(void);

    /// Set audio delay.
extern void VideoSetAudioDelay(int);

    /// Clear OSD.
extern void VideoOsdClear(void);

    /// Draw an OSD ARGB image.
extern void VideoOsdDrawARGB(int, int, int, int, int, const uint8_t *, int,
    int);

    /// Get OSD size.
extern void VideoGetOsdSize(int *, int *);

    /// Set video clock.
extern void VideoSetClock(VideoHwDecoder *, int64_t);

    /// Get video clock.
extern int64_t VideoGetClock(const VideoHwDecoder *);

    /// Set closing flag.
extern void VideoSetClosing(VideoHwDecoder *, int);

    /// Reset start of frame counter
extern void VideoResetStart(VideoHwDecoder *);

    /// Set trick play speed.
extern void VideoSetTrickSpeed(VideoHwDecoder *, int);

    /// Grab screen.
extern uint8_t *VideoGrab(int *, int *, int *, int);

    /// Grab screen raw.
extern uint8_t *VideoGrabService(int *, int *, int *);

    /// Get decoder statistics.
extern void VideoGetStats(VideoHwDecoder *, int *, int *, int *, int *);

    /// Get video stream size
extern void VideoGetVideoSize(VideoHwDecoder *, int *, int *, int *, int *);

extern void VideoOsdInit(void);		///< Setup osd.
extern void VideoOsdExit(void);		///< Cleanup osd.

extern void VideoInit(void);	///< Setup video module.
extern void VideoExit(void);		///< Cleanup and exit video module.

    /// Poll video input buffers.
extern int VideoPollInput(VideoStream *);

    /// Decode video input buffers.
extern int VideoDecodeInput(VideoStream *);

    /// Get number of input buffers.
extern int VideoGetBuffers(const VideoStream *);

/// @}
