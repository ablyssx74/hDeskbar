/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The settings window: a window of its own (an xdg_toplevel), drawn in the same look as the bar. What is chosen takes effect at
 * once and is kept in the settings file.
 */

#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>

#include <glib.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "icon.h"
#include "icons.h"
#include "loop.h"
#include "settings.h"
#include "scale.h"
#include "shm.h"
#include "style.h"
#include "colorwindow.h"
#include "util.h"
#include "wl.h"

namespace {

constexpr int kWidth = 380, kHeight = 340;

enum Kind { kCheckTabs, kCheckTray, kCheckCpu, kCheckVolume, kCheck24, kCheckSeconds, kCheckTop, kCheckRaise, kCheckHide,
	kCheckUpdates, kCheckExpand, kCheckOwnPanel, kPlace, kColor, kPanelColor, kAccentOpacity, kPanelOpacity, kChooseIcon,
	kDefaultIcon, kSize, kTab, kClose };

struct Hit {
	double x, y, w, h;
	Kind kind;
	int index;

	bool Contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

const char* const kTabNames[] = { "Show", "Behavior", "Look", "Header" };
constexpr int kTabCount = 4;
const int kSizes[] = { 16, 24, 32, 48, 64 };

struct Window {
	wl_surface* surface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_toplevel* toplevel = nullptr;
	bool configured = false;
	int hover = -1;
	int tab = 0;
	int drag = -1;			// the slider that is being moved
	std::vector<Hit> hits;
};

std::unique_ptr<Window> gWindow;
double gX = -1, gY = -1;

const struct { const char* label; Placement placement; } kPlaces[] = {
	{"Top left", Placement::TopLeft}, {"Top right", Placement::TopRight},
	{"Bottom left", Placement::BottomLeft}, {"Bottom right", Placement::BottomRight},
	{"Along the top", Placement::Top}, {"Along the bottom", Placement::Bottom},
};

constexpr double kSliderX = 24, kSliderW = 230;

void MakeHits(Window* w)
{
	w->hits.clear();
	const double tabW = (kWidth - 32.0) / kTabCount;
	for (int i = 0; i < kTabCount; i++)
		w->hits.push_back({ 16 + i * tabW, 12, tabW, 26, kTab, i });
	const double left = 24;
	switch (w->tab) {
		case 0:
			w->hits.push_back({ left, 60, 200, 22, kCheckTabs, 0 });
			w->hits.push_back({ left, 86, 200, 22, kCheckExpand, 0 });
			w->hits.push_back({ left, 112, 200, 22, kCheckTray, 0 });
			w->hits.push_back({ left, 138, 200, 22, kCheckCpu, 0 });
			w->hits.push_back({ left, 164, 200, 22, kCheckVolume, 0 });
			w->hits.push_back({ left, 190, 200, 22, kCheck24, 0 });
			w->hits.push_back({ left, 216, 200, 22, kCheckSeconds, 0 });
			break;
		case 1:
			for (int i = 0; i < 6; i++)
				w->hits.push_back({ left + (i % 2) * 160.0, 78 + (i / 2) * 26.0, 150, 22, kPlace, i });
			w->hits.push_back({ left, 176, 200, 22, kCheckTop, 0 });
			w->hits.push_back({ left, 202, 200, 22, kCheckRaise, 0 });
			w->hits.push_back({ left, 228, 200, 22, kCheckHide, 0 });
			w->hits.push_back({ left, 254, 200, 22, kCheckUpdates, 0 });
			break;
		case 2:
			w->hits.push_back({ left, 60, 100, 26, kColor, 0 });
			w->hits.push_back({ kSliderX, 100, kSliderW, 20, kAccentOpacity, 0 });
			w->hits.push_back({ left, 160, 100, 26, kPanelColor, 0 });
			w->hits.push_back({ left + 112, 160, 200, 26, kCheckOwnPanel, 0 });
			w->hits.push_back({ kSliderX, 200, kSliderW, 20, kPanelOpacity, 0 });
			break;
		case 3:
			w->hits.push_back({ left + 64, 62, 130, 26, kChooseIcon, 0 });
			w->hits.push_back({ left + 204, 62, 100, 26, kDefaultIcon, 0 });
			for (int i = 0; i < 5; i++)
				w->hits.push_back({ left + i * 66.0, 146, 60, 22, kSize, i });
			break;
	}
	w->hits.push_back({ kWidth - 24 - 80.0, kHeight - 24 - 26.0, 80, 26, kClose, 0 });
}

void Text(cairo_t* cr, const std::string& text, double x, double y, bool bold, const Color& color, double alpha = 1)
{
	PangoLayout* layout = pango_cairo_create_layout(cr);
	PangoFontDescription* font = pango_font_description_from_string(bold ? "Sans Bold 9" : "Sans 9");
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, text.c_str(), -1);
	int width = 0, height = 0;
	pango_layout_get_pixel_size(layout, &width, &height);
	cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
	cairo_move_to(cr, x, y - height / 2.0);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(font);
	g_object_unref(layout);
}

bool Checked(Kind kind)
{
	switch (kind) {
		case kCheckTabs: return gSettings.showTabs;
		case kCheckTray: return gSettings.showTray;
		case kCheckCpu: return gSettings.showCpu;
		case kCheckVolume: return gSettings.showVolume;
		case kCheck24: return gSettings.clock24;
		case kCheckSeconds: return gSettings.clockSeconds;
		case kCheckTop: return gSettings.alwaysOnTop;
		case kCheckRaise: return gSettings.autoRaise;
		case kCheckHide: return gSettings.autoHide;
		case kCheckUpdates: return gSettings.checkForUpdates;
		case kCheckExpand: return gSettings.expandTabs;
		case kCheckOwnPanel: return !gSettings.ownPanel;
		default: return false;
	}
}

const char* CheckLabel(Kind kind)
{
	switch (kind) {
		case kCheckTabs: return "Program tabs";
		case kCheckTray: return "System tray";
		case kCheckCpu: return "CPU graph";
		case kCheckVolume: return "Volume icon";
		case kCheck24: return "24-hour clock";
		case kCheckSeconds: return "Seconds in the clock";
		case kCheckTop: return "Always on top";
		case kCheckRaise: return "Auto-raise";
		case kCheckHide: return "Auto-hide";
		case kCheckUpdates: return "Check for updates";
		case kCheckExpand: return "Expanding tabs";
		case kCheckOwnPanel: return "Match the highlight";
		default: return "";
	}
}

double* OpacityOf(Kind kind)
{
	return kind == kAccentOpacity ? &gSettings.accentOpacity : &gSettings.panelOpacity;
}

// a pill with the color in it, which opens the picker; the pill shows what the color looks like with its opacity over a checker
void DrawSwatch(cairo_t* cr, const Hit& hit, const Color& color, double opacity, bool hover)
{
	RoundedRectPath(cr, hit.x + 0.5, hit.y + 0.5, hit.w - 1, hit.h - 1, 13, 13, 13, 13);
	cairo_save(cr);
	cairo_clip_preserve(cr);
	for (int cx = 0; cx < hit.w; cx += 8) {
		for (int cy = 0; cy < hit.h; cy += 8) {
			const bool dark = ((cx / 8) + (cy / 8)) % 2 == 0;
			cairo_set_source_rgb(cr, dark ? 0.78 : 0.95, dark ? 0.78 : 0.95, dark ? 0.78 : 0.95);
			cairo_rectangle(cr, hit.x + cx, hit.y + cy, 8, 8);
			cairo_fill(cr);
		}
	}
	cairo_set_source_rgba(cr, color.r, color.g, color.b, opacity);
	cairo_paint(cr);
	cairo_restore(cr);
	RoundedRectPath(cr, hit.x + 0.5, hit.y + 0.5, hit.w - 1, hit.h - 1, 13, 13, 13, 13);
	cairo_set_source_rgba(cr, kOutline.r, kOutline.g, kOutline.b, hover ? 0.9 : 0.6);
	cairo_set_line_width(cr, hover ? 2 : 1);
	cairo_stroke(cr);
}

void DrawSlider(cairo_t* cr, const Hit& hit, double opacity, const Color& ink)
{
	const double ty = hit.y + hit.h / 2, t = (opacity - 0.2) / 0.8;
	cairo_set_line_width(cr, 4);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_source_rgba(cr, kOutline.r, kOutline.g, kOutline.b, 0.25);
	cairo_move_to(cr, hit.x + 8, ty);
	cairo_line_to(cr, hit.x + hit.w - 8, ty);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, kAccent.r, kAccent.g, kAccent.b);
	cairo_move_to(cr, hit.x + 8, ty);
	cairo_line_to(cr, hit.x + 8 + (hit.w - 16) * t, ty);
	cairo_stroke(cr);
	cairo_new_path(cr);
	cairo_arc(cr, hit.x + 8 + (hit.w - 16) * t, ty, 7, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, kOutline.r, kOutline.g, kOutline.b, 0.7);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	char percent[16];
	snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(opacity * 100 + 0.5));
	Text(cr, percent, hit.x + hit.w + 12, ty, false, ink);
}

