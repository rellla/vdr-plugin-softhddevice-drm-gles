///
///	@file softhddevice.cpp	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011 - 2015 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 zille.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

#define __STDC_CONSTANT_MACROS		///< needed for ffmpeg UINT64_C

#include <string>
using std::string;
#include <fstream>
using std::ifstream;

#include <vdr/player.h>
#include <vdr/plugin.h>
#include <vdr/dvbspu.h>

#include "softhddevice-drm-gles.h"
#include "softhddevice_service.h"
#include "mediaplayer.h"
#include "misc.h"

#ifdef USE_GLES
#include "openglosd.h"
#endif

extern "C"
{
#include <libavcodec/avcodec.h>

#include "softhddev.h"
#include "audio.h"
#include "video.h"
#include "codec.h"
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Sets this OSD to be the active one.
**
**	@param on	true on, false off
**
**	@note only needed as workaround for text2skin plugin with
**	undrawn areas.
*/
void cSoftOsd::SetActive(bool on)
{
    Debug2(L_OSD, "OSD %s: %d level %d", __FUNCTION__, on, OsdLevel);

    if (Active() == on) {
	return;				// already active, no action
    }
    cOsd::SetActive(on);

    if (on) {
	Dirty = 1;
	// only flush here if there are already bitmaps
	if (GetBitmap(0)) {
	    Flush();
	}
    } else {
	OsdClose();
    }
}

/**
**	Constructor OSD.
**
**	Initializes the OSD with the given coordinates.
**
**	@param left	x-coordinate of osd on display
**	@param top	y-coordinate of osd on display
**	@param level	level of the osd (smallest is shown)
*/
cSoftOsd::cSoftOsd(int left, int top, uint level)
:cOsd(left, top, level)
{
    /* FIXME: OsdWidth/OsdHeight not correct!
     */
    Debug2(L_OSD, "OSD %s: %dx%d%+d%+d, %d", __FUNCTION__, OsdWidth(),
	OsdHeight(), left, top, level);

    OsdLevel = level;
}

/**
**	OSD Destructor.
**
**	Shuts down the OSD.
*/
cSoftOsd::~cSoftOsd(void)
{
    Debug2(L_OSD, "OSD %s: level %d", __FUNCTION__, OsdLevel);

    SetActive(false);
    // done by SetActive: OsdClose();
}

/**
**	Set the sub-areas to the given areas
*/
eOsdError cSoftOsd::SetAreas(const tArea * areas, int n)
{
    Debug2(L_OSD, "OSD %s: %d areas", __FUNCTION__, n);

    // clear old OSD, when new areas are set
    if (!IsTrueColor()) {
	cBitmap *bitmap;
	int i;

	for (i = 0; (bitmap = GetBitmap(i)); i++) {
	    bitmap->Clean();
	}
    }
    if (Active()) {
	OsdClose();
	Dirty = 1;
    }
    return cOsd::SetAreas(areas, n);
}

/**
**	Actually commits all data to the OSD hardware.
*/
void cSoftOsd::Flush(void)
{
    cPixmapMemory *pm;

    Debug2(L_OSD, "OSD %s: level %d active %d", __FUNCTION__, OsdLevel,
	Active());

    if (!Active()) {			// this osd is not active
	return;
    }

    if (!IsTrueColor()) {
	cBitmap *bitmap;
	int i;

#ifdef OSD_DEBUG
	static char warned;

	if (!warned) {
	    Debug2(L_OSD, "OSD %s: FIXME: should be truecolor",
		__FUNCTION__);
	    warned = 1;
	}
#endif
	// draw all bitmaps
	for (i = 0; (bitmap = GetBitmap(i)); ++i) {
	    uint8_t *argb;
	    int xs;
	    int ys;
	    int x;
	    int y;
	    int w;
	    int h;
	    int x1;
	    int y1;
	    int x2;
	    int y2;

	    // get dirty bounding box
	    if (Dirty) {		// forced complete update
		x1 = 0;
		y1 = 0;
		x2 = bitmap->Width() - 1;
		y2 = bitmap->Height() - 1;
	    } else if (!bitmap->Dirty(x1, y1, x2, y2)) {
		continue;		// nothing dirty continue
	    }
	    // convert and upload only visible dirty areas
	    xs = bitmap->X0() + Left();
	    ys = bitmap->Y0() + Top();
	    // FIXME: negtative position bitmaps
	    w = x2 - x1 + 1;
	    h = y2 - y1 + 1;
	    // clip to screen
	    if (1) {			// just for the case it makes trouble
		int width;
		int height;
		double video_aspect;

		if (xs < 0) {
		    if (xs + x1 < 0) {
			x1 -= xs + x1;
			w += xs + x1;
			if (w <= 0) {
			    continue;
			}
		    }
		    xs = 0;
		}
		if (ys < 0) {
		    if (ys + y1 < 0) {
			y1 -= ys + y1;
			h += ys + y1;
			if (h <= 0) {
			    continue;
			}
		    }
		    ys = 0;
		}
		::GetScreenSize(&width, &height, &video_aspect);
		if (w > width - xs - x1) {
		    w = width - xs - x1;
		    if (w <= 0) {
			continue;
		    }
		    x2 = x1 + w - 1;
		}
		if (h > height - ys - y1) {
		    h = height - ys - y1;
		    if (h <= 0) {
			continue;
		    }
		    y2 = y1 + h - 1;
		}
	    }
#ifdef DEBUG
	    if (w > bitmap->Width() || h > bitmap->Height()) {
		Fatal(": dirty area too big");
	    }
#endif
	    argb = (uint8_t *) malloc(w * h * sizeof(uint32_t));
	    for (y = y1; y <= y2; ++y) {
		for (x = x1; x <= x2; ++x) {
		    ((uint32_t *) argb)[x - x1 + (y - y1) * w] =
			bitmap->GetColor(x, y);
		}
	    }
	    Debug2(L_OSD, "OSD %s: draw %dx%d%+d%+d bm", __FUNCTION__, w, h,
		xs + x1, ys + y1);
	    OsdDrawARGB(0, 0, w, h, w * sizeof(uint32_t), argb, xs + x1,
		ys + y1);

	    bitmap->Clean();
	    // FIXME: reuse argb
	    free(argb);
	}
	Dirty = 0;
	return;
    }

    LOCK_PIXMAPS;
    while ((pm = (dynamic_cast < cPixmapMemory * >(RenderPixmaps())))) {
	int xp;
	int yp;
	int stride;
	int x;
	int y;
	int w;
	int h;

	x = pm->ViewPort().X();
	y = pm->ViewPort().Y();
	w = pm->ViewPort().Width();
	h = pm->ViewPort().Height();
	stride = w * sizeof(tColor);

	// clip to osd
	xp = 0;
	if (x < 0) {
	    xp = -x;
	    w -= xp;
	    x = 0;
	}

	yp = 0;
	if (y < 0) {
	    yp = -y;
	    h -= yp;
	    y = 0;
	}

	if (w > Width() - x) {
	    w = Width() - x;
	}
	if (h > Height() - y) {
	    h = Height() - y;
	}

	x += Left();
	y += Top();

	// clip to screen
	if (1) {			// just for the case it makes trouble
	    // and it can happen!
	    int width;
	    int height;
	    double video_aspect;

	    if (x < 0) {
		w += x;
		xp += -x;
		x = 0;
	    }
	    if (y < 0) {
		h += y;
		yp += -y;
		y = 0;
	    }

	    ::GetScreenSize(&width, &height, &video_aspect);
	    if (w > width - x) {
		w = width - x;
	    }
	    if (h > height - y) {
		h = height - y;
	    }
	}
	Debug2(L_OSD, "OSD %s: draw %dx%d%+d%+d*%d -> %+d%+d %p",
	    __FUNCTION__, w, h, xp, yp, stride, x, y, pm->Data());
	OsdDrawARGB(xp, yp, w, h, stride, pm->Data(), x, y);

	DestroyPixmap(pm);
    }
    Dirty = 0;
}

//////////////////////////////////////////////////////////////////////////////
//	OSD provider
//////////////////////////////////////////////////////////////////////////////

/**
**	Create a new OSD.
**
**	@param left	x-coordinate of OSD
**	@param top	y-coordinate of OSD
**	@param level	layer level of OSD
*/
cOsd *cSoftOsdProvider::CreateOsd(int left, int top, uint level)
{
#ifdef USE_GLES
    if (DisableOglOsd) {
        Debug2(L_OSD, "OSD %s: %d, %d, %d, OpenGL disabled, using software rendering", __FUNCTION__, left, top, level);
        return Osd = new cSoftOsd(left, top, level);
    }

    if (StartOpenGlThread()) {
        Debug2(L_OSD, "OSD %s: %d, %d, %d, using OpenGL OSD support", __FUNCTION__, left, top, level);
        return Osd = new cOglOsd(left, top, level, oglThread);
    }

    Debug2(L_OSD, "OSD %s: %d, %d, %d, OpenGL failed, using software rendering", __FUNCTION__, left, top, 999);
    DisableOglOsd = 1;
    return Osd = new cSoftOsd(left, top, 999);
#else
    Debug2(L_OSD, "OSD %s: %d, %d, %d", __FUNCTION__, left, top, level);
    return Osd = new cSoftOsd(left, top, level);
#endif
}

/**
**	Check if this OSD provider is able to handle a true color OSD.
**
**	@returns true we are able to handle a true color OSD.
*/
bool cSoftOsdProvider::ProvidesTrueColor(void)
{
    return true;
}

#ifdef USE_GLES
const cImage *cSoftOsdProvider::GetImageData(int ImageHandle) {
    return cOsdProvider::GetImageData(ImageHandle);
}

void cSoftOsdProvider::OsdSizeChanged(void) {
    // cleanup OpenGL context
    if (!DisableOglOsd)
        cSoftOsdProvider::StopOpenGlThread();
    cSoftOsdProvider::UpdateOsdSize();
}

bool cSoftOsdProvider::StartOpenGlThread(void) {
    if (DisableOglOsd) {
        Debug2(L_OPENGL, "OpenGL OSD disabled, OpenGL worker thread NOT started");
        return false;
    }

    if (oglThread.get()) {
        if (oglThread->Active()) {
            return true;
        }
        oglThread.reset();
    }
    cCondWait wait;
    Debug2(L_OPENGL, "Trying to start OpenGL worker thread");
    oglThread.reset(new cOglThread(&wait, ConfigMaxSizeGPUImageCache));
    wait.Wait();

    if (oglThread->Active()) {
        Info("OpenGL worker thread started");
        return true;
    }

    Debug2(L_OPENGL, "OpenGL worker thread NOT started");
    return false;
}

void cSoftOsdProvider::StopOpenGlThread(void) {
    Debug2(L_OPENGL, "stopping OpenGL worker thread");
    if (oglThread) {
        oglThread->Stop();
    }
    oglThread.reset();
    Info("OpenGL worker thread stopped");
}
#endif

/**
**	Create cOsdProvider class.
*/
cSoftOsdProvider::cSoftOsdProvider(void)
:  cOsdProvider()
{
    Debug("%s:", __FUNCTION__);
    Debug2(L_OSD, "OSD %s:", __FUNCTION__);
#ifdef USE_GLES
    if (!DisableOglOsd)
        StopOpenGlThread();
#endif
}

/**
**	Destroy cOsdProvider class.
*/
cSoftOsdProvider::~cSoftOsdProvider()
{
    Debug2(L_OSD, "%s:", __FUNCTION__);
#ifdef USE_GLES
    if (!DisableOglOsd)
        StopOpenGlThread();
#endif
}

//////////////////////////////////////////////////////////////////////////////
//	cMenuSetupPage
//////////////////////////////////////////////////////////////////////////////

/**
**	Create a seperator item.
**
**	@param label	text inside separator
*/
static inline cOsdItem *SeparatorItem(const char *label)
{
    cOsdItem *item;

    item = new cOsdItem(cString::sprintf("* %s: ", label));
    item->SetSelectable(false);

    return item;
}

/**
**	Create a collapsed item.
**
**	@param label	text inside collapsed
**	@param flag	flag handling collapsed or opened
**	@param msg	open message
*/
inline cOsdItem *cMenuSetupSoft::CollapsedItem(const char *label, int &flag,
    const char *msg)
{
    cOsdItem *item;

    item =
	new cMenuEditBoolItem(cString::sprintf("* %s", label), &flag,
	msg ? msg : tr("show"), tr("hide"));

    return item;
}

/**
**	Create setup menu.
*/
void cMenuSetupSoft::Create(void)
{
    int current;

    current = Current();		// get current menu item index
    Clear();				// clear the menu
#ifdef USE_GLES
#ifdef WRITE_PNG
//    pngVariant[0] = tr("none");
//    pngVariant[1] = tr("output fb");
//    pngVariant[2] = tr("render fb");
//    pngVariant[3] = tr("both");
#endif
#endif

    //
    //	general
    //
    Add(CollapsedItem(tr("General"), General));

    if (General) {
	Add(new cMenuEditBoolItem(tr("Make primary device"), &MakePrimary,
		trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Hide main menu entry"),
		&HideMainMenuEntry, trVDR("no"), trVDR("yes")));
	//
	//	osd
	//
#ifdef USE_GLES
	if (!DisableOglOsd) {
		Add(new cMenuEditIntItem(tr("GPU mem used for image caching (MB)"), &MaxSizeGPUImageCache, 0, 4000));
	}
#endif
    }

    //
    //	statistics
    //
    Add(CollapsedItem(tr("Statistics"), Statistics));
    if (Statistics) {
	int duped;
	int dropped;
	int counter;
	GetStats(&duped, &dropped, &counter);
	Add(new cOsdItem(cString::sprintf(tr
		(" Frames duped(%d) dropped(%d) total(%d)"),
		duped, dropped, counter), osUnknown, false));
#ifdef USE_GLES
	Add(new cOsdItem(cString::sprintf(tr
		(" OSD: Using %s rendering"), DisableOglOsd ? "software" : "hardware"), osUnknown, false));
#else
	Add(new cOsdItem(cString::sprintf(tr
		(" OSD: Using software rendering")), osUnknown, false));
#endif

    }

    //
    //	debug
    //
    Add(CollapsedItem(tr("Debug"), DebugMenu));
    if (DebugMenu) {
	Add(new cMenuEditBoolItem(tr("Enable H264 EOS TrickSpeed fix"), &H264EosTrickSpeed, trVDR("no"), trVDR("yes")));
#ifdef USE_GLES
#ifdef WRITE_PNG
	if (!DisableOglOsd) {
		Add(new cMenuEditBoolItem(tr("Write OSD to file"), &WritePngs, trVDR("no"), trVDR("yes")));
//		Add(new cMenuEditStraItem(tr("Write OSD to file"), &WritePngs, 4, pngVariant));
	}
#endif
#endif
    }
    //
    //	audio
    //
    Add(CollapsedItem(tr("Audio"), Audio));

    if (Audio) {
	Add(new cMenuEditIntItem(tr("Audio/Video delay (ms)"), &AudioDelay,
		-1000, 1000));
	Add(new cMenuEditBoolItem(tr("Volume control"), &AudioSoftvol,
		tr("Hardware"), tr("Software")));
	Add(new cMenuEditIntItem(tr("Audio buffer size (ms)"),
		&AudioBufferTime, 0, 1000));
	Add(new cMenuEditBoolItem(tr("Enable normalize volume"),
		&AudioNormalize, trVDR("no"), trVDR("yes")));
	if (AudioNormalize)
		Add(new cMenuEditIntItem(tr("  Max normalize factor (/1000)"),
			&AudioMaxNormalize, 0, 10000));
	Add(new cMenuEditBoolItem(tr("Enable volume compression"),
		&AudioCompression, trVDR("no"), trVDR("yes")));
	if (AudioCompression)
		Add(new cMenuEditIntItem(tr("  Max compression factor (/1000)"),
			&AudioMaxCompression, 0, 10000));
	Add(new cMenuEditIntItem(tr("Reduce stereo volume (/1000)"),
		&AudioStereoDescent, 0, 1000));
	Add(new cMenuEditBoolItem(tr("Enable Stereo downmix"),
		&AudioDownmix, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Pass-through default"),
		&AudioPassthroughDefault, trVDR("off"), trVDR("on")));
	if (AudioPassthroughDefault) {
		Add(new cMenuEditBoolItem(tr("\040\040PCM pass-through"),
			&AudioPassthroughPCM, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040AC-3 pass-through"),
			&AudioPassthroughAC3, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040E-AC-3 pass-through"),
			&AudioPassthroughEAC3, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("Enable automatic AES"), &AudioAutoAES,
			trVDR("no"), trVDR("yes")));
	}
    }
    Add(CollapsedItem(tr("Audio Filter"), AudioFilter));
    if (AudioFilter) {
		Add(new cMenuEditBoolItem(tr(" Enable Audio Equalizer"), &AudioEq,
			trVDR("no"), trVDR("yes")));
		if (AudioEq) {
			Add(new cMenuEditIntItem(tr("  60 Hz band gain"),
				&AudioEqBand[0], -15, 1));
			Add(new cMenuEditIntItem(tr("  72 Hz band gain"),
				&AudioEqBand[1], -15, 1));
			Add(new cMenuEditIntItem(tr("  107 Hz band gain"),
				&AudioEqBand[2], -15, 1));
			Add(new cMenuEditIntItem(tr("  150 Hz band gain"),
				&AudioEqBand[3], -15, 1));
			Add(new cMenuEditIntItem(tr("  220 Hz band gain"),
				&AudioEqBand[4], -15, 1));
			Add(new cMenuEditIntItem(tr("  310 Hz band gain"),
				&AudioEqBand[5], -15, 1));
			Add(new cMenuEditIntItem(tr("  430 Hz band gain"),
				&AudioEqBand[6], -15, 1));
			Add(new cMenuEditIntItem(tr("  620 Hz band gain"),
				&AudioEqBand[7], -15, 1));
			Add(new cMenuEditIntItem(tr("  860 Hz band gain"),
				&AudioEqBand[8], -15, 1));
			Add(new cMenuEditIntItem(tr("  1200 Hz band gain"),
				&AudioEqBand[9], -15, 1));
			Add(new cMenuEditIntItem(tr("  1700 Hz band gain"),
				&AudioEqBand[10], -15, 1));
			Add(new cMenuEditIntItem(tr("  2500 Hz band gain"),
				&AudioEqBand[11], -15, 1));
			Add(new cMenuEditIntItem(tr("  3500 Hz band gain"),
				&AudioEqBand[12], -15, 1));
			Add(new cMenuEditIntItem(tr("  4800 Hz band gain"),
				&AudioEqBand[13], -15, 1));
			Add(new cMenuEditIntItem(tr("  7000 Hz band gain"),
				&AudioEqBand[14], -15, 1));
			Add(new cMenuEditIntItem(tr("  9500 Hz band gain"),
				&AudioEqBand[15], -15, 1));
			Add(new cMenuEditIntItem(tr("  13500 Hz band gain"),
				&AudioEqBand[16], -15, 1));
			Add(new cMenuEditIntItem(tr("  17200 Hz band gain"),
				&AudioEqBand[17], -15, 1));
		}
	}

    SetCurrent(Get(current));		// restore selected menu entry
    Display();				// display build menu
}

/**
**	Process key for setup menu.
*/
eOSState cMenuSetupSoft::ProcessKey(eKeys key)
{
    int old_General = General;
#ifdef USE_GLES
#ifdef WRITE_PNG
    int old_DebugMenu = DebugMenu;
#endif
#endif
    int old_Statistics = Statistics;
    int old_Audio = Audio;
    int old_AudioFilter = AudioFilter;
    int old_AudioEq = AudioEq;
    int old_AudioNormalize = AudioNormalize;
    int old_AudioCompression = AudioCompression;
    int old_AudioPassthroughDefault = AudioPassthroughDefault;
    eOSState state = cMenuSetupPage::ProcessKey(key);

    if (key != kNone) {
		// update menu only, if something on the structure has changed
		// this is needed because VDR menus are evil slow
		if (old_General != General ||
			old_Audio != Audio || old_AudioFilter != AudioFilter ||
			old_AudioEq != AudioEq || old_AudioNormalize != AudioNormalize ||
			old_AudioCompression != AudioCompression ||
#ifdef USE_GLES
#ifdef WRITE_PNG
			old_DebugMenu != DebugMenu ||
#endif
#endif
			old_Statistics != Statistics ||
			old_AudioPassthroughDefault != AudioPassthroughDefault) {
			Create();			// update menu
		}
    }

    return state;
}

/**
**	Constructor setup menu.
**
**	Import global config variables into setup.
*/
cMenuSetupSoft::cMenuSetupSoft(void)
{
    //
    //	general
    //
    General = 0;
    MakePrimary = ConfigMakePrimary;
#ifdef USE_GLES
#ifdef WRITE_PNG
    DebugMenu = 0;
    WritePngs = ConfigWritePngs;
#endif
#endif
    H264EosTrickSpeed = ConfigH264EosTrickSpeed;
    Statistics = 0;
    HideMainMenuEntry = ConfigHideMainMenuEntry;
    //
    //	audio
    //
    Audio = 0;
    AudioDelay = ConfigVideoAudioDelay;
    AudioPassthroughDefault = AudioPassthroughState;
    AudioPassthroughPCM = ConfigAudioPassthrough & CodecPCM;
    AudioPassthroughAC3 = ConfigAudioPassthrough & CodecAC3;
    AudioPassthroughEAC3 = ConfigAudioPassthrough & CodecEAC3;
    AudioDownmix = ConfigAudioDownmix;
    AudioSoftvol = ConfigAudioSoftvol;
    AudioNormalize = ConfigAudioNormalize;
    AudioMaxNormalize = ConfigAudioMaxNormalize;
    AudioCompression = ConfigAudioCompression;
    AudioMaxCompression = ConfigAudioMaxCompression;
    AudioStereoDescent = ConfigAudioStereoDescent;
    AudioBufferTime = ConfigAudioBufferTime;
    AudioAutoAES = ConfigAudioAutoAES;
	//
	// audio filter
	//
    AudioEq = ConfigAudioEq;
    AudioFilter = 0;
    for (int i = 0; i < 18; i++) {
		AudioEqBand[i] = SetupAudioEqBand[i];
	}

#ifdef USE_GLES
    MaxSizeGPUImageCache = ConfigMaxSizeGPUImageCache;
#endif

    Create();
}

/**
**	Store setup.
*/
void cMenuSetupSoft::Store(void)
{
    SetupStore("MakePrimary", ConfigMakePrimary = MakePrimary);
#ifdef USE_GLES
#ifdef WRITE_PNG
    SetupStore("WritePngs", ConfigWritePngs = WritePngs);
#endif
#endif
    SetupStore("H264EosTrickSpeed", ConfigH264EosTrickSpeed = H264EosTrickSpeed);
    SetupStore("HideMainMenuEntry", ConfigHideMainMenuEntry = HideMainMenuEntry);
    SetupStore("AudioDelay", ConfigVideoAudioDelay = AudioDelay);
    VideoSetAudioDelay(ConfigVideoAudioDelay);

    // FIXME: can handle more audio state changes here
    // downmix changed reset audio, to get change direct
    if (ConfigAudioDownmix != AudioDownmix) {
	ResetChannelId();
    }
    ConfigAudioPassthrough = (AudioPassthroughPCM ? CodecPCM : 0)
	| (AudioPassthroughAC3 ? CodecAC3 : 0)
	| (AudioPassthroughEAC3 ? CodecEAC3 : 0);
    AudioPassthroughState = AudioPassthroughDefault;
    if (AudioPassthroughState) {
	SetupStore("AudioPassthrough", ConfigAudioPassthrough);
	CodecSetAudioPassthrough(ConfigAudioPassthrough);
    } else {
	SetupStore("AudioPassthrough", -ConfigAudioPassthrough);
	CodecSetAudioPassthrough(0);
    }
    SetupStore("AudioDownmix", ConfigAudioDownmix = AudioDownmix);
    AudioSetDownmix(ConfigAudioDownmix);
    SetupStore("AudioSoftvol", ConfigAudioSoftvol = AudioSoftvol);
    AudioSetSoftvol(ConfigAudioSoftvol);
    SetupStore("AudioNormalize", ConfigAudioNormalize = AudioNormalize);
    SetupStore("AudioMaxNormalize", ConfigAudioMaxNormalize =
	AudioMaxNormalize);
    AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
    SetupStore("AudioCompression", ConfigAudioCompression = AudioCompression);
    SetupStore("AudioMaxCompression", ConfigAudioMaxCompression =
	AudioMaxCompression);
    AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
    SetupStore("AudioStereoDescent", ConfigAudioStereoDescent =
	AudioStereoDescent);
    AudioSetStereoDescent(ConfigAudioStereoDescent);
    SetupStore("AudioBufferTime", ConfigAudioBufferTime = AudioBufferTime);
    AudioSetBufferTime(ConfigAudioBufferTime);
    SetupStore("AudioAutoAES", ConfigAudioAutoAES = AudioAutoAES);
    AudioSetAutoAES(ConfigAudioAutoAES);
	SetupStore("AudioEq", ConfigAudioEq = AudioEq);
	SetupStore("AudioEqBand01b", SetupAudioEqBand[0] = AudioEqBand[0]);
	SetupStore("AudioEqBand02b", SetupAudioEqBand[1] = AudioEqBand[1]);
	SetupStore("AudioEqBand03b", SetupAudioEqBand[2] = AudioEqBand[2]);
	SetupStore("AudioEqBand04b", SetupAudioEqBand[3] = AudioEqBand[3]);
	SetupStore("AudioEqBand05b", SetupAudioEqBand[4] = AudioEqBand[4]);
	SetupStore("AudioEqBand06b", SetupAudioEqBand[5] = AudioEqBand[5]);
	SetupStore("AudioEqBand07b", SetupAudioEqBand[6] = AudioEqBand[6]);
	SetupStore("AudioEqBand08b", SetupAudioEqBand[7] = AudioEqBand[7]);
	SetupStore("AudioEqBand09b", SetupAudioEqBand[8] = AudioEqBand[8]);
	SetupStore("AudioEqBand10b", SetupAudioEqBand[9] = AudioEqBand[9]);
	SetupStore("AudioEqBand11b", SetupAudioEqBand[10] = AudioEqBand[10]);
	SetupStore("AudioEqBand12b", SetupAudioEqBand[11] = AudioEqBand[11]);
	SetupStore("AudioEqBand13b", SetupAudioEqBand[12] = AudioEqBand[12]);
	SetupStore("AudioEqBand14b", SetupAudioEqBand[13] = AudioEqBand[13]);
	SetupStore("AudioEqBand15b", SetupAudioEqBand[14] = AudioEqBand[14]);
	SetupStore("AudioEqBand16b", SetupAudioEqBand[15] = AudioEqBand[15]);
	SetupStore("AudioEqBand17b", SetupAudioEqBand[16] = AudioEqBand[16]);
	SetupStore("AudioEqBand18b", SetupAudioEqBand[17] = AudioEqBand[17]);
    AudioSetEq(SetupAudioEqBand, ConfigAudioEq);
#ifdef USE_GLES
    SetupStore("MaxSizeGPUImageCache", ConfigMaxSizeGPUImageCache = MaxSizeGPUImageCache);
#endif
}

//////////////////////////////////////////////////////////////////////////////
//	cDevice
//////////////////////////////////////////////////////////////////////////////

/**
**	Constructor device.
*/
cSoftHdDevice::cSoftHdDevice(void)
{
    Debug("%s:", __FUNCTION__);
    spuDecoder = NULL;
}

/**
**	Destructor device.
*/
cSoftHdDevice::~cSoftHdDevice(void)
{
    Debug("%s:", __FUNCTION__);
    delete spuDecoder;
}

/**
**	Informs a device that it will be the primary device.
**
**	@param on	flag if becoming or loosing primary
*/
void cSoftHdDevice::MakePrimaryDevice(bool on)
{
	Debug("%s: %d", __FUNCTION__, on);
	if (!on) {
		::SoftHdDeviceExit();
	} else {
		::Start();
	}

	cDevice::MakePrimaryDevice(on);
	if (on) {
		new cSoftOsdProvider();
	}
}


/**
**	Get the device SPU decoder.
**
**	@returns a pointer to the device's SPU decoder (or NULL, if this
**	device doesn't have an SPU decoder)
*/
cSpuDecoder *cSoftHdDevice::GetSpuDecoder(void)
{
    Debug("%s:", __FUNCTION__);
    if (!spuDecoder && IsPrimaryDevice()) {
	spuDecoder = new cDvbSpuDecoder();
    }
    return spuDecoder;
}


/**
**	Tells whether this device has a MPEG decoder.
*/
bool cSoftHdDevice::HasDecoder(void) const
{
    return true;
}

/**
**	Returns true if this device can currently start a replay session.
*/
bool cSoftHdDevice::CanReplay(void) const
{
    Debug("%s:", __FUNCTION__);
    return true;
}

/**
**	Sets the device into the given play mode.
**
**	@param play_mode	new play mode (Audio/Video/External...)
*/
bool cSoftHdDevice::SetPlayMode(ePlayMode play_mode)
{
	Debug("%s: %d", __FUNCTION__, play_mode);
	return::SetPlayMode(play_mode);
}

/**
**	Gets the current System Time Counter, which can be used to
**	synchronize audio, video and subtitles.
*/
int64_t cSoftHdDevice::GetSTC(void)
{
//    Debug("%s:", __FUNCTION__);
    return::GetSTC();
}

/**
**	Set trick play speed.
**
**	Every single frame shall then be displayed the given number of
**	times.
**
**	@param speed	trick speed
**	@param forward	flag forward direction
*/
void cSoftHdDevice::TrickSpeed(int speed, bool forward)
{
    Debug("%s: %d %s", __FUNCTION__, speed, forward ? "forward" : "backward");
    ::TrickSpeed(speed, forward);
}

/**
**	Clears all video and audio data from the device.
*/
void cSoftHdDevice::Clear(void)
{
    Debug("%s:", __FUNCTION__);
    cDevice::Clear();
    ::Clear();
}

/**
**	Sets the device into play mode (after a previous trick mode)
*/
void cSoftHdDevice::Play(void)
{
    Debug("%s:", __FUNCTION__);
    cDevice::Play();
    ::Play();
}

/**
**	Puts the device into "freeze frame" mode.
*/
void cSoftHdDevice::Freeze(void)
{
    Debug("%s:", __FUNCTION__);
    cDevice::Freeze();
    ::Freeze();
}

/**
**	Turns off audio while replaying.
*/
void cSoftHdDevice::Mute(void)
{
    Debug("%s:", __FUNCTION__);
    cDevice::Mute();
    ::Mute();
}

/**
**	Display the given I-frame as a still picture.
**
**	@param data	pes or ts data of a frame
**	@param length	length of data area
*/
void cSoftHdDevice::StillPicture(const uchar * data, int length)
{
	if (data[0] == 0x47) {		// ts sync
		cDevice::StillPicture(data, length);
		return;
	}

	Debug("%s: %s %p %d", __FUNCTION__,
		data[0] == 0x47 ? "ts" : "pes", data, length);
	::StillPicture(data, length);
}

/**
**	Check if the device is ready for further action.
**
**	@param poller		file handles (unused)
**	@param timeout_ms	timeout in ms to become ready
**
**	@retval true	if ready
**	@retval false	if busy
*/
bool cSoftHdDevice::Poll(
    __attribute__ ((unused)) cPoller & poller, int timeout_ms)
{
//    Debug("%s: timeout %d", __FUNCTION__, timeout_ms);
    return::Poll(timeout_ms);
}

/**
**	Flush the device output buffers.
**
**	@param timeout_ms	timeout in ms to become ready
*/
bool cSoftHdDevice::Flush(int timeout_ms)
{
    Debug("%s: %d ms", __FUNCTION__, timeout_ms);
    return::Flush(timeout_ms);
}

// ----------------------------------------------------------------------------

/**
**	Sets the video display format to the given one (only useful if this
**	device has an MPEG decoder).
*/
void cSoftHdDevice:: SetVideoDisplayFormat(eVideoDisplayFormat
    video_display_format)
{
    Debug("%s: %d", __FUNCTION__, video_display_format);
    cDevice::SetVideoDisplayFormat(video_display_format);
}

/**
**	Sets the output video format to either 16:9 or 4:3 (only useful
**	if this device has an MPEG decoder).
**
**	Should call SetVideoDisplayFormat.
**
**	@param video_format16_9	flag true 16:9.
*/
void cSoftHdDevice::SetVideoFormat(bool video_format16_9)
{
    Debug("%s: %d", __FUNCTION__, video_format16_9);

    // FIXME: 4:3 / 16:9 video format not supported.

    SetVideoDisplayFormat(eVideoDisplayFormat(Setup.VideoDisplayFormat));
}

/**
**	Returns the width, height and video_aspect ratio of the currently
**	displayed video material.
**
**	@note the video_aspect is used to scale the subtitle.
*/
void cSoftHdDevice::GetVideoSize(int &width, int &height, double &video_aspect)
{
    ::GetVideoSize(&width, &height, &video_aspect);
}

/**
**	Returns the width, height and pixel_aspect ratio the OSD.
**
**	FIXME: Called every second, for nothing (no OSD displayed)?
*/
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &pixel_aspect)
{
    ::GetScreenSize(&width, &height, &pixel_aspect);
}

