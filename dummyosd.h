// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file dummyosd.h
 * Dummy OSD
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 *
 * The code is borrowed from the dummydevice plugin:
 *    http://phivdr.dyndns.org/vdr/
 * which was written by:
 *    Petri Hintukainen <phintuka@users.sourceforge.net>
 * and released under the GNU GPLv2
 */
#ifndef __DUMMYOSD_H
#define __DUMMYOSD_H

#include <vdr/osd.h>

/**
 * Dummy Pixmap for Skins
 *
 * This pixmap just inits but does nothing else
 *
 * @ingroup osd
 */
class cDummyPixmap : public cPixmap {
public:
	cDummyPixmap(int Layer, const cRect &ViewPort, const cRect &DrawPort = cRect::Null)
		: cPixmap(Layer, ViewPort, DrawPort) {}
	virtual ~cDummyPixmap(void) {}
	virtual void Clear(void) {}
	virtual void Fill([[maybe_unused]] tColor Color) {}
	virtual void DrawImage([[maybe_unused]] const cPoint &Point, [[maybe_unused]] const cImage &Image) {}
	virtual void DrawImage([[maybe_unused]] const cPoint &Point, [[maybe_unused]] int ImageHandle) {}
	virtual void DrawScaledImage([[maybe_unused]] const cPoint &Point, [[maybe_unused]] const cImage &Image, [[maybe_unused]] double FactorX, [[maybe_unused]] double FactorY, [[maybe_unused]] bool AntiAlias) {}
	virtual void DrawScaledImage([[maybe_unused]] const cPoint &Point, [[maybe_unused]] int ImageHandle, [[maybe_unused]] double FactorX, [[maybe_unused]] double FactorY, [[maybe_unused]] bool AntiAlias) {}
	virtual void DrawPixel([[maybe_unused]] const cPoint &Point, [[maybe_unused]] tColor Color) {}
	virtual void DrawBitmap([[maybe_unused]] const cPoint &Point, [[maybe_unused]] const cBitmap &Bitmap, [[maybe_unused]] tColor ColorFg = 0, [[maybe_unused]] tColor ColorBg = 0,
		[[maybe_unused]] bool Overlay = false) {}
	virtual void DrawText([[maybe_unused]] const cPoint &Point, [[maybe_unused]] const char *s, [[maybe_unused]] tColor ColorFg, [[maybe_unused]] tColor ColorBg, [[maybe_unused]] const cFont *Font,
		[[maybe_unused]] int Width = 0, [[maybe_unused]] int Height = 0, [[maybe_unused]] int Alignment = taDefault) {}
	virtual void DrawRectangle([[maybe_unused]] const cRect &Rect, [[maybe_unused]] tColor Color) {}
	virtual void DrawEllipse([[maybe_unused]] const cRect &Rect, [[maybe_unused]] tColor Color, [[maybe_unused]] int Quadrants = 0) {}
	virtual void DrawSlope([[maybe_unused]] const cRect &Rect, [[maybe_unused]] tColor Color, [[maybe_unused]] int Type) {}
	virtual void Render([[maybe_unused]] const cPixmap *Pixmap, [[maybe_unused]] const cRect &Source, [[maybe_unused]] const cPoint &Dest) {}
	virtual void Copy([[maybe_unused]] const cPixmap *Pixmap, [[maybe_unused]] const cRect &Source, [[maybe_unused]] const cPoint &Dest) {}
	virtual void Scroll([[maybe_unused]] const cPoint &Dest, [[maybe_unused]] const cRect &Source = cRect::Null) {}
	virtual void Pan([[maybe_unused]] const cPoint &Dest, [[maybe_unused]] const cRect &Source = cRect::Null) {}
};

/**
 * Dummy OSD
 *
 * This OSD just inits and can create a dummy pixmap but really nothing else
 *
 * @ingroup osd
 */
class cDummyOsd : public cOsd {
private:
	cDummyPixmap *m_pixmap;
public:
	cDummyOsd(int Left, int Top, uint Level) : cOsd(Left, Top, Level) {}
	virtual ~cDummyOsd() {}

	virtual cPixmap *CreatePixmap(int Layer, const cRect &ViewPort, const cRect &DrawPort = cRect::Null) {
		m_pixmap = new cDummyPixmap(Layer, ViewPort, DrawPort);
		return m_pixmap;
	}

	virtual void DestroyPixmap([[maybe_unused]] cPixmap *Pixmap) {}
	virtual void DrawImage([[maybe_unused]] const cPoint &Point, [[maybe_unused]] const cImage &Image) {}
	virtual void DrawImage([[maybe_unused]] const cPoint &Point, [[maybe_unused]] int ImageHandle) {}
	virtual eOsdError CanHandleAreas([[maybe_unused]] const tArea *Areas, [[maybe_unused]] int NumAreas) {return oeOk;}
	virtual eOsdError SetAreas([[maybe_unused]] const tArea *Areas, [[maybe_unused]] int NumAreas) {return oeOk;}
	virtual void SaveRegion([[maybe_unused]] int x1, [[maybe_unused]] int y1, [[maybe_unused]] int x2, [[maybe_unused]] int y2) {}
	virtual void RestoreRegion(void) {}
	virtual eOsdError SetPalette([[maybe_unused]] const cPalette &Palette, [[maybe_unused]] int Area) {return oeOk;}
	virtual void DrawPixel([[maybe_unused]] int x, [[maybe_unused]] int y, [[maybe_unused]] tColor Color) {}
	virtual void DrawBitmap([[maybe_unused]] int x, [[maybe_unused]] int y, [[maybe_unused]] const cBitmap &Bitmap, [[maybe_unused]] tColor ColorFg = 0,
		[[maybe_unused]] tColor ColorBg = 0, [[maybe_unused]] bool ReplacePalette = false, [[maybe_unused]] bool Overlay = false) {}
	virtual void DrawText([[maybe_unused]] int x, [[maybe_unused]] int y, [[maybe_unused]] const char *s, [[maybe_unused]] tColor ColorFg, [[maybe_unused]] tColor ColorBg,
		[[maybe_unused]] const cFont *Font, [[maybe_unused]] int Width = 0, [[maybe_unused]] int Height = 0, [[maybe_unused]] int Alignment = taDefault) {}
	virtual void DrawRectangle([[maybe_unused]] int x1, [[maybe_unused]] int y1, [[maybe_unused]] int x2, [[maybe_unused]] int y2, [[maybe_unused]] tColor Color) {}
	virtual void DrawEllipse([[maybe_unused]] int x1, [[maybe_unused]] int y1, [[maybe_unused]] int x2, [[maybe_unused]] int y2, [[maybe_unused]] tColor Color, [[maybe_unused]] int Quadrants = 0) {}
	virtual void DrawSlope([[maybe_unused]] int x1, [[maybe_unused]] int y1, [[maybe_unused]] int x2, [[maybe_unused]] int y2, [[maybe_unused]] tColor Color, [[maybe_unused]] int Type) {}
	virtual void Flush(void) {}
};

#endif
