// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file softhdosd.h
 * Software OSD Header File
 *
 * @copyright 2011, 2014 by Johns.  All Rights Reserved.
 * @copyright 2018 - 2019 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __SOFTOSD_H
#define __SOFTOSD_H

#ifdef USE_GLES
#include <memory>
#endif

#include <vdr/osd.h>

#ifdef USE_GLES
class cOglThread;
#endif
class cSoftHdDevice;

/**
 * @addtogroup osd
 * @{
 */

/**
 * Software Based OSD
 */
class cSoftOsd:public cOsd {
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

/**
 * Plugin OSD provider
 */
class cSoftOsdProvider:public cOsdProvider {
public:
	cSoftOsdProvider(cSoftHdDevice *);
	virtual ~cSoftOsdProvider();

	virtual cOsd * CreateOsd(int, int, uint);
	virtual bool ProvidesTrueColor(void) { return true; };
#ifdef USE_GLES
	void RequestStopOpenGlThread(void);
	void StopOpenGlThread(void);
	bool LockOpenGlThread(void);
	void UnlockOpenGlThread(void);
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

/** @} */

#endif
