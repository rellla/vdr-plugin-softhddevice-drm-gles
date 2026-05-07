// SPDX-License-Identifier: AGPL-3.0-or-later AND BSD-3-Clause

/*
 * Portions of this file are derived from Raspberry Pi SAND conversion code:
 *
 * Copyright (c) 2018 Raspberry Pi (Trading) Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: John Cox
 */

/**
 * @file grab.cpp
 * Grabbing Interface
 *
 * This file defines cGrabBuffer and cSoftHdGrab, which are used
 * to handle grab requests.
 *
 * This file contains code under multiple licenses:
 * - Raspberry Pi code: BSD-3-Clause
 * - Remaining code: AGPL-3.0-or-later
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
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

/**
 * @addtogroup misc
 * @{
 */

/****************************************************************************************
 * Image data conversion helpers
 ***************************************************************************************/
#define OPAQUE      0xff
#define TRANSPARENT 0x00
#define UNMULTIPLY(color, alpha) ((0xff * color) / alpha)
#define BLEND(back, front, alpha) ((front * alpha) + (back * (255 - alpha))) / 255

// --- Begin Raspberry Pi BSD-licensed code ---

/**
 * Convert the luma (Y plane) from SAND to linear buffer (8bit)
 */
void Sand128ToPlanarY8(uint8_t *dst, const unsigned int dstStride, const uint8_t *src,
                       unsigned int stride1, unsigned int stride2,
                       unsigned int w, unsigned int h)
{
	const unsigned int mask = stride1 - 1;

	if (w < stride1) {
		// All in one sand stripe
		const uint8_t *p = src;
		for (unsigned int i = 0; i != h; ++i, dst += dstStride, p += stride1) {
			memcpy(dst, p, w);
		}
	} else {
		const unsigned int sstride = stride1 * stride2;
		const uint8_t *p1 = src;
		const uint8_t *p2 = p1 + sstride;
		const unsigned int w1 = stride1;
		const unsigned int w3 = w & mask;
		const unsigned int w2 = w - (w1 + w3);

		for (unsigned int i = 0; i != h; ++i, dst += dstStride, p1 += stride1, p2 += stride1) {
			unsigned int j;
			const uint8_t *p = p2;
			uint8_t *d = dst;
			memcpy(d, p1, w1);
			d += w1;
			for (j = 0; j < w2; j += stride1, d += stride1, p += sstride) {
				memcpy(d, p, stride1);
			}
			memcpy(d, p, w3);
		}
	}
}

/**
 * Convert the luma (Y plane) from SAND to linear buffer (10bit)
 */
static void Sand30ToPlanarY16(uint8_t *dst, const unsigned int dstStride,
                              const uint8_t *src,
                              unsigned int stride1, unsigned int stride2,
                              unsigned int w, unsigned int h)
{
	const unsigned int x1 = (w / 3) * 4;
	const unsigned int xrem1 = w - (x1 >> 2) * 3;
	const unsigned int mask = stride1 - 1;

	const uint8_t *p0 = src;
	const unsigned int sliceInc = ((stride2 - 1) * stride1) >> 2;

	if (x1 == 0)
		return;

	for (unsigned int i = 0; i != h; ++i, dst += dstStride, p0 += stride1) {
		unsigned int x = 0;
		const uint32_t *p = (const uint32_t *)p0;
		uint16_t *d = (uint16_t *)dst;

		while (x != x1) {
			const uint32_t p3 = *p++;
			*d++ = p3 & 0x3ff;
			*d++ = (p3 >> 10) & 0x3ff;
			*d++ = (p3 >> 20) & 0x3ff;

			if (((x += 4) & mask) == 0)
				p += sliceInc;
		}

		if (xrem1 != 0) {
			const uint32_t p3 = *p;

			*d++ = p3 & 0x3ff;
			if (xrem1 == 2)
				*d++ = (p3 >> 10) & 0x3ff;
		}
	}
}

/**
 * Convert the chroma (UV plane) from SAND to linear buffer (10bit)
 */
