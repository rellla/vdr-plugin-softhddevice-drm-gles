/**
 * @file grab.cpp
 * Grabing classes
 *
 * This file defines cGrabBuffer and cSoftHdGrab, which are used
 * to handle grab requests.
 *
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

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <sys/mman.h>

#include <drm_fourcc.h>

#include "drmbuffer.h"
#include "grab.h"
#include "logger.h"
#include "videorender.h"

/****************************************************************************************
 * Image data conversion helpers
 ***************************************************************************************/
#define OPAQUE      0xff
#define TRANSPARENT 0x00
#define UNMULTIPLY(color, alpha) ((0xff * color) / alpha)
#define BLEND(back, front, alpha) ((front * alpha) + (back * (255 - alpha))) / 255

/**
 * Convert a DRM format to a ffmpeg AV format
 */
enum AVPixelFormat DrmFormatToAVFormat(cDrmBuffer *buf)
{
	if (buf->PixFmt() == DRM_FORMAT_NV12)
		return AV_PIX_FMT_NV12;

	if (buf->PixFmt() == DRM_FORMAT_YUV420)
		return AV_PIX_FMT_YUV420P;

	if (buf->PixFmt() == DRM_FORMAT_ARGB8888)
		return AV_PIX_FMT_RGBA;

	if (buf->PixFmt() == DRM_FORMAT_P030)
		return AV_PIX_FMT_NONE;

	return AV_PIX_FMT_NONE;
}

/**
 * Convert a DRM buffer to rgb format image
 *
 * Conversion is done with ffmpegs swscale
 *
 * @param[in] buf             pointer to the source drm buffer struct
 * @param[out] size           size of the return data
 * @param[in] dstW            width of the returned image
 * @param[in] dstH            height of the returned image
 * @param[in] dstPixFmt       pixel format of the returned image
 *
 * @returns                   a pointer to the image data
 */
static uint8_t *BufToRgb(cDrmBuffer *buf, int *size, int dstW, int dstH, enum AVPixelFormat dstPixFmt)
{
	uint8_t *srcData[4], *dstData[4];
	int srcLinesize[4], dstLinesize[4];

	int srcW = buf->Width();
	int srcH = buf->Height();

	enum AVPixelFormat src_pix_fmt = DrmFormatToAVFormat(buf);
	if (src_pix_fmt == AV_PIX_FMT_NONE) {
		LOGERROR("grab: %s: pixel format is not supported!", __FUNCTION__);
		return NULL;
	}

	int dstBufsize = 0;
	struct SwsContext *swsCtx;
	int ret;
	void *buffer = NULL;

	// planes aren't mmapped, return
	// this should be done before in VideoCloneBuf
	if (!buf->Plane(0)) {
		LOGERROR("grab: %s: prime data is not mapped!", __FUNCTION__);
		return NULL;
	}

	// convert yuv to rgb
	swsCtx = sws_getContext(srcW, srcH, src_pix_fmt,
	                        dstW, dstH, dstPixFmt,
	                        SWS_BILINEAR, NULL, NULL, NULL);
	if (!swsCtx) {
		LOGERROR("grab: %s: Could not create swsCtx", __FUNCTION__);
		munmap(buffer, buf->Size(0));
		return NULL;
	}

	if ((ret = av_image_alloc(dstData, dstLinesize, dstW, dstH, dstPixFmt, 1)) < 0) {
		LOGERROR("grab: %s: Could not alloc dst image", __FUNCTION__);
		munmap(buffer, buf->Size(0));
		sws_freeContext(swsCtx);
		return NULL;
	}
	dstBufsize = ret;

	// copy src pitches and data
	for (int i = 0; i < buf->NumPlanes(); i++) {
		srcLinesize[i] = buf->Pitch(i);
		srcData[i] = buf->Plane(i) + buf->Offset(i);
	}

	// scale image
	sws_scale(swsCtx,
	          (const uint8_t * const*)srcData, srcLinesize, 0, srcH,
	          dstData, dstLinesize);

	if (buffer)
		munmap(buffer, buf->Size(0));
	sws_freeContext(swsCtx);
	*size = dstBufsize;

	LOGDEBUG2(L_GRAB, "grab: %s: return image at %p size %d", __FUNCTION__, dstData[0], dstBufsize);
	return dstData[0];
}

