/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */
#ifndef HDESKBAR_ICONS_H
#define HDESKBAR_ICONS_H

#include <cairo/cairo.h>

#include <memory>
#include <string>

using IconRef = std::shared_ptr<cairo_surface_t>;

IconRef MakeIconRef(cairo_surface_t* surface);

// An icon of the user's icon theme (the theme's Inherits chain, then hicolor and the pixmaps), px pixels square; name may be
// a path. extraDir is searched first (a StatusNotifierItem's IconThemePath). Pictures are kept for next time.
IconRef CachedIcon(const std::string& name, int px, const std::string& extraDir = std::string());

#endif
