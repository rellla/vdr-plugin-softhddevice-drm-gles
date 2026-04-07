// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file mediaplayer.cpp
 * Mediaplayer
 *
 * This file defines all classes used for the integrated mediaplayer
 *    - cSoftHdPlayer (cPlayer)
 *    - cSoftHdControl (cControl)
 *
 * @copyright 2020 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <cstdlib>
#include <fstream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <vdr/player.h>

#include "audio.h"
#include "logger.h"
#include "misc.h"
#include "mediaplayer.h"
#include "softhddevice.h"
#include "softhdmenu.h"

/**
 * @addtogroup mediaplayer
 * @{
 */
cSoftHdControl *cSoftHdControl::m_pControl = NULL; ///< Control instance
cSoftHdPlayer *cSoftHdControl::m_pPlayer = NULL; //< Player instance
/** @} */

/*****************************************************************************
 * cPlaylistEntry
 ****************************************************************************/

/**
 * Builds the playlist entry from a file name
 *
 * @param path    full path name to the file
 */
cPlaylistEntry::cPlaylistEntry(std::string path)
	: m_path(path)
{
	m_file = m_path.substr(m_path.find_last_of("/") +1, std::string::npos);

	std::string subString = m_path.substr(0, m_path.find_last_of("/"));
	m_subFolder = subString.substr(subString.find_last_of("/") +1, std::string::npos);

	std::string folderString = m_path.substr(0, subString.find_last_of("/"));
	m_folder = folderString.substr(folderString.find_last_of("/") +1, std::string::npos);
}

/**
 * Compose a full-path-string for the OSD entry
 */
std::string cPlaylistEntry::OsdItemString(void)
{
	return (m_folder + " - " + m_subFolder + " - " + m_file);
}

/*****************************************************************************
 * cSoftHdPlayer (cPlayer mediaplayer)
 ****************************************************************************/

/**
 * Returns true, if the playlist is a m3u playlist
 *
 * @param source          file or playlist to be played
 *
 * @ingroup mediaplayer
 */
static bool IsM3UPlaylist(char *source)
{
	return (strcasestr(source, ".M3U") && !strcasestr(source, ".M3U8"));
}

/**
 * Create a new player for a file or playlist
 *
 * @param url             file or playlist to be played
 * @param device          pointer to device
 */
cSoftHdPlayer::cSoftHdPlayer(const char *url, cSoftHdDevice *device)
	: cPlayer(pmAudioVideo),
	  m_pDevice(device),
	  m_pAudio(m_pDevice->Audio())
{
	m_pSource = (char *) malloc(1 + strlen(url));
	strcpy(m_pSource, url);

	if (IsM3UPlaylist(m_pSource)) {
		ReadPlaylist(m_pSource);
		m_pCurrentEntry = m_pFirstEntry;
	} else
		m_pFirstEntry = m_pCurrentEntry = nullptr;

	LOGDEBUG2(L_MEDIA, "mediaplayer: %s: player started", __FUNCTION__);
}

cSoftHdPlayer::~cSoftHdPlayer()
{
	m_stopped = true;
	free(m_pSource);

	while (m_pFirstEntry) {
		cPlaylistEntry *entry = m_pFirstEntry;
		m_pFirstEntry = entry->GetNextEntry();
		delete entry;
		m_entries--;
	}

	LOGDEBUG2(L_MEDIA, "mediaplayer: %s: player stopped", __FUNCTION__);
}

/**
 * Start player thread
 *
 * Called right after the player has been attached
 *
 * @param on         true starts the player, false does nothing
 */
void cSoftHdPlayer::Activate(bool on)
{
	LOGDEBUG2(L_MEDIA, "mediaplayer: %s: %s", __FUNCTION__, on ? "On" : "Off");
	if (on)
		Start();
}

/**
 * Main thread action
 * which invokes replay start
 */
