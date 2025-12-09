/**
 * @file softhdsetupmenu.cpp
 * Setup menu class
 *
 * This file defines cMenuSetupSoft which describes the
 * setup menu and sets the config paramaters.
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

#include <vdr/plugin.h>

#include "logger.h"
#include "softhddevice.h"
#include "softhdsetupmenu.h"

/*****************************************************************************
 * cMenuSetupSoft - Setup menu
 ****************************************************************************/

/**
 * Create a seperator named item
 *
 * @param label       text inside separator
 */
static inline cOsdItem *SeparatorName(const char *label)
{
	return new cOsdItem(cString::sprintf("%s:", label), osUnknown, false);
}

/**
 * Create a collapsed item
 *
 * @param label     text inside collapsed
 * @param flag      flag handling collapsed or opened
 * @param msg       open message
 */
inline cOsdItem *cMenuSetupSoft::CollapsedItem(const char *label, int &flag, const char *msg)
{
	cOsdItem *item;

	item = new cMenuEditBoolItem(cString::sprintf("* %s", label), &flag,
		msg ? msg : tr("show"), tr("hide"));

	return item;
}

/**
 * Create setup menu.
 */
void cMenuSetupSoft::Create(void)
{
	int current;

	current = Current();	// get current menu item index
	Clear();            	// clear the menu

	//
	// General
	//
	Add(CollapsedItem(tr("General"), m_cGeneral));
	if (m_cGeneral) {
		Add(new cMenuEditBoolItem(tr("Hide main menu entry"), &m_cHideMainMenuEntry, trVDR("no"), trVDR("yes")));
#ifdef USE_GLES
		if (!m_pConfig->ConfigDisableOglOsd) {
			Add(new cMenuEditIntItem(tr("GPU mem used for image caching (MB)"), &m_cMaxSizeGPUImageCache, 0, 4000));
		}
#endif

		Add(new cMenuEditIntItem(tr("Additional buffer size (ms)"), &m_cAdditionalBufferLengthMs, 0, 1000));
	}

	//
	// Statistics
	//
	Add(CollapsedItem(tr("Statistics"), m_cStatistics));
	if (m_cStatistics) {
		int duped;
		int dropped;
		int counter;
		m_pDevice->GetStats(&duped, &dropped, &counter);
		Add(new cOsdItem(cString::sprintf(tr(" Frames duped(%d) dropped(%d) total(%d)"), duped, dropped, counter), osUnknown, false));
#ifdef USE_GLES
		Add(new cOsdItem(cString::sprintf(tr(" OSD: Using %s rendering"), m_pConfig->ConfigDisableOglOsd ? "software" : "hardware"), osUnknown, false));
#else
		Add(new cOsdItem(cString::sprintf(tr(" OSD: Using software rendering")), osUnknown, false));
#endif
	}

#ifdef USE_GLES
#ifdef WRITE_PNG
	//
	//	debug
	//
	if (!m_pConfig->ConfigDisableOglOsd) {
		Add(CollapsedItem(tr("Debug"), m_cDebugMenu));
		if (m_cDebugMenu) {
			Add(new cMenuEditBoolItem(tr("Write OSD to file"), &m_cWritePngs, trVDR("no"), trVDR("yes")));
		}
	}
#endif
#endif

	//
	// Logging
	//
	Add(CollapsedItem(tr("Logging"), m_cLogging));
	if (m_cLogging) {
		Add(new cMenuEditBoolItem(tr("Enable logging"), &m_cLogDefault, trVDR("off"), trVDR("on")));
		if (m_cLogDefault) {
			Add(new cMenuEditBoolItem(tr("\040\040Standard debug logs"), &m_cLogDebug_, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040AV Sync debug logs"), &m_cLogAVSync, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040Sound debug logs"), &m_cLogSound, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040OSD debug logs"), &m_cLogOSD, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040DRM debug logs"), &m_cLogDRM, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040Codec debug logs"), &m_cLogCodec, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040Stillpicture debug logs"), &m_cLogStill, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040Trickspeed debug logs"), &m_cLogTrick, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040Mediaplayer debug logs"), &m_cLogMedia, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040OpenGL OSD debug logs"), &m_cLogGL, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040OpenGL OSD time measurement"), &m_cLogGLTime, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040OpenGL OSD time measurement (extensive)"), &m_cLogGLTimeAll, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040Packet tracking logs"), &m_cLogPacket, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040Grabbing debug logs"), &m_cLogGrab, trVDR("no"), trVDR("yes")));
		}
	}

	//
	// Video
	//
	Add(CollapsedItem(tr("Video"), m_cVideoMenu));
	if (m_cVideoMenu) {
		Add(new cMenuEditBoolItem(tr("Disable deinterlacer"), &m_cDisableDeint, trVDR("no"), trVDR("yes")));
		Add(SeparatorName(tr("Picture-in-picture")));
		Add(new cMenuEditIntItem(tr(" video scaling factor (%)"), &m_cPipScalePercent, 10, 100));
		Add(new cMenuEditIntItem(tr(" video left (%)"), &m_cPipLeftPercent, 0, 100));
		Add(new cMenuEditIntItem(tr(" video top (%)"), &m_cPipTopPercent, 0, 100));
		Add(new cMenuEditBoolItem(tr(" use alternative position as default"), &m_cPipUseAlt, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditIntItem(tr(" alternative video scaling factor (%)"), &m_cPipAltScalePercent, 10, 100));
		Add(new cMenuEditIntItem(tr(" alternative video left (%)"), &m_cPipAltLeftPercent, 0, 100));
		Add(new cMenuEditIntItem(tr(" alternative video top (%)"), &m_cPipAltTopPercent, 0, 100));
	}

	//
	// Audio
	//
	Add(CollapsedItem(tr("Audio"), m_cAudio));
	if (m_cAudio) {
		Add(new cMenuEditIntItem(tr("Audio/Video delay (ms)"), &m_cAudioDelay, -1000, 1000));
		Add(new cMenuEditBoolItem(tr("Volume control"), &m_cAudioSoftvol, tr("Hardware"), tr("Software")));
		Add(new cMenuEditBoolItem(tr("Enable normalize volume"), &m_cAudioNormalize, trVDR("no"), trVDR("yes")));
		if (m_cAudioNormalize)
			Add(new cMenuEditIntItem(tr("  Max normalize factor (/1000)"), &m_cAudioMaxNormalize, 0, 10000));
		Add(new cMenuEditBoolItem(tr("Enable volume compression"), &m_cAudioCompression, trVDR("no"), trVDR("yes")));
		if (m_cAudioCompression)
			Add(new cMenuEditIntItem(tr("  Max compression factor (/1000)"), &m_cAudioMaxCompression, 0, 10000));
		Add(new cMenuEditIntItem(tr("Reduce stereo volume (/1000)"), &m_cAudioStereoDescent, 0, 1000));
		Add(new cMenuEditBoolItem(tr("Enable Stereo downmix"), &m_cAudioDownmix, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("Enable Pass-through"), &m_cAudioPassthroughDefault, trVDR("off"), trVDR("on")));
		if (m_cAudioPassthroughDefault) {
			Add(new cMenuEditBoolItem(tr("\040\040PCM pass-through"), &m_cAudioPassthroughPCM, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040AC-3 pass-through"), &m_cAudioPassthroughAC3, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040E-AC-3 pass-through"), &m_cAudioPassthroughEAC3, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("\040\040DTS pass-through"), &m_cAudioPassthroughDTS, trVDR("no"), trVDR("yes")));
			Add(new cMenuEditBoolItem(tr("Enable automatic AES"), &m_cAudioAutoAES, trVDR("no"), trVDR("yes")));
		}
	}

	//
	// Audio filter
	//
	Add(CollapsedItem(tr("Audio Filter"), m_cAudioFilter));
	if (m_cAudioFilter) {
		Add(new cMenuEditBoolItem(tr(" Enable Audio Equalizer"), &m_cAudioEq, trVDR("no"), trVDR("yes")));
		if (m_cAudioEq) {
			Add(new cMenuEditIntItem(tr("  60 Hz band gain"),   &m_cAudioEqBand[0], -15, 1));
			Add(new cMenuEditIntItem(tr("  72 Hz band gain"),   &m_cAudioEqBand[1], -15, 1));
			Add(new cMenuEditIntItem(tr("  107 Hz band gain"),   &m_cAudioEqBand[2], -15, 1));
			Add(new cMenuEditIntItem(tr("  150 Hz band gain"),   &m_cAudioEqBand[3], -15, 1));
			Add(new cMenuEditIntItem(tr("  220 Hz band gain"),   &m_cAudioEqBand[4], -15, 1));
			Add(new cMenuEditIntItem(tr("  310 Hz band gain"),   &m_cAudioEqBand[5], -15, 1));
			Add(new cMenuEditIntItem(tr("  430 Hz band gain"),   &m_cAudioEqBand[6], -15, 1));
			Add(new cMenuEditIntItem(tr("  620 Hz band gain"),   &m_cAudioEqBand[7], -15, 1));
			Add(new cMenuEditIntItem(tr("  860 Hz band gain"),   &m_cAudioEqBand[8], -15, 1));
			Add(new cMenuEditIntItem(tr("  1200 Hz band gain"),  &m_cAudioEqBand[9], -15, 1));
			Add(new cMenuEditIntItem(tr("  1700 Hz band gain"),  &m_cAudioEqBand[10], -15, 1));
			Add(new cMenuEditIntItem(tr("  2500 Hz band gain"),  &m_cAudioEqBand[11], -15, 1));
			Add(new cMenuEditIntItem(tr("  3500 Hz band gain"),  &m_cAudioEqBand[12], -15, 1));
			Add(new cMenuEditIntItem(tr("  4800 Hz band gain"),  &m_cAudioEqBand[13], -15, 1));
			Add(new cMenuEditIntItem(tr("  7000 Hz band gain"),  &m_cAudioEqBand[14], -15, 1));
			Add(new cMenuEditIntItem(tr("  9500 Hz band gain"),  &m_cAudioEqBand[15], -15, 1));
			Add(new cMenuEditIntItem(tr("  13500 Hz band gain"), &m_cAudioEqBand[16], -15, 1));
			Add(new cMenuEditIntItem(tr("  17200 Hz band gain"), &m_cAudioEqBand[17], -15, 1));
		}
	}

	SetCurrent(Get(current));	// restore selected menu entry
	Display();               	// display build menu
}

