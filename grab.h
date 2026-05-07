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
	cGrabBuffer(const char *identifier) : m_identifier(identifier) {};

	void FreeInput(void);
	void Clear(void);
	void Set(cDrmBuffer *);
	bool IsSet(void) { return !m_outputRect.IsEmpty(); };
	uint8_t *ConvertToRgb(int *);

	// setters and getters
	void SetOutputData(uint8_t *result) { m_pOutputData = result; };
	uint8_t *GetOutputData(void) { return m_pOutputData; };
	void SetOutputSize(int size) { m_outputSize = size; };
	int GetOutputSize(void) { return m_outputSize; };
	int GetOutputX(void) { return m_outputRect.X(); };
	int GetOutputY(void) { return m_outputRect.Y(); };
	int GetOutputWidth(void) { return m_outputRect.Width(); };
	int GetOutputHeight(void) { return m_outputRect.Height(); };

private:
	// output
	uint8_t *m_pOutputData = nullptr;    ///< pointer to grabbed image
	int m_outputSize = 0;                ///< size of grabbed data
	cRect m_outputRect;                  ///< rect of the grabbed data

	// input (copied from original cDrmBuffer)
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_pixFmt = 0;
	uint64_t m_modifier = 0;
	int m_numPlanes = 0;
	uint8_t *m_pPlane[4] = {};
	uint32_t m_offset[4] = {};
	uint32_t m_pitch[4] = {};
	uint32_t m_size[4] = {};

	const char *m_identifier;
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

	uint8_t *GetGrabbedVideoData(int *, int *, int *, int *, int *);
	uint8_t *GetGrabbedPipData(int *, int *, int *, int *, int *);
	uint8_t *GetGrabbedOsdData(int *, int *, int *, int *, int *);
	uint8_t *GetGrabbedData(int *, int *, int *, int *, int *, cGrabBuffer *);
};

#endif
