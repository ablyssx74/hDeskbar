/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "colorwindow.h"

#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "loop.h"
#include "scale.h"
#include "shm.h"
#include "wl.h"

namespace {

constexpr int kWidth = 440, kHeight = 372;

struct Box {
	double x, y, w, h;

	bool Contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
	Box Grown(double d) const { return { x - d, y - d, w + 2 * d, h + 2 * d }; }
};

constexpr Box kSquare = { 24, 48, 240, 160 };		// saturation (left to right) and brightness (bottom to top)
constexpr Box kHueStrip = { 280, 48, 22, 160 };
constexpr Box kNew = { 322, 66, 94, 48 };
constexpr Box kOld = { 322, 142, 94, 48 };
constexpr double kPresetTop = 248;

struct Rgb {
	int c[3];
};

const Rgb kPresets[20] = {
	{{216, 216, 216}}, {{245, 245, 245}}, {{190, 195, 205}}, {{140, 142, 150}}, {{90, 95, 110}},
	{{48, 50, 58}}, {{28, 28, 32}}, {{0, 0, 0}}, {{255, 203, 0}}, {{240, 140, 40}},
	{{200, 60, 60}}, {{220, 110, 160}}, {{130, 80, 190}}, {{70, 80, 170}}, {{70, 110, 200}},
	{{110, 170, 230}}, {{40, 150, 150}}, {{70, 160, 90}}, {{130, 140, 60}}, {{140, 95, 60}},
};

Box PresetBox(size_t i)
{
	return { 24.0 + (i % 10) * 40, kPresetTop + (i / 10) * 32, 32, 24 };
}

enum Button { kDefault, kCancel, kApply };

Box ButtonBox(int i)
{
	const double w = 100, h = 30, bottom = kHeight - 20.0;
	if (i == kDefault)
		return { 24, bottom - h, w, h };
	const double right = kWidth - 24.0 - (i == kCancel ? w + 10 : 0);
	return { right - w, bottom - h, w, h };
}

void HsvToRgb(double h, double s, double v, Color* out)
{
	const double hh = std::fmod(h, 1.0) * 6.0;
	const int sector = static_cast<int>(hh);
	const double f = hh - sector, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
	switch (sector) {
		case 0: *out = { v, t, p }; break;
		case 1: *out = { q, v, p }; break;
		case 2: *out = { p, v, t }; break;
		case 3: *out = { p, q, v }; break;
		case 4: *out = { t, p, v }; break;
		default: *out = { v, p, q }; break;
	}
}

void RgbToHsv(const Color& c, double* h, double* s, double* v)
{
	const double mx = std::max({ c.r, c.g, c.b }), mn = std::min({ c.r, c.g, c.b }), d = mx - mn;
	*v = mx;
	*s = mx > 0 ? d / mx : 0;
	if (d <= 0)
		return;		// grey: the hue stays, so the marker on the strip does not jump
	if (mx == c.r)
		*h = std::fmod((c.g - c.b) / d + 6.0, 6.0) / 6.0;
	else if (mx == c.g)
		*h = ((c.b - c.r) / d + 2.0) / 6.0;
	else
		*h = ((c.r - c.g) / d + 4.0) / 6.0;
}

struct Window {
	std::string title;
	Color original, current, defaultColor;
	double h = 0, s = 0, v = 0;
	enum { kNone, kDragSquare, kDragHue } drag = kNone;
	std::function<void(const Color&)> onChange;
	std::function<void(bool)> onDone;
	double x = -1, y = -1;
	bool configured = false, finished = false;
	wl_surface* surface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_toplevel* toplevel = nullptr;
};

std::unique_ptr<Window> gWindow;

void Text(cairo_t* cr, const std::string& text, double x, double y, const char* font, double alpha = 1)
{
	PangoLayout* layout = pango_cairo_create_layout(cr);
	PangoFontDescription* description = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, description);
	pango_font_description_free(description);
	pango_layout_set_text(layout, text.c_str(), -1);
	cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, alpha);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

