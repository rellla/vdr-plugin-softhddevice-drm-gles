// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file softhdosd.cpp
 * Software OSD
 *
 * This file provides cSoftOsd which is the software accelerated
 * version of this plugin (in contrast to the hardware accelerater cOglOsd).
 * It also decribes cSoftOsdProvider.
 *
 * @copyright 2011, 2015 by Johns.  All Rights Reserved.
 * @copyright 2018 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include "logger.h"
#include "softhddevice.h"
#include "softhdosd.h"

/**
 * Software Based OSD (CPU)
 *
 * @addtogroup osd
 * @{
 */

/**
 * Initializes a software based OSD with the given coordinates
 *
 * @param left     x-coordinate of osd on display
 * @param top      y-coordinate of osd on display
 * @param level    level of the osd (smallest is shown)
 * @param device   pointer to cSoftHdDevice object
 */
cSoftOsd::cSoftOsd(int left, int top, uint level, cSoftHdDevice *device)
	: cOsd(left, top, level),
	  m_pDevice(device),
	  m_osdLevel(level)
{
	LOGDEBUG2(L_OSD, "osd: %s: %dx%d%+d%+d, %d", __FUNCTION__, OsdWidth(), OsdHeight(), left, top, level);
}

/**
 * Shut down the OSD
 */
cSoftOsd::~cSoftOsd(void)
{
	LOGDEBUG2(L_OSD, "osd: %s: level %d", __FUNCTION__, m_osdLevel);

	SetActive(false);	// done by SetActive(): OsdClose();
}

/**
 * Sets this OSD to be the active one
 *
 * @param on          true on, false off
 *
 * @note only needed as workaround for text2skin plugin with undrawn areas
 */
void cSoftOsd::SetActive(bool on)
{
	LOGDEBUG2(L_OSD, "osd: %s: %d level %d", __FUNCTION__, on, m_osdLevel);

	if (Active() == on)
		return;		// already active, no action

	cOsd::SetActive(on);

	if (on) {
		m_dirty = true;
		// only flush here if there are already bitmaps
		if (GetBitmap(0)) {
			Flush();
		}
	} else
		m_pDevice->OsdClose();
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

	if (!Active())		// this osd is not active
		return;

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

/** @} */
