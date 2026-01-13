/**
 * @file grab.h
 * Grabber header file
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

#ifndef __GRAB_H
#define __GRAB_H

#include "drmbuffer.h"

#include <vdr/osd.h>

/**
 * cGrabBuffer - Grab buffer class
 *
 * Class to hold the data for a grabbed buffer.
 * Grab is triggered by VDR/ cSoftHdDevice, data is set by the renderer
 * and composed by cSoftHdDevice again.
 */
class cGrabBuffer
{
public:
	cGrabBuffer(void);
	virtual ~cGrabBuffer(void);
	void FreeBuf(void);

	// setters and getters
	void SetRect(int x, int y, int width, int height) { m_rect.Set(x, y, width, height); };
	void SetData(uint8_t *result) { m_pResult = result; };
	void SetSize(int size) { m_size = size; };
	void SetBuf(cDrmBuffer *buf) { m_pBuf = buf; };

	int GetX(void) { return m_rect.X(); };
	int GetY(void) { return m_rect.Y(); };
	int GetWidth(void) { return m_rect.Width(); };
	int GetHeight(void) { return m_rect.Height(); };
	uint8_t *GetData(void) { return m_pResult; };
	int GetSize(void) { return m_size; };
	cDrmBuffer *GetBuf(void) { return m_pBuf; };
private:
	uint8_t *m_pResult;          ///< pointer to grabbed image
	struct cDrmBuffer *m_pBuf;   ///< pointer to original buffer
	int m_size;                  ///< size of grabbed data
	cRect m_rect;                ///< rect of the grabbed data
};

#endif
