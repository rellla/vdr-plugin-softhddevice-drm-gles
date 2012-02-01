///
///	@file video.c	@brief Video module
///
///	Copyright (c) 2009 - 2012 by Johns.  All Rights Reserved.
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

///
///	@defgroup Video The video module.
///
///	This module contains all video rendering functions.
///
///	@todo disable screen saver support
///
///	Uses Xlib where it is needed for VA-API or vdpau.  XCB is used for
///	everything else.
///
///	- X11
///	- OpenGL rendering
///	- OpenGL rendering with GLX texture-from-pixmap
///	- Xrender rendering
///

#define USE_XLIB_XCB			///< use xlib/xcb backend
#define USE_AUTOCROP			///< compile auto-crop support
#define USE_GRAB			///< experimental grab code
#define noUSE_GLX			///< outdated GLX code
#define noUSE_DOUBLEBUFFER		///< use GLX double buffers

//#define USE_VAAPI				///< enable vaapi support
//#define USE_VDPAU				///< enable vdpau support
#define noUSE_BITMAP			///< use vdpau bitmap surface

#define USE_VIDEO_THREAD		///< run decoder in an own thread

#include <sys/time.h>
#include <sys/shm.h>
#include <sys/ipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#include <alsa/iatomic.h>		// portable atomic_t

#ifdef USE_VIDEO_THREAD
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#include <time.h>
#ifndef HAVE_PTHREAD_NAME
    /// only available with newer glibc
#define pthread_setname_np(thread, name)
#endif
#endif

#ifdef USE_XLIB_XCB
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <xcb/xcb.h>
#include <xcb/bigreq.h>
#include <xcb/dpms.h>
#include <xcb/glx.h>
#include <xcb/randr.h>
#include <xcb/screensaver.h>
#include <xcb/shm.h>
#include <xcb/xv.h>

#include <xcb/xcb_image.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
#include <xcb/xcb_ewmh.h>
#else // compatibility hack for old xcb-util

/**
 * @brief Action on the _NET_WM_STATE property
 */
typedef enum
{
    /* Remove/unset property */
    XCB_EWMH_WM_STATE_REMOVE = 0,
    /* Add/set property */
    XCB_EWMH_WM_STATE_ADD = 1,
    /* Toggle property	*/
    XCB_EWMH_WM_STATE_TOGGLE = 2
} xcb_ewmh_wm_state_action_t;
#endif
#include <xcb/xcb_keysyms.h>
#endif

#ifdef USE_GLX
#include <GL/gl.h>			// For GL_COLOR_BUFFER_BIT
#include <GL/glx.h>
// only for gluErrorString
#include <GL/glu.h>
#endif

#ifdef USE_VAAPI
#include <va/va_x11.h>
#ifdef USE_GLX
#include <va/va_glx.h>
#endif
#endif

#ifdef USE_VDPAU
#include <vdpau/vdpau_x11.h>
#include <libavcodec/vdpau.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavcodec/vaapi.h>
#include <libavutil/pixdesc.h>

#include "misc.h"
#include "video.h"
#include "audio.h"

#ifdef USE_XLIB_XCB

//----------------------------------------------------------------------------
//	Declarations
//----------------------------------------------------------------------------

///
///	Video resolutions selector.
///
typedef enum _video_resolutions_
{
    VideoResolution576i,		///< ...x576 interlaced
    VideoResolution720p,		///< ...x720 progressive
    VideoResolutionFake1080i,		///< 1280x1080 1440x1080 interlaced
    VideoResolution1080i,		///< 1920x1080 interlaced
    VideoResolutionMax			///< number of resolution indexs
} VideoResolutions;

///
///	Video deinterlace modes.
///
typedef enum _video_deinterlace_modes_
{
    VideoDeinterlaceBob,		///< bob deinterlace
    VideoDeinterlaceWeave,		///< weave deinterlace
    VideoDeinterlaceTemporal,		///< temporal deinterlace
    VideoDeinterlaceTemporalSpatial,	///< temporal spatial deinterlace
    VideoDeinterlaceSoftware,		///< software deinterlace
} VideoDeinterlaceModes;

///
///	Video scaleing modes.
///
typedef enum _video_scaling_modes_
{
    VideoScalingNormal,			///< normal scaling
    VideoScalingFast,			///< fastest scaling
    VideoScalingHQ,			///< high quality scaling
    VideoScalingAnamorphic,		///< anamorphic scaling
} VideoScalingModes;

///
///	Video zoom modes.
///
typedef enum _video_zoom_modes_
{
    VideoNormal,			///< normal
    VideoStretch,			///< stretch to all edges
    VideoCenterCutOut,			///< center and cut out
    VideoAnamorphic,			///< anamorphic scaled (unsupported)
} VideoZoomModes;

///
///	Video output module structure and typedef.
///
typedef struct _video_module_
{
    const char *Name;			///< video output module name

    char Enabled;			///< flag output module enabled

    /// allocate new video hw decoder
    VideoHwDecoder *(*const NewHwDecoder)(void);
    void (*const DelHwDecoder) (VideoHwDecoder *);
    unsigned (*const GetSurface) (VideoHwDecoder *);
    void (*const ReleaseSurface) (VideoHwDecoder *, unsigned);
    enum PixelFormat (*const get_format) (VideoHwDecoder *, AVCodecContext *,
	const enum PixelFormat *);
    void (*const RenderFrame) (VideoHwDecoder *, const AVCodecContext *,
	const AVFrame *);
    uint8_t *(*const GrabOutput)(int *, int *, int *);
    void (*const SetVideoMode) (void);
    void (*const ResetAutoCrop) (void);

    void (*const Thread) (void);	///< module display handler thread

    void (*const OsdClear) (void);	///< clear OSD
    void (*const OsdDrawARGB) (int, int, int, int, const uint8_t *);
    ///< draw OSD ARGB area
    void (*const OsdInit) (int, int);	///< initialize OSD
    void (*const OsdExit) (void);	///< cleanup OSD

    int (*const Init) (const char *);	///< initialize video output module
    void (*const Exit) (void);		///< cleanup video output module
} VideoModule;

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define CODEC_SURFACES_MAX	31	///< maximal of surfaces

#define CODEC_SURFACES_DEFAULT	(21+4)	///< default of surfaces
// FIXME: video-xvba only supports 14
#define xCODEC_SURFACES_DEFAULT	14	///< default of surfaces

#define CODEC_SURFACES_MPEG2	3	///< 1 decode, up to  2 references
#define CODEC_SURFACES_MPEG4	3	///< 1 decode, up to  2 references
#define CODEC_SURFACES_H264	21	///< 1 decode, up to 20 references
#define CODEC_SURFACES_VC1	3	///< 1 decode, up to  2 references

#define VIDEO_SURFACES_MAX	4	///< video output surfaces for queue
#define OUTPUT_SURFACES_MAX	4	///< output surfaces for flip page

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

static Display *XlibDisplay;		///< Xlib X11 display
static xcb_connection_t *Connection;	///< xcb connection
static xcb_colormap_t VideoColormap;	///< video colormap
static xcb_window_t VideoWindow;	///< video window
static xcb_cursor_t VideoBlankCursor;	///< empty invisible cursor

static int VideoWindowX;		///< video output window x coordinate
static int VideoWindowY;		///< video outout window y coordinate
static unsigned VideoWindowWidth;	///< video output window width
static unsigned VideoWindowHeight;	///< video output window height

static const VideoModule *VideoUsedModule;	///< selected video module

static char VideoHardwareDecoder;	///< flag use hardware decoder

static char VideoSurfaceModesChanged;	///< flag surface modes changed

    /// flag use transparent OSD.
static const char VideoTransparentOsd = 1;

static int VideoSkipLines;		///< skip video lines top/bottom

    /// Default deinterlace mode.
static VideoDeinterlaceModes VideoDeinterlace[VideoResolutionMax];

    /// Default number of deinterlace surfaces
static const int VideoDeinterlaceSurfaces = 4;

    /// Default Inverse telecine flag (VDPAU only).
static char VideoInverseTelecine[VideoResolutionMax];

    /// Default skip chroma deinterlace flag (VDPAU only).
static char VideoSkipChromaDeinterlace[VideoResolutionMax];

    /// Default amount of noise reduction algorithm to apply (0 .. 1000).
static int VideoDenoise[VideoResolutionMax];

    /// Default amount of of sharpening, or blurring, to apply (-1000 .. 1000).
static int VideoSharpen[VideoResolutionMax];

// FIXME: color space / studio levels

    /// Default scaling mode
static VideoScalingModes VideoScaling[VideoResolutionMax];

    /// Default audio/video delay
static int VideoAudioDelay;

    /// Default zoom mode
static VideoZoomModes Video4to3ZoomMode;

//static char VideoSoftStartSync;		///< soft start sync audio/video

static char Video60HzMode;		///< handle 60hz displays

static xcb_atom_t WmDeleteWindowAtom;	///< WM delete message atom
static xcb_atom_t NetWmState;		///< wm-state message atom
static xcb_atom_t NetWmStateFullscreen;	///< fullscreen wm-state message atom

extern uint32_t VideoSwitch;		///< ticks for channel switch
extern atomic_t VideoPacketsFilled;	///< how many of the buffer is used

#ifdef USE_VIDEO_THREAD

static pthread_t VideoThread;		///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoMutex;	///< video condition mutex
static pthread_mutex_t VideoLockMutex;	///< video lock mutex

#endif

static char OsdShown;			///< flag show osd
static int OsdWidth;			///< osd width
static int OsdHeight;			///< osd height
static int OsdDirtyX;			///< osd dirty area x
static int OsdDirtyY;			///< osd dirty area y
static int OsdDirtyWidth;		///< osd dirty area width
static int OsdDirtyHeight;		///< osd dirty area height

static int64_t VideoDeltaPTS;		///< FIXME: fix pts

//----------------------------------------------------------------------------
//	Common Functions
//----------------------------------------------------------------------------

static void VideoThreadLock(void);	///< lock video thread
static void VideoThreadUnlock(void);	///< unlock video thread

#if defined(DEBUG) || defined(AV_INFO)
///
///	Nice time-stamp string.
///
static const char *VideoTimeStampString(int64_t ts)
{
    static char buf[64];
    int hh;
    int mm;
    int ss;
    int uu;

    ts = ts / 90;
    uu = ts % 1000;
    ss = (ts / 1000) % 60;
    mm = (ts / 60000) % 60;
    hh = ts / 3600000;
    snprintf(buf, sizeof(buf), "%2d:%02d:%02d.%03d", hh, mm, ss, uu);

    return buf;
}
#endif

///
///	Update video pts.
///
///	@param pts_p		pointer to pts
///	@param interlaced	interlaced flag (frame isn't right)
///	@param frame		frame to display
///
///	@note frame->interlaced_frame can't be used for interlace detection
///
static void VideoSetPts(int64_t * pts_p, int interlaced, const AVFrame * frame)
{
    int64_t pts;

    // update video clock
    if ((uint64_t) * pts_p != AV_NOPTS_VALUE) {
	*pts_p += interlaced ? 40 * 90 : 20 * 90;
    }
    //pts = frame->best_effort_timestamp;
    pts = frame->pkt_pts;
    if ((uint64_t) pts == AV_NOPTS_VALUE || !pts) {
	// libav: 0.8pre didn't set pts
	pts = frame->pkt_dts;
    }
    // libav: sets only pkt_dts which can be 0
    if (pts && (uint64_t) pts != AV_NOPTS_VALUE) {
	// build a monotonic pts
	if ((uint64_t) * pts_p != AV_NOPTS_VALUE) {
	    int64_t delta;

	    delta = pts - *pts_p;
	    // ignore negative jumps
	    if (delta > -600 * 90 && delta <= -40 * 90) {
		if (-delta > VideoDeltaPTS) {
		    VideoDeltaPTS = -delta;
		    Debug(4,
			"video: %#012" PRIx64 "->%#012" PRIx64 " delta+%4"
			PRId64 " pts\n", *pts_p, pts, pts - *pts_p);
		}
		return;
	    }
	}
	if (*pts_p != pts) {
	    Debug(4,
		"video: %#012" PRIx64 "->%#012" PRIx64 " delta=%4" PRId64
		" pts\n", *pts_p, pts, pts - *pts_p);
	    *pts_p = pts;
	}
    }
}

///
///	Update output for new size or aspect ratio.
///
///	@param input_aspect_ratio	video stream aspect
///
static void VideoUpdateOutput(AVRational input_aspect_ratio, int input_width,
    int input_height, int *output_x, int *output_y, int *output_width,
    int *output_height, int *crop_x, int *crop_y, int *crop_width,
    int *crop_height)
{
    AVRational display_aspect_ratio;

    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
	input_aspect_ratio.num = 1;
	input_aspect_ratio.den = 1;
	Debug(3, "video: aspect defaults to %d:%d\n", input_aspect_ratio.num,
	    input_aspect_ratio.den);
    }

    av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
	input_width * input_aspect_ratio.num,
	input_height * input_aspect_ratio.den, 1024 * 1024);

    // InputWidth/Height can be zero = uninitialized
    if (!display_aspect_ratio.num || !display_aspect_ratio.den) {
	display_aspect_ratio.num = 1;
	display_aspect_ratio.den = 1;
    }

    Debug(3, "video: aspect %d:%d\n", display_aspect_ratio.num,
	display_aspect_ratio.den);

    *crop_x = 0;
    *crop_y = VideoSkipLines;
    *crop_width = input_width;
    *crop_height = input_height - VideoSkipLines * 2;

    // FIXME: store different positions for the ratios
    if (display_aspect_ratio.num == 4 && display_aspect_ratio.den == 3) {
	switch (Video4to3ZoomMode) {
	    case VideoNormal:
		goto normal;
	    case VideoStretch:
		goto stretch;
	    case VideoCenterCutOut:
		goto center_cut_out;
	    case VideoAnamorphic:
		// FIXME: rest should be done by hardware
		goto stretch;
	}
    }
    // FIXME: this overwrites user choosen output position

  normal:
    *output_x = 0;
    *output_y = 0;
    *output_width = (VideoWindowHeight * display_aspect_ratio.num)
	/ display_aspect_ratio.den;
    *output_height = (VideoWindowWidth * display_aspect_ratio.den)
	/ display_aspect_ratio.num;
    if ((unsigned)*output_width > VideoWindowWidth) {
	*output_width = VideoWindowWidth;
	*output_y = (VideoWindowHeight - *output_height) / 2;
    } else if ((unsigned)*output_height > VideoWindowHeight) {
	*output_height = VideoWindowHeight;
	*output_x = (VideoWindowWidth - *output_width) / 2;
    }
    Debug(3, "video: aspect output %dx%d+%d+%d\n", *output_width,
	*output_height, *output_x, *output_y);
    return;

  stretch:
    *output_x = 0;
    *output_y = 0;
    *output_width = VideoWindowWidth;
    *output_height = VideoWindowHeight;
    return;

  center_cut_out:
    *output_x = 0;
    *output_y = 0;
    *output_height = VideoWindowHeight;
    *output_width = VideoWindowWidth;

    *crop_width = (VideoWindowHeight * display_aspect_ratio.num)
	/ display_aspect_ratio.den;
    *crop_height = (VideoWindowWidth * display_aspect_ratio.den)
	/ display_aspect_ratio.num;

    // look which side must be cut
    if ((unsigned)*crop_width > VideoWindowWidth) {
	*crop_height = input_height;

	// adjust scaiing
	*crop_x = ((*crop_width - (signed)VideoWindowWidth) * input_width)
	    / (2 * VideoWindowWidth);
	*crop_width = input_width - *crop_x * 2;
    } else if ((unsigned)*crop_height > VideoWindowHeight) {
	*crop_width = input_width;

	// adjust scaiing
	*crop_y = ((*crop_height - (signed)VideoWindowHeight) * input_height)
	    / (2 * VideoWindowHeight);
	*crop_height = input_height - *crop_y * 2;
    } else {
	*crop_width = input_width;
	*crop_height = input_height;
    }
    Debug(3, "video: aspect crop %dx%d+%d+%d\n", *crop_width, *crop_height,
	*crop_x, *crop_y);
    return;
}

//----------------------------------------------------------------------------
//	GLX
//----------------------------------------------------------------------------

#ifdef USE_GLX

static int GlxEnabled = 1;		///< use GLX
static int GlxVSyncEnabled = 0;		///< enable/disable v-sync
static GLXContext GlxSharedContext;	///< shared gl context
static GLXContext GlxContext;		///< our gl context
static XVisualInfo *GlxVisualInfo;	///< our gl visual

static GLuint OsdGlTextures[2];		///< gl texture for OSD
static int OsdIndex;			///< index into OsdGlTextures

///
///	GLX extension functions
///@{
#ifdef GLX_MESA_swap_control
static PFNGLXSWAPINTERVALMESAPROC GlxSwapIntervalMESA;
#endif
#ifdef GLX_SGI_video_sync
static PFNGLXGETVIDEOSYNCSGIPROC GlxGetVideoSyncSGI;
#endif
#ifdef GLX_SGI_swap_control
static PFNGLXSWAPINTERVALSGIPROC GlxSwapIntervalSGI;
#endif

///@}

///
///	GLX check error.
///
static void GlxCheck(void)
{
    GLenum err;

    if ((err = glGetError()) != GL_NO_ERROR) {
	Debug(3, "video/glx: error %d '%s'\n", err, gluErrorString(err));
    }
}

///
///	GLX check if a GLX extension is supported.
///
///	@param ext	extension to query
///	@returns true if supported, false otherwise
///
static int GlxIsExtensionSupported(const char *ext)
{
    const char *extensions;

    if ((extensions =
	    glXQueryExtensionsString(XlibDisplay,
		DefaultScreen(XlibDisplay)))) {
	const char *s;
	int l;

	s = strstr(extensions, ext);
	l = strlen(ext);
	return s && (s[l] == ' ' || s[l] == '\0');
    }
    return 0;
}

#if 0
///
///	Setup GLX decoder
///
///	@param decoder	VA-API decoder
///
void GlxSetupDecoder(VaapiDecoder * decoder)
{
    int width;
    int height;
    int i;

    width = decoder->InputWidth;
    height = decoder->InputHeight;

    glEnable(GL_TEXTURE_2D);		// create 2d texture
    glGenTextures(2, decoder->GlTexture);
    GlxCheck();
    for (i = 0; i < 2; ++i) {
	glBindTexture(GL_TEXTURE_2D, decoder->GlTexture[i]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
	    GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
    }
    glDisable(GL_TEXTURE_2D);

    GlxCheck();
}
#endif

///
///	Render texture.
///
///	@param texture	2d texture
///
static inline void GlxRenderTexture(GLuint texture, int x, int y, int width,
    int height)
{
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);	// no color
    glBegin(GL_QUADS); {
	glTexCoord2f(1.0f, 1.0f);
	glVertex2i(x + width, y + height);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2i(x, y + height);
	glTexCoord2f(0.0f, 0.0f);
	glVertex2i(x, y);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2i(x + width, y);
#if 0
	glTexCoord2f(0.0f, 0.0f);
	glVertex2i(x, y);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2i(x, y + height);
	glTexCoord2f(1.0f, 1.0f);
	glVertex2i(x + width, y + height);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2i(x + width, y);
#endif
    }
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

///
///	Upload texture.
///
static void GlxUploadTexture(int x, int y, int width, int height,
    const uint8_t * argb)
{
    // FIXME: use other / faster uploads
    // ARB_pixelbuffer_object GL_PIXEL_UNPACK_BUFFER glBindBufferARB()
    // glMapBuffer() glUnmapBuffer()
    // glTexSubImage2D

    glEnable(GL_TEXTURE_2D);		// upload 2d texture

    glBindTexture(GL_TEXTURE_2D, OsdGlTextures[OsdIndex]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_BGRA,
	GL_UNSIGNED_BYTE, argb);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_TEXTURE_2D);
}

///
///	Render to glx texture.
///
static void GlxRender(int osd_width, int osd_height)
{
    static uint8_t *image;
    static uint8_t cycle;
    int x;
    int y;

    if (!OsdGlTextures[0] || !OsdGlTextures[1]) {
	return;
    }
    // render each frame kills performance

    // osd 1920 * 1080 * 4 (RGBA) * 50 (HZ) = 396 Mb/s

    // too big for alloca
    if (!image) {
	image = malloc(4 * osd_width * osd_height);
	memset(image, 0x00, 4 * osd_width * osd_height);
    }
    for (y = 0; y < osd_height; ++y) {
	for (x = 0; x < osd_width; ++x) {
	    ((uint32_t *) image)[x + y * osd_width] =
		0x00FFFFFF | (cycle++) << 24;
	}
    }
    cycle++;

    // FIXME: convert is for GLX texture unneeded
    // convert internal osd to image
    //GfxConvert(image, 0, 4 * osd_width);
    //

    GlxUploadTexture(0, 0, osd_width, osd_height, image);
}

///
///	Setup GLX window.
///
static void GlxSetupWindow(xcb_window_t window, int width, int height)
{
    uint32_t start;
    uint32_t end;
    int i;
    unsigned count;

    Debug(3, "video/glx: %s\n %x %dx%d", __FUNCTION__, window, width, height);

    // set glx context
    if (!glXMakeCurrent(XlibDisplay, window, GlxContext)) {
	Fatal(_("video/glx: can't make glx context current\n"));
	// FIXME: disable glx
	return;
    }

    Debug(3, "video/glx: ok\n");

#ifdef DEBUG
    // check if v-sync is working correct
    end = GetMsTicks();
    for (i = 0; i < 10; ++i) {
	start = end;

	glClear(GL_COLOR_BUFFER_BIT);
	glXSwapBuffers(XlibDisplay, window);
	end = GetMsTicks();

	GlxGetVideoSyncSGI(&count);
	Debug(3, "video/glx: %5d frame rate %d ms\n", count, end - start);
	// nvidia can queue 5 swaps
	if (i > 5 && (end - start) < 15) {
	    Warning(_("video/glx: no v-sync\n"));
	}
    }
#endif

    // viewpoint
    GlxCheck();
    glViewport(0, 0, width, height);
    glDepthRange(-1.0, 1.0);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glColor3f(1.0f, 1.0f, 1.0f);
    glClearDepth(1.0);
    GlxCheck();

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
    GlxCheck();

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);		// setup 2d drawing
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
#ifdef USE_DOUBLEBUFFER
    glDrawBuffer(GL_BACK);
#else
    glDrawBuffer(GL_FRONT);
#endif
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#ifdef DEBUG
#ifdef USE_DOUBLEBUFFER
    glDrawBuffer(GL_FRONT);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawBuffer(GL_BACK);
#endif
#endif

    // clear
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);	// intial background color
    glClear(GL_COLOR_BUFFER_BIT);
#ifdef DEBUG
    glClearColor(1.0f, 1.0f, 0.0f, 1.0f);	// background color
#endif
    GlxCheck();
}

///
///	Initialize GLX.
///
static void GlxInit(void)
{
    static GLint visual_attr[] = {
	GLX_RGBA,
	GLX_RED_SIZE, 8,
	GLX_GREEN_SIZE, 8,
	GLX_BLUE_SIZE, 8,
#ifdef USE_DOUBLEBUFFER
	GLX_DOUBLEBUFFER,
#endif
	None
    };
    XVisualInfo *vi;
    GLXContext context;
    int major;
    int minor;
    int glx_GLX_EXT_swap_control;
    int glx_GLX_MESA_swap_control;
    int glx_GLX_SGI_swap_control;
    int glx_GLX_SGI_video_sync;

    if (!glXQueryVersion(XlibDisplay, &major, &minor)) {
	Error(_("video/glx: no GLX support\n"));
	GlxEnabled = 0;
	return;
    }
    Info(_("video/glx: glx version %d.%d\n"), major, minor);

    //
    //	check which extension are supported
    //
    glx_GLX_EXT_swap_control = GlxIsExtensionSupported("GLX_EXT_swap_control");
    glx_GLX_MESA_swap_control =
	GlxIsExtensionSupported("GLX_MESA_swap_control");
    glx_GLX_SGI_swap_control = GlxIsExtensionSupported("GLX_SGI_swap_control");
    glx_GLX_SGI_video_sync = GlxIsExtensionSupported("GLX_SGI_video_sync");

#ifdef GLX_MESA_swap_control
    if (glx_GLX_MESA_swap_control) {
	GlxSwapIntervalMESA = (PFNGLXSWAPINTERVALMESAPROC)
	    glXGetProcAddress((const GLubyte *)"glXSwapIntervalMESA");
    }
    Debug(3, "video/glx: GlxSwapIntervalMESA=%p\n", GlxSwapIntervalMESA);
#endif
#ifdef GLX_SGI_swap_control
    if (glx_GLX_SGI_swap_control) {
	GlxSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC)
	    glXGetProcAddress((const GLubyte *)"glXSwapIntervalSGI");
    }
    Debug(3, "video/glx: GlxSwapIntervalSGI=%p\n", GlxSwapIntervalSGI);
#endif
#ifdef GLX_SGI_video_sync
    if (glx_GLX_SGI_video_sync) {
	GlxGetVideoSyncSGI = (PFNGLXGETVIDEOSYNCSGIPROC)
	    glXGetProcAddress((const GLubyte *)"glXGetVideoSyncSGI");
    }
    Debug(3, "video/glx: GlxGetVideoSyncSGI=%p\n", GlxGetVideoSyncSGI);
#endif
    // glXGetVideoSyncSGI glXWaitVideoSyncSGI

#if 0
    // FIXME: use xcb: xcb_glx_create_context
#endif

    // create glx context
    glXMakeCurrent(XlibDisplay, None, NULL);
    vi = glXChooseVisual(XlibDisplay, DefaultScreen(XlibDisplay), visual_attr);
    if (!vi) {
	Error(_("video/glx: can't get a RGB visual\n"));
	GlxEnabled = 0;
	return;
    }
    if (!vi->visual) {
	Error(_("video/glx: no valid visual found\n"));
	GlxEnabled = 0;
	return;
    }
    if (vi->bits_per_rgb < 8) {
	Error(_("video/glx: need atleast 8-bits per RGB\n"));
	GlxEnabled = 0;
	return;
    }
    context = glXCreateContext(XlibDisplay, vi, NULL, GL_TRUE);
    if (!context) {
	Error(_("video/glx: can't create glx context\n"));
	GlxEnabled = 0;
	return;
    }
    GlxSharedContext = context;
    context = glXCreateContext(XlibDisplay, vi, GlxSharedContext, GL_TRUE);
    if (!context) {
	Error(_("video/glx: can't create glx context\n"));
	GlxEnabled = 0;
	// FIXME: destroy GlxSharedContext
	return;
    }
    GlxContext = context;

    GlxVisualInfo = vi;
    Debug(3, "video/glx: visual %#02x depth %u\n", (unsigned)vi->visualid,
	vi->depth);

    //
    //	query default v-sync state
    //
    if (glx_GLX_EXT_swap_control) {
	unsigned tmp;

	tmp = -1;
	glXQueryDrawable(XlibDisplay, DefaultRootWindow(XlibDisplay),
	    GLX_SWAP_INTERVAL_EXT, &tmp);
	GlxCheck();

	Debug(3, "video/glx: default v-sync is %d\n", tmp);
    } else {
	Debug(3, "video/glx: default v-sync is unknown\n");
    }

    //
    //	disable wait on v-sync
    //
    // FIXME: sleep before swap / busy waiting hardware
    // FIXME: 60hz lcd panel
    // FIXME: config: default, on, off
#ifdef GLX_SGI_swap_control
    if (GlxVSyncEnabled < 0 && GlxSwapIntervalSGI) {
	if (GlxSwapIntervalSGI(0)) {
	    GlxCheck();
	    Warning(_("video/glx: can't disable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync disabled\n"));
	}
    } else
#endif
#ifdef GLX_MESA_swap_control
    if (GlxVSyncEnabled < 0 && GlxSwapIntervalMESA) {
	if (GlxSwapIntervalMESA(0)) {
	    GlxCheck();
	    Warning(_("video/glx: can't disable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync disabled\n"));
	}
    }
#endif

    //
    //	enable wait on v-sync
    //
#ifdef GLX_SGI_swap_control
    if (GlxVSyncEnabled > 0 && GlxSwapIntervalMESA) {
	if (GlxSwapIntervalMESA(1)) {
	    GlxCheck();
	    Warning(_("video/glx: can't enable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync enabled\n"));
	}
    } else
#endif
#ifdef GLX_MESA_swap_control
    if (GlxVSyncEnabled > 0 && GlxSwapIntervalSGI) {
	if (GlxSwapIntervalSGI(1)) {
	    GlxCheck();
	    Warning(_("video/glx: can't enable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync enabled\n"));
	}
    }
#endif
}

///
///	Cleanup GLX.
///
static void GlxExit(void)
{
    Debug(3, "video/glx: %s\n", __FUNCTION__);

    glFinish();

    // must destroy glx
    if (glXGetCurrentContext() == GlxContext) {
	// if currently used, set to none
	glXMakeCurrent(XlibDisplay, None, NULL);
    }
    if (GlxSharedContext) {
	glXDestroyContext(XlibDisplay, GlxSharedContext);
    }
    if (GlxContext) {
	glXDestroyContext(XlibDisplay, GlxContext);
    }
#if 0
    if (GlxThreadContext) {
	glXDestroyContext(XlibDisplay, GlxThreadContext);
    }
    // FIXME: must free GlxVisualInfo
#endif
}

#endif

//----------------------------------------------------------------------------
//	common functions
//----------------------------------------------------------------------------