static void Sand30ToPlanarC16(uint8_t *dstU, const unsigned int dstStrideU,
                              uint8_t *dstV, const unsigned int dstStrideV,
                              const uint8_t *src,
                              unsigned int stride1, unsigned int stride2,
                              unsigned int w, unsigned int h)
{
	const unsigned int x1 = (w / 3) * 8;
	const unsigned int xrem1 = w - (x1 >> 3) * 3;
	const unsigned int mask = stride1 - 1;

	const uint8_t *p0 = src;
	const unsigned int sliceInc = ((stride2 - 1) * stride1) >> 2;

	if (x1 == 0)
		return;

	for (unsigned int i = 0; i != h; ++i, dstU += dstStrideU, dstV += dstStrideV, p0 += stride1) {
		unsigned int x = 0;
		const uint32_t *p = (const uint32_t *)p0;
		uint16_t *du = (uint16_t *)dstU;
		uint16_t *dv = (uint16_t *)dstV;

		while (x != x1) {
			const uint32_t p3a = *p++;
			const uint32_t p3b = *p++;

			*du++ = p3a & 0x3ff;
			*dv++ = (p3a >> 10) & 0x3ff;
			*du++ = (p3a >> 20) & 0x3ff;
			*dv++ = p3b & 0x3ff;
			*du++ = (p3b >> 10) & 0x3ff;
			*dv++ = (p3b >> 20) & 0x3ff;

			if (((x += 8) & mask) == 0)
				p += sliceInc;
		}

		if (xrem1 != 0) {
			const uint32_t p3a = *p++;
			const uint32_t p3b = *p++;

			*du++ = p3a & 0x3ff;
			*dv++ = (p3a >> 10) & 0x3ff;
			if (xrem1 == 2) {
				*du++ = (p3a >> 20) & 0x3ff;
				*dv++ = p3b & 0x3ff;
			}
		}
	}
}
// --- End Raspberry Pi BSD-licensed code ---

/**
 * Convert a DRM format to a ffmpeg AV format
 */
enum AVPixelFormat DrmFormatToAVFormat(uint32_t pixFmt)
{
	switch (pixFmt) {
		case DRM_FORMAT_NV12:
			return AV_PIX_FMT_NV12;
		case DRM_FORMAT_YUV420:
			return AV_PIX_FMT_YUV420P;
		case DRM_FORMAT_ARGB8888:
			return AV_PIX_FMT_RGBA;
		case DRM_FORMAT_P030:
			return AV_PIX_FMT_YUV420P10;
		default:
			return AV_PIX_FMT_NONE;
	}

	return AV_PIX_FMT_NONE;
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
 * @return                      a pointer to the converted image data
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
		LOGERROR("%s: Could not create swsCtx", __FUNCTION__);
		return NULL;
	}

	if ((ret = av_image_alloc(dstData, dstLinesize, dstW, dstH, AV_PIX_FMT_RGB24, 1)) < 0) {
		LOGERROR("%s: Could not alloc dst image", __FUNCTION__);
		sws_freeContext(swsCtx);
		return NULL;
	}
	dstBufsize = ret;

	sws_scale(swsCtx,
	          (const uint8_t * const*)srcData, srcLinesize, 0, srcH,
	          dstData, dstLinesize);

	sws_freeContext(swsCtx);
	*size = dstBufsize;

	LOGDEBUG2(L_GRAB, "%s: return scaled image at %p size %d", __FUNCTION__, dstData[0], dstBufsize);
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
 * @return             0 on success, -1 on error
 */
static int BlitVideo(uint8_t *dst, uint8_t *src, int dstW, int dstH, int dstX, int dstY, int srcW, int srcH)
{
	int srcStride = srcW * 3;
	int dstStride = dstW * 3;

	if ((dstX + srcW > dstW) || (dstY + srcH > dstH)) {
		LOGDEBUG2(L_GRAB, "%s: wrong dimensions, cropping not supported!", __FUNCTION__);
		return -1;
	}

	// blit the (scaled) image into dst
	for (int y = 0; y < srcH; y++) {
		memcpy(&dst[((dstY + y) * dstStride + dstX * 3)], &src[y * srcStride], srcStride);
	}

	return 0;
}

