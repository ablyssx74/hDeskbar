/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "icon.h"

#include <librsvg/rsvg.h>

#include <cmath>

#include "about-icon-data.h"
#include "icon-data.h"

namespace {

// One picture, drawn at the pixels it needs (a screen with a scale has more of them) and kept for next time.
struct Cached {
	cairo_surface_t* surface = nullptr;
	int pixels = 0;
};

cairo_surface_t* Render(Cached* cache, const unsigned char* data, size_t length, int pixels)
{
	if (cache->surface != nullptr && cache->pixels == pixels)
		return cache->surface;
	if (cache->surface != nullptr)
		cairo_surface_destroy(cache->surface);
	cache->surface = nullptr;
	cache->pixels = pixels;

	GError* error = nullptr;
	RsvgHandle* handle = rsvg_handle_new_from_data(data, length, &error);
	if (handle == nullptr) {
		if (error != nullptr)
			g_error_free(error);
		return nullptr;
	}
	cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
	cairo_t* cr = cairo_create(surface);
	RsvgRectangle viewport = { 0, 0, static_cast<double>(pixels), static_cast<double>(pixels) };
	rsvg_handle_render_document(handle, cr, &viewport, nullptr);
	cairo_destroy(cr);
	g_object_unref(handle);
	cache->surface = surface;
	return surface;
}

void Draw(Cached* cache, const unsigned char* data, size_t length, cairo_t* cr, double x, double y, int size)
{
	cairo_matrix_t matrix;
	cairo_get_matrix(cr, &matrix);
	const double scale = matrix.xx > 0 ? matrix.xx : 1;
	const int pixels = static_cast<int>(std::lround(size * scale));
	cairo_surface_t* icon = Render(cache, data, length, pixels);
	if (icon == nullptr)
		return;
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, size / static_cast<double>(pixels), size / static_cast<double>(pixels));
	cairo_set_source_surface(cr, icon, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);
}

Cached gBar, gAbout;

}	// namespace

void DrawSnakeTrackerIcon(cairo_t* cr, double x, double y, int size)
{
	Draw(&gBar, kSnakeTrackerSvg, sizeof(kSnakeTrackerSvg), cr, x, y, size);
}

void DrawAboutIcon(cairo_t* cr, double x, double y, int size)
{
	Draw(&gAbout, kAboutSvg, sizeof(kAboutSvg), cr, x, y, size);
}
