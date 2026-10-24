/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The menu of the header, laid out like the Haiku Deskbar's leaf menu: the system items, the recent documents, folders and
 * applications, and under them the applications.
 */

#include "leaf.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

#include "apps.h"
#include "settings.h"
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
void Run(const std::string& command)
{
	GError* error = nullptr;
	if (!g_spawn_command_line_async(command.c_str(), &error)) {
		WarnLog("cannot run %s: %s\n", command.c_str(), error != nullptr ? error->message : "?");
		g_clear_error(&error);
	}
}

void OpenUri(const std::string& uri)
{
	GError* error = nullptr;
	if (!g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error)) {
		WarnLog("cannot open %s: %s\n", uri.c_str(), error != nullptr ? error->message : "?");
		g_clear_error(&error);
	}
}

// ---- the applications used lately (the ones this menu started) ----

std::string RecentApplicationsPath()
{
	return std::string(g_get_user_data_dir()) + "/hdeskbar/recent-applications";
}

std::vector<std::string> ReadRecentApplications()
{
	std::vector<std::string> ids;
	std::ifstream file(RecentApplicationsPath());
	std::string line;
	while (std::getline(file, line)) {
		if (!line.empty())
			ids.push_back(line);
	}
	return ids;
}

void RememberApplication(const std::string& id)
{
	std::vector<std::string> ids = ReadRecentApplications();
	ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
	ids.insert(ids.begin(), id);
	if (ids.size() > 12)
		ids.resize(12);
	g_mkdir_with_parents((std::string(g_get_user_data_dir()) + "/hdeskbar").c_str(), 0755);
	std::ofstream file(RecentApplicationsPath());
	for (const std::string& line : ids)
		file << line << "\n";
}

void Launch(AppEntry* app)
{
	GError* error = nullptr;
	if (!g_app_info_launch(G_APP_INFO(app->info), nullptr, nullptr, &error)) {
		WarnLog("cannot start %s: %s\n", app->name.c_str(), error != nullptr ? error->message : "?");
		g_clear_error(&error);
		return;
	}
	RememberApplication(app->id);
}

MenuItem ApplicationItem(AppEntry* app)
{
	MenuItem child;
	child.label = app->name;
	child.iconName = app->iconName;
	child.action = [app]() { Launch(app); };
	return child;
}

std::vector<MenuItem> RecentApplicationItems()
{
	std::vector<MenuItem> items;
	if (gApps != nullptr) {
		for (const std::string& id : ReadRecentApplications()) {
			if (AppEntry* app = gApps->ById(id))
				items.push_back(ApplicationItem(app));
		}
	}
	if (items.empty()) {
		MenuItem none;
		none.label = "No recent applications";
		none.enabled = false;
		items.push_back(none);
	}
	return items;
}

// ---- recent documents and folders: the GTK list, and KDE's ----

struct RecentFile {
	std::string uri, name, icon;
	gint64 time = 0;
	bool folder = false;
};