void Paint(Window* w)
{
	if (!w->configured)
		return;
	wl_buffer* buffer = NewShmBuffer(kWidth, kHeight, [w](cairo_t* cr) {
		const Color ink = { 0.10, 0.10, 0.16 };
		cairo_set_source_rgb(cr, kPanel.r, kPanel.g, kPanel.b);
		cairo_paint(cr);

		if (w->tab == 1)
			Text(cr, "Place", 16, 60, true, ink);
		else if (w->tab == 2) {
			Text(cr, "Highlight color (the SnakeTracker color)", 16, 48, true, ink);
			Text(cr, "Opacity", 24, 94, false, ink, 0.8);
			Text(cr, "Background color", 16, 148, true, ink);
			Text(cr, "Opacity", 24, 194, false, ink, 0.8);
		} else if (w->tab == 3) {
			Text(cr, "Icon", 16, 48, true, ink);
			Text(cr, "Size of the header and its icon", 16, 128, true, ink);
		}

		for (size_t i = 0; i < w->hits.size(); i++) {
			const Hit& hit = w->hits[i];
			const bool hover = static_cast<int>(i) == w->hover;
			switch (hit.kind) {
				case kTab: {
					const bool current = hit.index == w->tab;
					RoundedRectPath(cr, hit.x + 2, hit.y, hit.w - 4, hit.h, 12, 12, 12, 12);
					if (current) {
						cairo_pattern_t* raised = cairo_pattern_create_linear(0, hit.y, 0, hit.y + hit.h);
						const Color light = Mix(kAccent, {1, 1, 1}, 0.24), dark = Mix(kAccent, {0, 0, 0}, 0.14);
						cairo_pattern_add_color_stop_rgb(raised, 0, light.r, light.g, light.b);
						cairo_pattern_add_color_stop_rgb(raised, 0.5, kAccent.r, kAccent.g, kAccent.b);
						cairo_pattern_add_color_stop_rgb(raised, 1, dark.r, dark.g, dark.b);
						cairo_set_source(cr, raised);
						cairo_pattern_destroy(raised);
					} else
						cairo_set_source_rgba(cr, kAccent.r, kAccent.g, kAccent.b, hover ? 0.28 : 0.14);
					cairo_fill(cr);
					PangoLayout* layout = pango_cairo_create_layout(cr);
					PangoFontDescription* font = pango_font_description_from_string("Sans Bold 9");
					pango_layout_set_font_description(layout, font);
					pango_layout_set_text(layout, kTabNames[hit.index], -1);
					int width = 0, height = 0;
					pango_layout_get_pixel_size(layout, &width, &height);
					if (current)
						cairo_set_source_rgb(cr, 1, 1, 1);
					else
						cairo_set_source_rgba(cr, ink.r, ink.g, ink.b, 0.85);
					cairo_move_to(cr, hit.x + (hit.w - width) / 2, hit.y + (hit.h - height) / 2.0);
					pango_cairo_show_layout(cr, layout);
					pango_font_description_free(font);
					g_object_unref(layout);
					break;
				}
				case kCheckTabs:
				case kCheckTray:
				case kCheckCpu:
				case kCheckVolume:
				case kCheck24:
				case kCheckSeconds:
				case kCheckTop:
				case kCheckRaise:
				case kCheckHide:
				case kCheckUpdates:
				case kCheckOwnPanel:
				case kCheckExpand: {
					const double bx = hit.x, by = hit.y + (hit.h - 14) / 2;
					RoundedRectPath(cr, bx, by, 14, 14, 3, 3, 3, 3);
					cairo_set_source_rgb(cr, 1, 1, 1);
					cairo_fill_preserve(cr);
					cairo_set_source_rgba(cr, kOutline.r, kOutline.g, kOutline.b, hover ? 0.9 : 0.6);
					cairo_set_line_width(cr, 1);
					cairo_stroke(cr);
					if (Checked(hit.kind)) {
						cairo_set_source_rgb(cr, kAccent.r, kAccent.g, kAccent.b);
						cairo_set_line_width(cr, 2.2);
						cairo_move_to(cr, bx + 3, by + 7.5);
						cairo_line_to(cr, bx + 6, by + 10.5);
						cairo_line_to(cr, bx + 11.5, by + 3.5);
						cairo_stroke(cr);
					}
					Text(cr, CheckLabel(hit.kind), hit.x + 24, hit.y + hit.h / 2, false, ink);
					break;
				}
				case kPlace:
				case kSize: {
					const bool on = hit.kind == kPlace ? kPlaces[hit.index].placement == gSettings.placement
						: kSizes[hit.index] == gSettings.headerSize;
					const double cx = hit.x + 7, cy = hit.y + hit.h / 2;
					cairo_new_path(cr);
					cairo_arc(cr, cx, cy, 7, 0, 2 * M_PI);
					cairo_set_source_rgb(cr, 1, 1, 1);
					cairo_fill_preserve(cr);
					cairo_set_source_rgba(cr, kOutline.r, kOutline.g, kOutline.b, hover ? 0.9 : 0.6);
					cairo_set_line_width(cr, 1);
					cairo_stroke(cr);
					if (on) {
						cairo_new_path(cr);
						cairo_arc(cr, cx, cy, 3.5, 0, 2 * M_PI);
						cairo_set_source_rgb(cr, kAccent.r, kAccent.g, kAccent.b);
						cairo_fill(cr);
					}
					Text(cr, hit.kind == kPlace ? kPlaces[hit.index].label : std::to_string(kSizes[hit.index]), hit.x + 22,
						hit.y + hit.h / 2, false, ink);
					break;
				}
				case kColor:
					DrawSwatch(cr, hit, gSettings.accent, gSettings.accentOpacity, hover);
					break;
				case kPanelColor:
					DrawSwatch(cr, hit, kPanel, gSettings.panelOpacity, hover);
					break;
				case kAccentOpacity:
				case kPanelOpacity:
					DrawSlider(cr, hit, *OpacityOf(hit.kind), ink);
					break;
				case kChooseIcon:
				case kDefaultIcon:
				case kClose: {
					cairo_pattern_t* raised = cairo_pattern_create_linear(0, hit.y, 0, hit.y + hit.h);
					const Color light = Mix(kAccent, {1, 1, 1}, hover ? 0.34 : 0.24), dark = Mix(kAccent, {0, 0, 0}, 0.14);
					cairo_pattern_add_color_stop_rgb(raised, 0, light.r, light.g, light.b);
					cairo_pattern_add_color_stop_rgb(raised, 0.5, kAccent.r, kAccent.g, kAccent.b);
					cairo_pattern_add_color_stop_rgb(raised, 1, dark.r, dark.g, dark.b);
					RoundedRectPath(cr, hit.x, hit.y, hit.w, hit.h, 13, 13, 13, 13);
					cairo_set_source(cr, raised);
					cairo_fill(cr);
					cairo_pattern_destroy(raised);
					const char* label = hit.kind == kClose ? "Close" : (hit.kind == kChooseIcon ? "Choose..." : "Default");
					PangoLayout* layout = pango_cairo_create_layout(cr);
					PangoFontDescription* font = pango_font_description_from_string("Sans Bold 9");
					pango_layout_set_font_description(layout, font);
					pango_layout_set_text(layout, label, -1);
					int width = 0, height = 0;
					pango_layout_get_pixel_size(layout, &width, &height);
					cairo_set_source_rgb(cr, 1, 1, 1);
					cairo_move_to(cr, hit.x + (hit.w - width) / 2, hit.y + (hit.h - height) / 2.0);
					pango_cairo_show_layout(cr, layout);
					pango_font_description_free(font);
					g_object_unref(layout);
					break;
				}
			}
		}

		if (w->tab == 3) {
			// the icon the header has now, on the header's own color
			const double size = 40, x = 24, y = 56;
			RoundedRectPath(cr, x, y, size + 8, size + 8, 8, 8, 8, 8);
			cairo_set_source_rgb(cr, kAccent.r, kAccent.g, kAccent.b);
			cairo_fill(cr);
			IconRef chosen = gSettings.headerIcon.empty() ? nullptr : CachedIcon(gSettings.headerIcon, static_cast<int>(size));
			if (chosen) {
				cairo_save(cr);
				cairo_translate(cr, x + 4, y + 4);
				cairo_scale(cr, size / cairo_image_surface_get_width(chosen.get()), size / cairo_image_surface_get_height(chosen.get()));
				cairo_set_source_surface(cr, chosen.get(), 0, 0);
				cairo_paint(cr);
				cairo_restore(cr);
			} else
				DrawSnakeTrackerIcon(cr, x + 4, y + 4, static_cast<int>(size));
		}
	});
	if (buffer == nullptr)
		return;
	AttachBuffer(w->surface, buffer, kWidth, kHeight);
	wl_surface_commit(w->surface);
}