///
///	Calculate resolution group.
///
///	@param width		video picture raw width
///	@param height		video picture raw height
///	@param interlace	flag interlaced video picture
///
///	@note interlace isn't used yet and probably wrong set by caller.
///
static VideoResolutions VideoResolutionGroup(int width, int height,
    __attribute__ ((unused))
    int interlace)
{
    if (height <= 576) {
	return VideoResolution576i;
    }
    if (height <= 720) {
	return VideoResolution720p;
    }
    if (height < 1080) {
	return VideoResolutionFake1080i;
    }
    if (width < 1920) {
	return VideoResolutionFake1080i;
    }
    return VideoResolution1080i;
}

//----------------------------------------------------------------------------
//	auto-crop
//----------------------------------------------------------------------------

///
///	auto-crop context structure and typedef.
///
typedef struct _auto_crop_ctx_
{
    int X1;				///< detected left border
    int X2;				///< detected right border
    int Y1;				///< detected top border
    int Y2;				///< detected bottom border

    int Count;				///< counter to delay switch
    int State;				///< auto-crop state (0, 14, 16)

} AutoCropCtx;

#ifdef USE_AUTOCROP

#define YBLACK 0x20			///< below is black
#define UVBLACK 0x80			///< around is black
#define M64 UINT64_C(0x0101010101010101)	///< 64bit multiplicator

    /// auto-crop percent of video width to ignore logos
static const int AutoCropLogoIgnore = 24;
static int AutoCropInterval;		///< auto-crop check interval
static int AutoCropDelay;		///< auto-crop switch delay
static int AutoCropTolerance;		///< auto-crop tolerance

///
///	Detect black line Y.
///
///	@param data	Y plane pixel data
///	@param length	number of pixel to check
///	@param stride	offset of pixels
///
///	@note 8 pixel are checked at once, all values must be 8 aligned
///
static int AutoCropIsBlackLineY(const uint8_t * data, int length, int stride)
{
    int n;
    int o;
    uint64_t r;
    const uint64_t *p;

#ifdef DEBUG
    if ((size_t) data & 0x7 || stride & 0x7) {
	abort();
    }
#endif
    p = (const uint64_t *)data;
    n = length;				// FIXME: can remove n
    o = stride / 8;

    r = 0UL;
    while (--n >= 0) {
	r |= *p;
	p += o;
    }

    // below YBLACK(0x20) is black
    return !(r & ~((YBLACK - 1) * M64));
}

///
///	Auto detect black borders and crop them.
///
///	@param autocrop auto-crop variables
///	@param width	frame width in pixel
///	@param height	frame height in pixel
///	@param data	frame planes data (Y, U, V)
///	@param pitches	frame planes pitches (Y, U, V)
///
///	@note FIXME: can reduce the checked range, left, right crop isn't
///		used yet.
///
///	@note FIXME: only Y is checked, for black.
///
static void AutoCropDetect(AutoCropCtx * autocrop, int width, int height,
    void *data[3], uint32_t pitches[3])
{
    const void *data_y;
    unsigned length_y;
    int x;
    int y;
    int x1;
    int x2;
    int y1;
    int y2;
    int logo_skip;

    //
    //	ignore top+bottom 6 lines and left+right 8 pixels
    //
#define SKIP_X	8
#define SKIP_Y	6
    x1 = width - 1;
    x2 = 0;
    y1 = height - 1;
    y2 = 0;
    logo_skip = SKIP_X + (((width * AutoCropLogoIgnore) / 100 + 8) / 8) * 8;

    data_y = data[0];
    length_y = pitches[0];

    //
    //	search top
    //
    for (y = SKIP_Y; y < y1; ++y) {
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y,
		(width - 2 * logo_skip) / 8, 8)) {
	    if (y == SKIP_Y) {
		y = 0;
	    }
	    y1 = y;
	    break;
	}
    }
    //
    //	search bottom
    //
    for (y = height - SKIP_Y - 1; y > y2; --y) {
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y,
		(width - 2 * logo_skip) / 8, 8)) {
	    if (y == height - SKIP_Y - 1) {
		y = height - 1;
	    }
	    y2 = y;
	    break;
	}
    }
    //
    //	search left
    //
    for (x = SKIP_X; x < x1; x += 8) {
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y,
		height - 2 * SKIP_Y, length_y)) {
	    if (x == SKIP_X) {
		x = 0;
	    }
	    x1 = x;
	    break;
	}
    }
    //
    //	search right
    //
    for (x = width - SKIP_X - 8; x > x2; x -= 8) {
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y,
		height - 2 * SKIP_Y * 8, length_y)) {
	    if (x == width - SKIP_X - 8) {
		x = width - 1;
	    }
	    x2 = x;
	    break;
	}
    }

    if (0 && (y1 > SKIP_Y || x1 > SKIP_X)) {
	Debug(3, "video/autocrop: top=%d bottom=%d left=%d right=%d\n", y1, y2,
	    x1, x2);
    }

    autocrop->X1 = x1;
    autocrop->X2 = x2;
    autocrop->Y1 = y1;
    autocrop->Y2 = y2;
}

#endif

//----------------------------------------------------------------------------
//	software - deinterlace
//----------------------------------------------------------------------------

// FIXME: move general software deinterlace functions to here.

//----------------------------------------------------------------------------
//	VA-API
//----------------------------------------------------------------------------

#ifdef USE_VAAPI

static int VaapiBuggyVdpau;		///< fix libva-driver-vdpau bugs
static int VaapiBuggyIntel;		///< fix libva-driver-intel bugs

static VADisplay *VaDisplay;		///< VA-API display

static VAImage VaOsdImage = {
    .image_id = VA_INVALID_ID
};					///< osd VA-API image

static VASubpictureID VaOsdSubpicture = VA_INVALID_ID;	///< osd VA-API subpicture
static char VaapiUnscaledOsd;		///< unscaled osd supported

    /// VA-API decoder typedef
typedef struct _vaapi_decoder_ VaapiDecoder;

///
///	VA-API decoder
///
struct _vaapi_decoder_
{
    VADisplay *VaDisplay;		///< VA-API display

    xcb_window_t Window;		///< output window
    int OutputX;			///< output window x
    int OutputY;			///< output window y
    int OutputWidth;			///< output window width
    int OutputHeight;			///< output window height

    /// flags for put surface for different resolutions groups
    unsigned SurfaceFlagsTable[VideoResolutionMax];

    enum PixelFormat PixFmt;		///< ffmpeg frame pixfmt
    int WrongInterlacedWarned;		///< warning about interlace flag issued
    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    VAImage DeintImages[5];		///< deinterlace image buffers

    int GetPutImage;			///< flag get/put image can be used
    VAImage Image[1];			///< image buffer to update surface

    struct vaapi_context VaapiContext[1];	///< ffmpeg VA-API context

    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used surfaces
    /// used surface ids
    VASurfaceID SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free surfaces
    /// free surface ids
    VASurfaceID SurfacesFree[CODEC_SURFACES_MAX];

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height
#ifdef USE_AUTOCROP
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
#ifdef USE_GLX
    GLuint GlTexture[2];		///< gl texture for VA-API
    void *GlxSurface[2];		///< VA-API/GLX surface
#endif
    VASurfaceID BlackSurface;		///< empty black surface

    /// video surface ring buffer
    VASurfaceID SurfacesRb[VIDEO_SURFACES_MAX];
    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    int SurfaceField;			///< current displayed field
    int DropNextFrame;			///< flag drop next frame
    int DupNextFrame;			///< flag duplicate next frame
    struct timespec FrameTime;		///< time of last display
    int64_t PTS;			///< video PTS clock

    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
};

static VaapiDecoder *VaapiDecoders[1];	///< open decoder streams
static int VaapiDecoderN;		///< number of decoder streams

    /// forward display back surface
static void VaapiBlackSurface(VaapiDecoder *);

    /// forward destroy deinterlace images
static void VaapiDestroyDeinterlaceImages(VaapiDecoder *);

//----------------------------------------------------------------------------
//	VA-API Functions
//----------------------------------------------------------------------------

//	Surfaces -------------------------------------------------------------

///
///	Associate OSD with surface.
///
///	@param decoder	VA-API decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void VaapiAssociate(VaapiDecoder * decoder, int width, int height)
{
    int x;
    int y;
    int w;
    int h;

    if (VaOsdSubpicture == VA_INVALID_ID) {
	Warning(_("video/vaapi: no osd subpicture yet\n"));
	return;
    }

    x = 0;
    y = 0;
    w = VaOsdImage.width;
    h = VaOsdImage.height;

    // FIXME: associate only if osd is displayed
    if (VaapiUnscaledOsd) {
	if (decoder->SurfaceFreeN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, x, y, w, h, 0, 0,
		VideoWindowWidth, VideoWindowHeight,
		VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
	if (decoder->SurfaceUsedN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesUsed, decoder->SurfaceUsedN, x, y, w, h, 0, 0,
		VideoWindowWidth, VideoWindowHeight,
		VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
    } else {
	// FIXME: auto-crop wrong position
	if (decoder->SurfaceFreeN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, x, y, w, h, 0, 0,
		width, height, 0)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
	if (decoder->SurfaceUsedN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesUsed, decoder->SurfaceUsedN, x, y, w, h, 0, 0,
		width, height, 0)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
    }
}

///
///	Deassociate OSD with surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiDeassociate(VaapiDecoder * decoder)
{
    if (VaOsdSubpicture != VA_INVALID_ID) {
	if (decoder->SurfaceFreeN
	    && vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't deassociate %d surfaces\n"),
		decoder->SurfaceFreeN);
	}

	if (decoder->SurfaceUsedN
	    && vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesUsed, decoder->SurfaceUsedN)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't deassociate %d surfaces\n"),
		decoder->SurfaceUsedN);
	}
    }
}

///
///	Create surfaces for VA-API decoder.
///
///	@param decoder	VA-API decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void VaapiCreateSurfaces(VaapiDecoder * decoder, int width, int height)
{
#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
	Error(_("video/vaapi: surface needed not set\n"));
	decoder->SurfacesNeeded = 3 + VIDEO_SURFACES_MAX;
    }
#endif
    Debug(3, "video/vaapi: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	decoder->SurfacesNeeded);

    decoder->SurfaceFreeN = decoder->SurfacesNeeded;
    // VA_RT_FORMAT_YUV420 VA_RT_FORMAT_YUV422 VA_RT_FORMAT_YUV444
    if (vaCreateSurfaces(decoder->VaDisplay, width, height,
	    VA_RT_FORMAT_YUV420, decoder->SurfaceFreeN,
	    decoder->SurfacesFree) != VA_STATUS_SUCCESS) {
	Fatal(_("video/vaapi: can't create %d surfaces\n"),
	    decoder->SurfaceFreeN);
	// FIXME: write error handler / fallback
    }
    //
    //	update OSD associate
    //
    VaapiAssociate(decoder, width, height);
}

///
///	Destroy surfaces of VA-API decoder.
///
///	@param decoder	VA-API decoder
///
static void VaapiDestroySurfaces(VaapiDecoder * decoder)
{
    Debug(3, "video/vaapi: %s:\n", __FUNCTION__);

    //
    //	update OSD associate
    //
    VaapiDeassociate(decoder);

    if (vaDestroySurfaces(decoder->VaDisplay, decoder->SurfacesFree,
	    decoder->SurfaceFreeN)
	!= VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't destroy %d surfaces\n",
	    decoder->SurfaceFreeN);
    }
    decoder->SurfaceFreeN = 0;
    if (vaDestroySurfaces(decoder->VaDisplay, decoder->SurfacesUsed,
	    decoder->SurfaceUsedN)
	!= VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't destroy %d surfaces\n",
	    decoder->SurfaceUsedN);
    }
    decoder->SurfaceUsedN = 0;

    // FIXME surfaces used for output
}

    /// forward definition release surface
static void VaapiReleaseSurface(VaapiDecoder *, VASurfaceID);

///
///	Get a free surface.
///
///	@param decoder	VA-API decoder
///
///	@returns the oldest free surface
///
static VASurfaceID VaapiGetSurface(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    VASurfaceStatus status;
    int i;

    // try to use oldest surface
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	surface = decoder->SurfacesFree[i];
	if (vaQuerySurfaceStatus(decoder->VaDisplay, surface, &status)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaQuerySurface failed\n"));
	    status = VASurfaceReady;
	}
	// surface still in use, try next
	if (status != VASurfaceReady) {
	    Debug("video/vaapi: surface %#010x not ready: %d\n", surface,
		status);
	    if (!VaapiBuggyVdpau || i < 1) {
		continue;
	    }
	    usleep(1 * 1000);
	}
	// copy remaining surfaces down
	decoder->SurfaceFreeN--;
	for (; i < decoder->SurfaceFreeN; ++i) {
	    decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
	}
	decoder->SurfacesFree[i] = VA_INVALID_ID;

	// save as used
	decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

	return surface;
    }

    Error(_("video/vaapi: out of surfaces\n"));
    return VA_INVALID_ID;
}

///
///	Release a surface.
///
///	@param decoder	VA-API decoder
///	@param surface	surface no longer used
///
static void VaapiReleaseSurface(VaapiDecoder * decoder, VASurfaceID surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	if (decoder->SurfacesUsed[i] == surface) {
	    // no problem, with last used
	    decoder->SurfacesUsed[i] =
		decoder->SurfacesUsed[--decoder->SurfaceUsedN];
	    decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
	    return;
	}
    }
    Error(_("video/vaapi: release surface %#010x, which is not in use\n"),
	surface);
}

//	Init/Exit ------------------------------------------------------------

///
///	Debug VA-API decoder frames drop...
///
///	@param decoder	video hardware decoder
///
static void VaapiPrintFrames(const VaapiDecoder * decoder)
{
    Debug(3, "video/vaapi: %d missed, %d duped, %d dropped frames of %d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter);
#ifndef DEBUG
    (void)decoder;
#endif
}

///
///	Initialize surface flags.
///
///	@param decoder	video hardware decoder
///
static void VaapiInitSurfaceFlags(VaapiDecoder * decoder)
{
    int i;

    for (i = 0; i < VideoResolutionMax; ++i) {
	decoder->SurfaceFlagsTable[i] = VA_CLEAR_DRAWABLE;
	// color space conversion none, ITU-R BT.601, ITU-R BT.709
	if (i > VideoResolution576i) {
	    decoder->SurfaceFlagsTable[i] |= VA_SRC_BT709;
	} else {
	    decoder->SurfaceFlagsTable[i] |= VA_SRC_BT601;
	}

	// scaling flags FAST, HQ, NL_ANAMORPHIC
	// FIXME: need to detect the backend to choose the parameter
	switch (VideoScaling[i]) {
	    case VideoScalingNormal:
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_DEFAULT;
		break;
	    case VideoScalingFast:
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_FAST;
		break;
	    case VideoScalingHQ:
		// vdpau backend supports only VA_FILTER_SCALING_HQ
		// vdpau backend with advanced deinterlacer and my GT-210
		// is too slow
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_HQ;
		break;
	    case VideoScalingAnamorphic:
		// intel backend supports only VA_FILTER_SCALING_NL_ANAMORPHIC;
		// don't use it, its for 4:3 -> 16:9 scaling
		decoder->SurfaceFlagsTable[i] |=
		    VA_FILTER_SCALING_NL_ANAMORPHIC;
		break;
	}

	// deinterlace flags (not yet supported by libva)
	switch (VideoDeinterlace[i]) {
	    case VideoDeinterlaceBob:
		break;
	    case VideoDeinterlaceWeave:
		break;
	    case VideoDeinterlaceTemporal:
		//FIXME: private hack
		//decoder->SurfaceFlagsTable[i] |= 0x00002000;
		break;
	    case VideoDeinterlaceTemporalSpatial:
		//FIXME: private hack
		//decoder->SurfaceFlagsTable[i] |= 0x00006000;
		break;
	    case VideoDeinterlaceSoftware:
		break;
	}
    }
}

///
///	Allocate new VA-API decoder.
///
///	@returns a new prepared va-api hardware decoder.
///
static VaapiDecoder *VaapiNewDecoder(void)
{
    VaapiDecoder *decoder;
    int i;

    if (VaapiDecoderN == 1) {
	Fatal(_("video/vaapi: out of decoders\n"));
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Fatal(_("video/vaapi: out of memory\n"));
    }
    decoder->VaDisplay = VaDisplay;
    decoder->Window = VideoWindow;

    VaapiInitSurfaceFlags(decoder);

    decoder->DeintImages[0].image_id = VA_INVALID_ID;
    decoder->DeintImages[1].image_id = VA_INVALID_ID;
    decoder->DeintImages[2].image_id = VA_INVALID_ID;
    decoder->DeintImages[3].image_id = VA_INVALID_ID;
    decoder->DeintImages[4].image_id = VA_INVALID_ID;

    decoder->Image->image_id = VA_INVALID_ID;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
	decoder->SurfacesUsed[i] = VA_INVALID_ID;
	decoder->SurfacesFree[i] = VA_INVALID_ID;
    }

    // setup video surface ring buffer
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VA_INVALID_ID;
    }

    decoder->BlackSurface = VA_INVALID_ID;

    //
    //	Setup ffmpeg vaapi context
    //
    decoder->VaapiContext->display = VaDisplay;
    decoder->VaapiContext->config_id = VA_INVALID_ID;
    decoder->VaapiContext->context_id = VA_INVALID_ID;

#ifdef USE_GLX
    decoder->GlxSurface[0] = VA_INVALID_ID;
    decoder->GlxSurface[1] = VA_INVALID_ID;
    if (GlxEnabled) {
	// FIXME: create GLX context here
    }
#endif

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    decoder->GetPutImage = !VaapiBuggyIntel;

    VaapiDecoders[VaapiDecoderN++] = decoder;

    return decoder;
}

///
///	Cleanup VA-API.
///
///	@param decoder	va-api hw decoder
///
static void VaapiCleanup(VaapiDecoder * decoder)
{
    int filled;
    VASurfaceID surface;
    int i;

    // flush output queue, only 1-2 frames buffered, no big loss
    while ((filled = atomic_read(&decoder->SurfacesFilled))) {
	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&decoder->SurfacesFilled);

	surface = decoder->SurfacesRb[decoder->SurfaceRead];
	if (surface == VA_INVALID_ID) {
	    Error(_("video/vaapi: invalid surface in ringbuffer\n"));
	    continue;
	}
	// can crash and hang
	if (0 && vaSyncSurface(decoder->VaDisplay, surface)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
    }

    if (decoder->SurfaceRead != decoder->SurfaceWrite) {
	abort();
    }
    // clear ring buffer
    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VA_INVALID_ID;
    }

    decoder->WrongInterlacedWarned = 0;

    //	cleanup image
    if (decoder->Image->image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay,
		decoder->Image->image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	decoder->Image->image_id = VA_INVALID_ID;
    }
    //	cleanup context and config
    if (decoder->VaapiContext) {

	if (decoder->VaapiContext->context_id != VA_INVALID_ID) {
	    if (vaDestroyContext(VaDisplay,
		    decoder->VaapiContext->context_id) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy context!\n"));
	    }
	    decoder->VaapiContext->context_id = VA_INVALID_ID;
	}

	if (decoder->VaapiContext->config_id != VA_INVALID_ID) {
	    if (vaDestroyConfig(VaDisplay,
		    decoder->VaapiContext->config_id) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy config!\n"));
	    }
	    decoder->VaapiContext->config_id = VA_INVALID_ID;
	}
    }
    //	cleanup surfaces
    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	VaapiDestroySurfaces(decoder);
    }
    // cleanup images
    if (decoder->DeintImages[0].image_id != VA_INVALID_ID) {
	VaapiDestroyDeinterlaceImages(decoder);
    }

    decoder->SurfaceField = 1;

    //decoder->FrameCounter = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
}

///
///	Destroy a VA-API decoder.
///
///	@param decoder	VA-API decoder
///
static void VaapiDelDecoder(VaapiDecoder * decoder)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i] == decoder) {
	    VaapiDecoders[i] = NULL;
	    VaapiDecoderN--;
	    // FIXME: must copy last slot into empty slot and --
	    break;
	}
    }

    VaapiCleanup(decoder);

    if (decoder->BlackSurface != VA_INVALID_ID) {
	//
	//	update OSD associate
	//
	if (VaOsdSubpicture != VA_INVALID_ID) {
	    if (vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		    &decoder->BlackSurface, 1) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't deassociate black surfaces\n"));
	    }
	}
	if (vaDestroySurfaces(decoder->VaDisplay, &decoder->BlackSurface, 1)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy a surface\n"));
	}
    }
#ifdef USE_GLX
    if (decoder->GlxSurface[0] != VA_INVALID_ID) {
	if (vaDestroySurfaceGLX(VaDisplay, decoder->GlxSurface[0])
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy glx surface!\n"));
	}
    }
    if (decoder->GlxSurface[1] != VA_INVALID_ID) {
	if (vaDestroySurfaceGLX(VaDisplay, decoder->GlxSurface[1])
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy glx surface!\n"));
	}
    }
    if (decoder->GlTexture[0]) {
	glDeleteTextures(2, decoder->GlTexture);
    }
#endif

    VaapiPrintFrames(decoder);

    free(decoder);
}

///
///	VA-API setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if VA-API could be initialized, false otherwise.
///
static int VaapiInit(const char *display_name)
{
    int major;
    int minor;
    VADisplayAttribute attr;
    const char *s;

    VaOsdImage.image_id = VA_INVALID_ID;
    VaOsdSubpicture = VA_INVALID_ID;

#ifdef USE_GLX
    if (GlxEnabled) {			// support glx
	VaDisplay = vaGetDisplayGLX(XlibDisplay);
    } else
#endif
    {
	VaDisplay = vaGetDisplay(XlibDisplay);
    }
    if (!VaDisplay) {
	Error(_("video/vaapi: Can't connect VA-API to X11 server on '%s'"),
	    display_name);
	return 0;
    }

    if (vaInitialize(VaDisplay, &major, &minor) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Can't inititialize VA-API on '%s'"),
	    display_name);
	vaTerminate(VaDisplay);
	VaDisplay = NULL;
	return 0;
    }
    s = vaQueryVendorString(VaDisplay);
    Info(_("video/vaapi: libva %d.%d (%s) initialized\n"), major, minor, s);

    //
    //	Setup fixes for driver bugs.
    //
    if (strstr(s, "VDPAU")) {
	Info(_("video/vaapi: use vdpau bug workaround\n"));
	setenv("VDPAU_VIDEO_PUTSURFACE_FAST", "0", 0);
	VaapiBuggyVdpau = 1;
    }
    if (strstr(s, "Intel i965")) {
	VaapiBuggyIntel = 1;
    }
    //
    //	check if driver makes a copy of the VA surface for display.
    //
    attr.type = VADisplayAttribDirectSurface;
    attr.flags = VA_DISPLAY_ATTRIB_GETTABLE;
    if (vaGetDisplayAttributes(VaDisplay, &attr, 1) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Can't get direct-surface attribute\n"));
	attr.value = 1;
    }
    Info(_("video/vaapi: VA surface is %s\n"),
	attr.value ? _("direct mapped") : _("copied"));
    // FIXME: handle the cases: new liba: Don't use it.

#if 0
    //
    //	check the chroma format
    //
    attr.type = VAConfigAttribRTFormat attr.flags = VA_DISPLAY_ATTRIB_GETTABLE;
#endif
    return 1;
}

///
///	VA-API cleanup
///
static void VaapiExit(void)
{
    int i;

    // FIXME: more VA-API cleanups...
    // FIXME: can hang with vdpau in pthread_rwlock_wrlock

    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i]) {
	    VaapiDelDecoder(VaapiDecoders[i]);
	    VaapiDecoders[i] = NULL;
	}
    }
    VaapiDecoderN = 0;

    if (!VaDisplay) {
	vaTerminate(VaDisplay);
	VaDisplay = NULL;
    }
}

//----------------------------------------------------------------------------

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	VA-API decoder
///
static void VaapiUpdateOutput(VaapiDecoder * decoder)
{
    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, &decoder->OutputX, &decoder->OutputY,
	&decoder->OutputWidth, &decoder->OutputHeight, &decoder->CropX,
	&decoder->CropY, &decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

///
///	Find VA-API profile.
///
///	Check if the requested profile is supported by VA-API.
///
///	@param profiles a table of all supported profiles
///	@param n	number of supported profiles
///	@param profile	requested profile
///
///	@returns the profile if supported, -1 if unsupported.
///
static VAProfile VaapiFindProfile(const VAProfile * profiles, unsigned n,
    VAProfile profile)
{
    unsigned u;

    for (u = 0; u < n; ++u) {
	if (profiles[u] == profile) {
	    return profile;
	}
    }
    return -1;
}

///
///	Find VA-API entry point.
///
///	Check if the requested entry point is supported by VA-API.
///
///	@param entrypoints	a table of all supported entrypoints
///	@param n		number of supported entrypoints
///	@param entrypoint	requested entrypoint
///
///	@returns the entry point if supported, -1 if unsupported.
///
static VAEntrypoint VaapiFindEntrypoint(const VAEntrypoint * entrypoints,
    unsigned n, VAEntrypoint entrypoint)
{
    unsigned u;

    for (u = 0; u < n; ++u) {
	if (entrypoints[u] == entrypoint) {
	    return entrypoint;
	}
    }
    return -1;
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
///	@note + 2 surface for software deinterlace
///
static enum PixelFormat Vaapi_get_format(VaapiDecoder * decoder,
    AVCodecContext * video_ctx, const enum PixelFormat *fmt)
{
    const enum PixelFormat *fmt_idx;
    VAProfile profiles[vaMaxNumProfiles(VaDisplay)];
    int profile_n;
    VAEntrypoint entrypoints[vaMaxNumEntrypoints(VaDisplay)];
    int entrypoint_n;
    int p;
    int e;
    VAConfigAttrib attrib;

    Debug(3, "video: new stream format %d\n", GetMsTicks() - VideoSwitch);

    // create initial black surface and display
    VaapiBlackSurface(decoder);
    // cleanup last context
    VaapiCleanup(decoder);

    if (!VideoHardwareDecoder || (video_ctx->codec_id == CODEC_ID_MPEG2VIDEO
	    && VideoHardwareDecoder == 1)
	) {				// hardware disabled by config
	Debug(3, "codec: hardware acceleration disabled\n");
	goto slow_path;
    }

    p = -1;
    e = -1;

    //	prepare va-api profiles
    if (vaQueryConfigProfiles(VaDisplay, profiles, &profile_n)) {
	Error(_("codec: vaQueryConfigProfiles failed"));
	goto slow_path;
    }
    Debug(3, "codec: %d profiles\n", profile_n);

    // check profile
    switch (video_ctx->codec_id) {
	case CODEC_ID_MPEG2VIDEO:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_MPEG2 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n, VAProfileMPEG2Main);
	    break;
	case CODEC_ID_MPEG4:
	case CODEC_ID_H263:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_MPEG4 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n,
		VAProfileMPEG4AdvancedSimple);
	    break;
	case CODEC_ID_H264:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_H264 + VIDEO_SURFACES_MAX + 2;
	    // try more simple formats, fallback to better
	    if (video_ctx->profile == FF_PROFILE_H264_BASELINE) {
		p = VaapiFindProfile(profiles, profile_n,
		    VAProfileH264Baseline);
		if (p == -1) {
		    p = VaapiFindProfile(profiles, profile_n,
			VAProfileH264Main);
		}
	    } else if (video_ctx->profile == FF_PROFILE_H264_MAIN) {
		p = VaapiFindProfile(profiles, profile_n, VAProfileH264Main);
	    }
	    if (p == -1) {
		p = VaapiFindProfile(profiles, profile_n, VAProfileH264High);
	    }
	    break;
	case CODEC_ID_WMV3:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_VC1 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Main);
	    break;
	case CODEC_ID_VC1:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_VC1 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Advanced);
	    break;
	default:
	    goto slow_path;
    }
    if (p == -1) {
	Debug(3, "\tno profile found\n");
	goto slow_path;
    }
    Debug(3, "\tprofile %d\n", p);

    // prepare va-api entry points
    if (vaQueryConfigEntrypoints(VaDisplay, p, entrypoints, &entrypoint_n)) {
	Error(_("codec: vaQueryConfigEntrypoints failed"));
	goto slow_path;
    }
    Debug(3, "codec: %d entrypoints\n", entrypoint_n);
    //	look through formats
    for (fmt_idx = fmt; *fmt_idx != PIX_FMT_NONE; fmt_idx++) {
	Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
	// check supported pixel format with entry point
	switch (*fmt_idx) {
	    case PIX_FMT_VAAPI_VLD:
		e = VaapiFindEntrypoint(entrypoints, entrypoint_n,
		    VAEntrypointVLD);
		break;
	    case PIX_FMT_VAAPI_MOCO:
	    case PIX_FMT_VAAPI_IDCT:
		Debug(3, "codec: this VA-API pixel format is not supported\n");
	    default:
		continue;
	}
	if (e != -1) {
	    Debug(3, "\tentry point %d\n", e);
	    break;
	}
    }
    if (e == -1) {
	Warning(_("codec: unsupported: slow path\n"));
	goto slow_path;
    }
    //
    //	prepare decoder
    //
    memset(&attrib, 0, sizeof(attrib));
    attrib.type = VAConfigAttribRTFormat;
    if (vaGetConfigAttributes(decoder->VaDisplay, p, e, &attrib, 1)) {
	Error(_("codec: can't get attributes"));
	goto slow_path;
    }
    if (attrib.value & VA_RT_FORMAT_YUV420) {
	Info(_("codec: YUV 420 supported\n"));
    }
    if (attrib.value & VA_RT_FORMAT_YUV422) {
	Info(_("codec: YUV 422 supported\n"));
    }
    if (attrib.value & VA_RT_FORMAT_YUV444) {
	Info(_("codec: YUV 444 supported\n"));
    }

    if (!(attrib.value & VA_RT_FORMAT_YUV420)) {
	Warning(_("codec: YUV 420 not supported\n"));
	goto slow_path;
    }
    // create a configuration for the decode pipeline
    if (vaCreateConfig(decoder->VaDisplay, p, e, &attrib, 1,
	    &decoder->VaapiContext->config_id)) {
	Error(_("codec: can't create config"));
	goto slow_path;
    }
    // FIXME: interlaced not valid here?
    decoder->Resolution =
	VideoResolutionGroup(video_ctx->width, video_ctx->height,
	decoder->Interlaced);
    // FIXME: need only to create and destroy surfaces for size changes
    //		or when number of needed surfaces changed!
    VaapiCreateSurfaces(decoder, video_ctx->width, video_ctx->height);

    // bind surfaces to context
    if (vaCreateContext(decoder->VaDisplay, decoder->VaapiContext->config_id,
	    video_ctx->width, video_ctx->height, VA_PROGRESSIVE,
	    decoder->SurfacesFree, decoder->SurfaceFreeN,
	    &decoder->VaapiContext->context_id)) {
	Error(_("codec: can't create context"));
	goto slow_path;
    }

    decoder->PixFmt = *fmt_idx;
    decoder->InputWidth = video_ctx->width;
    decoder->InputHeight = video_ctx->height;
    decoder->InputAspect = video_ctx->sample_aspect_ratio;
    VaapiUpdateOutput(decoder);

