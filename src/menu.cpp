/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "menu.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "icons.h"
#include "loop.h"
#include "shm.h"
#include "style.h"
#include "util.h"
#include "wl.h"

namespace {

constexpr int kRow = 22;			// a row
constexpr int kSeparator = 7;		// a separator's
constexpr int kPad = 4;			// above the first row and under the last
constexpr double kMenuRadius = 8;
constexpr int kIconSize = 16;

struct Popup {
	std::vector<MenuItem> items;
	std::vector<int> tops;			// where each row starts, from the top of the content
	int content = 0;				// the height of all the rows
	int width = 0, height = 0;		// the size of the surface
	int scroll = 0;
	int hover = -1;
	bool hasIcons = false, hasChecks = false;
	bool configured = false;
	Popup* parent = nullptr;
	int parentRow = -1;
	wl_surface* surface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_popup* popup = nullptr;
	guint openTimer = 0;
};

std::vector<std::unique_ptr<Popup>> gStack;		// the menus that are open: the first is the root, the last the newest
Popup* gOver = nullptr;						// the menu the pointer is over
double gPointerX = 0, gPointerY = 0;

double MeasureLabel(const std::string& text)
{
	static cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	static cairo_t* cr = cairo_create(scratch);
	PangoLayout* layout = pango_cairo_create_layout(cr);
	PangoFontDescription* font = pango_font_description_from_string("Sans 9");
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, text.c_str(), -1);
	int width = 0, height = 0;
	pango_layout_get_pixel_size(layout, &width, &height);
	pango_font_description_free(font);
	g_object_unref(layout);
	return width;
}

void LayOut(Popup* p)
{
	p->tops.clear();
	int y = 0;
	double widest = 0;
	for (const MenuItem& item : p->items) {
		p->tops.push_back(y);
		y += item.separator ? kSeparator : kRow;
		if (!item.separator)
			widest = std::max(widest, MeasureLabel(item.label));
		if (!item.iconName.empty() || item.icon)
			p->hasIcons = true;
		if (item.check != kCheckNone)
			p->hasChecks = true;
	}
	p->content = y;
	p->width = static_cast<int>(std::ceil(widest)) + 8 + (p->hasIcons ? 24 : 4) + (p->hasChecks ? 16 : 0) + 26;
	p->width = std::clamp(p->width, 150, 420);
	const int most = (gWl.screenHeight > 0 ? gWl.screenHeight : 700) - 40;
	p->height = std::min(p->content + 2 * kPad, std::max(most, 100));
}

int ItemAt(const Popup* p, double y)
{
	const double inside = y - kPad + p->scroll;
	for (size_t i = 0; i < p->items.size(); i++) {
		const int top = p->tops[i];
		const int height = p->items[i].separator ? kSeparator : kRow;
		if (inside >= top && inside < top + height)
			return static_cast<int>(i);
	}
	return -1;
}