/**
 * Scale an image
 *
 * Conversion is done with ffmpegs swscale
 *
 * @param[in] src               pointer to the source data
 * @param[out] size             size of the return data
 * @param[in] srcW              source width
 * @param[in] srcH              source height
 * @param[in] dstW              width of the returned image
 * @param[in] dstH              height of the returned image
 *
 * @returns                     a pointer to the converted image data
 */
static uint8_t *ScaleRgb24(uint8_t *src, int *size, int srcW, int srcH, int dstW, int dstH)
{
	struct SwsContext *swsCtx;
	int dstBufsize = 0;
	int ret;

	uint8_t *dstData[4];
	int dstLinesize[4];
	uint8_t *srcData[4] = {src, NULL, NULL, NULL};
	int srcLinesize[4] = {3 * srcW, 0, 0, 0};

	swsCtx = sws_getContext(srcW, srcH, AV_PIX_FMT_RGB24,
	                        dstW, dstH, AV_PIX_FMT_RGB24,
	                        SWS_BILINEAR, NULL, NULL, NULL);
	if (!swsCtx) {
		LOGERROR("grab: %s: Could not create swsCtx", __FUNCTION__);
		return NULL;
	}

	if ((ret = av_image_alloc(dstData, dstLinesize, dstW, dstH, AV_PIX_FMT_RGB24, 1)) < 0) {
		LOGERROR("grab: %s: Could not alloc dst image", __FUNCTION__);
		sws_freeContext(swsCtx);
		return NULL;
	}
	dstBufsize = ret;

	sws_scale(swsCtx,
	          (const uint8_t * const*)srcData, srcLinesize, 0, srcH,
	          dstData, dstLinesize);

	sws_freeContext(swsCtx);
	*size = dstBufsize;

	LOGDEBUG2(L_GRAB, "grab: %s: return scaled image at %p size %d", __FUNCTION__, dstData[0], dstBufsize);
	return dstData[0];
}

/**
 * Blend two images
 *
 * Both, front and back image data have to be same size
 * front is the OSD (ARGB)
 * back is the video (RGB)
 * result is RGB
 *
 * @param[out] result   pointer to the resulting image data
 * @param[in] front     pointer to the upper image data
 * @param[in] back      pointer to the lower image data
 * @param[in] width     image width
 * @param[in] height    image height
 */
static void AlphaBlend(uint8_t *result, uint8_t *front, uint8_t *back, const unsigned int width, const unsigned int height)
{
	for (unsigned long index = 0; index < width * height; index++) {
		const uint8_t frontAlpha = front[3];

		if (frontAlpha == TRANSPARENT) {
			for (int i = 0; i < 3; i++) {
				result[i] = back[i];
			}
			back += 3;
			front += 4;
			result += 3;
			continue;
		}

		if (frontAlpha == OPAQUE) {
			for (int i = 0; i < 3; i++) {
				result[i] = front[i];
			}
			back += 3;
			front += 4;
			result += 3;
			continue;
		}

		const uint8_t backR = back[0];
		const uint8_t backG = back[1];
		const uint8_t backB = back[2];

		const uint8_t frontR = UNMULTIPLY(front[0], frontAlpha);
		const uint8_t frontG = UNMULTIPLY(front[1], frontAlpha);
		const uint8_t frontB = UNMULTIPLY(front[2], frontAlpha);

		const uint8_t R = BLEND(backR, frontR, frontAlpha);
		const uint8_t G = BLEND(backG, frontG, frontAlpha);
		const uint8_t B = BLEND(backB, frontB, frontAlpha);

		result[0] = R;
		result[1] = G;
		result[2] = B;

		back += 3;
		front += 4;
		result += 3;
	}
}