#ifdef USE_GLX
    if (GlxEnabled) {
	GlxSetupDecoder(decoder);
	// FIXME: try two textures, but vdpau-backend supports only 1 surface
	if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
		decoder->GlTexture[0], &decoder->GlxSurface[0])
	    != VA_STATUS_SUCCESS) {
	    Fatal(_("video/glx: can't create glx surfaces"));
	}
	// FIXME: this isn't usable with vdpau-backend
	/*
	   if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
	   decoder->GlTexture[1], &decoder->GlxSurface[1])
	   != VA_STATUS_SUCCESS) {
	   Fatal(_("video/glx: can't create glx surfaces"));
	   }
	 */
    }
#endif

    Debug(3, "\t%#010x %s\n", fmt_idx[0], av_get_pix_fmt_name(fmt_idx[0]));
    return *fmt_idx;

  slow_path:
    // no accelerated format found
    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 2;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;
    video_ctx->hwaccel_context = NULL;
    return avcodec_default_get_format(video_ctx, fmt);
}

///
///	Draw surface of the VA-API decoder with x11.
///
///	vaPutSurface with intel backend does sync on v-sync.
///
///	@param decoder	VA-API decoder
///	@param surface		VA-API surface id
///	@param interlaced	flag interlaced source
///	@param top_field_first	flag top_field_first for interlaced source
///	@param field		interlaced draw: 0 first field, 1 second field
///
static void VaapiPutSurfaceX11(VaapiDecoder * decoder, VASurfaceID surface,
    int interlaced, int top_field_first, int field)
{
    unsigned type;
    VAStatus status;
    uint32_t s;
    uint32_t e;

    // deinterlace
    if (interlaced
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceSoftware
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	if (top_field_first) {
	    if (field) {
		type = VA_BOTTOM_FIELD;
	    } else {
		type = VA_TOP_FIELD;
	    }
	} else {
	    if (field) {
		type = VA_TOP_FIELD;
	    } else {
		type = VA_BOTTOM_FIELD;
	    }
	}
    } else {
	type = VA_FRAME_PICTURE;
    }

    s = GetMsTicks();
    xcb_flush(Connection);
    if ((status = vaPutSurface(decoder->VaDisplay, surface, decoder->Window,
		// decoder src
		decoder->CropX, decoder->CropY, decoder->CropWidth,
		decoder->CropHeight,
		// video dst
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight, NULL, 0,
		type | decoder->SurfaceFlagsTable[decoder->Resolution]))
	!= VA_STATUS_SUCCESS) {
	// switching video kills VdpPresentationQueueBlockUntilSurfaceIdle
	Error(_("video/vaapi: vaPutSurface failed %d\n"), status);
    }

    if (0 && vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
    e = GetMsTicks();
    if (e - s > 2000) {
	Error(_("video/vaapi: gpu hung %d ms %d\n"), e - s,
	    decoder->FrameCounter);
	fprintf(stderr, _("video/vaapi: gpu hung %d ms %d\n"), e - s,
	    decoder->FrameCounter);
    }

    if (0) {
	// check if surface is really ready
	// VDPAU backend, says always ready
	VASurfaceStatus status;

	if (vaQuerySurfaceStatus(decoder->VaDisplay, surface, &status)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaQuerySurface failed\n"));
	    status = VASurfaceReady;
	}
	if (status != VASurfaceReady) {
	    Warning(_
		("video/vaapi: surface %#010x not ready: still displayed %d\n"),
		surface, status);
	    return;
	}
    }

    if (0) {
	int i;

	// look how the status changes the next 40ms
	for (i = 0; i < 40; ++i) {
	    VASurfaceStatus status;

	    if (vaQuerySurfaceStatus(VaDisplay, surface,
		    &status) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaQuerySurface failed\n"));
	    }
	    Debug(3, "video/vaapi: %2d %d\n", i, status);
	    usleep(1 * 1000);
	}
    }
    usleep(1 * 1000);
}

#ifdef USE_GLX

///
///	Render texture.
///
///	@param texture	2d texture
///
static inline void VideoRenderTexture(GLuint texture, int x, int y, int width,
    int height)
{
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);	// no color
    glBegin(GL_QUADS); {
	glTexCoord2f(1.0f, 1.0f);
	glVertex2i(x + width, y + height);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2i(x, y + height);
	glTexCoord2f(0.0f, 0.0f);
	glVertex2i(x, y);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2i(x + width, y);
#if 0
	glTexCoord2f(0.0f, 0.0f);
	glVertex2i(x, y);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2i(x, y + height);
	glTexCoord2f(1.0f, 1.0f);
	glVertex2i(x + width, y + height);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2i(x + width, y);
#endif
    }
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

///
///	Draw surface of the VA-API decoder with glx.
///
///	@param decoder	VA-API decoder
///	@param surface		VA-API surface id
///	@param interlaced	flag interlaced source
///	@param top_field_first	flag top_field_first for interlaced source
///	@param field		interlaced draw: 0 first field, 1 second field
///
static void VaapiPutSurfaceGLX(VaapiDecoder * decoder, VASurfaceID surface,
    int interlaced, int top_field_first, int field)
{
    unsigned type;
    uint32_t start;
    uint32_t copy;
    uint32_t end;

    // deinterlace
    if (interlaced
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	if (top_field_first) {
	    if (field) {
		type = VA_BOTTOM_FIELD;
	    } else {
		type = VA_TOP_FIELD;
	    }
	} else {
	    if (field) {
		type = VA_TOP_FIELD;
	    } else {
		type = VA_BOTTOM_FIELD;
	    }
	}
    } else {
	type = VA_FRAME_PICTURE;
    }
    start = GetMsTicks();
    if (vaCopySurfaceGLX(decoder->VaDisplay, decoder->GlxSurface[0], surface,
	    type | decoder->SurfaceFlagsTable[decoder->Resolution]) !=
	VA_STATUS_SUCCESS) {
	Error(_("video/glx: vaCopySurfaceGLX failed\n"));
	return;
    }
    copy = GetMsTicks();
    // hardware surfaces are always busy
    VideoRenderTexture(decoder->GlTexture[0], decoder->OutputX,
	decoder->OutputY, decoder->OutputWidth, decoder->OutputHeight);
    end = GetMsTicks();
    //Debug(3, "video/vaapi/glx: %d copy %d render\n", copy - start, end - copy);
}

#endif

///
///	Find VA-API image format.
///
///	@param decoder	VA-API decoder
///	@param pix_fmt		ffmpeg pixel format
///	@param[out] format	image format
///
///	FIXME: can fallback from I420 to YV12, if not supported
///	FIXME: must check if put/get with this format is supported (see intel)
///
static int VaapiFindImageFormat(VaapiDecoder * decoder,
    enum PixelFormat pix_fmt, VAImageFormat * format)
{
    VAImageFormat *imgfrmts;
    int imgfrmt_n;
    int i;
    unsigned fourcc;

    switch (pix_fmt) {			// convert ffmpeg to VA-API
	    // NV12, YV12, I420, BGRA
	    // intel: I420 is native format for MPEG-2 decoded surfaces
	    // intel: NV12 is native format for H.264 decoded surfaces
	case PIX_FMT_YUV420P:
	    // fourcc = VA_FOURCC_YV12; // YVU
	    fourcc = VA_FOURCC('I', '4', '2', '0');	// YUV
	    break;
	case PIX_FMT_NV12:
	    fourcc = VA_FOURCC_NV12;
	    break;
	default:
	    Fatal(_("video/vaapi: unsupported pixel format %d\n"), pix_fmt);
    }

    imgfrmt_n = vaMaxNumImageFormats(decoder->VaDisplay);
    imgfrmts = alloca(imgfrmt_n * sizeof(*imgfrmts));

    if (vaQueryImageFormats(decoder->VaDisplay, imgfrmts, &imgfrmt_n)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaQueryImageFormats failed\n"));
	return 0;
    }
    Debug(3, "video/vaapi: search format %c%c%c%c in %d image formats\n",
	fourcc, fourcc >> 8, fourcc >> 16, fourcc >> 24, imgfrmt_n);
    Debug(3, "video/vaapi: supported image formats:\n");
    for (i = 0; i < imgfrmt_n; ++i) {
	Debug(3, "video/vaapi:\t%c%c%c%c\t%d\n", imgfrmts[i].fourcc,
	    imgfrmts[i].fourcc >> 8, imgfrmts[i].fourcc >> 16,
	    imgfrmts[i].fourcc >> 24, imgfrmts[i].depth);
    }
    //
    //	search image format
    //
    for (i = 0; i < imgfrmt_n; ++i) {
	if (imgfrmts[i].fourcc == fourcc) {
	    *format = imgfrmts[i];
	    Debug(3, "video/vaapi: use\t%c%c%c%c\t%d\n", imgfrmts[i].fourcc,
		imgfrmts[i].fourcc >> 8, imgfrmts[i].fourcc >> 16,
		imgfrmts[i].fourcc >> 24, imgfrmts[i].depth);
	    return 1;
	}
    }

    Fatal("video/vaapi: pixel format %d unsupported by VA-API\n", pix_fmt);
    // FIXME: no fatal error!

    return 0;
}

///
///	Configure VA-API for new video format.
///
///	@param decoder	VA-API decoder
///
///	@note called only for software decoder.
///
static void VaapiSetup(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx)
{
    int width;
    int height;
    VAImageFormat format[1];

    // create initial black surface and display
    VaapiBlackSurface(decoder);
    // cleanup last context
    VaapiCleanup(decoder);

    width = video_ctx->width;
    height = video_ctx->height;
    // FIXME: remove this if
    if (decoder->Image->image_id != VA_INVALID_ID) {
	abort();			// should be done by VaapiCleanup()
    }
    VaapiFindImageFormat(decoder, video_ctx->pix_fmt, format);

    // FIXME: this image is only needed for software decoder and auto-crop
    if (decoder->GetPutImage
	&& vaCreateImage(VaDisplay, format, width, height,
	    decoder->Image) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create image!\n"));
    }
    Debug(3,
	"video/vaapi: created image %dx%d with id 0x%08x and buffer id 0x%08x\n",
	width, height, decoder->Image->image_id, decoder->Image->buf);

    // FIXME: interlaced not valid here?
    decoder->Resolution =
	VideoResolutionGroup(width, height, decoder->Interlaced);
    VaapiCreateSurfaces(decoder, width, height);

#ifdef USE_GLX
    if (GlxEnabled) {
	// FIXME: destroy old context

	GlxSetupDecoder(decoder);
	// FIXME: try two textures
	if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
		decoder->GlTexture[0], &decoder->GlxSurface[0])
	    != VA_STATUS_SUCCESS) {
	    Fatal(_("video/glx: can't create glx surfaces"));
	}
	/*
	   if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
	   decoder->GlTexture[1], &decoder->GlxSurface[1])
	   != VA_STATUS_SUCCESS) {
	   Fatal(_("video/glx: can't create glx surfaces"));
	   }
	 */
    }
#endif
}

#ifdef USE_AUTOCROP

///
///	VA-API auto-crop support.
///
///	@param decoder	VA-API hw decoder
///
static void VaapiAutoCrop(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    uint32_t width;
    uint32_t height;
    void *va_image_data;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;
    int i;

    width = decoder->InputWidth;
    height = decoder->InputHeight;

  again:
    if (decoder->GetPutImage && decoder->Image->image_id == VA_INVALID_ID) {
	VAImageFormat format[1];

	Debug(3, "video/vaapi: download image not available\n");

	// FIXME: PixFmt not set!
	//VaapiFindImageFormat(decoder, decoder->PixFmt, format);
	VaapiFindImageFormat(decoder, PIX_FMT_NV12, format);
	//VaapiFindImageFormat(decoder, PIX_FMT_YUV420P, format);
	if (vaCreateImage(VaDisplay, format, width, height,
		decoder->Image) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create image!\n"));
	    return;
	}
    }
    // no problem to go back, we just wrote it
    // FIXME: we can pass the surface through.
    surface =
	decoder->SurfacesRb[(decoder->SurfaceWrite + VIDEO_SURFACES_MAX -
	    1) % VIDEO_SURFACES_MAX];

    //	Copy data from frame to image
    if (!decoder->GetPutImage
	&& vaDeriveImage(decoder->VaDisplay, surface,
	    decoder->Image) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed\n"));
	decoder->GetPutImage = 1;
	goto again;
    }
    if (decoder->GetPutImage
	&& (i =
	    vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
		decoder->InputHeight,
		decoder->Image->image_id)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get auto-crop image %d\n"), i);
	printf(_("video/vaapi: can't get auto-crop image %d\n"), i);
	return;
    }
    if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map auto-crop image!\n"));
	return;
    }
    // convert vaapi to our frame format
    for (i = 0; (unsigned)i < decoder->Image->num_planes; ++i) {
	data[i] = va_image_data + decoder->Image->offsets[i];
	pitches[i] = decoder->Image->pitches[i];
    }

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);

    if (vaUnmapBuffer(VaDisplay, decoder->Image->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap auto-crop image!\n"));
    }
    if (!decoder->GetPutImage) {
	if (vaDestroyImage(VaDisplay, decoder->Image->image_id)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	decoder->Image->image_id = VA_INVALID_ID;
    }
    // FIXME: this a copy of vdpau, combine the two same things

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %d+%d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,
	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
	decoder->CropX = 0;
	decoder->CropY = (next_state == 16 ? crop16 : crop14) + VideoSkipLines;
	decoder->CropWidth = decoder->InputWidth;
	decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

	// FIXME: this overwrites user choosen output position
	// FIXME: resize kills the auto crop values
	// FIXME: support other 4:3 zoom modes
	decoder->OutputX = 0;
	decoder->OutputY = 0;
	decoder->OutputWidth = (VideoWindowHeight * next_state) / 9;
	decoder->OutputHeight = (VideoWindowWidth * 9) / next_state;
	if ((unsigned)decoder->OutputWidth > VideoWindowWidth) {
	    decoder->OutputWidth = VideoWindowWidth;
	    decoder->OutputY = (VideoWindowHeight - decoder->OutputHeight) / 2;
	} else if ((unsigned)decoder->OutputHeight > VideoWindowHeight) {
	    decoder->OutputHeight = VideoWindowHeight;
	    decoder->OutputX = (VideoWindowWidth - decoder->OutputWidth) / 2;
	}
	Debug(3, "video: aspect output %dx%d %dx%d+%d+%d\n",
	    decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
	    decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
	// sets AutoCrop->Count
	VaapiUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;
}

///
///	VA-API check if auto-crop todo.
///
///	@param decoder	VA-API hw decoder
///
///	@note a copy of VdpauCheckAutoCrop
///
static void VaapiCheckAutoCrop(VaapiDecoder * decoder)
{
    // reduce load, check only n frames
    if (AutoCropInterval && !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational display_aspect_ratio;

	av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	// only 4:3 with 16:9/14:9 inside supported
	if (display_aspect_ratio.num == 4 && display_aspect_ratio.den == 3) {
	    VaapiAutoCrop(decoder);
	} else {
	    decoder->AutoCrop->Count = 0;
	    decoder->AutoCrop->State = 0;
	}
    }
}

///
///	VA-API reset auto-crop.
///
static void VaapiResetAutoCrop(void)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiDecoders[i]->AutoCrop->State = 0;
	VaapiDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif

///
///	Queue output surface.
///
///	@param decoder	VA-API decoder
///	@param surface	output surface
///	@param softdec	software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void VaapiQueueSurface(VaapiDecoder * decoder, VASurfaceID surface,
    int softdec)
{
    VASurfaceID old;

    ++decoder->FrameCounter;

    if (1) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    ++decoder->FramesDropped;
	    Warning(_("video: output buffer full, dropping frame (%d/%d)\n"),
		decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
		VaapiPrintFrames(decoder);
	    }
	    if (softdec) {		// software surfaces only
		VaapiReleaseSurface(decoder, surface);
	    }
	    return;
	}
#if 0
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    VideoDisplayHandler();
	}
#endif
    }

    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])
	!= VA_INVALID_ID) {

	if (vaSyncSurface(decoder->VaDisplay, old) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
#if 0
	VASurfaceStatus status;

	if (vaQuerySurfaceStatus(decoder->VaDisplay, old, &status)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaQuerySurface failed\n"));
	    status = VASurfaceReady;
	}
	if (status != VASurfaceReady) {
	    Warning(_
		("video/vaapi: surface %#010x not ready: still displayed %d\n"),
		old, status);
	    if (0
		&& vaSyncSurface(decoder->VaDisplay,
		    old) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaSyncSurface failed\n"));
	    }
	}
#endif

	// now we can release the surface
	if (softdec) {			// software surfaces only
	    VaapiReleaseSurface(decoder, old);
	}
    }
#if 0
    // FIXME: intel seems to forget this, nvidia GT 210 has speed problems here
    if (VaapiBuggyIntel && VaOsdSubpicture != VA_INVALID_ID) {
	// FIXME: associate only if osd is displayed

	//
	//	associate the OSD with surface
	//
	if (VaapiUnscaledOsd) {
	    if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture, &surface, 1,
		    0, 0, VaOsdImage.width, VaOsdImage.height, 0, 0,
		    VideoWindowWidth, VideoWindowHeight,
		    VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't associate subpicture\n"));
	    }
	} else {
	    // FIXME: auto-crop wrong position
	    if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture, &surface, 1,
		    0, 0, VaOsdImage.width, VaOsdImage.height, 0, 0,
		    decoder->InputWidth, decoder->InputHeight, 0)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't associate subpicture\n"));
	    }
	}
    }
#endif

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)
	% VIDEO_SURFACES_MAX;
    atomic_inc(&decoder->SurfacesFilled);

    Debug(4, "video/vaapi: yy video surface %#010x ready\n", surface);
}

#if 0

    /// Return the absolute value of an integer.
#define ABS(i)	((i) >= 0 ? (i) : (-(i)))

///
///	ELA Edge-based Line Averaging
///	Low-Complexity Interpolation Method
///
///	abcdefg	   abcdefg	abcdefg	 abcdefg    abcdefg
///	   x	     x		  x	    x		 x
///	hijklmn	 hijklmn    hijklmn	   hijklmn	 hijklmn
///
static void FilterLine(const uint8_t * past, const uint8_t * cur,
    const uint8_t * future, int width, int above, int below)
{
    int a, b, c, d, e, f, g, h, i, j, k, l, m, n;
}

#endif

