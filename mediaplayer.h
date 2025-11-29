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

#include "softhddevice.h"

struct PLEntry {
	string Path;
	string File;
	string Folder;
	string SubFolder;
	struct PLEntry *NextEntry;
};

/*****************************************************************************
 * cSoftHdPlayer (cPlayer mediaplayer)
 *
 * player for mediaplayer mode
 ****************************************************************************/
class cSoftHdPlayer : public cPlayer, cThread
{
private:
	void Player(const char *);
	void ReadPL(const char *);

	char *m_pSource;
	int m_Entries;
	cSoftHdDevice *m_pDevice;
	cSoftHdAudio *m_pAudio;
protected:
	virtual void Activate(bool On);
	virtual void Action(void);
public:
	cSoftHdPlayer(const char *, cSoftHdDevice *);
	virtual ~cSoftHdPlayer();
	struct PLEntry *FirstEntry;
	struct PLEntry *CurrentEntry;
	void SetEntry(int);
	const char * GetTitle(void);
	int Jump;
	int Pause;
	int StopPlay;
	int Random;
	int NoModify;
	int CurrentTime;
	int Duration;
};

/*****************************************************************************
 * cSoftHdControl (cControl mediaplayer)
 *
 * control class for mediaplayer mode
 ****************************************************************************/
class cSoftHdControl : public cControl
{
private:
	void ShowProgress();

	static cSoftHdControl *m_pControl;
	static cSoftHdPlayer *m_pPlayer;
	cSkinDisplayReplay *m_pOsd;
	cSoftHdDevice *m_pDevice;
public:
	cSoftHdControl(const char *, cSoftHdDevice *);
	virtual ~cSoftHdControl();
	virtual void Hide(void);
	virtual cOsdObject *GetLOGINFO(void) { return NULL; };
	virtual eOSState ProcessKey(eKeys);
	static cSoftHdControl *Control() { return m_pControl; }
	static cSoftHdPlayer *Player() { return m_pPlayer; }
	bool Close;
};

#endif