std::vector<RecentFile> ReadRecentFiles()
{
	std::vector<RecentFile> files;
	std::set<std::string> seen;
	auto add = [&](const std::string& uri, const std::string& title, const std::string& mime, gint64 time) {
		if (uri.empty() || !seen.insert(uri).second)
			return;
		RecentFile file;
		file.uri = uri;
		file.time = time;
		char* path = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
		if (path != nullptr) {
			if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
				g_free(path);
				return;
			}
			file.folder = g_file_test(path, G_FILE_TEST_IS_DIR);
			char* base = g_filename_display_basename(path);
			file.name = title.empty() ? base : title;
			g_free(base);
			if (!file.folder) {
				const std::string type = !mime.empty() ? mime : [&]() {
					gboolean uncertain = FALSE;
					char* guessed = g_content_type_guess(path, nullptr, 0, &uncertain);
					std::string result = guessed != nullptr ? guessed : "";
					g_free(guessed);
					return result;
				}();
				// the icon names of the type, one after another: the first the theme has is drawn
				if (!type.empty()) {
					GIcon* icon = g_content_type_get_icon(type.c_str());
					if (G_IS_THEMED_ICON(icon)) {
						const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
						for (int i = 0; names != nullptr && names[i] != nullptr; i++)
							file.icon += std::string(i ? "," : "") + names[i];
					}
					if (icon != nullptr)
						g_object_unref(icon);
				}
			}
			g_free(path);
		} else
			file.name = title.empty() ? uri : title;
		if (file.folder)
			file.icon = "folder";
		else if (file.icon.empty())
			file.icon = "text-x-generic";
		files.push_back(file);
	};

	// the GTK list
	GBookmarkFile* bookmarks = g_bookmark_file_new();
	if (g_bookmark_file_load_from_file(bookmarks, (std::string(g_get_user_data_dir()) + "/recently-used.xbel").c_str(), nullptr)) {
		gsize count = 0;
		gchar** uris = g_bookmark_file_get_uris(bookmarks, &count);
		for (gsize i = 0; i < count; i++) {
			char* title = g_bookmark_file_get_title(bookmarks, uris[i], nullptr);
			char* mime = g_bookmark_file_get_mime_type(bookmarks, uris[i], nullptr);
			GDateTime* modified = g_bookmark_file_get_modified_date_time(bookmarks, uris[i], nullptr);
			add(uris[i], title != nullptr ? title : "", mime != nullptr ? mime : "",
				modified != nullptr ? g_date_time_to_unix(modified) : 0);
			g_free(title);
			g_free(mime);
		}
		g_strfreev(uris);
	}
	g_bookmark_file_free(bookmarks);

	// KDE's: a .desktop file for each, with the address in it
	const std::string directory = std::string(g_get_user_data_dir()) + "/RecentDocuments";
	if (GDir* dir = g_dir_open(directory.c_str(), 0, nullptr)) {
		while (const char* name = g_dir_read_name(dir)) {
			const std::string path = directory + "/" + name;
			GKeyFile* entry = g_key_file_new();
			if (g_key_file_load_from_file(entry, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
				char* url = g_key_file_get_string(entry, "Desktop Entry", "URL", nullptr);
				char* title = g_key_file_get_string(entry, "Desktop Entry", "Name", nullptr);
				GStatBuf fileInfo;
				const gint64 time = g_stat(path.c_str(), &fileInfo) == 0 ? fileInfo.st_mtime : 0;
				add(url != nullptr ? url : "", title != nullptr ? title : "", "", time);
				g_free(url);
				g_free(title);
			}
			g_key_file_free(entry);
		}
		g_dir_close(dir);
	}
	std::sort(files.begin(), files.end(), [](const RecentFile& a, const RecentFile& b) { return a.time > b.time; });
	return files;
}

std::vector<MenuItem> RecentFileItems(bool folders)
{
	std::vector<MenuItem> items;
	for (const RecentFile& file : ReadRecentFiles()) {
		if (file.folder != folders)
			continue;
		MenuItem item;
		item.label = file.name;
		item.iconName = file.icon;
		const std::string uri = file.uri;
		item.action = [uri]() { OpenUri(uri); };
		items.push_back(item);
		if (items.size() >= 15)
			break;
	}
	if (items.empty()) {
		MenuItem none;
		none.label = folders ? "No recent folders" : "No recent documents";
		none.enabled = false;
		items.push_back(none);
	}
	return items;
}

// ---- the volumes ----

std::vector<MenuItem> MountItems()
{
	std::vector<MenuItem> items;
	GVolumeMonitor* monitor = g_volume_monitor_get();
	std::set<std::string> shownMounts;
	GList* mounts = g_volume_monitor_get_mounts(monitor);
	for (GList* l = mounts; l != nullptr; l = l->next) {
		GMount* mount = G_MOUNT(l->data);
		if (g_mount_is_shadowed(mount))
			continue;
		GFile* root = g_mount_get_root(mount);
		char* uri = g_file_get_uri(root);
		char* name = g_mount_get_name(mount);
		MenuItem item;
		item.label = name;
		item.iconName = "drive-harddisk";
		const std::string target = uri;
		item.action = [target]() { OpenUri(target); };
		items.push_back(item);
		g_free(uri);
		g_free(name);
		g_object_unref(root);
	}
	g_list_free_full(mounts, g_object_unref);

	// a volume that is not mounted yet: choosing it mounts it, and opens it
	GList* volumes = g_volume_monitor_get_volumes(monitor);
	for (GList* l = volumes; l != nullptr; l = l->next) {
		GVolume* volume = G_VOLUME(l->data);
		GMount* mount = g_volume_get_mount(volume);
		if (mount != nullptr) {
			g_object_unref(mount);
			continue;
		}
		if (!g_volume_can_mount(volume))
			continue;
		char* name = g_volume_get_name(volume);
		MenuItem item;
		item.label = std::string(name) + " (not mounted)";
		item.iconName = "drive-removable-media";
		g_object_ref(volume);
		item.action = [volume]() {
			g_volume_mount(volume, G_MOUNT_MOUNT_NONE, nullptr, nullptr, [](GObject* source, GAsyncResult* result, gpointer) {
				GVolume* v = G_VOLUME(source);
				if (g_volume_mount_finish(v, result, nullptr)) {
					if (GMount* mounted = g_volume_get_mount(v)) {
						GFile* root = g_mount_get_root(mounted);
						char* uri = g_file_get_uri(root);
						OpenUri(uri);
						g_free(uri);
						g_object_unref(root);
						g_object_unref(mounted);
					}
				}
			}, nullptr);
		};
		items.push_back(item);
		g_free(name);
	}
	g_list_free_full(volumes, g_object_unref);
	g_object_unref(monitor);

	if (items.empty()) {
		MenuItem none;
		none.label = "No volumes";
		none.enabled = false;
		items.push_back(none);
	}
	return items;
}