///
///	Create and display a black empty surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiBlackSurface(VaapiDecoder * decoder)
{
    VAStatus status;
    uint32_t start;
    uint32_t sync;
    uint32_t put1;

    // wait until we have osd subpicture
    if (VaOsdSubpicture == VA_INVALID_ID) {
	Warning(_("video/vaapi: no osd subpicture yet\n"));
	return;
    }

    if (decoder->BlackSurface == VA_INVALID_ID) {
	if (vaCreateSurfaces(decoder->VaDisplay, VideoWindowWidth,
		VideoWindowHeight, VA_RT_FORMAT_YUV420, 1,
		&decoder->BlackSurface) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create a surface\n"));
	    return;
	}
	// full sized surface, no difference unscaled/scaled osd
	if (vaAssociateSubpicture(decoder->VaDisplay, VaOsdSubpicture,
		&decoder->BlackSurface, 1, 0, 0, VaOsdImage.width,
		VaOsdImage.height, 0, 0, VideoWindowWidth, VideoWindowHeight,
		0) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
	Debug(3, "video/vaapi: associate %08x\n", decoder->BlackSurface);
	// FIXME: check if intel forgets this also

	if (0 && decoder->Image->image_id == VA_INVALID_ID) {
	    VAImageFormat format[1];
	    void *va_image_data;
	    int i;

	    printf("No image\n");
	    VaapiFindImageFormat(decoder, PIX_FMT_NV12, format);
	    if ((status =
		    vaDeriveImage(decoder->VaDisplay, decoder->BlackSurface,
			decoder->Image)) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
		if (vaCreateImage(VaDisplay, format, VideoWindowWidth,
			VideoWindowHeight,
			decoder->Image) != VA_STATUS_SUCCESS) {
		    Error(_("video/vaapi: can't create image!\n"));
		}
	    }
	    if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't map the image!\n"));
	    }

	    for (i = 0; (unsigned)i < decoder->Image->data_size; i += 2) {
		((uint8_t *) va_image_data)[i + 0] = 0xFF;
		((uint8_t *) va_image_data)[i + 1] = 0xFF;
	    }

	    if (vaUnmapBuffer(VaDisplay,
		    decoder->Image->buf) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't unmap the image!\n"));
	    }
	    if (vaDestroyImage(VaDisplay,
		    decoder->Image->image_id) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy image!\n"));
	    }
	}
	// FIXME: intel didn't support put image.
	if (0
	    && vaPutImage(VaDisplay, decoder->BlackSurface,
		decoder->Image->image_id, 0, 0, VideoWindowWidth,
		VideoWindowHeight, 0, 0, VideoWindowWidth, VideoWindowHeight)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't put image!\n"));
	}

	start = GetMsTicks();
	if (vaSyncSurface(decoder->VaDisplay,
		decoder->BlackSurface) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
    } else {
	start = GetMsTicks();
    }

    Debug(4, "video/vaapi: yy black video surface %#010x displayed\n",
	decoder->BlackSurface);
    sync = GetMsTicks();
    xcb_flush(Connection);
    if ((status =
	    vaPutSurface(decoder->VaDisplay, decoder->BlackSurface,
		decoder->Window,
		// decoder src
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight,
		// video dst
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight, NULL, 0,
		VA_FRAME_PICTURE)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaPutSurface failed %d\n"), status);
    }
    clock_gettime(CLOCK_REALTIME, &decoder->FrameTime);

    put1 = GetMsTicks();
    if (put1 - sync > 2000) {
	Error(_("video/vaapi: gpu hung %d ms %d\n"), put1 - sync,
	    decoder->FrameCounter);
	fprintf(stderr, _("video/vaapi: gpu hung %d ms %d\n"), put1 - sync,
	    decoder->FrameCounter);
    }
    Debug(4, "video/vaapi: sync %2u put1 %2u\n", sync - start, put1 - sync);

    if (0 && vaSyncSurface(decoder->VaDisplay, decoder->BlackSurface)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
    usleep(1 * 1000);
}

///
///	Vaapi bob deinterlace.
///
///	@note FIXME: use common software deinterlace functions.
///
static void VaapiBob(VaapiDecoder * decoder, VAImage * src, VAImage * dst1,
    VAImage * dst2)
{
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
    uint32_t tick6;
    uint32_t tick7;
    uint32_t tick8;
    void *src_base;
    void *dst1_base;
    void *dst2_base;
    unsigned y;
    unsigned p;

    tick1 = GetMsTicks();
    if (vaMapBuffer(decoder->VaDisplay, src->buf,
	    &src_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
    tick2 = GetMsTicks();
    if (vaMapBuffer(decoder->VaDisplay, dst1->buf,
	    &dst1_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
    tick3 = GetMsTicks();
    if (vaMapBuffer(decoder->VaDisplay, dst2->buf,
	    &dst2_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
    tick4 = GetMsTicks();

    if (0) {				// test all updated
	memset(dst1_base, 0x00, dst1->data_size);
	memset(dst2_base, 0xFF, dst2->data_size);
	return;
    }
#if 0
    // interleave
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst1_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);

	    memcpy(dst2_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	}
    }
#endif
#if 1
    // use tmp copy
    if (1) {
	uint8_t *tmp;

	tmp = malloc(src->data_size);
	memcpy(tmp, src_base, src->data_size);

	for (p = 0; p < src->num_planes; ++p) {
	    for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
		memcpy(dst1_base + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);
		memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);

		memcpy(dst2_base + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
		memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
	    }

	}
	free(tmp);
    }
#endif
#if 0
    // use multiple tmp copy
    if (1) {
	uint8_t *tmp_src;
	uint8_t *tmp_dst1;
	uint8_t *tmp_dst2;

	tmp_src = malloc(src->data_size);
	memcpy(tmp_src, src_base, src->data_size);
	tmp_dst1 = malloc(src->data_size);
	tmp_dst2 = malloc(src->data_size);

	for (p = 0; p < src->num_planes; ++p) {
	    for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
		memcpy(tmp_dst1 + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);
		memcpy(tmp_dst1 + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);

		memcpy(tmp_dst2 + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
		memcpy(tmp_dst2 + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
	    }
	}
	memcpy(dst1_base, tmp_dst1, src->data_size);
	memcpy(dst2_base, tmp_dst2, src->data_size);

	free(tmp_src);
	free(tmp_dst1);
	free(tmp_dst2);
    }
#endif
#if 0
    // dst1 first
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst1_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);
	}
    }
    // dst2 next
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst2_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	}
    }
#endif

    tick5 = GetMsTicks();

    if (vaUnmapBuffer(decoder->VaDisplay, dst2->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
    tick6 = GetMsTicks();
    if (vaUnmapBuffer(decoder->VaDisplay, dst1->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
    tick7 = GetMsTicks();
    if (vaUnmapBuffer(decoder->VaDisplay, src->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
    tick8 = GetMsTicks();

    Debug(3, "video/vaapi: map=%2d/%2d/%2d deint=%2d umap=%2d/%2d/%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4,
	tick6 - tick5, tick7 - tick6, tick8 - tick7);
}

///
///	Create software deinterlace images.
///
///	@param decoder		VA-API decoder
///
static void VaapiCreateDeinterlaceImages(VaapiDecoder * decoder)
{
    VAImageFormat format[1];
    int i;

    // NV12, YV12, I420, BGRA
    // NV12 Y U/V 2x2
    // YV12 Y V U 2x2
    // I420 Y U V 2x2

    // Intel needs NV12
    VaapiFindImageFormat(decoder, PIX_FMT_NV12, format);
    //VaapiFindImageFormat(decoder, PIX_FMT_YUV420P, format);
    for (i = 0; i < 5; ++i) {
	if (vaCreateImage(decoder->VaDisplay, format, decoder->InputWidth,
		decoder->InputHeight,
		decoder->DeintImages + i) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create image!\n"));
	}
    }
#ifdef DEBUG
    if (1) {
	VAImage *img;

	img = decoder->DeintImages;
	Debug(3, "video/vaapi: %c%c%c%c %dx%d*%d\n", img->format.fourcc,
	    img->format.fourcc >> 8, img->format.fourcc >> 16,
	    img->format.fourcc >> 24, img->width, img->height,
	    img->num_planes);
    }
#endif
}

///
///	Destroy software deinterlace images.
///
///	@param decoder	VA-API decoder
///
static void VaapiDestroyDeinterlaceImages(VaapiDecoder * decoder)
{
    int i;

    for (i = 0; i < 5; ++i) {
	if (vaDestroyImage(decoder->VaDisplay,
		decoder->DeintImages[i].image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	decoder->DeintImages[i].image_id = VA_INVALID_ID;
    }
}

///
///	Vaapi software deinterlace.
///
///	@param decoder	VA-API decoder
///	@param surface	interlaced hardware surface
///
static void VaapiCpuDerive(VaapiDecoder * decoder, VASurfaceID surface)
{
    //
    //	vaPutImage not working, vaDeriveImage
    //
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
    VAImage image[1];
    VAImage dest1[1];
    VAImage dest2[1];
    VAStatus status;
    VASurfaceID out1;
    VASurfaceID out2;

    tick1 = GetMsTicks();
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, surface,
		image)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
	VaapiQueueSurface(decoder, surface, 0);
	VaapiQueueSurface(decoder, surface, 0);
	return;
    }
    tick2 = GetMsTicks();

    Debug(4, "video/vaapi: %c%c%c%c %dx%d*%d\n", image->format.fourcc,
	image->format.fourcc >> 8, image->format.fourcc >> 16,
	image->format.fourcc >> 24, image->width, image->height,
	image->num_planes);

    // get a free surfaces
    out1 = VaapiGetSurface(decoder);
    if (out1 == VA_INVALID_ID) {
	abort();
    }
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, out1,
		dest1)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
    }
    tick3 = GetMsTicks();
    out2 = VaapiGetSurface(decoder);
    if (out2 == VA_INVALID_ID) {
	abort();
    }
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, out2,
		dest2)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
    }
    tick4 = GetMsTicks();

    VaapiBob(decoder, image, dest1, dest2);
    tick5 = GetMsTicks();

    if (vaDestroyImage(VaDisplay, image->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy image!\n"));
    }
    if (vaDestroyImage(VaDisplay, dest1->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy image!\n"));
    }
    if (vaDestroyImage(VaDisplay, dest2->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy image!\n"));
    }

    VaapiQueueSurface(decoder, out1, 1);
    VaapiQueueSurface(decoder, out2, 1);

    tick5 = GetMsTicks();

    Debug(4, "video/vaapi: get=%2d get1=%2d get2=%d deint=%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4);
}

///
///	Vaapi software deinterlace.
///
///	@param decoder	VA-API decoder
///	@param surface	interlaced hardware surface
///
static void VaapiCpuPut(VaapiDecoder * decoder, VASurfaceID surface)
{
    //
    //	vaPutImage working
    //
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
    VAImage *img1;
    VAImage *img2;
    VAImage *img3;
    VASurfaceID out;
    VAStatus status;

    //
    //	Create deinterlace images.
    //
    if (decoder->DeintImages[0].image_id == VA_INVALID_ID) {
	VaapiCreateDeinterlaceImages(decoder);
    }

    if (0 && vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }

    img1 = decoder->DeintImages;
    img2 = decoder->DeintImages + 1;
    img3 = decoder->DeintImages + 2;

    tick1 = GetMsTicks();
    if (vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
	    decoder->InputHeight, img1->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get source image\n"));
	VaapiQueueSurface(decoder, surface, 0);
	VaapiQueueSurface(decoder, surface, 0);
	return;
    }
    tick2 = GetMsTicks();

    // FIXME: handle top_field_first

    VaapiBob(decoder, img1, img2, img3);
    tick3 = GetMsTicks();

    // get a free surface and upload the image
    out = VaapiGetSurface(decoder);
    if (out == VA_INVALID_ID) {
	abort();
    }
    if ((status =
	    vaPutImage(VaDisplay, out, img2->image_id, 0, 0, img2->width,
		img2->height, 0, 0, img2->width,
		img2->height)) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't put image: %d!\n", status);
	abort();
    }
    VaapiQueueSurface(decoder, out, 1);
    if (0 && vaSyncSurface(decoder->VaDisplay, out) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
    tick4 = GetMsTicks();

    Debug(4, "video/vaapi: deint %d %#010x -> %#010x\n", decoder->SurfaceField,
	surface, out);

    // get a free surface and upload the image
    out = VaapiGetSurface(decoder);
    if (out == VA_INVALID_ID) {
	abort();
    }
    if (vaPutImage(VaDisplay, out, img3->image_id, 0, 0, img3->width,
	    img3->height, 0, 0, img3->width,
	    img3->height) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't put image!\n");
    }
    VaapiQueueSurface(decoder, out, 1);
    if (0 && vaSyncSurface(decoder->VaDisplay, out) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
    tick5 = GetMsTicks();

    Debug(4, "video/vaapi: get=%2d deint=%2d put1=%2d put2=%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4);
}

///
///	Vaapi software deinterlace.
///
///	@param decoder	VA-API decoder
///	@param surface	interlaced hardware surface
///
static void VaapiCpuDeinterlace(VaapiDecoder * decoder, VASurfaceID surface)
{
    if (VaapiBuggyIntel) {
	VaapiCpuDerive(decoder, surface);
    } else {
	VaapiCpuPut(decoder, surface);
    }
    // FIXME: must release software input surface
}

///
///	Render a ffmpeg frame
///
///	@param decoder		VA-API decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VaapiRenderFrame(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VASurfaceID surface;
    int interlaced;

    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
    interlaced = frame->interlaced_frame;
    if (video_ctx->height == 720) {
	if (interlaced && !decoder->WrongInterlacedWarned) {
	    Debug(3, "video/vaapi: wrong interlace flag fixed\n");
	    decoder->WrongInterlacedWarned = 1;
	}
	interlaced = 0;
    } else {
	if (!interlaced && !decoder->WrongInterlacedWarned) {
	    Debug(3, "video/vaapi: wrong interlace flag fixed\n");
	    decoder->WrongInterlacedWarned = 1;
	}
	interlaced = 1;
    }

    // FIXME: should be done by init video_ctx->field_order
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != frame->top_field_first) {

#if 0
	// field_order only in git
	Debug(3, "video/vaapi: interlaced %d top-field-first %d - %d\n",
	    interlaced, frame->top_field_first, video_ctx->field_order);
#else
	Debug(3, "video/vaapi: interlaced %d top-field-first %d\n", interlaced,
	    frame->top_field_first);
#endif

	decoder->Interlaced = interlaced;
	decoder->TopFieldFirst = frame->top_field_first;
	decoder->SurfaceField = 1;
    }
    // update aspect ratio changes
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53,60,100)
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, frame->sample_aspect_ratio)) {
	Debug(3, "video/vaapi: aspect ratio changed\n");

	decoder->InputAspect = frame->sample_aspect_ratio;
	VaapiUpdateOutput(decoder);
    }
#else
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, video_ctx->sample_aspect_ratio)) {
	Debug(3, "video/vaapi: aspect ratio changed\n");

	decoder->InputAspect = video_ctx->sample_aspect_ratio;
	VaapiUpdateOutput(decoder);
    }
#endif

    //
    // Hardware render
    //
    if (video_ctx->hwaccel_context) {

	if (video_ctx->height != decoder->InputHeight
	    || video_ctx->width != decoder->InputWidth) {
	    Error(_("video/vaapi: stream <-> surface size mismatch\n"));
	    return;
	}

	surface = (unsigned)(size_t) frame->data[3];
	Debug(4, "video/vaapi: hw render hw surface %#010x\n", surface);

	if (interlaced
	    && VideoDeinterlace[decoder->Resolution] ==
	    VideoDeinterlaceSoftware) {
	    VaapiCpuDeinterlace(decoder, surface);
	} else {
	    VaapiQueueSurface(decoder, surface, 0);
	}

	//
	// VAImage render
	//
    } else {
	void *va_image_data;
	int i;
	AVPicture picture[1];
	int width;
	int height;

	Debug(4, "video/vaapi: hw render sw surface\n");

	width = video_ctx->width;
	height = video_ctx->height;
	//
	//	Check image, format, size
	//
	if ((decoder->GetPutImage && decoder->Image->image_id == VA_INVALID_ID)
	    || decoder->PixFmt != video_ctx->pix_fmt
	    || width != decoder->InputWidth
	    || height != decoder->InputHeight) {

	    Debug(3,
		"video/vaapi: stream <-> surface size/interlace mismatch\n");

	    decoder->PixFmt = video_ctx->pix_fmt;
	    // FIXME: aspect done above!
	    decoder->InputWidth = width;
	    decoder->InputHeight = height;

	    VaapiSetup(decoder, video_ctx);
	    VaapiUpdateOutput(decoder);
	}
	// FIXME: Need to insert software deinterlace here
	// FIXME: can/must insert auto-crop here (is done after upload)

	// get a free surface and upload the image
	surface = VaapiGetSurface(decoder);
	Debug(4, "video/vaapi: video surface %#010x displayed\n", surface);

	if (!decoder->GetPutImage
	    && vaDeriveImage(decoder->VaDisplay, surface,
		decoder->Image) != VA_STATUS_SUCCESS) {
	    VAImageFormat format[1];

	    Error(_("video/vaapi: vaDeriveImage failed\n"));

	    decoder->GetPutImage = 1;
	    VaapiFindImageFormat(decoder, video_ctx->pix_fmt, format);
	    if (vaCreateImage(VaDisplay, format, width, height,
		    decoder->Image) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't create image!\n"));
	    }
	}
	//
	//	Copy data from frame to image
	//
	if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't map the image!\n"));
	}
	// crazy: intel mixes YV12 and NV12 with mpeg
	if (decoder->Image->format.fourcc == VA_FOURCC_NV12) {
	    int x;

	    // intel NV12 convert YV12 to NV12

	    // copy Y
	    for (i = 0; i < height; ++i) {
		memcpy(va_image_data + decoder->Image->offsets[0] +
		    decoder->Image->pitches[0] * i,
		    frame->data[0] + frame->linesize[0] * i,
		    frame->linesize[0]);
	    }
	    // copy UV
	    for (i = 0; i < height / 2; ++i) {
		for (x = 0; x < frame->linesize[1]; ++x) {
		    ((uint8_t *) va_image_data)[decoder->Image->offsets[1]
			+ decoder->Image->pitches[1] * i + x * 2 + 0]
			= frame->data[1][i * frame->linesize[1] + x];
		    ((uint8_t *) va_image_data)[decoder->Image->offsets[1]
			+ decoder->Image->pitches[1] * i + x * 2 + 1]
			= frame->data[2][i * frame->linesize[2] + x];
		}
	    }
	    // vdpau uses this
	} else if (decoder->Image->format.fourcc == VA_FOURCC('I', '4', '2',
		'0')) {
	    picture->data[0] = va_image_data + decoder->Image->offsets[0];
	    picture->linesize[0] = decoder->Image->pitches[0];
	    picture->data[1] = va_image_data + decoder->Image->offsets[1];
	    picture->linesize[1] = decoder->Image->pitches[2];
	    picture->data[2] = va_image_data + decoder->Image->offsets[2];
	    picture->linesize[2] = decoder->Image->pitches[1];

	    av_picture_copy(picture, (AVPicture *) frame, video_ctx->pix_fmt,
		width, height);
	} else if (decoder->Image->num_planes == 3) {
	    picture->data[0] = va_image_data + decoder->Image->offsets[0];
	    picture->linesize[0] = decoder->Image->pitches[0];
	    picture->data[1] = va_image_data + decoder->Image->offsets[2];
	    picture->linesize[1] = decoder->Image->pitches[2];
	    picture->data[2] = va_image_data + decoder->Image->offsets[1];
	    picture->linesize[2] = decoder->Image->pitches[1];

	    av_picture_copy(picture, (AVPicture *) frame, video_ctx->pix_fmt,
		width, height);
	}

	if (vaUnmapBuffer(VaDisplay, decoder->Image->buf)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't unmap the image!\n"));
	}

	Debug(4, "video/vaapi: buffer %dx%d <- %dx%d\n", decoder->Image->width,
	    decoder->Image->height, width, height);

	if (decoder->GetPutImage
	    && (i =
		vaPutImage(VaDisplay, surface, decoder->Image->image_id, 0, 0,
		    width, height, 0, 0, width,
		    height)) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't put image err:%d!\n"), i);
	}

	if (!decoder->GetPutImage) {
	    if (vaDestroyImage(VaDisplay, decoder->Image->image_id)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy image!\n"));
	    }
	    decoder->Image->image_id = VA_INVALID_ID;
	}

	VaapiQueueSurface(decoder, surface, 1);
    }

    if (decoder->Interlaced) {
	++decoder->FrameCounter;
    }
}

///
///	Advance displayed frame.
///
static void VaapiAdvanceFrame(void)
{
    int i;

    // show any frame as fast as possible
    // we keep always the last frame in the ring buffer

    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiDecoder *decoder;
	VASurfaceID surface;
	int filled;

	decoder = VaapiDecoders[i];
	filled = atomic_read(&decoder->SurfacesFilled);

	// 0 -> 1
	// 1 -> 0 + advance
	if (VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceSoftware
	    && decoder->Interlaced) {
	    // FIXME: first frame is never shown
	    if (decoder->SurfaceField) {
		if (filled > 1) {
		    decoder->SurfaceField = 0;
		}
	    } else {
		decoder->SurfaceField = 1;
		return;
	    }
	}

	if (filled > 1) {
	    decoder->SurfaceRead = (decoder->SurfaceRead + 1)
		% VIDEO_SURFACES_MAX;
	    atomic_dec(&decoder->SurfacesFilled);

	    // wait for rendering finished
	    surface = decoder->SurfacesRb[decoder->SurfaceRead];
	    if (vaSyncSurface(decoder->VaDisplay, surface)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaSyncSurface failed\n"));
	    }
	    // debug duplicate frames
	} else if (filled == 1) {
	    ++decoder->FramesDuped;
	    decoder->DropNextFrame = 0;
	    // FIXME: don't warn after stream start
	    Warning(_
		("video: display buffer empty, duping frame (%d/%d) %d\n"),
		decoder->FramesDuped, decoder->FrameCounter,
		atomic_read(&VideoPacketsFilled));
	    if (!(decoder->FramesDisplayed % 300)) {
		VaapiPrintFrames(decoder);
	    }
	    // wait for rendering finished
	    surface = decoder->SurfacesRb[decoder->SurfaceRead];
	    if (vaSyncSurface(decoder->VaDisplay, surface)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaSyncSurface failed\n"));
	    }
	}
    }
}

///
///	Display a video frame.
///
///	@todo FIXME: add detection of missed frames
///
static void VaapiDisplayFrame(void)
{
    struct timespec nowtime;
    uint32_t start;
    uint32_t put1;
    uint32_t put2;
    int i;
    VaapiDecoder *decoder;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	for (i = 0; i < VaapiDecoderN; ++i) {
	    VaapiInitSurfaceFlags(VaapiDecoders[i]);
	}
	VideoSurfaceModesChanged = 0;
    }
    // look if any stream have a new surface available
    for (i = 0; i < VaapiDecoderN; ++i) {
	VASurfaceID surface;
	int filled;

	decoder = VaapiDecoders[i];
	decoder->FramesDisplayed++;

	filled = atomic_read(&decoder->SurfacesFilled);
	// no surface availble show black with possible osd
	if (!filled) {
	    VaapiBlackSurface(decoder);
	    continue;
	}

	surface = decoder->SurfacesRb[decoder->SurfaceRead];
#ifdef DEBUG
	if (surface == VA_INVALID_ID) {
	    printf(_("video/vaapi: invalid surface in ringbuffer\n"));
	}
	Debug(4, "video/vaapi: yy video surface %#010x displayed\n", surface);
#endif

	start = GetMsTicks();

	// VDPAU driver + INTEL driver does no v-sync with 1080
	if (0 && decoder->Interlaced
	    // FIXME: buggy libva-driver-vdpau, buggy libva-driver-intel
	    && (VaapiBuggyVdpau || (0 && VaapiBuggyIntel
		    && decoder->InputHeight == 1080))
	    && VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, 0);
	    put1 = GetMsTicks();

	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, 1);
	    put2 = GetMsTicks();
	} else {
	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, decoder->SurfaceField);
	    put1 = GetMsTicks();
	    put2 = put1;
	}
	clock_gettime(CLOCK_REALTIME, &nowtime);
	if ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
	    * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		decoder->FrameTime.tv_nsec) > 30 * 1000 * 1000) {
	    Debug(3, "video/vaapi: time/frame too long %ld ms\n",
		((nowtime.tv_sec - decoder->FrameTime.tv_sec)
		    * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
			decoder->FrameTime.tv_nsec)) / (1000 * 1000));
	    Debug(4, "video/vaapi: put1 %2u put2 %2u\n", put1 - start,
		put2 - put1);
	}
#ifdef noDEBUG
	Debug(3, "video/vaapi: time/frame %ld ms\n",
	    ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
		* 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		    decoder->FrameTime.tv_nsec)) / (1000 * 1000));
	if (put2 > start + 20) {
	    Debug(3, "video/vaapi: putsurface too long %u ms\n", put2 - start);
	}
	Debug(4, "video/vaapi: put1 %2u put2 %2u\n", put1 - start,
	    put2 - put1);
#endif

	decoder->FrameTime = nowtime;
    }
}

///
///	Sync and display surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiSyncDisplayFrame(VaapiDecoder * decoder)
{
    int filled;
    int64_t audio_clock;
    int64_t video_clock;

    if (!decoder->DupNextFrame && (!Video60HzMode
	    || decoder->FramesDisplayed % 6)) {
	VaapiAdvanceFrame();
    }

    VaapiDisplayFrame();

    //
    //	audio/video sync
    //
    audio_clock = AudioGetClock();
    video_clock = VideoGetClock();
    filled = atomic_read(&decoder->SurfacesFilled);
    // FIXME: audio not known assume 333ms delay

    if (decoder->DupNextFrame) {
	decoder->DupNextFrame--;
    } else if ((uint64_t) audio_clock != AV_NOPTS_VALUE
	&& (uint64_t) video_clock != AV_NOPTS_VALUE) {
	// both clocks are known

	if (abs(video_clock - audio_clock) > 5000 * 90) {
	    Debug(3, "video: pts difference too big\n");
	} else if (video_clock > audio_clock + VideoAudioDelay + 80 * 90) {
	    Debug(3, "video: slow down video\n");
	    decoder->DupNextFrame += 2;
	} else if (video_clock > audio_clock + VideoAudioDelay + 40 * 90) {
	    Debug(3, "video: slow down video\n");
	    decoder->DupNextFrame++;
	} else if (audio_clock + VideoAudioDelay > video_clock + 40 * 90
	    && filled > 1) {
	    Debug(3, "video: speed up video\n");
	    decoder->DropNextFrame = 1;
	}
    }
#if defined(DEBUG) || defined(AV_INFO)
    // debug audio/video sync
    if (decoder->DupNextFrame || decoder->DropNextFrame
	|| !(decoder->FramesDisplayed % (50 * 10))) {
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d v-buf\n",
	    VideoTimeStampString(video_clock),
	    (video_clock - audio_clock) / 90, AudioGetDelay() / 90,
	    (int)VideoDeltaPTS / 90, atomic_read(&VideoPacketsFilled));
    }
#endif
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		VA-API decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VaapiSyncRenderFrame(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VideoSetPts(&decoder->PTS, decoder->Interlaced, frame);

    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %d\n", GetMsTicks() - VideoSwitch);
    }

    if (decoder->DropNextFrame) {	// drop frame requested
	++decoder->FramesDropped;
	Warning(_("video: dropping frame (%d/%d)\n"), decoder->FramesDropped,
	    decoder->FrameCounter);
	if (!(decoder->FramesDisplayed % 300)) {
	    VaapiPrintFrames(decoder);
	}
	decoder->DropNextFrame--;
	return;
    }
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX - 1) {
	struct timespec abstime;

	pthread_mutex_unlock(&VideoLockMutex);

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_mutex_lock(&VideoLockMutex);
	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/vaapi: pthread_cond_timedwait error\n");
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	VaapiSyncDisplayFrame(decoder);
    }

    VaapiRenderFrame(decoder, video_ctx, frame);
#ifdef USE_AUTOCROP
    VaapiCheckAutoCrop(decoder);
#endif
}

///
///	Get VA-API decoder video clock.
///
///	@param decoder	VA-API decoder
///
static int64_t VaapiGetClock(const VaapiDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (!decoder || (uint64_t) decoder->PTS == AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	return decoder->PTS -
	    20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField);
    }
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) +
	1);
}

///
///	Set VA-API video mode.
///
static void VaapiSetVideoMode(void)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiUpdateOutput(VaapiDecoders[i]);
    }
}

#ifdef USE_VIDEO_THREAD

///
///	Handle a va-api display.
///
///	@todo FIXME: only a single decoder supported.
///
static void VaapiDisplayHandlerThread(void)
{
    int err;
    int filled;
    struct timespec nowtime;
    VaapiDecoder *decoder;

    if (!(decoder = VaapiDecoders[0])) {	// no stream available
	return;
    }
    //
    // fill frame output ring buffer
    //
    filled = atomic_read(&decoder->SurfacesFilled);
    err = 1;
    if (filled < VIDEO_SURFACES_MAX - 1) {
	// FIXME: hot polling
	pthread_mutex_lock(&VideoLockMutex);
	// fetch+decode or reopen
	err = VideoDecode();
	pthread_mutex_unlock(&VideoLockMutex);
    }
    if (err) {
	// FIXME: sleep on wakeup
	usleep(5 * 1000);		// nothing buffered
    }

    clock_gettime(CLOCK_REALTIME, &nowtime);
    // time for one frame over?
    if ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
	* 1000 * 1000 * 1000 + (nowtime.tv_nsec - decoder->FrameTime.tv_nsec) <
	15 * 1000 * 1000) {
	return;
    }

    pthread_mutex_lock(&VideoLockMutex);
    VaapiSyncDisplayFrame(decoder);
    pthread_mutex_unlock(&VideoLockMutex);
}

#endif

//----------------------------------------------------------------------------
//	VA-API OSD
//----------------------------------------------------------------------------

///
///	Clear subpicture image.
///
///	@note looked by caller
///
static void VaapiOsdClear(void)
{
    void *image_buffer;

    // osd image available?
    if (VaOsdImage.image_id == VA_INVALID_ID) {
	return;
    }

    Debug(3, "video/vaapi: clear image\n");

    // map osd surface/image into memory.
    if (vaMapBuffer(VaDisplay, VaOsdImage.buf, &image_buffer)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map osd image buffer\n"));
	return;
    }
    // have dirty area.
    if (OsdDirtyWidth && OsdDirtyHeight) {
	int o;

	Debug(3, "video/vaapi: handle osd dirty area\n");
	for (o = 0; o < OsdDirtyHeight; ++o) {
	    memset(image_buffer + (OsdDirtyX + (o +
			OsdDirtyY) * VaOsdImage.width) * 4, 0,
		OsdDirtyWidth * 4);
	}
    } else {
	// 100% transparent
	memset(image_buffer, 0x00, VaOsdImage.data_size);
    }

    if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap osd image buffer\n"));
    }
}

///
///	Upload ARGB to subpicture image.
///
///	@param x	x position of image in osd
///	@param y	y position of image in osd
///	@param width	width of image
///	@param height	height of image
///	@param argb	argb image
///
///	@note looked by caller
///
static void VaapiOsdDrawARGB(int x, int y, int width, int height,
    const uint8_t * argb)
{
    uint32_t start;
    uint32_t end;
    void *image_buffer;
    int o;

    // osd image available?
    if (VaOsdImage.image_id == VA_INVALID_ID) {
	return;
    }

    start = GetMsTicks();
    // map osd surface/image into memory.
    if (vaMapBuffer(VaDisplay, VaOsdImage.buf, &image_buffer)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map osd image buffer\n"));
	return;
    }
    // FIXME: convert image from ARGB to subpicture format, if not argb

    // copy argb to image
    for (o = 0; o < height; ++o) {
	memcpy(image_buffer + (x + (y + o) * VaOsdImage.width) * 4,
	    argb + o * width * 4, width * 4);
    }

    if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap osd image buffer\n"));
    }
    end = GetMsTicks();

    Debug(3, "video/vaapi: osd upload %dx%d+%d+%d %d ms %d\n", width, height,
	x, y, end - start, width * height * 4);
}

///
///	VA-API initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
///	@note subpicture is unusable, it can be scaled with the video image.
///
static void VaapiOsdInit(int width, int height)
{
    VAImageFormat *formats;
    unsigned *flags;
    unsigned format_n;
    unsigned u;
    unsigned v;
    int i;
    static uint32_t wanted_formats[] =
	{ VA_FOURCC('B', 'G', 'R', 'A'), VA_FOURCC_RGBA };

    if (VaOsdImage.image_id != VA_INVALID_ID) {
	Debug(3, "video/vaapi: osd already setup\n");
	return;
    }
    if (!VaDisplay) {
	Debug(3, "video/vaapi: va-api not setup\n");
	return;
    }
    //
    //	look through subpicture formats
    //
    format_n = vaMaxNumSubpictureFormats(VaDisplay);
    formats = alloca(format_n * sizeof(*formats));
    flags = alloca(format_n * sizeof(*formats));
    if (vaQuerySubpictureFormats(VaDisplay, formats, flags,
	    &format_n) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get subpicture formats"));
	return;
    }
#ifdef DEBUG
    Debug(3, "video/vaapi: supported subpicture formats:\n");
    for (u = 0; u < format_n; ++u) {
	Debug(3, "video/vaapi:\t%c%c%c%c flags %#x %s\n", formats[u].fourcc,
	    formats[u].fourcc >> 8, formats[u].fourcc >> 16,
	    formats[u].fourcc >> 24, flags[u],
	    flags[u] & VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD ?
	    "screen coord" : "");
    }
#endif
    for (v = 0; v < sizeof(wanted_formats) / sizeof(*wanted_formats); ++v) {
	for (u = 0; u < format_n; ++u) {
	    if (formats[u].fourcc == wanted_formats[v]) {
		goto found;
	    }
	}
    }
    Error(_("video/vaapi: can't find a supported subpicture format"));
    return;

  found:
    Debug(3, "video/vaapi: use %c%c%c%c subpicture format with flags %#x\n",
	formats[u].fourcc, formats[u].fourcc >> 8, formats[u].fourcc >> 16,
	formats[u].fourcc >> 24, flags[u]);

    VaapiUnscaledOsd = 0;
    if (flags[u] & VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD) {
	Info(_("video/vaapi: vaapi supports unscaled osd\n"));
	VaapiUnscaledOsd = 1;
    }
    //VaapiUnscaledOsd = 0;
    //Info(_("video/vaapi: unscaled osd disabled\n"));

    if (vaCreateImage(VaDisplay, &formats[u], width, height,
	    &VaOsdImage) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create osd image\n"));
	return;
    }
    if (vaCreateSubpicture(VaDisplay, VaOsdImage.image_id,
	    &VaOsdSubpicture) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create subpicture\n"));

	if (vaDestroyImage(VaDisplay,
		VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	VaOsdImage.image_id = VA_INVALID_ID;

	return;
    }
    // FIXME: must store format, to convert ARGB to it.

    // restore osd association
    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i]->InputWidth && VaapiDecoders[i]->InputHeight) {
	    VaapiAssociate(VaapiDecoders[i], VaapiDecoders[i]->InputWidth,
		VaapiDecoders[i]->InputHeight);
	}
    }
}

///
///	VA-API cleanup osd.
///
static void VaapiOsdExit(void)
{
    if (VaOsdImage.image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay,
		VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	VaOsdImage.image_id = VA_INVALID_ID;
    }

    if (VaOsdSubpicture != VA_INVALID_ID) {
	int i;

	for (i = 0; i < VaapiDecoderN; ++i) {
	    VaapiDeassociate(VaapiDecoders[i]);
	}

	if (vaDestroySubpicture(VaDisplay, VaOsdSubpicture)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy subpicture\n"));
	}
	VaOsdSubpicture = VA_INVALID_ID;
    }
}

///
///	VA-API module.
///
static const VideoModule VaapiModule = {
    .Name = "va-api",
    .Enabled = 1,
    .NewHwDecoder = (VideoHwDecoder * (*const)(void))VaapiNewDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VaapiDelDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *))VaapiGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))VaapiReleaseSurface,
    .get_format = (enum PixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum PixelFormat *))Vaapi_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VaapiSyncRenderFrame,
    .GrabOutput = NULL,
    .SetVideoMode = VaapiSetVideoMode,
    .ResetAutoCrop = VaapiResetAutoCrop,
    .Thread = VaapiDisplayHandlerThread,
    .OsdClear = VaapiOsdClear,
    .OsdDrawARGB = VaapiOsdDrawARGB,
    .OsdInit = VaapiOsdInit,
    .OsdExit = VaapiOsdExit,
    .Init = VaapiInit,
    .Exit = VaapiExit,
};

#endif

//----------------------------------------------------------------------------
//	VDPAU
//----------------------------------------------------------------------------

#ifdef USE_VDPAU

///
///	VDPAU decoder
///
typedef struct _vdpau_decoder_
{
    VdpDevice Device;			///< VDPAU device

    xcb_window_t Window;		///< output window
    int OutputX;			///< output window x
    int OutputY;			///< output window y
    int OutputWidth;			///< output window width
    int OutputHeight;			///< output window height

    enum PixelFormat PixFmt;		///< ffmpeg frame pixfmt
    int WrongInterlacedWarned;		///< warning about interlace flag issued
    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height

#ifdef USE_AUTOCROP
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
#ifdef noyetUSE_GLX
    GLuint GlTexture[2];		///< gl texture for VDPAU
    void *GlxSurface[2];		///< VDPAU/GLX surface
#endif

    VdpDecoderProfile Profile;		///< vdp decoder profile
    VdpDecoder VideoDecoder;		///< vdp video decoder
    VdpVideoMixer VideoMixer;		///< vdp video mixer
    VdpChromaType ChromaType;		///< vdp video surface chroma format
    VdpProcamp Procamp;			///< vdp procamp parameterization data

    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used video surfaces
    /// used video surface ids
    VdpVideoSurface SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free video surfaces
    /// free video surface ids
    VdpVideoSurface SurfacesFree[CODEC_SURFACES_MAX];

    /// video surface ring buffer
    VdpVideoSurface SurfacesRb[VIDEO_SURFACES_MAX];
    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    int SurfaceField;			///< current displayed field
    int DropNextFrame;			///< flag drop next frame
    int DupNextFrame;			///< flag duplicate next frame
    struct timespec FrameTime;		///< time of last display
    int64_t PTS;			///< video PTS clock

    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
} VdpauDecoder;

static volatile char VdpauPreemption;	///< flag preemption happened.

static VdpauDecoder *VdpauDecoders[1];	///< open decoder streams
static int VdpauDecoderN;		///< number of decoder streams

static VdpDevice VdpauDevice;		///< VDPAU device
static VdpGetProcAddress *VdpauGetProcAddress;	///< entry point to use

    /// presentation queue target
static VdpPresentationQueueTarget VdpauQueueTarget;
static VdpPresentationQueue VdpauQueue;	///< presentation queue
static VdpColor VdpauBackgroundColor[1];	///< queue background color

static int VdpauHqScalingMax;		///< highest supported scaling level
static int VdpauTemporal;		///< temporal deinterlacer supported
static int VdpauTemporalSpatial;	///< temporal spatial deint. supported
static int VdpauInverseTelecine;	///< inverse telecine deint. supported
static int VdpauNoiseReduction;		///< noise reduction supported
static int VdpauSharpness;		///< sharpness supported
static int VdpauSkipChroma;		///< skip chroma deint. supported

    /// display surface ring buffer
static VdpOutputSurface VdpauSurfacesRb[OUTPUT_SURFACES_MAX];
static int VdpauSurfaceIndex;		///< current display surface

