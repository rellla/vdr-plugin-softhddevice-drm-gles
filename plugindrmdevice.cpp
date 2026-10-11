// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file plugindrmdevice.cpp
 * Main Plugin Interface
 *
 * This file defines cPluginDrmDevice, which is the main class
 * for initializing the plugin itselft.
 *
 * @copyright 2011, 2015 by Johns.  All Rights Reserved.
 * @copyright 2018 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <libintl.h>

#include <vdr/player.h>
#include <vdr/plugin.h>

#include "logger.h"

#include "config.h"
#include "device.h"
#include "mediaplayer.h"
#include "menu.h"
#include "plugindrmdevice.h"
#include "setupmenu.h"

/**
 * @addtogroup plugin
 * @{
 */

/*****************************************************************************
 * Static variables
 ****************************************************************************/
static const char *const VERSION = "1.7.3" GIT_DESCRIBE;    ///< vdr-plugin version number
                                                            ///< Makefile extracts the version number for generating the file name
                                                            ///< for the distribution archive.

static const char *const DESCRIPTION = trNOOP("A DRM/KMS based output device");
                                               ///< vdr-plugin description.

static const char *const MAINMENUENTRY = trNOOP("DRMDevice");
                                               ///< what is displayed in the main menu entry

cDeviceMenu *cDeviceMenu::pMenu = NULL;  ///< Main Menu Instance

/** @} */

/*****************************************************************************
 * cPluginDrmDevice
 ****************************************************************************/

/**
 * cPluginDrmDevice constructor
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
cPluginDrmDevice::cPluginDrmDevice(void)
{
	m_pConfig = new cDeviceConfig();
	m_pDevice = new cOutputDevice(m_pConfig); // no need to delete m_pDevice, because VDR does it for us
}

/**
 * cPluginDrmDevice destructor
 *
 * Clean up after yourself!
 */
cPluginDrmDevice::~cPluginDrmDevice(void)
{
	delete m_pConfig;
}

/**
 * Return plugin version number
 *
 * @return the version number as constant string
 */
const char *cPluginDrmDevice::Version(void)
{
	return VERSION;
}

/**
 * Return plugin short description
 *
 * @return a short description as constant string
 */
const char *cPluginDrmDevice::Description(void)
{
	return tr(DESCRIPTION);
}

/**
 * Return a string that describes all known command line options
 *
 * @return the command line help as constant string
 */
const char *cPluginDrmDevice::CommandLineHelp(void)
{
	return "  -a device\taudio device (e.g. alsa: hw:0,0)\n"
	       "  -c channel\taudio mixer channel name (e.g. PCM)\n"
	       "  -o device\tdrm device (e.g. /dev/dri/card0)\n"
	       "  -d resolution\tdisplay resolution (e.g. 1920x1080@50)\n"
	       "  -g resolution\tosd resolution (e.g. 1920x1080)\n"
	       "  -D start in detached state\n"
	       "  -w workaround\tenable/disable workarounds\n"
#ifdef USE_GLES
	       "\tdisable-ogl-osd disable openGL osd\n"
#endif
	       "\tdisable-pip disable picture-in-picture\n"
	       "\n";
}

/**
 * Process the command line arguments.
 *
 * @param argc	number of arguments
 * @param argv	arguments vector
 */