void Paint(Popup* p)
{
	if (!p->configured || p->surface == nullptr)
		return;
	wl_buffer* buffer = NewShmBuffer(p->width, p->height, [p](cairo_t* cr) {
		const double w = p->width, h = p->height;
		RoundedRectPath(cr, 0, 0, w, h, kMenuRadius, kMenuRadius, kMenuRadius, kMenuRadius);
		cairo_set_source_rgb(cr, kPanel.r, kPanel.g, kPanel.b);
		cairo_fill(cr);

		cairo_save(cr);
		RoundedRectPath(cr, 1, 1, w - 2, h - 2, kMenuRadius - 1, kMenuRadius - 1, kMenuRadius - 1, kMenuRadius - 1);
		cairo_clip(cr);
		for (size_t i = 0; i < p->items.size(); i++) {
			const MenuItem& item = p->items[i];
			const double y = kPad + p->tops[i] - p->scroll;
			if (y + kRow < 0 || y > h)
				continue;
			if (item.separator) {
				cairo_set_source_rgba(cr, 0, 0, 0, 0.20);
				cairo_rectangle(cr, 8, y + 3, w - 16, 1);
				cairo_fill(cr);
				cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
				cairo_rectangle(cr, 8, y + 4, w - 16, 1);
				cairo_fill(cr);
				continue;
			}
			const bool lit = static_cast<int>(i) == p->hover && item.enabled;
			if (lit) {
				cairo_pattern_t* raised = cairo_pattern_create_linear(0, y, 0, y + kRow);
				const Color light = Mix(kAccent, {1, 1, 1}, 0.24), dark = Mix(kAccent, {0, 0, 0}, 0.14);
				cairo_pattern_add_color_stop_rgb(raised, 0, light.r, light.g, light.b);
				cairo_pattern_add_color_stop_rgb(raised, 0.5, kAccent.r, kAccent.g, kAccent.b);
				cairo_pattern_add_color_stop_rgb(raised, 1, dark.r, dark.g, dark.b);
				RoundedRectPath(cr, 4, y + 1, w - 8, kRow - 2, 10, 10, 10, 10);
				cairo_set_source(cr, raised);
				cairo_fill(cr);
				cairo_pattern_destroy(raised);
			}
			const double alpha = item.enabled ? 1.0 : 0.45;
			const double checkWidth = p->hasChecks ? 16 : 0;
			if (item.check != kCheckNone) {
				// a mark for a checked item, a dot for the chosen one of a set
				const bool on = item.check == kCheckOn || item.check == kRadioOn;
				if (on) {
					if (lit)
						cairo_set_source_rgb(cr, 1, 1, 1);
					else
						cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, alpha);
					if (item.check == kCheckOn) {
						cairo_set_line_width(cr, 1.8);
						cairo_move_to(cr, 8, y + kRow / 2.0);
						cairo_line_to(cr, 11, y + kRow / 2.0 + 3);
						cairo_line_to(cr, 16, y + kRow / 2.0 - 4);
						cairo_stroke(cr);
					} else {
						cairo_new_path(cr);
						cairo_arc(cr, 12, y + kRow / 2.0, 3, 0, 2 * M_PI);
						cairo_fill(cr);
					}
				}
			}
			IconRef icon = item.icon;
			if (!icon && !item.iconName.empty())
				icon = CachedIcon(item.iconName, kIconSize);
			if (icon) {
				{
					cairo_save(cr);
					cairo_translate(cr, 10 + checkWidth, y + (kRow - kIconSize) / 2.0);
					cairo_scale(cr, kIconSize / static_cast<double>(cairo_image_surface_get_width(icon.get())),
						kIconSize / static_cast<double>(cairo_image_surface_get_height(icon.get())));
					cairo_set_source_surface(cr, icon.get(), 0, 0);
					cairo_paint_with_alpha(cr, alpha);
					cairo_restore(cr);
				}
			}
			const double textX = (p->hasIcons ? 34 : 12) + checkWidth;
			PangoLayout* layout = pango_cairo_create_layout(cr);
			PangoFontDescription* font = pango_font_description_from_string("Sans 9");
			pango_layout_set_font_description(layout, font);
			pango_layout_set_text(layout, item.label.c_str(), -1);
			pango_layout_set_width(layout, static_cast<int>((w - textX - 20) * PANGO_SCALE));
			pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
			int textWidth = 0, textHeight = 0;
			pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
			if (lit)
				cairo_set_source_rgb(cr, 1, 1, 1);
			else
				cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, alpha);
			cairo_move_to(cr, textX, y + (kRow - textHeight) / 2.0);
			pango_cairo_show_layout(cr, layout);
			pango_font_description_free(font);
			g_object_unref(layout);

			if (item.submenu) {
				const double cx = w - 14, cy = y + kRow / 2.0;
				cairo_set_line_width(cr, 1.6);
				cairo_move_to(cr, cx - 2, cy - 3.5);
				cairo_line_to(cr, cx + 2, cy);
				cairo_line_to(cr, cx - 2, cy + 3.5);
				if (lit)
					cairo_set_source_rgb(cr, 1, 1, 1);
				else
					cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, alpha);
				cairo_stroke(cr);
			}
		}
		// the arrows of a long menu that scrolls
		if (p->content + 2 * kPad > p->height) {
			cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, 0.55);
			if (p->scroll > 0) {
				cairo_move_to(cr, w / 2 - 4, 8);
				cairo_line_to(cr, w / 2, 3);
				cairo_line_to(cr, w / 2 + 4, 8);
				cairo_fill(cr);
			}
			if (p->scroll + p->height - 2 * kPad < p->content) {
				cairo_move_to(cr, w / 2 - 4, h - 8);
				cairo_line_to(cr, w / 2, h - 3);
				cairo_line_to(cr, w / 2 + 4, h - 8);
				cairo_fill(cr);
			}
		}
		cairo_restore(cr);

		RoundedRectPath(cr, 0.5, 0.5, w - 1, h - 1, kMenuRadius - 0.5, kMenuRadius - 0.5, kMenuRadius - 0.5, kMenuRadius - 0.5);
		cairo_set_source_rgb(cr, kOutline.r, kOutline.g, kOutline.b);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	});
	if (buffer == nullptr)
		return;
	wl_surface_attach(p->surface, buffer, 0, 0);
	wl_surface_damage_buffer(p->surface, 0, 0, p->width, p->height);
	wl_surface_commit(p->surface);
}

