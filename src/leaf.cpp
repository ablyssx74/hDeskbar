/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "leaf.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <algorithm>
#include <map>

#include "apps.h"
#include "util.h"

namespace {

// The categories of the desktop menu specification that an application is filed under, the first of them that it has.
struct Category {
	const char* id;
	const char* label;
	const char* icon;
};

const Category kCategories[] = {
	{"AudioVideo", "Sound & Video", "applications-multimedia"},
	{"Development", "Development", "applications-development"},
	{"Education", "Education", "applications-education"},
	{"Game", "Games", "applications-games"},
	{"Graphics", "Graphics", "applications-graphics"},
	{"Network", "Internet", "applications-internet"},
	{"Office", "Office", "applications-office"},
	{"Science", "Science", "applications-science"},
	{"Settings", "Settings", "preferences-system"},
	{"System", "System", "applications-system"},
	{"Utility", "Accessories", "applications-utilities"},
};

// An action run as a program, away from this one.
void Run(const char* command)
{
	GError* error = nullptr;
	if (!g_spawn_command_line_async(command, &error)) {
		WarnLog("cannot run %s: %s\n", command, error != nullptr ? error->message : "?");
		g_clear_error(&error);
	}
}

void Launch(GDesktopAppInfo* info)
{
	GError* error = nullptr;
	if (!g_app_info_launch(G_APP_INFO(info), nullptr, nullptr, &error)) {
		WarnLog("cannot start %s: %s\n", g_app_info_get_name(G_APP_INFO(info)), error != nullptr ? error->message : "?");
		g_clear_error(&error);
	}
}

std::vector<MenuItem> ApplicationItems()
{
	std::map<std::string, std::vector<AppEntry*> > byCategory;
	if (gApps != nullptr) {
		for (const auto& entry : gApps->Entries()) {
			if (!entry->show || entry->info == nullptr)
				continue;
			const char* categories = g_desktop_app_info_get_categories(entry->info);
			std::string label = "Other";
			if (categories != nullptr) {
				const std::string list = std::string(";") + categories + ";";
				for (const Category& category : kCategories) {
					if (list.find(std::string(";") + category.id + ";") != std::string::npos) {
						label = category.label;
						break;
					}
				}
			}
			byCategory[label].push_back(entry.get());
		}
	}
	std::vector<MenuItem> items;
	auto add = [&](const std::string& label, const std::string& icon) {
		std::vector<AppEntry*> apps = byCategory[label];
		if (apps.empty())
			return;
		std::sort(apps.begin(), apps.end(), [](AppEntry* a, AppEntry* b) { return ToLower(a->name) < ToLower(b->name); });
		MenuItem item;
		item.label = label;
		item.iconName = icon;
		item.submenu = [apps]() {
			std::vector<MenuItem> children;
			for (AppEntry* app : apps) {
				MenuItem child;
				child.label = app->name;
				child.iconName = app->iconName;
				GDesktopAppInfo* info = app->info;
				child.action = [info]() { Launch(info); };
				children.push_back(child);
			}
			return children;
		};
		items.push_back(item);
	};
	for (const Category& category : kCategories)
		add(category.label, category.icon);
	add("Other", "applications-other");
	return items;
}

}	// namespace

std::vector<MenuItem> LeafMenuItems()
{
	std::vector<MenuItem> items;

	MenuItem about;
	about.label = "About hDeskbar";
	about.iconName = "help-about";
	about.action = []() { Run("xdg-open https://github.com/ablyssx74/hDeskbar"); };
	items.push_back(about);
	items.push_back(MenuItem::Separator());

	MenuItem applications;
	applications.label = "Applications";
	applications.iconName = "applications-all";
	applications.submenu = ApplicationItems;
	items.push_back(applications);
	items.push_back(MenuItem::Separator());

	MenuItem power;
	power.label = "Power";
	power.iconName = "system-shutdown";
	power.submenu = []() {
		std::vector<MenuItem> children;
		auto add = [&](const char* label, const char* icon, const char* command) {
			MenuItem item;
			item.label = label;
			item.iconName = icon;
			item.action = [command]() { Run(command); };
			children.push_back(item);
		};
		add("Lock Screen", "system-lock-screen", "loginctl lock-session");
		add("Log Out", "system-log-out", "sh -c 'loginctl terminate-session $XDG_SESSION_ID'");
		children.push_back(MenuItem::Separator());
		add("Restart", "system-reboot", "systemctl reboot");
		add("Shut Down", "system-shutdown", "systemctl poweroff");
		return children;
	};
	items.push_back(power);
	items.push_back(MenuItem::Separator());

	MenuItem quit;
	quit.label = "Quit hDeskbar";
	quit.iconName = "application-exit";
	quit.action = []() { exit(0); };
	items.push_back(quit);
	return items;
}