// ----------------------------------------------------------------------------

/**
**	Play a audio packet.
**
**	@param data	exactly one complete PES packet (which is incomplete)
**	@param length	length of PES packet
**	@param id	type of audio data this packet holds
*/
int cSoftHdDevice::PlayAudio(const uchar * data, int length, uchar id)
{
    //Debug("%s: %p %p %d %d", __FUNCTION__, this, data, length, id);

    return::PlayAudio(data, length, id);
}

void cSoftHdDevice::SetAudioTrackDevice(
    __attribute__ ((unused)) eTrackType type)
{
    //Debug("%s:", __FUNCTION__);
}

void cSoftHdDevice::SetDigitalAudioDevice( __attribute__ ((unused)) bool on)
{
    //Debug("%s: %s", __FUNCTION__, on ? "true" : "false");
}

void cSoftHdDevice::SetAudioChannelDevice( __attribute__ ((unused))
    int audio_channel)
{
    //Debug("%s: %d", __FUNCTION__, audio_channel);
}

int cSoftHdDevice::GetAudioChannelDevice(void)
{
    //Debug("%s:", __FUNCTION__);
    return 0;
}

/**
**	Sets the audio volume on this device (Volume = 0...255).
**
**	@param volume	device volume
*/
void cSoftHdDevice::SetVolumeDevice(int volume)
{
    Debug("%s: %d", __FUNCTION__, volume);

    ::SetVolumeDevice(volume);
}