void CloseFrom(size_t index);

const xdg_surface_listener kXdgSurfaceListener = {
	[](void* data, xdg_surface* xs, uint32_t serial) {
		auto* p = static_cast<Popup*>(data);
		xdg_surface_ack_configure(xs, serial);
		p->configured = true;
		Paint(p);
	},
};

const xdg_popup_listener kPopupListener = {
	[](void*, xdg_popup*, int32_t, int32_t, int32_t, int32_t) {},
	[](void*, xdg_popup*) { RunLater([]() { CloseMenus(); }); },		// the compositor took it away: a press outside
	[](void*, xdg_popup*, uint32_t) {},
};

// Creates the popup for p: attached to the bar (parent null) or to another menu, at the rectangle (x, y, w, h) of the parent.
void Create(Popup* p, Popup* parent, int x, int y, int w, int h, uint32_t anchor, uint32_t gravity, int offsetX, int offsetY,
	uint32_t constraints)
{
	p->surface = wl_compositor_create_surface(gWl.compositor);
	p->xdgSurface = xdg_wm_base_get_xdg_surface(gWl.wmBase, p->surface);
	xdg_surface_add_listener(p->xdgSurface, &kXdgSurfaceListener, p);
	xdg_positioner* positioner = xdg_wm_base_create_positioner(gWl.wmBase);
	xdg_positioner_set_size(positioner, p->width, p->height);
	xdg_positioner_set_anchor_rect(positioner, x, y, std::max(1, w), std::max(1, h));
	xdg_positioner_set_anchor(positioner, anchor);
	xdg_positioner_set_gravity(positioner, gravity);
	xdg_positioner_set_constraint_adjustment(positioner, constraints);
	if (offsetX || offsetY)
		xdg_positioner_set_offset(positioner, offsetX, offsetY);
	p->popup = xdg_surface_get_popup(p->xdgSurface, parent != nullptr ? parent->xdgSurface : nullptr, positioner);
	xdg_positioner_destroy(positioner);
	xdg_popup_add_listener(p->popup, &kPopupListener, p);
	if (parent == nullptr)
		zwlr_layer_surface_v1_get_popup(gWl.layerSurface, p->popup);
	if (gWl.serial != 0)
		xdg_popup_grab(p->popup, gWl.seat, gWl.serial);	// (without a press to name, as in a test, it is left ungrabbed)
	wl_surface_commit(p->surface);
}

void Destroy(Popup* p)
{
	if (p->openTimer != 0)
		g_source_remove(p->openTimer);
	if (p->popup != nullptr)
		xdg_popup_destroy(p->popup);
	if (p->xdgSurface != nullptr)
		xdg_surface_destroy(p->xdgSurface);
	if (p->surface != nullptr)
		wl_surface_destroy(p->surface);
	if (gOver == p)
		gOver = nullptr;
}

// Closes the menu at index and the ones opened from it (the newest first: a popup goes before its parent).
void CloseFrom(size_t index)
{
	while (gStack.size() > index) {
		Destroy(gStack.back().get());
		gStack.pop_back();
	}
	if (!gStack.empty() && index > 0)
		gStack.back()->hover = gStack.back()->hover;
}

void OpenSubmenu(Popup* parent, int row)
{
	// (it may be open already)
	size_t at = 0;
	while (at < gStack.size() && gStack[at].get() != parent)
		at++;
	if (at == gStack.size())
		return;
	if (at + 1 < gStack.size() && gStack[at + 1]->parentRow == row)
		return;
	CloseFrom(at + 1);
	const MenuItem& item = parent->items[row];
	if (!item.submenu)
		return;
	auto child = std::make_unique<Popup>();
	child->items = item.submenu();
	if (child->items.empty())
		return;
	child->parent = parent;
	child->parentRow = row;
	LayOut(child.get());
	Popup* raw = child.get();
	gStack.push_back(std::move(child));
	const int y = kPad + parent->tops[row] - parent->scroll;
	Create(raw, parent, 0, y, parent->width, kRow, XDG_POSITIONER_ANCHOR_TOP_RIGHT, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT, -3, -kPad,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
			| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
}

}	// namespace

