/**
 * @file softhdmenu.h
 * Setup menu class header file
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

#ifndef __SOFTHDMENU_H
#define __SOFTHDMENU_H

#include "softhddevice.h"
#include "softhdmenu.h"

class cSoftHdDevice;

/*****************************************************************************
 * cMenuSetupSoft - Setup menu
 ****************************************************************************/

/**
 * cMenuSetupSoft - SoftHdDevice plugin menu setup page class
 */
class cMenuSetupSoft:public cMenuSetupPage
{
public:
	cMenuSetupSoft(cSoftHdDevice *);
	virtual eOSState ProcessKey(eKeys);

protected:
	// local copies of global setup variables:

	// General
	int m_cGeneral;
	int m_cHideMainMenuEntry;
#ifdef USE_GLES
	int m_cMaxSizeGPUImageCache;
#endif

	// Statistics
	int m_cStatistics;

	// Debug
#ifdef USE_GLES
#ifdef WRITE_PNG
	int m_cDebugMenu;
	int m_cWritePngs;
//	const char *m_cPngVariant[4];
#endif
#endif

	// Logging
	int m_cLogging;
	int m_cLogDefault;
	int m_cLogDebug_;
	int m_cLogAVSync;
	int m_cLogSound;
	int m_cLogOSD;
	int m_cLogDRM;
	int m_cLogCodec;
	int m_cLogStill;
	int m_cLogTrick;
	int m_cLogMedia;
	int m_cLogGL;
	int m_cLogGLTime;
	int m_cLogGLTimeAll;
	int m_cLogPacket;
	int m_cLogGrab;

	// Video
	int m_cVideoMenu;
	int m_cDisableDeint;

	// Audio
	int m_cAudio;
	int m_cAudioDelay;
	int m_cAudioSoftvol;
	int m_cAudioBufferTime;
	int m_cAudioNormalize;
	int m_cAudioMaxNormalize;
	int m_cAudioCompression;
	int m_cAudioMaxCompression;
	int m_cAudioStereoDescent;
	int m_cAudioDownmix;
	int m_cAudioPassthroughDefault;
	int m_cAudioPassthroughPCM;
	int m_cAudioPassthroughAC3;
	int m_cAudioPassthroughEAC3;
	int m_cAudioPassthroughDTS;
	int m_cAudioAutoAES;
	int m_cAudioFilter;
	int m_cAudioEq;
	int m_cAudioEqBand[18];

private:
	cSoftHdDevice *m_pDevice;
	cSoftHdConfig *m_pConfig;
	cSoftHdAudio *m_pAudioDevice;

	inline cOsdItem * CollapsedItem(const char *, int &, const char * = NULL);
	void Create(void);

protected:
	virtual void Store(void);
};

#endif