/**
 * Process key for setup menu.
 *
 * @param key          pressed key
 */
eOSState cMenuSetupSoft::ProcessKey(eKeys key)
{
	int old_cGeneral = m_cGeneral;
#ifdef USE_GLES
#ifdef WRITE_PNG
	int old_cDebugMenu = m_cDebugMenu;
#endif
#endif
	int old_cStatistics = m_cStatistics;
	int old_cLogging = m_cLogging;
	int old_cLogDefault = m_cLogDefault;
	int old_cVideoMenu = m_cVideoMenu;
	int old_cAudio = m_cAudio;
	int old_cAudioNormalize = m_cAudioNormalize;
	int old_cAudioCompression = m_cAudioCompression;
	int old_cAudioPassthroughDefault = m_cAudioPassthroughDefault;
	int old_cAudioFilter = m_cAudioFilter;
	int old_cAudioEq = m_cAudioEq;

	eOSState state = cMenuSetupPage::ProcessKey(key);

	if (key != kNone) {
		// update menu only, if something on the structure has changed
		// this is needed because VDR menus are evil slow
		if (old_cGeneral                 != m_cGeneral ||
#ifdef USE_GLES
#ifdef WRITE_PNG
		    old_cDebugMenu               != m_cDebugMenu ||
#endif
#endif
		    old_cStatistics              != m_cStatistics ||
		    old_cLogging                 != m_cLogging ||
		    old_cLogDefault              != m_cLogDefault ||
		    old_cVideoMenu               != m_cVideoMenu ||
		    old_cAudio                   != m_cAudio ||
		    old_cAudioFilter             != m_cAudioFilter ||
		    old_cAudioEq                 != m_cAudioEq ||
		    old_cAudioNormalize          != m_cAudioNormalize ||
		    old_cAudioCompression        != m_cAudioCompression ||
		    old_cAudioPassthroughDefault != m_cAudioPassthroughDefault) {

			Create();	// update menu
		}
	}

	return state;
}