void OpenMenu(std::vector<MenuItem> items, const MenuAnchor& anchor)
{
	CloseMenus();
	if (items.empty() || gWl.wmBase == nullptr || gWl.layerSurface == nullptr)
		return;
	auto root = std::make_unique<Popup>();
	root->items = std::move(items);
	LayOut(root.get());
	Popup* raw = root.get();
	gStack.push_back(std::move(root));
	Create(raw, nullptr, anchor.x, anchor.y, anchor.w, anchor.h,
		anchor.above ? XDG_POSITIONER_ANCHOR_TOP_LEFT : XDG_POSITIONER_ANCHOR_BOTTOM_LEFT,
		anchor.above ? XDG_POSITIONER_GRAVITY_TOP_RIGHT : XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT, 0, 0,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
			| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
}

void CloseMenus()
{
	CloseFrom(0);
}

bool MenusOpen()
{
	return !gStack.empty();
}

bool MenuOwnsSurface(wl_surface* surface)
{
	for (const auto& p : gStack) {
		if (p->surface == surface)
			return true;
	}
	return false;
}

void MenuPointerEnter(wl_surface* surface, double x, double y)
{
	gOver = nullptr;
	for (const auto& p : gStack) {
		if (p->surface == surface)
			gOver = p.get();
	}
	MenuPointerMotion(x, y);
}

void MenuPointerLeave()
{
	if (gOver != nullptr && gOver->hover >= 0) {
		gOver->hover = -1;
		Paint(gOver);
	}
	gOver = nullptr;
}

void MenuPointerMotion(double x, double y)
{
	gPointerX = x;
	gPointerY = y;
	Popup* p = gOver;
	if (p == nullptr)
		return;
	const int row = ItemAt(p, y);
	if (row == p->hover)
		return;
	p->hover = row;
	Paint(p);
	// a row with a submenu opens it after a moment (so a pointer on its way across does not open every one); any other row
	// closes the menus that were opened from this one
	if (p->openTimer != 0) {
		g_source_remove(p->openTimer);
		p->openTimer = 0;
	}
	size_t at = 0;
	while (at < gStack.size() && gStack[at].get() != p)
		at++;
	if (row >= 0 && p->items[row].submenu && p->items[row].enabled) {
		struct Ctx { Popup* p; int row; };
		p->openTimer = g_timeout_add_full(G_PRIORITY_DEFAULT, 140, [](gpointer d) -> gboolean {
			auto* ctx = static_cast<Ctx*>(d);
			ctx->p->openTimer = 0;
			for (const auto& q : gStack) {
				if (q.get() == ctx->p) {
					OpenSubmenu(ctx->p, ctx->row);
					break;
				}
			}
			return G_SOURCE_REMOVE;
		}, new Ctx{ p, row }, [](gpointer d) { delete static_cast<Ctx*>(d); });
	} else if (at + 1 < gStack.size())
		CloseFrom(at + 1);
}

void MenuPointerButton(uint32_t, uint32_t state)
{
	Popup* p = gOver;
	if (p == nullptr || state != WL_POINTER_BUTTON_STATE_RELEASED)
		return;
	const int row = ItemAt(p, gPointerY);
	if (row < 0 || !p->items[row].enabled || p->items[row].separator)
		return;
	if (p->items[row].submenu) {
		OpenSubmenu(p, row);
		return;
	}
	std::function<void()> action = p->items[row].action;
	// (the menus go first: the action may open a window, or open another menu)
	RunLater([action]() {
		CloseMenus();
		if (action)
			action();
	});
}

void MenuPointerAxis(double value)
{
	Popup* p = gOver;
	if (p == nullptr)
		return;
	const int room = p->content + 2 * kPad - p->height;
	if (room <= 0)
		return;
	p->scroll = std::clamp(p->scroll + static_cast<int>(value * 2), 0, room);
	Paint(p);
}

// For testing: opens the submenu of a row of the menu at level (0 is the root), and lights the row.
void MenuDebugOpen(int level, int row)
{
	if (level < 0 || level >= static_cast<int>(gStack.size()))
		return;
	Popup* p = gStack[level].get();
	if (row < 0 || row >= static_cast<int>(p->items.size()))
		return;
	p->hover = row;
	Paint(p);
	OpenSubmenu(p, row);
}
