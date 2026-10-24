/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "alert.h"

#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <memory>

#include "loop.h"
#include "scale.h"
#include "shm.h"
#include "style.h"
#include "wl.h"

namespace {

constexpr int kWidth = 440;
constexpr int kButtonWidth = 110, kButtonHeight = 28, kGap = 10;

struct Alert {
	std::string title, text;
	std::vector<std::string> buttons;
	std::function<void(int)> onChoice;
	int defaultButton = 0;
	int height = 140;
	int hover = -1;
	bool configured = false, finished = false;
	wl_surface* surface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_toplevel* toplevel = nullptr;
};

std::vector<std::unique_ptr<Alert>> gAlerts;
Alert* gOver = nullptr;
double gX = -1, gY = -1;

PangoLayout* TextLayout(cairo_t* cr, const std::string& text, const char* font, int width)
{
	PangoLayout* layout = pango_cairo_create_layout(cr);
	PangoFontDescription* description = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, description);
	pango_font_description_free(description);
	pango_layout_set_text(layout, text.c_str(), -1);
	if (width > 0) {
		pango_layout_set_width(layout, width * PANGO_SCALE);
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	}
	return layout;
}

struct Box {
	double x, y, w, h;
	bool Contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

Box ButtonBox(const Alert* a, size_t i)
{
	const double right = kWidth - 24.0;
	const size_t fromRight = a->buttons.size() - 1 - i;
	return { right - (fromRight + 1) * kButtonWidth - fromRight * kGap, a->height - 20.0 - kButtonHeight, kButtonWidth, kButtonHeight };
}

void Paint(Alert* a)
{
	if (!a->configured)
		return;
	wl_buffer* buffer = NewShmBuffer(kWidth, a->height, [a](cairo_t* cr) {
		cairo_set_source_rgb(cr, kPanel.r, kPanel.g, kPanel.b);
		cairo_paint(cr);
		PangoLayout* title = TextLayout(cr, a->title, "Sans Bold 11", 0);
		cairo_set_source_rgb(cr, 0.10, 0.10, 0.16);
		cairo_move_to(cr, 24, 18);
		pango_cairo_show_layout(cr, title);
		g_object_unref(title);
		PangoLayout* text = TextLayout(cr, a->text, "Sans 10", kWidth - 48);
		cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, 0.9);
		cairo_move_to(cr, 24, 50);
		pango_cairo_show_layout(cr, text);
		g_object_unref(text);
		for (size_t i = 0; i < a->buttons.size(); i++) {
			const Box b = ButtonBox(a, i);
			const bool primary = static_cast<int>(i) == a->defaultButton;
			const bool hover = static_cast<int>(i) == a->hover;
			cairo_pattern_t* raised = cairo_pattern_create_linear(0, b.y, 0, b.y + b.h);
			const Color base = primary ? kAccent : Color{0.55, 0.57, 0.66};
			const Color light = Mix(base, {1, 1, 1}, hover ? 0.34 : 0.24), dark = Mix(base, {0, 0, 0}, 0.14);
			cairo_pattern_add_color_stop_rgb(raised, 0, light.r, light.g, light.b);
			cairo_pattern_add_color_stop_rgb(raised, 0.5, base.r, base.g, base.b);
			cairo_pattern_add_color_stop_rgb(raised, 1, dark.r, dark.g, dark.b);
			RoundedRectPath(cr, b.x, b.y, b.w, b.h, 14, 14, 14, 14);
			cairo_set_source(cr, raised);
			cairo_fill(cr);
			cairo_pattern_destroy(raised);
			PangoLayout* label = TextLayout(cr, a->buttons[i], primary ? "Sans Bold 10" : "Sans 10", 0);
			int w = 0, h = 0;
			pango_layout_get_pixel_size(label, &w, &h);
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_move_to(cr, b.x + (b.w - w) / 2, b.y + (b.h - h) / 2);
			pango_cairo_show_layout(cr, label);
			g_object_unref(label);
		}
	});
	if (buffer == nullptr)
		return;
	AttachBuffer(a->surface, buffer, kWidth, a->height);
	wl_surface_commit(a->surface);
}

void Remove(Alert* a)
{
	if (gOver == a)
		gOver = nullptr;
	if (a->toplevel != nullptr)
		xdg_toplevel_destroy(a->toplevel);
	if (a->xdgSurface != nullptr)
		xdg_surface_destroy(a->xdgSurface);
	if (a->surface != nullptr) {
		ScaleForgetSurface(a->surface);
		wl_surface_destroy(a->surface);
	}
	for (auto it = gAlerts.begin(); it != gAlerts.end(); ++it) {
		if (it->get() == a) {
			gAlerts.erase(it);
			return;
		}
	}
}

