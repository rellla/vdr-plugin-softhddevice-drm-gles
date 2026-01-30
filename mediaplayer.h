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

struct PLEntry {
	std::string Path;
	std::string File;
	std::string Folder;
	std::string SubFolder;
	struct PLEntry *NextEntry;
};

class cSoftHdAudio;
class cSoftHdDevice;

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

	struct PLEntry *FirstEntry;
	struct PLEntry *CurrentEntry;

	void SetEntry(int);
	const char * GetTitle(void);
	void JumpSec(int seconds) { m_jumpSec = seconds; };
	void Pause(bool pause) { m_paused = pause; };
	bool IsPaused(void) { return m_paused; };
	void Stop(void) { m_stopped = true; };
	void ToggleRandomPlay(void) { m_random = !m_random; }
	bool IsRandomPlayActive(void) { return m_random; }
	int CurrentTime(void) { return m_currentTime; }
	int Duration(void) { return m_duration; };

protected:
	virtual void Activate(bool On);
	virtual void Action(void);

private:
	void Player(const char *);
	void ReadPlaylist(const char *);

	char *m_pSource;
	int m_Entries;
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
	virtual cOsdObject *GetLOGINFO(void) { return NULL; };
	virtual eOSState ProcessKey(eKeys);
	static cSoftHdControl *Control() { return m_pControl; }
	static cSoftHdPlayer *Player() { return m_pPlayer; }
	void Close(void) { m_closing = true; };

private:
	void ShowProgress();

	static cSoftHdControl *m_pControl;
	static cSoftHdPlayer *m_pPlayer;
	cSkinDisplayReplay *m_pOsd = NULL;
	cSoftHdDevice *m_pDevice;

	bool m_closing = false;
};

#endif
