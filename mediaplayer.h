// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file mediaplayer.h
 * Mediaplayer Header File
 *
 * @copyright 2020 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __MEDIAPLAYER_H
#define __MEDIAPLAYER_H

#include <atomic>
#include <string>

#include <vdr/player.h>

class cSoftHdAudio;
class cSoftHdDevice;

/**
 * Mediaplayer Related Stuff
 * @defgroup mediaplayer Mediaplayer
 */

/**
 * Playlist Entry
 *
 * @ingroup mediaplayer
 */
class cPlaylistEntry {
public:
	cPlaylistEntry(std::string);

	std::string OsdItemString(void);
	cPlaylistEntry* GetNextEntry(void) { return m_pNextEntry; };
	void SetNextEntry(cPlaylistEntry *entry) { m_pNextEntry = entry; };
	std::string GetPath(void) { return m_path; };
private:
	std::string m_path;
	std::string m_file;
	std::string m_subFolder;
	std::string m_folder;
	cPlaylistEntry *m_pNextEntry = nullptr;
};

/**
 * Media Player
 *
 * player for mediaplayer mode
 *
 * @ingroup mediaplayer
 */
class cSoftHdPlayer : public cPlayer, cThread {
public:
	cSoftHdPlayer(const char *, cSoftHdDevice *);
	virtual ~cSoftHdPlayer();

	void SetEntry(int);
	const char *GetSource(void) { return m_pSource; };

	void JumpSec(int seconds) { m_jumpSec = seconds; };
	void Pause(bool pause) { m_paused = pause; };
	bool IsPaused(void) { return m_paused; };
	void Stop(void) { m_stopped = true; };
	void ToggleRandomPlay(void) { m_random = !m_random; }
	bool IsRandomPlayActive(void) { return m_random; }
	int GetCurrentTime(void) { return m_currentTime; }
	int GetDuration(void) { return m_duration; };
	cPlaylistEntry *GetFirstPlaylistEntry(void) { return m_pFirstEntry; };
	cPlaylistEntry *GetCurrentPlaylistEntry(void) { return m_pCurrentEntry; };

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

/**
 * Media Player Control
 *
 * @ingroup mediaplayer
 */
class cSoftHdControl : public cControl {
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
