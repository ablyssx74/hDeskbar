/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The freedesktop icon theme lookup, as hDesktop does it: the user's theme, its Inherits chain, then hicolor and the pixmaps,
 * the directory whose size is closest to what is asked; SVGs are drawn with librsvg at exactly the size asked for.
 */

#include "icons.h"

#include <cmath>

#include "scale.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <functional>
#include <vector>

#include "util.h"

bool gDebugEnabled = false;

namespace {

static std::string gIconThemeOverride;

// Tiny, forgiving INI reader: KDE's kdeglobals uses syntax GKeyFile rejects.
static std::map<std::string, std::map<std::string, std::string>> ReadIni(const std::string& path) {
    std::map<std::string, std::map<std::string, std::string>> result;
    std::string contents = ReadFile(path);
    std::string group;
    size_t pos = 0;
    while (pos < contents.size()) {
        size_t eol = contents.find('\n', pos);
        if (eol == std::string::npos) eol = contents.size();
        std::string line = Trim(contents.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            group = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        size_t bracket = key.find("[$");
        if (bracket != std::string::npos) key = key.substr(0, bracket);
        result[group][key] = Trim(line.substr(eq + 1));
    }
    return result;
}

static std::vector<std::string> SplitList(const std::string& s, char sep = ',') {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(sep, start);
        if (end == std::string::npos) end = s.size();
        std::string part = Trim(s.substr(start, end - start));
        if (!part.empty()) out.push_back(part);
        start = end + 1;
    }
    return out;
}

class IconTheme {
public:
    IconTheme() {
        std::string dataHome = g_get_user_data_dir();
        fBaseDirs.push_back(dataHome + "/icons");
        fBaseDirs.push_back(HomeDir() + "/.icons");
        for (const char* const* d = g_get_system_data_dirs(); d && *d; ++d) {
            fBaseDirs.push_back(std::string(*d) + "/icons");
        }
        fPixmapDirs.push_back("/usr/share/pixmaps");
        fThemeName = DetectThemeName();
        DebugLog("icon theme: %s\n", fThemeName.c_str());
    }

    const std::string& ThemeName() const { return fThemeName; }

    // Extra directory to search first (StatusNotifierItem's IconThemePath).
    std::string LookupPath(const std::string& name, int size, const std::string& extraDir = std::string()) {
        if (name.empty()) return std::string();
        if (name[0] == '/') return FileExists(name) ? name : std::string();

        std::string key = name + "@" + std::to_string(size) + "@" + extraDir;
        auto cached = fCache.find(key);
        if (cached != fCache.end()) return cached->second;

        std::string found;
        if (!extraDir.empty()) found = FindInFlatDir(extraDir, name, true);
        if (found.empty()) found = FindInThemes(name, size);
        if (found.empty()) {
            for (const auto& dir : fPixmapDirs) {
                found = FindInFlatDir(dir, name, false);
                if (!found.empty()) break;
            }
        }
        fCache[key] = found;
        return found;
    }

    // Loads a themed icon name or an absolute path into a square image of
    // pxSize device pixels. Falls back through the spec's "-" generic names
    // ("user-trash-full" -> "user-trash"). Returns nullptr when nothing matches.
    cairo_surface_t* Load(const std::string& name, int pxSize, const std::string& extraDir = std::string()) {
        std::string candidate = name;
        while (!candidate.empty()) {
            std::string path = LookupPath(candidate, pxSize, extraDir);
            if (!path.empty()) {
                cairo_surface_t* s = LoadFile(path, pxSize);
                if (s) return s;
            }
            size_t dash = candidate.rfind('-');
            if (dash == std::string::npos || candidate[0] == '/') break;
            candidate = candidate.substr(0, dash);
        }
        return nullptr;
    }

    // Rasterizes an image file into a centered, aspect-preserving square.
    static cairo_surface_t* LoadFile(const std::string& path, int pxSize) {
        if (pxSize <= 0) return nullptr;
        std::string lower = ToLower(path);
        cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pxSize, pxSize);
        cairo_t* cr = cairo_create(out);
        bool ok = false;