/**
 * Blit the video on black background
 *
 * @param[in] dst      pointer to the destination video
 * @param[in] src      pointer to the source video
 * @param[in] dstW     destination width of the image
 * @param[in] dstH     destination height of the image
 * @param[in] dstX     x offset of the (already scaled) video on the image
 * @param[in] dstY     y offset of the (already scaled) video on the image
 * @param[in] srcW     source video width
 * @param[in] srcH     source video height
 *
 * @returns            0 on success, -1 on error
 */
static int BlitVideo(uint8_t *dst, uint8_t *src, int dstW, int dstH, int dstX, int dstY, int srcW, int srcH)
{
	int srcStride = srcW * 3;
	int dstStride = dstW * 3;

	if ((dstX + srcW > dstW) || (dstY + srcH > dstH)) {
		LOGDEBUG2(L_GRAB, "grab: %s: wrong dimensions, cropping not supported!", __FUNCTION__);
		return -1;
	}

	// blit the (scaled) image into dst
	for (int y = 0; y < srcH; y++) {
		memcpy(&dst[((dstY + y) * dstStride + dstX * 3)], &src[y * srcStride], srcStride);
	}

	return 0;
}

/*****************************************************************************
 * cGrabBuffer class
 ****************************************************************************/

/**
 * Free the grab buffer
 */
void cGrabBuffer::FreeDrmBuf(void)
{
	if (!m_pBuf)
		return;

	for (int plane = 0; plane < m_pBuf->NumPlanes(); plane++) {
		if (m_pBuf->Size(plane)) {
		LOGDEBUG2(L_GRAB, "grab: %s: free buf %p (plane %d)", __FUNCTION__, m_pBuf->Plane(plane), plane);
		free(m_pBuf->Plane(plane));
		}
	}
	delete m_pBuf;

	m_pBuf = nullptr;
}

/**
 * Set the grab buffer and the dimensions how it is presented on the screen
 */
void cGrabBuffer::SetDrmBuf(cDrmBuffer *buf)
{
	m_pBuf = buf;
	m_rect.Set(buf->GetScreenRect().Point(), buf->GetScreenRect().Size());
}

/*****************************************************************************
 * cSoftHdGrab class
 ****************************************************************************/

/**
 * Start a grab in the video renderer
 *
 * @param jpeg            flag true, create JPEG data
 * @param quality         JPEG quality
 * @param width           width of requested grab image
 * @param height          height of requested grab image
 * @param screenWidth     current screen width
 * @param screenHeight    current screen height
 */
bool cSoftHdGrab::Start(bool jpeg, int quality, int width, int height, int screenWidth, int screenHeight)
{
	if (width == 0 || height == 0) {
		LOGDEBUG2(L_GRAB, "grab: %s: width and/or height must not be 0!", __FUNCTION__);
		return false;
	}

	LOGDEBUG2(L_GRAB, "grab: starting grab for %s image (%dx%d, quality %d)", jpeg ? "jpg" : "pnm", width, height, quality);

	m_isJpeg = jpeg;
	m_screenWidth = screenWidth;
	m_screenHeight = screenHeight;

	// Set defaults
	m_quality = quality < 0 ? 95 : quality;
	m_grabbedWidth = width   > 0 ? width : screenWidth;
	m_grabbedHeight = height > 0 ? height : screenHeight;
	m_grabbedImage = nullptr;

	m_isActive = true;

	if (m_pRender->TriggerGrab()) {
		m_pRender->ClearGrabBuffers();
		m_isActive = false;
		LOGDEBUG2(L_GRAB, "grab: grabbing %s image (%dx%d, quality %d) failed", jpeg ? "jpg" : "pnm", width, height, quality);
		return false;
	}

	return ProcessGrab();
}

