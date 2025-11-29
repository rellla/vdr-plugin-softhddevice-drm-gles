/**
 * @file drmbuffer.cpp
 * DRM buffer class
 *
 * This files defines cDrmBuffer, which is a class used to describe
 * a DRM buffer, keeping framebuffer and prime handles to be used
 * by the kernel display interface.
 *
 * @copyright (c) 2018 by zille.  All Rights Reserved.
 * @copyright (c) 2025 by Andreas Baierl. All Rights Reserved.
 *
 * License{AGPLv3
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

#include "drmdevice.h"
#include "logger.h"

/*****************************************************************************
 * cDrmBuffer class
 ****************************************************************************/

 /**
 * cDrmBuffer constructor
 *
 * @param device     pointer to cSoftHdDevice
 */
cDrmBuffer::cDrmBuffer(void)
{
	m_dirty = false;
	m_swbuffer = false;
	m_trickspeed = false;
	m_fdPrime[0] = 0;

	m_numPlanes = 0;
	for (int i = 0; i < 4; i++) {
		m_pPlane[i] = nullptr;
	}
}

/**
 * cDrmBuffer constructor
 *
 * Clone a cDrmBuffer
 *
 * @param src     src buffer to be cloned
 *
 * @returns       a new cDrmBuffer object cloned from src
 */
cDrmBuffer::cDrmBuffer(cDrmBuffer *src)
{
	m_width = src->m_width;
	m_height = src->m_height;
	m_fbId = src->m_fbId;
	m_pixFmt = src->m_pixFmt;
	m_numPlanes = src->m_numPlanes;
	m_trickspeed = src->m_trickspeed;
	m_swbuffer = src->m_swbuffer;
	m_numObjects = src->m_numObjects;
	m_dirty = false;

	for (int object = 0; object < m_numObjects; object++) {
		m_fdPrime[object] = src->m_fdPrime[object];
	}

	for (int i = 0; i < src->m_numPlanes; i++) {
		m_size[i] = src->m_size[i];
		m_pitch[i] = src->m_pitch[i];
		m_handle[i] = src->m_handle[i];
		m_offset[i] = src->m_offset[i];
		m_objIdx[i] = src->m_objIdx[i];
	}

	void *src_buffer = NULL;
	void *dst_buffer = NULL;

	// planes aren't mmapped, do it (PRIME)
	if (!src->m_pPlane[0]) {
		for (int object = 0; object < m_numObjects; object++) {
			// memcpy mmapped data
			dst_buffer = malloc(src->m_size[object]);
			src_buffer = mmap(NULL, src->m_size[object], PROT_READ, MAP_SHARED, src->m_fdPrime[object], 0);
			if (src_buffer == MAP_FAILED) {
				LOGERROR("drmbuffer: %s (clone): cannot map buffer size %d prime_fd %d (%d): %m",
					__FUNCTION__, src->m_size[object], src->m_fdPrime[object], errno);
				return;
			}

			LOGDEBUG2(L_GRAB, "drmbuffer: %s (clone): Copy %p to %p", __FUNCTION__, src_buffer, dst_buffer);
			memcpy(dst_buffer, src_buffer, src->m_size[object]);
			munmap(src_buffer, src->m_size[object]);
			for (int plane = 0; plane < m_numPlanes; plane++) {
				if (m_objIdx[plane] == object) {
					m_pPlane[plane] = (uint8_t *)dst_buffer;
					LOGDEBUG2(L_GRAB, "drmbuffer: %s (clone): plane[%d] gets %p (object %d)", __FUNCTION__, plane, dst_buffer, object);
				}
			}
		}
	} else {
		for (int plane = 0; plane < m_numPlanes; plane++) {
			dst_buffer = malloc(m_size[plane]);
			memcpy(dst_buffer, src->m_pPlane[plane], src->m_size[plane]);
			m_pPlane[plane] = (uint8_t *)dst_buffer;
		}
	}

	for (int plane = 0; plane < m_numPlanes; plane++) {
		LOGDEBUG2(L_GRAB, "drmbuffer: %s (clone): Cloned plane %d address %p pitch %d offset %d handle %d size %d",
			__FUNCTION__, plane, m_pPlane[plane], m_pitch[plane], m_offset[plane], m_handle[plane], m_size[plane]);
	}
}