        if (g_str_has_suffix(lower.c_str(), ".svg") || g_str_has_suffix(lower.c_str(), ".svgz")) {
            GError* err = nullptr;
            RsvgHandle* handle = rsvg_handle_new_from_file(path.c_str(), &err);
            if (handle != nullptr) {
                RsvgRectangle viewport = {0, 0, static_cast<double>(pxSize), static_cast<double>(pxSize)};
                ok = rsvg_handle_render_document(handle, cr, &viewport, nullptr);
                g_object_unref(handle);
            }
            g_clear_error(&err);
        } else if (g_str_has_suffix(lower.c_str(), ".png")) {
            cairo_surface_t* png = cairo_image_surface_create_from_png(path.c_str());
            if (cairo_surface_status(png) == CAIRO_STATUS_SUCCESS) {
                PaintScaled(cr, png, pxSize);
                ok = true;
            }
            cairo_surface_destroy(png);
        } else {
            // Anything else (xpm, jpg, ...) goes through gdk-pixbuf, which
            // librsvg already pulls in.
            ok = LoadWithPixbuf(cr, path, pxSize);
        }

        cairo_destroy(cr);
        if (!ok) {
            cairo_surface_destroy(out);
            return nullptr;
        }
        cairo_surface_flush(out);
        return out;
    }

    static void PaintScaled(cairo_t* cr, cairo_surface_t* src, int pxSize) {
        int w = cairo_image_surface_get_width(src);
        int h = cairo_image_surface_get_height(src);
        if (w <= 0 || h <= 0) return;
        double s = std::min(static_cast<double>(pxSize) / w, static_cast<double>(pxSize) / h);
        cairo_save(cr);
        cairo_translate(cr, (pxSize - w * s) / 2.0, (pxSize - h * s) / 2.0);
        cairo_scale(cr, s, s);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint(cr);
        cairo_restore(cr);
    }

private:
    struct ThemeDir {
        std::string subdir;
        int size = 48, scale = 1, minSize = 48, maxSize = 48, threshold = 2;
        enum { kFixed, kScalable, kThreshold } type = kThreshold;
    };
    struct Theme {
        bool valid = false;
        std::vector<std::string> roots;     // every base dir that has this theme
        std::vector<std::string> inherits;
        std::vector<ThemeDir> dirs;
    };

    static bool LoadWithPixbuf(cairo_t* cr, const std::string& path, int pxSize);

    std::string DetectThemeName() {
        if (const char* env = g_getenv("HDESKTOP_ICON_THEME")) return env;
        if (!gIconThemeOverride.empty()) return gIconThemeOverride;

        std::string configHome = g_get_user_config_dir();
        auto fromKde = [&]() -> std::string {
            auto ini = ReadIni(configHome + "/kdeglobals");
            auto g = ini.find("Icons");
            if (g != ini.end()) {
                auto k = g->second.find("Theme");
                if (k != g->second.end() && !k->second.empty()) return k->second;
            }
            return std::string();
        };
        auto fromGtk = [&]() -> std::string {
            for (const char* file : {"/gtk-4.0/settings.ini", "/gtk-3.0/settings.ini"}) {
                auto ini = ReadIni(configHome + file);
                auto g = ini.find("Settings");
                if (g != ini.end()) {
                    auto k = g->second.find("gtk-icon-theme-name");
                    if (k != g->second.end() && !k->second.empty()) return k->second;
                }
            }
            GSettingsSchemaSource* src = g_settings_schema_source_get_default();
            GSettingsSchema* schema = src ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE) : nullptr;
            if (schema != nullptr) {
                std::string name;
                if (g_settings_schema_has_key(schema, "icon-theme")) {
                    GSettings* gs = g_settings_new("org.gnome.desktop.interface");
                    char* v = g_settings_get_string(gs, "icon-theme");
                    if (v) { name = v; g_free(v); }
                    g_object_unref(gs);
                }
                g_settings_schema_unref(schema);
                return name;
            }
            return std::string();
        };