// ---- the applications ----

std::vector<MenuItem> ApplicationItems(bool preferences)
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
			if ((label == "Settings") == preferences)
				byCategory[label].push_back(entry.get());
		}
	}
	auto sorted = [&](const std::string& label) {
		std::vector<AppEntry*> apps = byCategory[label];
		std::sort(apps.begin(), apps.end(), [](AppEntry* a, AppEntry* b) { return ToLower(a->name) < ToLower(b->name); });
		return apps;
	};
	std::vector<MenuItem> items;
	if (preferences) {
		for (AppEntry* app : sorted("Settings"))
			items.push_back(ApplicationItem(app));
		return items;
	}
	auto add = [&](const std::string& label, const std::string& icon) {
		const std::vector<AppEntry*> apps = sorted(label);
		if (apps.empty())
			return;
		MenuItem item;
		item.label = label;
		item.iconName = icon;
		item.submenu = [apps]() {
			std::vector<MenuItem> children;
			for (AppEntry* app : apps)
				children.push_back(ApplicationItem(app));
			return children;
		};
		items.push_back(item);
	};
	for (const Category& category : kCategories) {
		if (std::string(category.id) != "Settings")
			add(category.label, category.icon);
	}
	add("Other", "applications-other");
	return items;
}

// The program that looks things up, if there is one.
std::string FindCommand()
{
	for (const char* command : {"krunner", "rofi -show drun", "wofi --show drun", "fuzzel", "albert show", "ulauncher-toggle"}) {
		const std::string program = std::string(command).substr(0, std::string(command).find(' '));
		char* found = g_find_program_in_path(program.c_str());
		if (found != nullptr) {
			g_free(found);
			return command;
		}
	}
	return std::string();
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

	const std::string find = FindCommand();
	MenuItem finder;
	finder.label = "Find...";
	finder.iconName = "system-search";
	finder.enabled = !find.empty();
	finder.action = [find]() { Run(find); };
	items.push_back(finder);

	MenuItem tray;
	tray.label = "Show tray icons";
	tray.check = gSettings.showTray ? kCheckOn : kCheckOff;
	tray.action = []() {
		gSettings.showTray = !gSettings.showTray;
		SaveSettings();
		if (gOnSettingsChanged)
			gOnSettingsChanged();
	};
	items.push_back(tray);

	MenuItem mount;
	mount.label = "Mount";
	mount.iconName = "drive-harddisk";
	mount.submenu = MountItems;
	items.push_back(mount);

	MenuItem preferences;
	preferences.label = "Deskbar preferences...";
	preferences.iconName = "preferences-system";
	preferences.action = []() { OpenSettingsWindow(); };
	items.push_back(preferences);

	MenuItem quit;
	quit.label = "Quit hDeskbar";
	quit.iconName = "application-exit";
	quit.action = []() { exit(0); };
	items.push_back(quit);
	items.push_back(MenuItem::Separator());

	MenuItem shutdown;
	shutdown.label = "Shutdown...";
	shutdown.iconName = "system-shutdown";
	shutdown.submenu = []() {
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
	items.push_back(shutdown);
	items.push_back(MenuItem::Separator());

	MenuItem documents;
	documents.label = "Recent documents";
	documents.submenu = []() { return RecentFileItems(false); };
	items.push_back(documents);
	MenuItem folders;
	folders.label = "Recent folders";
	folders.submenu = []() { return RecentFileItems(true); };
	items.push_back(folders);
	MenuItem recentApplications;
	recentApplications.label = "Recent applications";
	recentApplications.submenu = RecentApplicationItems;
	items.push_back(recentApplications);
	items.push_back(MenuItem::Separator());

	MenuItem applications;
	applications.label = "Applications";
	applications.iconName = "applications-all";
	applications.submenu = []() { return ApplicationItems(false); };
	items.push_back(applications);
	MenuItem settings;
	settings.label = "Preferences";
	settings.iconName = "preferences-system";
	settings.submenu = []() { return ApplicationItems(true); };
	items.push_back(settings);
	return items;
}
