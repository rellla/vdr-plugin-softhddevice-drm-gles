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

#include <atomic>
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

	void FreeInput(void);
	void Clear(void);
	void Set(cDrmBuffer *);
	bool IsSet(void) { return !m_outputRect.IsEmpty(); };

	// setters and getters
	// output
	void SetOutputData(uint8_t *result) { m_pOutputData = result; };
	uint8_t *GetOutputData(void) { return m_pOutputData; };
	void SetOutputSize(int size) { m_outputSize = size; };
	int GetOutputSize(void) { return m_outputSize; };
	int GetOutputX(void) { return m_outputRect.X(); };
	int GetOutputY(void) { return m_outputRect.Y(); };
	int GetOutputWidth(void) { return m_outputRect.Width(); };
	int GetOutputHeight(void) { return m_outputRect.Height(); };

	// input
	uint32_t Width(void) { return m_width; };
	uint32_t Height(void) { return m_height; };
	uint32_t PixFmt(void) { return m_pixFmt; };
	uint64_t Modifier(void) { return m_modifier; };
	int NumPlanes(void) { return m_numPlanes; };
	uint8_t *Plane(int idx) { return m_pPlane[idx]; };
	uint32_t Offset(int idx) { return m_offset[idx]; };
	uint32_t Pitch(int idx) { return m_pitch[idx]; };
	uint32_t Size(int idx) { return m_size[idx]; };

private:
	// output
	uint8_t *m_pOutputData = nullptr;    ///< pointer to grabbed image
	int m_outputSize = 0;                ///< size of grabbed data
	cRect m_outputRect;                  ///< rect of the grabbed data

	// input
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_pixFmt = 0;
	uint64_t m_modifier = 0;
	int m_numPlanes = 0;
	uint8_t *m_pPlane[4] = {};
	uint32_t m_offset[4] = {};
	uint32_t m_pitch[4] = {};
	uint32_t m_size[4] = {};
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

	bool IsActive(void) { return m_active; };
	bool Start(bool, int, int, int, int, int);
	uint8_t *Image(void) { return m_grabbedImage; };
	int Size(void) { return m_grabbedSize; };
	bool ProcessGrab(void);
	void Finish(void);

private:
	cVideoRender *m_pRender;         ///< pointer to cVideoRender object
	uint8_t *m_grabbedImage;         ///< pointer to the finished grabbed image
	int m_grabbedSize;               ///< data size of the grabbed image
	std::atomic<bool> m_active = false; ///< true, if a grab process is currently running

	bool m_isJpeg = true;            ///< true, if a jpeg image was requested
	int m_quality;                   ///< quality of the jpeg image
	int m_grabbedWidth;              ///< pixel width of the grabbed image
	int m_grabbedHeight;             ///< pixel height of the grabbed image
	int m_screenWidth;               ///< pixel screenwidth
	int m_screenHeight;              ///< pixel screenheight

	uint8_t *GetGrab(int *, int *, int *, int *, int *, Grabtype);
};

#endif