void cSoftHdPlayer::Action(void)
{
	LOGDEBUG2(L_MEDIA, "mediaplayer: %s:", __FUNCTION__);
	m_noModify = false;

	if (IsM3UPlaylist(m_pSource)) {
		while(m_pCurrentEntry) {
			m_jumpSec = 0;
			Play(m_pCurrentEntry->GetPath().c_str());

			if (!m_noModify) {
				m_pCurrentEntry = m_pCurrentEntry->GetNextEntry();

				if (m_random) {
					srand (time (NULL));
					SetEntry(std::rand() % (m_entries));
				}
			}
			m_noModify = 0;

			if (cSoftHdMenu::Menu())
				cSoftHdMenu::Menu()->PlayListMenu();
		}
	} else
		Play(m_pSource);

	while(m_pAudio->GetHardwareOutputPtsMs() != AV_NOPTS_VALUE)
		usleep(5000);

	cSoftHdControl::Control()->Close();
}

/**
 * Read the playlist file
 *
 * @param playlist       full path to the playlist
 */
void cSoftHdPlayer::ReadPlaylist(const char *playlist)
{
	std::ifstream inputFile;
	cPlaylistEntry *lastEntry = nullptr;
	m_entries = 0;

	inputFile.open(playlist);
	if (!inputFile.good()) {
		LOGERROR("mediaplayer: %s: open PL %s failed", __FUNCTION__, playlist);
		return;
	}

	while (!inputFile.eof()) {
		std::string s;
		getline(inputFile, s);
		if (s.size() && s.compare(0, 1, "#")) {
			cPlaylistEntry *entry = new cPlaylistEntry(s);

			if (!lastEntry)
				m_pFirstEntry = entry;
			else
				lastEntry->SetNextEntry(entry);

			lastEntry = entry;
			m_entries++;
		}
	}

	inputFile.close();
}

/**
 * Set the current entry to play
 *
 * @param index       list index (in random mode) or menu item index cOsdMenu::Current()
 */
void cSoftHdPlayer::SetEntry(int index)
{
	cPlaylistEntry *entry;
	entry = m_pFirstEntry;

	for (int i = 0; i < index ; i++) {
		entry = entry->GetNextEntry();
	}

	m_pCurrentEntry = entry;
	m_noModify = true;
	m_stopped = true;
}

/**
 * Play a file
 *
 * @param url       file to play
 */
void cSoftHdPlayer::Play(const char *url)
{
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(59,0,100)
	AVCodec *videoCodec;
#else
	const AVCodec *videoCodec;
#endif
	int err = 0;
	int audioStreamIdx = 0;
	int videoStreamIdx;
	int jumpStreamIdx = 0;
	int startTime;

	m_stopped = false;
	m_jumpSec = 0;

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
			audioStreamIdx = jumpStreamIdx = i;
			break;
		}
	}

	videoStreamIdx = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &videoCodec, 0);

	if (videoStreamIdx < 0) {
		LOGDEBUG2(L_MEDIA, "mediaplayer: %s: stream does not seem to contain video", __FUNCTION__);
	} else {
		m_pDevice->SetVideoCodec(videoCodec->id,
			format->streams[videoStreamIdx]->codecpar,
			format->streams[videoStreamIdx]->time_base);
		jumpStreamIdx = videoStreamIdx;
	}

	m_duration = format->duration / AV_TIME_BASE;
	startTime = format->start_time / AV_TIME_BASE;

	AVPacket *packet = nullptr;
	while (!m_stopped) {
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
				m_stopped = true;
				av_packet_free(&packet);
				break;
			}
		}

		if (audioStreamIdx == packet->stream_index) {
			if (!m_pDevice->PlayAudioPkts(packet)) {
				usleep(packet->duration * AV_TIME_BASE *
					av_q2d(format->streams[audioStreamIdx]->time_base));
			} else {
				m_currentTime = m_pAudio->GetHardwareOutputPtsMs() / 1000 - startTime;
				av_packet_free(&packet);
				packet = nullptr;
			}
		} else if (videoStreamIdx == packet->stream_index) {
			if (!m_pDevice->PlayVideoPkts(packet)) {
				usleep(packet->duration * AV_TIME_BASE *
					av_q2d(format->streams[videoStreamIdx]->time_base));
			} else {
				packet = nullptr;
			}
		} else {
			av_packet_free(&packet);
			packet = nullptr;
		}

		while (m_paused && !m_stopped)
			sleep(1);

		if (m_jumpSec && format->pb->seekable) {
			av_seek_frame(format, format->streams[jumpStreamIdx]->index,
				packet->pts + (int64_t)(m_jumpSec /		// - BufferOffset
				av_q2d(format->streams[jumpStreamIdx]->time_base)), 0);
			m_pDevice->Clear();
			m_jumpSec = 0;

			if (packet) {
				av_packet_free(&packet);
				packet = nullptr;
			}
		}

		if (m_stopped) {
			m_pDevice->Clear();
			break;
		}
	}

	if (packet)
		av_packet_free(&packet);

	m_duration = 0;
	m_currentTime = 0;

	avformat_close_input(&format);
	avformat_free_context(format);
}

