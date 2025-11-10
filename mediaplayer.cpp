/**
 * @file mediaplayer.cpp
 * Mediaplayer class
 *
 * This file defines all classes used for the integrated mediaplayer
 *    - cSoftHdPlayer (cPlayer)
 *    - cSoftHdControl (cControl)
 *    - cSoftHdMenu (cOsdMenu)
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
 * cSoftHdPlayer (cPlayer mediaplayer)
 ****************************************************************************/

cSoftHdPlayer::cSoftHdPlayer(const char *url, cSoftHdDevice *device)
:cPlayer (pmAudioVideo)
{
//	m_pPlayer= this;
	m_pSource = (char *) malloc(1 + strlen(url));
	strcpy(m_pSource, url);
	if (strcasestr(m_pSource, ".M3U") && !strcasestr(m_pSource, ".M3U8")) {
		ReadPL(m_pSource);
		CurrentEntry = FirstEntry;
	} else {
		FirstEntry = CurrentEntry = NULL;
	}
	Pause = 0;
	Random = 0;
	m_pDevice = device;
	m_pAudio = m_pDevice->Audio();
//	LOGDEBUG2(L_MEDIA, "mediaplayer: %s: Player gestartet.", __FUNCTION__);
}

cSoftHdPlayer::~cSoftHdPlayer()
{
	StopPlay = 1;
	free(m_pSource);
	if (FirstEntry) {
		while(FirstEntry) {
			PLEntry *entry = FirstEntry;
			FirstEntry = entry->NextEntry;
			delete entry;
			m_Entries--;
		}
	}

//	LOGDEBUG2(L_MEDIA, "mediaplayer: %s: Player beendet.", __FUNCTION__);
}

void cSoftHdPlayer::Activate(bool On)
{
//	LOGDEBUG2(L_MEDIA, "mediaplayer: %s: %s", __FUNCTION__, On ? "On" : "Off");
	if (On)
		Start();
}

void cSoftHdPlayer::Action(void)
{
//	LOGDEBUG2(L_MEDIA, "mediaplayer: %s:", __FUNCTION__);
	NoModify = 0;

	if (strcasestr(m_pSource, ".M3U") && !strcasestr(m_pSource, ".M3U8")) {
		while(CurrentEntry) {
			Jump = 0;
			Player(CurrentEntry->Path.c_str());

			if (!NoModify) {
				CurrentEntry = CurrentEntry->NextEntry;

				if (Random) {
					srand (time (NULL));
					SetEntry(std::rand() % (m_Entries));
				}
			}
			NoModify = 0;

			if (cSoftHdMenu::Menu()) {
				cSoftHdMenu::Menu()->PlayListMenu();
			}
		}
	} else {
		Player(m_pSource);
	}

	while(m_pAudio->GetClock() != AV_NOPTS_VALUE)
		usleep(5000);

	cSoftHdControl::Control()->Close = true;
}

void cSoftHdPlayer::ReadPL(const char *Playlist)
{
	ifstream f;
	PLEntry *last_entry = NULL;
	m_Entries = 0;

	f.open(Playlist);
	if (!f.good()) {
		LOGERROR("mediaplayer: %s: open PL %s failed", __FUNCTION__, Playlist);
		return;
	}

	while (!f.eof()) {
		string s;
		getline(f, s);
		if (s.size() && s.compare(0, 1, "#")) {
			PLEntry *entry = new PLEntry;
			entry->NextEntry = NULL;

			entry->Path = s;
			entry->File = entry->Path.substr(entry->Path.find_last_of("/") +1, string::npos);

			string SubString = entry->Path.substr(0, entry->Path.find_last_of("/"));
			entry->SubFolder = SubString.substr(SubString.find_last_of("/") +1, string::npos);

			string FolderString = entry->Path.substr(0, SubString.find_last_of("/"));
			entry->Folder = FolderString.substr(FolderString.find_last_of("/") +1, string::npos);

			if (!last_entry) {
				FirstEntry = entry;
			} else {
				last_entry->NextEntry = entry;
			}
			last_entry = entry;
			m_Entries++;
		}
	}

	f.close();
}

