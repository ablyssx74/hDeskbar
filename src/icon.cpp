/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "icon.h"

#include <librsvg/rsvg.h>

#include <cmath>

#include "about-icon-data.h"
#include "icons.h"
#include "scale.h"
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

Cached gBar;

}	// namespace

void DrawSnakeTrackerIcon(cairo_t* cr, double x, double y, int size)
{
	Draw(&gBar, kSnakeTrackerSvg, sizeof(kSnakeTrackerSvg), cr, x, y, size);
}


IconRef AboutMenuIcon(int size)
{
	const int pixels = static_cast<int>(std::lround(size * RenderScale()));
	static Cached cache;
	cairo_surface_t* surface = Render(&cache, kAboutSvg, sizeof(kAboutSvg), pixels);
	if (surface == nullptr)
		return nullptr;
	// (the cache keeps one; the menu gets its own reference to the picture)
	cairo_surface_reference(surface);
	return MakeIconRef(surface);
}

void DrawSnakeTrackerShadow(cairo_t* cr, double x, double y, int size, double dx, double dy, double alpha)
{
	cairo_matrix_t matrix;
	cairo_get_matrix(cr, &matrix);
	const double scale = matrix.xx > 0 ? matrix.xx : 1;
	const int pixels = static_cast<int>(std::lround(size * scale));
	cairo_surface_t* icon = Render(&gBar, kSnakeTrackerSvg, sizeof(kSnakeTrackerSvg), pixels);
	if (icon == nullptr)
		return;
	// the shape is used as a mask a few times, a little apart, so the edge is soft
	static const double spread[][2] = { {0, 0}, {-1.5, 0}, {1.5, 0}, {0, -1.5}, {0, 1.5}, {-1, -1}, {1, 1}, {-1, 1}, {1, -1} };
	const double each = alpha / 4.0;
	for (const auto& s : spread) {
		cairo_save(cr);
		cairo_translate(cr, x + dx + s[0], y + dy + s[1]);
		cairo_scale(cr, size / static_cast<double>(pixels), size / static_cast<double>(pixels));
		cairo_set_source_rgba(cr, 0, 0, 0, each);
		cairo_mask_surface(cr, icon, 0, 0);
		cairo_restore(cr);
	}
}
