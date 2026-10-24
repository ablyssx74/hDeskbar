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
	bool clock24 = false;		// 13:49 instead of 1:49 PM
	Color accent = { 70 / 255.0, 110 / 255.0, 200 / 255.0 };
};

extern Settings gSettings;

// The names the command line and the file use for the places ("top-left", "top-right", "bottom-left", "bottom-right", "top",
// "bottom").
bool ParsePlacement(const std::string& name, Placement* placement);
const char* PlacementName(Placement placement);

void LoadSettings();
void SaveSettings();

// Called when the window has changed something (the place, a part shown or left out, the color): the bar follows.
extern std::function<void()> gOnSettingsChanged;

void OpenSettingsWindow();
bool SettingsOwnsSurface(wl_surface* surface);
void SettingsPointerEnter(double x, double y);
void SettingsPointerLeave();
void SettingsPointerMotion(double x, double y);
void SettingsPointerButton(uint32_t button, uint32_t state);

#endif
