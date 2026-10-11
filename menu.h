// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file menu.h
 * Plugin Main Menu Header File
 *
 * @copyright 2020 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __DRMDEVICE_MENU_H
#define __DRMDEVICE_MENU_H

#include <string>

#include <vdr/osdbase.h>

class cOutputDevice;

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
class cDeviceMenu : public cOsdMenu {
public:
	cDeviceMenu(const char *, cOutputDevice *, int = 0, int = 0, int = 0, int = 0, int = 0);
	virtual ~cDeviceMenu();
	static cDeviceMenu *pMenu;
	static cDeviceMenu *Menu() { return pMenu; }

	// mediaplayer
	void PlayListMenu(void);
	virtual eOSState ProcessKey(eKeys);

private:
	cOutputDevice *m_pDevice;

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
