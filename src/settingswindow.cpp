/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The settings window: a window of its own (an xdg_toplevel), drawn in the same look as the bar. What is chosen takes effect at
 * once and is kept in the settings file.
 */

#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>

#include <cmath>
#include <memory>
#include <vector>

#include "loop.h"
#include "settings.h"
#include "shm.h"
#include "style.h"
#include "util.h"
#include "wl.h"

namespace {

constexpr int kWidth = 360, kHeight = 376;

struct Swatch {
	const char* name;
	Color color;
};

const Swatch kSwatches[] = {
	{"Blue", { 70 / 255.0, 110 / 255.0, 200 / 255.0 }},
	{"Purple", { 125 / 255.0, 95 / 255.0, 205 / 255.0 }},
	{"Teal", { 40 / 255.0, 150 / 255.0, 160 / 255.0 }},
	{"Green", { 70 / 255.0, 160 / 255.0, 90 / 255.0 }},
	{"Orange", { 225 / 255.0, 130 / 255.0, 45 / 255.0 }},
	{"Red", { 205 / 255.0, 70 / 255.0, 70 / 255.0 }},
	{"Pink", { 210 / 255.0, 95 / 255.0, 150 / 255.0 }},
	{"Gray", { 110 / 255.0, 115 / 255.0, 130 / 255.0 }},
};

enum Kind { kCheckTabs, kCheckTray, kCheckCpu, kCheckVolume, kPlace, kColor, kClose };

struct Hit {
	double x, y, w, h;
	Kind kind;
	int index;

