// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file grab.h
 * Grabbing Interface Header File
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __GRAB_H
#define __GRAB_H

#include <cstdint>

#include <vdr/osd.h>

class cDrmBuffer;
class cVideoRender;

/**
 * @addtogroup misc
 * @{
 */

enum Grabtype {
	GRABVIDEO,
	GRABPIP,
	GRABOSD
};

inline const char* GrabtypeToString(Grabtype t) {
    switch(t) {
        case Grabtype::GRABVIDEO: return "VIDEO";
        case Grabtype::GRABPIP: return "PIP";
        case Grabtype::GRABOSD: return "OSD";
    }
    return "Unknown";
}

/** @} */

/**
 * Grabbing Buffer
 *
 * Holds the data for a grabbed buffer.
 * The grab is triggered by VDR/ cSoftHdDevice, data is set by the renderer
 * and composed by cSoftHdDevice again.
 *
 * @ingroup misc
 */
class cGrabBuffer {
public:
	cGrabBuffer(void) = default;

	void FreeDrmBuf(void);
	void SetDrmBuf(cDrmBuffer *);

	// setters and getters
	void SetData(uint8_t *result) { m_pResult = result; };
	void SetSize(int size) { m_size = size; };

	int GetX(void) { return m_rect.X(); };
	int GetY(void) { return m_rect.Y(); };
	int GetWidth(void) { return m_rect.Width(); };
	int GetHeight(void) { return m_rect.Height(); };
	uint8_t *GetData(void) { return m_pResult; };
	int GetSize(void) { return m_size; };
	cDrmBuffer *GetDrmBuf(void) { return m_pBuf; };
private:
	uint8_t *m_pResult = nullptr;        ///< pointer to grabbed image
	struct cDrmBuffer *m_pBuf = nullptr; ///< pointer to original buffer
	int m_size = 0;                      ///< size of grabbed data
	cRect m_rect;                        ///< rect of the grabbed data
};

/**
 * Grabbing Processor
 *
 * Handles the grabbing workflow from triggering the grab to returning the result
 *
 * @ingroup misc
 */
class cSoftHdGrab {
public:
	cSoftHdGrab(cVideoRender *render) : m_pRender(render) {};

	bool Active(void) { return m_isActive; };
	bool Start(bool, int, int, int, int, int);
	uint8_t *Image(void) { return m_grabbedImage; };
	int Size(void) { return m_grabbedSize; };

private:
	cVideoRender *m_pRender;         ///< pointer to cVideoRender object
	uint8_t *m_grabbedImage;         ///< pointer to the finished grabbed image
	int m_grabbedSize;               ///< data size of the grabbed image
	bool m_isActive = false;         ///< true, if a grab process is currently running

	bool m_isJpeg = true;            ///< true, if a jpeg image was requested
	int m_quality;                   ///< quality of the jpeg image
	int m_grabbedWidth;              ///< pixel width of the grabbed image
	int m_grabbedHeight;             ///< pixel height of the grabbed image
	int m_screenWidth;               ///< pixel screenwidth
	int m_screenHeight;              ///< pixel screenheight

	bool ProcessGrab(void);
	uint8_t *GetGrab(int *, int *, int *, int *, int *, Grabtype);
};

#endif