bool cPluginDrmDevice::ProcessArgs(int argc, char *argv[])
{
//	LOGDEBUG("plugin: %s:", __FUNCTION__);

	//
	// Parse arguments.
	//

	for (;;) {
#ifdef USE_GLES
		switch (getopt(argc, argv, "-a:c:o:x:d:g:Dw:")) {
#else
		switch (getopt(argc, argv, "-a:c:o:x:d:g:D")) {
#endif
		case 'a':           // audio device for pcm
			m_pConfig->ConfigAudioPCMDevice = optarg;
			continue;
		case 'c':           // channel of audio mixer
			m_pConfig->ConfigAudioMixerChannel = optarg;
			continue;
		case 'o':           // set display drm device
			m_pConfig->ConfigDrmDevice = optarg;
			continue;
		case 'x':           // set display drm connector
			m_pConfig->ConfigDrmConnector = optarg;
			continue;
		case 'd': {         // set display output
			m_pConfig->ConfigDisplayResolution = optarg;
			int w = 0;
			int h = 0;
			double rate = 0.0;
			if (sscanf(m_pConfig->ConfigDisplayResolution, "%dx%d@%lf", &w, &h, &rate) != 3) {
				fprintf(stderr, gettext("Wring argument for option '%c'\n"), optopt);
				return 0;
			}
			m_pConfig->UserSetDrmMode = { w, h, rate, false };
			continue;
		}
		case 'g':           // set osd (rendering) size
			m_pConfig->ConfigOsdResolution = optarg;
			continue;
		case 'D':           // start plugin in detached state
			m_pDevice->SetStartDetached();
			continue;
		case 'w':           // workarounds
			if (!strcasecmp("disable-pip", optarg)) {
				m_pDevice->SetDisablePip();
#ifdef USE_GLES
			} else if (!strcasecmp("disable-ogl-osd", optarg)) {
				m_pDevice->SetDisableOglOsd();
#endif
			} else {
				fprintf(stderr, gettext("Workaround '%s' unsupported\n"),
				optarg);
				return 0;
			}
			continue;
		case EOF:
			break;
		case '-':
			fprintf(stderr, gettext("We need no long options\n"));
			return 0;
		case ':':
			fprintf(stderr, gettext("Missing argument for option '%c'\n"), optopt);
			return 0;
		default:
			fprintf(stderr, gettext("Unknown option '%c'\n"), optopt);
			return 0;
		}
		break;
	}

	while (optind < argc) {
		fprintf(stderr, gettext("Unhandled argument '%s'\n"), argv[optind++]);
	}

	return 1;
}

/**
 * Initializes the DVB devices
 *
 * Must be called before accessing any DVB functions
 */
bool cPluginDrmDevice::Initialize(void)
{
//	LOGDEBUG("plugin: %s:", __FUNCTION__);

	return m_pDevice->Initialize();
}

/**
 * Start any background activities the plugin shall perform
 */
bool cPluginDrmDevice::Start(void)
{
//	LOGDEBUG("plugin: %s:", __FUNCTION__);

	return m_pDevice->Start();
}

/**
 * Shutdown plugin
 *
 * Stop any background activities the plugin is performing
 */
void cPluginDrmDevice::Stop(void)
{
	//LOGDEBUG("plugin: %s:", __FUNCTION__);

	m_pDevice->Stop();
}

/**
 * Create main menu entry
 */
const char *cPluginDrmDevice::MainMenuEntry(void)
{
	//LOGDEBUG("plugin: %s:", __FUNCTION__);

	return m_pConfig->ConfigHideMainMenuEntry ? NULL : tr(MAINMENUENTRY);
}

/**
 * Perform the action when selected from the main VDR menu
 */
cOsdObject *cPluginDrmDevice::MainMenuAction(void)
{
	//LOGDEBUG("plugin: %s:", __FUNCTION__);

	return new cDeviceMenu("DRMDevice", m_pDevice);
}

/**
 * Return our setup menu
 */
cMenuSetupPage *cPluginDrmDevice::SetupMenu(void)
{
	//LOGDEBUG("plugin: %s:", __FUNCTION__);

	return new cDeviceSetupMenu(m_pDevice);
}

/*****************************************************************************
 * cPluginDrmDevice - Setup parameters
 ****************************************************************************/

/**
 * Parse setup parameters
 *
 * @param name      paramter name (case sensetive)
 * @param value     value as string
 *
 * @retval true     if the parameter is supported
 * @retval false    if the parameter is unsupported
 */
bool cPluginDrmDevice::SetupParse(const char *name, const char *value)
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
bool cPluginDrmDevice::Service(const char *id, void *data)
{
	//LOGDEBUG("plugin: %s: id %s", __FUNCTION__, id);
	(void)id;
	(void)data;

	return false;
}

/*****************************************************************************
 * cPluginDrmDevice - SVDRP
 ****************************************************************************/

/**
 * SVDRP commands help text
 *
 * @ingroup plugin
 */
static const char *SVDRPHelpText[] = {
	"PLAY Url\n" "    Play the media from the given url.\n",
	"DETA\n" "        Detach the plugin.\n",
	"ATTA\n" "        Attach the plugin.\n",
	"STAT\n" "        Get attached/detached status.\n"
	"    ATTACHED -> 910\n"
	"    DETACHED -> 911\n",
	"PION\n" "        Enable picture-in-picture.\n",
	"PIOF\n" "        Disable picture-in-picture.\n",
	"PITO\n" "        Toggle picture-in-picture.\n",
	"PIPU\n" "        Pip channel up.\n",
	"PIPD\n" "        Pip channel down.\n",
	"PIPC\n" "        Pip swap channels.\n",
	"PIPS\n" "        Pip switch main stream to pip channel and close pip.\n",
	"PIIP\n" "        Pip swap positions.\n",
	NULL
};

/**
 * Return SVDRP commands help pages
 *
 * return a pointer to a list of help strings for all of the plugin's
 * SVDRP commands.
 */
const char **cPluginDrmDevice::SVDRPHelpPages(void)
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
cString cPluginDrmDevice::SVDRPCommand(const char *command, const char *option, int &reply_code)
{
	// mediaplayer
	if (!strcasecmp(command, "PLAY")) {
		LOGDEBUG2(L_MEDIA, "plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
		cControl::Launch(new cDrmDeviceControl(option, m_pDevice));
		return "PLAY url";
	}

	// attach/detach
	if (!strcasecmp(command, "DETA")) {
		LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
		if (m_pDevice->IsDetached())
			return "DRMDevice is already detached";

		m_pDevice->Detach();
		return "Detached DRMDevice";
	}
	if (!strcasecmp(command, "ATTA")) {
		LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
		if (!m_pDevice->IsDetached())
			return "DRMDevice is not detached";

		m_pDevice->Attach();
		return "Attached DRMDevice";
	}
	if (!strcasecmp(command, "STAT")) {
		LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
		if (!m_pDevice->IsDetached()) {
			reply_code = 910;
			return "DRMDevice is attached";
		} else {
			reply_code = 911;
			return "DRMDevice is detached";
		}
	}

	// pip
	if (m_pDevice->UsePip()) {
		if (!strcasecmp(command, "PION")) {
			LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
			if (m_pDevice->PipIsEnabled())
				return "Pip is already enabled";

			m_pDevice->PipEnable();
			return "Pip was enabled";
		}
		if (!strcasecmp(command, "PIOF")) {
			LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
			if (!m_pDevice->PipIsEnabled())
				return "Pip isn't enabled";

			m_pDevice->PipDisable();
			return "Pip was disabled";
		}
		if (!strcasecmp(command, "PITO")) {
			LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
			if (!m_pDevice->PipIsEnabled()) {
				m_pDevice->PipEnable();
				return "Pip was enabled";
			} else {
				m_pDevice->PipDisable();
				return "Pip was disabled";
			}
		}
		if (!strcasecmp(command, "PIPU")) {
			LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
			if (!m_pDevice->PipIsEnabled())
				return "Pip isn't enabled";

			m_pDevice->PipChannelChange(1);
			return "Pip channel up";
		}
		if (!strcasecmp(command, "PIPD")) {
			LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
			if (!m_pDevice->PipIsEnabled())
				return "Pip isn't enabled";

			m_pDevice->PipChannelChange(-1);
			return "Pip channel down";
		}
		if (!strcasecmp(command, "PIPC")) {
			LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
			if (!m_pDevice->PipIsEnabled())
				return "Pip isn't enabled";

			m_pDevice->PipChannelSwap(false);
			return "Pip swap channels";
		}
		if (!strcasecmp(command, "PIPS")) {
			LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
			if (!m_pDevice->PipIsEnabled())
				return "Pip isn't enabled";

			m_pDevice->PipChannelSwap(true);
			return "Pip switch main stream to pip channel and close pip";
		}
		if (!strcasecmp(command, "PIPP")) {
			LOGDEBUG("plugin: %s: SVDRPCommand: %s %s", __FUNCTION__, command, option);
			if (!m_pDevice->PipIsEnabled())
				return "Pip isn't enabled";

			m_pDevice->PipSwapPosition();
			return "Pip swap position";
		}
	}

	return NULL;
}

VDRPLUGINCREATOR(cPluginDrmDevice);	// Don't touch this!
