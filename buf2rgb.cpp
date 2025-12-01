/**
 * @file buf2rgb.cpp
 * Some helper functions to convert and blit buffers
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

#include "buf2rgb.h"
#include "logger.h"
#include "drmbuffer.h"

#define OPAQUE      0xff
#define TRANSPARENT 0x00
#define UNMULTIPLY(color, alpha) ((0xff * color) / alpha)
#define BLEND(back, front, alpha) ((front * alpha) + (back * (255 - alpha))) / 255

/****************************************************************************************
 * Image data conversion helpers
 ***************************************************************************************/

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
uint8_t *BufToRgb(cDrmBuffer *buf, int *size, int dstW, int dstH, enum AVPixelFormat dstPixFmt)
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
uint8_t *ScaleRgb24(uint8_t *src, int *size, int srcW, int srcH, int dstW, int dstH)
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
void AlphaBlend(uint8_t *result, uint8_t *front, uint8_t *back, const unsigned int width, const unsigned int height)
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
int BlitVideo(uint8_t *dst, uint8_t *src, int dstW, int dstH, int dstX, int dstY, int srcW, int srcH)
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

/**
 * Print raw stream data
 *
 * @param data        pointer to stream data
 * @param size        data size
 */
void PrintStreamData(const uint8_t *data, int size)
{
	LOGDEBUG("Stream: %02x %02x %02x %02x %02x %02x %02x %02x %02x "
	         "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
	         "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x size %d",
	         data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8],
	         data[9], data[10], data[11], data[12], data[13], data[14], data[15], data[16], data[17],
	         data[18], data[19], data[20], data[21], data[22], data[23], data[24], data[25], data[26],
	         data[27], data[28], data[29], data[30], data[31], data[32], data[33], data[34], size);
}
