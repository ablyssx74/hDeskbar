/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "tooltip.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "loop.h"
#include "shm.h"
#include "style.h"
#include "wl.h"

namespace {

struct Tip {
	std::string text;
	int width = 0, height = 0;
	bool configured = false;
	wl_surface* surface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_popup* popup = nullptr;
};

std::unique_ptr<Tip> gTip;

PangoLayout* MakeLayout(cairo_t* cr, const std::string& text, PangoFontDescription** font)
{
	PangoLayout* layout = pango_cairo_create_layout(cr);
	*font = pango_font_description_from_string("Sans Bold 9");
	pango_layout_set_font_description(layout, *font);
	pango_layout_set_text(layout, text.c_str(), -1);
	return layout;
}

void Paint(Tip* tip)
{
	if (!tip->configured)
		return;
	wl_buffer* buffer = NewShmBuffer(tip->width, tip->height, [tip](cairo_t* cr) {
		const double w = tip->width, h = tip->height;
		RoundedRectPath(cr, 0, 0, w, h, h / 2, h / 2, h / 2, h / 2);
		cairo_set_source_rgba(cr, 15 / 255.0, 15 / 255.0, 15 / 255.0, 0.88);
		cairo_fill(cr);
		PangoFontDescription* font = nullptr;
		PangoLayout* layout = MakeLayout(cr, tip->text, &font);
		int textWidth = 0, textHeight = 0;
		pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_move_to(cr, (w - textWidth) / 2.0, (h - textHeight) / 2.0);
		pango_cairo_show_layout(cr, layout);
		pango_font_description_free(font);
		g_object_unref(layout);
	});
	if (buffer == nullptr)
		return;
	wl_surface_attach(tip->surface, buffer, 0, 0);
	wl_surface_damage_buffer(tip->surface, 0, 0, tip->width, tip->height);
	wl_surface_commit(tip->surface);
}

const xdg_surface_listener kXdgSurfaceListener = {
	[](void*, xdg_surface* xs, uint32_t serial) {
		xdg_surface_ack_configure(xs, serial);
		if (gTip) {
			gTip->configured = true;
			Paint(gTip.get());
		}
	},
};

const xdg_popup_listener kPopupListener = {
	[](void*, xdg_popup*, int32_t, int32_t, int32_t, int32_t) {},
	[](void*, xdg_popup*) { RunLater([]() { HideTooltip(); }); },
	[](void*, xdg_popup*, uint32_t) {},
};

}	// namespace

void ShowTooltip(const std::string& text, int x, int y, int w, int h, bool above)
{
	HideTooltip();
	if (gWl.wmBase == nullptr || gWl.layerSurface == nullptr || text.empty())
		return;
	gTip = std::make_unique<Tip>();
	Tip* tip = gTip.get();
	tip->text = text;
	{
		static cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
		static cairo_t* cr = cairo_create(scratch);
		PangoFontDescription* font = nullptr;
		PangoLayout* layout = MakeLayout(cr, text, &font);
		int textWidth = 0, textHeight = 0;
		pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
		pango_font_description_free(font);
		g_object_unref(layout);
		tip->height = textHeight + 10;
		tip->width = textWidth + tip->height;		// a capsule: room for the round ends
	}
	tip->surface = wl_compositor_create_surface(gWl.compositor);
	// no input: the pointer is over the bar, and stays that way
	wl_region* none = wl_compositor_create_region(gWl.compositor);
	wl_surface_set_input_region(tip->surface, none);
	wl_region_destroy(none);
	tip->xdgSurface = xdg_wm_base_get_xdg_surface(gWl.wmBase, tip->surface);
	xdg_surface_add_listener(tip->xdgSurface, &kXdgSurfaceListener, tip);
	xdg_positioner* positioner = xdg_wm_base_create_positioner(gWl.wmBase);
	xdg_positioner_set_size(positioner, tip->width, tip->height);
	xdg_positioner_set_anchor_rect(positioner, x, y, std::max(1, w), std::max(1, h));
	xdg_positioner_set_anchor(positioner, above ? XDG_POSITIONER_ANCHOR_TOP : XDG_POSITIONER_ANCHOR_BOTTOM);
	xdg_positioner_set_gravity(positioner, above ? XDG_POSITIONER_GRAVITY_TOP : XDG_POSITIONER_GRAVITY_BOTTOM);
	xdg_positioner_set_offset(positioner, 0, above ? -4 : 4);
	xdg_positioner_set_constraint_adjustment(positioner, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
		| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
	tip->popup = xdg_surface_get_popup(tip->xdgSurface, nullptr, positioner);
	xdg_positioner_destroy(positioner);
	xdg_popup_add_listener(tip->popup, &kPopupListener, tip);
	zwlr_layer_surface_v1_get_popup(gWl.layerSurface, tip->popup);
	wl_surface_commit(tip->surface);
}

void HideTooltip()
{
	if (!gTip)
		return;
	if (gTip->popup != nullptr)
		xdg_popup_destroy(gTip->popup);
	if (gTip->xdgSurface != nullptr)
		xdg_surface_destroy(gTip->xdgSurface);
	if (gTip->surface != nullptr)
		wl_surface_destroy(gTip->surface);
	gTip.reset();
}

bool TooltipShown()
{
	return static_cast<bool>(gTip);
}