        std::string name;
        if (DesktopIs("KDE")) name = fromKde();
        if (name.empty()) name = fromGtk();
        if (name.empty()) name = fromKde();
        if (name.empty() || !GetTheme(name).valid) {
            for (const char* fallback : {"breeze", "Papirus", "Adwaita", "hicolor"}) {
                if (GetTheme(fallback).valid) return fallback;
            }
            return "hicolor";
        }
        return name;
    }

    Theme& GetTheme(const std::string& name) {
        auto it = fThemes.find(name);
        if (it != fThemes.end()) return it->second;
        Theme& theme = fThemes[name];
        std::string indexPath;
        for (const auto& base : fBaseDirs) {
            std::string root = base + "/" + name;
            if (!IsDirectory(root)) continue;
            theme.roots.push_back(root);
            if (indexPath.empty() && FileExists(root + "/index.theme")) indexPath = root + "/index.theme";
        }
        if (indexPath.empty()) return theme;
        auto ini = ReadIni(indexPath);
        auto& main = ini["Icon Theme"];
        theme.inherits = SplitList(main["Inherits"]);
        std::vector<std::string> subdirs = SplitList(main["Directories"]);
        for (const auto& extra : SplitList(main["ScaledDirectories"])) subdirs.push_back(extra);
        for (const auto& sub : subdirs) {
            auto g = ini.find(sub);
            if (g == ini.end()) continue;
            ThemeDir d;
            d.subdir = sub;
            auto get = [&](const char* k, int def) {
                auto kv = g->second.find(k);
                return (kv == g->second.end()) ? def : atoi(kv->second.c_str());
            };
            d.size = get("Size", 48);
            d.scale = get("Scale", 1);
            d.minSize = get("MinSize", d.size);
            d.maxSize = get("MaxSize", d.size);
            d.threshold = get("Threshold", 2);
            auto t = g->second.find("Type");
            std::string type = (t == g->second.end()) ? "Threshold" : t->second;
            if (type == "Fixed") d.type = ThemeDir::kFixed;
            else if (type == "Scalable") d.type = ThemeDir::kScalable;
            else d.type = ThemeDir::kThreshold;
            theme.dirs.push_back(d);
        }
        theme.valid = true;
        return theme;
    }

    static int SizeDistance(const ThemeDir& d, int size) {
        int dirSize = d.size * d.scale;
        switch (d.type) {
            case ThemeDir::kFixed:
                return std::abs(dirSize - size);
            case ThemeDir::kScalable:
                if (size < d.minSize * d.scale) return d.minSize * d.scale - size;
                if (size > d.maxSize * d.scale) return size - d.maxSize * d.scale;
                return 0;
            case ThemeDir::kThreshold:
            default:
                if (size < (d.size - d.threshold) * d.scale) return (d.size - d.threshold) * d.scale - size;
                if (size > (d.size + d.threshold) * d.scale) return size - (d.size + d.threshold) * d.scale;
                return 0;
        }
    }

    std::string FindInTheme(const std::string& themeName, const std::string& icon, int size) {
        Theme& theme = GetTheme(themeName);
        if (!theme.valid) return std::string();
        std::string best;
        int bestDistance = 1 << 30;
        bool bestIsSvg = false;
        for (const auto& d : theme.dirs) {
            int distance = SizeDistance(d, size);
            // Only look at a directory if it could beat what we already have;
            // a bitmap that has to be scaled *up* is penalized so a scalable
            // SVG wins over a blurry small PNG.
            if (distance > bestDistance) continue;
            for (const auto& root : theme.roots) {
                for (const char* ext : {".svg", ".png", ".xpm"}) {
                    std::string path = root + "/" + d.subdir + "/" + icon + ext;
                    if (!FileExists(path)) continue;
                    bool isSvg = (ext[1] == 's');
                    int penalized = distance;
                    if (!isSvg && d.type != ThemeDir::kScalable && d.size * d.scale < size) penalized += 1;
                    if (penalized < bestDistance || (penalized == bestDistance && isSvg && !bestIsSvg)) {
                        best = path;
                        bestDistance = penalized;
                        bestIsSvg = isSvg;
                    }
                    break;
                }
            }
            if (bestDistance == 0 && bestIsSvg) return best;
        }
        return best;
    }

    std::string FindInThemes(const std::string& icon, int size) {
        std::vector<std::string> order;
        std::set<std::string> seen;
        std::function<void(const std::string&)> walk = [&](const std::string& name) {
            if (seen.count(name)) return;
            seen.insert(name);
            order.push_back(name);
            for (const auto& parent : GetTheme(name).inherits) walk(parent);
        };
        walk(fThemeName);
        // Breeze and Adwaita hold most generic names the others inherit from.
        for (const char* extra : {"breeze", "Adwaita", "hicolor"}) walk(extra);
        for (const auto& name : order) {
            std::string found = FindInTheme(name, icon, size);
            if (!found.empty()) return found;
        }
        // Many themes now ship status/panel icons only as "-symbolic".
        if (!g_str_has_suffix(icon.c_str(), "-symbolic")) {
            for (const auto& name : order) {
                std::string found = FindInTheme(name, icon + "-symbolic", size);
                if (!found.empty()) return found;
            }
        }
        // Last resort: any other installed theme that happens to have it
        // (tray apps often assume the icon exists everywhere).
        for (const auto& base : fBaseDirs) {
            DIR* d = opendir(base.c_str());
            if (d == nullptr) continue;
            std::vector<std::string> themes;
            while (struct dirent* e = readdir(d)) {
                if (e->d_name[0] != '.' && !seen.count(e->d_name)) themes.push_back(e->d_name);
            }
            closedir(d);
            for (const auto& name : themes) {
                seen.insert(name);
                std::string found = FindInTheme(name, icon, size);
                if (!found.empty()) return found;
            }
        }
        return std::string();
    }

    static std::string FindInFlatDir(const std::string& dir, const std::string& icon, bool recurseOneLevel) {
        for (const char* ext : {".svg", ".png", ".xpm"}) {
            std::string p = dir + "/" + icon + ext;
            if (FileExists(p)) return p;
        }
        if (recurseOneLevel) {
            // IconThemePath usually points at a whole mini theme (hicolor/NNxNN/apps).
            DIR* d = opendir(dir.c_str());
            if (d == nullptr) return std::string();
            std::string found;
            std::vector<std::string> subdirs;
            while (struct dirent* e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                subdirs.push_back(dir + "/" + e->d_name);
            }
            closedir(d);
            for (const auto& sub : subdirs) {
                for (const char* mid : {"", "/apps", "/status", "/panel"}) {
                    for (const char* ext : {".svg", ".png"}) {
                        std::string p = sub + mid + "/" + icon + ext;
                        if (FileExists(p)) return p;
                    }
                }
                DIR* d2 = opendir(sub.c_str());
                if (d2 == nullptr) continue;
                std::vector<std::string> subsub;
                while (struct dirent* e = readdir(d2)) {
                    if (e->d_name[0] == '.') continue;
                    subsub.push_back(sub + "/" + e->d_name);
                }
                closedir(d2);
                for (const auto& ss : subsub) {
                    for (const char* mid : {"", "/apps", "/status", "/panel"}) {
                        for (const char* ext : {".svg", ".png"}) {
                            std::string p = ss + mid + "/" + icon + ext;
                            if (FileExists(p)) return p;
                        }
                    }
                }
            }
        }
        return std::string();
    }

    std::vector<std::string> fBaseDirs;
    std::vector<std::string> fPixmapDirs;
    std::string fThemeName;
    std::map<std::string, Theme> fThemes;
    std::unordered_map<std::string, std::string> fCache;
};

