/**
 * @file softosd.cpp
 * Osd class
 *
 * This file provides cSoftOsd which is the software accelerated
 * version of this plugin (in contrast to the hardware accelerater cOglOsd).
 * It also decribes cSoftOsdProvider.
 *
 * @copyright (c) 2011, 2015 by Johns.  All Rights Reserved.
 * @copyright (c) 2018 zille.  All Rights Reserved.
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
#ifdef USE_GLES
#include "openglosd.h"
#endif
#include "softhddevice.h"
#include "softhdosd.h"

/*****************************************************************************
 * OSD (software)
 ****************************************************************************/

/**
 * cSoftOsd constructor
 *
 * Initializes the OSD with the given coordinates.
 *
 * @param left     x-coordinate of osd on display
 * @param top      y-coordinate of osd on display
 * @param level    level of the osd (smallest is shown)
 * @param device   pointer to cSoftHdDevice object
 */
cSoftOsd::cSoftOsd(int left, int top, uint level, cSoftHdDevice *device)
:cOsd(left, top, level)
{
	// FIXME: OsdWidth/OsdHeight not correct!
	LOGDEBUG2(L_OSD, "osd: %s: %dx%d%+d%+d, %d", __FUNCTION__, OsdWidth(), OsdHeight(), left, top, level);

	m_pDevice = device;
	m_osdLevel = level;
}

/**
 * cSoftOsd destructor
 *
 * Shuts down the OSD.
 */
cSoftOsd::~cSoftOsd(void)
{
	LOGDEBUG2(L_OSD, "osd: %s: level %d", __FUNCTION__, m_osdLevel);

	SetActive(false);	// done by SetActive(): OsdClose();
}

/**
 *	Sets this OSD to be the active one.
 *
 *	@param on          true on, false off
 *
 *	@note only needed as workaround for text2skin plugin with
 *	undrawn areas.
 */
void cSoftOsd::SetActive(bool on)
{
	LOGDEBUG2(L_OSD, "osd: %s: %d level %d", __FUNCTION__, on, m_osdLevel);

	if (Active() == on) {
		return;				// already active, no action
	}
	cOsd::SetActive(on);

	if (on) {
		m_dirty = true;
		// only flush here if there are already bitmaps
		if (GetBitmap(0)) {
			Flush();
		}
	} else {
		m_pDevice->OsdClose();
	}
}

/**
 * Set the sub-areas to the given areas
 */
eOsdError cSoftOsd::SetAreas(const tArea * areas, int n)
{
	LOGDEBUG2(L_OSD, "osd: %s: %d areas", __FUNCTION__, n);

	// clear old OSD, when new areas are set
	if (!IsTrueColor()) {
		cBitmap *bitmap;
		int i;

		for (i = 0; (bitmap = GetBitmap(i)); i++)
			bitmap->Clean();
	}

	if (Active()) {
		m_pDevice->OsdClose();
		m_dirty = true;
	}

	return cOsd::SetAreas(areas, n);
}

/**
 * Actually commit all data to the OSD hardware
 */