/**
 * Call rgb to jpeg for C Plugin
 */
extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality,
	int width, int height)
{
	return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size, quality);
}

/** @} */

/*****************************************************************************
 * cGrabBuffer class
 ****************************************************************************/

/**
 * Free the grab input buffer
 */
void cGrabBuffer::FreeInput(void)
{
	// early return, because the grab buffer wasn't set
	if (m_outputRect.IsEmpty())
		return;

	// free the allocated memory
	for (int plane = 0; plane < m_numPlanes; plane++) {
		if (m_size[plane]) {
			LOGDEBUG2(L_GRAB, "%s: %s: free buf %p (plane %d)", m_identifier, __FUNCTION__, m_pPlane[plane], plane);
			free(m_pPlane[plane]);
		}
	}

	// reset member variables
	m_width = 0;
	m_height = 0;
	m_pixFmt = 0;
	m_modifier = 0;
	m_numPlanes = 0;

	for (int i = 0; i < 4; i++) {
		m_offset[i] = 0;
		m_pitch[i] = 0;
		m_size[i] = 0;
		m_pPlane[i] = nullptr;
	}
}

/**
 * Clear the grab buffer (input and output data)
 */
void cGrabBuffer::Clear(void)
{
	FreeInput(); // input should already be freed and reset after ConvertToRgb

	m_outputRect.Set(0, 0, 0, 0);
	m_outputSize = 0;
	m_pOutputData = nullptr; // result needs to be freed by the caller of GetGrabbedData()
}

/**
 * Set the grab buffer and the dimensions how it is presented on the screen
 *
 * @param src          original drm buffer, where the parameters and data is copied from
 */
void cGrabBuffer::Set(cDrmBuffer *src)
{
	if (src->GetScreenRect().IsEmpty())
		return;

	void *src_buffer = nullptr;
	void *dst_buffer = nullptr;

	// planes aren't mmapped, do it (PRIME)
	if (!src->Plane(0)) {
		for (int object = 0; object < src->NumObjects(); object++) {

			// memcpy mmapped data
			dst_buffer = malloc(src->Size(object));
			if (!dst_buffer) {
				LOGERROR("%s: %s: cannot allocate destination buffer (%d): %m", m_identifier, __FUNCTION__, errno);
				// @todo handle possible leaks of previous objects
				return;
			}

			src_buffer = mmap(NULL, src->Size(object), PROT_READ, MAP_SHARED, src->DmaBufHandle(object), 0);
			if (src_buffer == MAP_FAILED) {
				free(dst_buffer);
				LOGERROR("%s: %s: cannot map buffer size %d prime_fd %d (%d): %m",
					m_identifier, __FUNCTION__, src->Size(object), src->DmaBufHandle(object), errno);
				// @todo handle possible leaks of previous objects
				return;
			}

			memcpy(dst_buffer, src_buffer, src->Size(object));
			munmap(src_buffer, src->Size(object));

			for (int plane = 0; plane < src->NumPlanes(); plane++) {
				if (src->ObjectIndex(plane) == object)
					m_pPlane[plane] = (uint8_t *)dst_buffer;
			}
		}
	} else {
		for (int plane = 0; plane < src->NumPlanes(); plane++) {
			dst_buffer = malloc(src->Size(plane));
			if (!dst_buffer) {
				LOGERROR("%s: %s: cannot allocate destination buffer (%d): %m", m_identifier, __FUNCTION__, errno);
				return;
			}
			memcpy(dst_buffer, src->Plane(plane), src->Size(plane));
			m_pPlane[plane] = (uint8_t *)dst_buffer;
		}
	}

	m_width = src->Width();
	m_height = src->Height();
	m_pixFmt = src->PixFmt();
	m_modifier = src->Modifier();
	m_numPlanes = src->NumPlanes();

	for (int i = 0; i < src->NumPlanes(); i++) {
		m_offset[i] = src->Offset(i);
		m_pitch[i] = src->Pitch(i);
		m_size[i] = src->Size(i);
	}

	m_outputRect.Set(src->GetScreenRect().Point(), src->GetScreenRect().Size());

	LOGDEBUG2(L_GRAB, "%s: %s: Set input buffer: %dx%d pixFmt %d modifier %d num planes %d with %dx%d at %d|%d",
		m_identifier, __FUNCTION__, m_width, m_height, m_pixFmt, m_modifier, m_numPlanes, m_outputRect.Width(), m_outputRect.Height(), m_outputRect.X(), m_outputRect.Y());
	for (int plane = 0; plane < m_numPlanes; plane++) {
		LOGDEBUG2(L_GRAB, "%s: %s: Copied plane %d address %p pitch %d offset %d size %d",
			m_identifier, __FUNCTION__, plane, m_pPlane[plane], m_pitch[plane], m_offset[plane], m_size[plane]);
	}
}

