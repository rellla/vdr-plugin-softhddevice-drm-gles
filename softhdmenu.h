/**
 * @file softhdmenu.h
 * Softhddevice menu class header file
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

#ifndef __SOFTHDMENU_H
#define __SOFTHDMENU_H

#include "softhddevice.h"

/*****************************************************************************
 * cSoftHdMenu
 ****************************************************************************/

typedef enum {
	Initial,
	Blue,
	Red
} HotkeyState;

class cSoftHdMenu : public cOsdMenu
{
public:
	cSoftHdMenu(const char *, cSoftHdDevice *, int = 0, int = 0, int = 0, int = 0, int = 0);
	virtual ~cSoftHdMenu();
	static cSoftHdMenu *pSoftHdMenu;
	static cSoftHdMenu *Menu() { return pSoftHdMenu; }

	// mediaplayer
	void PlayListMenu(void);
	virtual eOSState ProcessKey(eKeys);

private:
	cSoftHdDevice *m_pDevice;

	HotkeyState m_hotkeyState;
	void HandleHotKey(int);

	// mediaplayer
	void MainMenu(void);
	void SelectPL(void);
	void FindFile(string, FILE *);
	void MakePlayList(const char *, const char *);
	int TestMedia(const char *);
	void PlayMedia(const char *);
	string m_path;
	string m_lastItem;
	string m_playlist;
};

#endif