/**
 * cMenuSetupSoft constructor
 *
 * Import global config variables into setup
 */
cMenuSetupSoft::cMenuSetupSoft(cSoftHdDevice *device)
{
	m_pDevice = device;
	m_pAudioDevice = m_pDevice->Audio();
	m_pConfig = m_pDevice->Config();

	//
	// General
	//
	m_cGeneral = 0;
	m_cHideMainMenuEntry = m_pConfig->ConfigHideMainMenuEntry;
#ifdef USE_GLES
	m_cMaxSizeGPUImageCache = m_pConfig->ConfigMaxSizeGPUImageCache;
#endif
	m_cAdditionalBufferLengthMs= m_pConfig->ConfigAdditionalBufferLengthMs;

	//
	//	Debug
	//
#ifdef USE_GLES
#ifdef WRITE_PNG
	m_cDebugMenu = 0;
	m_cWritePngs = m_pConfig->ConfigWritePngs;
#endif
#endif

	//
	// Statistics
	//
	m_cStatistics = 0;

	//
	// Logging
	//
	m_cLogging = 0;
	m_cLogDefault   = m_pConfig->ConfigLogState;
	m_cLogDebug_    = m_pConfig->ConfigLogLevels & L_DEBUG;
	m_cLogAVSync    = m_pConfig->ConfigLogLevels & L_AV_SYNC;
	m_cLogSound     = m_pConfig->ConfigLogLevels & L_SOUND;
	m_cLogOSD       = m_pConfig->ConfigLogLevels & L_OSD;
	m_cLogDRM       = m_pConfig->ConfigLogLevels & L_DRM;
	m_cLogCodec     = m_pConfig->ConfigLogLevels & L_CODEC;
	m_cLogStill     = m_pConfig->ConfigLogLevels & L_STILL;
	m_cLogTrick     = m_pConfig->ConfigLogLevels & L_TRICK;
	m_cLogMedia     = m_pConfig->ConfigLogLevels & L_MEDIA;
	m_cLogGL        = m_pConfig->ConfigLogLevels & L_OPENGL;
	m_cLogGLTime    = m_pConfig->ConfigLogLevels & L_OPENGL_TIME;
	m_cLogGLTimeAll = m_pConfig->ConfigLogLevels & L_OPENGL_TIME_ALL;
	m_cLogPacket    = m_pConfig->ConfigLogLevels & L_PACKET;
	m_cLogGrab      = m_pConfig->ConfigLogLevels & L_GRAB;

	//
	// Video
	//
	m_cVideoMenu = 0;
	m_cDisableDeint = m_pConfig->ConfigDisableDeint;
	m_cPipScalePercent = m_pConfig->ConfigPipScalePercent;
	m_cPipLeftPercent = m_pConfig->ConfigPipLeftPercent;
	m_cPipTopPercent = m_pConfig->ConfigPipTopPercent;
	m_cPipUseAlt = m_pConfig->ConfigPipUseAlt;
	m_cPipAltScalePercent = m_pConfig->ConfigPipAltScalePercent;
	m_cPipAltLeftPercent = m_pConfig->ConfigPipAltLeftPercent;
	m_cPipAltTopPercent = m_pConfig->ConfigPipAltTopPercent;

	//
	// Audio
	//
	m_cAudio = 0;
	m_cAudioDelay              = m_pConfig->ConfigVideoAudioDelayMs;
	m_cAudioSoftvol            = m_pConfig->ConfigAudioSoftvol;
	m_cAudioNormalize          = m_pConfig->ConfigAudioNormalize;
	m_cAudioMaxNormalize       = m_pConfig->ConfigAudioMaxNormalize;
	m_cAudioCompression        = m_pConfig->ConfigAudioCompression;
	m_cAudioMaxCompression     = m_pConfig->ConfigAudioMaxCompression;
	m_cAudioStereoDescent      = m_pConfig->ConfigAudioStereoDescent;
	m_cAudioDownmix            = m_pConfig->ConfigAudioDownmix;
	m_cAudioPassthroughDefault = m_pConfig->ConfigAudioPassthroughState;
	m_cAudioPassthroughPCM     = m_pConfig->ConfigAudioPassthroughMask & CODEC_PCM;
	m_cAudioPassthroughAC3     = m_pConfig->ConfigAudioPassthroughMask & CODEC_AC3;
	m_cAudioPassthroughEAC3    = m_pConfig->ConfigAudioPassthroughMask & CODEC_EAC3;
	m_cAudioPassthroughDTS     = m_pConfig->ConfigAudioPassthroughMask & CODEC_DTS;
	m_cAudioAutoAES            = m_pConfig->ConfigAudioAutoAES;

	//
	// Audio filter
	//
	m_cAudioEq = m_pConfig->ConfigAudioEq;
	m_cAudioFilter = 0;
	for (int i = 0; i < 18; i++) {
		m_cAudioEqBand[i] = m_pConfig->ConfigAudioEqBand[i];
	}

	Create();
}