// ----------------------------------------------------------------------------

/**
**	Play a video packet.
**
**	@param data		exactly one complete PES packet (which is incomplete)
**	@param length	length of PES packet
*/
int cSoftHdDevice::PlayVideo(const uchar * data, int length)
{
    //Debug("%s: %p %d", __FUNCTION__, data, length);
    return::PlayVideo(data, length);
}

/**
**	Grabs the currently visible screen image.
**
**	@param size	size of the returned data
**	@param jpeg	flag true, create JPEG data
**	@param quality	JPEG quality
**	@param width	number of horizontal pixels in the frame
**	@param height	number of vertical pixels in the frame
*/
uchar *cSoftHdDevice::GrabImage(int &size, bool jpeg, int quality, int width,
    int height)
{
    Debug("%s: %d, %d, %d, %dx%d", __FUNCTION__, size, jpeg,
	quality, width, height);

    if (quality < 0) {			// caller should care, but fix it
	quality = 95;
    }

    return::GrabImage(&size, jpeg, quality, width, height);
}

/**
**	Ask the output, if it can scale video.
**
**	@param rect	requested video window rectangle
**
**	@returns	the real rectangle or cRect::NULL if invalid
*/
cRect cSoftHdDevice::CanScaleVideo(const cRect & rect, __attribute__ ((unused)) int alignment)
{
    return rect;
}