	bool Contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct Window {
	wl_surface* surface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_toplevel* toplevel = nullptr;
	bool configured = false;
	int hover = -1;
	std::vector<Hit> hits;
};

std::unique_ptr<Window> gWindow;
double gX = -1, gY = -1;

const struct { const char* label; Placement placement; } kPlaces[] = {
	{"Top left", Placement::TopLeft}, {"Top right", Placement::TopRight},
	{"Bottom left", Placement::BottomLeft}, {"Bottom right", Placement::BottomRight},
	{"Along the top", Placement::Top}, {"Along the bottom", Placement::Bottom},
};

void MakeHits(Window* w)
{
	w->hits.clear();
	const double left = 24;
	w->hits.push_back({ left, 44, 300, 22, kCheckTabs, 0 });
	w->hits.push_back({ left, 68, 300, 22, kCheckTray, 0 });
	w->hits.push_back({ left, 92, 300, 22, kCheckCpu, 0 });
	w->hits.push_back({ left, 116, 300, 22, kCheckVolume, 0 });
	for (int i = 0; i < 6; i++)
		w->hits.push_back({ left + (i % 2) * 160.0, 168 + (i / 2) * 24.0, 150, 22, kPlace, i });
	for (int i = 0; i < 8; i++)
		w->hits.push_back({ left + i * 38.0, 286, 30, 30, kColor, i });
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
		default: return false;
	}
}

void Paint(Window* w)
{
	if (!w->configured)
		return;
	wl_buffer* buffer = NewShmBuffer(kWidth, kHeight, [w](cairo_t* cr) {
		const Color ink = { 0.10, 0.10, 0.16 };
		cairo_set_source_rgb(cr, kPanel.r, kPanel.g, kPanel.b);
		cairo_paint(cr);

		Text(cr, "Show", 16, 26, true, ink);
		Text(cr, "Place", 16, 150, true, ink);
		Text(cr, "SnakeTracker color", 16, 268, true, ink);

		for (size_t i = 0; i < w->hits.size(); i++) {
			const Hit& hit = w->hits[i];
			const bool hover = static_cast<int>(i) == w->hover;
			switch (hit.kind) {
				case kCheckTabs:
				case kCheckTray:
				case kCheckCpu:
				case kCheckVolume: {
					static const char* const labels[] = { "Program tabs", "System tray", "CPU graph", "Volume icon" };
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
					Text(cr, labels[hit.kind - kCheckTabs], hit.x + 24, hit.y + hit.h / 2, false, ink);
					break;
				}
				case kPlace: {
					const double cx = hit.x + 7, cy = hit.y + hit.h / 2;
					cairo_new_path(cr);
					cairo_arc(cr, cx, cy, 7, 0, 2 * M_PI);
					cairo_set_source_rgb(cr, 1, 1, 1);
					cairo_fill_preserve(cr);
					cairo_set_source_rgba(cr, kOutline.r, kOutline.g, kOutline.b, hover ? 0.9 : 0.6);
					cairo_set_line_width(cr, 1);
					cairo_stroke(cr);
					if (kPlaces[hit.index].placement == gSettings.placement) {
						cairo_new_path(cr);
						cairo_arc(cr, cx, cy, 3.5, 0, 2 * M_PI);
						cairo_set_source_rgb(cr, kAccent.r, kAccent.g, kAccent.b);
						cairo_fill(cr);
					}
					Text(cr, kPlaces[hit.index].label, hit.x + 22, hit.y + hit.h / 2, false, ink);
					break;
				}
				case kColor: {
					const Color& color = kSwatches[hit.index].color;
					RoundedRectPath(cr, hit.x + 2, hit.y + 2, hit.w - 4, hit.h - 4, 7, 7, 7, 7);
					cairo_set_source_rgb(cr, color.r, color.g, color.b);
					cairo_fill(cr);
					const bool chosen = std::abs(color.r - gSettings.accent.r) < 0.01 && std::abs(color.g - gSettings.accent.g) < 0.01
						&& std::abs(color.b - gSettings.accent.b) < 0.01;
					if (chosen || hover) {
						RoundedRectPath(cr, hit.x + 0.5, hit.y + 0.5, hit.w - 1, hit.h - 1, 9, 9, 9, 9);
						cairo_set_source_rgba(cr, kOutline.r, kOutline.g, kOutline.b, chosen ? 0.9 : 0.4);
						cairo_set_line_width(cr, chosen ? 2 : 1);
						cairo_stroke(cr);
					}
					break;
				}
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
					Text(cr, "Close", hit.x + 24, hit.y + hit.h / 2, true, { 1, 1, 1 });
					break;
				}
			}
		}
	});
	if (buffer == nullptr)
		return;
	wl_surface_attach(w->surface, buffer, 0, 0);
	wl_surface_damage_buffer(w->surface, 0, 0, kWidth, kHeight);
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
	if (gWindow->surface != nullptr)
		wl_surface_destroy(gWindow->surface);
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

}	// namespace

void OpenSettingsWindow()
{
	if (gWindow || gWl.wmBase == nullptr)
		return;
	gWindow = std::make_unique<Window>();
	Window* w = gWindow.get();
	MakeHits(w);
	w->surface = wl_compositor_create_surface(gWl.compositor);
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
	const int hit = HitAt(x, y);
	if (hit != gWindow->hover) {
		gWindow->hover = hit;
		Paint(gWindow.get());
	}
}

void SettingsPointerButton(uint32_t button, uint32_t state)
{
	if (!gWindow || button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_RELEASED)
		return;
	const int index = HitAt(gX, gY);
	if (index < 0)
		return;
	const Hit hit = gWindow->hits[index];
	switch (hit.kind) {
		case kCheckTabs: gSettings.showTabs = !gSettings.showTabs; break;
		case kCheckTray: gSettings.showTray = !gSettings.showTray; break;
		case kCheckCpu: gSettings.showCpu = !gSettings.showCpu; break;
		case kCheckVolume: gSettings.showVolume = !gSettings.showVolume; break;
		case kPlace: gSettings.placement = kPlaces[hit.index].placement; break;
		case kColor:
			gSettings.accent = kSwatches[hit.index].color;
			SetAccent(gSettings.accent);
			break;
		case kClose:
			RunLater([]() { CloseWindow(); });
			return;
	}
	SaveSettings();
	if (gOnSettingsChanged)
		gOnSettingsChanged();
	Paint(gWindow.get());
}