void cSoftHdPlayer::SetEntry(int index)
{
	PLEntry *entry;
	entry = FirstEntry;

	for(int i = 0; i < index ; i++) {
		entry = entry->NextEntry;
	}
	CurrentEntry = entry;
	NoModify = 1;
	StopPlay = 1;
}

void cSoftHdPlayer::Player(const char *url)
{
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(59,0,100)
	AVCodec *video_codec;
#else
	const AVCodec *video_codec;
#endif
	int err = 0;
	int audio_stream_index = 0;
	int video_stream_index;
	int jump_stream_index = 0;
	int start_time;

	StopPlay = 0;
	Jump = 0;

	AVFormatContext *format = avformat_alloc_context();
	if (avformat_open_input(&format, url, NULL, NULL) != 0) {
		LOGERROR("mediaplayer: %s: Could not open file '%s'", __FUNCTION__, url);
		return;
	}
#ifdef MEDIA_DEBUG
	av_dump_format(format, -1, url, 0);
#endif
	if (avformat_find_stream_info(format, NULL) < 0) {
		LOGERROR("mediaplayer: %s: Could not retrieve stream info from file '%s'", __FUNCTION__, url);
		return;
	}

	for (unsigned int i = 0; i < format->nb_streams; i++) {
		if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			m_pDevice->SetAudioCodec(format->streams[i]->codecpar->codec_id,
				format->streams[i]->codecpar, format->streams[i]->time_base);
			audio_stream_index = jump_stream_index = i;
			break;
		}
	}

	video_stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO,
		-1, -1, &video_codec, 0);

	if (video_stream_index < 0) {
		LOGDEBUG2(L_MEDIA, "mediaplayer: %s: stream does not seem to contain video", __FUNCTION__);
	} else {
		m_pDevice->SetVideoCodec(video_codec->id,
			format->streams[video_stream_index]->codecpar,
			format->streams[video_stream_index]->time_base);
		jump_stream_index = video_stream_index;
	}

	Duration = format->duration / AV_TIME_BASE;
	start_time = format->start_time / AV_TIME_BASE;

	AVPacket *packet = nullptr;
	while (!StopPlay) {
		if (!packet) {
			packet = av_packet_alloc();
			if (!packet) {
				LOGFATAL("mediaplayer: %s: out of memory", __FUNCTION__);
				return;
			}

			err = av_read_frame(format, packet);
			if (err < 0) {
				LOGDEBUG2(L_MEDIA, "mediaplayer: %s: av_read_frame error: %s", __FUNCTION__,
					av_err2str(err));
				StopPlay = 1;
				av_packet_free(&packet);
				break;
			}
		}

		if (audio_stream_index == packet->stream_index) {
			if (!m_pDevice->PlayAudioPkts(packet)) {
				usleep(packet->duration * AV_TIME_BASE *
					av_q2d(format->streams[audio_stream_index]->time_base));
			} else {
				CurrentTime = m_pAudio->GetClock() / 1000 - start_time;
				av_packet_free(&packet);
				packet = nullptr;
			}
		} else if (video_stream_index == packet->stream_index) {
			if (!m_pDevice->PlayVideoPkts(packet)) {
				usleep(packet->duration * AV_TIME_BASE *
					av_q2d(format->streams[video_stream_index]->time_base));
			} else {
				packet = nullptr;
			}
		} else {
			av_packet_free(&packet);
			packet = nullptr;
		}

		while (Pause && !StopPlay)
			sleep(1);

		if (Jump && format->pb->seekable) {
			av_seek_frame(format, format->streams[jump_stream_index]->index,
				packet->pts + (int64_t)(Jump /		// - BufferOffset
				av_q2d(format->streams[jump_stream_index]->time_base)), 0);
			m_pDevice->Clear();
			Jump = 0;

			if (packet) {
				av_packet_free(&packet);
				packet = nullptr;
			}
		}

		if (StopPlay) {
			m_pDevice->Clear();
			break;
		}
	}

	if (packet)
		av_packet_free(&packet);

	Duration = 0;
	CurrentTime = 0;

	avformat_close_input(&format);
	avformat_free_context(format);
}