bool IconTheme::LoadWithPixbuf(cairo_t* cr, const std::string& path, int pxSize) {
    GError* err = nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), pxSize, pxSize, TRUE, &err);
    if (pb == nullptr) {
        g_clear_error(&err);
        return false;
    }
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int stride = gdk_pixbuf_get_rowstride(pb), n = gdk_pixbuf_get_n_channels(pb);
    bool alpha = gdk_pixbuf_get_has_alpha(pb);
    const guchar* px = gdk_pixbuf_get_pixels(pb);
    cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char* dst = cairo_image_surface_get_data(img);
    int dstStride = cairo_image_surface_get_stride(img);
    for (int y = 0; y < h; ++y) {
        const guchar* s = px + y * stride;
        uint32_t* d = reinterpret_cast<uint32_t*>(dst + y * dstStride);
        for (int x = 0; x < w; ++x) {
            uint32_t r = s[0], g = s[1], b = s[2], a = alpha ? s[3] : 255;
            r = r * a / 255; g = g * a / 255; b = b * a / 255;
            d[x] = (a << 24) | (r << 16) | (g << 8) | b;
            s += n;
        }
    }
    cairo_surface_mark_dirty(img);
    PaintScaled(cr, img, pxSize);
    cairo_surface_destroy(img);
    g_object_unref(pb);
    return true;
}

IconTheme* Theme()
{
	static IconTheme* theme = new IconTheme();
	return theme;
}

}	// namespace

IconRef MakeIconRef(cairo_surface_t* surface)
{
	if (surface == nullptr)
		return nullptr;
	return IconRef(surface, [](cairo_surface_t* p) { cairo_surface_destroy(p); });
}

IconRef CachedIcon(const std::string& name, int px, const std::string& extraDir)
{
	static std::map<std::string, IconRef> cache;
	if (name.empty() || px <= 0)
		return nullptr;
	px = static_cast<int>(std::lround(px * RenderScale()));		// (the drawing scales it back down to the logical size)
	const std::string key = name + "@" + std::to_string(px) + "@" + extraDir;
	auto found = cache.find(key);
	if (found != cache.end())
		return found->second;
	IconRef ref = MakeIconRef(Theme()->Load(name, px, extraDir));
	cache[key] = ref;
	return ref;
}