/**
**	Scale the currently shown video.
**
**	@param rect	video window rectangle
*/
void cSoftHdDevice::ScaleVideo(const cRect & rect)
{
    Debug2(L_OSD, "OSD %s: %dx%d%+d%+d",
        __FUNCTION__, rect.Width(), rect.Height(), rect.X(), rect.Y());
    ::ScaleVideo(rect.X(), rect.Y(), rect.Width(), rect.Height());
}

/**
**	Call rgb to jpeg for C Plugin.
*/
extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality,
    int width, int height)
{
    return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size,
	quality);
}

//////////////////////////////////////////////////////////////////////////////
//	cPlugin
//////////////////////////////////////////////////////////////////////////////

/**
**	Initialize any member variables here.
**
**	@note DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
**	VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
*/
cPluginSoftHdDevice::cPluginSoftHdDevice(void)
{
    //Debug("%s:", __FUNCTION__);
}

/**
**	Clean up after yourself!
*/
cPluginSoftHdDevice::~cPluginSoftHdDevice(void)
{
    //Debug("%s:", __FUNCTION__);

    ::SoftHdDeviceExit();
}

/**
**	Return plugin version number.
**
**	@returns version number as constant string.
*/
const char *cPluginSoftHdDevice::Version(void)
{
    return VERSION;
}

