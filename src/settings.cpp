/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "settings.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "util.h"

Settings gSettings;
std::function<void()> gOnSettingsChanged;

namespace {

const struct { const char* name; Placement placement; } kPlacements[] = {
	{"top-left", Placement::TopLeft}, {"top-right", Placement::TopRight},
	{"bottom-left", Placement::BottomLeft}, {"bottom-right", Placement::BottomRight},
	{"top", Placement::Top}, {"bottom", Placement::Bottom},
};

std::string ConfigPath()
{
	return std::string(g_get_user_config_dir()) + "/hdeskbar/hdeskbar.conf";
}

}	// namespace

bool ParsePlacement(const std::string& name, Placement* placement)
{
	for (const auto& entry : kPlacements) {
		if (name == entry.name) {
			*placement = entry.placement;
			return true;
		}
	}
	return false;
}

const char* PlacementName(Placement placement)
{
	for (const auto& entry : kPlacements) {
		if (entry.placement == placement)
			return entry.name;
	}
	return "top-right";
}

void LoadSettings()
{
	GKeyFile* file = g_key_file_new();
	if (g_key_file_load_from_file(file, ConfigPath().c_str(), G_KEY_FILE_NONE, nullptr)) {
		auto flag = [&](const char* key, bool* value) {
			GError* error = nullptr;
			const gboolean read = g_key_file_get_boolean(file, "hdeskbar", key, &error);
			if (error == nullptr)
				*value = read;
			g_clear_error(&error);
		};
		flag("show_tabs", &gSettings.showTabs);
		flag("show_tray", &gSettings.showTray);
		flag("show_cpu", &gSettings.showCpu);
		flag("show_volume", &gSettings.showVolume);
		flag("clock_24_hour", &gSettings.clock24);
		flag("clock_seconds", &gSettings.clockSeconds);
		flag("always_on_top", &gSettings.alwaysOnTop);
		flag("auto_raise", &gSettings.autoRaise);
		flag("auto_hide", &gSettings.autoHide);
		flag("expand_tabs", &gSettings.expandTabs);
		flag("check_for_updates", &gSettings.checkForUpdates);
		char* place = g_key_file_get_string(file, "hdeskbar", "placement", nullptr);
		if (place != nullptr) {
			ParsePlacement(place, &gSettings.placement);
			g_free(place);
		}
		auto color = [&](const char* key, Color* out) {
			char* text = g_key_file_get_string(file, "hdeskbar", key, nullptr);
			unsigned value = 0;
			if (text != nullptr && sscanf(text, "#%x", &value) == 1)
				*out = { ((value >> 16) & 0xff) / 255.0, ((value >> 8) & 0xff) / 255.0, (value & 0xff) / 255.0 };
			g_free(text);
		};
		auto number = [&](const char* key, double* out) {
			GError* error = nullptr;
			const double read = g_key_file_get_double(file, "hdeskbar", key, &error);
			if (error == nullptr)
				*out = std::clamp(read, 0.2, 1.0);
			g_clear_error(&error);
		};
		color("accent", &gSettings.accent);
		color("panel", &gSettings.panel);
		flag("own_panel", &gSettings.ownPanel);
		number("accent_opacity", &gSettings.accentOpacity);
		number("panel_opacity", &gSettings.panelOpacity);
		char* icon = g_key_file_get_string(file, "hdeskbar", "header_icon", nullptr);
		if (icon != nullptr) {
			gSettings.headerIcon = icon;
			g_free(icon);
		}
		GError* error = nullptr;
		const int size = g_key_file_get_integer(file, "hdeskbar", "header_size", &error);
		if (error == nullptr && size >= 16 && size <= 64)
			gSettings.headerSize = size;
		g_clear_error(&error);
	}
	g_key_file_free(file);
	ApplyColors();
}

void ApplyColors()
{
	SetColors(gSettings.accent, gSettings.ownPanel, gSettings.panel, gSettings.accentOpacity, gSettings.panelOpacity);
}

void SaveSettings()
{
	GKeyFile* file = g_key_file_new();
	g_key_file_set_boolean(file, "hdeskbar", "show_tabs", gSettings.showTabs);
	g_key_file_set_boolean(file, "hdeskbar", "show_tray", gSettings.showTray);
	g_key_file_set_boolean(file, "hdeskbar", "show_cpu", gSettings.showCpu);
	g_key_file_set_boolean(file, "hdeskbar", "show_volume", gSettings.showVolume);
	g_key_file_set_boolean(file, "hdeskbar", "clock_24_hour", gSettings.clock24);
	g_key_file_set_boolean(file, "hdeskbar", "clock_seconds", gSettings.clockSeconds);
	g_key_file_set_boolean(file, "hdeskbar", "always_on_top", gSettings.alwaysOnTop);
	g_key_file_set_boolean(file, "hdeskbar", "auto_raise", gSettings.autoRaise);
	g_key_file_set_boolean(file, "hdeskbar", "auto_hide", gSettings.autoHide);
	g_key_file_set_boolean(file, "hdeskbar", "expand_tabs", gSettings.expandTabs);
	g_key_file_set_boolean(file, "hdeskbar", "check_for_updates", gSettings.checkForUpdates);
	g_key_file_set_string(file, "hdeskbar", "placement", PlacementName(gSettings.placement));
	auto hex = [](const Color& c) {
		char text[16];
		snprintf(text, sizeof(text), "#%02x%02x%02x", static_cast<int>(c.r * 255 + 0.5), static_cast<int>(c.g * 255 + 0.5),
			static_cast<int>(c.b * 255 + 0.5));
		return std::string(text);
	};
	g_key_file_set_string(file, "hdeskbar", "accent", hex(gSettings.accent).c_str());
	g_key_file_set_string(file, "hdeskbar", "panel", hex(gSettings.panel).c_str());
	g_key_file_set_boolean(file, "hdeskbar", "own_panel", gSettings.ownPanel);
	g_key_file_set_double(file, "hdeskbar", "accent_opacity", gSettings.accentOpacity);
	g_key_file_set_double(file, "hdeskbar", "panel_opacity", gSettings.panelOpacity);
	g_key_file_set_string(file, "hdeskbar", "header_icon", gSettings.headerIcon.c_str());
	g_key_file_set_integer(file, "hdeskbar", "header_size", gSettings.headerSize);
	const std::string path = ConfigPath();
	g_mkdir_with_parents((std::string(g_get_user_config_dir()) + "/hdeskbar").c_str(), 0755);
	g_key_file_save_to_file(file, path.c_str(), nullptr);
	g_key_file_free(file);
}