const char * cSoftHdPlayer::GetTitle(void)
{
	if (CurrentEntry)
		return CurrentEntry->Path.c_str();

	return m_pSource;
}

/*****************************************************************************
 * cSoftHdControl (cControl mediaplayer)
 ****************************************************************************/

cSoftHdControl *cSoftHdControl::m_pControl = NULL;
cSoftHdPlayer *cSoftHdControl::m_pPlayer = NULL;

/**
**	Player control constructor.
*/
cSoftHdControl::cSoftHdControl(const char *url, cSoftHdDevice *device)
:cControl(m_pPlayer = new cSoftHdPlayer(url, device))
{
//	LOGDEBUG2(L_MEDIA, "cSoftHdControl: Player gestartet.");
	m_pControl = this;
	Close = false;
	m_pOsd = NULL;
	m_pDevice = device;
}

/**
**	Player control destructor.
*/
cSoftHdControl::~cSoftHdControl()
{
	delete m_pPlayer;
	m_pPlayer = NULL;
	m_pControl = NULL;
	LOGDEBUG2(L_MEDIA, "mediaplayer: %s: Player beendet.", __FUNCTION__);
}

void cSoftHdControl::Hide(void)
{
	LOGDEBUG2(L_MEDIA, "mediaplayer: %s:", __FUNCTION__);
	if (m_pOsd) {
		delete m_pOsd;
		m_pOsd = NULL;
	}
}

void cSoftHdControl::ShowProgress(void)
{
	if (!m_pOsd) {
		LOGDEBUG2(L_MEDIA, "mediaplayer: %s: get OSD", __FUNCTION__);
		m_pOsd = Skins.Current()->DisplayReplay(false);
	}

	m_pOsd->SetTitle(m_pPlayer->GetTitle());
	m_pOsd->SetProgress(m_pPlayer->CurrentTime, m_pPlayer->Duration);
	m_pOsd->SetCurrent(IndexToHMSF(m_pPlayer->CurrentTime, false, 1));
	m_pOsd->SetTotal(IndexToHMSF(m_pPlayer->Duration, false, 1));

	Skins.Flush();
}

/**
 * Handle a key event
 *
 * @param key     key pressed
 */
eOSState cSoftHdControl::ProcessKey(eKeys key)
{
	switch (key) {
	case kNone:
		if (m_pOsd)
			ShowProgress();
		if (Close) {
			Hide();
			return osStopReplay;
		}
		break;

	case kOk:
		if (m_pOsd) {
			Hide();
		} else {
			ShowProgress();
		}
		break;

	case kPlay:
		if (m_pPlayer->Pause) {
			m_pPlayer->Pause = 0;
			m_pDevice->Play();
		}
		break;

	case kGreen:
		m_pPlayer->Jump = -60;
	break;

	case kYellow:
		m_pPlayer->Jump = 60;
	break;

	case kBlue:
		Hide();
		m_pPlayer->StopPlay = 1;
		return osStopReplay;

	case kPause:
		if (m_pPlayer->Pause) {
			m_pPlayer->Pause = 0;
			m_pDevice->Play();
		} else {
			m_pPlayer->Pause = 1;
			m_pDevice->Freeze();
		}
		break;

	case kNext:
		m_pPlayer->StopPlay = 1;
		break;

	default:
		break;
	}

	return osContinue;
}

/*****************************************************************************
 * cSoftHdMenu (cOsdMenu mediaplayer)
 ****************************************************************************/
cSoftHdMenu *cSoftHdMenu::pSoftHdMenu = NULL;

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