#ifdef USE_BITMAP
    /// bitmap surfaces for osd
static VdpBitmapSurface VdpauOsdBitmapSurface[2] = {
    VDP_INVALID_HANDLE, VDP_INVALID_HANDLE
};
#else
    /// output surfaces for osd
static VdpOutputSurface VdpauOsdOutputSurface[2] = {
    VDP_INVALID_HANDLE, VDP_INVALID_HANDLE
};
#endif
static int VdpauOsdSurfaceIndex;	///< index into double buffered osd

///
///	Function pointer of the VDPAU device.
///
///@{
static VdpGetErrorString *VdpauGetErrorString;
static VdpDeviceDestroy *VdpauDeviceDestroy;
static VdpGenerateCSCMatrix *VdpauGenerateCSCMatrix;
static VdpVideoSurfaceQueryCapabilities *VdpauVideoSurfaceQueryCapabilities;
static VdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities *
    VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities;
static VdpVideoSurfaceCreate *VdpauVideoSurfaceCreate;
static VdpVideoSurfaceDestroy *VdpauVideoSurfaceDestroy;
static VdpVideoSurfaceGetParameters *VdpauVideoSurfaceGetParameters;
static VdpVideoSurfaceGetBitsYCbCr *VdpauVideoSurfaceGetBitsYCbCr;
static VdpVideoSurfacePutBitsYCbCr *VdpauVideoSurfacePutBitsYCbCr;

static VdpOutputSurfaceQueryCapabilities *VdpauOutputSurfaceQueryCapabilities;

static VdpOutputSurfaceCreate *VdpauOutputSurfaceCreate;
static VdpOutputSurfaceDestroy *VdpauOutputSurfaceDestroy;
static VdpOutputSurfaceGetParameters *VdpauOutputSurfaceGetParameters;
static VdpOutputSurfaceGetBitsNative *VdpauOutputSurfaceGetBitsNative;
static VdpOutputSurfacePutBitsNative *VdpauOutputSurfacePutBitsNative;

static VdpBitmapSurfaceQueryCapabilities *VdpauBitmapSurfaceQueryCapabilities;
static VdpBitmapSurfaceCreate *VdpauBitmapSurfaceCreate;
static VdpBitmapSurfaceDestroy *VdpauBitmapSurfaceDestroy;

static VdpBitmapSurfacePutBitsNative *VdpauBitmapSurfacePutBitsNative;

static VdpOutputSurfaceRenderOutputSurface
    *VdpauOutputSurfaceRenderOutputSurface;
static VdpOutputSurfaceRenderBitmapSurface
    *VdpauOutputSurfaceRenderBitmapSurface;

static VdpDecoderQueryCapabilities *VdpauDecoderQueryCapabilities;
static VdpDecoderCreate *VdpauDecoderCreate;
static VdpDecoderDestroy *VdpauDecoderDestroy;

static VdpDecoderRender *VdpauDecoderRender;

static VdpVideoMixerQueryFeatureSupport *VdpauVideoMixerQueryFeatureSupport;
static VdpVideoMixerQueryAttributeSupport
    *VdpauVideoMixerQueryAttributeSupport;

static VdpVideoMixerCreate *VdpauVideoMixerCreate;
static VdpVideoMixerSetFeatureEnables *VdpauVideoMixerSetFeatureEnables;
static VdpVideoMixerSetAttributeValues *VdpauVideoMixerSetAttributeValues;

static VdpVideoMixerDestroy *VdpauVideoMixerDestroy;
static VdpVideoMixerRender *VdpauVideoMixerRender;
static VdpPresentationQueueTargetDestroy *VdpauPresentationQueueTargetDestroy;
static VdpPresentationQueueCreate *VdpauPresentationQueueCreate;
static VdpPresentationQueueDestroy *VdpauPresentationQueueDestroy;
static VdpPresentationQueueSetBackgroundColor *
    VdpauPresentationQueueSetBackgroundColor;

static VdpPresentationQueueGetTime *VdpauPresentationQueueGetTime;
static VdpPresentationQueueDisplay *VdpauPresentationQueueDisplay;
static VdpPresentationQueueBlockUntilSurfaceIdle
    *VdpauPresentationQueueBlockUntilSurfaceIdle;
static VdpPresentationQueueQuerySurfaceStatus
    *VdpauPresentationQueueQuerySurfaceStatus;
static VdpPreemptionCallbackRegister *VdpauPreemptionCallbackRegister;

static VdpPresentationQueueTargetCreateX11 *
    VdpauPresentationQueueTargetCreateX11;
///@}

static void VdpauOsdInit(int, int);	///< forward definition

//----------------------------------------------------------------------------

///
///	Create surfaces for VDPAU decoder.
///
///	@param decoder	VDPAU hw decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void VdpauCreateSurfaces(VdpauDecoder * decoder, int width, int height)
{
    int i;

#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
	Error(_("video/vaapi: surface needed not set\n"));
	decoder->SurfacesNeeded = 3 + VIDEO_SURFACES_MAX;
    }
#endif
    Debug(3, "video/vdpau: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	decoder->SurfacesNeeded);

    // allocate only the number of needed surfaces
    decoder->SurfaceFreeN = decoder->SurfacesNeeded;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	VdpStatus status;

	status =
	    VdpauVideoSurfaceCreate(decoder->Device, decoder->ChromaType,
	    width, height, decoder->SurfacesFree + i);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't create video surface: %s\n"),
		VdpauGetErrorString(status));
	    decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;
	    // FIXME: better error handling
	}
	Debug(4, "video/vdpau: created video surface %dx%d with id 0x%08x\n",
	    width, height, decoder->SurfacesFree[i]);
    }
}

///
///	Destroy surfaces of VDPAU decoder.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauDestroySurfaces(VdpauDecoder * decoder)
{
    int i;
    VdpStatus status;

    Debug(3, "video/vdpau: %s\n", __FUNCTION__);

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
#ifdef DEBUG
	if (decoder->SurfacesFree[i] == VDP_INVALID_HANDLE) {
	    Debug(3, "video/vdpau: invalid surface\n");
	}
#endif
	Debug(4, "video/vdpau: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesFree[i]);
	status = VdpauVideoSurfaceDestroy(decoder->SurfacesFree[i]);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video surface: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;
    }
    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
#ifdef DEBUG
	if (decoder->SurfacesUsed[i] == VDP_INVALID_HANDLE) {
	    Debug(3, "video/vdpau: invalid surface\n");
	}
#endif
	Debug(4, "video/vdpau: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesUsed[i]);
	status = VdpauVideoSurfaceDestroy(decoder->SurfacesUsed[i]);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video surface: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->SurfacesUsed[i] = VDP_INVALID_HANDLE;
    }
    decoder->SurfaceFreeN = 0;
    decoder->SurfaceUsedN = 0;
}

///
///	Get a free surface.
///
///	@param decoder	VDPAU hw decoder
///
///	@returns the oldest free surface
///
static unsigned VdpauGetSurface(VdpauDecoder * decoder)
{
    VdpVideoSurface surface;
    int i;

    if (!decoder->SurfaceFreeN) {
	Error(_("video/vdpau: out of surfaces\n"));

	return VDP_INVALID_HANDLE;
    }
    // use oldest surface
    surface = decoder->SurfacesFree[0];

    decoder->SurfaceFreeN--;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
    }
    decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;

    // save as used
    decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

    return surface;
}

///
///	Release a surface.
///
///	@param decoder	VDPAU hw decoder
///	@param surface	surface no longer used
///
static void VdpauReleaseSurface(VdpauDecoder * decoder, unsigned surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	if (decoder->SurfacesUsed[i] == surface) {
	    // no problem, with last used
	    decoder->SurfacesUsed[i] =
		decoder->SurfacesUsed[--decoder->SurfaceUsedN];
	    decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
	    return;
	}
    }
    Error(_("video/vdpau: release surface %#08x, which is not in use\n"),
	surface);
}

///
///	Debug VDPAU decoder frames drop...
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauPrintFrames(const VdpauDecoder * decoder)
{
    Debug(3, "video/vdpau: %d missed, %d duped, %d dropped frames of %d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter);
#ifndef DEBUG
    (void)decoder;
#endif
}

///
///	Create and setup VDPAU mixer.
///
///	@param decoder	VDPAU hw decoder
///
///	@note don't forget to update features, paramaters, attributes table
///	size, if more is add.
///
static void VdpauMixerSetup(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;
    VdpVideoMixerFeature features[15];
    VdpBool enables[15];
    int feature_n;
    VdpVideoMixerAttribute attributes[4];
    void const *attribute_value_ptrs[4];
    int attribute_n;
    uint8_t skip_chroma_value;
    float noise_reduction_level;
    float sharpness_level;
    VdpColorStandard color_standard;
    VdpCSCMatrix csc_matrix[1];

    //
    //	Build enables table
    //
    feature_n = 0;
    if (VdpauTemporal) {
	enables[feature_n] =
	    (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceTemporal
	    || VideoDeinterlace[decoder->Resolution] ==
	    VideoDeinterlaceTemporalSpatial) ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
	Debug(3, "video/vdpau: temporal deinterlace %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauTemporalSpatial) {
	enables[feature_n] =
	    VideoDeinterlace[decoder->Resolution] ==
	    VideoDeinterlaceTemporalSpatial ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] =
	    VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
	Debug(3, "video/vdpau: temporal spatial deinterlace %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauInverseTelecine) {
	enables[feature_n] =
	    VideoInverseTelecine[decoder->Resolution] ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
	Debug(3, "video/vdpau: inverse telecine %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauNoiseReduction) {
	enables[feature_n] =
	    VideoDenoise[decoder->Resolution] ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
	Debug(3, "video/vdpau: noise reduction %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauSharpness) {
	enables[feature_n] =
	    VideoSharpen[decoder->Resolution] ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
	Debug(3, "video/vdpau: sharpness %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    for (i = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1;
	i <= VdpauHqScalingMax; ++i) {
	enables[feature_n] =
	    VideoScaling[decoder->Resolution] ==
	    VideoScalingHQ ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = i;
	Debug(3, "video/vdpau: high quality scaling %d %s\n",
	    1 + i - VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1,
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    status =
	VdpauVideoMixerSetFeatureEnables(decoder->VideoMixer, feature_n,
	features, enables);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't set mixer feature enables: %s\n"),
	    VdpauGetErrorString(status));
    }
    //
    //	build attributes table
    //

    /*
       FIXME:
       VDP_VIDEO_MIXER_ATTRIBUTE_LUMA_KEY_MIN_LUMA
       VDP_VIDEO_MIXER_ATTRIBUTE_LUMA_KEY_MAX_LUMA
     */
    attribute_n = 0;
    if (VdpauSkipChroma) {
	skip_chroma_value = VideoSkipChromaDeinterlace[decoder->Resolution];
	attributes[attribute_n]
	    = VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE;
	attribute_value_ptrs[attribute_n++] = &skip_chroma_value;
	Debug(3, "video/vdpau: skip chroma deinterlace %s\n",
	    skip_chroma_value ? "enabled" : "disabled");
    }
    if (VdpauNoiseReduction) {
	noise_reduction_level = VideoDenoise[decoder->Resolution] / 1000.0;
	attributes[attribute_n]
	    = VDP_VIDEO_MIXER_ATTRIBUTE_NOISE_REDUCTION_LEVEL;
	attribute_value_ptrs[attribute_n++] = &noise_reduction_level;
	Debug(3, "video/vdpau: noise reduction level %1.3f\n",
	    noise_reduction_level);
    }
    if (VdpauSharpness) {
	sharpness_level = VideoSharpen[decoder->Resolution] / 1000.0;
	attributes[attribute_n]
	    = VDP_VIDEO_MIXER_ATTRIBUTE_SHARPNESS_LEVEL;
	attribute_value_ptrs[attribute_n++] = &sharpness_level;
	Debug(3, "video/vdpau: sharpness level %+1.3f\n", sharpness_level);
    }
    // FIXME: studio colors, VideoColorStandard[decoder->Resolution]
    if (decoder->InputWidth > 1280 || decoder->InputHeight > 576) {
	// HDTV
	color_standard = VDP_COLOR_STANDARD_ITUR_BT_709;
	Debug(3, "video/vdpau: color space ITU-R BT.709\n");
    } else {
	// SDTV
	color_standard = VDP_COLOR_STANDARD_ITUR_BT_601;
	Debug(3, "video/vdpau: color space ITU-R BT.601\n");
    }

    status =
	VdpauGenerateCSCMatrix(&decoder->Procamp, color_standard, csc_matrix);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't generate CSC matrix: %s\n"),
	    VdpauGetErrorString(status));
    }

    attributes[attribute_n] = VDP_VIDEO_MIXER_ATTRIBUTE_CSC_MATRIX;
    attribute_value_ptrs[attribute_n++] = csc_matrix;

    status =
	VdpauVideoMixerSetAttributeValues(decoder->VideoMixer, attribute_n,
	attributes, attribute_value_ptrs);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't set mixer attribute values: %s\n"),
	    VdpauGetErrorString(status));
    }
}

///
///	Create and setup VDPAU mixer.
///
///	@param decoder	VDPAU hw decoder
///
///	@note don't forget to update features, paramaters, attributes table
///	size, if more is add.
///
static void VdpauMixerCreate(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;
    VdpVideoMixerFeature features[15];
    int feature_n;
    VdpVideoMixerParameter paramaters[4];
    void const *value_ptrs[4];
    int parameter_n;
    VdpChromaType chroma_type;
    int layers;

    //
    //	Build feature table
    //
    feature_n = 0;
    if (VdpauTemporal) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
    }
    if (VdpauTemporalSpatial) {
	features[feature_n++] =
	    VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
    }
    if (VdpauInverseTelecine) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
    }
    if (VdpauNoiseReduction) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
    }
    if (VdpauSharpness) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
    }
    for (i = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1;
	i <= VdpauHqScalingMax; ++i) {
	features[feature_n++] = i;
    }

    decoder->ChromaType = chroma_type = VDP_CHROMA_TYPE_420;
    // FIXME: use best chroma

    //
    //	Setup parameter/value tables
    //
    paramaters[0] = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH;
    value_ptrs[0] = &decoder->InputWidth;
    paramaters[1] = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT;
    value_ptrs[1] = &decoder->InputHeight;
    paramaters[2] = VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE;
    value_ptrs[2] = &chroma_type;
    layers = 0;
    paramaters[3] = VDP_VIDEO_MIXER_PARAMETER_LAYERS;
    value_ptrs[3] = &layers;
    parameter_n = 4;

    status =
	VdpauVideoMixerCreate(VdpauDevice, feature_n, features, parameter_n,
	paramaters, value_ptrs, &decoder->VideoMixer);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: can't create video mixer: %s\n"),
	    VdpauGetErrorString(status));
	// FIXME: no fatal errors
    }

    VdpauMixerSetup(decoder);
}

///
///	Allocate new VDPAU decoder.
///
///	@returns a new prepared vdpau hardware decoder.
///
static VdpauDecoder *VdpauNewDecoder(void)
{
    VdpauDecoder *decoder;
    int i;

    if (VdpauDecoderN == 1) {
	Error(_("video/vdpau: out of decoders\n"));
	return NULL;
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Error(_("video/vdpau: out of memory\n"));
	return NULL;
    }
    decoder->Device = VdpauDevice;
    decoder->Window = VideoWindow;

    decoder->Profile = VDP_INVALID_HANDLE;
    decoder->VideoDecoder = VDP_INVALID_HANDLE;
    decoder->VideoMixer = VDP_INVALID_HANDLE;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
	decoder->SurfacesUsed[i] = VDP_INVALID_HANDLE;
	decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;
    }

    //
    // setup video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VDP_INVALID_HANDLE;
    }
    // we advance before display, to loose no surface, we set it before
    //decoder->SurfaceRead = VIDEO_SURFACES_MAX - 1;
    //decoder->SurfaceField = 1;

#ifdef DEBUG
    if (VIDEO_SURFACES_MAX < 1 + 1 + 1 + 1) {
	Error(_
	    ("video/vdpau: need 1 future, 1 current, 1 back and 1 work surface\n"));
    }
#endif

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    // Procamp operation parameterization data
    decoder->Procamp.struct_version = VDP_PROCAMP_VERSION;
    decoder->Procamp.brightness = 0.0;
    decoder->Procamp.contrast = 1.0;
    decoder->Procamp.saturation = 1.0;
    decoder->Procamp.hue = 0.0;		// default values

    // FIXME: hack
    VdpauDecoderN = 1;
    VdpauDecoders[0] = decoder;

    return decoder;
}

///
///	Cleanup VDPAU.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauCleanup(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;

    if (decoder->VideoDecoder != VDP_INVALID_HANDLE) {
	// hangs in lock
	status = VdpauDecoderDestroy(decoder->VideoDecoder);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video decoder: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->VideoDecoder = VDP_INVALID_HANDLE;
	decoder->Profile = VDP_INVALID_HANDLE;
    }

    if (decoder->VideoMixer != VDP_INVALID_HANDLE) {
	status = VdpauVideoMixerDestroy(decoder->VideoMixer);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video mixer: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->VideoMixer = VDP_INVALID_HANDLE;
    }

    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	VdpauDestroySurfaces(decoder);
    }
    //
    // reset video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VDP_INVALID_HANDLE;
    }
    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;

    decoder->SurfaceField = 0;

    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
}

///
///	Destroy a VDPAU decoder.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauDelDecoder(VdpauDecoder * decoder)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	if (VdpauDecoders[i] == decoder) {
	    VdpauDecoders[i] = NULL;
	    VdpauDecoderN--;
	    // FIXME: must copy last slot into empty slot and --
	    break;
	}
    }

    VdpauCleanup(decoder);

    VdpauPrintFrames(decoder);

    free(decoder);
}

///
///	Get the proc address.
///
///	@param id		VDP function id
///	@param[out] addr	address of VDP function
///	@param name		name of function for error message
///
static inline void VdpauGetProc(const VdpFuncId id, void *addr,
    const char *name)
{
    VdpStatus status;

    status = VdpauGetProcAddress(VdpauDevice, id, addr);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: Can't get function address of '%s': %s\n"), name,
	    VdpauGetErrorString(status));
	// FIXME: rewrite none fatal
    }
}

///
///	Initialize output queue.
///
static void VdpauInitOutputQueue(void)
{
    VdpStatus status;
    int i;

    status =
	VdpauPresentationQueueTargetCreateX11(VdpauDevice, VideoWindow,
	&VdpauQueueTarget);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create presentation queue target: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }

    status =
	VdpauPresentationQueueCreate(VdpauDevice, VdpauQueueTarget,
	&VdpauQueue);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create presentation queue: %s\n"),
	    VdpauGetErrorString(status));
	VdpauPresentationQueueTargetDestroy(VdpauQueueTarget);
	VdpauQueueTarget = 0;
	return;
    }

    VdpauBackgroundColor->red = 0.01;
    VdpauBackgroundColor->green = 0.02;
    VdpauBackgroundColor->blue = 0.03;
    VdpauBackgroundColor->alpha = 1.00;
    VdpauPresentationQueueSetBackgroundColor(VdpauQueue, VdpauBackgroundColor);

    //
    //	Create display output surfaces
    //
    for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	VdpRGBAFormat format;

	format = VDP_RGBA_FORMAT_B8G8R8A8;
	// FIXME: does a 10bit rgba produce a better output?
	// format = VDP_RGBA_FORMAT_R10G10B10A2;
	status =
	    VdpauOutputSurfaceCreate(VdpauDevice, format, VideoWindowWidth,
	    VideoWindowHeight, VdpauSurfacesRb + i);
	if (status != VDP_STATUS_OK) {
	    Fatal(_("video/vdpau: can't create output surface: %s\n"),
		VdpauGetErrorString(status));
	}
	Debug(3, "video/vdpau: created output surface %dx%d with id 0x%08x\n",
	    VideoWindowWidth, VideoWindowHeight, VdpauSurfacesRb[i]);
    }
}

///
///	Cleanup output queue.
///
static void VdpauExitOutputQueue(void)
{
    int i;

    //
    //	destroy display output surfaces
    //
    for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	VdpStatus status;

	Debug(4, "video/vdpau: destroy output surface with id 0x%08x\n",
	    VdpauSurfacesRb[i]);
	if (VdpauSurfacesRb[i] != VDP_INVALID_HANDLE) {
	    status = VdpauOutputSurfaceDestroy(VdpauSurfacesRb[i]);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't destroy output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauSurfacesRb[i] = VDP_INVALID_HANDLE;
	}
    }
    if (VdpauQueue) {
	VdpauPresentationQueueDestroy(VdpauQueue);
	VdpauQueue = 0;
    }
    if (VdpauQueueTarget) {
	VdpauPresentationQueueTargetDestroy(VdpauQueueTarget);
	VdpauQueueTarget = 0;
    }
}

///
///	Display preemption callback.
///
///	@param device	device that had its display preempted
///	@param context	client-supplied callback context
///
static void VdpauPreemptionCallback(VdpDevice device, __attribute__ ((unused))
    void *context)
{
    Debug(3, "video/vdpau: display preemption\n");
    if (device != VdpauDevice) {
	Error(_("video/vdpau preemption device not our device\n"));
	return;
    }
    VdpauPreemption = 1;		// set flag for video thread
}

///
///	VDPAU setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if VDPAU could be initialized, false otherwise.
///
static int VdpauInit(const char *display_name)
{
    VdpStatus status;
    VdpGetApiVersion *get_api_version;
    uint32_t api_version;
    VdpGetInformationString *get_information_string;
    const char *information_string;
    int i;
    VdpBool flag;
    uint32_t max_width;
    uint32_t max_height;

    status =
	vdp_device_create_x11(XlibDisplay, DefaultScreen(XlibDisplay),
	&VdpauDevice, &VdpauGetProcAddress);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: Can't create vdp device on display '%s'\n"),
	    display_name);
	return 0;
    }
    // get error function first, for better error messages
    status =
	VdpauGetProcAddress(VdpauDevice, VDP_FUNC_ID_GET_ERROR_STRING,
	(void **)&VdpauGetErrorString);
    if (status != VDP_STATUS_OK) {
	Error(_
	    ("video/vdpau: Can't get function address of 'GetErrorString'\n"));
	// FIXME: destroy_x11 VdpauDeviceDestroy
	return 0;
    }
    // get destroy device next, for cleaning up
    VdpauGetProc(VDP_FUNC_ID_DEVICE_DESTROY, &VdpauDeviceDestroy,
	"DeviceDestroy");

    // get version
    VdpauGetProc(VDP_FUNC_ID_GET_API_VERSION, &get_api_version,
	"GetApiVersion");
    VdpauGetProc(VDP_FUNC_ID_GET_INFORMATION_STRING, &get_information_string,
	"VdpauGetProc");
    status = get_api_version(&api_version);
    // FIXME: check status
    status = get_information_string(&information_string);
    // FIXME: check status

    Info(_("video/vdpau: VDPAU API version: %u\n"), api_version);
    Info(_("video/vdpau: VDPAU information: %s\n"), information_string);

    // FIXME: check if needed capabilities are available

    VdpauGetProc(VDP_FUNC_ID_GENERATE_CSC_MATRIX, &VdpauGenerateCSCMatrix,
	"GenerateCSCMatrix");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_QUERY_CAPABILITIES,
	&VdpauVideoSurfaceQueryCapabilities, "VideoSurfaceQueryCapabilities");
    VdpauGetProc
	(VDP_FUNC_ID_VIDEO_SURFACE_QUERY_GET_PUT_BITS_Y_CB_CR_CAPABILITIES,
	&VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities,
	"VideoSurfaceQueryGetPutBitsYCbCrCapabilities");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_CREATE, &VdpauVideoSurfaceCreate,
	"VideoSurfaceCreate");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_DESTROY, &VdpauVideoSurfaceDestroy,
	"VideoSurfaceDestroy");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_GET_PARAMETERS,
	&VdpauVideoSurfaceGetParameters, "VideoSurfaceGetParameters");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR,
	&VdpauVideoSurfaceGetBitsYCbCr, "VideoSurfaceGetBitsYCbCr");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR,
	&VdpauVideoSurfacePutBitsYCbCr, "VideoSurfacePutBitsYCbCr");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_CAPABILITIES,
	&VdpauOutputSurfaceQueryCapabilities,
	"OutputSurfaceQueryCapabilities");
#if 0
    VdpauGetProc
	(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_GET_PUT_BITS_NATIVE_CAPABILITIES, &,
	"");
    VdpauGetProc
	(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_PUT_BITS_INDEXED_CAPABILITIES, &,
	"");
    VdpauGetProc
	(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_PUT_BITS_Y_CB_CR_CAPABILITIES, &,
	"");
#endif
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_CREATE, &VdpauOutputSurfaceCreate,
	"OutputSurfaceCreate");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_DESTROY,
	&VdpauOutputSurfaceDestroy, "OutputSurfaceDestroy");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_GET_PARAMETERS,
	&VdpauOutputSurfaceGetParameters, "OutputSurfaceGetParameters");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_GET_BITS_NATIVE,
	&VdpauOutputSurfaceGetBitsNative, "OutputSurfaceGetBitsNative");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_NATIVE,
	&VdpauOutputSurfacePutBitsNative, "OutputSurfacePutBitsNative");
#if 0
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_INDEXED, &, "");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_Y_CB_CR, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_QUERY_CAPABILITIES,
	&VdpauBitmapSurfaceQueryCapabilities,
	"BitmapSurfaceQueryCapabilities");
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_CREATE, &VdpauBitmapSurfaceCreate,
	"BitmapSurfaceCreate");
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_DESTROY,
	&VdpauBitmapSurfaceDestroy, "BitmapSurfaceDestroy");
    // VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_GET_PARAMETERS, &VdpauBitmapSurfaceGetParameters, "BitmapSurfaceGetParameters");
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_PUT_BITS_NATIVE,
	&VdpauBitmapSurfacePutBitsNative, "BitmapSurfacePutBitsNative");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_OUTPUT_SURFACE,
	&VdpauOutputSurfaceRenderOutputSurface,
	"OutputSurfaceRenderOutputSurface");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_BITMAP_SURFACE,
	&VdpauOutputSurfaceRenderBitmapSurface,
	"OutputSurfaceRenderBitmapSurface");
#if 0
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_VIDEO_SURFACE_LUMA, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_DECODER_QUERY_CAPABILITIES,
	&VdpauDecoderQueryCapabilities, "DecoderQueryCapabilities");
    VdpauGetProc(VDP_FUNC_ID_DECODER_CREATE, &VdpauDecoderCreate,
	"DecoderCreate");
    VdpauGetProc(VDP_FUNC_ID_DECODER_DESTROY, &VdpauDecoderDestroy,
	"DecoderDestroy");
#if 0
    VdpauGetProc(VDP_FUNC_ID_DECODER_GET_PARAMETERS,
	&VdpauDecoderGetParameters, "DecoderGetParameters");
#endif
    VdpauGetProc(VDP_FUNC_ID_DECODER_RENDER, &VdpauDecoderRender,
	"DecoderRender");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_FEATURE_SUPPORT,
	&VdpauVideoMixerQueryFeatureSupport, "VideoMixerQueryFeatureSupport");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_SUPPORT, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_ATTRIBUTE_SUPPORT,
	&VdpauVideoMixerQueryAttributeSupport,
	"VideoMixerQueryAttributeSupport");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_VALUE_RANGE, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_ATTRIBUTE_VALUE_RANGE, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_CREATE, &VdpauVideoMixerCreate,
	"VideoMixerCreate");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_SET_FEATURE_ENABLES,
	&VdpauVideoMixerSetFeatureEnables, "VideoMixerSetFeatureEnables");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_SET_ATTRIBUTE_VALUES,
	&VdpauVideoMixerSetAttributeValues, "VideoMixerSetAttributeValues");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_FEATURE_SUPPORT, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_FEATURE_ENABLES, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_PARAMETER_VALUES, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_ATTRIBUTE_VALUES, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_DESTROY, &VdpauVideoMixerDestroy,
	"VideoMixerDestroy");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_RENDER, &VdpauVideoMixerRender,
	"VideoMixerRender");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_DESTROY,
	&VdpauPresentationQueueTargetDestroy,
	"PresentationQueueTargetDestroy");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_CREATE,
	&VdpauPresentationQueueCreate, "PresentationQueueCreate");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_DESTROY,
	&VdpauPresentationQueueDestroy, "PresentationQueueDestroy");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_SET_BACKGROUND_COLOR,
	&VdpauPresentationQueueSetBackgroundColor,
	"PresentationQueueSetBackgroundColor");
#if 0
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_GET_BACKGROUND_COLOR,
	&VdpauPresentationQueueGetBackgroundColor,
	"PresentationQueueGetBackgroundColor");
