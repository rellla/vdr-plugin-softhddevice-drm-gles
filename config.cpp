/**
 * @file softhdconfig.cpp
 * SoftHdDevice config class
 *
 * This file defines cSoftHdConfig, which is used to keep all
 * the config settings, which are set via setup.conf, commandline
 * or setup menu.
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

#include "config.h"

/*****************************************************************************
 * cSoftHdConfig - Setup parameters
 ****************************************************************************/

/**
 * cSoftHdConfig destructor
 */
cSoftHdConfig::~cSoftHdConfig(void)
{
}

/**
 * Parse setup parameters
 *
 * @param name      paramter name (case sensetive)
 * @param value     value as string
 *
 * @returns         true if the parameter is supported, false otherwise
 */
bool cSoftHdConfig::SetupParse(const char *name, const char *value, cSoftHdDevice *device, cSoftHdAudio *audio)
{
	//LOGDEBUG("config: %s: '%s' = '%s'", __FUNCTION__, name, value);

	if        (!strcasecmp(name, "MakePrimary"))           { ConfigMakePrimary = atoi(value);
#ifdef USE_GLES
#ifdef WRITE_PNG
	} else if (!strcasecmp(name, "WritePngs"))             { ConfigWritePngs = atoi(value);
#endif
#endif
	} else if (!strcasecmp(name, "HideMainMenuEntry"))     { ConfigHideMainMenuEntry = atoi(value);
	} else if (!strcasecmp(name, "LogLevel"))              { ConfigLog = abs(atoi(value));
		                                                     LogState = atoi(value) > 0;
		                                                     	SetLogState();
	} else if (!strcasecmp(name, "DisableDeint"))          { ConfigDisableDeint = atoi(value);
		                                                     	device->SetDisableDeint();
	} else if (!strcasecmp(name, "AudioDelay"))            { ConfigVideoAudioDelay = atoi(value);
		                                                     	device->SetVideoAudioDelay(ConfigVideoAudioDelay);
	} else if (!strcasecmp(name, "AudioPassthrough"))      { AudioPassthroughState = atoi(value) > 0;
		                                                     ConfigAudioPassthrough = abs(atoi(value));
		                                                     	SetPassthrough(device);
	} else if (!strcasecmp(name, "AudioDownmix"))          { ConfigAudioDownmix = atoi(value);
		                                                     	audio->SetDownmix(ConfigAudioDownmix);
	} else if (!strcasecmp(name, "AudioSoftvol"))          { ConfigAudioSoftvol = atoi(value),
		                                                     	audio->SetSoftvol(ConfigAudioSoftvol);
	} else if (!strcasecmp(name, "AudioNormalize"))        { ConfigAudioNormalize = atoi(value);
		                                                     	audio->SetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
	} else if (!strcasecmp(name, "AudioMaxNormalize"))     { ConfigAudioMaxNormalize = atoi(value);
		                                                     	audio->SetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
	} else if (!strcasecmp(name, "AudioCompression"))      { ConfigAudioCompression = atoi(value);
		                                                     	audio->SetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
	} else if (!strcasecmp(name, "AudioMaxCompression"))   { ConfigAudioMaxCompression = atoi(value);
		                                                     	audio->SetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
	} else if (!strcasecmp(name, "AudioStereoDescent"))    { ConfigAudioStereoDescent = atoi(value);
		                                                     	audio->SetStereoDescent(ConfigAudioStereoDescent);
	} else if (!strcasecmp(name, "AudioBufferTime"))       { ConfigAudioBufferTime = atoi(value);
	} else if (!strcasecmp(name, "AudioAutoAES"))          { ConfigAudioAutoAES = atoi(value);
		                                                     	audio->SetAutoAES(ConfigAudioAutoAES);
	} else if (!strcasecmp(name, "AudioEq"))               { ConfigAudioEq = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand01b"))        { SetupAudioEqBand[0] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand02b"))        { SetupAudioEqBand[1] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand03b"))        { SetupAudioEqBand[2] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand04b"))        { SetupAudioEqBand[3] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand05b"))        { SetupAudioEqBand[4] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand06b"))        { SetupAudioEqBand[5] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand07b"))        { SetupAudioEqBand[6] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand08b"))        { SetupAudioEqBand[7] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand09b"))        { SetupAudioEqBand[8] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand10b"))        { SetupAudioEqBand[9] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand11b"))        { SetupAudioEqBand[10] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand12b"))        { SetupAudioEqBand[11] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand13b"))        { SetupAudioEqBand[12] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand14b"))        { SetupAudioEqBand[13] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand15b"))        { SetupAudioEqBand[14] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand16b"))        { SetupAudioEqBand[15] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand17b"))        { SetupAudioEqBand[16] = atoi(value);
	} else if (!strcasecmp(name, "AudioEqBand18b"))        { SetupAudioEqBand[17] = atoi(value);
		                                                     	audio->SetEq(SetupAudioEqBand, ConfigAudioEq);
#ifdef USE_GLES
	} else if (!strcasecmp(name, "MaxSizeGPUImageCache"))  { ConfigMaxSizeGPUImageCache = atoi(value);
#endif
	} else
		return false;

	return true;
}

void cSoftHdConfig::PrintLogLevel(int loglevel)
{
	if (!loglevel)
		return;

	char prefix[256] = "Set loglevels:";
	if (loglevel & L_DEBUG)
		strcat(prefix, " standard debugs,");
	if (loglevel & L_AV_SYNC)
		strcat(prefix, " AV-Sync,");
	if (loglevel & L_SOUND)
		strcat(prefix, " sound,");
	if (loglevel & L_OSD)
		strcat(prefix, " osd,");
	if (loglevel & L_DRM)
		strcat(prefix, " drm,");
	if (loglevel & L_CODEC)
		strcat(prefix, " codec,");
	if (loglevel & L_STILL)
		strcat(prefix, " stillpicture,");
	if (loglevel & L_TRICK)
		strcat(prefix, " trickspeed,");
	if (loglevel & L_MEDIA)
		strcat(prefix, " mediaplayer,");
	if ((loglevel & L_OPENGL) ||
	    (loglevel & L_OPENGL_TIME) ||
	    (loglevel & L_OPENGL_TIME_ALL))
		strcat(prefix, " OpenGL OSD,");
	if (loglevel & L_PACKET)
		strcat(prefix, " packet tracking,");
	if (loglevel & L_GRAB)
		strcat(prefix, " grabbing");

	LOGINFO("%s", prefix);
}

void cSoftHdConfig::SetLogState(void)
{
	if (LogState) {
		PrintLogLevel(ConfigLog);
		cSoftHdLogger::GetLogger()->SetLogLevel(ConfigLog);
	} else {
		PrintLogLevel(0);
		cSoftHdLogger::GetLogger()->SetLogLevel(0);
	}
}

void cSoftHdConfig::SetPassthrough(cSoftHdDevice *device)
{
	if (AudioPassthroughState) {
		device->SetPassthrough(ConfigAudioPassthrough);
	} else {
		device->SetPassthrough(0);
	}
}
