/**
 * @file softhddevice-drm-gles.cpp
 * Main plugin class
 *
 * This file defines cPluginSoftHdDevice, which is the main class
 * for initializing the plugin itselft.
 *
 * @copyright (c) 2011, 2015 by Johns.  All Rights Reserved.
 * @copyright (c) 2018 zille.  All Rights Reserved.
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

#define __STDC_CONSTANT_MACROS      ///< needed for ffmpeg UINT64_C

#include <string>
using std::string;
#include <fstream>
using std::ifstream;

#include <vdr/player.h>
#include <vdr/plugin.h>

#include "logger.h"

#include "softhddevice-drm-gles.h"
#include "softhddevice.h"
#include "softhdmenu.h"
#include "mediaplayer.h"

#ifdef USE_GLES
#include "openglosd.h"
#endif

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include "videostream.h"
#include "videorender.h"
#include "audio.h"
#include "codec_audio.h"
#include "softhdosd.h"
#include "softhdsetupmenu.h"

/*****************************************************************************
 * Static variables
 ****************************************************************************/
static const char *const VERSION = "1.2.1";    ///< vdr-plugin version number
                                               ///< Makefile extracts the version number for generating the file name
                                               ///< for the distribution archive.

static const char *const DESCRIPTION = trNOOP("A software and GPU emulated HD device");
                                               ///< vdr-plugin description.

static const char *const MAINMENUENTRY = trNOOP("SHD Media Player");
                                               ///< what is displayed in the main menu entry

cSoftHdMenu *cSoftHdMenu::pSoftHdMenu = NULL;

/*****************************************************************************
 * cPluginSoftHdDevice
 ****************************************************************************/

/**
 * cPluginSoftHdDevice constructor
 *
 * Initialize any member variables here.
 *
 * @note DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
 * VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
 *
 * We only create the config and the device itself, because Plugin->SetupParse
 * is done next and that one needs config to be available.
 * SetupParse must not access any other objects!
 */
cPluginSoftHdDevice::cPluginSoftHdDevice(void)
{
	m_pConfig = new cSoftHdConfig();
	m_pDevice = new cSoftHdDevice(m_pConfig); // no need to delete m_pDevice, because VDR does it for us
}

/**
 * cPluginSoftHdDevice destructor
 *
 * Clean up after yourself!
 */
cPluginSoftHdDevice::~cPluginSoftHdDevice(void)
{
	delete m_pConfig;
}

/**
 * Return plugin version number
 *
 * @returns version number as constant string
 */
const char *cPluginSoftHdDevice::Version(void)
{
	return VERSION;
}

/**
 * Return plugin short description
 *
 * @returns short description as constant string
 */
const char *cPluginSoftHdDevice::Description(void)
{
	return tr(DESCRIPTION);
}

/**
 * Return a string that describes all known command line options
 *
 * @returns command line help as constant string
 */
const char *cPluginSoftHdDevice::CommandLineHelp(void)
{
	return m_pDevice->CommandLineHelp();
}

/**
 * Process the command line arguments
 */
bool cPluginSoftHdDevice::ProcessArgs(int argc, char *argv[])
{
//	LOGDEBUG("plugin: %s:", __FUNCTION__);

	return m_pDevice->ProcessArgs(argc, argv);
}

/**
 * Initializes the DVB devices
 *
 * Must be called before accessing any DVB functions
 *
 * @returns true if any devices are available.
 */
bool cPluginSoftHdDevice::Initialize(void)
{
//	LOGDEBUG("plugin: %s:", __FUNCTION__);

	m_pDevice->Init();

	return true;
}

/**
 * Start any background activities the plugin shall perform
 */
bool cPluginSoftHdDevice::Start(void)
{
//	LOGDEBUG("plugin: %s:", __FUNCTION__);

	m_pDevice->Start();

	return true;
}

/**
 * Shutdown plugin
 *
 * Stop any background activities the plugin is performing
 */
void cPluginSoftHdDevice::Stop(void)
{
	//LOGDEBUG("plugin: %s:", __FUNCTION__);

	m_pDevice->Stop();
}