/**
 * Convert a grabbed buffer to rgb format image
 *
 * Conversion is done with ffmpegs swscale
 *
 * @param[out] size           size of the return data
 *
 * @return                    a pointer to the image data
 */
uint8_t *cGrabBuffer::ConvertToRgb(int *size)
{
	uint8_t *srcData[4] = {nullptr};
	uint8_t *dstData[4] = {nullptr};;
	int srcLinesize[4] = {0};
	int dstLinesize[4] = {0};
	int dstW = GetOutputWidth();
	int dstH = GetOutputHeight();
	enum AVPixelFormat dstPixFmt = strcmp(m_identifier, "OSD") == 0 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGB24;

	int srcW = m_width;
	int srcH = m_height;

	enum AVPixelFormat srcPixFmt = DrmFormatToAVFormat(m_pixFmt);
	if (srcPixFmt == AV_PIX_FMT_NONE) {
		LOGERROR("%s: %s: pixel format is not supported!", m_identifier, __FUNCTION__);
		return NULL;
	}

	int dstBufsize = 0;
	struct SwsContext *swsCtx;
	int ret;

	// planes aren't mmapped, return
	if (!m_pPlane[0]) {
		LOGERROR("%s: %s: prime data is not mapped!", m_identifier, __FUNCTION__);
		return NULL;
	}

	// convert yuv to rgb
	swsCtx = sws_getContext(srcW, srcH, srcPixFmt,
	                        dstW, dstH, dstPixFmt,
	                        SWS_BILINEAR, NULL, NULL, NULL);
	if (!swsCtx) {
		LOGERROR("%s: %s: Could not create swsCtx", m_identifier, __FUNCTION__);
		return NULL;
	}

	if ((ret = av_image_alloc(dstData, dstLinesize, dstW, dstH, dstPixFmt, 1)) < 0) {
		LOGERROR("%s: %s: Could not alloc dst image", m_identifier, __FUNCTION__);
		sws_freeContext(swsCtx);
		return NULL;
	}
	dstBufsize = ret;

	// De-tile sand format
	uint8_t *tmpData[4] = {0};
	int tmpLinesize[4] = {0};
	int tmpImgSize = 0;
	if (m_modifier == DRM_FORMAT_MOD_BROADCOM_SAND128) {
		if (m_pixFmt == DRM_FORMAT_NV12) {
			int stride1 = 128;

			tmpImgSize = av_image_alloc(tmpData, tmpLinesize, srcW, srcH, srcPixFmt, 1);
			if (tmpImgSize < 0) {
				LOGERROR("%s: %s: Could not alloc tmp image", m_identifier, __FUNCTION__);
				sws_freeContext(swsCtx);
				return NULL;
			}

			Sand128ToPlanarY8(tmpData[0], tmpLinesize[0],
			                  m_pPlane[0] + m_offset[0],
			                  stride1, m_pitch[0],
			                  srcW, srcH);
			Sand128ToPlanarY8(tmpData[1], tmpLinesize[1],
			                  m_pPlane[1] + m_offset[1],
			                  stride1, m_pitch[1],
			                  srcW, srcH / 2);

			srcData[0] = tmpData[0];
			srcData[1] = tmpData[1];
			srcLinesize[0] = tmpLinesize[0];
			srcLinesize[1] = tmpLinesize[1];
		} else if (m_pixFmt == DRM_FORMAT_P030) {
			int stride1 = 128;

			tmpImgSize = av_image_alloc(tmpData, tmpLinesize, srcW, srcH, srcPixFmt, 1);
			if (tmpImgSize < 0) {
				LOGERROR("%s: %s: Could not alloc tmp image", m_identifier, __FUNCTION__);
				sws_freeContext(swsCtx);
				return NULL;
			}

			Sand30ToPlanarY16(tmpData[0], tmpLinesize[0],
			                  m_pPlane[0] + m_offset[0],
			                  stride1, m_pitch[0],
			                  srcW, srcH);
			Sand30ToPlanarC16(tmpData[1], tmpLinesize[1],
			                  tmpData[2], tmpLinesize[2],
			                  m_pPlane[1] + m_offset[1],
			                  stride1, m_pitch[1],
			                  srcW / 2, srcH / 2);

			srcData[0] = tmpData[0];
			srcData[1] = tmpData[1];
			srcData[2] = tmpData[2];
			srcLinesize[0] = tmpLinesize[0];
			srcLinesize[1] = tmpLinesize[1];
			srcLinesize[2] = tmpLinesize[2];
		}
	} else {
		// copy src pitches and data
		for (int i = 0; i < m_numPlanes; i++) {
			srcLinesize[i] = m_pitch[i];
			srcData[i] = m_pPlane[i] + m_offset[i];
		}
	}

	// scale image
	sws_scale(swsCtx, (const uint8_t * const*)srcData, srcLinesize, 0, srcH, dstData, dstLinesize);

	sws_freeContext(swsCtx);
	*size = dstBufsize;

	if (tmpImgSize > 0)
		av_freep(&tmpData[0]);

	LOGDEBUG2(L_GRAB, "%s: %s: return image at %p size %d", m_identifier, __FUNCTION__, dstData[0], dstBufsize);

	return dstData[0];
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
	m_active = true;

	LOGDEBUG2(L_GRAB, "Starting grab for %s image (%dx%d, quality %d)", jpeg ? "jpg" : "pnm", width, height, quality);

	// always clear the buffers in case sth. unexpected happend
	m_pRender->ClearGrabBuffers();

	m_isJpeg = jpeg;
	m_screenWidth = screenWidth;
	m_screenHeight = screenHeight;

	// Set defaults
	m_quality = quality < 0 ? 95 : quality;
	m_grabbedWidth = width   > 0 ? width : screenWidth;
	m_grabbedHeight = height > 0 ? height : screenHeight;
	m_grabbedImage = nullptr;

	if (m_pRender->TriggerGrab()) {
		Finish();
		LOGDEBUG2(L_GRAB, "Grabbing %s image (%dx%d, quality %d) failed", jpeg ? "jpg" : "pnm", width, height, quality);
		return false;
	}

	return true;
}

