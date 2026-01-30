/**
 * @file mediaplayer.h
 * Mediaplayer class header file
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

#ifndef __MEDIAPLAYER_H
#define __MEDIAPLAYER_H

#include <atomic>
#include <string>

#include <vdr/player.h>

class cSoftHdAudio;
class cSoftHdDevice;

/*****************************************************************************
 * cPlaylistEntry
 *
 * class for a playlist entry
 ****************************************************************************/
class cPlaylistEntry
{
public:
	cPlaylistEntry(std::string);

	std::string OsdItemString(void);
	cPlaylistEntry* NextEntry(void) { return m_pNextEntry; };
	void SetNextEntry(cPlaylistEntry *entry) { m_pNextEntry = entry; };
	std::string Path(void) { return m_path; };
private:
	std::string m_path;
	std::string m_file;
	std::string m_subFolder;
	std::string m_folder;
	cPlaylistEntry *m_pNextEntry = nullptr;
};

/*****************************************************************************
 * cSoftHdPlayer (cPlayer mediaplayer)
 *
 * player for mediaplayer mode
 ****************************************************************************/
class cSoftHdPlayer : public cPlayer, cThread
{
public:
	cSoftHdPlayer(const char *, cSoftHdDevice *);
	virtual ~cSoftHdPlayer();

	void SetEntry(int);
	const char *Source(void) { return m_pSource; };

	void JumpSec(int seconds) { m_jumpSec = seconds; };
	void Pause(bool pause) { m_paused = pause; };
	bool IsPaused(void) { return m_paused; };
	void Stop(void) { m_stopped = true; };
	void ToggleRandomPlay(void) { m_random = !m_random; }
	bool IsRandomPlayActive(void) { return m_random; }
	int CurrentTime(void) { return m_currentTime; }
	int Duration(void) { return m_duration; };
	cPlaylistEntry *FirstPlaylistEntry(void) { return m_pFirstEntry; };
	cPlaylistEntry *CurrentPlaylistEntry(void) { return m_pCurrentEntry; };

protected:
	virtual void Activate(bool On);
	virtual void Action(void);

private:
	void Play(const char *);
	void ReadPlaylist(const char *);

	cPlaylistEntry *m_pFirstEntry = nullptr;
	cPlaylistEntry *m_pCurrentEntry = nullptr;

	char *m_pSource;
	int m_entries;
	cSoftHdDevice *m_pDevice;
	cSoftHdAudio *m_pAudio;
	std::atomic<int> m_jumpSec = 0;
	std::atomic<bool> m_paused = false;
	std::atomic<bool> m_stopped = false;
	std::atomic<bool> m_random = false;
	bool m_noModify = false;
	int m_currentTime = 0;
	int m_duration = 0;
};

/*****************************************************************************
 * cSoftHdControl (cControl mediaplayer)
 *
 * control class for mediaplayer mode
 ****************************************************************************/
class cSoftHdControl : public cControl
{
public:
	cSoftHdControl(const char *, cSoftHdDevice *);
	virtual ~cSoftHdControl();

	virtual void Hide(void);
	virtual eOSState ProcessKey(eKeys);
	static cSoftHdControl *Control() { return m_pControl; }
	static cSoftHdPlayer *Player() { return m_pPlayer; }
	void Close(void) { m_closing = true; };

private:
	void ShowProgress();

	static cSoftHdControl *m_pControl;
	static cSoftHdPlayer *m_pPlayer;
	cSkinDisplayReplay *m_pOsd = nullptr;
	cSoftHdDevice *m_pDevice;
	bool m_closing = false;
};

#endif
