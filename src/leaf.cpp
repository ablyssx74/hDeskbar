/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The menu of the header, laid out like the Haiku Deskbar's leaf menu: the system items, the recent documents, folders and
 * applications, and under them the applications.
 */

#include "leaf.h"

#include "alert.h"
#include "icon.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <cstdlib>

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

struct UnmountedVolume {
	std::string device, label;
};

// the value of KEY="..." in a line of `lsblk -P` (lsblk writes odd characters as \xHH)
static std::string PairValue(const std::string& line, const char* key)
{
	const std::string find = std::string(key) + "=\"";
	size_t at = 0;
	while ((at = line.find(find, at)) != std::string::npos && at != 0 && line[at - 1] != ' ')
		at += find.size();
	if (at == std::string::npos)
		return "";
	at += find.size();
	std::string value;
	for (size_t i = at; i < line.size() && line[i] != '"'; i++) {
		if (line[i] == '\\' && i + 3 < line.size() && line[i + 1] == 'x') {
			value += static_cast<char>(std::strtol(line.substr(i + 2, 2).c_str(), nullptr, 16));
			i += 3;
		} else
			value += line[i];
	}
	return value;
}

static std::vector<UnmountedVolume> UnmountedVolumes()
{
	std::vector<UnmountedVolume> volumes;
	FILE* pipe = popen("lsblk -P -o PATH,LABEL,FSTYPE,MOUNTPOINT,SIZE,TYPE 2>/dev/null", "r");
	if (pipe == nullptr)
		return volumes;
	char buffer[2048];
	while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
		const std::string line = buffer;
		const std::string type = PairValue(line, "TYPE"), fs = PairValue(line, "FSTYPE");
		if (fs.empty() || !PairValue(line, "MOUNTPOINT").empty())
			continue;
		if (type.rfind("loop", 0) == 0 || fs == "swap" || fs == "LVM2_member" || fs == "crypto_LUKS" || fs == "linux_raid_member"
			|| fs == "zfs_member" || fs == "squashfs")
			continue;
		UnmountedVolume volume;
		volume.device = PairValue(line, "PATH");
		std::string name = PairValue(line, "LABEL");
		if (name.empty())
			name = volume.device.substr(volume.device.rfind('/') + 1);
		const std::string size = PairValue(line, "SIZE");
		volume.label = size.empty() ? name : name + " (" + size + ")";
		volumes.push_back(volume);
	}
	pclose(pipe);
	return volumes;
}

static std::string MakeFileUri(const std::string& path)
{
	char* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
	const std::string result = uri != nullptr ? uri : "file://" + path;
	g_free(uri);
	return result;
}

// mounts a volume with udisks and opens it; the mounting waits for the answer in a thread of its own
static void MountAndOpen(const std::string& device)
{
	std::thread([device]() {
		const std::string command = "udisksctl mount -b '" + device + "' 2>&1";
		FILE* pipe = popen(command.c_str(), "r");
		if (pipe == nullptr)
			return;
		std::string output;
		char buffer[512];
		while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
			output += buffer;
		pclose(pipe);
		// "Mounted /dev/sdb1 at /run/media/user/LABEL." (or "... is already mounted at `/path'.")
		size_t at = output.rfind(" at ");
		if (at == std::string::npos)
			return;
		std::string path = output.substr(at + 4);
		while (!path.empty() && (path.back() == '\n' || path.back() == '.' || path.back() == '\'' || path.back() == ' '))
			path.pop_back();
		if (!path.empty() && (path[0] == '`' || path[0] == '\''))
			path.erase(0, 1);
		if (path.empty() || path[0] != '/')
			return;
		g_idle_add([](gpointer data) -> gboolean {
			std::string* uri = static_cast<std::string*>(data);
			OpenUri(*uri);
			delete uri;
			return G_SOURCE_REMOVE;
		}, new std::string(MakeFileUri(path)));
	}).detach();
}

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

	// the volumes that are not mounted: dimmed, and choosing one mounts it (udisks, as the desktop's file manager does) and
	// opens it. They come from lsblk: the volume monitor of GIO only knows them when gvfs' udisks monitor runs.
	for (const UnmountedVolume& volume : UnmountedVolumes()) {
		MenuItem item;
		item.label = volume.label;
		item.iconName = "drive-harddisk";
		item.dim = true;
		const std::string device = volume.device;
		item.action = [device]() { MountAndOpen(device); };
		items.push_back(item);
	}
	g_object_unref(monitor);

	if (items.empty()) {
		MenuItem none;
		none.label = "No volumes";
		none.enabled = false;
		items.push_back(none);
	}
	return items;
}

// ---- browsing the files ----

