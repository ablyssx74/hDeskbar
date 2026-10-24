/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "icon.h"

#include <librsvg/rsvg.h>

#include "icon-data.h"

namespace {

cairo_surface_t* gCache = nullptr;
int gCacheSize = 0;

cairo_surface_t* Render(int size)
{
	if (gCache != nullptr && gCacheSize == size)
		return gCache;
	if (gCache != nullptr)
		cairo_surface_destroy(gCache);
	gCache = nullptr;
	gCacheSize = size;

	GError* error = nullptr;
	RsvgHandle* handle = rsvg_handle_new_from_data(kSnakeTrackerSvg, sizeof(kSnakeTrackerSvg), &error);
	if (handle == nullptr) {
		if (error != nullptr)
			g_error_free(error);
		return nullptr;
	}
	cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t* cr = cairo_create(surface);
	RsvgRectangle viewport = { 0, 0, static_cast<double>(size), static_cast<double>(size) };
	rsvg_handle_render_document(handle, cr, &viewport, nullptr);
	cairo_destroy(cr);
	g_object_unref(handle);
	gCache = surface;
	return gCache;
}

}	// namespace

void DrawSnakeTrackerIcon(cairo_t* cr, double x, double y, int size)
{
	cairo_surface_t* icon = Render(size);
	if (icon == nullptr)
		return;
	cairo_set_source_surface(cr, icon, x, y);
	cairo_paint(cr);
}
