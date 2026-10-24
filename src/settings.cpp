/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "settings.h"

#include <glib.h>
#include <glib/gstdio.h>

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
		char* place = g_key_file_get_string(file, "hdeskbar", "placement", nullptr);
		if (place != nullptr) {
			ParsePlacement(place, &gSettings.placement);
			g_free(place);
		}
		char* accent = g_key_file_get_string(file, "hdeskbar", "accent", nullptr);
		if (accent != nullptr) {
			unsigned value = 0;
			if (sscanf(accent, "#%x", &value) == 1) {
				gSettings.accent = { ((value >> 16) & 0xff) / 255.0, ((value >> 8) & 0xff) / 255.0, (value & 0xff) / 255.0 };
			}
			g_free(accent);
		}
	}
	g_key_file_free(file);
	SetAccent(gSettings.accent);
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
	g_key_file_set_string(file, "hdeskbar", "placement", PlacementName(gSettings.placement));
	char accent[16];
	snprintf(accent, sizeof(accent), "#%02x%02x%02x", static_cast<int>(gSettings.accent.r * 255 + 0.5),
		static_cast<int>(gSettings.accent.g * 255 + 0.5), static_cast<int>(gSettings.accent.b * 255 + 0.5));
	g_key_file_set_string(file, "hdeskbar", "accent", accent);
	const std::string path = ConfigPath();
	g_mkdir_with_parents((std::string(g_get_user_config_dir()) + "/hdeskbar").c_str(), 0755);
	g_key_file_save_to_file(file, path.c_str(), nullptr);
	g_key_file_free(file);
}