/**
 * Convert the cloned drm buffer data to RGB(void, pip) or ARGB (osd)
 * and returns a pointer to the raw data.
 *
 * @param[out] size            size of the grabbed buffer
 * @param[out] width           width of the grabbed buffer
 * @param[out] height          height of the grabbed buffer
 * @param[out] x               x offset of the grabbed buffer
 * @param[out] y               y offset of the grabbed buffer
 * @param[in]  buffer type     buffer type (Grabtype)
 *
 * @returns pointer to the raw buffer data
 */
uint8_t *cSoftHdGrab::GetGrab(int *size, int *width, int *height, int *x, int *y, Grabtype type)
{
	int psize = 0;
	cGrabBuffer *grab = nullptr;

	switch (type) {
		case Grabtype::GRABVIDEO:
			grab = m_pRender->GetGrabbedVideoBuffer();
			break;
		case Grabtype::GRABPIP:
			grab = m_pRender->GetGrabbedPipBuffer();
			break;
		case Grabtype::GRABOSD:
			grab = m_pRender->GetGrabbedOsdBuffer();
			break;
		default:
			LOGFATAL("grab: %s no valid type, bug!", __FUNCTION__);
	}

	cDrmBuffer *buf = grab->GetDrmBuf();

	// early return if buf = NULL
	if (!buf) {
		grab->SetData(NULL);
		grab->SetSize(0);
		return nullptr;
	}

	for (int plane = 0; plane < buf->NumPlanes(); plane++) {
		LOGDEBUG2(L_GRAB, "grab: %s: %s plane %d address %p pitch %d offset %d handle %d size %d", __FUNCTION__,
			   GrabtypeToString(type), plane, buf->Plane(plane), buf->Pitch(plane), buf->Offset(plane), buf->PrimeHandle(plane), buf->Size(plane));
	}
	// result's width and height are original dimensions how buffer is presented on the screen
	uint8_t * result = BufToRgb(buf, &psize, grab->GetWidth(), grab->GetHeight(), type == Grabtype::GRABOSD ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGB24);
	grab->SetData(result);
	grab->SetSize(psize);
	grab->FreeDrmBuf();

	if (size)
		*size = grab->GetSize();
	if (width)
		*width = grab->GetWidth();
	if (height)
		*height = grab->GetHeight();
	if (x)
		*x = grab->GetX();
	if (y)
		*y = grab->GetY();

	return grab->GetData();
}

/**
 * Call rgb to jpeg for C Plugin
 */
extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality,
	int width, int height)
{
	return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size, quality);
}

/**
 * Start the conversion
 *
 * This does the following:
 * 1) Get grabbed video data if available, otherwise create a black screen for video
 * 2) Get grabbed pip data if available
 * 3) Get grabbed osd data if available
 * 4) Blit the video data into a black background image if it is scaled
 * 5) If available, blit the pip data onto it
 * 6) If available, blit the osd data onto it respecting alpha values
 * 7) Scale the result to the user requested size
 * 8) Create a jpg or pnm image as requested
 */