/**
 * Clean up
 *
 * Clears the dedicated buffers in the renderer again and
 * sets the grab inactive.
 */
void cSoftHdGrab::Finish(void)
{
	m_pRender->ClearGrabBuffers();
	m_active = false;
}

uint8_t *cSoftHdGrab::GetGrabbedVideoData(int *size, int *width, int *height, int *x, int *y)
{
	cGrabBuffer *grab = m_pRender->GetGrabbedVideoBuffer();
	return GetGrabbedData(size, width, height, x, y, grab);
}

uint8_t *cSoftHdGrab::GetGrabbedPipData(int *size, int *width, int *height, int *x, int *y)
{
	cGrabBuffer *grab = m_pRender->GetGrabbedPipBuffer();
	return GetGrabbedData(size, width, height, x, y, grab);
}

uint8_t *cSoftHdGrab::GetGrabbedOsdData(int *size, int *width, int *height, int *x, int *y)
{
	cGrabBuffer *grab = m_pRender->GetGrabbedOsdBuffer();
	return GetGrabbedData(size, width, height, x, y, grab);
}


/**
 * Convert the cloned drm buffer data to RGB(void, pip) or ARGB (osd)
 * and return a pointer to the raw data.
 *
 * @param[out] size            size of the grabbed buffer
 * @param[out] width           width of the grabbed buffer
 * @param[out] height          height of the grabbed buffer
 * @param[out] x               x offset of the grabbed buffer
 * @param[out] y               y offset of the grabbed buffer
 * @param[in]  grab            grabbed buffer
 *
 * @return a pointer to the raw buffer data
 */