#endif
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_GET_TIME,
	&VdpauPresentationQueueGetTime, "PresentationQueueGetTime");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_DISPLAY,
	&VdpauPresentationQueueDisplay, "PresentationQueueDisplay");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE,
	&VdpauPresentationQueueBlockUntilSurfaceIdle,
	"PresentationQueueBlockUntilSurfaceIdle");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_QUERY_SURFACE_STATUS,
	&VdpauPresentationQueueQuerySurfaceStatus,
	"PresentationQueueQuerySurfaceStatus");
    VdpauGetProc(VDP_FUNC_ID_PREEMPTION_CALLBACK_REGISTER,
	&VdpauPreemptionCallbackRegister, "PreemptionCallbackRegister");

    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11,
	&VdpauPresentationQueueTargetCreateX11,
	"PresentationQueueTargetCreateX11");

    status =
	VdpauPreemptionCallbackRegister(VdpauDevice, VdpauPreemptionCallback,
	NULL);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't register preemption callback: %s\n"),
	    VdpauGetErrorString(status));
    }
    //
    //	Look which levels of high quality scaling are supported
    //
    for (i = 0; i < 9; ++i) {
	status =
	    VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	    VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + i, &flag);
	if (status != VDP_STATUS_OK) {
	    Warning(_("video/vdpau: can't query feature '%s': %s\n"),
		"high-quality-scaling", VdpauGetErrorString(status));
	    break;
	}
	if (!flag) {
	    break;
	}
	VdpauHqScalingMax =
	    VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + i;
    }

    //
    //	Cache some features
    //
    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "deinterlace-temporal", VdpauGetErrorString(status));
    } else {
	VdpauTemporal = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "deinterlace-temporal-spatial", VdpauGetErrorString(status));
    } else {
	VdpauTemporalSpatial = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "inverse-telecine", VdpauGetErrorString(status));
    } else {
	VdpauInverseTelecine = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "noise-reduction", VdpauGetErrorString(status));
    } else {
	VdpauNoiseReduction = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_SHARPNESS, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"), "sharpness",
	    VdpauGetErrorString(status));
    } else {
	VdpauSharpness = flag;
    }

    status =
	VdpauVideoMixerQueryAttributeSupport(VdpauDevice,
	VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "skip-chroma-deinterlace", VdpauGetErrorString(status));
    } else {
	VdpauSkipChroma = flag;
    }

    // VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR

    Info(_("video/vdpau: highest supported high quality scaling %d\n"),
	VdpauHqScalingMax - VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 +
	1);
    Info(_("video/vdpau: feature deinterlace temporal %s\n"),
	VdpauTemporal ? _("supported") : _("unsupported"));
    Info(_("video/vdpau: feature deinterlace temporal spatial %s\n"),
	VdpauTemporalSpatial ? _("supported") : _("unsupported"));
    Info(_("video/vdpau: attribute skip chroma deinterlace %s\n"),
	VdpauSkipChroma ? _("supported") : _("unsupported"));

    //
    //	video formats
    //
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_420,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query video surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:2:0 chroma format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_422,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query video surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:2:2 chroma format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_444,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query video surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:4:4 chroma format with %dx%d supported\n"),
	    max_width, max_height);
    }
    // FIXME: does only check for chroma formats, but no action
    status =
	VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities(VdpauDevice,
	VDP_CHROMA_TYPE_422, VDP_YCBCR_FORMAT_YUYV, &flag);
    if (status != VDP_STATUS_OK || !flag) {
	Error(_("video/vdpau: doesn't support yuvy video surface\n"));
    }
    status =
	VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities(VdpauDevice,
	VDP_CHROMA_TYPE_420, VDP_YCBCR_FORMAT_YV12, &flag);
    if (status != VDP_STATUS_OK || !flag) {
	Error(_("video/vdpau: doesn't support yv12 video surface\n"));
    }

    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_B8G8R8A8, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 8bit BGRA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_R8G8B8A8, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 8bit RGBA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_R10G10B10A2, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 10bit RGBA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_B10G10R10A2, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 8bit BRGA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    // FIXME: does only check for rgba formats, but no action

    //
    //	Create presentation queue, only one queue pro window
    //
    VdpauInitOutputQueue();

    return 1;
}

///
///	VDPAU cleanup.
///
static void VdpauExit(void)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	if (VdpauDecoders[i]) {
	    VdpauDelDecoder(VdpauDecoders[i]);
	    VdpauDecoders[i] = NULL;
	}
    }

    if (VdpauDevice) {
	VdpauExitOutputQueue();

	// FIXME: more VDPAU cleanups...

	if (VdpauDeviceDestroy) {
	    VdpauDeviceDestroy(VdpauDevice);
	}
	VdpauDevice = 0;
    }
}

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauUpdateOutput(VdpauDecoder * decoder)
{
    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, &decoder->OutputX, &decoder->OutputY,
	&decoder->OutputWidth, &decoder->OutputHeight, &decoder->CropX,
	&decoder->CropY, &decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

///
///	Configure VDPAU for new video format.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauSetupOutput(VdpauDecoder * decoder)
{
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t width;
    uint32_t height;

    // FIXME: need only to create and destroy surfaces for size changes
    //		or when number of needed surfaces changed!
    decoder->Resolution =
	VideoResolutionGroup(decoder->InputWidth, decoder->InputHeight,
	decoder->Interlaced);
    VdpauCreateSurfaces(decoder, decoder->InputWidth, decoder->InputHeight);

    VdpauMixerCreate(decoder);

    VdpauUpdateOutput(decoder);		// update aspect/scaling

    //	get real surface size
    status =
	VdpauVideoSurfaceGetParameters(decoder->SurfacesFree[0], &chroma_type,
	&width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }
    // vdpau can choose different sizes, must use them for putbits
    if (chroma_type != decoder->ChromaType
	|| width != (uint32_t) decoder->InputWidth
	|| height != (uint32_t) decoder->InputHeight) {
	// FIXME: must rewrite the code to support this case
	Fatal(_("video/vdpau: video surface type/size mismatch\n"));
    }
}

///
///	Check profile supported.
///
///	@param decoder		VDPAU hw decoder
///	@param profile		VDPAU profile requested
///
static VdpDecoderProfile VdpauCheckProfile(VdpauDecoder * decoder,
    VdpDecoderProfile profile)
{
    VdpStatus status;
    VdpBool is_supported;
    uint32_t max_level;
    uint32_t max_macroblocks;
    uint32_t max_width;
    uint32_t max_height;

    status =
	VdpauDecoderQueryCapabilities(decoder->Device, profile, &is_supported,
	&max_level, &max_macroblocks, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query decoder capabilities: %s\n"),
	    VdpauGetErrorString(status));
	return VDP_INVALID_HANDLE;
    }
    Debug(3,
	"video/vdpau: profile %d with level %d, macro blocks %d, width %d, height %d %ssupported\n",
	profile, max_level, max_macroblocks, max_width, max_height,
	is_supported ? "" : "not ");
    return is_supported ? profile : VDP_INVALID_HANDLE;
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
static enum PixelFormat Vdpau_get_format(VdpauDecoder * decoder,
    AVCodecContext * video_ctx, const enum PixelFormat *fmt)
{
    const enum PixelFormat *fmt_idx;
    VdpDecoderProfile profile;
    VdpStatus status;
    int max_refs;

    Debug(3, "video: new stream format %d\n", GetMsTicks() - VideoSwitch);
    VdpauCleanup(decoder);

    if (!VideoHardwareDecoder || (video_ctx->codec_id == CODEC_ID_MPEG2VIDEO
	    && VideoHardwareDecoder == 1)
	) {				// hardware disabled by config
	Debug(3, "codec: hardware acceleration disabled\n");
	goto slow_path;
    }
    //
    //	look through formats
    //
    Debug(3, "%s: codec %d fmts:\n", __FUNCTION__, video_ctx->codec_id);
    for (fmt_idx = fmt; *fmt_idx != PIX_FMT_NONE; fmt_idx++) {
	Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
	// check supported pixel format with entry point
	switch (*fmt_idx) {
	    case PIX_FMT_VDPAU_H264:
	    case PIX_FMT_VDPAU_MPEG1:
	    case PIX_FMT_VDPAU_MPEG2:
	    case PIX_FMT_VDPAU_WMV3:
	    case PIX_FMT_VDPAU_VC1:
	    case PIX_FMT_VDPAU_MPEG4:
		break;
	    default:
		continue;
	}
	break;
    }

    if (*fmt_idx == PIX_FMT_NONE) {
	Error(_("video/vdpau: no valid vdpau pixfmt found\n"));
	goto slow_path;
    }

    max_refs = CODEC_SURFACES_DEFAULT;
    // check profile
    switch (video_ctx->codec_id) {
	case CODEC_ID_MPEG1VIDEO:
	    max_refs = 2;
	    profile = VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_MPEG1);
	    break;
	case CODEC_ID_MPEG2VIDEO:
	    max_refs = 2;
	    profile =
		VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_MPEG2_MAIN);
	    break;
	case CODEC_ID_MPEG4:
	case CODEC_ID_H263:
	    /*
	       p = VaapiFindProfile(profiles, profile_n,
	       VAProfileMPEG4AdvancedSimple);
	     */
	    goto slow_path;
	case CODEC_ID_H264:
	    // FIXME: can calculate level 4.1 limits
	    max_refs = 16;
	    // try more simple formats, fallback to better
	    if (video_ctx->profile == FF_PROFILE_H264_BASELINE) {
		profile =
		    VdpauCheckProfile(decoder,
		    VDP_DECODER_PROFILE_H264_BASELINE);
		if (profile == VDP_INVALID_HANDLE) {
		    profile =
			VdpauCheckProfile(decoder,
			VDP_DECODER_PROFILE_H264_MAIN);
		}
		if (profile == VDP_INVALID_HANDLE) {
		    profile =
			VdpauCheckProfile(decoder,
			VDP_DECODER_PROFILE_H264_HIGH);
		}
	    } else if (video_ctx->profile == FF_PROFILE_H264_MAIN) {
		profile =
		    VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_H264_MAIN);
		if (profile == VDP_INVALID_HANDLE) {
		    profile =
			VdpauCheckProfile(decoder,
			VDP_DECODER_PROFILE_H264_HIGH);
		}
	    } else {
		profile =
		    VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_H264_MAIN);
	    }
	    break;
	case CODEC_ID_WMV3:
	    /*
	       p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Main);
	     */
	    goto slow_path;
	case CODEC_ID_VC1:
	    /*
	       p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Advanced);
	     */
	    goto slow_path;
	default:
	    goto slow_path;
    }

    if (profile == VDP_INVALID_HANDLE) {
	Error(_("video/vdpau: no valid profile found\n"));
	goto slow_path;
    }

    Debug(3, "video/vdpau: create decoder profile=%d %dx%d #%d refs\n",
	profile, video_ctx->width, video_ctx->height, max_refs);

    decoder->Profile = profile;
    decoder->SurfacesNeeded = max_refs + VIDEO_SURFACES_MAX;
    status =
	VdpauDecoderCreate(VdpauDevice, profile, video_ctx->width,
	video_ctx->height, max_refs, &decoder->VideoDecoder);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create decoder: %s\n"),
	    VdpauGetErrorString(status));
	goto slow_path;
    }
    // FIXME: combine this with VdpauSetupOutput and software decoder part
    decoder->CropX = 0;
    decoder->CropY = VideoSkipLines;
    decoder->CropWidth = video_ctx->width;
    decoder->CropHeight = video_ctx->height - VideoSkipLines * 2;

    decoder->PixFmt = *fmt_idx;
    decoder->InputWidth = video_ctx->width;
    decoder->InputHeight = video_ctx->height;
    decoder->InputAspect = video_ctx->sample_aspect_ratio;

    VdpauSetupOutput(decoder);

    Debug(3, "\t%#010x %s\n", fmt_idx[0], av_get_pix_fmt_name(fmt_idx[0]));
    return *fmt_idx;

  slow_path:
    // no accelerated format found
    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 2;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;
    video_ctx->hwaccel_context = NULL;
    return avcodec_default_get_format(video_ctx, fmt);
}

#ifdef USE_GRAB

///
///	Grab video surface.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauGrabVideoSurface(VdpauDecoder * decoder)
{
    VdpVideoSurface surface;
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    VdpYCbCrFormat format;

    // FIXME: test function to grab output surface content
    // for screen shots, atom light and auto crop.

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
	% VIDEO_SURFACES_MAX];

    //	get real surface size
    status =
	VdpauVideoSurfaceGetParameters(surface, &chroma_type, &width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }
    switch (chroma_type) {
	case VDP_CHROMA_TYPE_420:
	case VDP_CHROMA_TYPE_422:
	case VDP_CHROMA_TYPE_444:
	    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
		+ ((width + 1) / 2) * ((height + 1) / 2);
	    base = malloc(size);
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return;
	    }
	    pitches[0] = width;
	    pitches[1] = width / 2;
	    pitches[2] = width / 2;
	    data[0] = base;
	    data[1] = base + width * height;
	    data[2] = base + width * height + width * height / 4;
	    format = VDP_YCBCR_FORMAT_YV12;
	    break;
	default:
	    Error(_("video/vdpau: unsupported chroma type %d\n"), chroma_type);
	    return;
    }
    status = VdpauVideoSurfaceGetBitsYCbCr(surface, format, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }

    free(base);
}

///
///	Grab output surface.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *VdpauGrabOutputSurface(int *ret_size, int *ret_width,
    int *ret_height)
{
    VdpOutputSurface surface;
    VdpStatus status;
    VdpRGBAFormat rgba_format;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[1];
    uint32_t pitches[1];
    VdpRect source_rect;

    // FIXME: test function to grab output surface content

    surface = VdpauSurfacesRb[VdpauSurfaceIndex];

    //	get real surface size
    status =
	VdpauOutputSurfaceGetParameters(surface, &rgba_format, &width,
	&height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get output surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return NULL;
    }

    Debug(3, "video/vdpau: grab %dx%d format %d\n", width, height,
	rgba_format);

    // FIXME: scale surface to requested size with
    //		VdpauOutputSurfaceRenderOutputSurface

    switch (rgba_format) {
	case VDP_RGBA_FORMAT_B8G8R8A8:
	case VDP_RGBA_FORMAT_R8G8B8A8:
	    size = width * height * sizeof(uint32_t);
	    base = malloc(size);
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return NULL;
	    }
	    pitches[0] = width * sizeof(uint32_t);
	    data[0] = base;
	    break;
	case VDP_RGBA_FORMAT_R10G10B10A2:
	case VDP_RGBA_FORMAT_B10G10R10A2:
	case VDP_RGBA_FORMAT_A8:
	default:
	    Error(_("video/vdpau: unsupported rgba format %d\n"), rgba_format);
	    return NULL;
    }

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = source_rect.x0 + width;
    source_rect.y1 = source_rect.y0 + height;
    status =
	VdpauOutputSurfaceGetBitsNative(surface, &source_rect, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits native: %s\n"),
	    VdpauGetErrorString(status));
	free(base);
	return NULL;
    }

    if (ret_size) {
	*ret_size = size;
    }
    if (ret_width) {
	*ret_width = width;
    }
    if (ret_height) {
	*ret_height = height;
    }

    return base;
}

#endif

#ifdef USE_AUTOCROP

///
///	VDPAU auto-crop support.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauAutoCrop(VdpauDecoder * decoder)
{
    VdpVideoSurface surface;
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;
    VdpYCbCrFormat format;

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
	% VIDEO_SURFACES_MAX];

    //	get real surface size
    status =
	VdpauVideoSurfaceGetParameters(surface, &chroma_type, &width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }
    switch (chroma_type) {
	case VDP_CHROMA_TYPE_420:
	case VDP_CHROMA_TYPE_422:
	case VDP_CHROMA_TYPE_444:
	    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
		+ ((width + 1) / 2) * ((height + 1) / 2);
	    base = malloc(size);
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return;
	    }
	    pitches[0] = width;
	    pitches[1] = width / 2;
	    pitches[2] = width / 2;
	    data[0] = base;
	    data[1] = base + width * height;
	    data[2] = base + width * height + width * height / 4;
	    format = VDP_YCBCR_FORMAT_YV12;
	    break;
	default:
	    Error(_("video/vdpau: unsupported chroma type %d\n"), chroma_type);
	    return;
    }
    status = VdpauVideoSurfaceGetBitsYCbCr(surface, format, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);
    free(base);

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %d+%d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,
	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
	decoder->CropX = 0;
	decoder->CropY = (next_state == 16 ? crop16 : crop14) + VideoSkipLines;
	decoder->CropWidth = decoder->InputWidth;
	decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

	// FIXME: this overwrites user choosen output position
	// FIXME: resize kills the auto crop values
	// FIXME: support other 4:3 zoom modes
	decoder->OutputX = 0;
	decoder->OutputY = 0;
	decoder->OutputWidth = (VideoWindowHeight * next_state) / 9;
	decoder->OutputHeight = (VideoWindowWidth * 9) / next_state;
	if ((unsigned)decoder->OutputWidth > VideoWindowWidth) {
	    decoder->OutputWidth = VideoWindowWidth;
	    decoder->OutputY = (VideoWindowHeight - decoder->OutputHeight) / 2;
	} else if ((unsigned)decoder->OutputHeight > VideoWindowHeight) {
	    decoder->OutputHeight = VideoWindowHeight;
	    decoder->OutputX = (VideoWindowWidth - decoder->OutputWidth) / 2;
	}
	Debug(3, "video: aspect output %dx%d %dx%d+%d+%d\n",
	    decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
	    decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
	decoder->CropX = 0;
	decoder->CropY = VideoSkipLines;
	decoder->CropWidth = decoder->InputWidth;
	decoder->CropHeight = decoder->InputHeight - VideoSkipLines * 2;

	// sets AutoCrop->Count
	VdpauUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;
}

///
///	VDPAU check if auto-crop todo.
///
///	@param decoder	VDPAU hw decoder
///
///	@note a copy of VaapiCheckAutoCrop
///
static void VdpauCheckAutoCrop(VdpauDecoder * decoder)
{
    // reduce load, check only n frames
    if (AutoCropInterval && !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational display_aspect_ratio;

	av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	// only 4:3 with 16:9/14:9 inside supported
	if (display_aspect_ratio.num == 4 && display_aspect_ratio.den == 3) {
	    VdpauAutoCrop(decoder);
	} else {
	    decoder->AutoCrop->Count = 0;
	    decoder->AutoCrop->State = 0;
	}
    }
}

///
///	VDPAU reset auto-crop.
///
static void VdpauResetAutoCrop(void)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	VdpauDecoders[i]->AutoCrop->State = 0;
	VdpauDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif

///
///	Queue output surface.
///
///	@param decoder	VDPAU hw decoder
///	@param surface	output surface
///	@param softdec	software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void VdpauQueueSurface(VdpauDecoder * decoder, VdpVideoSurface surface,
    int softdec)
{
    VdpVideoSurface old;

    ++decoder->FrameCounter;

    if (1) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    Warning(_
		("video/vdpau: output buffer full, dropping frame (%d/%d)\n"),
		++decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
		VdpauPrintFrames(decoder);
	    }
	    // software surfaces only
	    if (softdec) {
		VdpauReleaseSurface(decoder, surface);
	    }
	    return;
	}
#if 0
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    VideoDisplayHandler();
	}
#endif
    }

    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])
	!= VDP_INVALID_HANDLE) {

	// now we can release the surface, software surfaces only
	if (softdec) {
	    VdpauReleaseSurface(decoder, old);
	}
    }

    Debug(4, "video/vdpau: yy video surface %#08x@%d ready\n", surface,
	decoder->SurfaceWrite);

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)
	% VIDEO_SURFACES_MAX;
    atomic_inc(&decoder->SurfacesFilled);
}

///
///	Render a ffmpeg frame.
///
///	@param decoder		VDPAU hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VdpauRenderFrame(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VdpStatus status;
    VdpVideoSurface surface;
    int interlaced;

    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
    interlaced = frame->interlaced_frame;
    if (video_ctx->height == 720) {
	if (interlaced && !decoder->WrongInterlacedWarned) {
	    Debug(3, "video/vdpau: wrong interlace flag fixed\n");
	    decoder->WrongInterlacedWarned = 1;
	}
	interlaced = 0;
    } else {
	if (!interlaced && !decoder->WrongInterlacedWarned) {
	    Debug(3, "video/vdpau: wrong interlace flag fixed\n");
	    decoder->WrongInterlacedWarned = 1;
	}
	interlaced = 1;
    }

    // FIXME: should be done by init video_ctx->field_order
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != frame->top_field_first) {

	Debug(3, "video/vdpau: interlaced %d top-field-first %d\n", interlaced,
	    frame->top_field_first);

	decoder->Interlaced = interlaced;
	decoder->TopFieldFirst = frame->top_field_first;
	decoder->SurfaceField = 0;
    }
    // update aspect ratio changes
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53,60,100)
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, frame->sample_aspect_ratio)) {
	Debug(3, "video/vdpau: aspect ratio changed\n");

	decoder->InputAspect = frame->sample_aspect_ratio;
	VdpauUpdateOutput(decoder);
    }
#else
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, frame->sample_aspect_ratio)) {
	Debug(3, "video/vdpau: aspect ratio changed\n");

	decoder->InputAspect = video_ctx->sample_aspect_ratio;
	VdpauUpdateOutput(decoder);
    }
#endif

    //
    // Hardware render
    //
    // VDPAU: PIX_FMT_VDPAU_H264 .. PIX_FMT_VDPAU_VC1 PIX_FMT_VDPAU_MPEG4
    if ((PIX_FMT_VDPAU_H264 <= video_ctx->pix_fmt
	    && video_ctx->pix_fmt <= PIX_FMT_VDPAU_VC1)
	|| video_ctx->pix_fmt == PIX_FMT_VDPAU_MPEG4) {
	struct vdpau_render_state *vrs;

	vrs = (struct vdpau_render_state *)frame->data[0];
	surface = vrs->surface;
	Debug(4, "video/vdpau: hw render hw surface %#08x\n", surface);

	if (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceSoftware
	    && interlaced) {
	    // FIXME: software deinterlace avpicture_deinterlace
	    // FIXME: VdpauCpuDeinterlace(decoder, surface);
	} else {
	    VdpauQueueSurface(decoder, surface, 0);
	}

	//
	// PutBitsYCbCr render
	//
    } else {
	void const *data[3];
	uint32_t pitches[3];

	//
	//	Check image, format, size
	//
	if (decoder->PixFmt != video_ctx->pix_fmt
	    || video_ctx->width != decoder->InputWidth
	    || video_ctx->height != decoder->InputHeight) {

	    decoder->CropX = 0;
	    decoder->CropY = VideoSkipLines;
	    decoder->CropWidth = video_ctx->width;
	    decoder->CropHeight = video_ctx->height - VideoSkipLines * 2;

	    decoder->PixFmt = video_ctx->pix_fmt;
	    decoder->InputWidth = video_ctx->width;
	    decoder->InputHeight = video_ctx->height;

	    //
	    //	detect interlaced input
	    //
	    Debug(3, "video/vdpau: interlaced %d top-field-first %d\n",
		frame->interlaced_frame, frame->top_field_first);
	    // FIXME: I hope this didn't change in the middle of the stream

	    VdpauCleanup(decoder);
	    VdpauSetupOutput(decoder);
	}
	//
	//	Copy data from frame to image
	//
	switch (video_ctx->pix_fmt) {
	    case PIX_FMT_YUV420P:
		break;
	    case PIX_FMT_YUV422P:
	    case PIX_FMT_YUV444P:
	    default:
		Fatal(_("video/vdpau: pixel format %d not supported\n"),
		    video_ctx->pix_fmt);
	}

	// convert ffmpeg order to vdpau
	data[0] = frame->data[0];
	data[1] = frame->data[2];
	data[2] = frame->data[1];
	pitches[0] = frame->linesize[0];
	pitches[1] = frame->linesize[2];
	pitches[2] = frame->linesize[1];

	surface = VdpauGetSurface(decoder);
	status =
	    VdpauVideoSurfacePutBitsYCbCr(surface, VDP_YCBCR_FORMAT_YV12, data,
	    pitches);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't put video surface bits: %s\n"),
		VdpauGetErrorString(status));
	}

	VdpauQueueSurface(decoder, surface, 1);
    }

    if (frame->interlaced_frame) {
	++decoder->FrameCounter;
    }
}

///
///	Render osd surface to output surface.
///
static void VdpauMixOsd(void)
{
    VdpOutputSurfaceRenderBlendState blend_state;
    VdpRect source_rect;
    VdpRect output_rect;
    VdpStatus status;

    //uint32_t start;
    //uint32_t end;

    //
    //	blend overlay over output
    //
    blend_state.struct_version = VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION;
    blend_state.blend_factor_source_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_SRC_ALPHA;
    blend_state.blend_factor_source_alpha =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE;
    blend_state.blend_factor_destination_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_state.blend_factor_destination_alpha =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_state.blend_equation_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD;
    blend_state.blend_equation_alpha =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD;

    // use dirty area
    if (OsdDirtyWidth && OsdDirtyHeight) {
	source_rect.x0 = OsdDirtyX;
	source_rect.y0 = OsdDirtyY;
	source_rect.x1 = source_rect.x0 + OsdDirtyWidth;
	source_rect.y1 = source_rect.y0 + OsdDirtyHeight;

	output_rect.x0 = (OsdDirtyX * VideoWindowWidth) / OsdWidth;
	output_rect.y0 = (OsdDirtyY * VideoWindowHeight) / OsdHeight;
	output_rect.x1 =
	    output_rect.x0 + (OsdDirtyWidth * VideoWindowWidth) / OsdWidth;
	output_rect.y1 =
	    output_rect.y0 + (OsdDirtyHeight * VideoWindowHeight) / OsdHeight;
    } else {
	source_rect.x0 = 0;
	source_rect.y0 = 0;
	source_rect.x1 = OsdWidth;
	source_rect.y1 = OsdHeight;

	output_rect.x0 = 0;
	output_rect.y0 = 0;
	output_rect.x1 = VideoWindowWidth;
	output_rect.y1 = VideoWindowHeight;
    }

    //start = GetMsTicks();

    // FIXME: double buffered osd disabled
    VdpauOsdSurfaceIndex = 1;
#ifdef USE_BITMAP
    status =
	VdpauOutputSurfaceRenderBitmapSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdBitmapSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	VideoTransparentOsd ? &blend_state : NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render bitmap surface: %s\n"),
	    VdpauGetErrorString(status));
    }
#else
    status =
	VdpauOutputSurfaceRenderOutputSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdOutputSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	VideoTransparentOsd ? &blend_state : NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
#endif
    //end = GetMsTicks();
    /*
       Debug(4, "video:/vdpau: osd render %d %d ms\n", VdpauOsdSurfaceIndex,
       end - start);
     */

    VdpauOsdSurfaceIndex = !VdpauOsdSurfaceIndex;
}

///
///	Render video surface to output surface.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauMixVideo(VdpauDecoder * decoder)
{
    VdpVideoSurface current;
    VdpRect video_src_rect;
    VdpRect dst_rect;
    VdpRect dst_video_rect;
    VdpStatus status;

#ifdef USE_AUTOCROP
    // FIXME: can move to render frame
    VdpauCheckAutoCrop(decoder);
#endif

    dst_rect.x0 = 0;			// window output (clip)
    dst_rect.y0 = 0;
    dst_rect.x1 = VideoWindowWidth;
    dst_rect.y1 = VideoWindowHeight;

    video_src_rect.x0 = decoder->CropX;	// video source (crop)
    video_src_rect.y0 = decoder->CropY;
    video_src_rect.x1 = decoder->CropX + decoder->CropWidth;
    video_src_rect.y1 = decoder->CropY + decoder->CropHeight;

    dst_video_rect.x0 = decoder->OutputX;	// video output (scale)
    dst_video_rect.y0 = decoder->OutputY;
    dst_video_rect.x1 = decoder->OutputX + decoder->OutputWidth;
    dst_video_rect.y1 = decoder->OutputY + decoder->OutputHeight;

    if (decoder->Interlaced
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	//
	//	Build deinterlace structures
	//
	VdpVideoMixerPictureStructure cps;
	VdpVideoSurface past[3];
	int past_n;
	VdpVideoSurface future[3];
	int future_n;

#ifdef DEBUG
	if (atomic_read(&decoder->SurfacesFilled) < 3) {
	    Debug(3, "only %d\n", atomic_read(&decoder->SurfacesFilled));
	}
#endif
	// FIXME: can use VDP_INVALID_HANDLE to support less surface on start

	if (VideoDeinterlaceSurfaces == 5) {
	    past_n = 2;
	    future_n = 2;

	    // FIXME: wrong for bottom-field first
	    // read: past: B0 T0 current T1 future B1 T2 (0 1 2)
	    // read: past: T1 B0 current B1 future T2 B2 (0 1 2)
	    if (decoder->TopFieldFirst != decoder->SurfaceField) {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;

		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = past[1];
		current = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		future[0] = current;
		future[1] = decoder->SurfacesRb[(decoder->SurfaceRead + 2)
		    % VIDEO_SURFACES_MAX];
		// FIXME: can support 1 future more
	    } else {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;

		// FIXME: can support 1 past more
		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		current = past[0];
		future[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 2)
		    % VIDEO_SURFACES_MAX];
		future[1] = future[0];
	    }

	} else if (VideoDeinterlaceSurfaces == 4) {
	    past_n = 2;
	    future_n = 1;

	    // FIXME: wrong for bottom-field first
	    // read: past: B0 T0 current T1 future B1 (0 1 2)
	    // read: past: T1 B0 current B1 future T2 (0 1 2)
	    if (decoder->TopFieldFirst != decoder->SurfaceField) {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;

		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = past[1];
		current = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		future[0] = current;
	    } else {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;

		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		current = past[0];
		future[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 2)
		    % VIDEO_SURFACES_MAX];
	    }

	} else {
	    Error(_("video/vdpau: %d surface deinterlace unsupported\n"),
		VideoDeinterlaceSurfaces);
	}

	// FIXME: past_n, future_n here:
	Debug(4, " %02d	 %02d(%c%02d) %02d  %02d\n", past[1], past[0],
	    cps == VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD ? 'T' : 'B',
	    current, future[0], future[1]);

	status =
	    VdpauVideoMixerRender(decoder->VideoMixer, VDP_INVALID_HANDLE,
	    NULL, cps, past_n, past, current, future_n, future,
	    &video_src_rect, VdpauSurfacesRb[VdpauSurfaceIndex], &dst_rect,
	    &dst_video_rect, 0, NULL);
    } else {
	current = decoder->SurfacesRb[decoder->SurfaceRead];

	status =
	    VdpauVideoMixerRender(decoder->VideoMixer, VDP_INVALID_HANDLE,
	    NULL, VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME, 0, NULL, current, 0,
	    NULL, &video_src_rect, VdpauSurfacesRb[VdpauSurfaceIndex],
	    &dst_rect, &dst_video_rect, 0, NULL);
    }
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render mixer: %s\n"),
	    VdpauGetErrorString(status));
    }

    Debug(4, "video/vdpau: yy video surface %#08x@%d displayed\n", current,
	decoder->SurfaceRead);
}