void CloseWindow()
{
	if (!gWindow)
		return;
	if (gWindow->toplevel != nullptr)
		xdg_toplevel_destroy(gWindow->toplevel);
	if (gWindow->xdgSurface != nullptr)
		xdg_surface_destroy(gWindow->xdgSurface);
	if (gWindow->surface != nullptr) {
		ScaleForgetSurface(gWindow->surface);
		wl_surface_destroy(gWindow->surface);
	}
	gWindow.reset();
}

const xdg_surface_listener kXdgSurfaceListener = {
	[](void*, xdg_surface* xs, uint32_t serial) {
		xdg_surface_ack_configure(xs, serial);
		if (gWindow) {
			gWindow->configured = true;
			Paint(gWindow.get());
		}
	},
};

const xdg_toplevel_listener kToplevelListener = {
	[](void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {},
	[](void*, xdg_toplevel*) { RunLater([]() { CloseWindow(); }); },
	[](void*, xdg_toplevel*, int32_t, int32_t) {},
	[](void*, xdg_toplevel*, wl_array*) {},
};

int HitAt(double x, double y)
{
	if (!gWindow)
		return -1;
	for (size_t i = 0; i < gWindow->hits.size(); i++) {
		if (gWindow->hits[i].Contains(x, y))
			return static_cast<int>(i);
	}
	return -1;
}

// the picture of an icon: the file chooser of the desktop (kdialog on KDE, else zenity) runs on its own, and its answer is the file
bool gAsking = false;

void ChooseIcon()
{
	if (gAsking)
		return;
	const std::string home = g_get_home_dir();
	const char* kdialog[] = { "kdialog", "--getopenfilename", home.c_str(), "image/png image/svg+xml image/jpeg image/x-xpixmap|Pictures", nullptr };
	const char* zenity[] = { "zenity", "--file-selection", "--title=Icon for the header", "--file-filter=Pictures | *.png *.svg *.svgz *.jpg *.jpeg *.xpm", nullptr };
	const char** command = nullptr;
	if (g_find_program_in_path("kdialog") != nullptr)
		command = kdialog;
	else if (g_find_program_in_path("zenity") != nullptr)
		command = zenity;
	if (command == nullptr)
		return;
	GPid pid = 0;
	int out = -1;
	if (!g_spawn_async_with_pipes(nullptr, const_cast<char**>(command), nullptr,
			static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL), nullptr, nullptr,
			&pid, nullptr, &out, nullptr, nullptr))
		return;
	gAsking = true;
	struct Waiting { int out; };
	g_child_watch_add(pid, [](GPid pid, gint, gpointer data) {
		Waiting* waiting = static_cast<Waiting*>(data);
		std::string answer;
		char buffer[512];
		ssize_t n;
		while ((n = read(waiting->out, buffer, sizeof(buffer))) > 0)
			answer.append(buffer, n);
		close(waiting->out);
		delete waiting;
		g_spawn_close_pid(pid);
		while (!answer.empty() && (answer.back() == '\n' || answer.back() == '\r'))
			answer.pop_back();
		if (!answer.empty() && answer[0] == '/') {
			gSettings.headerIcon = answer;
			SaveSettings();
			if (gOnSettingsChanged)
				gOnSettingsChanged();
			if (gWindow)
				Paint(gWindow.get());
		}
		gAsking = false;		// (a second request is possible once the first has answered)
	}, new Waiting{ out });
}