// the icon names of a folder: its own for the home and the folders the desktop knows, else the plain one
static std::string FolderIcons(const std::string& path)
{
	if (path == "/")
		return "drive-harddisk";
	if (path == g_get_home_dir())
		return "user-home,folder-home,folder";
	static const std::vector<std::pair<GUserDirectory, const char*> > special = {
		{ G_USER_DIRECTORY_DESKTOP, "user-desktop" }, { G_USER_DIRECTORY_DOCUMENTS, "folder-documents" },
		{ G_USER_DIRECTORY_DOWNLOAD, "folder-download" }, { G_USER_DIRECTORY_MUSIC, "folder-music" },
		{ G_USER_DIRECTORY_PICTURES, "folder-pictures" }, { G_USER_DIRECTORY_PUBLIC_SHARE, "folder-publicshare" },
		{ G_USER_DIRECTORY_TEMPLATES, "folder-templates" }, { G_USER_DIRECTORY_VIDEOS, "folder-videos" },
	};
	for (const auto& place : special) {
		const char* directory = g_get_user_special_dir(place.first);
		if (directory != nullptr && path == directory)
			return std::string(place.second) + ",folder";
	}
	return "folder,inode-directory";
}

static std::string FileIcons(const std::string& name)
{
	std::string icons;
	char* type = g_content_type_guess(name.c_str(), nullptr, 0, nullptr);
	GIcon* icon = type != nullptr ? g_content_type_get_icon(type) : nullptr;
	if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
		const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
		for (int i = 0; names != nullptr && names[i] != nullptr; i++)
			icons += std::string(i ? "," : "") + names[i];
	}
	if (icon != nullptr)
		g_object_unref(icon);
	g_free(type);
	return icons.empty() ? "text-x-generic" : icons;
}

// The contents of a folder as a menu, like Tracker's folder menus: the folders first, then the files, hidden ones left out;
// hovering a folder opens it as a submenu, choosing a folder or a file opens it (a file in its program).
static std::vector<MenuItem> FolderItems(const std::string& directory)
{
	std::vector<MenuItem> items;
	DIR* dir = opendir(directory.c_str());
	if (dir == nullptr) {
		items.push_back(MenuItem::Header("Can't read this folder"));
		return items;
	}
	struct Entry {
		std::string name, key;
		bool folder;
	};
	std::vector<Entry> entries;
	while (struct dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;
		const std::string full = (directory == "/" ? "" : directory) + "/" + entry->d_name;
		const bool folder = entry->d_type == DT_DIR
			|| ((entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) && g_file_test(full.c_str(), G_FILE_TEST_IS_DIR));
		char* folded = g_utf8_casefold(entry->d_name, -1);
		char* key = g_utf8_collate_key_for_filename(folded, -1);
		entries.push_back({ entry->d_name, key, folder });
		g_free(key);
		g_free(folded);
	}
	closedir(dir);
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
		return a.folder != b.folder ? a.folder : a.key < b.key;
	});
	// a folder like /usr/bin holds thousands: past this many, the last row opens the folder itself
	const size_t most = 1000;
	for (size_t i = 0; i < entries.size() && i < most; i++) {
		const std::string full = (directory == "/" ? "" : directory) + "/" + entries[i].name;
		MenuItem item;
		item.label = entries[i].name;
		item.action = [full]() { OpenUri(MakeFileUri(full)); };
		if (entries[i].folder) {
			item.clickActs = true;
			item.iconName = FolderIcons(full);
			item.submenu = [full]() { return FolderItems(full); };
		} else
			item.iconName = FileIcons(entries[i].name);
		items.push_back(item);
	}
	if (entries.size() > most) {
		items.push_back(MenuItem::Separator());
		MenuItem more;
		more.label = "Open folder to see " + std::to_string(entries.size() - most) + " more...";
		more.iconName = "folder-open";
		more.action = [directory]() { OpenUri(MakeFileUri(directory)); };
		items.push_back(more);
	}
	return items;
}

static std::vector<MenuItem> BrowseItems()
{
	std::vector<MenuItem> items;
	const std::string home = g_get_home_dir();
	for (const std::pair<std::string, std::string>& place : { std::make_pair(std::string("/"), std::string("/")),
			std::make_pair(std::string("Home"), home) }) {
		MenuItem item;
		item.label = place.first;
		item.iconName = FolderIcons(place.second);
		const std::string path = place.second;
		item.action = [path]() { OpenUri(MakeFileUri(path)); };
		item.submenu = [path]() { return FolderItems(path); };
		item.clickActs = true;
		items.push_back(item);
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
	about.icon = AboutMenuIcon(16);
	about.action = []() { ShowAbout(); };
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

	MenuItem browse;
	browse.label = "Browse";
	browse.submenu = BrowseItems;
	items.push_back(browse);
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