// The answer: the window goes, and the choice is told (after, so what it does may open another).
void Finish(Alert* a, int choice)
{
	if (a->finished)
		return;
	a->finished = true;
	std::function<void(int)> done = a->onChoice;
	RunLater([a, done, choice]() {
		bool present = false;
		for (const auto& alert : gAlerts)
			present |= alert.get() == a;
		if (present)
			Remove(a);
		if (done)
			done(choice);
	});
}

const xdg_surface_listener kXdgSurfaceListener = {
	[](void* data, xdg_surface* xs, uint32_t serial) {
		auto* a = static_cast<Alert*>(data);
		xdg_surface_ack_configure(xs, serial);
		a->configured = true;
		Paint(a);
	},
};

const xdg_toplevel_listener kToplevelListener = {
	[](void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {},
	[](void* data, xdg_toplevel*) { Finish(static_cast<Alert*>(data), 0); },
	[](void*, xdg_toplevel*, int32_t, int32_t) {},
	[](void*, xdg_toplevel*, wl_array*) {},
};

}	// namespace

void ShowAlert(const std::string& title, const std::string& text, std::vector<std::string> buttons,
	std::function<void(int)> onChoice, int defaultButton)
{
	if (gWl.wmBase == nullptr)
		return;
	auto alert = std::make_unique<Alert>();
	Alert* a = alert.get();
	a->title = title;
	a->text = text;
	a->buttons = buttons.empty() ? std::vector<std::string>{"OK"} : std::move(buttons);
	a->onChoice = std::move(onChoice);
	a->defaultButton = (defaultButton < 0 || defaultButton >= static_cast<int>(a->buttons.size())) ? a->buttons.size() - 1
		: defaultButton;
	{
		// the height: the title, the text where it wraps, and the buttons
		static cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
		static cairo_t* cr = cairo_create(scratch);
		PangoLayout* layout = TextLayout(cr, a->text, "Sans 10", kWidth - 48);
		int w = 0, h = 0;
		pango_layout_get_pixel_size(layout, &w, &h);
		g_object_unref(layout);
		a->height = 50 + h + 24 + kButtonHeight + 20;
	}
	a->surface = wl_compositor_create_surface(gWl.compositor);
	ScaleTrackSurface(a->surface);
	a->xdgSurface = xdg_wm_base_get_xdg_surface(gWl.wmBase, a->surface);
	xdg_surface_add_listener(a->xdgSurface, &kXdgSurfaceListener, a);
	a->toplevel = xdg_surface_get_toplevel(a->xdgSurface);
	xdg_toplevel_add_listener(a->toplevel, &kToplevelListener, a);
	xdg_toplevel_set_title(a->toplevel, a->title.c_str());
	xdg_toplevel_set_app_id(a->toplevel, "hdeskbar");
	xdg_toplevel_set_min_size(a->toplevel, kWidth, a->height);
	xdg_toplevel_set_max_size(a->toplevel, kWidth, a->height);
	wl_surface_commit(a->surface);
	gAlerts.push_back(std::move(alert));
}

bool AlertOwnsSurface(wl_surface* surface)
{
	for (const auto& a : gAlerts) {
		if (a->surface == surface)
			return true;
	}
	return false;
}

void AlertPointerEnter(wl_surface* surface, double x, double y)
{
	gOver = nullptr;
	for (const auto& a : gAlerts) {
		if (a->surface == surface)
			gOver = a.get();
	}
	AlertPointerMotion(x, y);
}

void AlertPointerLeave()
{
	if (gOver != nullptr && gOver->hover >= 0) {
		gOver->hover = -1;
		Paint(gOver);
	}
	gOver = nullptr;
}

void AlertPointerMotion(double x, double y)
{
	gX = x;
	gY = y;
	if (gOver == nullptr)
		return;
	int hover = -1;
	for (size_t i = 0; i < gOver->buttons.size(); i++) {
		if (ButtonBox(gOver, i).Contains(x, y))
			hover = static_cast<int>(i);
	}
	if (hover != gOver->hover) {
		gOver->hover = hover;
		Paint(gOver);
	}
}

void AlertPointerButton(uint32_t button, uint32_t state)
{
	if (gOver == nullptr || button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_RELEASED)
		return;
	for (size_t i = 0; i < gOver->buttons.size(); i++) {
		if (ButtonBox(gOver, i).Contains(gX, gY)) {
			Finish(gOver, static_cast<int>(i));
			return;
		}
	}
}