void Swatch(cairo_t* cr, const Box& b, const Color& c)
{
	RoundedRectPath(cr, b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, 4, 4, 4, 4);
	cairo_set_source_rgb(cr, c.r, c.g, c.b);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

void Paint(Window* w)
{
	if (!w->configured)
		return;
	wl_buffer* buffer = NewShmBuffer(kWidth, kHeight, [w](cairo_t* cr) {
		cairo_set_source_rgb(cr, kPanel.r, kPanel.g, kPanel.b);
		cairo_paint(cr);
		Text(cr, w->title, 24, 14, "Sans Bold 11");

		// the square: the hue at full, white to the right edge fading in from the left, black from the bottom
		Color hue;
		HsvToRgb(w->h, 1, 1, &hue);
		cairo_rectangle(cr, kSquare.x, kSquare.y, kSquare.w, kSquare.h);
		cairo_set_source_rgb(cr, hue.r, hue.g, hue.b);
		cairo_fill(cr);
		cairo_pattern_t* white = cairo_pattern_create_linear(kSquare.x, 0, kSquare.x + kSquare.w, 0);
		cairo_pattern_add_color_stop_rgba(white, 0, 1, 1, 1, 1);
		cairo_pattern_add_color_stop_rgba(white, 1, 1, 1, 1, 0);
		cairo_rectangle(cr, kSquare.x, kSquare.y, kSquare.w, kSquare.h);
		cairo_set_source(cr, white);
		cairo_fill(cr);
		cairo_pattern_destroy(white);
		cairo_pattern_t* black = cairo_pattern_create_linear(0, kSquare.y, 0, kSquare.y + kSquare.h);
		cairo_pattern_add_color_stop_rgba(black, 0, 0, 0, 0, 0);
		cairo_pattern_add_color_stop_rgba(black, 1, 0, 0, 0, 1);
		cairo_rectangle(cr, kSquare.x, kSquare.y, kSquare.w, kSquare.h);
		cairo_set_source(cr, black);
		cairo_fill(cr);
		cairo_pattern_destroy(black);
		cairo_rectangle(cr, kSquare.x - 0.5, kSquare.y - 0.5, kSquare.w + 1, kSquare.h + 1);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		cairo_new_path(cr);
		cairo_arc(cr, kSquare.x + w->s * kSquare.w, kSquare.y + (1 - w->v) * kSquare.h, 6, 0, 2 * M_PI);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_set_line_width(cr, 3);
		cairo_stroke_preserve(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);

		// the hue strip, and its marker
		cairo_pattern_t* hues = cairo_pattern_create_linear(0, kHueStrip.y, 0, kHueStrip.y + kHueStrip.h);
		for (int i = 0; i <= 6; i++) {
			Color c;
			HsvToRgb(i / 6.0, 1, 1, &c);
			cairo_pattern_add_color_stop_rgb(hues, i / 6.0, c.r, c.g, c.b);
		}
		cairo_rectangle(cr, kHueStrip.x, kHueStrip.y, kHueStrip.w, kHueStrip.h);
		cairo_set_source(cr, hues);
		cairo_fill(cr);
		cairo_pattern_destroy(hues);
		cairo_rectangle(cr, kHueStrip.x - 0.5, kHueStrip.y - 0.5, kHueStrip.w + 1, kHueStrip.h + 1);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		const double hy = kHueStrip.y + w->h * kHueStrip.h;
		cairo_rectangle(cr, kHueStrip.x - 3, hy - 3, kHueStrip.w + 6, 6);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_stroke(cr);

		// the new color and the current one, and the new one's number
		Text(cr, "New", kNew.x, kNew.y - 20, "Sans 9", 0.7);
		Swatch(cr, kNew, w->current);
		Text(cr, "Current", kOld.x, kOld.y - 20, "Sans 9", 0.7);
		Swatch(cr, kOld, w->original);
		char hex[16];
		snprintf(hex, sizeof(hex), "#%02X%02X%02X", static_cast<int>(std::lround(w->current.r * 255)),
			static_cast<int>(std::lround(w->current.g * 255)), static_cast<int>(std::lround(w->current.b * 255)));
		Text(cr, hex, kNew.x, kOld.y + kOld.h + 8, "Sans Bold 10");

		Text(cr, "Presets", 24, kPresetTop - 24, "Sans Bold 10");
		for (size_t i = 0; i < 20; i++) {
			const Box b = PresetBox(i);
			const Color c = { kPresets[i].c[0] / 255.0, kPresets[i].c[1] / 255.0, kPresets[i].c[2] / 255.0 };
			Swatch(cr, b, c);
			const bool chosen = std::abs(c.r - w->current.r) < 0.003 && std::abs(c.g - w->current.g) < 0.003
				&& std::abs(c.b - w->current.b) < 0.003;
			if (chosen || b.Contains(w->x, w->y)) {
				RoundedRectPath(cr, b.x - 2.5, b.y - 2.5, b.w + 5, b.h + 5, 5, 5, 5, 5);
				cairo_set_source_rgba(cr, kAccent.r, kAccent.g, kAccent.b, chosen ? 1.0 : 0.6);
				cairo_set_line_width(cr, 2);
				cairo_stroke(cr);
			}
		}

		static const char* const labels[] = { "Default", "Cancel", "Apply" };
		for (int i = 0; i < 3; i++) {
			const Box b = ButtonBox(i);
			const bool hover = b.Contains(w->x, w->y), primary = i == kApply;
			cairo_pattern_t* raised = cairo_pattern_create_linear(0, b.y, 0, b.y + b.h);
			const Color base = primary ? kAccent : Color{0.55, 0.57, 0.66};
			const Color light = Mix(base, {1, 1, 1}, hover ? 0.34 : 0.24), dark = Mix(base, {0, 0, 0}, 0.14);
			cairo_pattern_add_color_stop_rgb(raised, 0, light.r, light.g, light.b);
			cairo_pattern_add_color_stop_rgb(raised, 0.5, base.r, base.g, base.b);
			cairo_pattern_add_color_stop_rgb(raised, 1, dark.r, dark.g, dark.b);
			RoundedRectPath(cr, b.x, b.y, b.w, b.h, 15, 15, 15, 15);
			cairo_set_source(cr, raised);
			cairo_fill(cr);
			cairo_pattern_destroy(raised);
			PangoLayout* layout = pango_cairo_create_layout(cr);
			PangoFontDescription* description = pango_font_description_from_string(primary ? "Sans Bold 10" : "Sans 10");
			pango_layout_set_font_description(layout, description);
			pango_font_description_free(description);
			pango_layout_set_text(layout, labels[i], -1);
			int tw = 0, th = 0;
			pango_layout_get_pixel_size(layout, &tw, &th);
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_move_to(cr, b.x + (b.w - tw) / 2, b.y + (b.h - th) / 2);
			pango_cairo_show_layout(cr, layout);
			g_object_unref(layout);
		}
	});
	if (buffer == nullptr)
		return;
	AttachBuffer(w->surface, buffer, kWidth, kHeight);
	wl_surface_commit(w->surface);
}

void Chosen(Window* w)
{
	HsvToRgb(w->h, w->s, w->v, &w->current);
	if (w->onChange)
		w->onChange(w->current);
}

void SetColor(Window* w, const Color& c)
{
	w->current = c;
	RgbToHsv(c, &w->h, &w->s, &w->v);
	if (w->onChange)
		w->onChange(c);
}

void Finish(Window* w, bool applied)
{
	if (w->finished)
		return;
	w->finished = true;
	std::function<void(bool)> done = w->onDone;
	RunLater([done, applied]() {
		if (gWindow) {
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
		if (done)
			done(applied);
	});
}

void Cancel(Window* w)
{
	SetColor(w, w->original);
	Finish(w, false);
}

void DragTo(Window* w, double x, double y)
{
	if (w->drag == Window::kDragSquare) {
		w->s = std::clamp((x - kSquare.x) / kSquare.w, 0.0, 1.0);
		w->v = 1.0 - std::clamp((y - kSquare.y) / kSquare.h, 0.0, 1.0);
	} else if (w->drag == Window::kDragHue) {
		w->h = std::clamp((y - kHueStrip.y) / kHueStrip.h, 0.0, 1.0);
	}
	Chosen(w);
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
	[](void*, xdg_toplevel*) { if (gWindow) Cancel(gWindow.get()); },
	[](void*, xdg_toplevel*, int32_t, int32_t) {},
	[](void*, xdg_toplevel*, wl_array*) {},
};

}	// namespace

void OpenColorWindow(const std::string& title, const Color& current, const Color& defaultColor,
	std::function<void(const Color&)> onChange, std::function<void(bool)> onDone)
{
	if (gWindow || gWl.wmBase == nullptr)
		return;
	gWindow = std::make_unique<Window>();
	Window* w = gWindow.get();
	w->title = title;
	w->original = w->current = current;
	w->defaultColor = defaultColor;
	RgbToHsv(current, &w->h, &w->s, &w->v);
	w->onChange = std::move(onChange);
	w->onDone = std::move(onDone);
	w->surface = wl_compositor_create_surface(gWl.compositor);
	ScaleTrackSurface(w->surface);
	w->xdgSurface = xdg_wm_base_get_xdg_surface(gWl.wmBase, w->surface);
	xdg_surface_add_listener(w->xdgSurface, &kXdgSurfaceListener, w);
	w->toplevel = xdg_surface_get_toplevel(w->xdgSurface);
	xdg_toplevel_add_listener(w->toplevel, &kToplevelListener, w);
	xdg_toplevel_set_title(w->toplevel, title.c_str());
	xdg_toplevel_set_app_id(w->toplevel, "hdeskbar");
	xdg_toplevel_set_min_size(w->toplevel, kWidth, kHeight);
	xdg_toplevel_set_max_size(w->toplevel, kWidth, kHeight);
	wl_surface_commit(w->surface);
}

bool ColorWindowOpen()
{
	return static_cast<bool>(gWindow);
}

bool ColorWindowOwnsSurface(wl_surface* surface)
{
	return gWindow && gWindow->surface == surface;
}

void ColorWindowPointerEnter(double x, double y)
{
	ColorWindowPointerMotion(x, y);
}

void ColorWindowPointerLeave()
{
	if (gWindow) {
		gWindow->x = gWindow->y = -1;
		Paint(gWindow.get());
	}
}

void ColorWindowPointerMotion(double x, double y)
{
	if (!gWindow)
		return;
	gWindow->x = x;
	gWindow->y = y;
	if (gWindow->drag != Window::kNone)
		DragTo(gWindow.get(), x, y);
	Paint(gWindow.get());
}

void ColorWindowPointerButton(uint32_t button, uint32_t state)
{
	if (!gWindow || button != BTN_LEFT)
		return;
	Window* w = gWindow.get();
	const double x = w->x, y = w->y;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (kSquare.Grown(4).Contains(x, y))
			w->drag = Window::kDragSquare;
		else if (kHueStrip.Grown(4).Contains(x, y))
			w->drag = Window::kDragHue;
		if (w->drag != Window::kNone) {
			DragTo(w, x, y);
			Paint(w);
			return;
		}
		for (size_t i = 0; i < 20; i++) {
			if (PresetBox(i).Contains(x, y)) {
				SetColor(w, { kPresets[i].c[0] / 255.0, kPresets[i].c[1] / 255.0, kPresets[i].c[2] / 255.0 });
				Paint(w);
				return;
			}
		}
		return;
	}
	if (w->drag != Window::kNone) {
		w->drag = Window::kNone;
		return;
	}
	if (ButtonBox(kDefault).Contains(x, y)) {
		SetColor(w, w->defaultColor);
		Paint(w);
	} else if (ButtonBox(kCancel).Contains(x, y))
		Cancel(w);
	else if (ButtonBox(kApply).Contains(x, y))
		Finish(w, true);
}
