/**
 * @file drmbuffer.h
 * DRM buffer header file
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

#ifndef __DRMBUFFER_H
#define __DRMBUFFER_H

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/hwcontext_drm.h>
}

#ifdef USE_GLES
#include <gbm.h>
#endif

struct format_plane_info
{
	uint8_t bitspp;
	uint8_t xsub;
	uint8_t ysub;
};

struct format_info
{
	uint32_t format;
	const char *fourcc;
	uint8_t num_planes;
	struct format_plane_info planes[4];
};

class cDrmBuffer {
public:
	cDrmBuffer(void);
	cDrmBuffer(cDrmBuffer *src);
#ifdef USE_GLES
	cDrmBuffer(int, uint32_t, uint32_t, uint32_t, struct gbm_bo *);
#endif
	virtual ~cDrmBuffer(void);

	int Setup(int, uint32_t, uint32_t, uint32_t, AVDRMFrameDescriptor *);
	void Destroy(void);
	void InitBo(int, uint32_t, uint32_t, uint32_t, struct gbm_bo *);
	void FillBlack(void);

	// setter and getter methods
	uint32_t Width(void) { return m_width; };
	void SetWidth(uint32_t width) { m_width = width; };
	uint32_t Height(void) { return m_height; };
	void SetHeight(uint32_t height) { m_height = height; };
	uint32_t PixFmt(void) { return m_pixFmt; };
	void SetPixFmt(uint32_t pixFmt) { m_pixFmt = pixFmt; };

	bool IsDirty(void) { return m_dirty; };
	void MarkClean(void) { m_dirty = false; };
	void MarkDirty(void) { m_dirty = true; };
	bool IsSwBuffer(void) { return m_swbuffer; };
	void MarkAsHwBuffer(void) { m_swbuffer = false; };
	void MarkAsSwBuffer(void) { m_swbuffer = true; };

	void SetTrickspeed(bool trickspeed) { m_trickspeed = trickspeed; };
	bool IsTrickspeedBuffer(void) { return m_trickspeed; };

	int Id(void) { return m_fbId; };
	void SetId(int id) { m_fbId = id; };
	void SetFdDrm(int fdDrm) { m_fdDrm = fdDrm; };
	int NumPlanes(void) { return m_numPlanes; };
	void SetNumPlanes(int numPlanes) { m_numPlanes = numPlanes; };
	int FdPrime(int idx) { return m_fdPrime[idx]; };
	void SetFdPrime(int idx, uint32_t fd) { m_fdPrime[idx] = fd; };
	void SetNumObjects(int numObjects) { m_numObjects = numObjects; };
	void SetObjectIndex(int idx, uint32_t objIdx) { m_objIdx[idx] = objIdx; };
	uint8_t *Plane(int idx) { return m_pPlane[idx]; };
	uint32_t Handle(int idx) { return m_handle[idx]; };
	uint32_t *Handle(void) { return m_handle; };
	void SetHandle(int idx, uint32_t handle) { m_handle[idx] = handle; };
	uint32_t Offset(int idx) { return m_offset[idx]; };
	uint32_t *Offset(void) { return m_offset; };
	void SetOffset(int idx, uint32_t offset) { m_offset[idx] = offset; };
	uint32_t Pitch(int idx) { return m_pitch[idx]; };
	uint32_t *Pitch(void) { return m_pitch; };
	void SetPitch(int idx, uint32_t pitch) { m_pitch[idx] = pitch; };
	uint32_t Size(int idx) { return m_size[idx]; };
	uint32_t *Size(void) { return m_size; };
	void SetSize(int idx, uint32_t size) { m_size[idx] = size; };

	AVFrame *Frame(void) { return m_pFrame; };
	void SetFrame(AVFrame *frame) { m_pFrame = frame; };

private:
	uint32_t m_width;           ///< buffer width
	uint32_t m_height;          ///< buffer height
	uint32_t m_pixFmt;          ///< buffer pixel format

	bool m_dirty;               ///< true, if the buffer is dirty (it was written to)
	bool m_swbuffer;            ///< true, if the buffer is a software buffer
	bool m_trickspeed;          ///< true, if the buffer is a trickspeed buffer

	uint32_t m_fbId;            ///< framebuffer id
	int m_fdDrm;                ///< drm file desriptor

	int m_numPlanes;            ///< number of planes in the buffer

	int m_fdPrime[4];           ///< prime file descriptros (corresponding to objIdx)
	int m_numObjects;           ///< number of prime objects in the buffer
	int m_objIdx[4];            ///< index of the objects
	uint32_t m_primehandle[4];  ///< primedata objects prime handles (count is numObjects, index is objIdx)

	uint8_t *m_pPlane[4];       ///< array of the plane data
	uint32_t m_handle[4];       ///< array of the plane handles
	uint32_t m_offset[4];       ///< array of the plane offset
	uint32_t m_pitch[4];        ///< array of the plane pitch
	uint32_t m_size[4];         ///< array of the plane size

	AVFrame *m_pFrame;
#ifdef USE_GLES
	struct gbm_bo *m_pBo;       ///< pointer to the gbm buffer object
#endif
};

#endif
