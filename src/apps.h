/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The desktop applications (.desktop files), and the way a window's app_id is matched to one of them: Wayland tells us an app_id
 * for each window, and the ids in the wild are inconsistent ("firefox" and "org.mozilla.firefox", "Code" and "code-oss"...).
 */
#ifndef HDESKBAR_APPS_H
#define HDESKBAR_APPS_H

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "util.h"

struct AppEntry {
    std::string id;          // desktop file id ("org.kde.dolphin.desktop")
    std::string name;
    std::string iconName;    // themed name or absolute path
    std::string path;        // .desktop file path
    std::string executable;
    std::string wmClass;
    bool show = true;        // NoDisplay/Hidden/OnlyShowIn aware
    bool isFileManager = false;
    GDesktopAppInfo* info = nullptr;
};

class AppDatabase {
public:
    AppDatabase() { Reload(); }
    ~AppDatabase() { Clear(); }

    void Reload() {
        Clear();
        GList* all = g_app_info_get_all();
        for (GList* l = all; l != nullptr; l = l->next) {
            GAppInfo* ai = G_APP_INFO(l->data);
            if (!G_IS_DESKTOP_APP_INFO(ai)) continue;
            GDesktopAppInfo* dai = G_DESKTOP_APP_INFO(ai);
            auto entry = std::make_unique<AppEntry>();
            entry->info = G_DESKTOP_APP_INFO(g_object_ref(dai));
            const char* id = g_app_info_get_id(ai);
            entry->id = id ? id : "";
            const char* name = g_app_info_get_name(ai);
            entry->name = name ? name : entry->id;
            const char* fn = g_desktop_app_info_get_filename(dai);
            entry->path = fn ? fn : "";
            const char* exe = g_app_info_get_executable(ai);
            entry->executable = exe ? exe : "";
            const char* wm = g_desktop_app_info_get_startup_wm_class(dai);
            entry->wmClass = wm ? wm : "";
            entry->show = g_app_info_should_show(ai);
            const char* cats = g_desktop_app_info_get_categories(dai);
            entry->isFileManager = cats && strstr(cats, "FileManager") != nullptr;
            entry->iconName = IconNameFor(g_app_info_get_icon(ai));
            fById[ToLower(entry->id)] = entry.get();
            fEntries.push_back(std::move(entry));
        }
        g_list_free_full(all, g_object_unref);
        fResolveCache.clear();
    }

    static std::string IconNameFor(GIcon* icon) {
        if (icon == nullptr) return std::string();
        if (G_IS_THEMED_ICON(icon)) {
            const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
            if (names && names[0]) return names[0];
        } else if (G_IS_FILE_ICON(icon)) {
            GFile* f = g_file_icon_get_file(G_FILE_ICON(icon));
            char* p = g_file_get_path(f);
            std::string r = p ? p : "";
            g_free(p);
            return r;
        }
        return std::string();
    }

    const std::vector<std::unique_ptr<AppEntry>>& Entries() const { return fEntries; }

    AppEntry* ById(const std::string& id) {
        auto it = fById.find(ToLower(id));
        return it == fById.end() ? nullptr : it->second;
    }

    // app_id (Wayland) -> desktop entry, or nullptr.
    AppEntry* Resolve(const std::string& appId) {
        if (appId.empty()) return nullptr;
        auto cached = fResolveCache.find(appId);
        if (cached != fResolveCache.end()) return cached->second;
        AppEntry* found = ResolveUncached(appId);
        fResolveCache[appId] = found;
        return found;
    }

    // A process's executable -> desktop entry (tray/CPU menu icons).
    AppEntry* ResolveExecutable(const std::string& exeBase) {
        std::string lower = ToLower(exeBase);
        for (const auto& e : fEntries) {
            if (!e->executable.empty() && ToLower(Basename(e->executable)) == lower) return e.get();
        }
        return Resolve(exeBase);
    }

    static std::string Basename(const std::string& p) {
        size_t slash = p.rfind('/');
        return slash == std::string::npos ? p : p.substr(slash + 1);
    }

private:
    AppEntry* ResolveUncached(const std::string& appId) {
        std::string lower = ToLower(appId);
        // 1. exact desktop id, as the xdg-shell spec intends
        if (AppEntry* e = ById(lower + ".desktop")) return e;
        // 2. StartupWMClass
        for (const auto& e : fEntries) {
            if (!e->wmClass.empty() && ToLower(e->wmClass) == lower) return e.get();
        }
        // 3. last reverse-DNS component of the desktop id ("org.mozilla.firefox" <-> "firefox")
        std::string lastComponent = lower;
        size_t dot = lastComponent.rfind('.');
        if (dot != std::string::npos) lastComponent = lastComponent.substr(dot + 1);
        for (const auto& e : fEntries) {
            std::string id = ToLower(e->id);
            if (g_str_has_suffix(id.c_str(), ".desktop")) id.resize(id.size() - 8);
            std::string idLast = id;
            size_t d = idLast.rfind('.');
            if (d != std::string::npos) idLast = idLast.substr(d + 1);
            if (id == lower || idLast == lower || idLast == lastComponent) return e.get();
        }
        // 4. executable name
        for (const auto& e : fEntries) {
            if (!e->executable.empty()) {
                std::string exe = ToLower(Basename(e->executable));
                if (exe == lower || exe == lastComponent) return e.get();
            }
        }
        // 5. display name
        for (const auto& e : fEntries) {
            if (ToLower(e->name) == lower) return e.get();
        }
        // 6. GIO's own fuzzy search as a last resort
        gchar*** results = g_desktop_app_info_search(appId.c_str());
        AppEntry* best = nullptr;
        if (results != nullptr) {
            if (results[0] && results[0][0]) best = ById(results[0][0]);
            for (gchar*** r = results; *r; ++r) g_strfreev(*r);
            g_free(results);
        }
        return best;
    }

    void Clear() {
        for (auto& e : fEntries) {
            if (e->info) g_object_unref(e->info);
        }
        fEntries.clear();
        fById.clear();
        fResolveCache.clear();
    }

    std::vector<std::unique_ptr<AppEntry>> fEntries;
    std::unordered_map<std::string, AppEntry*> fById;
    std::unordered_map<std::string, AppEntry*> fResolveCache;
};

extern AppDatabase* gApps;

#endif
