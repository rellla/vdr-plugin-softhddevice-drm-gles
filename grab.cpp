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

#include "logger.h"
#include "grab.h"
#include "buf2rgb.h"
#include "videorender.h"

/*****************************************************************************
 * cGrabBuffer class
 ****************************************************************************/

/**
 * Grab buffer class constructor
 */
cGrabBuffer::cGrabBuffer(void)
{
	m_pBuf = NULL;
	m_pResult = NULL;

	cRect m_rect;
	m_size = 0;
}

/**
 * Grab buffer class destructor
 */
cGrabBuffer::~cGrabBuffer(void)
{
}

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

/*****************************************************************************
 * cSoftHdGrab class
 ****************************************************************************/

/**
 * Grabber class constructor
 */
cSoftHdGrab::cSoftHdGrab(cVideoRender *render)
{
	m_pRender = render;
}

/**
 * Grabber class destructor
 */
cSoftHdGrab::~cSoftHdGrab(void)
{
}

/**
 * Start a grab in the video renderer
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
 * Return a pointer to the grabbed data and get dimensions
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