/*****************************************************************************
 * cSoftHdControl (cControl mediaplayer)
 ****************************************************************************/

/**
 * Create a new control interface and corresponding player
 *
 * @param url             file or playlist to be played
 * @param device          pointer to device
 */
cSoftHdControl::cSoftHdControl(const char *url, cSoftHdDevice *device)
	: cControl(m_pPlayer = new cSoftHdPlayer(url, device)),
	  m_pDevice(device)
{
	m_pControl = this;
}

cSoftHdControl::~cSoftHdControl()
{
	delete m_pPlayer;
	m_pPlayer = NULL;
	m_pControl = NULL;
}

/**
 * Close the replay OSD
 */
void cSoftHdControl::Hide(void)
{
	LOGDEBUG2(L_MEDIA, "mediaplayer: %s:", __FUNCTION__);
	if (m_pOsd)
		delete m_pOsd;

	m_pOsd = NULL;
}

/**
 * Open the replay OSD
 */
void cSoftHdControl::ShowProgress(void)
{
	if (!m_pOsd) {
		LOGDEBUG2(L_MEDIA, "mediaplayer: %s: get OSD", __FUNCTION__);
		m_pOsd = Skins.Current()->DisplayReplay(false);
	}

	m_pOsd->SetTitle(m_pPlayer->GetCurrentPlaylistEntry() ? m_pPlayer->GetCurrentPlaylistEntry()->GetPath().c_str() : m_pPlayer->GetSource());
	m_pOsd->SetProgress(m_pPlayer->GetCurrentTime(), m_pPlayer->GetDuration());
	m_pOsd->SetCurrent(IndexToHMSF(m_pPlayer->GetCurrentTime(), false, 1));
	m_pOsd->SetTotal(IndexToHMSF(m_pPlayer->GetDuration(), false, 1));

	Skins.Flush();
}

/**
 * Handle a key event
 *
 * @param key     pressed key
 */
eOSState cSoftHdControl::ProcessKey(eKeys key)
{
	switch (key) {
		case kNone:
			if (m_pOsd)
				ShowProgress();
			if (m_closing) {
				Hide();
				return osStopReplay;
			}
			break;
		case kOk:
			if (m_pOsd)
				Hide();
			else
				ShowProgress();
			break;
		case kPlay:
		case kUp:
			if (m_pPlayer->IsPaused()) {
				m_pPlayer->Pause(false);
				m_pDevice->Play();
			}
			break;
		case kGreen:
			m_pPlayer->JumpSec(-60);
			break;
		case kYellow:
			m_pPlayer->JumpSec(60);
			break;
		case kLeft:
			m_pPlayer->JumpSec(-5);
			break;
		case kRight:
			m_pPlayer->JumpSec(5);
			break;
		case kBlue:
		case kBack:
			Hide();
			m_pPlayer->Stop();
			return osStopReplay;
		case kPause:
		case kDown:
			if (m_pPlayer->IsPaused()) {
				m_pPlayer->Pause(false);
				m_pDevice->Play();
			} else {
				m_pPlayer->Pause(true);
				m_pDevice->Freeze();
			}
		break;
		case kNext:
			m_pPlayer->Stop();
			break;
		default:
			break;
	}

	return osContinue;
}
