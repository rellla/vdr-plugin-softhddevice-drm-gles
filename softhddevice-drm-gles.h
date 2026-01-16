/**
 * @file softhddevice-drm-gles.h
 * Main plugin class header file
 *
 * @copyright (c) 2011, 2014 by Johns.  All Rights Reserved.
 * @copyright (c) 2018 - 2019 zille.  All Rights Reserved.
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

#ifndef __SOFTHDDEVICE_DRM_GLES_H
#define __SOFTHDDEVICE_DRM_GLES_H

#include "vdr/plugin.h"

#ifdef USE_GLES
#include "openglosd.h"
#endif

#include "softhddevice.h"
#include "config.h"

#ifndef GIT_DESCRIBE
#define GIT_DESCRIBE "-unknown"
#endif

class cSoftHdDevice;

/*****************************************************************************
 * Plugin
 ****************************************************************************/

/**
 * cPluginSoftHdDevice - SoftHdDevice plugin class
 */
class cPluginSoftHdDevice : public cPlugin
{
public:
	cPluginSoftHdDevice(void);
	virtual ~cPluginSoftHdDevice(void);
	virtual const char *Version(void);
	virtual const char *Description(void);
	virtual const char *CommandLineHelp(void);
	virtual bool ProcessArgs(int, char *[]);
	virtual bool Initialize(void);
	virtual bool Start(void);
	virtual void Stop(void);
	virtual const char *MainMenuEntry(void);
	virtual cOsdObject *MainMenuAction(void);
	virtual cMenuSetupPage *SetupMenu(void);
	virtual bool SetupParse(const char *, const char *);
	virtual bool Service(const char *, void * = NULL);
	virtual const char **SVDRPHelpPages(void);
	virtual cString SVDRPCommand(const char *, const char *, int &);
private:
	cSoftHdDevice *m_pDevice;          ///< pointer to cSoftHdDevice object
	cSoftHdConfig *m_pConfig;          ///< pointer to cSoftHdConfig object
};

#endif