void ChangedBySlider()
{
	ApplyColors();
	SaveSettings();
	if (gOnSettingsChanged)
		gOnSettingsChanged();
	if (gWindow)
		Paint(gWindow.get());
}

void MoveSlider(const Hit& hit, double x)
{
	double t = (x - (hit.x + 8)) / (hit.w - 16);
	t = std::clamp(t, 0.0, 1.0);
	*OpacityOf(hit.kind) = std::round((0.2 + 0.8 * t) * 100) / 100.0;
	ChangedBySlider();
}

}	// namespace

void OpenSettingsWindow()
{
	if (gWindow || gWl.wmBase == nullptr)
		return;
	gWindow = std::make_unique<Window>();
	Window* w = gWindow.get();
	MakeHits(w);
	w->surface = wl_compositor_create_surface(gWl.compositor);
	ScaleTrackSurface(w->surface);
	w->xdgSurface = xdg_wm_base_get_xdg_surface(gWl.wmBase, w->surface);
	xdg_surface_add_listener(w->xdgSurface, &kXdgSurfaceListener, w);
	w->toplevel = xdg_surface_get_toplevel(w->xdgSurface);
	xdg_toplevel_add_listener(w->toplevel, &kToplevelListener, w);
	xdg_toplevel_set_title(w->toplevel, "hDeskbar Settings");
	xdg_toplevel_set_app_id(w->toplevel, "hdeskbar");
	xdg_toplevel_set_min_size(w->toplevel, kWidth, kHeight);
	xdg_toplevel_set_max_size(w->toplevel, kWidth, kHeight);
	wl_surface_commit(w->surface);
}