void cSoftOsd::Flush(void)
{
	cPixmapMemory *pm;

	LOGDEBUG2(L_OSD, "osd: %s: level %d active %d", __FUNCTION__, m_osdLevel,
	Active());

	if (!Active()) {	// this osd is not active
		return;
	}

	if (!IsTrueColor()) {
		cBitmap *bitmap;
		int i;

		static char warned;

		if (!warned) {
			LOGDEBUG2(L_OSD, "osd: %s: FIXME: should be truecolor", __FUNCTION__);
			warned = 1;
		}

		// draw all bitmaps
		for (i = 0; (bitmap = GetBitmap(i)); ++i) {
			uint8_t *argb;
			int xs;
			int ys;
			int x;
			int y;
			int w;
			int h;
			int x1;
			int y1;
			int x2;
			int y2;

			// get dirty bounding box
			if (m_dirty) {		// forced complete update
				x1 = 0;
				y1 = 0;
				x2 = bitmap->Width() - 1;
				y2 = bitmap->Height() - 1;
			} else if (!bitmap->Dirty(x1, y1, x2, y2)) {
				continue;		// nothing dirty continue
			}

			// convert and upload only visible dirty areas
			xs = bitmap->X0() + Left();
			ys = bitmap->Y0() + Top();

			// FIXME: negtative position bitmaps
			w = x2 - x1 + 1;
			h = y2 - y1 + 1;

			// clip to screen
			int width;
			int height;
			double videoAspect;

			if (xs < 0) {
				if (xs + x1 < 0) {
					x1 -= xs + x1;
					w += xs + x1;
					if (w <= 0)
						continue;
				}
				xs = 0;
			}

			if (ys < 0) {
				if (ys + y1 < 0) {
					y1 -= ys + y1;
					h += ys + y1;
					if (h <= 0)
						continue;
				}
				ys = 0;
			}

			m_pDevice->GetOsdSize(width, height, videoAspect);
			if (w > width - xs - x1) {
				w = width - xs - x1;
				if (w <= 0)
					continue;
				x2 = x1 + w - 1;
			}

			if (h > height - ys - y1) {
				h = height - ys - y1;
				if (h <= 0)
					continue;
				y2 = y1 + h - 1;
			}

			if (w > bitmap->Width() || h > bitmap->Height())
				LOGDEBUG2(L_OSD, "osd: %s: dirty area too big", __FUNCTION__);

			argb = (uint8_t *) malloc(w * h * sizeof(uint32_t));
			for (y = y1; y <= y2; ++y) {
				for (x = x1; x <= x2; ++x) {
					((uint32_t *) argb)[x - x1 + (y - y1) * w] =
					bitmap->GetColor(x, y);
				}
			}
			LOGDEBUG2(L_OSD, "osd: %s: draw %dx%d%+d%+d bm", __FUNCTION__, w, h, xs + x1, ys + y1);
			m_pDevice->OsdDrawARGB(0, 0, w, h, w * sizeof(uint32_t), argb, xs + x1, ys + y1);

			bitmap->Clean();

			// FIXME: reuse argb
			free(argb);
		}
		m_dirty = false;
		return;
	}

	LOCK_PIXMAPS;
	while ((pm = (dynamic_cast < cPixmapMemory * >(RenderPixmaps())))) {
		int xp;
		int yp;
		int stride;
		int x;
		int y;
		int w;
		int h;

		x = pm->ViewPort().X();
		y = pm->ViewPort().Y();
		w = pm->ViewPort().Width();
		h = pm->ViewPort().Height();
		stride = w * sizeof(tColor);

		// clip to osd
		xp = 0;
		if (x < 0) {
			xp = -x;
			w -= xp;
			x = 0;
		}

		yp = 0;
		if (y < 0) {
			yp = -y;
			h -= yp;
			y = 0;
		}

		if (w > Width() - x)
			w = Width() - x;
		if (h > Height() - y)
			h = Height() - y;

		x += Left();
		y += Top();

		// clip to screen
		int width;
		int height;
		double videoAspect;

		if (x < 0) {
			w += x;
			xp += -x;
			x = 0;
		}
		if (y < 0) {
			h += y;
			yp += -y;
			y = 0;
		}

		m_pDevice->GetOsdSize(width, height, videoAspect);
		if (w > width - x)
			w = width - x;
		if (h > height - y)
			h = height - y;

		LOGDEBUG2(L_OSD, "osd: %s: draw %dx%d%+d%+d*%d -> %+d%+d %p", __FUNCTION__, w, h, xp, yp, stride, x, y, pm->Data());
		m_pDevice->OsdDrawARGB(xp, yp, w, h, stride, pm->Data(), x, y);

		DestroyPixmap(pm);
	}
	m_dirty = false;
}

/*****************************************************************************
 * OSD provider
 ****************************************************************************/

/**
 * cOsdProvider constructor
 */
cSoftOsdProvider::cSoftOsdProvider(cSoftHdDevice *device) : cOsdProvider()
{
	LOGDEBUG2(L_OSD, "osdprovider: %s:", __FUNCTION__);
	m_pDevice = device;

#ifdef USE_GLES
	if (!m_pDevice->OglOsdIsDisabled())
		StopOpenGlThread();
#endif
}

/**
 * cOsdProvider destructor
 */
cSoftOsdProvider::~cSoftOsdProvider()
{
	LOGDEBUG2(L_OSD, "osdprovider %s:", __FUNCTION__);
#ifdef USE_GLES
	if (!m_pDevice->OglOsdIsDisabled())
		StopOpenGlThread();
#endif
}

