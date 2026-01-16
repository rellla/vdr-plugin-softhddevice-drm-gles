/**
 * @file softhdosd.h
 * Softhddevice osd header file
 *
 * Copyright: (c) 2011, 2014 by Johns.  All Rights Reserved.
 * Copyright (c) 2018 - 2019 zille.  All Rights Reserved.
 * Copyright: (c) 2025 by Andreas Baierl. All Rights Reserved.
 *
 * License: AGPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 */

#ifndef __SOFTOSD_H
#define __SOFTOSD_H

#ifdef USE_GLES
#include "openglosd.h"
#endif

#include "softhddevice.h"

class cSoftHdDevice;
class cOglThread;

/*****************************************************************************
 * OSD (software)
 ****************************************************************************/

/**
 * cSoftOsd - SoftHdDevice plugin software OSD class
 */
class cSoftOsd:public cOsd
{
public:
	cSoftOsd(int, int, uint, cSoftHdDevice *);
	virtual ~cSoftOsd(void);

	virtual eOsdError SetAreas(const tArea *, int);
	virtual void Flush(void);
	virtual void SetActive(bool);

private:
	cSoftHdDevice *m_pDevice;        ///< pointer to the cSoftHdDevice object
	bool m_dirty = false;            ///< flag to force redrawing everything
	int m_osdLevel;                  ///< current osd level
};

/*****************************************************************************
 * OSD provider
 ****************************************************************************/

/**
 * cSoftOsdProvider - SoftHdDevice plugin OSD provider class
 */
class cSoftOsdProvider:public cOsdProvider
{
public:
	cSoftOsdProvider(cSoftHdDevice *);
	virtual ~cSoftOsdProvider();

	virtual cOsd * CreateOsd(int, int, uint);
	virtual bool ProvidesTrueColor(void);
#ifdef USE_GLES
	void StopOpenGlThread(void);
	const cImage *GetImageData(int ImageHandle);
	void OsdSizeChanged(void);
#endif

private:
	cOsd *m_pOsd;                              ///< pointer to single OSD (currently not really used in cSoftOsdProvider?)
	cSoftHdDevice *m_pDevice;                  ///< pointer to the cSoftHdDevice object
#ifdef USE_GLES
	std::shared_ptr<cOglThread> m_pOglThread;  ///< OpenGL OSD thread
	bool StartOpenGlThread(void);
#endif

protected:
#ifdef USE_GLES
	virtual int StoreImageData(const cImage &Image);
	virtual void DropImageData(int ImageHandle);
#endif
};

#endif