///
///	Create and display a black empty surface.
///
///	@param decoder	VDPAU hw decoder
///
///	@FIXME: render only video area, not fullscreen!
///
static void VdpauBlackSurface(VdpauDecoder * decoder)
{
    VdpStatus status;
    void *image;
    void const *data[1];
    uint32_t pitches[1];
    VdpRect dst_rect;

    Debug(3, "video/vdpau: black surface\n");

    // FIXME: clear video window area
    (void)decoder;

    image = calloc(4, VideoWindowWidth * VideoWindowHeight);

    dst_rect.x0 = 0;
    dst_rect.y0 = 0;
    dst_rect.x1 = dst_rect.x0 + VideoWindowWidth;
    dst_rect.y1 = dst_rect.y0 + VideoWindowHeight;
    data[0] = image;
    pitches[0] = VideoWindowWidth * 4;

    status =
	VdpauOutputSurfacePutBitsNative(VdpauSurfacesRb[VdpauSurfaceIndex],
	data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: output surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }

    free(image);
}

///
///	Advance displayed frame.
///
static void VdpauAdvanceFrame(void)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	int filled;
	VdpauDecoder *decoder;

	decoder = VdpauDecoders[i];

	// next field
	if (decoder->Interlaced) {
	    decoder->SurfaceField ^= 1;
	}
	// next surface, if complete frame is displayed
	if (!decoder->SurfaceField) {
	    // check decoder, if new surface is available
	    // need 2 frames for progressive
	    // need 4 frames for interlaced
	    filled = atomic_read(&decoder->SurfacesFilled);
	    if (filled <= 1 + 2 * decoder->Interlaced) {
		// keep use of last surface
		++decoder->FramesDuped;
		decoder->DropNextFrame = 0;
		// FIXME: don't warn after stream start
		Warning(_
		    ("video: display buffer empty, duping frame (%d/%d) %d\n"),
		    decoder->FramesDuped, decoder->FrameCounter,
		    atomic_read(&VideoPacketsFilled));
		if (!(decoder->FramesDisplayed % 300)) {
		    VdpauPrintFrames(decoder);
		}
		decoder->SurfaceField = decoder->Interlaced;
	    } else {
		decoder->SurfaceRead = (decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX;
		atomic_dec(&decoder->SurfacesFilled);
	    }
	}
    }
}

///
///	Display a video frame.
///
static void VdpauDisplayFrame(void)
{
    VdpStatus status;
    VdpTime first_time;
    static VdpTime last_time;
    int i;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	for (i = 0; i < VdpauDecoderN; ++i) {
	    if (VdpauDecoders[i]->VideoMixer != VDP_INVALID_HANDLE) {
		VdpauMixerSetup(VdpauDecoders[i]);
	    }
	}
	VideoSurfaceModesChanged = 0;
    }
    //
    //	wait for surface visible (blocks max ~5ms)
    //
    status =
	VdpauPresentationQueueBlockUntilSurfaceIdle(VdpauQueue,
	VdpauSurfacesRb[VdpauSurfaceIndex], &first_time);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't block queue: %s\n"),
	    VdpauGetErrorString(status));
    }
    // check if surface was displayed for more than 1 frame
    if (last_time && first_time > last_time + 21 * 1000 * 1000) {
	Debug(3, "video/vdpau: %ld display time %ld\n", first_time / 1000,
	    (first_time - last_time) / 1000);
	// FIXME: can be more than 1 frame long shown
	for (i = 0; i < VdpauDecoderN; ++i) {
	    VdpauDecoders[i]->FramesMissed++;
	    Warning(_("video: missed frame (%d/%d)\n"),
		VdpauDecoders[i]->FramesMissed,
		VdpauDecoders[i]->FrameCounter);
	    if (!(VdpauDecoders[i]->FramesDisplayed % 300)) {
		VdpauPrintFrames(VdpauDecoders[i]);
	    }
	}
    }
    last_time = first_time;

    //
    //	Render videos into output
    //
    for (i = 0; i < VdpauDecoderN; ++i) {
	int filled;
	VdpauDecoder *decoder;

	decoder = VdpauDecoders[i];
	decoder->FramesDisplayed++;

	filled = atomic_read(&decoder->SurfacesFilled);
	// need 1 frame for progressive, 3 frames for interlaced
	if (filled < 1 + 2 * decoder->Interlaced) {
	    // FIXME: rewrite MixVideo to support less surfaces
	    VdpauBlackSurface(decoder);
	    continue;
	}

	VdpauMixVideo(decoder);
    }

    //
    //	add osd to surface
    //
    if (OsdShown) {			// showing costs performance
	VdpauMixOsd();
    }
    //
    //	place surface in presentation queue
    //
    status =
	VdpauPresentationQueueDisplay(VdpauQueue,
	VdpauSurfacesRb[VdpauSurfaceIndex], 0, 0, 0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't queue display: %s\n"),
	    VdpauGetErrorString(status));
    }

    for (i = 0; i < VdpauDecoderN; ++i) {
	clock_gettime(CLOCK_REALTIME, &VdpauDecoders[i]->FrameTime);
    }

    VdpauSurfaceIndex = (VdpauSurfaceIndex + 1) % OUTPUT_SURFACES_MAX;

    xcb_flush(Connection);
}

///
///	Sync and display surface.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauSyncDisplayFrame(VdpauDecoder * decoder)
{
    int filled;
    int64_t audio_clock;
    int64_t video_clock;

    if (!decoder->DupNextFrame && (!Video60HzMode
	    || decoder->FramesDisplayed % 6)) {
	VdpauAdvanceFrame();
    }

    VdpauDisplayFrame();

    //
    //	audio/video sync
    //
    audio_clock = AudioGetClock();
    video_clock = VideoGetClock();
    filled = atomic_read(&decoder->SurfacesFilled);
    // FIXME: audio not known assume 333ms delay

    if (decoder->DupNextFrame) {
	decoder->DupNextFrame--;
    } else if ((uint64_t) audio_clock != AV_NOPTS_VALUE
	&& (uint64_t) video_clock != AV_NOPTS_VALUE) {
	// both clocks are known

	if (abs(video_clock - audio_clock) > 5000 * 90) {
	    Debug(3, "video: pts difference too big\n");
	} else if (video_clock > audio_clock + VideoAudioDelay + 80 * 90) {
	    Debug(3, "video: slow down video\n");
	    decoder->DupNextFrame += 2;
	} else if (video_clock > audio_clock + VideoAudioDelay + 40 * 90) {
	    Debug(3, "video: slow down video\n");
	    decoder->DupNextFrame++;
	} else if (audio_clock + VideoAudioDelay > video_clock + 40 * 90
	    && filled > 1 + 2 * decoder->Interlaced) {
	    Debug(3, "video: speed up video\n");
	    decoder->DropNextFrame = 1;
	}
    }
#if defined(DEBUG) || defined(AV_INFO)
    // debug audio/video sync
    if (decoder->DupNextFrame || decoder->DropNextFrame
	|| !(decoder->FramesDisplayed % (50 * 10))) {
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d v-buf\n",
	    VideoTimeStampString(video_clock),
	    (video_clock - audio_clock) / 90, AudioGetDelay() / 90,
	    (int)VideoDeltaPTS / 90, atomic_read(&VideoPacketsFilled));
    }
#endif
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		VDPAU hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VdpauSyncRenderFrame(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VideoSetPts(&decoder->PTS, decoder->Interlaced, frame);

    if (VdpauPreemption) {		// display preempted
	return;
    }
#ifdef DEBUG
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %d\n", GetMsTicks() - VideoSwitch);
    }
#endif

    if (decoder->DropNextFrame) {	// drop frame requested
	++decoder->FramesDropped;
	Warning(_("video: dropping frame (%d/%d)\n"), decoder->FramesDropped,
	    decoder->FrameCounter);
	if (!(decoder->FramesDisplayed % 300)) {
	    VdpauPrintFrames(decoder);
	}
	decoder->DropNextFrame--;
	return;
    }
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	struct timespec abstime;

	pthread_mutex_unlock(&VideoLockMutex);

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	// fix dead-lock with VdpauExit
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_mutex_lock(&VideoLockMutex);
	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/vdpau: pthread_cond_timedwait error\n");
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	if (VdpauPreemption) {		// display become preempted
	    return;
	}
	VdpauSyncDisplayFrame(decoder);
    }

    VdpauRenderFrame(decoder, video_ctx, frame);
}

///
///	Get VDPAU decoder video clock.
///
///	@param decoder	VDPAU hw decoder
///
static int64_t VdpauGetClock(const VdpauDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (!decoder || (uint64_t) decoder->PTS == AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	return decoder->PTS -
	    20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField);
    }
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) +
	1);
}

///
///	Recover from preemption.
///
static int VdpauPreemptionRecover(void)
{
    VdpStatus status;
    int i;

    VdpauPreemption = 0;

    Debug(3, "video/vdpau: display preempted\n");

    status =
	vdp_device_create_x11(XlibDisplay, DefaultScreen(XlibDisplay),
	&VdpauDevice, &VdpauGetProcAddress);
    if (status != VDP_STATUS_OK) {
	VdpauPreemption = 1;
	return -1;
    }

    VdpauInitOutputQueue();

    // mixer
    for (i = 0; i < VdpauDecoderN; ++i) {
	VdpauDecoders[i]->VideoDecoder = VDP_INVALID_HANDLE;
	VdpauDecoders[i]->VideoMixer = VDP_INVALID_HANDLE;
	VdpauDecoders[i]->SurfaceFreeN = 0;
	VdpauDecoders[i]->SurfaceUsedN = 0;
    }

    // FIXME: codec has still some surfaces used

    //
    //	invalid osd bitmap/output surfaces
    //
    for (i = 0; i < 1; ++i) {
#ifdef USE_BITMAP
	VdpauOsdBitmapSurface[i] = VDP_INVALID_HANDLE;
#else
	VdpauOsdOutputSurface[i] = VDP_INVALID_HANDLE;
    }
#endif

    VdpauOsdInit(OsdWidth, OsdHeight);

    return 1;
}

///
///	Set VA-API video mode.
///
static void VdpauSetVideoMode(void)
{
    int i;

    VdpauExitOutputQueue();

    VdpauInitOutputQueue();
    for (i = 0; i < VdpauDecoderN; ++i) {
	VdpauUpdateOutput(VdpauDecoders[i]);
    }
}

#ifdef USE_VIDEO_THREAD

///
///	Handle a VDPAU display.
///
///	@todo FIXME: only a single decoder supported.
///
static void VdpauDisplayHandlerThread(void)
{
    int err;
    int filled;
    struct timespec nowtime;
    VdpauDecoder *decoder;

    if (!(decoder = VdpauDecoders[0])) {	// no stream available
	return;
    }

    if (VdpauPreemption) {		// display preempted
	if (VdpauPreemptionRecover()) {
	    usleep(15 * 1000);
	    return;
	}
    }
    //
    // fill frame output ring buffer
    //
    filled = atomic_read(&decoder->SurfacesFilled);
    err = 1;
    if (filled < VIDEO_SURFACES_MAX) {
	// FIXME: hot polling
	pthread_mutex_lock(&VideoLockMutex);
	// fetch+decode or reopen
	err = VideoDecode();
	pthread_mutex_unlock(&VideoLockMutex);
    }
    if (err) {
	// FIXME: sleep on wakeup
	usleep(5 * 1000);		// nothing buffered
    }

    clock_gettime(CLOCK_REALTIME, &nowtime);
    // time for one frame over?
    if (				//filled<VIDEO_SURFACES_MAX &&
	(nowtime.tv_sec - decoder->FrameTime.tv_sec)
	* 1000 * 1000 * 1000 + (nowtime.tv_nsec - decoder->FrameTime.tv_nsec) <
	15 * 1000 * 1000) {
	return;
    }

    pthread_mutex_lock(&VideoLockMutex);
    VdpauSyncDisplayFrame(decoder);
    pthread_mutex_unlock(&VideoLockMutex);
}

#endif

///
///	Set video output position.
///
///	@param decoder	VDPAU hw decoder
///	@param x	video output x coordinate inside the window
///	@param y	video output y coordinate inside the window
///	@param width	video output width
///	@param height	video output height
///
///	@note FIXME: need to know which stream.
///
static void VdpauSetOutputPosition(VdpauDecoder * decoder, int x, int y,
    int width, int height)
{
    decoder->OutputX = x;
    decoder->OutputY = y;
    decoder->OutputWidth = width;
    decoder->OutputHeight = height;

    // next video pictures are automatic rendered to correct position
}

//----------------------------------------------------------------------------
//	VDPAU OSD
//----------------------------------------------------------------------------

static const uint8_t OsdZeros[1920 * 1080 * 4];	///< 0 for clear osd

///
///	Clear subpicture image.
///
///	@note looked by caller
///
static void VdpauOsdClear(void)
{
    VdpStatus status;
    void const *data[1];
    uint32_t pitches[1];
    VdpRect dst_rect;

    if (VdpauPreemption) {		// display preempted
	return;
    }
    // osd image available?
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#else
    if (VdpauOsdOutputSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#endif

    if (OsdWidth * OsdHeight > 1920 * 1080) {
	Error(_("video/vdpau: osd too big: unsupported\n"));
	return;
    }
    // have dirty area.
    if (OsdDirtyWidth && OsdDirtyHeight) {
	Debug(3, "video/vdpau: osd clear dirty %dx%d+%d+%d\n", OsdDirtyWidth,
	    OsdDirtyHeight, OsdDirtyX, OsdDirtyY);
	dst_rect.x0 = OsdDirtyX;
	dst_rect.y0 = OsdDirtyY;
	dst_rect.x1 = dst_rect.x0 + OsdDirtyWidth;
	dst_rect.y1 = dst_rect.y0 + OsdDirtyHeight;
    } else {
	Debug(3, "video/vdpau: osd clear image\n");
	dst_rect.x0 = 0;
	dst_rect.y0 = 0;
	dst_rect.x1 = dst_rect.x0 + OsdWidth;
	dst_rect.y1 = dst_rect.y0 + OsdHeight;
    }
    data[0] = OsdZeros;
    pitches[0] = OsdWidth * 4;

#ifdef USE_BITMAP
    status =
	VdpauBitmapSurfacePutBitsNative(VdpauOsdBitmapSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: bitmap surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#else
    status =
	VdpauOutputSurfacePutBitsNative(VdpauOsdOutputSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: output surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#endif
}

///
///	Upload ARGB to subpicture image.
///
///	@param x	x position of image in osd
///	@param y	y position of image in osd
///	@param width	width of image
///	@param height	height of image
///	@param argb	argb image
///
///	@note looked by caller
///
static void VdpauOsdDrawARGB(int x, int y, int width, int height,
    const uint8_t * argb)
{
    VdpStatus status;
    void const *data[1];
    uint32_t pitches[1];
    VdpRect dst_rect;
    uint32_t start;
    uint32_t end;

    if (VdpauPreemption) {		// display preempted
	return;
    }
    // osd image available?
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#else
    if (VdpauOsdOutputSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#endif

    start = GetMsTicks();

    dst_rect.x0 = x;
    dst_rect.y0 = y;
    dst_rect.x1 = dst_rect.x0 + width;
    dst_rect.y1 = dst_rect.y0 + height;
    data[0] = argb;
    pitches[0] = width * 4;

#ifdef USE_BITMAP
    status =
	VdpauBitmapSurfacePutBitsNative(VdpauOsdBitmapSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: bitmap surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#else
    status =
	VdpauOutputSurfacePutBitsNative(VdpauOsdOutputSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: output surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#endif
    end = GetMsTicks();

    Debug(3, "video/vdpau: osd upload %dx%d+%d+%d %d ms %d\n", width, height,
	x, y, end - start, width * height * 4);
}

///
///	VDPAU initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
static void VdpauOsdInit(int width, int height)
{
    int i;
    VdpStatus status;

    if (!VdpauDevice) {
	Debug(3, "video/vdpau: vdpau not setup\n");
	return;
    }
    //
    //	create bitmap/surface for osd
    //
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[0] == VDP_INVALID_HANDLE) {
	for (i = 0; i < 1; ++i) {
	    status =
		VdpauBitmapSurfaceCreate(VdpauDevice, VDP_RGBA_FORMAT_B8G8R8A8,
		width, height, VDP_TRUE, VdpauOsdBitmapSurface + i);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create bitmap surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    Debug(4,
		"video/vdpau: created bitmap surface %dx%d with id 0x%08x\n",
		width, height, VdpauOsdBitmapSurface[i]);
	}
    }
#else
    if (VdpauOsdOutputSurface[0] == VDP_INVALID_HANDLE) {
	for (i = 0; i < 1; ++i) {
	    status =
		VdpauOutputSurfaceCreate(VdpauDevice, VDP_RGBA_FORMAT_B8G8R8A8,
		width, height, VdpauOsdOutputSurface + i);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    Debug(4,
		"video/vdpau: created osd output surface %dx%d with id 0x%08x\n",
		width, height, VdpauOsdOutputSurface[i]);
	}
    }
#endif
    Debug(3, "video/vdpau: osd surfaces created\n");
}

///
///	Cleanup osd.
///
static void VdpauOsdExit(void)
{
    int i;

    //
    //	destroy osd bitmap/output surfaces
    //
#ifdef USE_BITMAP
    for (i = 0; i < 1; ++i) {
	VdpStatus status;

	if (VdpauOsdBitmapSurface[i] != VDP_INVALID_HANDLE) {
	    status = VdpauBitmapSurfaceDestroy(VdpauOsdBitmapSurface[i]);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't destroy bitmap surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauOsdBitmapSurface[i] = VDP_INVALID_HANDLE;
	}
    }
#else
    for (i = 0; i < 1; ++i) {
	VdpStatus status;

	if (VdpauOsdOutputSurface[i] != VDP_INVALID_HANDLE) {
	    status = VdpauOutputSurfaceDestroy(VdpauOsdOutputSurface[i]);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't destroy output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauOsdOutputSurface[i] = VDP_INVALID_HANDLE;
	}
    }
#endif
}

///
///	VDPAU module.
///
static const VideoModule VdpauModule = {
    .Name = "vdpau",
    .Enabled = 1,
    .NewHwDecoder = (VideoHwDecoder * (*const)(void))VdpauNewDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VdpauDelDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *))VdpauGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))VdpauReleaseSurface,
    .get_format = (enum PixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum PixelFormat *))Vdpau_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VdpauSyncRenderFrame,
    .GrabOutput = VdpauGrabOutputSurface,
    .SetVideoMode = VdpauSetVideoMode,
    .ResetAutoCrop = VdpauResetAutoCrop,
    .Thread = VdpauDisplayHandlerThread,
    .OsdClear = VdpauOsdClear,
    .OsdDrawARGB = VdpauOsdDrawARGB,
    .OsdInit = VdpauOsdInit,
    .OsdExit = VdpauOsdExit,
    .Init = VdpauInit,
    .Exit = VdpauExit,
};

#endif

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

///
///	Clear the OSD.
///
///	@todo I use glTexImage2D to clear the texture, are there faster and
///	better ways to clear a texture?
///
void VideoOsdClear(void)
{
    if (VideoThread) {
	VideoThreadLock();
    }
#ifdef USE_GLX
    if (GlxEnabled) {
	void *texbuf;

	texbuf = calloc(OsdWidth * OsdHeight, 4);
	glEnable(GL_TEXTURE_2D);	// 2d texture
	glBindTexture(GL_TEXTURE_2D, OsdGlTextures[OsdIndex]);
	// upload no image data, clears texture (on some drivers only)
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, OsdWidth, OsdHeight, 0,
	    GL_BGRA, GL_UNSIGNED_BYTE, texbuf);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
	GlxCheck();
	free(texbuf);
    }
#endif

    if (VideoUsedModule) {
	VideoUsedModule->OsdClear();
    }
    OsdDirtyX = OsdWidth;
    OsdDirtyY = OsdHeight;
    OsdDirtyWidth = 0;
    OsdDirtyHeight = 0;
    OsdShown = 0;

    if (VideoThread) {
	VideoThreadUnlock();
    }
}

///
///	Draw an OSD ARGB image.
///
///	@param x	x position of image in osd
///	@param y	y position of image in osd
///	@param width	width of image
///	@param height	height of image
///	@param argb	argb image
///
void VideoOsdDrawARGB(int x, int y, int width, int height,
    const uint8_t * argb)
{
    if (VideoThread) {
	VideoThreadLock();
    }
    // update dirty area
    if (x < OsdDirtyX) {
	if (OsdDirtyWidth) {
	    OsdDirtyWidth += OsdDirtyX - x;
	}
	OsdDirtyX = x;
    }
    if (y < OsdDirtyY) {
	if (OsdDirtyHeight) {
	    OsdDirtyHeight += OsdDirtyY - y;
	}
	OsdDirtyY = y;
    }
    if (x + width > OsdDirtyX + OsdDirtyWidth) {
	OsdDirtyWidth = x + width - OsdDirtyX;
    }
    if (y + height > OsdDirtyY + OsdDirtyHeight) {
	OsdDirtyHeight = y + height - OsdDirtyY;
    }
    Debug(3, "video: osd dirty %dx%d+%d+%d -> %dx%d+%d+%d\n", width, height, x,
	y, OsdDirtyWidth, OsdDirtyHeight, OsdDirtyX, OsdDirtyY);

#ifdef USE_GLX
    if (GlxEnabled) {
	Debug(3, "video: %p <-> %p\n", glXGetCurrentContext(), GlxContext);
	GlxUploadTexture(x, y, height, width, argb);
	VideoThreadUnlock();
	return;
    }
#endif
    if (VideoUsedModule) {
	VideoUsedModule->OsdDrawARGB(x, y, width, height, argb);
    }
    OsdShown = 1;

    if (VideoThread) {
	VideoThreadUnlock();
    }
}

///
///	Get OSD size.
///
void VideoGetOsdSize(int *width, int *height)
{
    *width = 1920;
    *height = 1080;			// unknown default
    if (OsdWidth && OsdHeight) {
	*width = OsdWidth;
	*height = OsdHeight;
    }
}

///
///	Setup osd.
///
///	FIXME: looking for BGRA, but this fourcc isn't supported by the
///	drawing functions yet.
///
void VideoOsdInit(void)
{
    OsdWidth = VideoWindowWidth;	// FIXME: must be configured
    OsdHeight = VideoWindowHeight;

#ifdef USE_GLX
    // FIXME: make an extra function for this
    if (GlxEnabled) {
	int i;

	Debug(3, "video/glx: %p <-> %p\n", glXGetCurrentContext(), GlxContext);

	//
	//  create a RGBA texture.
	//
	glEnable(GL_TEXTURE_2D);	// create 2d texture(s)

	glGenTextures(2, OsdGlTextures);
	for (i = 0; i < 2; ++i) {
	    glBindTexture(GL_TEXTURE_2D, OsdGlTextures[i]);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		GL_CLAMP_TO_EDGE);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		GL_CLAMP_TO_EDGE);
	    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, OsdWidth, OsdHeight, 0,
		GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	glDisable(GL_TEXTURE_2D);
	return;
    }
#endif

    if (VideoThread) {
	VideoThreadLock();
    }
    if (VideoUsedModule) {
	VideoUsedModule->OsdInit(OsdWidth, OsdHeight);
    }
    if (VideoThread) {
	VideoThreadUnlock();
    }
    VideoOsdClear();
}

///
///	Cleanup OSD.
///
void VideoOsdExit(void)
{
    if (VideoThread) {
	VideoThreadLock();
    }
    if (VideoUsedModule) {
	VideoUsedModule->OsdExit();
    }
    if (VideoThread) {
	VideoThreadUnlock();
    }
}

#if 0

//----------------------------------------------------------------------------
//	Overlay
//----------------------------------------------------------------------------

///
///	Render osd surface.
///
void VideoRenderOverlay(void)
{
#ifdef USE_GLX
    if (GlxEnabled) {
	GlxRender(OsdWidth, OsdHeight);
    } else
#endif
    {
    }
}

///
///	Display overlay surface.
///
void VideoDisplayOverlay(void)
{
#ifdef USE_GLX
    if (GlxEnabled) {
	int osd_x1;
	int osd_y1;

	osd_x1 = 0;
	osd_y1 = 0;
#ifdef noDEBUG
	osd_x1 = 100;
	osd_y1 = 100;
#endif
	GlxRenderTexture(OsdGlTextures[OsdIndex], osd_x1, osd_y1,
	    VideoWindowWidth, VideoWindowHeight);
	return;
    }
#endif
#ifdef USE_VAAPI
    {
	void *image_buffer;
	static int counter;

	// upload needs long time
	if (counter == 5) {
	    //return;
	}
	// osd image available?
	if (VaOsdImage.image_id == VA_INVALID_ID) {
	    return;
	}
	// FIXME: this version hangups
	//return;

	// map osd surface/image into memory.
	if (vaMapBuffer(VaDisplay, VaOsdImage.buf,
		&image_buffer) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't map osd image buffer\n"));
	    return;
	}
	// 100% transparent
	memset(image_buffer, 0x80 | counter++, VaOsdImage.data_size);

	// convert internal osd to VA-API image
	//GfxConvert(image_buffer, VaOsdImage.offsets[0], VaOsdImage.pitches[0]);

	if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't unmap osd image buffer\n"));
	}
    }
#endif
}

#endif

//----------------------------------------------------------------------------
//	Frame
//----------------------------------------------------------------------------

#if 0

///
///	Display a single frame.
///
static void VideoDisplayFrame(void)
{
#ifdef USE_GLX
    if (GlxEnabled) {
	VideoDisplayOverlay();

#ifdef USE_DOUBLEBUFFER
	glXSwapBuffers(XlibDisplay, VideoWindow);
#else
	glFinish();			// wait for all execution finished
#endif
	GlxCheck();

	glClear(GL_COLOR_BUFFER_BIT);
    }
#endif

    if (VideoUsedModule) {
	VideoUsedModule->DisplayFrame();
    }
}

#endif

//----------------------------------------------------------------------------
//	Events
//----------------------------------------------------------------------------

/// C callback feed key press
extern void FeedKeyPress(const char *, const char *, int, int);

///
///	Handle X11 events.
///
///	@todo	Signal WmDeleteMessage to application.
///
static void VideoEvent(void)
{
    XEvent event;
    KeySym keysym;

    //char buf[32];

    XNextEvent(XlibDisplay, &event);
    switch (event.type) {
	case ClientMessage:
	    Debug(3, "video/event: ClientMessage\n");
	    if (event.xclient.data.l[0] == (long)WmDeleteWindowAtom) {
		Debug(3, "video/event: wm-delete-message\n");
		FeedKeyPress("XKeySym", "Close", 0, 0);
	    }
	    break;

	case MapNotify:
	    Debug(3, "video/event: MapNotify\n");
	    // �wm workaround
	    xcb_change_window_attributes(Connection, VideoWindow,
		XCB_CW_CURSOR, &VideoBlankCursor);
	    break;
	case Expose:
	    //Debug(3, "video/event: Expose\n");
	    break;
	case ReparentNotify:
	    Debug(3, "video/event: ReparentNotify\n");
	    break;
	case ConfigureNotify:
	    //Debug(3, "video/event: ConfigureNotify\n");
	    VideoSetVideoMode(event.xconfigure.x, event.xconfigure.y,
		event.xconfigure.width, event.xconfigure.height);
	    break;
	case ButtonPress:
	    VideoSetFullscreen(-1);
	    break;
	case KeyPress:
	    keysym = XLookupKeysym(&event.xkey, 0);
#if 0
	    switch (keysym) {
		case XK_d:
		    break;
		case XK_S:
		    break;
	    }
#endif
	    if (keysym == NoSymbol) {
		Warning(_("video/event: No symbol for %d\n"),
		    event.xkey.keycode);
	    }
	    FeedKeyPress("XKeySym", XKeysymToString(keysym), 0, 0);
	    /*
	       if (XLookupString(&event.xkey, buf, sizeof(buf), &keysym, NULL)) {
	       FeedKeyPress("XKeySym", buf, 0, 0);
	       } else {
	       FeedKeyPress("XKeySym", XKeysymToString(keysym), 0, 0);
	       }
	     */
	case KeyRelease:
	    break;
	default:
#if 0
	    if (XShmGetEventBase(XlibDisplay) + ShmCompletion == event.type) {
		// printf("ShmCompletion\n");
	    }
#endif
	    Debug(3, "Unsupported event type %d\n", event.type);
	    break;
    }
}

///
///	Poll all x11 events.
///
void VideoPollEvent(void)
{
    while (XPending(XlibDisplay)) {
	VideoEvent();
    }
}

//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

#ifdef USE_VIDEO_THREAD

#ifdef USE_GLX
static GLXContext GlxThreadContext;	///< our gl context for the thread
#endif

///
///	Lock video thread.
///
static void VideoThreadLock(void)
{
    if (pthread_mutex_lock(&VideoLockMutex)) {
	Error(_("video: can't lock thread\n"));
    }
}

///
///	Unlock video thread.
///
static void VideoThreadUnlock(void)
{
    if (pthread_mutex_unlock(&VideoLockMutex)) {
	Error(_("video: can't unlock thread\n"));
    }
}