/**
 * Create main menu entry
 */
const char *cPluginSoftHdDevice::MainMenuEntry(void)
{
	//LOGDEBUG("plugin: %s:", __FUNCTION__);

	return m_pConfig->ConfigHideMainMenuEntry ? NULL : tr(MAINMENUENTRY);
}

/**
 * Perform the action when selected from the main VDR menu
 */
cOsdObject *cPluginSoftHdDevice::MainMenuAction(void)
{
	//LOGDEBUG("plugin: %s:", __FUNCTION__);

	return new cSoftHdMenu("SoftHdDevice", m_pDevice);
}

/**
 * Return our setup menu
 */
cMenuSetupPage *cPluginSoftHdDevice::SetupMenu(void)
{
	//LOGDEBUG("plugin: %s:", __FUNCTION__);

	return new cMenuSetupSoft(m_pDevice);
}

/*****************************************************************************
 * cPluginSoftHdDevice - Setup parameters
 ****************************************************************************/

/**
 * Parse setup parameters
 *
 * @param name      paramter name (case sensetive)
 * @param value     value as string
 *
 * @returns         true if the parameter is supported, false otherwise
 */
bool cPluginSoftHdDevice::SetupParse(const char *name, const char *value)
{
	return m_pConfig->SetupParse(name, value);
}

/**
 * Receive requests or messages
 *
 * @param id     unique identification string that identifies the
 *               service protocol
 * @param data	 custom data structure
 */
bool cPluginSoftHdDevice::Service(const char *id, void *data)
{
	//LOGDEBUG("plugin: %s: id %s", __FUNCTION__, id);
	(void)id;
	(void)data;

	return false;
}

/*****************************************************************************
 * cPluginSoftHdDevice - SVDRP
 ****************************************************************************/

/**
 * SVDRP commands help text
 */
static const char *SVDRPHelpText[] = {
	"PLAY Url\n" "    Play the media from the given url.\n",
	"DETA\n" "        Detach the plugin.\n",
	"ATTA\n" "        Attach the plugin.\n",
	"STAT\n" "        Get attached/detached status.\n"
	"    ATTACHED -> 910\n"
	"    DETACHED -> 911\n",
	NULL
};

/**
 * Return SVDRP commands help pages
 *
 * return a pointer to a list of help strings for all of the plugin's
 * SVDRP commands.
 */
const char **cPluginSoftHdDevice::SVDRPHelpPages(void)
{
	return SVDRPHelpText;
}

/**
 * Handle SVDRP commands
 *
 * @param command       SVDRP command
 * @param option        all command arguments
 * @param reply_code    reply code
 */
cString cPluginSoftHdDevice::SVDRPCommand(const char *command, const char *option, int &reply_code)
{
	if (!strcasecmp(command, "PLAY")) {
		LOGDEBUG2(L_MEDIA, "plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
		cControl::Launch(new cSoftHdControl(option, m_pDevice));
		return "PLAY url";
	}
	if (!strcasecmp(command, "DETA")) {
		LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
		if (m_pDevice->IsDetached())
			return "SoftHdDevice is already detached";

		if (m_pDevice->Replaying()) {
			LOGDEBUG("plugin: %s: Device is replaying, stop replay first", __FUNCTION__);
			m_pDevice->StopReplay();
		}

		m_pDevice->Detach();
		return "Detached SoftHdDevice";
	}
	if (!strcasecmp(command, "ATTA")) {
		LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
		if (!m_pDevice->IsDetached())
			return "SoftHdDevice is not detached";

		m_pDevice->Attach();
		return "Attached SoftHdDevice";
	}
	if (!strcasecmp(command, "STAT")) {
		LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
		if (!m_pDevice->IsDetached()) {
			reply_code = 910;
			return "SoftHdDevice is attached";
		} else {
			reply_code = 911;
			return "SoftHdDevice is detached";
		}
	}

	return NULL;
}

VDRPLUGINCREATOR(cPluginSoftHdDevice);	// Don't touch this!
