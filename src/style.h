/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The colors of the SnakeTracker look, and the shapes the bar and its menus are drawn with.
 */
#ifndef HDESKBAR_STYLE_H
#define HDESKBAR_STYLE_H

#include <cairo/cairo.h>

#include <cmath>

struct Color {
	double r, g, b;
};

constexpr Color kPanel = {200 / 255.0, 203 / 255.0, 235 / 255.0};		// the lavender panel
constexpr Color kOutline = {20 / 255.0, 22 / 255.0, 30 / 255.0};		// the line round the bar
constexpr Color kAccent = {70 / 255.0, 110 / 255.0, 200 / 255.0};		// the SnakeTracker color

inline Color Mix(const Color& a, const Color& b, double t)
{
	return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

// A rectangle with a radius for each of its corners (0 for a square one).
inline void RoundedRectPath(cairo_t* cr, double x, double y, double w, double h, double tl, double tr, double br, double bl)
{
	cairo_new_sub_path(cr);
	cairo_move_to(cr, x + tl, y);
	cairo_line_to(cr, x + w - tr, y);
	if (tr > 0)
		cairo_arc(cr, x + w - tr, y + tr, tr, -M_PI / 2, 0);
	cairo_line_to(cr, x + w, y + h - br);
	if (br > 0)
		cairo_arc(cr, x + w - br, y + h - br, br, 0, M_PI / 2);
	cairo_line_to(cr, x + bl, y + h);
	if (bl > 0)
		cairo_arc(cr, x + bl, y + h - bl, bl, M_PI / 2, M_PI);
	cairo_line_to(cr, x, y + tl);
	if (tl > 0)
		cairo_arc(cr, x + tl, y + tl, tl, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

#endif