///
///	Video render thread.
///
static void *VideoDisplayHandlerThread(void *dummy)
{
    Debug(3, "video: display thread started\n");

#ifdef USE_GLX
    if (GlxEnabled) {
	Debug(3, "video: %p <-> %p\n", glXGetCurrentContext(),
	    GlxThreadContext);
	GlxThreadContext =
	    glXCreateContext(XlibDisplay, GlxVisualInfo, GlxContext, GL_TRUE);
	if (!GlxThreadContext) {
	    Error(_("video/glx: can't create glx context\n"));
	    return NULL;
	}
	// set glx context
	if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext)) {
	    GlxCheck();
	    Error(_("video/glx: can't make glx context current\n"));
	    return NULL;
	}
    }
#endif

    for (;;) {
	// fix dead-lock with VdpauExit
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	VideoPollEvent();

	if (VideoUsedModule) {
	    VideoUsedModule->Thread();
	} else {
	    XEvent event;

	    // FIXME: move into noop module
	    // avoid 100% cpu use

	    XPeekEvent(XlibDisplay, &event);
	}
    }

    return dummy;
}

///
///	Initialize video threads.
///
static void VideoThreadInit(void)
{
    pthread_mutex_init(&VideoMutex, NULL);
    pthread_mutex_init(&VideoLockMutex, NULL);
    pthread_cond_init(&VideoWakeupCond, NULL);
    pthread_create(&VideoThread, NULL, VideoDisplayHandlerThread, NULL);
    pthread_setname_np(VideoThread, "softhddev video");
}

///
///	Exit and cleanup video threads.
///
static void VideoThreadExit(void)
{
    if (VideoThread) {
	void *retval;

	Debug(3, "video: video thread canceled\n");
	//VideoThreadLock();
	// FIXME: can't cancel locked
	if (pthread_cancel(VideoThread)) {
	    Error(_("video: can't queue cancel video display thread\n"));
	}
	//VideoThreadUnlock();
	if (pthread_join(VideoThread, &retval) || retval != PTHREAD_CANCELED) {
	    Error(_("video: can't cancel video display thread\n"));
	}
	pthread_cond_destroy(&VideoWakeupCond);
	pthread_mutex_destroy(&VideoLockMutex);
	pthread_mutex_destroy(&VideoMutex);
	VideoThread = 0;
    }
}

///
///	Video display wakeup.
///
///	New video arrived, wakeup video thread.
///
void VideoDisplayWakeup(void)
{
    if (!XlibDisplay) {			// not yet started
	return;
    }

    if (!VideoThread) {			// start video thread, if needed
	VideoThreadInit();
    }
}

#endif

//----------------------------------------------------------------------------
//	Video API
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

///
///	Table of all audio modules.
///
static const VideoModule *VideoModules[] = {
#ifdef USE_VDPAU
    &VdpauModule,
#endif
#ifdef USE_VAAPI
    &VaapiModule,
#endif
    //&NoopModule
};

///
///	Video hardware decoder
///
struct _video_hw_decoder_
{
    union
    {
#ifdef USE_VAAPI
	VaapiDecoder Vaapi;		///< VA-API decoder structure
#endif
#ifdef USE_VDPAU
	VdpauDecoder Vdpau;		///< vdpau decoder structure
#endif
    };
};

///
///	Allocate new video hw decoder.
///
///	@returns a new initialized video hardware decoder.
///
VideoHwDecoder *VideoNewHwDecoder(void)
{
    if (!XlibDisplay || !VideoUsedModule) {	// waiting for x11 start
	return NULL;
    }
    return VideoUsedModule->NewHwDecoder();
}

///
///	Destroy a video hw decoder.
///
///	@param decoder	video hardware decoder
///
void VideoDelHwDecoder(VideoHwDecoder * decoder)
{
    if (decoder && VideoUsedModule) {
	VideoUsedModule->DelHwDecoder(decoder);
    }
}

///
///	Get a free hardware decoder surface.
///
///	@param decoder	video hardware decoder
///
///	@returns the oldest free surface or invalid surface
///
unsigned VideoGetSurface(VideoHwDecoder * decoder)
{
    if (VideoUsedModule) {
	return VideoUsedModule->GetSurface(decoder);
    }
    return -1;
}

///
///	Release a hardware decoder surface.
///
///	@param decoder	VDPAU video hardware decoder
///	@param surface	surface no longer used
///
void VideoReleaseSurface(VideoHwDecoder * decoder, unsigned surface)
{
    // FIXME: must be guarded against calls, after VideoExit
    if (!XlibDisplay) {			// no init or failed
	return;
    }
    if (VideoUsedModule) {
	VideoUsedModule->ReleaseSurface(decoder, surface);
    }
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
enum PixelFormat Video_get_format(VideoHwDecoder * decoder,
    AVCodecContext * video_ctx, const enum PixelFormat *fmt)
{
    if (VideoUsedModule) {
	return VideoUsedModule->get_format(decoder, video_ctx, fmt);
    }
    return fmt[0];
}

///
///	Display a ffmpeg frame
///
///	@param decoder		VDPAU video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
void VideoRenderFrame(VideoHwDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    if (frame->repeat_pict) {
	Warning(_("video: repeated pict found, but not handled\n"));
    }
    if (VideoUsedModule) {
	VideoUsedModule->RenderFrame(decoder, video_ctx, frame);
    }
}

///
///	Get VA-API ffmpeg context
///
///	@param decoder	VA-API decoder
///
struct vaapi_context *VideoGetVaapiContext(VideoHwDecoder * decoder)
{
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule) {
	return decoder->Vaapi.VaapiContext;
    }
#endif
    (void)decoder;
    Error(_("video/vaapi: get vaapi context, without vaapi enabled\n"));
    return NULL;
}

#ifdef USE_VDPAU
///
///	Draw ffmpeg vdpau render state.
///
///	@param decoder	video hw decoder
///	@param vrs	vdpau render state
///
void VideoDrawRenderState(VideoHwDecoder * hw_decoder,
    struct vdpau_render_state *vrs)
{
    if (VideoUsedModule == &VdpauModule) {
	VdpStatus status;
	uint32_t start;
	uint32_t end;
	VdpauDecoder *decoder;

	decoder = &hw_decoder->Vdpau;
	if (decoder->VideoDecoder == VDP_INVALID_HANDLE) {
	    Debug(3, "video/vdpau: recover preemption\n");
	    status =
		VdpauDecoderCreate(VdpauDevice, decoder->Profile,
		decoder->InputWidth, decoder->InputHeight,
		decoder->SurfacesNeeded - VIDEO_SURFACES_MAX,
		&decoder->VideoDecoder);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create decoder: %s\n"),
		    VdpauGetErrorString(status));
	    }

	    VdpauSetupOutput(decoder);
	    return;
	}

	Debug(4, "video/vdpau: decoder render to %#010x\n", vrs->surface);
	start = GetMsTicks();
	status =
	    VdpauDecoderRender(decoder->VideoDecoder, vrs->surface,
	    (VdpPictureInfo const *)&vrs->info, vrs->bitstream_buffers_used,
	    vrs->bitstream_buffers);
	end = GetMsTicks();
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: decoder rendering failed: %s\n"),
		VdpauGetErrorString(status));
	}
	if (end - start > 35) {
	    // report this
	    Info(_("video/vdpau: decoder render too slow %u ms\n"),
		end - start);
	}
	return;
    }
    Error(_("video/vdpau: draw render state, without vdpau enabled\n"));
    return;
}
#endif

#ifndef USE_VIDEO_THREAD

///
///	Video render.
///
///	@FIXME: old, not used and not uptodate code path
///
void VideoDisplayHandler(void)
{
    uint32_t now;

    if (!XlibDisplay) {			// not yet started
	return;
    }

    now = GetMsTicks();
    if (now < VaapiDecoders[0]->LastFrameTick) {
	return;
    }
    if (now - VaapiDecoders[0]->LastFrameTick < 500) {
	return;
    }
    VideoPollEvent();
    VaapiBlackSurface(VaapiDecoders[0]);

    return;
    VideoDisplayFrame();
}

#endif

///
///	Get video clock.
///
///	@note this isn't monoton, decoding reorders frames,
///	setter keeps it monotonic
///	@todo we have multiple clocks, for multiple stream
///
int64_t VideoGetClock(void)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	return VdpauGetClock(VdpauDecoders[0]);
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule) {
	return VaapiGetClock(VaapiDecoders[0]);
    }
#endif
    return 0L;
}

///
///	Grab full screen image.
///
///	@param size[out]	size of allocated image
///	@param width[in,out]	width of image
///	@param height[in,out]	height of image
///
uint8_t *VideoGrab(int *size, int *width, int *height, int write_header)
{
    Debug(3, "video: grab\n");

#ifdef USE_GRAB
    if (VideoUsedModule && VideoUsedModule->GrabOutput) {
	uint8_t *data;
	uint8_t *rgb;
	char buf[64];
	int i;
	int n;
	int scale_width;
	int scale_height;
	int x;
	int y;
	double src_x;
	double src_y;
	double scale_x;
	double scale_y;

	scale_width = *width;
	scale_height = *height;
	data = VideoUsedModule->GrabOutput(size, width, height);

	if (scale_width <= 0) {
	    scale_width = *width;
	}
	if (scale_height <= 0) {
	    scale_height = *height;
	}
	// hardware didn't scale for us, use simple software scaler
	if (scale_width != *width && scale_height != *height) {
	    n = 0;
	    if (write_header) {
		n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n",
		    scale_width, scale_height);
	    }
	    rgb = malloc(scale_width * scale_height * 3 + n);
	    if (!rgb) {
		Error(_("video: out of memory\n"));
		free(data);
		return NULL;
	    }
	    *size = scale_width * scale_height * 3 + n;
	    memcpy(rgb, buf, n);	// header

	    scale_x = (double)*width / scale_width;
	    scale_y = (double)*height / scale_height;

	    src_y = 0.0;
	    for (y = 0; y < scale_height; y++) {
		int o;

		src_x = 0.0;
		o = (int)src_y **width;

		for (x = 0; x < scale_width; x++) {
		    i = 4 * (o + (int)src_x);

		    rgb[n + (x + y * scale_width) * 3 + 0] = data[i + 2];
		    rgb[n + (x + y * scale_width) * 3 + 1] = data[i + 1];
		    rgb[n + (x + y * scale_width) * 3 + 2] = data[i + 0];

		    src_x += scale_x;
		}

		src_y += scale_y;
	    }

	    *width = scale_width;
	    *height = scale_height;

	    // grabed image of correct size convert BGRA -> RGB
	} else {
	    n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", *width,
		*height);
	    rgb = malloc(*width * *height * 3 + n);
	    if (!rgb) {
		Error(_("video: out of memory\n"));
		free(data);
		return NULL;
	    }
	    memcpy(rgb, buf, n);	// header

	    for (i = 0; i < *size / 4; ++i) {	// convert bgra -> rgb
		rgb[n + i * 3 + 0] = data[i * 4 + 2];
		rgb[n + i * 3 + 1] = data[i * 4 + 1];
		rgb[n + i * 3 + 2] = data[i * 4 + 0];
	    }

	    *size = *width * *height * 3 + n;
	}
	free(data);

	return rgb;
    }
#endif
    (void)size;
    (void)width;
    (void)height;
    (void)write_header;
    return NULL;
}

//----------------------------------------------------------------------------
//	Setup
//----------------------------------------------------------------------------

///
///	Create main window.
///
///	@param parent	parent of new window
///	@param visual	visual of parent
///	@param depth	depth of parent
///
static void VideoCreateWindow(xcb_window_t parent, xcb_visualid_t visual,
    uint8_t depth)
{
    uint32_t values[4];
    xcb_intern_atom_reply_t *reply;
    xcb_pixmap_t pixmap;
    xcb_cursor_t cursor;

    Debug(3, "video: visual %#0x depth %d\n", visual, depth);

    // Color map
    VideoColormap = xcb_generate_id(Connection);
    xcb_create_colormap(Connection, XCB_COLORMAP_ALLOC_NONE, VideoColormap,
	parent, visual);

    values[0] = 0;
    values[1] = 0;
    values[2] =
	XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
	XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
	XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    values[3] = VideoColormap;
    VideoWindow = xcb_generate_id(Connection);
    xcb_create_window(Connection, depth, VideoWindow, parent, VideoWindowX,
	VideoWindowY, VideoWindowWidth, VideoWindowHeight, 0,
	XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
	XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK |
	XCB_CW_COLORMAP, values);

    // define only available with xcb-utils-0.3.8
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
    // FIXME: utf _NET_WM_NAME
    xcb_icccm_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,
	sizeof("softhddevice") - 1, "softhddevice");
    xcb_icccm_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,
	sizeof("softhddevice") - 1, "softhddevice");
#endif
    // define only available with xcb-utils-0.3.6
#ifdef XCB_NUM_WM_HINTS_ELEMENTS
    // FIXME: utf _NET_WM_NAME
    xcb_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING,
	sizeof("softhddevice") - 1, "softhddevice");
    xcb_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING,
	sizeof("softhddevice") - 1, "softhddevice");
#endif

    // FIXME: size hints

    // register interest in the delete window message
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("WM_DELETE_WINDOW") - 1, "WM_DELETE_WINDOW"),
		NULL))) {
	WmDeleteWindowAtom = reply->atom;
	free(reply);
	if ((reply =
		xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection,
			0, sizeof("WM_PROTOCOLS") - 1, "WM_PROTOCOLS"),
		    NULL))) {
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
	    xcb_icccm_set_wm_protocols(Connection, VideoWindow, reply->atom, 1,
		&WmDeleteWindowAtom);
#endif
#ifdef XCB_NUM_WM_HINTS_ELEMENTS
	    xcb_set_wm_protocols(Connection, reply->atom, VideoWindow, 1,
		&WmDeleteWindowAtom);
#endif
	    free(reply);
	}
    }
    //
    //	prepare fullscreen.
    //
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("_NET_WM_STATE") - 1, "_NET_WM_STATE"), NULL))) {
	NetWmState = reply->atom;
	free(reply);
    }
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("_NET_WM_STATE_FULLSCREEN") - 1,
		    "_NET_WM_STATE_FULLSCREEN"), NULL))) {
	NetWmStateFullscreen = reply->atom;
	free(reply);
    }

    xcb_map_window(Connection, VideoWindow);

    //
    //	hide cursor
    //
    pixmap = xcb_generate_id(Connection);
    xcb_create_pixmap(Connection, 1, pixmap, parent, 1, 1);
    cursor = xcb_generate_id(Connection);
    xcb_create_cursor(Connection, cursor, pixmap, pixmap, 0, 0, 0, 0, 0, 0, 1,
	1);

    values[0] = cursor;
    xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR,
	values);
    VideoBlankCursor = cursor;
    // FIXME: free cursor/pixmap needed?
}

///
///	Set video geometry.
///
///	@param geometry	 [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]
///
int VideoSetGeometry(const char *geometry)
{
    int flags;

    flags =
	XParseGeometry(geometry, &VideoWindowX, &VideoWindowY,
	&VideoWindowWidth, &VideoWindowHeight);

    return 0;
}

///
///	Set video output position.
///
///	@param x	video output x coordinate inside the window
///	@param y	video output y coordinate inside the window
///	@param width	video output width
///	@param height	video output height
///
///	@note FIXME: need to know which stream.
///
void VideoSetOutputPosition(int x, int y, int width, int height)
{
    // FIXME: high level, currently works osd relative
    if (!OsdWidth || !OsdHeight) {
	return;
    }
    x = (x * VideoWindowWidth) / OsdWidth;
    y = (y * VideoWindowHeight) / OsdHeight;
    width = (width * VideoWindowWidth) / OsdWidth;
    height = (height * VideoWindowHeight) / OsdHeight;
    if (VideoThread) {
	VideoThreadLock();
    }
    if (VideoUsedModule) {
	// FIXME: what stream?
    }
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	VdpauSetOutputPosition(VdpauDecoders[0], x, y, width, height);
    }
#endif
#ifdef USE_VAPI
    // FIXME: not supported by vaapi without unscaled OSD,
    // FIXME: if used to position video inside osd
#endif
    if (VideoThread) {
	VideoThreadUnlock();
    }
}

///
///	Set video window position.
///
///	@param x	window x coordinate
///	@param y	window y coordinate
///	@param width	window width
///	@param height	window height
///
///	@note no need to lock, only called from inside the video thread
///
void VideoSetVideoMode( __attribute__ ((unused))
    int x, __attribute__ ((unused))
    int y, int width, int height)
{
    Debug(4, "video: %s %dx%d%+d%+d\n", __FUNCTION__, width, height, x, y);

    if ((unsigned)width == VideoWindowWidth
	&& (unsigned)height == VideoWindowHeight) {
	return;				// same size nothing todo
    }

    VideoOsdExit();
    // FIXME: must tell VDR that the OsdSize has been changed!

    if (VideoThread) {
	VideoThreadLock();
    }
    VideoWindowWidth = width;
    VideoWindowHeight = height;
    if (VideoUsedModule) {
	VideoUsedModule->SetVideoMode();
    }
    if (VideoThread) {
	VideoThreadUnlock();
    }
    VideoOsdInit();
}

///
///	Set video display format.
///
void VideoSetDisplayFormat(int format)
{
    VideoOsdExit();
    // FIXME: must tell VDR that the OsdSize has been changed!

    if (VideoThread) {
	VideoThreadLock();
    }

    switch (format) {
	case 0:			// pan&scan (we have no pan&scan)
	    Video4to3ZoomMode = VideoStretch;
	    break;
	case 1:			// letter box
	    Video4to3ZoomMode = VideoNormal;
	    break;
	case 2:			// center cut-out
	    Video4to3ZoomMode = VideoCenterCutOut;
	    break;
    }

    if (VideoUsedModule) {
	VideoUsedModule->SetVideoMode();
    }
    if (VideoThread) {
	VideoThreadUnlock();
    }
    VideoOsdInit();
}

///
///	Send fullscreen message to window.
///
///	@param onoff	-1 toggle, true turn on, false turn off
///
void VideoSetFullscreen(int onoff)
{
    xcb_client_message_event_t event;

    memset(&event, 0, sizeof(event));
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = VideoWindow;
    event.type = NetWmState;
    if (onoff < 0) {
	event.data.data32[0] = XCB_EWMH_WM_STATE_TOGGLE;
    } else if (onoff) {
	event.data.data32[0] = XCB_EWMH_WM_STATE_ADD;
    } else {
	event.data.data32[0] = XCB_EWMH_WM_STATE_REMOVE;
    }
    event.data.data32[1] = NetWmStateFullscreen;

    xcb_send_event(Connection, XCB_SEND_EVENT_DEST_POINTER_WINDOW,
	DefaultRootWindow(XlibDisplay),
	XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (void *)&event);
    Debug(3, "video/x11: send fullscreen message %x %x\n",
	event.data.data32[0], event.data.data32[1]);
}

///
///	Set deinterlace mode.
///
void VideoSetDeinterlace(int mode[VideoResolutionMax])
{
    VideoDeinterlace[0] = mode[0];
    VideoDeinterlace[1] = mode[1];
    VideoDeinterlace[2] = mode[2];
    VideoDeinterlace[3] = mode[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set skip chroma deinterlace on/off.
///
void VideoSetSkipChromaDeinterlace(int onoff[VideoResolutionMax])
{
    VideoSkipChromaDeinterlace[0] = onoff[0];
    VideoSkipChromaDeinterlace[1] = onoff[1];
    VideoSkipChromaDeinterlace[2] = onoff[2];
    VideoSkipChromaDeinterlace[3] = onoff[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set denoise level (0 .. 1000).
///
void VideoSetDenoise(int level[VideoResolutionMax])
{
    VideoDenoise[0] = level[0];
    VideoDenoise[1] = level[1];
    VideoDenoise[2] = level[2];
    VideoDenoise[3] = level[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set sharpness level (-1000 .. 1000).
///
void VideoSetSharpen(int level[VideoResolutionMax])
{
    VideoSharpen[0] = level[0];
    VideoSharpen[1] = level[1];
    VideoSharpen[2] = level[2];
    VideoSharpen[3] = level[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set scaling mode.
///
///	@param mode	table with VideoResolutionMax values
///
void VideoSetScaling(int mode[VideoResolutionMax])
{
    VideoScaling[0] = mode[0];
    VideoScaling[1] = mode[1];
    VideoScaling[2] = mode[2];
    VideoScaling[3] = mode[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set skip lines.
///
///	@param lines	lines in pixel
///
void VideoSetSkipLines(int lines)
{
    VideoSkipLines = lines;
}

///
///	Set audio delay.
///
///	@param ms	delay in ms
///
void VideoSetAudioDelay(int ms)
{
    VideoAudioDelay = ms * 90;
}

///
///	Set auto-crop parameters.
///
void VideoSetAutoCrop(int interval, int delay, int tolerance)
{
#ifdef USE_AUTOCROP
    AutoCropInterval = interval;
    AutoCropDelay = delay;
    AutoCropTolerance = tolerance;

    if (VideoThread) {
	VideoThreadLock();
    }
    if (VideoUsedModule) {
	VideoUsedModule->ResetAutoCrop();
    }
    if (VideoThread) {
	VideoThreadUnlock();
    }
#else
    (void)interval;
    (void)delay;
    (void)tolerance;
#endif
}

///
///	Initialize video output module.
///
///	@param display_name	X11 display name
///
void VideoInit(const char *display_name)
{
    int screen_nr;
    int i;
    xcb_screen_iterator_t screen_iter;
    xcb_screen_t *screen;

    if (XlibDisplay) {			// allow multiple calls
	Debug(3, "video: x11 already setup\n");
	return;
    }
    // Open the connection to the X server.
    // use the DISPLAY environment variable as the default display name
    if (!display_name) {
	display_name = getenv("DISPLAY");
	if (!display_name) {
	    // use :0.0 as default display name
	    display_name = ":0.0";
	}
    }
    if (!(XlibDisplay = XOpenDisplay(display_name))) {
	Fatal(_("video: Can't connect to X11 server on '%s'"), display_name);
	// FIXME: we need to retry connection
    }
    // XInitThreads();
    // Convert XLIB display to XCB connection
    if (!(Connection = XGetXCBConnection(XlibDisplay))) {
	Fatal(_("video: Can't convert XLIB display to XCB connection"));
    }
    // prefetch extensions
    //xcb_prefetch_extension_data(Connection, &xcb_big_requests_id);
    //xcb_prefetch_extension_data(Connection, &xcb_dpms_id);
    //xcb_prefetch_extension_data(Connection, &xcb_glx_id);
    //xcb_prefetch_extension_data(Connection, &xcb_randr_id);
    //xcb_prefetch_extension_data(Connection, &xcb_screensaver_id);
    //xcb_prefetch_extension_data(Connection, &xcb_shm_id);
    //xcb_prefetch_extension_data(Connection, &xcb_xv_id);

    // Get the requested screen number
    screen_nr = DefaultScreen(XlibDisplay);
    screen_iter = xcb_setup_roots_iterator(xcb_get_setup(Connection));
    for (i = 0; i < screen_nr; ++i) {
	xcb_screen_next(&screen_iter);
    }
    screen = screen_iter.data;

    //
    //	Default window size
    //
    if (!VideoWindowHeight) {
	if (VideoWindowWidth) {
	    VideoWindowHeight = (VideoWindowWidth * 9) / 16;
	}
	VideoWindowHeight = 576;
    }
    if (!VideoWindowWidth) {
	VideoWindowWidth = (VideoWindowHeight * 16) / 9;
    }
    //
    //	prepare opengl
    //
#ifdef USE_GLX
    if (GlxEnabled) {

	GlxInit();
	// FIXME: use root window?
	VideoCreateWindow(screen->root, GlxVisualInfo->visualid,
	    GlxVisualInfo->depth);
	GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight);
    } else
#endif

	//
	// Create output window
	//
    if (1) {				// FIXME: use window mode

	VideoCreateWindow(screen->root, screen->root_visual,
	    screen->root_depth);

    } else {
	// FIXME: support embedded mode
	VideoWindow = screen->root;

	// FIXME: VideoWindowHeight VideoWindowWidth
    }

    Debug(3, "video: window prepared\n");

    //
    //	prepare hardware decoder VA-API/VDPAU
    //	FIXME: make the used output modules configurable
    //
    for (i = 0; i < (int)(sizeof(VideoModules) / sizeof(*VideoModules)); ++i) {
	if (VideoModules[i]->Enabled) {
	    if (VideoModules[i]->Init(display_name)) {
		VideoUsedModule = VideoModules[i];
		break;
	    }
	}
    }

    // FIXME: make it configurable from gui
    VideoHardwareDecoder = -1;
    if (getenv("NO_MPEG_HW")) {
	VideoHardwareDecoder = 1;
    }
    if (getenv("NO_HW")) {
	VideoHardwareDecoder = 0;
    }
    //xcb_prefetch_maximum_request_length(Connection);
    xcb_flush(Connection);
}

///
///	Cleanup video output module.
///
void VideoExit(void)
{
    if (!XlibDisplay) {			// no init or failed
	return;
    }
#ifdef USE_VIDEO_THREAD
    VideoThreadExit();
    // VDPAU cleanup hangs in XLockDisplay every 100 exits
    // XUnlockDisplay(XlibDisplay);
    // xcb_flush(Connection);
#endif
    if (VideoUsedModule) {
	VideoUsedModule->Exit();
    }
#ifdef USE_GLX
    if (GlxEnabled) {
	GlxExit();
    }
#endif

    //
    //	Reenable screensaver / DPMS.
    //
    //X11SuspendScreenSaver(XlibDisplay, False);
    //X11DPMSEnable(XlibDisplay);

    //
    //	FIXME: cleanup.
    //
    //RandrExit();

    //
    //	X11/xcb cleanup
    //
    if (VideoWindow != XCB_NONE) {
	xcb_destroy_window(Connection, VideoWindow);
	VideoWindow = XCB_NONE;
    }
    if (XlibDisplay) {
	if (XCloseDisplay(XlibDisplay)) {
	    Error(_("video: error closing display\n"));
	}
	XlibDisplay = NULL;
    }
}

#endif

#ifdef VIDEO_TEST

#include <getopt.h>

int SysLogLevel;			///< show additional debug informations
uint32_t VideoSwitch;			///< required

uint64_t AudioGetDelay(void)		///< required
{
    return 0UL;
}

int64_t AudioGetClock(void)		///< required
{
    return AV_NOPTS_VALUE;
}

void FeedKeyPress( __attribute__ ((unused))
    const char *x, __attribute__ ((unused))
    const char *y, __attribute__ ((unused))
    int a, __attribute__ ((unused))
    int b)
{
}

int VideoDecode(void)
{
    return -1;
}

///
///	Print version.
///
static void PrintVersion(void)
{
    printf("video_test: video tester Version " VERSION
#ifdef GIT_REV
	"(GIT-" GIT_REV ")"
#endif
	",\n\t(c) 2009 - 2012 by Johns\n"
	"\tLicense AGPLv3: GNU Affero General Public License version 3\n");
}

///
///	Print usage.
///
static void PrintUsage(void)
{
    printf("Usage: video_test [-?dhv]\n"
	"\t-d\tenable debug, more -d increase the verbosity\n"
	"\t-? -h\tdisplay this message\n" "\t-v\tdisplay version information\n"
	"Only idiots print usage on stderr!\n");
}

///
///	Main entry point.
///
///	@param argc	number of arguments
///	@param argv	arguments vector
///
///	@returns -1 on failures, 0 clean exit.
///
int main(int argc, char *const argv[])
{
    uint32_t start_tick;
    uint32_t tick;
    int n;
    VideoHwDecoder *video_hw_decoder;

    SysLogLevel = 0;

    //
    //	Parse command line arguments
    //
    for (;;) {
	switch (getopt(argc, argv, "hv?-c:dg:")) {
	    case 'd':			// enabled debug
		++SysLogLevel;
		continue;
	    case 'g':			// geometry
		if (VideoSetGeometry(optarg) < 0) {
		    fprintf(stderr,
			_
			("Bad formated geometry please use: [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]\n"));
		    return 0;
		}
		continue;

	    case EOF:
		break;
	    case 'v':			// print version
		PrintVersion();
		return 0;
	    case '?':
	    case 'h':			// help usage
		PrintVersion();
		PrintUsage();
		return 0;
	    case '-':
		PrintVersion();
		PrintUsage();
		fprintf(stderr, "\nWe need no long options\n");
		return -1;
	    case ':':
		PrintVersion();
		fprintf(stderr, "Missing argument for option '%c'\n", optopt);
		return -1;
	    default:
		PrintVersion();
		fprintf(stderr, "Unkown option '%c'\n", optopt);
		return -1;
	}
	break;
    }
    if (optind < argc) {
	PrintVersion();
	while (optind < argc) {
	    fprintf(stderr, "Unhandled argument '%s'\n", argv[optind++]);
	}
	return -1;
    }
    //
    //	  main loop
    //
    VideoInit(NULL);
    VideoOsdInit();
    video_hw_decoder = VideoNewHwDecoder();
    start_tick = GetMsTicks();
    n = 0;
    for (;;) {
#if 0
	VideoRenderOverlay();
	VideoDisplayOverlay();
	glXSwapBuffers(XlibDisplay, VideoWindow);
	GlxCheck();
	glClear(GL_COLOR_BUFFER_BIT);

	XSync(XlibDisplay, False);
	XFlush(XlibDisplay);
#endif
#ifdef USE_VAAPI
	if (VideoVaapiEnabled) {
	    VaapiDisplayFrame();
	}
#endif
#ifdef USE_VDPAU
	if (VideoVdpauEnabled) {
	    VdpauDisplayFrame();
	}
#endif
	tick = GetMsTicks();
	n++;
	if (!(n % 100)) {
	    printf("%d ms / frame\n", (tick - start_tick) / n);
	}
	usleep(2 * 1000);
    }
    VideoExit();

    return 0;
}

#endif