/**
**	Return plugin short description.
**
**	@returns short description as constant string.
*/
const char *cPluginSoftHdDevice::Description(void)
{
    return tr(DESCRIPTION);
}

/**
**	Return a string that describes all known command line options.
**
**	@returns command line help as constant string.
*/
const char *cPluginSoftHdDevice::CommandLineHelp(void)
{
    return::CommandLineHelp();
}

/**
**	Process the command line arguments.
*/
bool cPluginSoftHdDevice::ProcessArgs(int argc, char *argv[])
{
    //Debug("%s:", __FUNCTION__);

    return::ProcessArgs(argc, argv);
}

/**
**	Initializes the DVB devices.
**
**	Must be called before accessing any DVB functions.
**
**	@returns true if any devices are available.
*/
bool cPluginSoftHdDevice::Initialize(void)
{
    //Debug("%s:", __FUNCTION__);

    MyDevice = new cSoftHdDevice();

    return true;
}

/**
**	 Start any background activities the plugin shall perform.
*/
bool cPluginSoftHdDevice::Start(void)
{
	//Debug("%s:", __FUNCTION__);

	if (!MyDevice->IsPrimaryDevice()) {
		Info("softhddevice %d is not the primary device!",
			MyDevice->DeviceNumber());
		if (ConfigMakePrimary) {
			// Must be done in the main thread
			Debug("makeing softhddevice %d the primary device!",
				MyDevice->DeviceNumber());
			DoMakePrimary = MyDevice->DeviceNumber() + 1;
		}
	}
	::Start();

    return true;
}

