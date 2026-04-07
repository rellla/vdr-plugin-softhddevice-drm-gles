// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file softhdmenu.h
 * Plugin Main Menu Header File
 *
 * @copyright 2020 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __SOFTHDMENU_H
#define __SOFTHDMENU_H

#include <string>

#include <vdr/osdbase.h>

class cSoftHdDevice;

/**
 * Plugin Menus
 * @defgroup menu Menus
 */

/**
 * Hotkey States
 *
 * @ingroup menu
 */
typedef enum {
	Initial,
	Blue,
	Red
} HotkeyState;

/**
 * Plugin Main Menu
 *
 * @ingroup menu
 */
class cSoftHdMenu : public cOsdMenu {
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

	HotkeyState m_hotkeyState = HotkeyState::Initial;
	void HandleHotKey(int);

	// mediaplayer
	void MainMenu(void);
	void SelectPlaylistMenu(void);
	void FindFileMenu(std::string, FILE *);
	void MakePlayList(const char *, const char *);
	bool IsValidMediaFile(const char *);
	void PlayMedia(const char *);
	std::string m_path;
	std::string m_lastItem;
	std::string m_playlist;
};

#endif