bool cSoftHdGrab::ProcessGrab(void)
{
	int screenSize = m_screenWidth * m_screenHeight * 3; // we want a RGB24

	int videoSize = 0;                  // data size of the grabbed video
	int videoWidth = m_screenWidth;     // width of the grabbed video
	int videoHeight = m_screenHeight;   // height of the grabbed video
	int videoX = 0, videoY = 0;         // x, y of the grabbed video

	int pipSize = 0;                    // data size of the grabbed pip video
	int pipWidth = m_screenWidth;       // width of the grabbed pip video
	int pipHeight = m_screenHeight;     // height of the grabbed pip video
	int pipX = 0, pipY = 0;             // x, y of the grabbed pip video

	// fetch video data
	// Video comes as RGB, width and height is original screen dimension (video is maybe scaled)
	uint8_t *video = GetGrab(&videoSize, &videoWidth, &videoHeight, &videoX, &videoY, Grabtype::GRABVIDEO);
	if (!video) {
		LOGDEBUG2(L_GRAB, "grab: %s: no video data available, create black screen!", __FUNCTION__);
		video = (uint8_t *)calloc(1, screenSize);
	}

	// fetch pip data
	// Pip video comes as RGB, width and height is original screen dimension (video is maybe scaled)
	uint8_t *pip = GetGrab(&pipSize, &pipWidth, &pipHeight, &pipX, &pipY, Grabtype::GRABPIP);
	if (!pip)
		LOGDEBUG2(L_GRAB, "grab: %s: no pip data available, skip it", __FUNCTION__);

	// fetch osd data
	// OSD comes as ARGB, width and height is original screen dimension (osd is always fullscreen)
	uint8_t *osd = GetGrab(NULL, NULL, NULL, NULL, NULL, Grabtype::GRABOSD);
	if (!osd)
		LOGDEBUG2(L_GRAB, "grab: %s: no osd data available, skip it", __FUNCTION__);

	// blit the video into a full black screen if scaled
	uint8_t *videoResult = video;

	bool needsScaling = (videoWidth != m_screenWidth || videoHeight != m_screenHeight || videoX != 0 || videoY != 0);
	if (needsScaling) {
		videoResult = (uint8_t *)calloc(1, screenSize);
		if (BlitVideo(videoResult, video, m_screenWidth, m_screenHeight, videoX, videoY, videoWidth, videoHeight)) {
			free(videoResult);
			free(video);
			m_isActive = false;
			LOGDEBUG2(L_GRAB, "grab: grab failed during VIDEO blit");
			return false;
		}
		free(video);
	}

	// blit the pip video into the main video if available
	if (pip) {
		if (BlitVideo(videoResult, pip, m_screenWidth, m_screenHeight, pipX, pipY, pipWidth, pipHeight)) {
			free(videoResult);
			free(pip);
			m_isActive = false;
			LOGDEBUG2(L_GRAB, "grab: grab failed during PIP blit");
			return false;
		}
		free(pip);
	}

	// alphablend fullscreen video/pip with osd if available
	uint8_t *result = videoResult;
	if (osd) {
		result = (uint8_t *)malloc(screenSize);
		AlphaBlend(result, osd, videoResult, m_screenWidth, m_screenHeight);
		free(videoResult);
		free(osd);
	}

	// scale result to requested size width + height, if it differs from fullscreen
	uint8_t *scaledResult = result;
	int scaledSize = screenSize;

	needsScaling = (m_screenWidth != m_grabbedWidth || m_screenHeight != m_grabbedHeight);
	if (needsScaling) {
		scaledResult = ScaleRgb24(result, &scaledSize, m_screenWidth, m_screenHeight, m_grabbedWidth, m_grabbedHeight);
		free(result);
	}

	// make jpeg or pnm
	if (m_isJpeg) {
		m_grabbedImage = CreateJpeg(scaledResult, &m_grabbedSize, m_quality, m_grabbedWidth, m_grabbedHeight);
	} else {  // add header to raw data
		char buf[64];
		int n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", m_grabbedWidth, m_grabbedHeight);
		m_grabbedImage = (uint8_t *)malloc(scaledSize + n);
		memcpy(m_grabbedImage, buf, n);
		memcpy(m_grabbedImage + n, scaledResult, scaledSize);
		m_grabbedSize = scaledSize + n;
	}

	free(scaledResult);
	m_isActive = false;

	LOGDEBUG2(L_GRAB, "grab: finished %s image (%dx%d, quality %d) at %p (size %d)", m_isJpeg ? "jpg" : "pnm", m_grabbedWidth, m_grabbedHeight, m_isJpeg ? m_quality : 0, m_grabbedImage, m_grabbedSize);

	return true;
}