/**
 * Create a new OSD
 *
 * Create either a hardware accelerated (cOglOsd) or software rendered (cSoftOsd) OSD
 * 
 * @param left   x-coordinate of OSD
 * @param top    y-coordinate of OSD
 * @param level  layer level of OSD
 */
cOsd *cSoftOsdProvider::CreateOsd(int left, int top, uint level)
{
#ifdef USE_GLES
	if (m_pDevice->OglOsdIsDisabled()) {
		LOGDEBUG("osdprovider: %s: %d, %d, %d, OpenGL disabled, using software rendering", __FUNCTION__, left, top, level);
		return m_pOsd = new cSoftOsd(left, top, level, m_pDevice);
	}

	if (StartOpenGlThread()) {
		LOGDEBUG2(L_OSD, "osdprovider: %s: %d, %d, %d, using OpenGL OSD support", __FUNCTION__, left, top, level);
		return m_pOsd = new cOglOsd(left, top, level, m_pOglThread, m_pDevice);
	}

	LOGDEBUG("osdprovider: %s: %d, %d, %d, OpenGL failed, using software rendering", __FUNCTION__, left, top, 999);
	m_pDevice->SetDisableOglOsd();
	return m_pOsd = new cSoftOsd(left, top, 999, m_pDevice);
#else
	LOGDEBUG2(L_OSD, "osdprovider: %s: %d, %d, %d", __FUNCTION__, left, top, level);
	return m_pOsd = new cSoftOsd(left, top, level, m_pDevice);
#endif
}

/**
 * Check if this OSD provider is able to handle a true color OSD.
 *
 * @returns true we are able to handle a true color OSD.
 */
bool cSoftOsdProvider::ProvidesTrueColor(void)
{
	return true;
}

#ifdef USE_GLES
/**
 * Stop the OpenGL thread, if the osd size changed and update the size
 */
void cSoftOsdProvider::OsdSizeChanged(void) {
	// cleanup OpenGL context
	if (!m_pDevice->OglOsdIsDisabled())
		cSoftOsdProvider::StopOpenGlThread();
	cSoftOsdProvider::UpdateOsdSize();
}

/**
 * Start the OpenGL thread
 */
bool cSoftOsdProvider::StartOpenGlThread(void) {
	if (m_pDevice->OglOsdIsDisabled()) {
		LOGDEBUG2(L_OPENGL, "osdprovider: %s: OpenGL OSD disabled, OpenGL worker thread NOT started", __FUNCTION__);
		return false;
	}

	if (m_pOglThread.get()) {
		if (m_pOglThread->Active()) {
			return true;
		}
		m_pOglThread.reset();
	}
	cCondWait wait;
	LOGDEBUG2(L_OPENGL, "osdprovider: %s: Trying to start OpenGL worker thread", __FUNCTION__);
	m_pOglThread.reset(new cOglThread(&wait, m_pDevice->MaxSizeGPUImageCache(), m_pDevice));
	wait.Wait();

	if (m_pOglThread->Active()) {
		LOGINFO("OpenGL worker thread started");
		return true;
	}

	LOGDEBUG2(L_OPENGL, "osdprovider: %s: OpenGL worker thread NOT started", __FUNCTION__);
	return false;
}

/**
 * Stop the OpenGL thread
 */
void cSoftOsdProvider::StopOpenGlThread(void) {
	LOGDEBUG2(L_OPENGL, "osdprovider: %s: stopping OpenGL worker thread", __FUNCTION__);
	if (m_pOglThread) {
		m_pOglThread->Stop();
	}
	m_pOglThread.reset();
	LOGINFO("OpenGL worker thread stopped");
}

/**
 * Store image data
 */
int cSoftOsdProvider::StoreImageData(const cImage &Image)
{
	if (StartOpenGlThread()) {
		int imgHandle = m_pOglThread->StoreImage(Image);
		return imgHandle;
	}
	return 0;
}

/**
 * Get stored image data
 */
const cImage *cSoftOsdProvider::GetImageData(int ImageHandle) {
	return cOsdProvider::GetImageData(ImageHandle);
}

/**
 * Drop stored image data
 */
void cSoftOsdProvider::DropImageData(int imgHandle)
{
	if (StartOpenGlThread())
		m_pOglThread->DropImageData(imgHandle);
}
#endif
