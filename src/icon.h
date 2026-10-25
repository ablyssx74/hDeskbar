/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */
#ifndef HDESKBAR_ICON_H
#define HDESKBAR_ICON_H

#include <cairo/cairo.h>

#include "icons.h"

// The SnakeTracker icon (the server's), drawn size x size with its top left at x, y.
void DrawSnakeTrackerIcon(cairo_t* cr, double x, double y, int size);

// A soft shadow of the same icon, offset by dx, dy (a few copies of its shape in black, faint, spread a little).
void DrawSnakeTrackerShadow(cairo_t* cr, double x, double y, int size, double dx, double dy, double alpha);

// The same snake as a picture for a menu item (size logical pixels square).
IconRef AboutMenuIcon(int size);

#endif
