/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * What the user can choose, kept in ~/.config/hdeskbar/hdeskbar.conf, and the window to choose it in.
 */
#ifndef HDESKBAR_SETTINGS_H
#define HDESKBAR_SETTINGS_H

#include <wayland-client.h>

#include <functional>
#include <string>

#include "style.h"

enum class Placement { TopLeft, TopRight, BottomLeft, BottomRight, Top, Bottom };

struct Settings {
	Placement placement = Placement::TopRight;
	bool showTabs = true;
	bool showTray = true;
	bool showCpu = true;
	bool showVolume = true;
	bool clockSeconds = false;	// 1:49:30 PM
	bool alwaysOnTop = true;	// off: the bar is behind the windows
	bool autoRaise = false;		// behind the windows, it comes over them when the pointer is at its edge
	bool autoHide = false;		// it shrinks to a line at the edge of the screen, and comes back when the pointer is there
	bool checkForUpdates = true;	// read the VERSION file of the repository a few seconds after the start
	bool expandTabs = true;		// a vertical bar shows the windows of a program under its tab (off: the tab opens a list)
	bool clock24 = false;		// 13:49 instead of 1:49 PM
	Color accent = { 70 / 255.0, 110 / 255.0, 200 / 255.0 };		// the SnakeTracker color, the highlight
	bool ownPanel = false;					// off: the panel is a light tint of the accent
	Color panel = { 200 / 255.0, 203 / 255.0, 235 / 255.0 };	// the background of the bar and the menus
	double accentOpacity = 1.0;				// 0.2 to 1
	double panelOpacity = 1.0;
	std::string headerIcon;					// a picture's file or an icon's name for the header (empty: the SnakeTracker snake)
	int headerSize = 24;					// the header's height, and its icon's size
};

extern Settings gSettings;

// The names the command line and the file use for the places ("top-left", "top-right", "bottom-left", "bottom-right", "top",
// "bottom").
bool ParsePlacement(const std::string& name, Placement* placement);
const char* PlacementName(Placement placement);

void LoadSettings();
void SaveSettings();

// What a change of the colors does: the colors of the drawing follow the settings.
void ApplyColors();

// Called when the window has changed something (the place, a part shown or left out, the color): the bar follows.
extern std::function<void()> gOnSettingsChanged;

void OpenSettingsWindow();
bool SettingsOwnsSurface(wl_surface* surface);
void SettingsPointerEnter(double x, double y);
void SettingsPointerLeave();
void SettingsPointerMotion(double x, double y);
void SettingsPointerButton(uint32_t button, uint32_t state);

#endif
