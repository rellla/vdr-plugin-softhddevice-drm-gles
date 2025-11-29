/**
 * @file softhdmenu.cpp
 * Softhddevice menu class
 *
 * @copyright (c) 2020 zille.  All Rights Reserved.
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

#include <cstdlib>
#include <sys/stat.h>

#include <string>
using std::string;
#include <fstream>
using std::ifstream;
#include <sys/stat.h>

#include <vdr/interface.h>
#include <vdr/player.h>
#include <vdr/plugin.h>
#include <vdr/videodir.h>

#include "mediaplayer.h"
#include "softhdmenu.h"
#include "softhddevice.h"
#include "logger.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "misc.h"
}

#include "videostream.h"
#include "audio.h"


/*****************************************************************************
 * cSoftHdMenu
 ****************************************************************************/

/**
 * Softhddevice menu constructor
 */
cSoftHdMenu::cSoftHdMenu(const char *title, cSoftHdDevice *device,
                         int c0, int c1, int c2, int c3, int c4)
:cOsdMenu(title, c0, c1, c2, c3, c4)
{
	pSoftHdMenu = this;
	Playlist.clear();
	m_pDevice = device;

	if (cSoftHdControl::Control() && cSoftHdControl::Control()->Player()->FirstEntry) {
		LOGDEBUG2(L_MEDIA, "mediaplayer: %s: pointer to cSoftHdControl exist.", __FUNCTION__);
		PlayListMenu();		// Test if PL!!!
	} else {
		MainMenu();
	}
}

/**
 * Softhddevice menu destructor
 */
cSoftHdMenu::~cSoftHdMenu()
{
	pSoftHdMenu = NULL;
}

/**
 * Create main menu
 */
void cSoftHdMenu::MainMenu(void)
{
	int current;

	current = Current();               // get current menu item index
	Clear();                           // clear the menu

	Add(new cOsdItem(hk(tr(" play file / make play list")), osUser1));
	Add(new cOsdItem(hk(tr(" select play list")), osUser2));

	SetCurrent(Get(current));          // restore selected menu entry
	Display();
}

/**
 * Create playlist menu
 */
void cSoftHdMenu::PlayListMenu(void)
{
	struct PLEntry *entry = cSoftHdControl::Control()->Player()->FirstEntry;
	Clear();
	while (1) {
		string String = entry->Folder
			+ " - " + entry->SubFolder
			+ " - " + entry->File;
		Add(new cOsdItem(String.c_str()), (entry == cSoftHdControl::Control()->Player()->CurrentEntry));

		if (entry->NextEntry) {
			entry = entry->NextEntry;
		} else {
			break;
		}
	}
	SetHelp(cSoftHdControl::Control()->Player()->Random ? "Random Play" : " No Random Play",
		"Jump -1 min", "Jump +1 min", "End player");
	Display();
}

/**
 * Create select playlist menu
 */
void cSoftHdMenu::SelectPL(void)
{
	struct dirent **DirList;
	int n, i;

	if ((n = scandir(cPlugin::ConfigDirectory("softhddevice-drm-gles"), &DirList, NULL, alphasort)) == -1) {
		LOGERROR("mediaplayer: %s: searching PL in %s failed (%d): %m", __FUNCTION__,
			cPlugin::ConfigDirectory("softhddevice-drm-gles"), errno);
	} else {
		Clear();
		for (i = 0; i < n; i++) {
			if (DirList[i]->d_name[0] != '.' && (strcasestr(DirList[i]->d_name, ".M3U"))) {
				Add(new cOsdItem(DirList[i]->d_name));
			}
		}
		SetHelp("Play PL", NULL, NULL, NULL);
		Display();
	}
}

/**
 * Create sub menu find file or make a play list.
 *
 * @param SearchPath     path to start search mediafile
 * @param playlist       if there is a play list write to play list else make a new menu
 */
void cSoftHdMenu::FindFile(string SearchPath, FILE *playlist)
{
	struct dirent **DirList;
	int n, i;
	const char * sp;

	if (!SearchPath.size())
		sp = "/";
	else sp = SearchPath.c_str();

	if (!playlist) {
		Clear();
		if (SearchPath.size())
			Add(new cOsdItem("[..]"));
	}

	if ((n = scandir(sp, &DirList, NULL, alphasort)) == -1) {
		LOGERROR("mediaplayer: %s: scanning directory %s failed (%d): %m", __FUNCTION__, sp, errno);
	} else {
		struct stat fileAttributs;
		for (i = 0; i < n; i++) {
			string str = SearchPath + "/" + DirList[i]->d_name;
			if (stat(str.c_str(), &fileAttributs) == -1) {
				LOGERROR("mediaplayer: %s: stat on %s failed (%d): %m", __FUNCTION__, str.c_str(), errno);
			} else {
			if (S_ISDIR(fileAttributs.st_mode) && DirList[i]->d_name[0] != '.') {
				if (playlist) {
					FindFile(str.c_str(), playlist);
				} else {
					Add(new cOsdItem(DirList[i]->d_name),
						!LastItem.compare(0, LastItem.length(), DirList[i]->d_name));
				}
			}
			}
		}
		for (i = 0; i < n; i++) {
			string str = SearchPath + "/" + DirList[i]->d_name;
			if (stat(str.c_str(), &fileAttributs) == -1) {
				LOGERROR("mediaplayer: %s: stat on %s failed (%d): %m", __FUNCTION__, str.c_str(), errno);
			} else {
			if (S_ISREG(fileAttributs.st_mode) && DirList[i]->d_name[0] != '.') {
				if (playlist) {
					if (TestMedia(DirList[i]->d_name))
						fprintf(playlist, "%s/%s\n", SearchPath.c_str(),
							DirList[i]->d_name);
				} else {
					Add(new cOsdItem(DirList[i]->d_name));
				}
			}
			}
		}
	}

	if (!playlist) {
		SetHelp( Playlist.empty() ? "Play File" : "Play PL", "New PL", "Add to PL", NULL);
//		SetHelp(Control->Player->Running ? NULL : "Set new PL",
//			Control->Player->Running ? "Play Menu" : "Select PL");
		Display();
	}
}