#ifdef USE_GLES
/**
 * cDrmBuffer constructor
 *
 * Create a new cDrmBuffer from a gbm buffer object
 *
 * @param fdDrm          drm file descriptor
 * @param width          buffer width
 * @param height         buffer height
 * @param pixFmt         buffer pixel format
 * @param bo             pointer to gbm buffer object
 *
 * @returns              a new cDrmBuffer object from a gbm buffer object
 */
cDrmBuffer::cDrmBuffer(int fdDrm, uint32_t width, uint32_t height, uint32_t pixFmt, struct gbm_bo *bo)
{
	m_fdDrm = fdDrm;
	m_width = width;
	m_height = height;
	m_pixFmt = pixFmt;
	m_pBo = bo;
	m_numPlanes = 0;
	for (int i = 0; i < 4; i++) {
		m_pPlane[i] = nullptr;
		m_handle[i] = 0;
		m_offset[i] = 0;
		m_pitch[i] = 0;
		m_size[i] = 0;
	}
	m_dirty = false;
	m_swbuffer = false;
	m_trickspeed = false;
}
#endif

/**
 * cDrmBuffer destructor
 */
cDrmBuffer::~cDrmBuffer(void)
{
}

/**
 * Clear and destroy the buffer object and its parameters
 */
void cDrmBuffer::Destroy(void)
{
	struct drm_mode_destroy_dumb dreq;
	LOGDEBUG2(L_DRM, "drmbuffer: %s: destroy FB %d", __FUNCTION__, m_fbId);

	for (int i = 0; i < m_numPlanes; i++) {
		if (m_pPlane[i]) {
			if (munmap(m_pPlane[i], m_size[i]))
				LOGERROR("drmbuffer: %s: failed unmap FB (%d): %m", __FUNCTION__, errno);
		}
	}

	if (drmModeRmFB(m_fdDrm, m_fbId) < 0)
		LOGERROR("drmbuffer: %s: cannot rm FB (%d): %m", __FUNCTION__, errno);

	if (m_fdPrime[0] && m_swbuffer) {
		if (close(m_fdPrime[0]))
			LOGERROR("drmbuffer: %s: error closing prime fd %d (%d): %m", __FUNCTION__, m_fdPrime[0], errno);
		m_fdPrime[0] = 0;
	}

	for (int i = 0; i < m_numPlanes; i++) {
		if (m_pPlane[i]) {
			memset(&dreq, 0, sizeof(dreq));
			dreq.handle = m_handle[i];

			if (drmIoctl(m_fdDrm, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) < 0)
				LOGERROR("drmbuffer: %s: cannot destroy dumb buffer (%d): %m", __FUNCTION__, errno);
			m_handle[i] = 0;

		}

		m_pPlane[i] = 0;
		m_size[i] = 0;
		m_pitch[i] = 0;
		m_offset[i] = 0;
		m_objIdx[i] = 0;
	}

	for (int i = 0; i < m_numObjects; i++) {
		if (m_primehandle[i]) {
			// this can happen, when we SetPlayMode 0 while in trickspeed
			// does not show negative effects, but its not nice though -> TODO
			if (drmIoctl(m_fdDrm, DRM_IOCTL_GEM_CLOSE, &m_primehandle[i]) < 0)
				LOGERROR("drmbuffer: %s: cannot close handle %d FB %d GEM (%d): %m", __FUNCTION__, m_primehandle[i], m_fbId, errno);
		}
	}

	m_width = 0;
	m_height = 0;
	m_fbId = 0;
	m_trickspeed = false;
	m_dirty = false;
	m_swbuffer = false;
	m_numPlanes = 0;
}

/**
 * Infos of a pixel format
 *
 * Each entry describes a format in the following matter:
 * {
 * 	uint32_t format,
 * 	const char *fourcc,
 * 	uint8_t num_planes,
 * 	struct format_plane_info planes[4]
 * }
 *
 * The format_plane_info is:
 * {
 * 	uint8_t bitspp,
 * 	uint8_t xsub,
 * 	uint8_t ysub,
 * }
 */