/**
 * Store setup
 */
void cMenuSetupSoft::Store(void)
{
	//
	// General
	//
	SetupStore("HideMainMenuEntry", m_pConfig->ConfigHideMainMenuEntry = m_cHideMainMenuEntry);
#ifdef USE_GLES
	SetupStore("MaxSizeGPUImageCache", m_pConfig->ConfigMaxSizeGPUImageCache = m_cMaxSizeGPUImageCache);
#endif
	SetupStore("AdditionalBufferLengthMs", m_pConfig->ConfigAdditionalBufferLengthMs = m_cAdditionalBufferLengthMs);

	//
	// Debug
	//
#ifdef USE_GLES
#ifdef WRITE_PNG
	m_pConfig->ConfigWritePngs = m_cWritePngs;
	SetupStore("WritePngs", m_pConfig->ConfigWritePngs);
#endif
#endif

	//
	// Logging
	//
	m_pConfig->ConfigLogLevels =
		(m_cLogDebug_    ? L_DEBUG : 0) |
		(m_cLogAVSync    ? L_AV_SYNC : 0) |
		(m_cLogSound     ? L_SOUND : 0) |
		(m_cLogOSD       ? L_OSD : 0) |
		(m_cLogDRM       ? L_DRM : 0) |
		(m_cLogCodec     ? L_CODEC : 0) |
		(m_cLogStill     ? L_STILL : 0) |
		(m_cLogTrick     ? L_TRICK : 0) |
		(m_cLogMedia     ? L_MEDIA : 0) |
		(m_cLogGL        ? L_OPENGL : 0) |
		(m_cLogGLTime    ? L_OPENGL_TIME : 0) |
		(m_cLogGLTimeAll ? L_OPENGL_TIME_ALL : 0) |
		(m_cLogPacket    ? L_PACKET : 0) |
		(m_cLogGrab      ? L_GRAB : 0);
	m_pConfig->ConfigLogState = m_cLogDefault;

	if (m_pConfig->ConfigLogState) {
		SetupStore("LogLevel", m_pConfig->ConfigLogLevels);
		m_pConfig->PrintLogLevel(m_pConfig->ConfigLogLevels);
		cSoftHdLogger::GetLogger()->SetLogLevel(m_pConfig->ConfigLogLevels);
	} else {
		SetupStore("LogLevel", -m_pConfig->ConfigLogLevels);
		cSoftHdLogger::GetLogger()->SetLogLevel(0);
	}

	//
	// Video
	//
	SetupStore("DisableDeint", m_pConfig->ConfigDisableDeint = m_cDisableDeint);
	if (m_pConfig->ConfigDisableDeint) {
		LOGDEBUG("Disable deinterlacer!");
	}
	m_pDevice->SetDisableDeint();

	// pip
	SetupStore("PipScalePercent", m_pConfig->ConfigPipScalePercent = m_cPipScalePercent);
	SetupStore("PipLeftPercent", m_pConfig->ConfigPipLeftPercent = m_cPipLeftPercent);
	SetupStore("PipTopPercent", m_pConfig->ConfigPipTopPercent = m_cPipTopPercent);
	SetupStore("PipUseAlt", m_pConfig->ConfigPipUseAlt = m_cPipUseAlt);
	SetupStore("PipAltScalePercent", m_pConfig->ConfigPipAltScalePercent = m_cPipAltScalePercent);
	SetupStore("PipAltLeftPercent", m_pConfig->ConfigPipAltLeftPercent = m_cPipAltLeftPercent);
	SetupStore("PipAltTopPercent", m_pConfig->ConfigPipAltTopPercent = m_cPipAltTopPercent);
	m_pDevice->PipSetSize();

	//
	// Audio
	//
	SetupStore("AudioDelay", m_pConfig->ConfigVideoAudioDelayMs = m_cAudioDelay);
	SetupStore("AudioSoftvol", m_pConfig->ConfigAudioSoftvol = m_cAudioSoftvol);
	m_pAudioDevice->SetSoftvol(m_pConfig->ConfigAudioSoftvol);
	SetupStore("AudioNormalize", m_pConfig->ConfigAudioNormalize = m_cAudioNormalize);
	SetupStore("AudioMaxNormalize", m_pConfig->ConfigAudioMaxNormalize = m_cAudioMaxNormalize);
	m_pAudioDevice->SetNormalize(m_pConfig->ConfigAudioNormalize, m_pConfig->ConfigAudioMaxNormalize);
	SetupStore("AudioCompression", m_pConfig->ConfigAudioCompression = m_cAudioCompression);
	SetupStore("AudioMaxCompression", m_pConfig->ConfigAudioMaxCompression = m_cAudioMaxCompression);
	m_pAudioDevice->SetCompression(m_pConfig->ConfigAudioCompression, m_pConfig->ConfigAudioMaxCompression);
	SetupStore("AudioStereoDescent", m_pConfig->ConfigAudioStereoDescent = m_cAudioStereoDescent);
	m_pAudioDevice->SetStereoDescent(m_pConfig->ConfigAudioStereoDescent);
	SetupStore("AudioDownmix", m_pConfig->ConfigAudioDownmix = m_cAudioDownmix);
	m_pAudioDevice->SetDownmix(m_pConfig->ConfigAudioDownmix);
	// FIXME: can handle more audio state changes here
	// downmix changed reset audio, to get change direct
	if (m_pConfig->ConfigAudioDownmix != m_cAudioDownmix) {
		m_pDevice->ResetChannelId();
	}
	m_pConfig->ConfigAudioPassthroughMask = (m_cAudioPassthroughPCM ? CODEC_PCM : 0)
	                                      | (m_cAudioPassthroughAC3 ? CODEC_AC3 : 0)
	                                      | (m_cAudioPassthroughEAC3 ? CODEC_EAC3 : 0)
	                                      | (m_cAudioPassthroughDTS ? CODEC_DTS : 0);
	m_pConfig->ConfigAudioPassthroughState = m_cAudioPassthroughDefault;
	if (m_pConfig->ConfigAudioPassthroughState) {
		SetupStore("AudioPassthrough", m_pConfig->ConfigAudioPassthroughMask);
		m_pDevice->SetPassthrough(m_pConfig->ConfigAudioPassthroughMask);
	} else {
		SetupStore("AudioPassthrough", -m_pConfig->ConfigAudioPassthroughMask);
		m_pDevice->SetPassthrough(0);
	}
	SetupStore("AudioAutoAES", m_pConfig->ConfigAudioAutoAES = m_cAudioAutoAES);
	m_pAudioDevice->SetAutoAES(m_pConfig->ConfigAudioAutoAES);

	//
	// Audio filter
	//
	SetupStore("AudioEq", m_pConfig->ConfigAudioEq = m_cAudioEq);
	SetupStore("AudioEqBand01b", m_pConfig->ConfigAudioEqBand[0]  = m_cAudioEqBand[0]);
	SetupStore("AudioEqBand02b", m_pConfig->ConfigAudioEqBand[1]  = m_cAudioEqBand[1]);
	SetupStore("AudioEqBand03b", m_pConfig->ConfigAudioEqBand[2]  = m_cAudioEqBand[2]);
	SetupStore("AudioEqBand04b", m_pConfig->ConfigAudioEqBand[3]  = m_cAudioEqBand[3]);
	SetupStore("AudioEqBand05b", m_pConfig->ConfigAudioEqBand[4]  = m_cAudioEqBand[4]);
	SetupStore("AudioEqBand06b", m_pConfig->ConfigAudioEqBand[5]  = m_cAudioEqBand[5]);
	SetupStore("AudioEqBand07b", m_pConfig->ConfigAudioEqBand[6]  = m_cAudioEqBand[6]);
	SetupStore("AudioEqBand08b", m_pConfig->ConfigAudioEqBand[7]  = m_cAudioEqBand[7]);
	SetupStore("AudioEqBand09b", m_pConfig->ConfigAudioEqBand[8]  = m_cAudioEqBand[8]);
	SetupStore("AudioEqBand10b", m_pConfig->ConfigAudioEqBand[9]  = m_cAudioEqBand[9]);
	SetupStore("AudioEqBand11b", m_pConfig->ConfigAudioEqBand[10] = m_cAudioEqBand[10]);
	SetupStore("AudioEqBand12b", m_pConfig->ConfigAudioEqBand[11] = m_cAudioEqBand[11]);
	SetupStore("AudioEqBand13b", m_pConfig->ConfigAudioEqBand[12] = m_cAudioEqBand[12]);
	SetupStore("AudioEqBand14b", m_pConfig->ConfigAudioEqBand[13] = m_cAudioEqBand[13]);
	SetupStore("AudioEqBand15b", m_pConfig->ConfigAudioEqBand[14] = m_cAudioEqBand[14]);
	SetupStore("AudioEqBand16b", m_pConfig->ConfigAudioEqBand[15] = m_cAudioEqBand[15]);
	SetupStore("AudioEqBand17b", m_pConfig->ConfigAudioEqBand[16] = m_cAudioEqBand[16]);
	SetupStore("AudioEqBand18b", m_pConfig->ConfigAudioEqBand[17] = m_cAudioEqBand[17]);
	m_pAudioDevice->SetEq(m_pConfig->ConfigAudioEqBand, m_pConfig->ConfigAudioEq);
}
