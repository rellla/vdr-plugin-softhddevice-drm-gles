// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file plugindrmdevice.h
 * Main Plugin Interface Header File
 *
 * @copyright 2011, 2014 by Johns.  All Rights Reserved.
 * @copyright 2018 - 2019 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __DRMDEVICE_PLUGIN_DRM_DEVICE_H
#define __DRMDEVICE_PLUGIN_DRM_DEVICE_H

#include <vdr/plugin.h>

#include "git-version.h"

#ifndef GIT_DESCRIBE
#define GIT_DESCRIBE "-unknown"
#endif

class cOutputDevice;
class cDeviceConfig;

/**
 * Main Plugin Interface
 * @defgroup plugin Main Plugin
 */

/**
 * Main Plugin Class
 *
 * @ingroup plugin
 */
class cPluginDrmDevice : public cPlugin {
public:
	cPluginDrmDevice(void);
	virtual ~cPluginDrmDevice(void);
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
	virtual bool Service(const char *, void * = nullptr);
	virtual const char **SVDRPHelpPages(void);
	virtual cString SVDRPCommand(const char *, const char *, int &);
private:
	cOutputDevice *m_pDevice;          ///< pointer to cOutputDevice object
	cDeviceConfig *m_pConfig;          ///< pointer to cDeviceConfig object
};

#endif
