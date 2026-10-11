// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file softosd.h
 * Software OSD Header File
 *
 * @copyright 2011, 2014 by Johns.  All Rights Reserved.
 * @copyright 2018 - 2019 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __DRMDEVICE_SOFTOSD_H
#define __DRMDEVICE_SOFTOSD_H

#include <vdr/osd.h>

class cOutputDevice;

/**
 * OSD Rendering Interface
 * @defgroup osd OSD
 */

/**
 * Software Based OSD (CPU)
 *
 * @ingroup osd
 */
class cSoftOsd:public cOsd {
public:
	cSoftOsd(int, int, uint, cOutputDevice *);
	virtual ~cSoftOsd(void);

	virtual eOsdError SetAreas(const tArea *, int);
	virtual void Flush(void);
	virtual void SetActive(bool);

private:
	cOutputDevice *m_pDevice;        ///< pointer to the cOutputDevice object
	bool m_dirty = false;            ///< flag to force redrawing everything
	int m_osdLevel;                  ///< current osd level
};

#endif