/**
 * Make a play list
 *
 * @param Target     path to start search mediafiles
 * @param mode       open file mode
*/
void cSoftHdMenu::MakePlayList(const char * Target, const char * mode)
{
	if (Playlist.empty())
		Playlist = "/default.m3u";		// if (!Playlist) ???

	string PlPath = cPlugin::ConfigDirectory("softhddevice-drm-gles");
	PlPath.append(Playlist.c_str());
	FILE *playlist = fopen(PlPath.c_str(), mode);

	if (playlist != NULL) {
		if (TestMedia(Target)) {
			fprintf(playlist, "%s/%s\n", Path.c_str(), Target);
		} else {
			string str = Path + "/" + Target;
			FindFile(str.c_str(), playlist);
		}
	fclose (playlist);
	}
}

/**
 * Handle key event
 *
 * @param key       key event
 */
eOSState cSoftHdMenu::ProcessKey(eKeys key)
{
	eOSState state;
	cOsdItem *item;

	item = (cOsdItem *) Get(Current());
	state = cOsdMenu::ProcessKey(key);

	switch (state) {
		case osUser1:                   // play file / make play list
			Path = cVideoDirectory::Name();
			FindFile(Path, NULL);
			return osContinue;
		case osUser2:                   // select play list
			Path = cPlugin::ConfigDirectory("softhddevice-drm-gles");
			SelectPL();
			return osContinue;
		default:
			break;
	}

	switch (key) {
		case kOk:
			if (strcasestr(item->Text(), "[..]")) {
				string NewPath = Path.substr(0 ,Path.find_last_of("/"));

				if (!LastItem.empty())
					LastItem.clear();
				LastItem = Path.substr(Path.find_last_of("/") + 1);

				Path = NewPath;
				FindFile(Path.c_str(), NULL);
				break;
			}
			if (cSoftHdControl::Control() && cSoftHdControl::Control()->Player()->CurrentEntry) {
				cSoftHdControl::Control()->Player()->SetEntry(Current());
//				PlayListMenu();
				break;
			}
			if (TestMedia(item->Text())) {
				PlayMedia(item->Text());
				return osEnd;
			} else {
				string NewPath = Path + "/" + item->Text();
				struct stat sb;
				if (stat(NewPath.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
					Path = NewPath;
					FindFile(NewPath.c_str(), NULL);
				}
			}
			break;
		case kRed:
			if (cSoftHdControl::Control() && cSoftHdControl::Control()->Player()->CurrentEntry) {
				cSoftHdControl::Control()->Player()->Random ^= 1;
				PlayListMenu();
				break;
			}
			if (Playlist.empty()) {
				if (TestMedia(item->Text())) {
					PlayMedia(item->Text());
					return osEnd;
				}
			} else {
				Path = cPlugin::ConfigDirectory("softhddevice-drm-gles");
				PlayMedia(Playlist.c_str());
				return osEnd;
			}
			break;
		case kGreen:
			if (cSoftHdControl::Control()) {
				cSoftHdControl::Control()->Player()->Jump = -60;
			} else {
				MakePlayList(item->Text(), "w");
				Interface->Confirm(tr("New Playlist"), 1, true);
				if (!LastItem.empty())
					LastItem.clear();
				LastItem = item->Text();
				FindFile(Path.c_str(), NULL);
			}
			break;
		case kYellow:
			if (cSoftHdControl::Control()) {
				cSoftHdControl::Control()->Player()->Jump = 60;
			} else {
				MakePlayList(item->Text(), "a");
				Interface->Confirm(tr("Added to Playlist"), 1, true);
			}
			break;
		case kBlue:
			state = osStopReplay;
			break;
		case kPlay:
			if (TestMedia(item->Text())) {
				PlayMedia(item->Text());
				return osEnd;
			}
			break;
		case kNext:
			if (cSoftHdControl::Control())
				cSoftHdControl::Control()->Player()->StopPlay = 1;
			break;
		default:
			break;
	}

	return state;
}

/**
 * Play media file
 *
 * @param name        file name
 */
void cSoftHdMenu::PlayMedia(const char *name)
{
	string aim = Path + "/" + name;
	if (!cSoftHdControl::Control()) {
		cControl::Launch(new cSoftHdControl(aim.c_str(), m_pDevice));
	} else {
		LOGERROR("mediaplayer: %s: can't start %s", __FUNCTION__, aim.c_str());
	}
}

/**
 * Test if it's a media file.
 *
 * @param name        file name
 * @returns           true if it's a media file
 */
int cSoftHdMenu::TestMedia(const char *name)
{
	if (strcasestr(name, ".MP3"))
		return 1;
	if (strcasestr(name, ".MP4"))
		return 1;
	if (strcasestr(name, ".MKV"))
		return 1;
	if (strcasestr(name, ".MPG"))
		return 1;
	if (strcasestr(name, ".AVI"))
		return 1;
	if (strcasestr(name, ".M2TS"))
		return 1;
	if (strcasestr(name, ".MPEG"))
		return 1;
	if (strcasestr(name, ".M3U"))
		return 1;
	if (strcasestr(name, ".TS"))
		return 1;

	return 0;
}