/**
**	Shutdown plugin.  Stop any background activities the plugin is
**	performing.
*/
void cPluginSoftHdDevice::Stop(void)
{
    //Debug("%s:", __FUNCTION__);

    ::Stop();
}

/**
**	Create main menu entry.
*/
const char *cPluginSoftHdDevice::MainMenuEntry(void)
{
    //Debug("%s:", __FUNCTION__);

    return ConfigHideMainMenuEntry ? NULL : tr(MAINMENUENTRY);
}

/**
**	Perform the action when selected from the main VDR menu.
*/
cOsdObject *cPluginSoftHdDevice::MainMenuAction(void)
{
    //Debug("%s:", __FUNCTION__);

    return new cSoftHdMenu("SoftHdDevice");
}

/**
**	Return our setup menu.
*/
cMenuSetupPage *cPluginSoftHdDevice::SetupMenu(void)
{
    //Debug("%s:", __FUNCTION__);

    return new cMenuSetupSoft;
}

/**
**	Parse setup parameters
**
**	@param name	paramter name (case sensetive)
**	@param value	value as string
**
**	@returns true if the parameter is supported.
*/
bool cPluginSoftHdDevice::SetupParse(const char *name, const char *value)
{
    //Debug("%s: '%s' = '%s'", __FUNCTION__, name, value);

    if (!strcasecmp(name, "MakePrimary")) {
	ConfigMakePrimary = atoi(value);
	return true;
    }
#ifdef USE_GLES
#ifdef WRITE_PNG
    if (!strcasecmp(name, "WritePngs")) {
	ConfigWritePngs = atoi(value);
	return true;
    }
#endif
#endif
    if (!strcasecmp(name, "H264EosTrickSpeed")) {
	ConfigH264EosTrickSpeed = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "HideMainMenuEntry")) {
	ConfigHideMainMenuEntry = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioDelay")) {
	VideoSetAudioDelay(ConfigVideoAudioDelay = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioPassthrough")) {
	int i;

	i = atoi(value);
	AudioPassthroughState = i > 0;
	ConfigAudioPassthrough = abs(i);
	if (AudioPassthroughState) {
	    CodecSetAudioPassthrough(ConfigAudioPassthrough);
	} else {
	    CodecSetAudioPassthrough(0);
	}
	return true;
    }
    if (!strcasecmp(name, "AudioDownmix")) {
	AudioSetDownmix(ConfigAudioDownmix = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioSoftvol")) {
	AudioSetSoftvol(ConfigAudioSoftvol = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioNormalize")) {
	ConfigAudioNormalize = atoi(value);
	AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
	return true;
    }
    if (!strcasecmp(name, "AudioMaxNormalize")) {
	ConfigAudioMaxNormalize = atoi(value);
	AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
	return true;
    }
    if (!strcasecmp(name, "AudioCompression")) {
	ConfigAudioCompression = atoi(value);
	AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
	return true;
    }
    if (!strcasecmp(name, "AudioMaxCompression")) {
	ConfigAudioMaxCompression = atoi(value);
	AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
	return true;
    }
    if (!strcasecmp(name, "AudioStereoDescent")) {
	ConfigAudioStereoDescent = atoi(value);
	AudioSetStereoDescent(ConfigAudioStereoDescent);
	return true;
    }
    if (!strcasecmp(name, "AudioBufferTime")) {
	ConfigAudioBufferTime = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioAutoAES")) {
	ConfigAudioAutoAES = atoi(value);
	AudioSetAutoAES(ConfigAudioAutoAES);
	return true;
    }
    if (!strcasecmp(name, "AudioEq")) {
	ConfigAudioEq = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand01b")) {
	SetupAudioEqBand[0] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand02b")) {
	SetupAudioEqBand[1] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand03b")) {
	SetupAudioEqBand[2] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand04b")) {
	SetupAudioEqBand[3] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand05b")) {
	SetupAudioEqBand[4] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand06b")) {
	SetupAudioEqBand[5] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand07b")) {
	SetupAudioEqBand[6] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand08b")) {
	SetupAudioEqBand[7] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand09b")) {
	SetupAudioEqBand[8] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand10b")) {
	SetupAudioEqBand[9] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand11b")) {
	SetupAudioEqBand[10] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand12b")) {
	SetupAudioEqBand[11] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand13b")) {
	SetupAudioEqBand[12] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand14b")) {
	SetupAudioEqBand[13] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand15b")) {
	SetupAudioEqBand[14] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand16b")) {
	SetupAudioEqBand[15] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand17b")) {
	SetupAudioEqBand[16] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand18b")) {
	SetupAudioEqBand[17] = atoi(value);
	AudioSetEq(SetupAudioEqBand, ConfigAudioEq);
	return true;
    }
#ifdef USE_GLES
    if (!strcasecmp(name, "MaxSizeGPUImageCache")) {
	ConfigMaxSizeGPUImageCache = atoi(value);
	return true;
    }
#endif
    return false;
}

/**
**	Receive requests or messages.
**
**	@param id	unique identification string that identifies the
**			service protocol
**	@param data	custom data structure
*/
bool cPluginSoftHdDevice::Service(const char *id, void *data)
{
    //Debug("%s: id %s", __FUNCTION__, id);

    if (strcmp(id, ATMO_GRAB_SERVICE) == 0) {
	int width;
	int height;

	if (data == NULL) {
	    return true;
	}

	SoftHDDevice_AtmoGrabService_v1_0_t *r =
	    (SoftHDDevice_AtmoGrabService_v1_0_t *) data;
	if (r->structSize != sizeof(SoftHDDevice_AtmoGrabService_v1_0_t)
	    || r->analyseSize < 64 || r->analyseSize > 256
	    || r->clippedOverscan < 0 || r->clippedOverscan > 200) {
	    return false;
	}

	width = r->analyseSize * -1;	// Internal marker for Atmo grab service
	height = r->clippedOverscan;

	r->img = VideoGrabService(&r->imgSize, &width, &height);
	if (r->img == NULL) {
	    return false;
	}
	r->imgType = GRAB_IMG_RGBA_FORMAT_B8G8R8A8;
	r->width = width;
	r->height = height;
	return true;
    }

    if (strcmp(id, ATMO1_GRAB_SERVICE) == 0) {
	SoftHDDevice_AtmoGrabService_v1_1_t *r;

	if (!data) {
	    return true;
	}

	r = (SoftHDDevice_AtmoGrabService_v1_1_t *) data;
	r->img = VideoGrabService(&r->size, &r->width, &r->height);
	if (!r->img) {
	    return false;
	}

	return true;
    }

    return false;
}

//----------------------------------------------------------------------------
//	cPlugin SVDRP
//----------------------------------------------------------------------------

/**
**	SVDRP commands help text.
**	FIXME: translation?
*/
static const char *SVDRPHelpText[] = {
	"PLAY Url\n" "    Play the media from the given url.\n",
	NULL
};

/**
**	Return SVDRP commands help pages.
**
**	return a pointer to a list of help strings for all of the plugin's
**	SVDRP commands.
*/
const char **cPluginSoftHdDevice::SVDRPHelpPages(void)
{
    return SVDRPHelpText;
}

/**
**	Handle SVDRP commands.
**
**	@param command		SVDRP command
**	@param option		all command arguments
**	@param reply_code	reply code
*/
cString cPluginSoftHdDevice::SVDRPCommand(const char *command,
		__attribute__ ((unused)) const char *option,
		__attribute__ ((unused)) int &reply_code)
{
	if (!strcasecmp(command, "PLAY")) {
		Debug2(L_MEDIA, "SVDRPCommand: %s %s", command, option);
		cControl::Launch(new cSoftHdControl(option));
		return "PLAY url";
	}

    return NULL;
}

VDRPLUGINCREATOR(cPluginSoftHdDevice);	// Don't touch this!
