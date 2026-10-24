/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * Menus, in the SnakeTracker look: a lavender panel with a rounded outline, the row under the pointer raised in the accent color,
 * submenus beside their parent's row. Each menu is an xdg_popup of its own (the compositor puts it where it fits and takes
 * it away when the pointer is pressed outside).
 */
#ifndef HDESKBAR_MENU_H
#define HDESKBAR_MENU_H

#include <wayland-client.h>

#include "icons.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum MenuCheck { kCheckNone = 0, kCheckOff, kCheckOn, kRadioOff, kRadioOn };

struct MenuItem {
	std::string label;
	std::string iconName;
	IconRef icon;				// a picture of its own, when there is no name for it
	MenuCheck check = kCheckNone;
	bool separator = false;
	bool enabled = true;
	std::function<void()> action;
	std::function<std::vector<MenuItem>()> submenu;		// the items of the submenu, made when it opens

	static MenuItem Separator()
	{
		MenuItem item;
		item.separator = true;
		return item;
	}
};

// Where a menu hangs: a rectangle of the bar (in its pixels), and which corner of it the menu's corner is put on.
struct MenuAnchor {
	int x = 0, y = 0, w = 1, h = 1;
	bool above = false;		// the menu opens upward from the rectangle (a bar at the bottom of the screen)
	int side = 0;			// -1: the menu opens beside the rectangle, to its left; 1: to its right (a vertical bar); 0: below it
};

void OpenMenu(std::vector<MenuItem> items, const MenuAnchor& anchor);
void CloseMenus();
bool MenusOpen();

// The pointer, when it is over one of the menus' surfaces.
bool MenuOwnsSurface(wl_surface* surface);
void MenuPointerEnter(wl_surface* surface, double x, double y);
void MenuPointerLeave();
void MenuPointerMotion(double x, double y);
void MenuPointerButton(uint32_t button, uint32_t state);
void MenuPointerAxis(double value);

// For testing: opens the submenu of a row of the menu at a level.
void MenuDebugOpen(int level, int row);

#endif