bool SettingsOwnsSurface(wl_surface* surface)
{
	return gWindow && gWindow->surface == surface;
}

void SettingsPointerEnter(double x, double y)
{
	SettingsPointerMotion(x, y);
}

void SettingsPointerLeave()
{
	if (gWindow && gWindow->hover >= 0) {
		gWindow->hover = -1;
		Paint(gWindow.get());
	}
	gX = gY = -1;
}

void SettingsPointerMotion(double x, double y)
{
	gX = x;
	gY = y;
	if (!gWindow)
		return;
	if (gWindow->drag >= 0 && gWindow->drag < static_cast<int>(gWindow->hits.size())) {
		MoveSlider(gWindow->hits[gWindow->drag], x);
		return;
	}
	const int hit = HitAt(x, y);
	if (hit != gWindow->hover) {
		gWindow->hover = hit;
		Paint(gWindow.get());
	}
}

void SettingsPointerButton(uint32_t button, uint32_t state)
{
	if (!gWindow || button != BTN_LEFT)
		return;
	const int index = HitAt(gX, gY);
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (index >= 0 && (gWindow->hits[index].kind == kAccentOpacity || gWindow->hits[index].kind == kPanelOpacity)) {
			gWindow->drag = index;
			MoveSlider(gWindow->hits[index], gX);
		}
		return;
	}
	if (gWindow->drag >= 0) {
		gWindow->drag = -1;
		return;
	}
	if (index < 0)
		return;
	const Hit hit = gWindow->hits[index];
	switch (hit.kind) {
		case kTab:
			gWindow->tab = hit.index;
			MakeHits(gWindow.get());
			gWindow->hover = HitAt(gX, gY);
			Paint(gWindow.get());
			return;
		case kCheckTabs: gSettings.showTabs = !gSettings.showTabs; break;
		case kCheckTray: gSettings.showTray = !gSettings.showTray; break;
		case kCheckCpu: gSettings.showCpu = !gSettings.showCpu; break;
		case kCheckVolume: gSettings.showVolume = !gSettings.showVolume; break;
		case kCheck24: gSettings.clock24 = !gSettings.clock24; break;
		case kCheckSeconds: gSettings.clockSeconds = !gSettings.clockSeconds; break;
		case kCheckTop: gSettings.alwaysOnTop = !gSettings.alwaysOnTop; break;
		case kCheckRaise: gSettings.autoRaise = !gSettings.autoRaise; break;
		case kCheckHide: gSettings.autoHide = !gSettings.autoHide; break;
		case kCheckUpdates: gSettings.checkForUpdates = !gSettings.checkForUpdates; break;
		case kCheckExpand: gSettings.expandTabs = !gSettings.expandTabs; break;
		case kCheckOwnPanel:
			gSettings.ownPanel = !gSettings.ownPanel;
			ApplyColors();
			break;
		case kPlace: gSettings.placement = kPlaces[hit.index].placement; break;
		case kSize: gSettings.headerSize = kSizes[hit.index]; break;
		case kDefaultIcon: gSettings.headerIcon.clear(); break;
		case kChooseIcon:
			ChooseIcon();
			return;
		case kColor:
			OpenColorWindow("Highlight color", gSettings.accent, Color{ 70 / 255.0, 110 / 255.0, 200 / 255.0 },
				[](const Color& c) {
					gSettings.accent = c;
					ApplyColors();
					SaveSettings();
					if (gOnSettingsChanged)
						gOnSettingsChanged();
					if (gWindow)
						Paint(gWindow.get());
				},
				[](bool) {});
			return;
		case kPanelColor:
			OpenColorWindow("Background color", kPanel, Color{ 200 / 255.0, 203 / 255.0, 235 / 255.0 },
				[](const Color& c) {
					gSettings.panel = c;
					gSettings.ownPanel = true;
					ApplyColors();
					SaveSettings();
					if (gOnSettingsChanged)
						gOnSettingsChanged();
					if (gWindow)
						Paint(gWindow.get());
				},
				[](bool) {});
			return;
		case kAccentOpacity:
		case kPanelOpacity:
			return;
		case kClose:
			RunLater([]() { CloseWindow(); });
			return;
	}
	SaveSettings();
	if (gOnSettingsChanged)
		gOnSettingsChanged();
	Paint(gWindow.get());
}