uint8_t *cSoftHdGrab::GetGrabbedData(int *size, int *width, int *height, int *x, int *y, cGrabBuffer *grab)
{
	int psize = 0;

	// early return if no grab buffer is set
	if (!grab->IsSet()) {
		grab->SetOutputData(NULL);
		grab->SetOutputSize(0);
		return nullptr;
	}

	// result's width and height are original dimensions how buffer is presented on the screen
	uint8_t *result = grab->ConvertToRgb(&psize);
	grab->FreeInput();

	grab->SetOutputData(result);
	grab->SetOutputSize(psize);

	if (size)
		*size = grab->GetOutputSize();
	if (width)
		*width = grab->GetOutputWidth();
	if (height)
		*height = grab->GetOutputHeight();
	if (x)
		*x = grab->GetOutputX();
	if (y)
		*y = grab->GetOutputY();

	return grab->GetOutputData();
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
 *
 * @return true, if grab succeeded, false otherwise
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
	uint8_t *video = GetGrabbedVideoData(&videoSize, &videoWidth, &videoHeight, &videoX, &videoY);
	if (!video) {
		LOGDEBUG2(L_GRAB, "%s: no video data available, create black screen!", __FUNCTION__);
		video = (uint8_t *)calloc(1, screenSize);
	}

	// fetch pip data
	// Pip video comes as RGB, width and height is original screen dimension (video is maybe scaled)
	uint8_t *pip = GetGrabbedPipData(&pipSize, &pipWidth, &pipHeight, &pipX, &pipY);
	if (!pip)
		LOGDEBUG2(L_GRAB, "%s: no pip data available, skip it", __FUNCTION__);

	// fetch osd data
	// OSD comes as ARGB, width and height is original screen dimension (osd is always fullscreen)
	uint8_t *osd = GetGrabbedOsdData(NULL, NULL, NULL, NULL, NULL);
	if (!osd)
		LOGDEBUG2(L_GRAB, "%s: no osd data available, skip it", __FUNCTION__);

	// blit the video into a full black screen if scaled
	uint8_t *videoResult = video;

	bool needsScaling = (videoWidth != m_screenWidth || videoHeight != m_screenHeight || videoX != 0 || videoY != 0);
	if (needsScaling) {
		videoResult = (uint8_t *)calloc(1, screenSize);
		if (BlitVideo(videoResult, video, m_screenWidth, m_screenHeight, videoX, videoY, videoWidth, videoHeight)) {
			free(videoResult);
			free(video);
			Finish();
			LOGDEBUG2(L_GRAB, "grab failed during VIDEO blit");
			return false;
		}
		free(video);
	}

	// blit the pip video into the main video if available
	if (pip) {
		if (BlitVideo(videoResult, pip, m_screenWidth, m_screenHeight, pipX, pipY, pipWidth, pipHeight)) {
			free(videoResult);
			free(pip);
			Finish();
			LOGDEBUG2(L_GRAB, "grab failed during PIP blit");
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
	LOGDEBUG2(L_GRAB, "Finished %s image (%dx%d, quality %d) at %p (size %d)", m_isJpeg ? "jpg" : "pnm", m_grabbedWidth, m_grabbedHeight, m_isJpeg ? m_quality : 0, m_grabbedImage, m_grabbedSize);

	return true;
}