static const struct format_info format_info_array[] = {
	{ DRM_FORMAT_NV12, "NV12", 2, { { 8, 1, 1 }, { 16, 2, 2 } }, },
	{ DRM_FORMAT_YUV420, "YU12", 3, { { 8, 1, 1 }, { 8, 2, 2 }, {8, 2, 2 } }, },
	{ DRM_FORMAT_ARGB8888, "AR24", 1, { { 32, 1, 1 } }, },
};

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/**
 * Find infos for the given pixel format
 *
 * @param format      pixel format
 *
 * @returns           the infos of the format as a struct
 */
const struct format_info *FindFormat(uint32_t format)
{
	for (int i = 0; i < (int)ARRAY_SIZE(format_info_array); i++) {
		if (format == format_info_array[i].format)
			return &format_info_array[i];
	}
	return NULL;
}

/**
 * Setup the buffer
 *
 * @param fdDrm          drm file descriptor
 * @param width          buffer width
 * @param height         buffer height
 * @param pixFmt         buffer pixel format
 * @param primedata      AVDRMFrameDescriptor or NULL (if this is a software buffer)
 *
 * @returns 0            on success, 1 or negative error value on error
 */
int cDrmBuffer::Setup(int fdDrm, uint32_t width, uint32_t height, uint32_t pixFmt, AVDRMFrameDescriptor *primedata)
{
	uint64_t modifier[4] = { 0, 0, 0, 0 };
	uint32_t mod_flags = 0;
	m_handle[0] = m_handle[1] = m_handle[2] = m_handle[3] = 0;
	m_pitch[0] = m_pitch[1] = m_pitch[2] = m_pitch[3] = 0;
	m_offset[0] = m_offset[1] = m_offset[2] = m_offset[3] = 0;
	m_numObjects = m_numPlanes = 0;

	m_width = width;
	m_height = height;
	m_pixFmt = pixFmt;
	m_fdDrm = fdDrm;
	m_pFrame = nullptr;

	if (primedata) {
		// we have no DRM objects yet, so return
		if (!primedata->nb_objects) {
			LOGWARNING("drmbuffer: %s: No primedata objects available!", __FUNCTION__);
			return 1;
		}

		AVDRMLayerDescriptor *layer = &primedata->layers[0];

		m_pixFmt = layer->format;
		m_numPlanes = layer->nb_planes;
		m_numObjects = primedata->nb_objects;

//		LOGDEBUG2(L_DRM, "drmbuffer: %s: PRIMEDATA %d x %d, pix_fmt %4.4s nb_planes %d nb_objects %d", __FUNCTION__,
//			m_width, m_height, (char *)&m_pixFmt, m_numPlanes, m_numObjects);

		// create handles for PrimeFDs
		for (int object = 0; object < primedata->nb_objects; object++) {
			if (drmPrimeFDToHandle(fdDrm,
				primedata->objects[object].fd, &m_primehandle[object])) {

				LOGERROR("drmbuffer: %s: PRIMEDATA Failed to retrieve the Prime Handle %i size %zu (%d): %m", __FUNCTION__,
					primedata->objects[object].fd,
					primedata->objects[object].size, errno);
				return -errno;
			}
			m_fdPrime[object] = primedata->objects[object].fd;
			m_size[object] = primedata->objects[object].size;
//			LOGDEBUG2(L_DRM, "drmbuffer: %s: PRIMEDATA create handle for PrimeFD (%d|%i): PrimeFD %i ToHandle %i size %zu modifier %" PRIx64 "",
//				__FUNCTION__, object, primedata->nb_objects, primedata->objects[object].fd, m_primehandle[object],
//				primedata->objects[object].size, primedata->objects[object].format_modifier);
		}

		// fill the planes
		for (int plane = 0; plane < layer->nb_planes; plane++) {
			int object = layer->planes[plane].object_index;
			uint32_t handle = m_primehandle[object];
			if (handle) {
				m_handle[plane] = handle;
				m_pitch[plane] = layer->planes[plane].pitch;
				m_offset[plane] = layer->planes[plane].offset;
				m_objIdx[plane] = object;
				if (primedata->objects[object].format_modifier)
					modifier[plane] = primedata->objects[object].format_modifier;

//				LOGDEBUG2(L_DRM, "drmbuffer: %s: PRIMEDATA fill plane %d: handle %d object_index %i pitch %d offset %d size %d modifier %" PRIx64 " (plane not mapped!)",
//					__FUNCTION__, plane, m_handle[plane], m_objIdx[plane], m_pitch[plane], m_offset[plane], m_size[plane], modifier[plane]);
			}
		}
		if (modifier[0] && modifier[0] != DRM_FORMAT_MOD_INVALID)
			mod_flags = DRM_MODE_FB_MODIFIERS;
	} else {
		const struct format_info *format_info = FindFormat(m_pixFmt);
		if (!format_info) {
			LOGERROR("drmbuffer: %s: No suitable format found!", __FUNCTION__);
			return 1;
		}

		m_numPlanes = format_info->num_planes;

//		LOGDEBUG2(L_DRM, "drmbuffer: %s:  %d x %d, pix_fmt %4.4s nb_planes %d", __FUNCTION__,
//			m_width, m_height, (char *)&m_pixFmt, m_numPlanes);

		for (int plane = 0; plane < format_info->num_planes; plane++) {
			const struct format_plane_info *plane_info = &format_info->planes[plane];

			struct drm_mode_create_dumb creq;
			creq.height = m_height / plane_info->ysub;
			creq.width = m_width / plane_info->xsub;
			creq.bpp = plane_info->bitspp;
			creq.flags = 0;
			creq.handle = 0;
			creq.pitch = 0;
			creq.size = 0;

			if (drmIoctl(fdDrm, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0){
				LOGERROR("drmbuffer: %s: cannot create dumb buffer %dx%d@%d (%d): %m", __FUNCTION__,
					creq.width, creq.height, creq.bpp, errno);
				return -errno;
			}

			m_handle[plane] = creq.handle;
			m_pitch[plane] = creq.pitch;
			m_size[plane] = creq.size;

			struct drm_mode_map_dumb mreq;
			memset(&mreq, 0, sizeof(struct drm_mode_map_dumb));
			mreq.handle = m_handle[plane];

			if (drmIoctl(fdDrm, DRM_IOCTL_MODE_MAP_DUMB, &mreq)){
				LOGERROR("drmbuffer: %s: cannot prepare dumb buffer for mapping (%d): %m", __FUNCTION__, errno);
				return -errno;
			}

			m_pPlane[plane] = (uint8_t *)mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fdDrm, mreq.offset);

			if (m_pPlane[plane] == MAP_FAILED) {
				LOGERROR("drmbuffer: %s: cannot map dumb buffer (%d): %m", __FUNCTION__, errno);
				return -errno;
			}

			memset(m_pPlane[plane], 0, m_size[plane]);

//			LOGDEBUG2(L_DRM, "drmbuffer: %s: fill plane %d: handle %d pitch %d offset %d size %d address %p", __FUNCTION__,
//				plane, m_handle[plane], m_pitch[plane], m_offset[plane], m_size[plane], m_pPlane[plane]);
		}
	}

	int ret = -1;
	ret = drmModeAddFB2WithModifiers(fdDrm, m_width, m_height, m_pixFmt,
					 m_handle, m_pitch, m_offset, modifier, &m_fbId, mod_flags);

	if (ret) {
		if (mod_flags)
			LOGERROR("drmbuffer: %s: cannot create modifiers framebuffer (%d): %m", __FUNCTION__, errno);

		ret = drmModeAddFB2(fdDrm, m_width, m_height, m_pixFmt,
			m_handle, m_pitch, m_offset, &m_fbId, 0);
	}

	if (ret)
		LOGFATAL("drmbuffer: %s: cannot create framebuffer (%d): %m", __FUNCTION__, errno);

	LOGDEBUG2(L_DRM, "drmbuffer: %s: Added %sFB fb_id %d width %d height %d pix_fmt %4.4s", __FUNCTION__,
		primedata ? "primedata " : "", m_fbId, m_width, m_height, (char *)&m_pixFmt);

	m_dirty = true;
	return 0;
}

/**
 * Color the buffer black
 */
void cDrmBuffer::FillBlack(void)
{
	for (uint32_t i = 0; i < m_width * m_height; ++i) {
		m_pPlane[0][i] = 0x10;
		if (i < m_width * m_height / 2)
			m_pPlane[1][i] = 0x80;
	}
}
